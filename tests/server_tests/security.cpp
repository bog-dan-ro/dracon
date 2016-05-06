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

// Robustness / security tests driven over a raw socket, so we can send the
// malformed and hostile requests that libcurl refuses to produce. They target
// the request parser and the connection lifecycle. All of them talk to the
// plaintext port; the parser is shared with the TLS path.

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <string>

#include <raw_client.h>

using dracon::test::raw_client;
using namespace std::chrono_literals;

namespace {

// Sends one request, returns the first response's status code.
std::string request_status(const std::string &request)
{
    raw_client c;
    BOOST_CHECK(c.send(request));
    return raw_client::status(c.recv_all());
}

BOOST_AUTO_TEST_SUITE(security)

BOOST_AUTO_TEST_CASE(valid_baseline)
{
    BOOST_CHECK_EQUAL(request_status("GET /test0 HTTP/1.1\r\nHost: x\r\n\r\n"), "200");
}

BOOST_AUTO_TEST_CASE(malformed_request_line)
{
    BOOST_CHECK_EQUAL(request_status("GET /test0\r\n\r\n"), "400");           // no HTTP version
    BOOST_CHECK_EQUAL(request_status("GET  /test0 HTTP/1.1\r\n\r\n"), "400"); // empty method/target
    BOOST_CHECK_EQUAL(request_status("/test0 HTTP/1.1\r\n\r\n"), "400");      // no method
    BOOST_CHECK_EQUAL(request_status("GET /te st0 HTTP/1.1\r\n\r\n"), "400"); // space in target
}

BOOST_AUTO_TEST_CASE(unsupported_or_invalid_version)
{
    BOOST_CHECK_EQUAL(request_status("GET /test0 HTTP/2.0\r\n\r\n"), "505");
    BOOST_CHECK_EQUAL(request_status("GET /test0 HTTP/0.9\r\n\r\n"), "505");
    BOOST_CHECK_EQUAL(request_status("GET /test0 HTTP/1.x\r\n\r\n"), "400");
}

BOOST_AUTO_TEST_CASE(invalid_control_chars_in_header)
{
    BOOST_CHECK_EQUAL(request_status("GET /test0 HTTP/1.1\r\nHost : x\r\n\r\n"), "400"); // space before colon
    BOOST_CHECK_EQUAL(request_status("GET /test0 HTTP/1.1\r\n: x\r\n\r\n"), "400");      // empty field name
    BOOST_CHECK_EQUAL(request_status("GET /test0 HTTP/1.1\r\nX: a\r\n b\r\n\r\n"), "400"); // obsolete line folding
}

BOOST_AUTO_TEST_CASE(oversized_head_is_rejected)
{
    // A single header value larger than the head-size limit -> 431
    std::string req = "GET /test0 HTTP/1.1\r\nX-Long: ";
    req.append(9000, 'a');
    req += "\r\n\r\n";
    BOOST_CHECK_EQUAL(request_status(req), "431");
}

BOOST_AUTO_TEST_CASE(too_many_headers_is_rejected)
{
    std::string req = "GET /test0 HTTP/1.1\r\n";
    for (int i = 0; i < 200; ++i)
        req += "X-H: v\r\n";
    req += "\r\n";
    BOOST_CHECK_EQUAL(request_status(req), "431");
}

BOOST_AUTO_TEST_CASE(leading_blank_line_flood_is_bounded)
{
    // Regression: an endless stream of blank lines must not be buffered without
    // limit while the server waits for a request line.
    raw_client c;
    std::string crlf;
    crlf.reserve(9000);
    while (crlf.size() < 9000)
        crlf += "\r\n";
    c.send(crlf);
    BOOST_CHECK_EQUAL(raw_client::status(c.recv_all()), "431");
}

BOOST_AUTO_TEST_CASE(content_length_transfer_encoding_conflict)
{
    // Classic request-smuggling vector: both framing headers present -> reject.
    BOOST_CHECK_EQUAL(request_status("POST /test0 HTTP/1.1\r\nContent-Length: 0\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n"), "400");
}

BOOST_AUTO_TEST_CASE(conflicting_duplicate_content_length)
{
    BOOST_CHECK_EQUAL(request_status("POST /test0 HTTP/1.1\r\nContent-Length: 1\r\n"
                            "Content-Length: 2\r\n\r\n"), "400");
}

BOOST_AUTO_TEST_CASE(invalid_content_length)
{
    BOOST_CHECK_EQUAL(request_status("POST /test0 HTTP/1.1\r\nContent-Length: -1\r\n\r\n"), "400");
    BOOST_CHECK_EQUAL(request_status("POST /test0 HTTP/1.1\r\nContent-Length: 0x10\r\n\r\n"), "400");
    BOOST_CHECK_EQUAL(request_status("POST /test0 HTTP/1.1\r\nContent-Length: 9999999999999999999999\r\n\r\n"), "400");
}

BOOST_AUTO_TEST_CASE(transfer_encoding_must_end_with_chunked)
{
    BOOST_CHECK_EQUAL(request_status("POST /test0 HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n"), "400");
    BOOST_CHECK_EQUAL(request_status("POST /test0 HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"), "501");
    BOOST_CHECK_EQUAL(request_status("POST /test0 HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n"), "501");
}

BOOST_AUTO_TEST_CASE(malformed_chunk_size)
{
    BOOST_CHECK_EQUAL(request_status("POST /echoTest HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"), "400");
    BOOST_CHECK_EQUAL(request_status("POST /echoTest HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                            "10000000000000000\r\n"), "400"); // > 16 hex digits
}

BOOST_AUTO_TEST_CASE(well_formed_chunked_request_is_consumed)
{
    // The test plugin has no endpoint that echoes a chunked request body (they
    // all require Content-Length), so byte-exact chunked decoding is checked by
    // the parser unit tests. Here we only verify the server decodes the chunked
    // framing and answers promptly instead of stalling waiting for more body:
    // if chunk parsing were wrong, recv_all would time out with an empty result.
    raw_client c;
    BOOST_REQUIRE(c.send("POST /echoTest HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
    const auto resp = c.recv_all();
    BOOST_CHECK_MESSAGE(raw_client::status(resp).size() == 3u, "expected a prompt HTTP response, got: " << resp);
}

BOOST_AUTO_TEST_CASE(pipelined_requests_get_separate_responses)
{
    raw_client c;
    BOOST_REQUIRE(c.send("GET /test0 HTTP/1.1\r\nHost: x\r\n\r\n"
                       "GET /test0 HTTP/1.1\r\nHost: x\r\n\r\n"));
    auto resp = c.recv_responses(2);
    BOOST_CHECK_EQUAL(raw_client::response_count(resp), 2u);
    BOOST_CHECK_EQUAL(raw_client::status(resp), "200");
}

BOOST_AUTO_TEST_CASE(keep_alive_connection_reuse)
{
    raw_client c;
    for (int i = 0; i < 5; ++i) {
        BOOST_REQUIRE_MESSAGE(c.send("GET /test0 HTTP/1.1\r\nHost: x\r\n\r\n"), "request " << i);
        auto resp = c.recv_responses(1);
        BOOST_CHECK_MESSAGE(raw_client::status(resp) == "200", "request " << i);
    }
}

BOOST_AUTO_TEST_CASE(slowloris_partial_head_times_out)
{
    // A client that opens a connection and never finishes the head must be
    // dropped by the headers timeout, not held forever.
    raw_client c;
    BOOST_REQUIRE(c.send("GET /test0 HTTP/1.1\r\nHost: x\r\n")); // no terminating CRLF
    const auto start = std::chrono::steady_clock::now();
    auto resp = c.recv_all(); // blocks until the server closes (or read timeout)
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // The server's default headers timeout is 5s; make sure it actually closed.
    BOOST_CHECK_LT(elapsed, 20s);
    BOOST_CHECK(resp.empty() || raw_client::status(resp) == "408");
}

BOOST_AUTO_TEST_CASE(byte_at_a_time_head_is_parsed)
{
    // Feed a valid request one byte at a time: exercises incremental head parsing.
    raw_client c;
    BOOST_REQUIRE(c.send_slow("GET /test0 HTTP/1.1\r\nHost: x\r\n\r\n", 1, 2ms));
    BOOST_CHECK_EQUAL(raw_client::status(c.recv_all()), "200");
}


BOOST_AUTO_TEST_SUITE_END()
} // namespace
