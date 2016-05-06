/*
    Copyright (C) 2026, BogDan Vatra <bogdan@kde.org>

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

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace dracon {
namespace test {

/*!
 * \brief The raw_client class
 *
 * A minimal blocking plaintext TCP client used by the security tests to send
 * requests that libcurl would refuse to produce: malformed request lines,
 * oversized or duplicated headers, smuggling vectors, pipelined requests, and
 * deliberately slow/partial writes. It never interprets the bytes it sends, so
 * the server's own parser is what is under test.
 */
class raw_client
{
public:
    /// Connects to 127.0.0.1:\a port (default Dracon HTTP port). Throws on failure.
    explicit raw_client(uint16_t port = 8080, std::chrono::milliseconds read_timeout = std::chrono::seconds{4});
    ~raw_client();

    raw_client(raw_client &&) noexcept;
    raw_client &operator=(raw_client &&) noexcept;
    raw_client(const raw_client &) = delete;
    raw_client &operator=(const raw_client &) = delete;

    /// Writes \a data to the socket. Returns false if the peer already closed.
    bool send(std::string_view data);

    /// Writes \a data one \a chunk of bytes at a time, sleeping \a delay between
    /// writes. Useful to exercise incremental parsing and slow-client handling.
    bool send_slow(std::string_view data, size_t chunk, std::chrono::milliseconds delay);

    /// Reads until the peer closes the connection or the read timeout elapses.
    /// Returns everything received so far.
    std::string recv_all();

    /// Reads until \a count HTTP responses (by "\r\n\r\n" head + Content-Length,
    /// or connection close) are seen, the peer closes, or the timeout elapses.
    std::string recv_responses(size_t count);

    /// Closes the socket. Further send()/recv() calls are no-ops / empty.
    void close();

    bool connected() const { return m_fd >= 0; }

    /// The 3-digit status of the first response in \a raw ("" if none).
    static std::string status(std::string_view raw);
    /// Number of "HTTP/1." response heads in \a raw.
    static size_t response_count(std::string_view raw);

private:
    int m_fd = -1;
};

} // namespace test
} // namespace Dracon
