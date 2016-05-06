/*
    Copyright (C) 2022 by BogDan Vatra <bogdan@kde.org>

    Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <boost/test/unit_test.hpp>
#include <easy_curl.h>

namespace {
using namespace std;

static std::string url{"http://localhost:8080/v1"};

BOOST_AUTO_TEST_SUITE(devices_test)

BOOST_AUTO_TEST_CASE(no_devices)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url + "/devices"));
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body, "[]");
}

BOOST_AUTO_TEST_CASE(post_devices)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url + "/devices"));
    auto reply = curl.post(R"(["dev1","dev2", "dev3"])");
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body, "");
}

BOOST_AUTO_TEST_CASE(get_devices)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url + "/devices"));
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body, R"([{"id":0,"name":"dev1"},{"id":1,"name":"dev2"},{"id":2,"name":"dev3"}])");
}

BOOST_AUTO_TEST_CASE(get_device)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url + "/devices/1"));
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body, R"([{"id":1,"name":"dev2"}])");
}

BOOST_AUTO_TEST_CASE(delete_device)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url + "/devices/1"));
    auto reply = curl.del();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body, "");
}

BOOST_AUTO_TEST_CASE(get_devices_after_delete)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url + "/devices"));
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body, R"([{"id":0,"name":"dev1"},{"id":1,"name":"dev3"}])");
}

BOOST_AUTO_TEST_CASE(delete_all_devices)
{
    dracon::test::easy_curl curl;
    BOOST_CHECK_NO_THROW(curl.set_url(url + "/devices/0"));
    auto reply = curl.del();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body, "");
    reply = curl.del();
    BOOST_CHECK_EQUAL(reply.status, "200");
    BOOST_CHECK_EQUAL(reply.body, "");
    reply = curl.del();
    BOOST_CHECK_EQUAL(reply.status, "404"); // there is no device 0 anymore
    BOOST_CHECK_EQUAL(reply.body, "");
}

BOOST_AUTO_TEST_CASE(get_invalid_device)
{
    dracon::test::easy_curl curl;
    // a device id which isn't a number is a bad request, not a server error
    BOOST_CHECK_NO_THROW(curl.set_url(url + "/devices/bla"));
    auto reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "400");
    // ... and one which is simply not there is a 404
    BOOST_CHECK_NO_THROW(curl.set_url(url + "/devices/0"));
    reply = curl.get();
    BOOST_CHECK_EQUAL(reply.status, "404");
}



BOOST_AUTO_TEST_SUITE_END()
} // namespace {
