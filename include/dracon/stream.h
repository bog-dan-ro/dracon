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

#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <memory>
#include <vector>
#include <system_error>

#include <dracon/utils.h>

namespace dracon {

using namespace std::string_view_literals;

/// The terminating chunk of a chunked response, written by ~chunked_stream()
inline constexpr auto end_of_chunked_stream = "0\r\n\r\n"sv;


/// The line terminator of every HTTP header and chunk header
inline constexpr auto crlf_string = "\r\n"sv;

/*!
 * \brief The const_buffer struct
 *
 * A pointer and a length, laid out exactly like an iovec so that a span of
 * them can be handed to writev() as it is. It does not own the memory and
 * does not copy it, so whatever it points at has to outlive the write.
 *
 * There are implicit constructors from std::string_view and std::string,
 * which is what makes "stream << some_string" and write({a, b, c}) read well.
 */
struct const_buffer
{
    const_buffer() = default;
    const_buffer(const void *ptr, size_t length)
        : ptr(ptr)
        , length(length)
    {}
    const_buffer(std::string_view data)
        : ptr(data.data())
        , length(data.size())
    {}
    const_buffer(const std::string &data)
        : ptr(data.data())
        , length(data.size())
    {}
    union {
        const void *ptr = nullptr;
        const char *c_ptr;
    };
    size_t length = 0;
};

class request;
class abstract_stream
{
public:
    struct abstract_wakeupper
    {
        virtual ~abstract_wakeupper() = default;
        virtual void wakeup() noexcept = 0;
    };

public:
    virtual ~abstract_stream() = default;
    /*!
     * \brief read
     *
     * Reads the rest of \a req from the socket: the body is handed to the
     * callback the session registered with request::append_body_callback(), and
     * the request is left in request::parse_state::completed.
     *
     * Prefer the operator>>(abstract_stream &, request &) spelling, which also
     * answers an Expect: 100-continue.
     *
     * Yields until the whole request has arrived, and throws a dracon::response
     * on a malformed one, an int status code on a body which is larger than the
     * session allowed, or a std::system_error when the connection dies.
     */
    virtual void read(request &req) = 0;

    /*!
     * \brief write
     *
     * Writes \a buffer to the socket, yielding as often as needed until all of
     * it is out. Throws a std::system_error if the connection dies meanwhile.
     */
    virtual void write(const_buffer buffer) = 0;

    /*!
     * \brief write
     *
     * Writes the specified \a buffers to socket, in one syscall where the
     * socket allows it.
     *
     * A span, so an array, a vector or any contiguous range works and nothing
     * is allocated to make the call:
     * \code
     *     const dracon::const_buffer parts[]{head, body};
     *     stream.write(parts);
     * \endcode
     * The buffers, and whatever they point at, only have to live until the
     * call returns.
     */
    virtual void write(std::span<const const_buffer> buffers) = 0;

    /*!
     * \brief yield
     *
     * Suspends the session and gives the thread back to the event loop. It
     * resumes when something calls wakeup() on the object wakeupper()
     * returned, which is how a session waits for work running on another
     * thread without blocking the loop.
     *
     * \return an empty error code when the session was woken up, and the
     * reason otherwise: the client hung up, or the session timed out. A
     * handler has nothing left to answer with at that point and should throw
     * it.
     */
    virtual std::error_code yield() noexcept = 0;

    /*!
     * \brief wakeupper
     *
     * \return the handle another thread uses to resume a session which is
     * sitting in yield(). Take it before handing the work over, and keep the
     * shared_ptr alive for as long as that thread may still call wakeup():
     * waking up a session which is already gone is safe, dereferencing a dead
     * handle is not.
     */
    virtual std::shared_ptr<abstract_wakeupper> wakeupper() const noexcept = 0;

    /*!
     * \brief keep_alive
     * \return for how long the connection stays open after the response, or 0
     * when it is closed right away.
     */
    virtual std::chrono::seconds keep_alive() const noexcept = 0;

    /*!
     * \brief set_keep_alive
     *
     * Keeps the connection open for \a seconds after the response has been
     * sent, waiting for the next request on it. 0 closes it immediately.
     *
     * The server sets this from the request and the configured keep alive
     * timeout, so a session only calls it to shorten or to refuse the reuse of
     * a connection. Writing a response which carries its own
     * response::set_keep_alive() calls it as well.
     */
    virtual void set_keep_alive(std::chrono::seconds seconds) noexcept = 0;

    /*!
     * \brief peer_address
     * \return the numeric address of the client, "127.0.0.1" or
     * "::ffff:10.0.0.7". It is the address of the peer on the socket, so
     * behind a proxy this is the proxy.
     */
    virtual const std::string& peer_address() const noexcept = 0;

    /*!
     * \brief is_secured_connection
     * \return true when the connection is an SSL one
     */
    virtual bool is_secured_connection() const noexcept { return false; }

    /*!
     * \brief socket_write_size
     * \return the socket send buffer size in bytes
     */
    virtual int socket_write_size() const = 0;

    /*!
     * \brief set_socket_write_size
     *
     * Sets the socket sending buffer size
     *
     * \param size in bytes
     */
    virtual void set_socket_write_size(int size) = 0;

    /*!
     * \brief socket_read_size
     * \return the socket receive buffer size in bytes
     */
    virtual int socket_read_size() const = 0;

    /*!
     * \brief set_socket_read_size
     *
     * Sets the socket receiving buffer size
     *
     * \param size in bytes
     */
    virtual void set_socket_read_size(int size) = 0;

    /*!
     * \brief session_timeout
     * \return how long the whole session may still take
     */
    virtual std::chrono::seconds session_timeout() const noexcept = 0;

    /*!
     * \brief set_session_timeout
     *
     * Gives the session \a seconds from now to finish. Once it is up the
     * connection is closed, whether the session is reading, writing or sitting
     * in yield().
     *
     * The server derives a timeout from the Content-Length of the request and
     * from the length of the response, which covers most sessions. A session
     * which reads a chunked request, or which waits on something slow, gets
     * the five minute default and should set a timeout of its own.
     */
    virtual void set_session_timeout(std::chrono::seconds seconds) noexcept = 0;
};

/*!
 * \brief The next_layer_stream class
 *
 * Forwards every abstract_stream call to the stream underneath. It is the base
 * for a stream which changes how the body is written, such as chunked_stream:
 * derive from it and override only what the layer actually does.
 *
 * The next layer is held by reference and has to outlive the wrapper.
 */
class next_layer_stream : public abstract_stream
{
public:
    next_layer_stream(abstract_stream &next_layer)
        : m_next_layer(next_layer)
    {}

    // abstract_stream interface
    void read(request &req) override
    {
        m_next_layer.read(req);
    }

    std::error_code yield() noexcept override
    {
        return m_next_layer.yield();
    }

    std::shared_ptr<abstract_wakeupper> wakeupper() const noexcept override
    {
        return m_next_layer.wakeupper();
    }

    void set_keep_alive(std::chrono::seconds seconds) noexcept override
    {
        m_next_layer.set_keep_alive(seconds);
    }

    std::chrono::seconds keep_alive() const noexcept override
    {
        return m_next_layer.keep_alive();
    }

    const std::string& peer_address() const noexcept override
    {
        return m_next_layer.peer_address();
    }

    bool is_secured_connection() const noexcept override
    {
        return m_next_layer.is_secured_connection();
    }

    int socket_write_size() const override
    {
        return m_next_layer.socket_write_size();
    }

    void set_socket_write_size(int size) override
    {
        m_next_layer.set_socket_write_size(size);
    }

    int socket_read_size() const override
    {
        return m_next_layer.socket_read_size();
    }

    void set_socket_read_size(int size) override
    {
        m_next_layer.set_socket_read_size(size);
    }

    std::chrono::seconds session_timeout() const noexcept override
    {
        return m_next_layer.session_timeout();
    }

    void set_session_timeout(std::chrono::seconds seconds) noexcept override
    {
        m_next_layer.set_session_timeout(seconds);
    }

protected:
    abstract_stream &m_next_layer;
};


/*!
 * \brief The chunked_stream class
 *
 * Wraps every write in the "<hex size>CRLF ... CRLF" framing of a chunked
 * response, and writes the terminating chunk when it is destroyed. Empty
 * writes are dropped, since a zero sized chunk would end the response.
 *
 * Announce the encoding first, then keep the wrapper in the scope which
 * produces the body:
 * \code
 *     stream << dracon::response{200}.set_content_length(dracon::chunked_data);
 *     {
 *         dracon::chunked_stream chunked{stream};
 *         for (const auto &row : rows)
 *             chunked << row;
 *     }   // the terminating chunk goes out here
 * \endcode
 * A throw inside that scope still emits the terminating chunk, which tells the
 * client the response is complete when it is not. Where that matters, let the
 * exception escape the scope of the chunked_stream, or close the connection.
 */
class chunked_stream final : public next_layer_stream
{
public:
    chunked_stream(abstract_stream &next_layer)
        : next_layer_stream(next_layer)
    {}

    ~chunked_stream()
    {
        try {
            m_next_layer.write(end_of_chunked_stream);
        } catch (...) {}
    }

    // abstract_stream interface
    void write(const_buffer buff) final
    {
        if (!buff.length)
            return;
        char header[24];
        const const_buffer parts[]{chunk_header(header, buff.length), buff, crlf_string};
        m_next_layer.write(parts);
    }

    void write(std::span<const const_buffer> buffers) final
    {
        size_t size = 0;
        for (const auto &buffer : buffers)
            size += buffer.length;
        if (!size)
            return;
        char header[24];
        std::vector<const_buffer> parts;
        parts.reserve(buffers.size() + 2);
        parts.emplace_back(chunk_header(header, size));
        parts.insert(parts.end(), buffers.begin(), buffers.end());
        parts.emplace_back(crlf_string);
        m_next_layer.write(parts);
    }

private:
    /// Writes the "<hex size>CRLF" chunk header into \a buffer, which must be
    /// at least 24 bytes long, and returns a view of it
    static std::string_view chunk_header(char *buffer, size_t size)
    {
        auto res = std::to_chars(buffer, buffer + 18, size, 16);
        *res.ptr++ = '\r';
        *res.ptr++ = '\n';
        return {buffer, size_t(res.ptr - buffer)};
    }
};

/// Writes \a buff to \a stream. std::string and std::string_view convert to
/// const_buffer, so "stream << some_string" works as it reads.
inline abstract_stream& operator<<(abstract_stream &stream, const_buffer buff)
{
    stream.write(buff);
    return stream;
}

/*!
 * \brief The ostream_buffer class
 *
 * A std::streambuf on top of an abstract_stream, for a body which is easier to
 * produce with the standard formatting machinery than with writes:
 * \code
 *     dracon::ostream_buffer buffer{stream};
 *     std::ostream out{&buffer};
 *     out << "took " << std::fixed << std::setprecision(2) << seconds << "s\n";
 * \endcode
 * Output is collected until it fills the socket send buffer, so a formatted
 * body costs about as many writes as the same body written by hand. The
 * destructor flushes what is left, but it swallows the error if that write
 * fails; call pubsync() yourself where the failure has to be noticed.
 *
 * It writes straight to the stream, which makes it as good a fit for a chunked
 * response: give it a chunked_stream and every flush becomes one chunk.
 */
class ostream_buffer : public std::streambuf
{
public:
    ostream_buffer(abstract_stream &stream)
        : m_stream(stream)
        , m_capacity(size_t(std::max(4096, stream.socket_write_size())))
    {
        reserve_buffer();
    }

    ~ostream_buffer() override
    {
        try {
            sync();
        } catch (...) {}
    }

protected:
    void reserve_buffer()
    {
        m_buffer.reserve(m_capacity);
    }

    int sync() override
    {
        if (!m_buffer.empty()) {
            std::string pending;
            pending.swap(m_buffer);
            reserve_buffer();
            m_stream.write(pending);
        }
        return 0;
    }

    int_type overflow(int_type __c) override
    {
        if (traits_type::eq_int_type(__c, traits_type::eof()))
            return traits_type::not_eof(__c);
        if (m_buffer.size() >= m_buffer.capacity())
            sync();
        m_buffer += traits_type::to_char_type(__c);
        return __c;
    }

    std::streamsize xsputn(const char_type *__s, std::streamsize __n) override
    {
        if (__n < 0)
            return __n;
        auto sz = size_t(__n);
        while (sz) {
            if (sz > m_buffer.capacity()) {
                if (m_buffer.size()) {
                    std::string pending;
                    pending.swap(m_buffer);
                    reserve_buffer();
                    const const_buffer parts[]{pending, std::string_view{__s, sz}};
                    m_stream.write(parts);
                } else {
                    m_stream.write(std::string_view{__s, sz});
                }
                return sz;
            }
            if (m_buffer.size() >= m_buffer.capacity())
                sync();
            size_t len = std::min(size_t(sz), m_buffer.capacity() - m_buffer.size());
            m_buffer.append(__s, len);
            sz -= len;
            __s += len;
        }
        return __n;
    }


private:
    abstract_stream &m_stream;
    std::string m_buffer;
    const size_t m_capacity;
};

} // namespace Dracon
