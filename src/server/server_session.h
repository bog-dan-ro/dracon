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

#include "sessions_event_loop.h"
#include "server_logger.h"


#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <type_traits>

#include <boost/coroutine2/coroutine.hpp>

#include "server.h"
#include "streams.h"

namespace dracon::internal {

struct wakeupper : dracon::abstract_stream::abstract_wakeupper
{
    wakeupper(sessions_event_loop *event_loop, basic_server_session *session)
        : m_event_loop(event_loop)
        , m_session(session)
    {}
    // abstract_wakeupper interface
    void wakeup() noexcept override
    {
        m_event_loop->wakeup_session(m_session);
    }
    sessions_event_loop *m_event_loop;
    basic_server_session *m_session;
};

class basic_server_session
{
public:
    basic_server_session(sessions_event_loop *event_loop, int sock, std::string sock_addr, uint32_t order);
    virtual ~basic_server_session();
    void init_session();

    inline uint32_t order() const noexcept { return m_order; }
    inline int sock() const noexcept { return m_sock;}
    const std::string &peer_address() const noexcept;

    void set_next_timeout(std::chrono::seconds seconds) noexcept
    {
        m_next_timeout = clock::now() + seconds;
        m_event_loop->notify_deadline(m_next_timeout);
    }

    /// True once the destructor started, @see sessions_event_loop::delete_later
    bool is_deleting() const noexcept { return m_deleting.load(std::memory_order_relaxed); }
    // basic_server_session interface
    time_point next_timeout() const noexcept
    {
        std::unique_lock<std::mutex> lock{m_stream_mutex};
        if (m_stream)
            return m_stream->next_timeout();
        return m_next_timeout;
    }

    virtual void process_events(uint32_t events) noexcept = 0;
    virtual void timeout() noexcept = 0;
    virtual void wakeup() noexcept = 0;

protected:
    void mark_deleting() noexcept { m_deleting.store(true, std::memory_order_relaxed); }

protected:
    int m_sock;
    uint32_t m_order;
    std::string m_peer_addr;
    sessions_event_loop *m_event_loop;
    mutable std::mutex m_stream_mutex;
    time_point m_next_timeout;
    std::unique_ptr<basic_http_session> m_stream;
    std::atomic_bool m_deleting{false};
};

template <typename SocketStream>
class server_session : public basic_server_session
{
    static_assert(std::is_base_of<basic_http_session, SocketStream>::value, "SocketStream must subclass basic_http_session");
public:
    server_session(sessions_event_loop *event_loop, int sock, std::string &&sock_addr, uint32_t order)
        : basic_server_session(event_loop, sock, std::move(sock_addr), order)
        , m_io_yield(std::bind(&server_session::io_loop, this, std::placeholders::_1))
    {
        TRACE(dracon::internal::server_logger) << (void*)this
                                     << " eventLoop: " << event_loop
                                     << " socket:" << sock;
        int opt = 1;
        if (setsockopt(m_sock, SOL_TCP, TCP_NODELAY, &opt, sizeof(int)))
            throw std::runtime_error{"Can't set socket option TCP_NODELAY"};
        set_next_timeout(std::is_same_v<SocketStream, ssl_socket_session> ? server::ssl_accept_timeout()
                                                                      : server::headers_timeout());
    }

    ~server_session() override
    {
        mark_deleting();
        quit_io_loop(std::make_error_code(std::errc::operation_canceled));
        try {
            m_event_loop->unregister_session(this);
        } catch (...) {}
        ::close(m_sock);
    }

    void quit_io_loop(std::error_code ec)
    {
        try {
            while (m_io_yield) m_io_yield(ec);
        } catch (const std::error_code &ec) {
            ERROR(server_logger) << ec.message();
        } catch (const std::exception &e) {
            ERROR(server_logger) << e.what();
        } catch (...) {
            ERROR(server_logger) << "Unhandled error";
        }
    }

    void process_events(uint32_t events) noexcept override
    {
        try {
            if (events & (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
                quit_io_loop(std::make_error_code(std::errc::io_error));
                m_event_loop->delete_later(this);
            } else if (events & (EPOLLIN | EPOLLPRI | EPOLLOUT)) {
                if (m_io_yield) {
                    m_io_yield({});
                } else {
                    m_event_loop->delete_later(this);
                }
            } else {
                WARNING(server_logger) << "Unhandled epool events " << events;
                m_event_loop->delete_later(this);
            }
        } catch (const std::exception &e) {
            DEBUG(server_logger) << m_peer_addr << e.what();
            m_event_loop->delete_later(this);
        } catch (...) {
            DEBUG(server_logger) << m_peer_addr << "Unkown exception, terminating the session";
            m_event_loop->delete_later(this);
        }
    }

    void timeout() noexcept override
    {
        quit_io_loop(std::make_error_code(std::errc::timed_out));
        m_event_loop->delete_later(this);
    }

    void wakeup() noexcept override
    {
        try {
            if (m_io_yield)
                m_io_yield({});
            else
                m_event_loop->delete_later(this);
        } catch (const std::exception &e) {
            ERROR(server_logger) << e.what();
            m_event_loop->delete_later(this);
        } catch (...) {
            ERROR(server_logger) << "Unhandled error";
            m_event_loop->delete_later(this);
        }
    }

protected:
    void io_loop(yield_type &yield)
    {
        try {
            {
                auto wu = std::make_shared<wakeupper>(m_event_loop,
                                                      static_cast<basic_server_session*>(this));
                auto stream = std::make_unique<SocketStream>(m_event_loop, m_sock,
                                                             yield, m_peer_addr,
                                                             wu);
                std::unique_lock<std::mutex> lock{m_stream_mutex};
                m_stream = std::move(stream);
            }
            m_stream->io_loop();
        } catch(...) {
            m_event_loop->delete_later(this);
        }
    }

protected:
    using call = boost::coroutines2::coroutine<std::error_code>::push_type;
    call m_io_yield;
};

} // namespace dracon::internal
