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

#include <span>

#include <boost/coroutine2/coroutine.hpp>

#include <dracon/stream.h>
#include <dracon/utils.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "http_parser.h"

namespace dracon::internal {

using yield_type = boost::coroutines2::coroutine<std::error_code>::pull_type;
using clock = std::chrono::high_resolution_clock;
using time_point = std::chrono::time_point<clock>;
using namespace std::chrono_literals;

class sessions_event_loop;

struct mutable_buffer
{
    mutable_buffer() = default;
    mutable_buffer(std::string &data)
        : ptr(&data[0])
        , length(data.length())
    {}
    mutable_buffer(void *ptr, size_t length)
        : ptr(ptr)
        , length(length)
    {}
    void *ptr = nullptr;
    size_t length = 0;
};

class basic_http_session : public dracon::abstract_stream
{
public:
    basic_http_session(sessions_event_loop *event_loop, int socket, yield_type &yield, const std::string& peer_address, const std::shared_ptr<abstract_wakeupper> &wakeupper);
    ~basic_http_session() override;

    // abstract_stream interface
    void read(dracon::request &req) noexcept(false) override;
    void write(dracon::const_buffer buffer) noexcept(false) override;
    void write(std::span<const dracon::const_buffer> buffers) noexcept(false) override;

    std::error_code yield() noexcept override;
    std::shared_ptr<abstract_wakeupper> wakeupper() const noexcept override;

    void set_keep_alive(std::chrono::seconds seconds) noexcept override;
    std::chrono::seconds keep_alive() const noexcept override;

    const std::string& peer_address() const noexcept override;

    int socket_write_size() const noexcept(false) override;
    void set_socket_write_size(int size) noexcept(false) override;

    int socket_read_size() const noexcept(false) override;
    void set_socket_read_size(int size) noexcept(false) override;

    std::chrono::seconds session_timeout() const noexcept override;
    void set_session_timeout(std::chrono::seconds seconds) noexcept override;

    time_point next_timeout() const noexcept;

    void io_loop();

protected:
    virtual void shutdown() noexcept = 0;
    virtual ssize_t read_some(mutable_buffer buff, std::error_code &ec) noexcept = 0;
    virtual ssize_t write_some(dracon::const_buffer buff, std::error_code &ec) noexcept = 0;
    virtual ssize_t write_some(std::span<const dracon::const_buffer> buff, std::error_code &ec) noexcept = 0;

    dracon::request read_headers();

protected:
    yield_type &m_yield;
    int m_socket;
    std::chrono::seconds m_keep_alive{0};
    std::chrono::seconds m_session_timeout{0};
    time_point m_session_timeout_time_point;
    sessions_event_loop *m_event_loop;

    http_request_parser m_parser;
    // Scratch copy of the descriptors handed to write(): the caller's array is
    // never touched, and this one keeps its capacity between calls
    std::vector<dracon::const_buffer> m_write_buffers;
    // SO_SNDBUF / SO_RCVBUF are asked for on every vectored write, cache them
    mutable int m_socket_write_size = 0;
    mutable int m_socket_read_size = 0;
    bool m_can_write_errror = false;
    std::shared_ptr<abstract_wakeupper> m_wakeupper;
    std::string m_pending; // received bytes not consumed yet
    const std::string &m_peer_address;
};

class socket_session final: public basic_http_session
{
public:
    socket_session(dracon::internal::sessions_event_loop *event_loop, int socket, yield_type &yield, const std::string &peer_address, const std::shared_ptr<abstract_wakeupper> &wakeupper);

protected:
    // basic_http_session interface
    void shutdown() noexcept final;
    ssize_t read_some(mutable_buffer buff, std::error_code &ec) noexcept final;
    ssize_t write_some(dracon::const_buffer buff, std::error_code &ec) noexcept final;
    ssize_t write_some(std::span<const dracon::const_buffer> buff, std::error_code &ec) noexcept final;
};

class ssl_socket_session final: public basic_http_session
{
public:
    ssl_socket_session(dracon::internal::sessions_event_loop *event_loop, int socket, yield_type &yield, const std::string& peer_address, const std::shared_ptr<abstract_wakeupper> &wakeupper);

protected:
    // basic_http_session interface
    void shutdown() noexcept final;
    ssize_t read_some(mutable_buffer buff, std::error_code &ec) noexcept final;
    ssize_t write_some(dracon::const_buffer buff, std::error_code &ec) noexcept final;
    ssize_t write_some(std::span<const dracon::const_buffer> buffers, std::error_code &ec) noexcept final;
    bool is_secured_connection() const noexcept final { return true; }

private:
    std::unique_ptr<SSL, void (*)(SSL *)> m_ssl;
};
} // namespace dracon::internal
