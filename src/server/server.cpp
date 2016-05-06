/*
    Copyright (C) 2022, BogDan Vatra <bogdan@kde.org>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <stdexcept>

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <grp.h>
#include <malloc.h>
#include <pwd.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <openssl/err.h>
#include <sys/epoll.h>
#include <sys/socket.h>


#include <boost/algorithm/string.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/core.hpp>
#include <boost/log/common.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/filter_parser.hpp>
#include <boost/log/utility/setup/formatter_parser.hpp>
#include <boost/log/utility/setup/from_settings.hpp>
#include <boost/log/utility/setup/settings.hpp>
#include <boost/log/sources/severity_feature.hpp>
#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <dracon/exceptions.h>
#include <dracon/logging.h>

#include "server.h"
#include "server_logger.h"
#include "server_session.h"
#include "server_service_sessions.h"
#include "sessions_event_loop.h"
#include "streams.h"

#if OPENSSL_VERSION_NUMBER < 0x3000000fL
#error "Only SSL 3.0+ is supported"
#endif

template<typename T>
using deleted_unique_ptr = std::unique_ptr<T, std::function<void(T *)>>;

namespace dracon::internal {

using namespace std::string_view_literals;

tagged_logger<> server_logger{"Server"};

namespace {
    uint32_t queued_connections = 20000;
    uint32_t event_loops_size = std::max(uint32_t(2), std::thread::hardware_concurrency());

    std::vector<std::pair<std::string, std::string>> merged_properties(const boost::property_tree::ptree &node, const std::string &prev_nodes = {})
    {
        std::vector<std::pair<std::string, std::string>> res;
        if (node.empty()) {
            res.emplace_back(std::pair<std::string, std::string>{prev_nodes, node.data()});
            return res;
        }
        for (const auto &n : node) {
            auto pn = prev_nodes;
            if (!pn.empty())
                pn += ".";
            pn += n.first;
            auto props = merged_properties(n.second, pn);
            res.insert(res.end(), props.begin(), props.end());
        }
        return res;
    }

    static void unblock_signal(int signum)
    {
        sigset_t sigs;
        sigemptyset(&sigs);
        sigaddset(&sigs, signum);
        sigprocmask(SIG_UNBLOCK, &sigs, nullptr);
    }

    std::string stack_trace(int discard = 0)
    {
        void *addresses[100];
        auto count = backtrace(addresses, 100);
        char **symbols = backtrace_symbols(addresses, count);
        std::ostringstream stack_ttrace;
        for (int i = discard; i < count; ++i) {
            Dl_info dlinfo;
            if(!dladdr(addresses[i], &dlinfo))
                break;
            const char * symbol = dlinfo.dli_sname;
            int status;
            auto demangled = abi::__cxa_demangle(symbol, nullptr, nullptr, &status);
            if (demangled && !status)
                symbol = demangled;
            if (symbol)
                stack_ttrace <<  dlinfo.dli_fname << " (" << symbol << ")" << " [" << addresses[i] << "]" << std::endl;
            else
                stack_ttrace << symbols[i] << std::endl;

            if (demangled)
                free(demangled);
        }
        free(symbols);
        return stack_ttrace.str();
    }

    static void signal_handler(int sig, siginfo_t *info, void *)
    {
        /// Transform segmentation violations signals into exceptions
        if (sig == SIGSEGV && info->si_addr == nullptr) {
            unblock_signal(SIGSEGV);
            ERROR(server_logger) << "Segmentation fault\n" << stack_trace(3);
            throw dracon::segmentation_fault_error("Segmentation fault");
        }

        /// Transform floation-point errors signals into exceptions
        if (sig == SIGFPE && (info->si_code == FPE_INTDIV || info->si_code == FPE_FLTDIV)) {
            unblock_signal(SIGFPE);
            ERROR(server_logger) << "Floating point error\n" << stack_trace(3);
            throw dracon::floating_point_error("Floating point error");
        }
        if (sig == SIGTERM || sig == SIGINT) {
            server::exit_signal_handler();
            return;
        }
        ERROR(server_logger) << "Unexpected signal " << sig << "\n" << stack_trace(3);
        throw std::runtime_error("Unexpected signal " + std::to_string(sig));
    }
}

std::chrono::seconds server::s_headers_timeout{5s};
std::chrono::seconds server::s_ssl_accept_timeout{5s};
std::chrono::seconds server::s_ssl_shutdown_timeout{2s};
std::chrono::seconds server::s_keep_alive_timeout{10s};

/*!
 * \brief server::exit_signal_handler
 *
 * Exit the server
 */
void server::exit_signal_handler()
{
    // Quit server loop
    INFO(server_logger) << "shutting down the server";
    instance().m_shutdown.store(true);
}

std::chrono::seconds server::keep_alive_timeout()
{
    return s_keep_alive_timeout;
}

std::chrono::seconds server::headers_timeout()
{
    return s_headers_timeout;
}

std::chrono::seconds server::ssl_accept_timeout()
{
    return s_ssl_accept_timeout;
}

std::chrono::seconds server::ssl_shutdown_timeout()
{
    return s_ssl_shutdown_timeout;
}

/// Makes socket nonblocking
/*!
 * \brief server::bind
 *
 * \param type the socket type
 * \param port the port that on which will be bound
 *
 * \return the bound socket
 */
int server::bind(socket_type type, int port)
{
    int sock = -1;
    if ((sock = ::socket(type == IPV4 ? AF_INET : AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0)
        throw std::runtime_error{"Can't create the socket"};

    int opt = 1;
    if (::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        throw std::runtime_error{"Can't set the socket SO_REUSEADDR option"};

    if (type == IPV4) {
        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family      = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_ANY);
        saddr.sin_port        = htons(port);
        if (::bind(sock,(struct sockaddr *) &saddr, sizeof(saddr)) < 0)
            throw std::runtime_error{"Can't bind the socket"};
    } else {
        opt = 1;
        if (::setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) < 0)
            throw std::runtime_error{"Can't set the socket IPV6_V6ONLY option"};

        struct sockaddr_in6 saddr6;
        memset(&saddr6, 0, sizeof(saddr6));
        saddr6.sin6_addr = in6addr_any;
        saddr6.sin6_port = htons(port);
        saddr6.sin6_family = AF_INET6;
        if (::bind(sock,(struct sockaddr *) &saddr6, sizeof(saddr6)) < 0)
            throw std::runtime_error{"Can't bind the socket"};
    }

    if (::listen(sock, queued_connections) == -1)
        throw std::runtime_error{"Can't listen on the socket"};

    struct epoll_event event;
    event.data.ptr = nullptr;
    event.data.fd = sock;
    event.events = EPOLLIN | EPOLLPRI | EPOLLRDHUP | EPOLLET;

    if (epoll_ctl(m_epoll_handler, EPOLL_CTL_ADD, sock, &event))
        throw std::runtime_error{"Can't  epoll_ctl"};

    ++m_events_size;
    return sock;
}

/*!
 * \brief server::instance
 *
 * \return the server instance
 */
server &server::instance()
{
    static server server;
    return server;
}

/*!
 * \brief server::exec
 *
 * Executes the server loop.
 * The loop is also used to accept incoming connections
 *
 * \param argc main function argc
 * \param argv main function argc
 *
 * \return the status
 */
int server::exec(int argc, char *argv[])
{
    static bool running = false;
    if (running)
        throw std::runtime_error{"Already running"};

    boost::log::add_common_attributes();
    boost::log::register_simple_filter_factory<boost::log::trivial::severity_level, char>("Severity");
    boost::log::register_simple_formatter_factory<boost::log::trivial::severity_level, char>("Severity");

    // server start time, will be used by server sessions
    m_start_time = std::chrono::system_clock::now();

    namespace po = boost::program_options;
    namespace fs = std::filesystem;
    int http_port = 8080; // Default HTTP port
    int https_port = 8443; // Default HTTPS port
    uint32_t max_connections_per_ip = 500;
    bool workload_balancing = true;

    // Default plugins path
    std::string plugins_path = fs::canonical(fs::path(argv[0])).parent_path().parent_path().append("lib/dracon/plugins").string();
    std::string conf_dir = fs::canonical(fs::path(argv[0])).parent_path().parent_path().append("etc/dracon").string();
    std::string drop_user;
    std::string drop_group;
    bool print_pid = false;
    // server arguments
    po::options_description desc{"Dracon options"};
    desc.add_options()
            ("conf,c", po::value<std::string>(&conf_dir)->implicit_value(conf_dir), "configurations path")
            ("plugins-dir,d", po::value<std::string>(&plugins_path)->implicit_value(plugins_path), "plugins dir")
            ("workers,w", po::value<uint32_t>(&event_loops_size)->implicit_value(event_loops_size), "workers")
            ("user,u", po::value<std::string>(&drop_user), "username to drop privileges to")
            ("group,g", po::value<std::string>(&drop_group), "optional group to drop privileges to, if missing the main user group will be used")
            ("pid", po::bool_switch(&print_pid), "print Dracon pid")
            ("help,h", "print this help")
            ;

    try {
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
        if (vm.count("help")) {
            std::cout << desc << std::endl;
            exit_signal_handler();
            return 0;
        }
    } catch (po::error& e) {
        std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
        std::cerr << desc << std::endl;
        throw;
    }

    if (event_loops_size < 1)
        throw std::runtime_error("Invalid workers count");

    bool enable_server_status = false;
    gid_t gid = gid_t(-1);
    uid_t uid = uid_t(-1);
    boost::log::settings logging_settings;
    if (!conf_dir.empty()) {
        auto cur_path = std::filesystem::current_path();
        std::filesystem::current_path(conf_dir);
        namespace pt = boost::property_tree;
        pt::ptree properties;
        pt::read_info("server.conf", properties);
        auto logging_properties = merged_properties(properties.get_child("logging"));
        for (const auto &kv : logging_properties)
            logging_settings[kv.first] = kv.second;

        s_keep_alive_timeout = std::chrono::seconds{properties.get("keepalive_timeout", s_keep_alive_timeout.count())};
        s_headers_timeout = std::chrono::seconds{properties.get("headers_timeout", s_headers_timeout.count())};
        enable_server_status = properties.get("server_status", false);
        http_port = properties.get("http_port", -1);
        queued_connections = properties.get("queued_connections", queued_connections);
        max_connections_per_ip = properties.get("max_connections_per_ip", max_connections_per_ip);
        workload_balancing = properties.get("workload_balancing", workload_balancing);
        TRACE(server_logger) << "http port:" << http_port;
        if (properties.find("https") != properties.not_found()) {
            TRACE(server_logger) << "https section found in config";
            if (properties.get("https.enabled", false)) {
                s_ssl_accept_timeout = std::chrono::seconds{properties.get("https.accept_timeout", s_ssl_accept_timeout.count())};
                s_ssl_shutdown_timeout = std::chrono::seconds{properties.get("https.shutdown_timeout", s_ssl_shutdown_timeout.count())};

                https_port = properties.get("https.port", https_port);
                TRACE(server_logger) << "https enabled in configm port=" << https_port;

                std::string ctx_method = boost::algorithm::to_lower_copy(properties.get<std::string>("https.ssl.ctx_method"));
                DEBUG(server_logger) << "SSL_CTX_new(" << ctx_method << ")";
                if (!(m_ssl_context = SSL_CTX_new(ctx_method == "dtls"sv ? DTLS_server_method() : TLS_server_method())))
                    throw std::runtime_error("Can't create SSL Context");

                // load SSL CTX configuration
                auto ctx_conf = deleted_unique_ptr<SSL_CONF_CTX>(SSL_CONF_CTX_new(), [](SSL_CONF_CTX *ptr){SSL_CONF_CTX_free(ptr);});
                if (!ctx_conf)
                    throw std::runtime_error(ERR_error_string(ERR_get_error(), nullptr));
                SSL_CONF_CTX_set_ssl_ctx(ctx_conf.get(), m_ssl_context);
                SSL_CONF_CTX_set_flags(ctx_conf.get(), SSL_CONF_FLAG_FILE | SSL_CONF_FLAG_SERVER | SSL_CONF_FLAG_CERTIFICATE | SSL_CONF_FLAG_REQUIRE_PRIVATE | SSL_CONF_FLAG_SHOW_ERRORS);

                auto cxt_settings = merged_properties(properties.get_child("https.ssl.cxt_settings"));
                for (const auto &kv : cxt_settings) {
                    DEBUG(server_logger) << "SSL_CONF_cmd(" << kv.first << ", " << kv.second << ")";
                    if (SSL_CONF_cmd(ctx_conf.get(), kv.first.c_str(), kv.second.empty() ? nullptr : kv.second.c_str()) < 1)
                        throw std::runtime_error{ERR_error_string(ERR_get_error(), nullptr)};
                }

                if (SSL_CONF_CTX_finish(ctx_conf.get()) != 1 || SSL_CTX_check_private_key(m_ssl_context) != 1)
                    throw std::runtime_error(ERR_error_string(ERR_get_error(), nullptr));

                SSL_CTX_set_read_ahead(m_ssl_context, 1);
                SSL_CTX_set_mode(m_ssl_context, SSL_MODE_RELEASE_BUFFERS | SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
            } else {
                https_port = -1;
            }
        }

        if (!getuid() && (!drop_user.empty() ||
                (properties.find("privileges") != properties.not_found() && properties.get("privileges.drop", false)))) {
            auto usr = drop_user.empty() ? properties.get<std::string>("privileges.user") : drop_user;
            auto user = getpwnam(usr.c_str());
            if (!user)
                throw std::runtime_error("Can't find user \"" + usr + "\"");
            uid = user->pw_uid;
            gid = user->pw_gid;
            auto grp = drop_group.empty() ? properties.get<std::string>("privileges.group", std::string{}) : drop_group;
            if (!grp.empty()) {
                auto group =getgrnam(grp.c_str());
                if (!group)
                    throw std::runtime_error("Can't find group \"" + grp + "\"");
                gid = group->gr_gid;
            }
        }
        std::filesystem::current_path(cur_path);
    }

    if (http_port < 0 && https_port < 0)
        throw std::runtime_error{"No HTTP nor HTTPS ports specified"};

    // load plugins
    if (fs::is_directory(plugins_path)) {
        fs::directory_iterator end_iter;
        for (fs::directory_iterator dir_itr{plugins_path}; dir_itr != end_iter; ++dir_itr) {
            try {
                if (fs::is_regular_file(dir_itr->status()))
                    m_plugins.emplace_back(dir_itr->path().string(), conf_dir);
            } catch (const std::exception &e) {
                ERROR(server_logger) << e.what();
            }
        }
    }

    // at the end add the server sessions
    if (enable_server_status)
        m_plugins.emplace_back(&server_sessions::create_session, UINT32_MAX / 2);
    std::sort(m_plugins.begin(), m_plugins.end(), [](const server_plugin &a, const server_plugin &b){return a.order() < b.order();});

    // accept thread must have insane priority to be able to accept connections
    // as fast as possible
    sched_param sch;
    sch.sched_priority = sched_get_priority_max(SCHED_RR);
    pthread_setschedparam(pthread_self(), SCHED_RR, &sch);

    // Bind IPv4 & IPv6 http ports
    if (http_port > 0) {
        bind(IPV4, http_port);
        bind(IPV6, http_port);
        INFO(server_logger) << "listen on :"<< http_port << " port";
    }

    if (https_port > 0) {
        // Bind IPv4 & IPv6 https ports
        m_https4_sock = bind(IPV4, https_port);
        m_https6_sock = bind(IPV6, https_port);
        INFO(server_logger) << "listen on :"<< https_port << " port";
    }

    if (!getuid() && gid != gid_t(-1) && uid != uid_t(-1)) {
        if (setgroups(0, nullptr) || setgid(gid) || setuid(uid))
             throw std::runtime_error("Can't drop privileges");
        if (uid && (!getuid() || !geteuid() || setuid(0) != -1))
             throw std::runtime_error("Dropping privileges didn't stick");
        INFO(server_logger) << "Droping privileges";
    }

    boost::log::init_from_settings(logging_settings);
    INFO(server_logger) << "Logging setup succeeded";

    auto event_loops = std::make_unique<sessions_event_loop[]>(event_loops_size);
    for (uint32_t i = 0; i < event_loops_size; ++i)
        event_loops[i].set_workload_balancing(workload_balancing);

    INFO(server_logger) << "using " << event_loops_size << " worker threads";

    INFO(server_logger) << "using " << queued_connections << " queued connections";

    // allocate epoll list
    const auto epoll_list = std::make_unique<epoll_event[]>(m_events_size);

    if (print_pid)
        std::cout << "pid:" << getpid() << std::endl << std::flush;

    // Wait for incoming connections
    bool trimmed = false;
    while (!m_shutdown) {
        int triggered_events = epoll_wait(m_epoll_handler, epoll_list.get(), m_events_size, 1000);
        if (triggered_events > 0) {
            trimmed = false;
        } else if (!trimmed && active_sessions() <= 1) { // No more pending sessions?
            malloc_trim(0); // release the memory to OS
            trimmed = true;
        }

        for (int i = 0; i < triggered_events; ++i)
        {
            auto events = epoll_list[i].events;
            if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
                throw std::runtime_error{"listen socket error"};

            if (events & (EPOLLIN | EPOLLPRI)) {
                // It's time to accept all connections
                struct sockaddr_storage in_addr;
                while (!m_shutdown) {
                    int fd = epoll_list[i].data.fd;
                    bool ssl = fd == m_https4_sock || fd == m_https6_sock;
                    socklen_t in_len = sizeof(struct sockaddr_storage);
                    int sock = ::accept4(fd, (struct sockaddr *)&in_addr, &in_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (-1 == sock)
                        break;

                    uint32_t order;
                    auto addr = dracon::address_text(in_addr);
                    {
                        std::unique_lock<std::mutex> lock{m_connections_per_ip_mutex};
                        if (m_connections_per_ip[addr] >= max_connections_per_ip) {
                            ::close(sock);
                            continue;
                        }
                        order = m_connections_per_ip[addr]++;
                    }

                    //TODO: here we can check if sock address is banned
                    //and we can drop the connection

                    // Find the least used session
                    sessions_event_loop *best_loop = event_loops.get();
                    for (uint32_t i = 1; i < event_loops_size; ++i) {
                        sessions_event_loop &loop = event_loops[i];
                        if (best_loop->active_sessions() > loop.active_sessions())
                            best_loop = &loop;
                    }
                    basic_server_session *session = nullptr;
                    try {
                        // Let's try to create a new session
                        if (ssl)
                            session = new server_session<ssl_socket_session>(best_loop, sock, std::move(addr), order);
                        else
                            session = new server_session<socket_session>(best_loop, sock, std::move(addr), order);
                        session->init_session();
                    } catch (const std::exception &e) {
                        WARNING(server_logger) << " Can't create session, reason: " << e.what();
                        if (session)
                            delete session;
                        else
                            ::close(sock);
                    } catch (...) {
                        // if we can't create a new session
                        // then just close the socket
                        WARNING(server_logger) << " Can't create session, for unknown reason";
                        if (session)
                            delete session;
                        else
                            ::close(sock);
                    }
                }
            }
        }
    }

    // Shutdown event loops
    for (uint32_t i = 0; i < event_loops_size; ++i)
        event_loops[i].shutdown();

    event_loops.reset();

    // Delete all active sessions
    for (auto &session : m_active_sessions)
        delete session;

    m_plugins.clear();
    return 0;
}

void server::server_session_created(basic_server_session *session)
{
    std::unique_lock<std::mutex> lock{m_active_sessions_mutex};
    m_active_sessions.insert(session);
    const auto count = m_active_sessions.size();
    m_active_sessions_count.store(count, std::memory_order_relaxed);
    if (count > m_peak_sessions)
        m_peak_sessions = count;
}

/*!
 * \brief server::server_session_deleted
 *
 * Called by the server_session when is deleted
 * \param session that was deleted
 */
void server::server_session_deleted(basic_server_session *session)
{
    {
        std::unique_lock<std::mutex> lock{m_active_sessions_mutex};
        m_active_sessions.erase(session);
        m_active_sessions_count.store(m_active_sessions.size(), std::memory_order_relaxed);
    }

    {
        const auto addr = session->peer_address();
        std::unique_lock<std::mutex> lock{m_connections_per_ip_mutex};
        auto it = m_connections_per_ip.find(addr);
        assert(it != m_connections_per_ip.end());
        if (--(it->second) == 0)
            m_connections_per_ip.erase(it);
    }
}

std::function<void (dracon::abstract_stream &, dracon::request &)> server::create_session(const dracon::request &request)
{
    for (const auto &plugin : m_plugins) {
        if (auto service = plugin.create_session(request))
            return service;
    }
    return {};
}

/*!
 * \brief server::peak_sessions
 * \return the peak of simulatneous connections since the beginning
 */
size_t server::peak_sessions() const noexcept
{
    return m_peak_sessions;
}

/*!
 * \brief server::active_sessions
 * \return the number of active connections
 */
size_t server::active_sessions() const noexcept
{
    return m_active_sessions_count.load(std::memory_order_relaxed);
}

/*!
 * \brief dracon::internal::server::uptime
 * \return the number of seconds since the server started
 */
std::chrono::seconds dracon::internal::server::uptime() const
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - m_start_time);
}

SSL_CTX *dracon::internal::server::ssl_context() const
{
    return m_ssl_context;
}

/*!
 * \brief server::server
 *
 * Creates the server object & the server events loop
 */
server::server()
{
    m_epoll_handler = epoll_create1(EPOLL_CLOEXEC);

    // register signal handlers
    struct sigaction sa;

    sa.sa_sigaction = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;

    if (sigaction(SIGFPE, &sa, nullptr) != 0)
        throw std::runtime_error{"Can't register SIGFPE signal callback"};

    if (sigaction(SIGILL, &sa, nullptr) != 0)
        throw std::runtime_error{"Can't register SIGILL signal callback"};

    if (sigaction(SIGINT, &sa, nullptr) != 0)
        throw std::runtime_error{"Can't register SIGINT signal callback"};

    if (sigaction(SIGSEGV, &sa, nullptr) != 0)
        throw std::runtime_error{"Can't register SIGSEGV signal callback"};

    if (sigaction(SIGTERM, &sa, nullptr) != 0)
        throw std::runtime_error{"Can't register SIGTERM signal callback"};

    // Ignore sigpipe
    signal(SIGPIPE, SIG_IGN);
}

/*!
 * \brief server::~server
 *
 * Cleanup everything
 */
server::~server()
{
    try {
        if (m_ssl_context)
            SSL_CTX_free(m_ssl_context);
    } catch (...) {}
}

} // namespace dracon::internal
