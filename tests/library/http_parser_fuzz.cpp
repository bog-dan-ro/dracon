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

// Deterministic in-process fuzzer for the request parser. It asserts the two
// safety invariants that matter under hostile input:
//   * the parser only ever fails by throwing a dracon::response (a status
//     code), never a crash, an unbounded loop, or any other exception type;
//   * it never reports consuming more bytes than it was given.
// Build it with -fsanitize=address,undefined to also catch memory errors.

#include <boost/test/unit_test.hpp>

#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <http_parser.h>

using dracon::internal::http_request_parser;
using dracon::request;
using state = dracon::request::parse_state;

namespace {

// Feeds a whole message (head + body) through the parser exactly once and
// checks the invariants. Returns without asserting on malformed input.
void feed_once(std::string_view data)
{
    request req;
    http_request_parser parser;
    std::string body;
    req.append_body_callback([&](std::string_view b) { body.append(b); }, 1u << 20);
    try {
        // parse_head consumes every complete line it has in one call.
        size_t off = parser.parse_head(data, req);
        BOOST_REQUIRE_LE(off, data.size());
        if (req.state() != state::headers_completed && req.state() != state::completed)
            return; // incomplete head
        // Drive the body to completion or until no progress is made.
        while (req.state() != state::completed && off <= data.size()) {
            const auto n = parser.parse_body(data.substr(off), req);
            BOOST_REQUIRE_LE(off + n, data.size());
            if (n == 0)
                break; // needs more data
            off += n;
        }
    } catch (const dracon::response &) {
        // Expected way to reject malformed input.
    }
}

// A byte alphabet biased towards HTTP-significant characters so the fuzzer
// spends its time near interesting boundaries rather than in random noise.
constexpr char alphabet[] = {
    '\r', '\n', ' ', '\t', ':', ';', ',', '/', '.', '%', '+', '-', '=', '?',
    '0', '1', '2', '9', 'a', 'f', 'x', 'A', 'F', 'G', 'E', 'T', 'P', 'O', 'S',
    'H', '{', '}', '\0', '\x7f', char(0x80), char(0xff)
};

std::string random_bytes(std::mt19937_64 &rng, size_t max_len)
{
    std::uniform_int_distribution<size_t> len_dist(0, max_len);
    std::uniform_int_distribution<int> pick(0, int(sizeof(alphabet)) - 1);
    std::uniform_int_distribution<int> raw(0, 255);
    std::bernoulli_distribution use_alphabet(0.85);
    const size_t len = len_dist(rng);
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i)
        out += use_alphabet(rng) ? alphabet[pick(rng)] : char(raw(rng));
    return out;
}

BOOST_AUTO_TEST_SUITE(http_parser_fuzz)

BOOST_AUTO_TEST_CASE(random_bytes_never_crash)
{
    std::mt19937_64 rng(0xC0FFEE);
    for (int i = 0; i < 200000; ++i)
        feed_once(random_bytes(rng, 128));
}

BOOST_AUTO_TEST_CASE(mutated_valid_requests)
{
    const std::vector<std::string> seeds = {
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n",
        "POST /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
        "POST /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        "GET /a?q=1&r=2 HTTP/1.1\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n",
    };
    std::mt19937_64 rng(0x1234ABCD);
    std::uniform_int_distribution<int> op(0, 2);
    for (int i = 0; i < 200000; ++i) {
        std::string s = seeds[rng() % seeds.size()];
        const int mutations = 1 + int(rng() % 4);
        for (int m = 0; m < mutations && !s.empty(); ++m) {
            const size_t pos = rng() % s.size();
            switch (op(rng)) {
            case 0: s[pos] = char(rng() & 0xff); break;              // flip
            case 1: s.insert(pos, 1, alphabet[rng() % sizeof(alphabet)]); break; // insert
            case 2: s.erase(pos, 1); break;                          // delete
            }
        }
        feed_once(s);
    }
}

BOOST_AUTO_TEST_CASE(split_feeding_matches_whole_feeding)
{
    // For structured chunked/CL messages, feeding at every split point must
    // produce the same delivered body as feeding the whole buffer at once.
    struct sample { std::string data; std::string body; };
    const std::vector<sample> cases = {
        {"POST /a HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world", "hello world"},
        {"POST /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
         "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", "Wikipedia"},
    };
    for (const auto &c : cases) {
        for (size_t split = 0; split <= c.data.size(); ++split) {
            http_request_parser parser;
            request req;
            std::string body;
            req.append_body_callback([&](std::string_view b) { body.append(b); }, 1u << 20);
            std::string pending{c.data.substr(0, split)};
            bool head_done = false;
            auto pump = [&](std::string_view more) {
                pending.append(more);
                if (!head_done) {
                    pending.erase(0, parser.parse_head(pending, req));
                    head_done = req.state() == state::headers_completed || req.state() == state::completed;
                }
                if (head_done && req.state() != state::completed)
                    pending.erase(0, parser.parse_body(pending, req));
            };
            pump({});
            pump(c.data.substr(split));
            BOOST_REQUIRE_MESSAGE(req.state() == state::completed, "split " << split);
            BOOST_REQUIRE_MESSAGE(body == c.body, "split " << split);
        }
    }
}


BOOST_AUTO_TEST_SUITE_END()
} // namespace
