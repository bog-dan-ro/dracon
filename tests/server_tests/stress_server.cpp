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

#include <boost/test/unit_test.hpp>
#include <easy_curl.h>
#include <boost/algorithm/string/predicate.hpp>
#include "utils.h"

namespace {
using namespace std;

void parse_echo_data(const std::string &data, size_t &content_length, std::unordered_map<string, string> &headers, std::string &body)
{
    content_length = 0;
    size_t pos = 0;
    while (pos < data.size()) {
        auto p = data.find('\n', pos);
        if (p == std::string::npos)
            break;
        auto line = data.substr(pos, p - pos);
        if (boost::starts_with(line, "~~~~ ")) {
            if (boost::starts_with(line.c_str() + 5, "ContentLength: ")) {
                content_length = std::strtoul(line.c_str() + 20, nullptr, 10);
            } else if (boost::starts_with(line.c_str() + 5, "Body:")) {
                body = data.substr(p + 1);
                break;
            }
        } else {
            auto sep_pos = line.find(" : ");
            headers[line.substr(0, sep_pos)] = line.substr(sep_pos + 3);
        }
        pos = p + 1;
    }
}

const std::string test_body_data = R"(/*
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
 */)";
BOOST_AUTO_TEST_SUITE(stress)

BOOST_AUTO_TEST_CASE(server)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url("http://localhost:8080/echoTest"));
    BOOST_CHECK_NO_THROW(curl.set_headers({
                                        {"Super__________________long_______________field",
                                         "with___________super________log____---------value"}
                                    }));
    auto reply = curl.post(test_body_data);
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.headers["Connection"], "keep-alive");
    BOOST_CHECK_EQUAL(reply.headers["Keep-Alive"], "timeout=10");
    BOOST_CHECK_EQUAL(reply.headers["Transfer-Encoding"], "chunked");
    size_t content_length;
    std::unordered_map<string, string> headers;
    std::string body;
    BOOST_CHECK_NO_THROW(parse_echo_data(reply.body, content_length, headers, body));
    BOOST_CHECK_EQUAL(content_length, test_body_data.size());
    BOOST_CHECK_EQUAL(headers["Super__________________long_______________field"],
            "with___________super________log____---------value");
    BOOST_CHECK_EQUAL(body, test_body_data);
}


BOOST_AUTO_TEST_SUITE_END()
} // namespace
