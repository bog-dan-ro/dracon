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

#include <chrono>
#include <boost/test/data/monomorphic.hpp>
#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>
#include <easy_curl.h>

#include "utils.h"

namespace {
using namespace std;

namespace data = boost::unit_test::data;
// Every case runs once per protocol. String literals, so there is no
// static initialisation order to worry about at registration time.
const char *const protocols[] = {"http", "https"};

BOOST_AUTO_TEST_SUITE(response_status_error)

BOOST_DATA_TEST_CASE(secure_only, data::make(protocols), protocol)
{
    using clock = std::chrono::high_resolution_clock;
    auto start = clock::now();
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test100")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");

    BOOST_CHECK_NO_THROW(curl.set_url("http://localhost:8080/secureOnly"));
    reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "403");
    BOOST_CHECK_EQUAL(reply.headers["ErrorKey1"], "Value1");
    BOOST_CHECK_EQUAL(reply.headers["ErrorKey2"], "Value2");
    BOOST_CHECK_EQUAL(reply.body, "Only secured connections allowed");

    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test100")));
    curl.ingnore_invalid_ssl_certificate();
    reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_LE(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count(),  duration(protocol, 2000));
}

BOOST_DATA_TEST_CASE(from_request_complete, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/testThowFromRequestComplete")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "412");
    BOOST_CHECK_EQUAL(reply.body.size(), 0);
}

BOOST_DATA_TEST_CASE(expectations100, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/testExpectation")));
    curl.ingnore_invalid_ssl_certificate();
    curl.set_headers({
                        {"Expect", "100-continue"},
                        {"X-Continue", "100"}
                    });
    auto reply = curl.post("some data");
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body.size(), 0);
}

BOOST_DATA_TEST_CASE(expectations417, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/testExpectation")));
    curl.ingnore_invalid_ssl_certificate();
    curl.set_headers({{"Expect", "100-continue"}});
    auto reply = curl.post("some data");
    BOOST_CHECK_EQUAL(reply.status, "417");
    BOOST_CHECK_EQUAL(reply.body.size(), 0);
}


BOOST_DATA_TEST_CASE(from_body, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/testThowFromBody")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.post("some data");
    BOOST_CHECK_EQUAL(reply.status, "400");
    BOOST_CHECK_EQUAL(reply.headers["BodyKey1"], "Value1");
    BOOST_CHECK_EQUAL(reply.headers["BodyKey2"], "Value2");
    BOOST_CHECK_EQUAL(reply.body, "Body too big, lose some weight");
}

BOOST_DATA_TEST_CASE(from_write_response, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/testThowFromWriteResponse")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "409");
    BOOST_CHECK_EQUAL(reply.headers["WriteRes1"], "Value1");
    BOOST_CHECK_EQUAL(reply.headers["WriteRes2"], "Value2");
    BOOST_CHECK_EQUAL(reply.body, "Throw from WriteResponse");
}

BOOST_DATA_TEST_CASE(from_write_response_std, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    using clock = std::chrono::high_resolution_clock;
    auto start = clock::now();
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test100")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");

    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/testThowFromWriteResponseStd")));
    curl.ingnore_invalid_ssl_certificate();
    reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "500");
    BOOST_CHECK_EQUAL(reply.body, "Throw from WriteResponseStd");

    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test100")));
    curl.ingnore_invalid_ssl_certificate();
    reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_LE(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count(),  duration(protocol, 2000));
}

BOOST_DATA_TEST_CASE(from_write_response_after_write, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    using clock = std::chrono::high_resolution_clock;
    auto start = clock::now();
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test100")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");

    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/testThowFromWriteResponseAfterWrite")));
    curl.ingnore_invalid_ssl_certificate();
    BOOST_CHECK_THROW(curl.get(), std::runtime_error);

    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test100")));
    curl.ingnore_invalid_ssl_certificate();
    reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_LE(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count(), duration(protocol, 2000));
}

BOOST_DATA_TEST_CASE(after_wakeup, data::make(protocols), protocol)
{
    dracon::test::easy_curl curl;
    using clock = std::chrono::high_resolution_clock;
    auto start = clock::now();
    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test100")));
    curl.ingnore_invalid_ssl_certificate();
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");

    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/testThrowAfterWakeup")));
    curl.ingnore_invalid_ssl_certificate();
    reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "404");

    BOOST_CHECK_NO_THROW(curl.set_url(url(protocol, "/test100")));
    curl.ingnore_invalid_ssl_certificate();
    reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_LE(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count(), duration(protocol, 2000));
}


BOOST_AUTO_TEST_SUITE_END()
} // namespace {
