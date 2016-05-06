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

#include "raw_client.h"

#include <cstring>
#include <stdexcept>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dracon {
namespace test {

raw_client::raw_client(uint16_t port, std::chrono::milliseconds read_timeout)
{
    m_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0)
        throw std::runtime_error{"RawClient: can't create socket"};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(m_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(m_fd);
        m_fd = -1;
        throw std::runtime_error{"RawClient: can't connect to 127.0.0.1:" + std::to_string(port)};
    }

    int opt = 1;
    ::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    timeval tv{};
    tv.tv_sec = read_timeout.count() / 1000;
    tv.tv_usec = (read_timeout.count() % 1000) * 1000;
    ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

raw_client::~raw_client()
{
    close();
}

raw_client::raw_client(raw_client &&other) noexcept
    : m_fd(other.m_fd)
{
    other.m_fd = -1;
}

raw_client &raw_client::operator=(raw_client &&other) noexcept
{
    if (this != &other) {
        close();
        m_fd = other.m_fd;
        other.m_fd = -1;
    }
    return *this;
}

void raw_client::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool raw_client::send(std::string_view data)
{
    while (!data.empty()) {
        ssize_t n = ::send(m_fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        data.remove_prefix(size_t(n));
    }
    return true;
}

bool raw_client::send_slow(std::string_view data, size_t chunk, std::chrono::milliseconds delay)
{
    if (!chunk)
        chunk = 1;
    while (!data.empty()) {
        const auto part = data.substr(0, chunk);
        if (!send(part))
            return false;
        data.remove_prefix(part.size());
        if (!data.empty())
            std::this_thread::sleep_for(delay);
    }
    return true;
}

std::string raw_client::recv_all()
{
    std::string out;
    char buf[16 * 1024];
    for (;;) {
        ssize_t n = ::recv(m_fd, buf, sizeof(buf), 0);
        if (n > 0)
            out.append(buf, size_t(n));
        else
            break; // peer closed (0) or timeout/error (<0)
    }
    return out;
}

std::string raw_client::recv_responses(size_t count)
{
    std::string out;
    char buf[16 * 1024];
    while (response_count(out) < count) {
        ssize_t n = ::recv(m_fd, buf, sizeof(buf), 0);
        if (n > 0)
            out.append(buf, size_t(n));
        else
            break;
    }
    return out;
}

std::string raw_client::status(std::string_view raw)
{
    // HTTP/1.1 <status> ...
    const auto sp = raw.find(' ');
    if (sp == std::string_view::npos || sp + 4 > raw.size())
        return {};
    return std::string{raw.substr(sp + 1, 3)};
}

size_t raw_client::response_count(std::string_view raw)
{
    size_t count = 0;
    for (size_t pos = 0; (pos = raw.find("HTTP/1.", pos)) != std::string_view::npos; pos += 7)
        ++count;
    return count;
}

} // namespace test
} // namespace Dracon
