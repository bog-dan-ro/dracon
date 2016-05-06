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

#include <boost/test/unit_test.hpp>

#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <http_parser.h>

using namespace std::string_view_literals;
using dracon::internal::http_request_parser;
using dracon::request;
using state = dracon::request::parse_state;

namespace {

// Mirrors what basic_http_session does: accumulates the received bytes and
// feeds them to the parser until the request is complete
struct parser_driver
{
    http_request_parser parser;
    request req;
    std::string pending;
    std::string body;
    bool head_done = false;
    size_t max_body = std::numeric_limits<size_t>::max() - 1;

    bool feed(std::string_view chunk)
    {
        pending.append(chunk);
        if (!head_done) {
            pending.erase(0, parser.parse_head(pending, req));
            if (req.state() != state::headers_completed && req.state() != state::completed)
                return false; // head not fully parsed yet
            head_done = true;
            req.append_body_callback([this](std::string_view data) { body.append(data); }, max_body);
        }
        if (req.state() != state::completed)
            pending.erase(0, parser.parse_body(pending, req));
        return req.state() == state::completed;
    }
};

// Returns the status code of the dracon::response thrown by fn, or 0
uint16_t status_of(const std::function<void()> &fn)
{
    try {
        fn();
    } catch (const dracon::response &res) {
        return res.status_code();
    }
    return 0;
}

uint16_t head_status(std::string_view data)
{
    return status_of([data] {
        request req;
        http_request_parser parser;
        parser.parse_head(data, req);
    });
}

request parse_head(std::string_view data)
{
    request req;
    http_request_parser parser;
    BOOST_CHECK_EQUAL(parser.parse_head(data, req), data.size());
    return req;
}

} // namespace

BOOST_AUTO_TEST_SUITE(http_parser)

BOOST_AUTO_TEST_CASE(request_line_and_headers)
{
    constexpr auto data = "GET /index.html?a=1&b=2 HTTP/1.1\r\n"
                          "Host: example.com\r\n"
                          "user-agent:  \ttest/1.0 \t\r\n"
                          "X-Custom-ID:value\r\n"
                          "\r\n"sv;
    auto req = parse_head(data);
    BOOST_CHECK_EQUAL(req.method(), "GET");
    BOOST_CHECK_EQUAL(req.url(), "/index.html?a=1&b=2");
    BOOST_CHECK(req.state() == state::completed);
    BOOST_CHECK(req.keep_alive());
    BOOST_CHECK_EQUAL(req.size(), 3u);
    BOOST_CHECK_EQUAL(req["Host"], "example.com");
    BOOST_CHECK_EQUAL(req["User-Agent"], "test/1.0");
    BOOST_CHECK_EQUAL(req["X-Custom-Id"], "value");
    BOOST_CHECK_EQUAL(req.content_length(), dracon::chunked_data);
}

BOOST_AUTO_TEST_CASE(any_token_method)
{
    auto req = parse_head("PURGE /cache HTTP/1.1\r\n\r\n");
    BOOST_CHECK_EQUAL(req.method(), "PURGE");
    BOOST_CHECK_EQUAL(req.url(), "/cache");
}

BOOST_AUTO_TEST_CASE(bare_lf_and_leading_empty_lines)
{
    auto req = parse_head("\r\n\n\r\nGET / HTTP/1.1\nHost: x\n\n");
    BOOST_CHECK_EQUAL(req.method(), "GET");
    BOOST_CHECK_EQUAL(req["Host"], "x");
    BOOST_CHECK(req.state() == state::completed);
}

BOOST_AUTO_TEST_CASE(pipelined_requests)
{
    constexpr auto first = "GET /a HTTP/1.1\r\n\r\n"sv;
    constexpr auto second = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n"sv;
    const std::string data = std::string{first} + std::string{second};
    http_request_parser parser;
    request req1;
    BOOST_REQUIRE_EQUAL(parser.parse_head(data, req1), first.size());
    BOOST_CHECK_EQUAL(req1.url(), "/a");
    request req2;
    BOOST_REQUIRE_EQUAL(parser.parse_head(std::string_view{data}.substr(first.size()), req2), second.size());
    BOOST_CHECK_EQUAL(req2.url(), "/b");
}

BOOST_AUTO_TEST_CASE(keep_alive)
{
    BOOST_CHECK(parse_head("GET / HTTP/1.1\r\n\r\n").keep_alive());
    BOOST_CHECK(!parse_head("GET / HTTP/1.1\r\nConnection: close\r\n\r\n").keep_alive());
    BOOST_CHECK(!parse_head("GET / HTTP/1.1\r\nConnection: Upgrade, CLOSE\r\n\r\n").keep_alive());
    BOOST_CHECK(parse_head("GET / HTTP/1.1\r\nConnection: keep-alive\r\n\r\n").keep_alive());
    BOOST_CHECK(!parse_head("GET / HTTP/1.0\r\n\r\n").keep_alive());
    BOOST_CHECK(parse_head("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n").keep_alive());
    BOOST_CHECK(!parse_head("GET / HTTP/1.0\r\nConnection: keep-alive, close\r\n\r\n").keep_alive());
}

BOOST_AUTO_TEST_CASE(incremental_head)
{
    constexpr auto data = "POST /x HTTP/1.1\r\nHost: example.com\r\nContent-Length: 3\r\n\r\n"sv;

    // A prefix without a complete line yet consumes nothing.
    {
        request req;
        http_request_parser parser;
        BOOST_CHECK_EQUAL(parser.parse_head("POST /x HTT", req), 0u);
        BOOST_CHECK(req.method().empty());
    }
    // As soon as the request line is complete it is consumed and the method and
    // url are captured, with the state advanced to processing_header.
    {
        request req;
        http_request_parser parser;
        constexpr auto req_line = "POST /x HTTP/1.1\r\n"sv;
        BOOST_CHECK_EQUAL(parser.parse_head(req_line, req), req_line.size());
        BOOST_CHECK_EQUAL(req.method(), "POST");
        BOOST_CHECK_EQUAL(req.url(), "/x");
        BOOST_CHECK(req.state() == state::processing_header);
    }
    // Feeding one byte at a time (consume, then append the next byte) reaches the
    // same result as feeding the whole head at once.
    {
        request req;
        http_request_parser parser;
        std::string pending;
        for (char c : data) {
            pending += c;
            pending.erase(0, parser.parse_head(pending, req));
        }
        BOOST_CHECK_EQUAL(req.method(), "POST");
        BOOST_CHECK_EQUAL(req.url(), "/x");
        BOOST_CHECK_EQUAL(req["Host"], "example.com");
        BOOST_CHECK(req.state() == state::headers_completed);
        BOOST_CHECK_EQUAL(req.content_length(), 3u);
        BOOST_CHECK(pending.empty());
    }
    // Whole head in one call.
    {
        request req;
        http_request_parser parser;
        BOOST_CHECK_EQUAL(parser.parse_head(data, req), data.size());
        BOOST_CHECK(req.state() == state::headers_completed);
        BOOST_CHECK_EQUAL(req.content_length(), 3u);
    }
}

BOOST_AUTO_TEST_CASE(repeated_headers)
{
    auto req = parse_head("GET / HTTP/1.1\r\nAccept: a\r\naccept: b\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n");
    BOOST_CHECK_EQUAL(req["Accept"], "a, b");
    BOOST_CHECK_EQUAL(req["Content-Length"], "0");
    BOOST_CHECK_EQUAL(head_status("GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"), 400);
}

BOOST_AUTO_TEST_CASE(invalid_heads)
{
    const std::vector<std::pair<std::string_view, uint16_t>> cases{
        {"GET / HTTP/2.0\r\n\r\n", 505},
        {"GET / HTTP/0.9\r\n\r\n", 505},
        {"GET /\r\n\r\n", 400},
        {"GET / HTTP/1.1 \r\n\r\n", 400},
        {"GET / HTTP/1.x\r\n\r\n", 400},
        {"GET / http/1.1\r\n\r\n", 400},
        {"GET  / HTTP/1.1\r\n\r\n", 400},
        {" GET / HTTP/1.1\r\n\r\n", 400},
        {"GET /a b HTTP/1.1\r\n\r\n", 400},
        {"GET /a\x7f HTTP/1.1\r\n\r\n", 400},
        {"G\x01T / HTTP/1.1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost example\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n: x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a\x01z\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a\rz\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nContent-Length:\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nContent-Length: 12a\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nContent-Length: +1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nContent-Length: 0x10\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
    };
    for (const auto &[data, status] : cases)
        BOOST_CHECK_MESSAGE(head_status(data) == status, data);
}

BOOST_AUTO_TEST_CASE(head_too_large)
{
    std::string head = "GET / HTTP/1.1\r\nX-Long: ";
    head.append(http_request_parser::max_head_size, 'a');
    BOOST_CHECK_EQUAL(head_status(head), 431);                   // no terminator yet
    BOOST_CHECK_EQUAL(head_status(head + "\r\n\r\n"), 431);      // complete, but too big

    std::string many = "GET / HTTP/1.1\r\n";
    for (size_t i = 0; i < http_request_parser::max_headers_count; ++i)
        many += "A: b\r\n";
    BOOST_CHECK_EQUAL(head_status(many + "\r\n"), 0);
    BOOST_CHECK_EQUAL(head_status(many + "A: b\r\n\r\n"), 431);
}

BOOST_AUTO_TEST_CASE(content_length_body)
{
    parser_driver driver;
    BOOST_CHECK(!driver.feed("POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello"));
    BOOST_CHECK(driver.req.state() == state::processing_body);
    BOOST_CHECK(driver.feed(" world"));
    BOOST_CHECK_EQUAL(driver.body, "hello world");
    BOOST_CHECK(driver.pending.empty());

    parser_driver pipelined;
    BOOST_CHECK(pipelined.feed("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhelloGET /next HTTP/1.1\r\n\r\n"));
    BOOST_CHECK_EQUAL(pipelined.body, "hello");
    BOOST_CHECK_EQUAL(pipelined.pending, "GET /next HTTP/1.1\r\n\r\n");

    parser_driver empty;
    BOOST_CHECK(empty.feed("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n"));
    BOOST_CHECK(empty.body.empty());
}

BOOST_AUTO_TEST_CASE(chunked_body)
{
    constexpr auto head = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"sv;
    constexpr auto body = "4\r\nWiki\r\n5;ext=\"1\"\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nTrailer: x\r\n\r\n"sv;
    constexpr auto expected = "Wikipedia in\r\n\r\nchunks."sv;
    const std::string data = std::string{head} + std::string{body};

    {
        parser_driver driver;
        BOOST_CHECK(driver.feed(data));
        BOOST_CHECK_EQUAL(driver.body, expected);
        BOOST_CHECK(driver.pending.empty());
    }
    {
        // byte by byte
        parser_driver driver;
        for (size_t i = 0; i < data.size(); ++i)
            BOOST_CHECK_MESSAGE(driver.feed(std::string_view{data}.substr(i, 1)) == (i + 1 == data.size()), "byte " << i);
        BOOST_CHECK_EQUAL(driver.body, expected);
    }
    for (size_t split = 1; split < data.size(); ++split) {
        parser_driver driver;
        BOOST_CHECK_MESSAGE(!driver.feed(std::string_view{data}.substr(0, split)), "split " << split);
        BOOST_CHECK_MESSAGE(driver.feed(std::string_view{data}.substr(split)), "split " << split);
        BOOST_CHECK_MESSAGE(driver.body == expected, "split " << split);
    }
    {
        // bare LF, uppercase hex, pipelined request after the last chunk
        parser_driver driver;
        BOOST_CHECK(driver.feed("POST / HTTP/1.1\nTransfer-Encoding: chunked\n\nA\n0123456789\n0\n\nGET / HTTP/1.1\r\n\r\n"));
        BOOST_CHECK_EQUAL(driver.body, "0123456789");
        BOOST_CHECK_EQUAL(driver.pending, "GET / HTTP/1.1\r\n\r\n");
    }
}

BOOST_AUTO_TEST_CASE(invalid_chunks)
{
    constexpr auto head = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"sv;
    const std::vector<std::pair<std::string_view, uint16_t>> cases{
        {"zz\r\n", 400},
        {"\r\n", 400},
        {"5 x\r\n", 400},
        {"11111111111111111\r\n", 400},
        {"5\r\nhelloXX", 400},
        {"5\r\nhello\rX", 400},
        {std::string_view{"5\r\nhel\0o\r\n", 10}, 0},
    };
    for (const auto &[body, status] : cases) {
        const auto data = std::string{head} + std::string{body};
        BOOST_CHECK_MESSAGE(status_of([&] { parser_driver{}.feed(data); }) == status, body);
    }
    std::string long_line(http_request_parser::max_chunk_line_size + 1, '1');
    BOOST_CHECK_EQUAL(status_of([&] { parser_driver{}.feed(std::string{head} + long_line); }), 400);
}

BOOST_AUTO_TEST_CASE(max_body_size)
{
    parser_driver driver;
    driver.max_body = 4;
    BOOST_CHECK_EQUAL(status_of([&] { driver.feed("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello"); }), 413);

    parser_driver chunked;
    chunked.max_body = 4;
    BOOST_CHECK_EQUAL(status_of([&] { chunked.feed("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n"); }), 413);
    BOOST_CHECK_EQUAL(chunked.body, "abc");

    // no body callback installed
    BOOST_CHECK_EQUAL(status_of([] {
        request req;
        http_request_parser parser;
        constexpr auto data = "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello"sv;
        auto consumed = parser.parse_head(data, req);
        parser.parse_body(data.substr(consumed), req);
    }), 400);
}

BOOST_AUTO_TEST_CASE(slow_steaming)
{
    request req;
    http_request_parser parser;
    constexpr auto data = "GET /index.html?a=1&b=2 HTTP/1.1\r\n"
                          "Host: example.com\r\n"
                          "user-agent:  \ttest/1.0 \t\r\n"
                          "X-Custom-ID:value\r\n"
                          "\r\n"sv;
    // parse_head returns how many bytes it consumed, so the caller advances its
    // offset by that amount (pos += ...). Each iteration reveals one more byte,
    // mimicking a slow stream that delivers the head a little at a time.
    size_t pos = 0;
    for (size_t i = 0; i < data.size(); ++i)
        pos += parser.parse_head(data.substr(pos, i), req);

    BOOST_CHECK_EQUAL(req.method(), "GET");
    BOOST_CHECK_EQUAL(req.url(), "/index.html?a=1&b=2");
    BOOST_CHECK(req.state() == state::completed);
    BOOST_CHECK(req.keep_alive());
    BOOST_CHECK_EQUAL(req.size(), 3u);
    BOOST_CHECK_EQUAL(req["Host"], "example.com");
    BOOST_CHECK_EQUAL(req["User-Agent"], "test/1.0");
    BOOST_CHECK_EQUAL(req["X-Custom-Id"], "value");
    BOOST_CHECK_EQUAL(req.content_length(), dracon::chunked_data);
}

BOOST_AUTO_TEST_SUITE_END()
