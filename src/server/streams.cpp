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

    AGPL EXCEPTION:
    The AGPL license applies only to this file itself.

    As a special exception, the copyright holders of this file give you permission
    to use it, regardless of the license terms of your work, and to copy and distribute
    them under terms of your choice.
    If you do any changes to this file, these changes must be published under AGPL.
*/

#include "streams.h"

#include <limits.h>
#include <sys/uio.h>

#include <cstddef>

#include <dracon/http.h>
#include <dracon/logging.h>

#include "server.h"
#include "server_logger.h"
#include "sessions_event_loop.h"


namespace dracon::internal {

template<typename T>
using deleted_unique_ptr = std::unique_ptr<T,std::function<void(T*)>>;

using namespace std::chrono_literals;

basic_http_session::basic_http_session(sessions_event_loop *event_loop, int socket, yield_type &yield, const std::string &peer_address, const std::shared_ptr<abstract_wakeupper> &wakeupper)
    : m_yield(yield)
    , m_socket(socket)
    , m_event_loop(event_loop)
    , m_wakeupper(wakeupper)
    , m_peer_address(peer_address)
{}

basic_http_session::~basic_http_session() = default;

void basic_http_session::read(dracon::request &req) noexcept(false)
{
    if (req.state() == dracon::request::parse_state::completed)
        return;

    // Whatever goes wrong from here on (a body nobody wants to read, a body
    // over the limit, broken chunking, a dead socket) leaves the connection
    // with an unread request on it, so it can't be reused. Give up the keep
    // alive *before* the error response is written, or it would advertise a
    // keep alive the server is about to drop.
    struct keep_alive_guard
    {
        ~keep_alive_guard()
        {
            if (failed)
                session->set_keep_alive(std::chrono::seconds{0});
        }
        basic_http_session *session;
        bool failed = true;
    } guard{this};

    m_can_write_errror = true;
    auto &buffer = m_event_loop->shared_read_buffer;
    for (;;) {
        if (!m_pending.empty()) {
            m_pending.erase(0, m_parser.parse_body(m_pending, req));
            if (req.state() == dracon::request::parse_state::completed) {
                guard.failed = false;
                return;
            }
        }
        std::error_code ec;
        auto sz = read_some({buffer.data(), buffer.size()}, ec);
        if (ec)
            throw ec;
        if (!sz) {
            if ((ec = m_yield().get()))
                throw ec;
            continue;
        }
        std::string_view data{buffer.data(), size_t(sz)};
        if (m_pending.empty()) {
            data.remove_prefix(m_parser.parse_body(data, req));
            if (req.state() == dracon::request::parse_state::completed) {
                m_pending.assign(data);
                guard.failed = false;
                return;
            }
        }
        m_pending.append(data);
    }
}

void basic_http_session::write(dracon::const_buffer buffer) noexcept(false)
{
    while (buffer.length) {
        std::error_code ec;
        size_t written = write_some(buffer, ec);
        if (!written) {
            if ((ec = m_yield().get()))
                throw ec;
            continue;
        }
        if (ec)
            throw ec;
        buffer.c_ptr += written;
        buffer.length -= written;
    }
}

void basic_http_session::write(std::span<const dracon::const_buffer> buffers) noexcept(false)
{
    size_t total = 0;
    for (const auto &buff : buffers)
        total += buff.length;
    if (!total)
        return;

    // A partial write has to shorten the first buffer still pending, and the
    // caller's array must not notice, so the descriptors are copied once into
    // the session's scratch vector. It keeps its capacity, so this stops
    // allocating after the first vectored write on the connection - the old
    // code took the vector by value and built a fresh one on *every* partial
    // write.
    m_write_buffers.assign(buffers.begin(), buffers.end());
    std::span<dracon::const_buffer> pending{m_write_buffers};
    while (!pending.empty()) {
        std::error_code ec;
        auto written = write_some(pending, ec);
        if (ec)
            throw ec;
        if (!written) {
            if ((ec = m_yield().get()))
                throw ec;
            continue;
        }
        // drop whatever went out completely
        while (!pending.empty() && pending.front().length <= size_t(written)) {
            written -= pending.front().length;
            pending = pending.subspan(1);
        }
        // ... and shorten the one which went out half way
        if (!pending.empty() && written) {
            pending.front().c_ptr += written;
            pending.front().length -= size_t(written);
        }
    }
}

std::error_code dracon::internal::basic_http_session::yield() noexcept
{
    return m_yield().get();
}

std::shared_ptr<dracon::abstract_stream::abstract_wakeupper> basic_http_session::wakeupper() const noexcept
{
    return m_wakeupper;
}

void basic_http_session::set_keep_alive(std::chrono::seconds seconds) noexcept
{
    m_keep_alive = seconds;
}

std::chrono::seconds basic_http_session::keep_alive() const noexcept
{
    return m_keep_alive;
}

const std::string &basic_http_session::peer_address() const noexcept
{
    return m_peer_address;
}

int basic_http_session::socket_write_size() const noexcept(false)
{
    if (!m_socket_write_size) {
        int optval = 0;
        socklen_t optlen = sizeof(optval);
        getsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &optval, &optlen);
        m_socket_write_size = optval / 2;
    }
    return m_socket_write_size;
}

void basic_http_session::set_socket_write_size(int size) noexcept(false)
{
    if (setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &size, sizeof(int)))
        throw std::make_error_code(std::errc::invalid_argument);
    m_socket_write_size = 0;
}

int basic_http_session::socket_read_size() const noexcept(false)
{
    if (!m_socket_read_size) {
        int optval = 0;
        socklen_t optlen = sizeof(optval);
        getsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &optval, &optlen);
        m_socket_read_size = optval / 2;
    }
    return m_socket_read_size;
}

void basic_http_session::set_socket_read_size(int size) noexcept(false)
{
    if (setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &size, sizeof(int)))
        throw std::make_error_code(std::errc::invalid_argument);
    m_socket_read_size = 0;
}

std::chrono::seconds basic_http_session::session_timeout() const noexcept
{
    return m_session_timeout;
}

void basic_http_session::set_session_timeout(std::chrono::seconds seconds) noexcept
{
    m_session_timeout = seconds;
    if (seconds.count()) {
        m_session_timeout_time_point = clock::now() + seconds;
        m_event_loop->notify_deadline(m_session_timeout_time_point);
    } else {
        m_session_timeout_time_point = {};
    }
}

time_point basic_http_session::next_timeout() const noexcept
{
    return m_session_timeout_time_point;
}

void basic_http_session::io_loop()
{
    try {
        set_session_timeout(server::headers_timeout());
        do {
            dracon::request req = read_headers();
            set_keep_alive(req.keep_alive() * server::keep_alive_timeout());
            auto session = server::instance().create_session(req);
            if (!session) {
                INFO(dracon::internal::server_logger) << peer_address() << " invalid url " << req.method() << " " << req.url();
                write(dracon::response{503}.to_string());
                break;
            }
            size_t content_length = req.content_length();
            if (content_length != dracon::chunked_data)
                set_session_timeout(server::keep_alive_timeout() + 1s * (content_length / (512 * 1024)));
            else
                set_session_timeout(5min); // In this case the session should set a proper timeout
            session(*this, req);
            if (req.state() != dracon::request::parse_state::completed) {
                WARNING(dracon::internal::server_logger) << peer_address() << " unread request body for "
                                               << req.method() << " " << req.url()
                                               << ", closing the connection";
                set_keep_alive(0s);
            }
            set_session_timeout(keep_alive());
            server::instance().session_served();
        } while (!m_yield.get() && m_keep_alive.count());
    } catch (int error) {
        DEBUG(dracon::internal::server_logger) << peer_address() << " status code " << error;
        if (m_can_write_errror) {
            write(dracon::response{error > 0 && error < std::numeric_limits<uint16_t>::max()
                                       ? uint16_t(error)
                                       : uint16_t(500)}
                      .to_string());
        }
    } catch (const dracon::response &res) {
        DEBUG(dracon::internal::server_logger) << peer_address() << " status code " << res.status_code() << " body " << res.body();
        if (m_can_write_errror)
            write(res.to_string());
    } catch (const std::exception &e) {
        DEBUG(dracon::internal::server_logger) << peer_address() << " std message " << e.what();
        if (m_can_write_errror)
            write(dracon::response{500, e.what()}.to_string());
    } catch (const std::error_code &ec) {
        DEBUG(dracon::internal::server_logger) <<  peer_address() << " error code " << ec.message();
        if (m_can_write_errror)
            write(dracon::response{500, ec.message()}.to_string());
    } catch (...) {
        DEBUG(dracon::internal::server_logger) << peer_address() << " Unknown error";
        if (m_can_write_errror)
            write(dracon::response{}.to_string());
    }
    shutdown();
}

dracon::request basic_http_session::read_headers()
{
    dracon::request req;
    auto head_parsed = [](const dracon::request &r) {
        return r.state() == dracon::request::parse_state::headers_completed
               || r.state() == dracon::request::parse_state::completed;
    };
    auto &buffer = m_event_loop->shared_read_buffer;
    for (;;) {
        // The parser consumes whatever complete lines it has; anything it can't
        // consume yet (a partial line, the body, a pipelined request) stays in
        // m_pending to be fed again with the next read.
        if (!m_pending.empty()) {
            m_pending.erase(0, m_parser.parse_head(m_pending, req));
            if (head_parsed(req))
                return req;
        }
        std::error_code ec;
        auto sz = read_some({buffer.data(), buffer.size()}, ec);
        if (ec)
            throw ec;
        if (!sz) {
            if ((ec = m_yield().get()))
                throw ec;
            continue;
        }
        m_can_write_errror = true;
        std::string_view data{buffer.data(), size_t(sz)};
        if (m_pending.empty()) {
            // fast path: parse straight from the shared read buffer
            data.remove_prefix(m_parser.parse_head(data, req));
            if (head_parsed(req)) {
                m_pending.assign(data); // body and/or pipelined request(s)
                return req;
            }
        }
        m_pending.append(data); // keep the unconsumed head tail
    }
}


socket_session::socket_session(sessions_event_loop *event_loop, int socket, yield_type &yield, const std::string &peer_address, const std::shared_ptr<abstract_wakeupper> &wakeupper)
    : basic_http_session(event_loop, socket, yield, peer_address, wakeupper)
{}

void socket_session::shutdown() noexcept
{
    ::shutdown(m_socket, SHUT_RDWR);
}

ssize_t socket_session::read_some(mutable_buffer buff, std::error_code &ec) noexcept
{
    ssize_t res = ::read(m_socket, buff.ptr, buff.length);
    if (res < 0) {
        if (errno != EAGAIN) {
            ec = std::make_error_code(std::errc(errno));
        } else {
            ec = {};
            res = 0;
        }
    } else {
        ec = {};
    }
    return res;
}

ssize_t socket_session::write_some(dracon::const_buffer buff, std::error_code &ec) noexcept
{
    ssize_t res = ::write(m_socket, buff.ptr, buff.length);
    if (res < 0) {
        if (errno != EAGAIN) {
            ec = std::make_error_code(std::errc(errno));
        } else {
            ec = {};
            res = 0;
        }
    } else {
        ec = {};
        m_can_write_errror = false;
    }
    return res;
}

// The vectored writes hand const_buffer straight to writev, so the two have to
// stay interchangeable
static_assert(sizeof(dracon::const_buffer) == sizeof(iovec));
static_assert(offsetof(dracon::const_buffer, ptr) == offsetof(iovec, iov_base));
static_assert(offsetof(dracon::const_buffer, length) == offsetof(iovec, iov_len));

ssize_t socket_session::write_some(std::span<const dracon::const_buffer> buff, std::error_code &ec) noexcept
{
    // writev fails with EINVAL past IOV_MAX, and the caller loops over whatever
    // is left anyway
    const auto count = int(std::min<size_t>(buff.size(), IOV_MAX));
    ssize_t res = ::writev(m_socket, reinterpret_cast<const iovec *>(buff.data()), count);
    if (res < 0) {
        if (errno != EAGAIN) {
            ec = std::make_error_code(std::errc(errno));
        } else {
            ec = {};
            res = 0;
        }
    } else {
        ec = {};
        m_can_write_errror = false;
    }
    return res;
}

ssl_socket_session::ssl_socket_session(sessions_event_loop *event_loop, int socket, yield_type &yield, const std::string &peer_address, const std::shared_ptr<abstract_wakeupper> &wakeupper)
    : basic_http_session(event_loop, socket, yield, peer_address, wakeupper)
    , m_ssl(std::unique_ptr<SSL, void (*)(SSL *)>(SSL_new(server::instance().ssl_context()), SSL_free))
{
    if (!m_ssl)
        throw std::runtime_error(ERR_error_string(ERR_get_error(), nullptr));

    if (!SSL_set_fd(m_ssl.get(), m_socket))
        throw std::runtime_error(ERR_error_string(ERR_get_error(), nullptr));

    set_session_timeout(server::ssl_accept_timeout());
    int ret;
    while ((ret = SSL_accept(m_ssl.get())) != 1) {
        int err = SSL_get_error(m_ssl.get(), ret);
        switch (err) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            if (auto ec = m_yield().get())
                throw ec;
            continue;
        default:
            DEBUG(dracon::internal::server_logger) << m_peer_address << " SSL_accept failed: "
                                         << ERR_reason_error_string(ERR_peek_last_error());
            throw std::make_error_code(std::errc::io_error);
        }
    }
}

void ssl_socket_session::shutdown() noexcept
{
    set_session_timeout(server::ssl_shutdown_timeout());
    if (SSL_is_init_finished(m_ssl.get()) == 1) {
        int count = 5;
        while (count--) {
            int res = SSL_shutdown(m_ssl.get());
            if (!res && !m_yield().get())
                continue;
            break;
        }
    }
    ::shutdown(m_socket, SHUT_RDWR);
}

ssize_t ssl_socket_session::read_some(mutable_buffer buff, std::error_code &ec) noexcept
{
    // make sure we start reading with no pending errors
    ERR_clear_error();
    auto sz = SSL_read(m_ssl.get(), buff.ptr, buff.length < INT_MAX ? int(buff.length) : INT_MAX);
    if (sz <= 0) {
        int err = SSL_get_error(m_ssl.get(), sz);
        switch (err) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            ec = {};
            return 0;
        case SSL_ERROR_ZERO_RETURN: // peer closed the TLS connection cleanly
            ec = std::make_error_code(std::errc::connection_reset);
            return -1;
        default:
            DEBUG(dracon::internal::server_logger) << peer_address() << " SSL_read failed: "
                                         << ERR_reason_error_string(ERR_peek_last_error());
            ec = std::make_error_code(std::errc::io_error);
            return -1;
        }
    }
    return sz;
}

ssize_t ssl_socket_session::write_some(dracon::const_buffer buff, std::error_code &ec) noexcept
{
    // make sure we start writing with no pending errors
    ERR_clear_error();
    auto sz = SSL_write(m_ssl.get(), buff.ptr, buff.length < INT_MAX ? static_cast<int>(buff.length) : INT_MAX);
    if (sz <= 0) {
        int err = SSL_get_error(m_ssl.get(), sz);
        switch (err) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            ec = {};
            return 0;
        default:
            DEBUG(dracon::internal::server_logger) << peer_address() << " SSL_write failed: "
                                         << ERR_reason_error_string(ERR_peek_last_error());
            ec = std::make_error_code(std::errc::io_error);
            return -1;
        }
    }
    m_can_write_errror = false;
    return sz;
}

ssize_t ssl_socket_session::write_some(std::span<const dracon::const_buffer> buffers,
                                    std::error_code &ec) noexcept
{
    if (buffers.empty()) {
        ec = {};
        return 0;
    }
    // Don't copy the buffers if the first piece is bigger than the socket write buffer size,
    // or we have only one buffer
    size_t socket_size = socket_write_size();
    if (buffers.size() == 1 || buffers.front().length >= socket_size)
        return write_some(buffers.front(), ec);

    auto flat_buffer = m_event_loop->shared_write_buffer(socket_size);
    flat_buffer->reset();
    char *pos = flat_buffer->data();
    const char *end = pos + std::min(flat_buffer->size(), socket_size);
    for (size_t i = 0; i < buffers.size() && pos != end; i++) {
        const size_t size = std::min<size_t>(buffers[i].length, end - pos);
        memcpy(pos, buffers[i].ptr, size);
        pos += size;
    }
    flat_buffer->set_current_size(pos - flat_buffer->data());
    return write_some(flat_buffer->current_string(), ec);
}

} // namespace dracon::internal
