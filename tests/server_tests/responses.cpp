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

#include <boost/test/data/monomorphic.hpp>
#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>
#include <easy_curl.h>

#include "utils.h"

std::string huge_data;

namespace {
using namespace std;

namespace data = boost::unit_test::data;
// Every case runs once per protocol. String literals, so there is no
// static initialisation order to worry about at registration time.
const char *const protocols[] = {"http", "https"};

BOOST_AUTO_TEST_SUITE(responses)

BOOST_DATA_TEST_CASE(zero, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test0")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.headers["Connection"], "keep-alive");
    BOOST_CHECK_EQUAL(reply.headers["Content-Length"], "0");
    BOOST_CHECK_EQUAL(reply.headers["Keep-Alive"], "timeout=10");
    BOOST_CHECK_EQUAL(reply.body.empty(), true);
}

BOOST_DATA_TEST_CASE(test100, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test100")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.headers["Connection"], "keep-alive");
    BOOST_CHECK_EQUAL(reply.headers["Content-Length"], "100");
    BOOST_CHECK_EQUAL(reply.headers["Keep-Alive"], "timeout=10");
    BOOST_CHECK_EQUAL(reply.body, "100XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
}

BOOST_DATA_TEST_CASE(test50m, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test50m")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.headers["Connection"], "keep-alive");
    BOOST_CHECK_EQUAL(reply.headers["Content-Length"], "52428800");
    BOOST_CHECK_EQUAL(reply.headers["Keep-Alive"], "timeout=10");
    BOOST_CHECK_EQUAL(reply.body, huge_data);
}

BOOST_DATA_TEST_CASE(test50m_iovec, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test50ms")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.headers["Connection"], "keep-alive");
    BOOST_CHECK_EQUAL(reply.headers["Content-Length"], "52428800");
    BOOST_CHECK_EQUAL(reply.headers["Keep-Alive"], "timeout=10");
    BOOST_CHECK_EQUAL(reply.body, huge_data);
}

BOOST_DATA_TEST_CASE(test50m_chunked, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test50mChunked")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.headers["Connection"], "keep-alive");
    BOOST_CHECK_EQUAL(reply.headers["Transfer-Encoding"], "chunked");
    BOOST_CHECK_EQUAL(reply.headers["Keep-Alive"], "timeout=10");
    BOOST_CHECK_EQUAL(reply.body.size(), huge_data.size());
}

BOOST_DATA_TEST_CASE(test_worker, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/testWorker")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.headers["Connection"], "keep-alive");
    BOOST_CHECK_EQUAL(reply.headers["Transfer-Encoding"], "chunked");
    BOOST_CHECK_EQUAL(reply.headers["Keep-Alive"], "timeout=10");
    BOOST_CHECK_EQUAL(reply.body.size() > 100000, true);
    for (int i = 0 ; i < 100000; ++i)
        BOOST_CHECK_EQUAL(reply.body[i],  '0' + i % 10);
}

void test_post_ppp(const std::string &url, const std::string &data, const std::string &status)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.request("PATCH", data);
    BOOST_CHECK_EQUAL(reply.status, status);
    BOOST_CHECK_EQUAL(reply.body.size(), data.size());
    BOOST_CHECK_EQUAL(huge_data, reply.body);
}

BOOST_DATA_TEST_CASE(test_ppp, data::make(protocols), protocol)
{
    test_post_ppp(url(protocol, "/testPPP"), huge_data, "200");
}


BOOST_DATA_TEST_CASE(restful_captures_as_arguments, data::make(protocols), protocol)
{
    // One handler serves all four customer routes and takes the captures as
    // arguments; the routes which capture less hand over empty strings
    dracon::test::easy_curl curl;
    curl.ingnore_invalid_ssl_certificate();
    const auto get = [&curl, protocol](const std::string &path) {
        BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, path)));
        const auto reply = curl.get();
        BOOST_CHECK_EQUAL(reply.status, "200");
        return reply.body;
    };
    BOOST_CHECK_EQUAL(get("/test/rest/v1/customers"), "customer_id: \nlicense_id: \n");
    BOOST_CHECK_EQUAL(get("/test/rest/v1/customers/42"), "customer_id: 42\nlicense_id: \n");
    BOOST_CHECK_EQUAL(get("/test/rest/v1/customers/42/licenses"), "customer_id: 42\nlicense_id: \n");
    BOOST_CHECK_EQUAL(get("/test/rest/v1/customers/42/licenses/7"), "customer_id: 42\nlicense_id: 7\n");
}

BOOST_DATA_TEST_CASE(restful_optional_captures, data::make(protocols), protocol)
{
    // The same shape with std::optional captures: a route which doesn't
    // declare a capture hands over a nullopt, not an empty string
    dracon::test::easy_curl curl;
    curl.ingnore_invalid_ssl_certificate();
    const auto get = [&curl, protocol](const std::string &path) {
        BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, path)));
        const auto reply = curl.get();
        BOOST_CHECK_EQUAL(reply.status, "200");
        return reply.body;
    };
    BOOST_CHECK_EQUAL(get("/test/rest/v1/orders"), "order_id: <none>\nitem_id: <none>\n");
    BOOST_CHECK_EQUAL(get("/test/rest/v1/orders/42"), "order_id: 42\nitem_id: <none>\n");
    BOOST_CHECK_EQUAL(get("/test/rest/v1/orders/42/items/7"), "order_id: 42\nitem_id: 7\n");
}

BOOST_DATA_TEST_CASE(restful_parsed_route, data::make(protocols), protocol)
{
    // The same routes, reached through the parsed_route form, which also sees
    // the query strings and the methods the route allows
    dracon::test::easy_curl curl;
    curl.ingnore_invalid_ssl_certificate();
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test/rest/v1/customers/42/licenses/7?a=b&c=d")));
    const auto reply = curl.del();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body,
                      "Got 2 captured resources\n"
                      "and 2 queries\n"
                      "All methods but OPTIONS GET, DELETE \n"
                      "Resource: 42\n"
                      "Resource: 7\n"
                      "Query name: a  value: b\n"
                      "Query name: c  value: d\n");
}

BOOST_DATA_TEST_CASE(restful_unknown_method_is_rejected, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    curl.ingnore_invalid_ssl_certificate();
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test/rest/v1/customers/42")));
    auto reply = curl.put("nope");
    BOOST_CHECK_EQUAL(reply.status, "405");
    BOOST_CHECK_EQUAL(reply.headers["Allow"], "GET, DELETE");
}

BOOST_DATA_TEST_CASE(restful_handler_without_a_request, data::make(protocols), protocol)
{
    // The OPTIONS handler takes (abstract_stream &, const parsed_route &): no
    // request &, which is optional in every shape
    dracon::test::easy_curl curl;
    curl.ingnore_invalid_ssl_certificate();
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test/rest/v1/customers/42")));
    auto reply = curl.opt();
    BOOST_CHECK_EQUAL(reply.status, "204");
    BOOST_CHECK_EQUAL(reply.headers["Allow"], "OPTIONS, GET, DELETE");
    BOOST_CHECK(reply.body.empty());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace {
