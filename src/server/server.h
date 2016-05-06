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

#pragma once

#include <netdb.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <openssl/ssl.h>

#include <dracon/logging.h>
#include <dracon/utils.h>

#include "server_plugin.h"

namespace dracon {
class abstract_stream;
class request;
}

namespace dracon::internal {

class basic_server_session;
class sessions_event_loop;

class server
{
public:
    static server &instance();
    int exec(int argc, char *argv[]);
    void server_session_created(basic_server_session *session);
    void server_session_deleted(basic_server_session *session);
    std::function<void(dracon::abstract_stream &, dracon::request &)> create_session(const dracon::request &request);
    size_t peak_sessions() const noexcept;
    size_t active_sessions() const noexcept;
    std::chrono::seconds uptime() const;
    inline void session_served() { ++m_served_sessions; }
    uint64_t served_sessions() const { return m_served_sessions; }
    SSL_CTX *ssl_context() const;
    static void exit_signal_handler();
    static std::chrono::seconds keep_alive_timeout();
    static std::chrono::seconds headers_timeout();
    static std::chrono::seconds ssl_accept_timeout();
    static std::chrono::seconds ssl_shutdown_timeout();

private:
    server();
    ~server();

    enum socket_type {
        IPV4,
        IPV6
    };
    int bind(socket_type type, int port);

private:
    std::atomic_bool m_shutdown{false};
    std::atomic<size_t> m_peak_sessions{0};
    std::atomic<size_t> m_served_sessions{0};
    int m_events_size = 0;
    int m_epoll_handler;
    mutable std::mutex m_active_sessions_mutex;
    std::unordered_set<basic_server_session*> m_active_sessions;
    // The count is published separately: /server_status and the accept loop
    // read it, and neither may contend on the sessions mutex to do so
    std::atomic<size_t> m_active_sessions_count{0};
    std::vector<server_plugin> m_plugins;
    std::chrono::system_clock::time_point m_start_time;
    SSL_CTX *m_ssl_context = nullptr;
    std::mutex m_connections_per_ip_mutex;
    std::map<std::string, uint32_t> m_connections_per_ip;
    int m_https4_sock = -1;
    int m_https6_sock = -1;
    static std::chrono::seconds s_headers_timeout;
    static std::chrono::seconds s_ssl_accept_timeout;
    static std::chrono::seconds s_ssl_shutdown_timeout;
    static std::chrono::seconds s_keep_alive_timeout;
};

} // namespace dracon::internal
