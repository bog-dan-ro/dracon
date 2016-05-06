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
#include <dracon/restful.h>
#include <memory>
#include <optional>
#include <ostream>
#include <span>

// The routes return std::optional<int>, teach Boost.test how to print one so a
// failed comparison says what it got instead of failing to compile
namespace boost::test_tools::tt_detail {
template <typename T>
struct print_log_value<std::optional<T>>
{
    void operator()(std::ostream &os, const std::optional<T> &value)
    {
        if (value)
            os << *value;
        else
            os << "nullopt";
    }
};
} // namespace boost::test_tools::tt_detail

namespace {
    using namespace std;
    using namespace dracon;

    class test_router : public restful_router<std::optional<int>, int>
    {
    public:
        test_router(const std::string &base_url = {})
            : restful_router(base_url)
        {}

        const auto &base_url() const { return m_base_url; }
        const auto &routes() const { return m_routes; }
    };

    BOOST_AUTO_TEST_SUITE(restful_routing)

    BOOST_AUTO_TEST_CASE(router_construction)
    {
        {
            test_router test_no_base{};
            BOOST_CHECK_EQUAL(test_no_base.routes().size(), 0);
            BOOST_CHECK_EQUAL(test_no_base.base_url().size(), 0);
        }
        {
            test_router test_no_base{"///"};
            BOOST_CHECK_EQUAL(test_no_base.routes().size(), 0);
            BOOST_CHECK_EQUAL(test_no_base.base_url().size(), 0);
        }
        {
            test_router test_base{"///a/b//c///"};
            BOOST_CHECK_EQUAL(test_base.routes().size(), 0);
            BOOST_CHECK_EQUAL(test_base.base_url().size(), 3);
            BOOST_CHECK_EQUAL(test_base.base_url().at(0), "a");
            BOOST_CHECK_EQUAL(test_base.base_url().at(1), "b");
            BOOST_CHECK_EQUAL(test_base.base_url().at(2), "c");
        }
    }


    BOOST_AUTO_TEST_CASE(create_route)
    {
        test_router router{};
        auto route = router.create_route("/parents");
        BOOST_CHECK_EQUAL(router.routes().size(), 1);
        route->add_method_handler("OPTIONS", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods.size(), 0);
            return ++a;
        });
        auto route2 = router.create_route("/parents");
        BOOST_CHECK_EQUAL(router.routes().size(), 1);
        BOOST_CHECK_EQUAL(route, route2);
        auto parent_route = router.create_route("/parents/{parent}");
        BOOST_CHECK_EQUAL(router.routes().size(), 2);
        auto parent_route2 = router.create_route("/parents/{parent}");
        BOOST_CHECK_EQUAL(router.routes().size(), 2);
        BOOST_CHECK_EQUAL(parent_route, parent_route2);
    }

    BOOST_AUTO_TEST_CASE(create_handler)
    {
        test_router router{};
        // -------------------------------------------------------- //
        auto parents_route = router.create_route("/parents");
        BOOST_CHECK_EQUAL(router.routes().size(), 1);
        parents_route->add_method_handler("OPTIONS", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, POST");
            return a + 1;
        });
        parents_route->add_method_handler("GET", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, POST");
            return a + 2;
        });
        parents_route->add_method_handler("DELETE", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, POST");
            return a + 3;
        });
        parents_route->add_method_handler("POST", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, POST");
            return a + 4;
        });

        // -------------------------------------------------------- //
        auto parent_route = router.create_route("/parents/{parent}");
        parent_route->add_method_handler("OPTIONS", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 1);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "1234");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, PUT, PATCH");
            return a + 10;
        });
        parent_route->add_method_handler("GET", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 1);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "2345");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, PUT, PATCH");
            return a + 20;
        });
        parent_route->add_method_handler("DELETE", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 1);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "3456");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, PUT, PATCH");
            return a + 30;
        });
        parent_route->add_method_handler("PUT", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 1);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "4567");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, PUT, PATCH");
            return a + 40;
        });
        parent_route->add_method_handler("PATCH", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 1);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "5678");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, PUT, PATCH");
            return a + 50;
        });

        // -------------------------------------------------------- //
        auto children = router.create_route("/parents/{parent}/children");
        children->add_method_handler("GET", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 1);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "615243");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 1);
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).first, "key1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).second, "value1");
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, POST");
            return a + 200;
        });
        children->add_method_handler("DELETE", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 1);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "273645");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 2);
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).first, "key2");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).second, "value2");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(1).first, "key1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(1).second, "value1");
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, POST");
            return a + 300;
        });
        children->add_method_handler("POST", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 1);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "837465");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 3);
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).first, "key1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).second, "value1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(1).first, "key2");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(1).second, "value1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(2).first, "q");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(2).second, "search term");
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, POST");
            return a + 400;
        });
        children->add_method_handler("OPTIONS", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 1);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "495867");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 3);
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).first, "key1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).second, "value1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(1).first, "key1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(1).second, "value2");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(2).first, "key3");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(2).second, "value 3");
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, POST");
            return a + 100;
        });

        auto complex = router.create_route("/parents/{mother}/{father}/children/{name}/{age}/{height}");
        complex->add_method_handler("GET", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 5);
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 0), "Anna");
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 1), "George");
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 2), "Jonny");
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 3), "14");
            BOOST_CHECK_EQUAL(captured_resource(parsed_route.captures, 4), "165");
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 3);
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).first, "key1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(0).second, "value1");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(1).first, "key2");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(1).second, "value2");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(2).first, "key3");
            BOOST_CHECK_EQUAL(parsed_route.queries.at(2).second, "value3");
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET");
            return a + 1000;
        });

        // -------------------------------------------------------- //
        BOOST_CHECK_EQUAL(2, router.create_handler("/parents", "OPTIONS", 1));
        BOOST_CHECK_EQUAL(3, router.create_handler("/parents", "GET", 1));
        BOOST_CHECK_EQUAL(5, router.create_handler("/parents", "POST", 1));
        BOOST_CHECK_EQUAL(4, router.create_handler("/parents", "DELETE", 1));

        // There is no PUT method for /parents
        try {
            router.create_handler("/parents", "PUT", 1);
            BOOST_REQUIRE(false);
        } catch (const response& e) {
            BOOST_CHECK_EQUAL(e.status_code() , 405);
            BOOST_CHECK_EQUAL(e.at("Allow"), "GET, DELETE, POST");
        } catch(...) {
            BOOST_REQUIRE(false);
        }

        // Replace old method
        parents_route->add_method_handler("OPTIONS", [](const parsed_route &parsed_route, int a) -> std::optional<int> {
            BOOST_CHECK_EQUAL(parsed_route.captures.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.queries.size(), 0);
            BOOST_CHECK_EQUAL(parsed_route.all_but_options_node_methods, "GET, DELETE, POST");
            return a - 1;
        });
        BOOST_CHECK_EQUAL(0, router.create_handler("/parents", "OPTIONS", 1));
        BOOST_CHECK_EQUAL(0, router.create_handler("parents", "OPTIONS", 1));
        BOOST_CHECK_EQUAL(0, router.create_handler("/////parents", "OPTIONS", 1));
        BOOST_CHECK_EQUAL(0, router.create_handler("/////parents//", "OPTIONS", 1));
        BOOST_CHECK_EQUAL(0, router.create_handler("/////parents//", "OPTIONS", 1));
        // -------------------------------------------------------- //

        // -------------------------------------------------------- //
        BOOST_CHECK_EQUAL(11, router.create_handler("/parents/1234", "OPTIONS", 1));
        BOOST_CHECK_EQUAL(22, router.create_handler("/parents/2345", "GET", 2));
        BOOST_CHECK_EQUAL(33, router.create_handler("/parents/3456", "DELETE", 3));
        BOOST_CHECK_EQUAL(44, router.create_handler("/parents/4567", "PUT", 4));
        BOOST_CHECK_EQUAL(55, router.create_handler("/parents/5678", "PATCH", 5));

        BOOST_CHECK_EQUAL(11, router.create_handler("parents//1234", "OPTIONS", 1));
        BOOST_CHECK_EQUAL(11, router.create_handler("/////parents//1234", "OPTIONS", 1));
        BOOST_CHECK_EQUAL(11, router.create_handler("/////parents//1234//", "OPTIONS", 1));
        // -------------------------------------------------------- //


        // -------------------------------------------------------- //
        BOOST_CHECK_EQUAL(222, router.create_handler("/parents/615243/children?key1=value1", "GET", 22));
        BOOST_CHECK_EQUAL(333, router.create_handler("/parents/273645/children?key2=value2&key1=value1", "DELETE", 33));
        BOOST_CHECK_EQUAL(444, router.create_handler("/parents/837465/children?key1=value1&key2=value1&q=search%20term", "POST", 44));
        BOOST_CHECK_EQUAL(111, router.create_handler("/parents/495867/children?key1=value1&key1=value2&key3=value%203", "OPTIONS", 11));

        BOOST_CHECK_EQUAL(111, router.create_handler("parents/495867/children?key1=value1&key1=value2&key3=value%203", "OPTIONS", 11));
        BOOST_CHECK_EQUAL(111, router.create_handler("parents//495867//children//?&key1=value1&key1=value2&key3=value%203", "OPTIONS", 11));
        BOOST_CHECK_EQUAL(111, router.create_handler("//parents//495867//children//?&key1=value1&&&key1=value2&key3=value%203&&&", "OPTIONS", 11));
        // A '=' inside a value is part of the value, @see query_value_may_contain_equals
        // -------------------------------------------------------- //


        // -------------------------------------------------------- //
        BOOST_CHECK_EQUAL(1111, router.create_handler("/parents/Anna/George/children/Jonny/14/165?key1=value1&key2=value2&key3=value3", "GET", 111));

        BOOST_CHECK_EQUAL(1111, router.create_handler("parents/Anna/George/children/Jonny/14/165?key1=value1&key2=value2&key3=value3&", "GET", 111));
        BOOST_CHECK_EQUAL(1111, router.create_handler("//parents//Anna//George////children///Jonny/14/165////?&&&key1=value1&&&&key2=value2&key3=value3&&&", "GET", 111));
        // -------------------------------------------------------- //
    }

    BOOST_AUTO_TEST_CASE(query_value_may_contain_equals)
    {
        // Only the first '=' of a pair separates the key from the value: '=' is
        // legal inside a query value, and base64 padding depends on it. It used
        // to be answered with a 400 ("a=b=c") or, because split() drops the
        // empty pieces, silently truncated ("t=...==" lost its padding).
        restful_router<int> router;
        query_strings queries;
        router.create_route("items")->add_method_handler("GET", [&queries](parsed_route route) {
            queries = std::move(route.queries);
            return 1;
        });
        auto query = [&](const std::string &url) -> const query_strings & {
            queries.clear();
            BOOST_CHECK_EQUAL(router.create_handler(url, "GET"), 1);
            return queries;
        };

        {
            const auto &q = query("/items?a=b=c");
            BOOST_REQUIRE_EQUAL(q.size(), 1);
            BOOST_CHECK_EQUAL(q.at(0).first, "a");
            BOOST_CHECK_EQUAL(q.at(0).second, "b=c");
        }
        { // base64 padding must survive, and it must not eat the next pair
            const auto &q = query("/items?t=eyJ0eXAiOiJKV1QifQ==&next=1");
            BOOST_REQUIRE_EQUAL(q.size(), 2);
            BOOST_CHECK_EQUAL(q.at(0).first, "t");
            BOOST_CHECK_EQUAL(q.at(0).second, "eyJ0eXAiOiJKV1QifQ==");
            BOOST_CHECK_EQUAL(q.at(1).first, "next");
            BOOST_CHECK_EQUAL(q.at(1).second, "1");
        }
        { // a value which starts with '=' keeps it
            const auto &q = query("/items?a==b");
            BOOST_REQUIRE_EQUAL(q.size(), 1);
            BOOST_CHECK_EQUAL(q.at(0).first, "a");
            BOOST_CHECK_EQUAL(q.at(0).second, "=b");
        }
        { // the escaped form decodes to the very same value
            const auto &q = query("/items?a=b%3Dc");
            BOOST_REQUIRE_EQUAL(q.size(), 1);
            BOOST_CHECK_EQUAL(q.at(0).first, "a");
            BOOST_CHECK_EQUAL(q.at(0).second, "b=c");
        }
        { // trailing '=' is still an empty value
            const auto &q = query("/items?a=");
            BOOST_REQUIRE_EQUAL(q.size(), 1);
            BOOST_CHECK_EQUAL(q.at(0).first, "a");
            BOOST_CHECK_EQUAL(q.at(0).second, "");
        }

        // a pair with no key at all stays a bad request
        BOOST_CHECK_THROW(router.create_handler("/items?=", "GET"), response);
    }

    BOOST_AUTO_TEST_CASE(method_does_not_shadow_later_routes)
    {
        restful_router<int> router;
        router.create_route("items/{id}")->add_method_handler("GET", [](parsed_route) { return 1; });
        router.create_route("items/all")->add_method_handler("POST", [](parsed_route) { return 2; });

        BOOST_CHECK_EQUAL(router.create_handler("/items/42", "GET"), 1);
        BOOST_CHECK_EQUAL(router.create_handler("/items/all", "GET"), 1);
        BOOST_CHECK_EQUAL(router.create_handler("/items/all", "POST"), 2);
    }

BOOST_AUTO_TEST_SUITE_END()

    /*!
     * The handler shapes add_method_handler() accepts. A session needs a stream
     * to run against and nothing here touches the socket, so a stream which
     * does nothing at all is enough.
     */
    struct fake_stream final : abstract_stream
    {
        void read(request &) override {}
        void write(const_buffer) override {}
        void write(std::span<const const_buffer>) override {}
        std::error_code yield() noexcept override { return {}; }
        std::shared_ptr<abstract_wakeupper> wakeupper() const noexcept override { return {}; }
        std::chrono::seconds keep_alive() const noexcept override { return {}; }
        void set_keep_alive(std::chrono::seconds) noexcept override {}
        const std::string &peer_address() const noexcept override
        {
            static const std::string address{"test"};
            return address;
        }
        int socket_write_size() const override { return 4096; }
        void set_socket_write_size(int) override {}
        int socket_read_size() const override { return 4096; }
        void set_socket_read_size(int) override {}
        std::chrono::seconds session_timeout() const noexcept override { return {}; }
        void set_session_timeout(std::chrono::seconds) noexcept override {}
    };

    /// Registers \a handler for GET on the four customer routes, and returns
    /// something which runs the session matching a url
    template <typename Handler>
    auto router_for(Handler handler)
    {
        auto router = std::make_shared<restful_router<>>("/test/rest/v1/");
        for (const auto route : {"customers",
                                 "customers/{customer_id}",
                                 "customers/{customer_id}/licenses",
                                 "customers/{customer_id}/licenses/{license_id}"}) {
            router->create_route(route)->add_method_handler("GET", handler);
        }
        return [router](std::string_view url) {
            fake_stream stream;
            request req;
            auto session = router->create_handler(url, "GET");
            BOOST_REQUIRE_MESSAGE(bool(session), "no route matched " << url);
            session(stream, req);
        };
    }

    BOOST_AUTO_TEST_SUITE(restful_handlers)

    BOOST_AUTO_TEST_CASE(captures_as_arguments_with_request)
    {
        std::string seen;
        auto run = router_for([&seen](abstract_stream &, request &,
                                     const std::string &customer_id = {},
                                     const std::string &license_id = {}) {
            seen = customer_id + "|" + license_id;
        });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "|");
        run("/test/rest/v1/customers/42");
        BOOST_CHECK_EQUAL(seen, "42|");
        run("/test/rest/v1/customers/42/licenses");
        BOOST_CHECK_EQUAL(seen, "42|");
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "42|7");
    }

    BOOST_AUTO_TEST_CASE(captures_as_arguments_without_request)
    {
        std::string seen;
        auto run = router_for([&seen](abstract_stream &,
                                     const std::string &customer_id = {},
                                     const std::string &license_id = {}) {
            seen = customer_id + "|" + license_id;
        });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "|");
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "42|7");
    }

    BOOST_AUTO_TEST_CASE(optional_captures_with_request)
    {
        // std::optional captures tell "the matched route has no such capture"
        // from "the capture is there and empty"
        std::string seen;
        auto run = router_for([&seen](abstract_stream &, request &,
                                     const std::optional<std::string> &customer_id = {},
                                     const std::optional<std::string> &license_id = {}) {
            seen = customer_id.value_or("-") + "|" + license_id.value_or("-");
        });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "-|-");
        run("/test/rest/v1/customers/42");
        BOOST_CHECK_EQUAL(seen, "42|-");
        run("/test/rest/v1/customers/42/licenses");
        BOOST_CHECK_EQUAL(seen, "42|-");
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "42|7");
    }

    BOOST_AUTO_TEST_CASE(optional_captures_without_request)
    {
        std::string seen;
        auto run = router_for([&seen](abstract_stream &,
                                     const std::optional<std::string> &customer_id = {},
                                     const std::optional<std::string> &license_id = {}) {
            seen = customer_id.value_or("-") + "|" + license_id.value_or("-");
        });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "-|-");
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "42|7");
    }

    BOOST_AUTO_TEST_CASE(optional_capture_of_an_empty_route_part)
    {
        // "//" collapses while splitting, so an empty capture never reaches a
        // handler: a route which doesn't have the capture is the only way an
        // optional comes back as nullopt
        std::string seen;
        auto run = router_for([&seen](abstract_stream &, request &,
                                     const std::optional<std::string> &customer_id = {}) {
            seen = customer_id ? "engaged:" + *customer_id : "nullopt";
        });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "nullopt");
        run("/test/rest/v1/customers/42");
        BOOST_CHECK_EQUAL(seen, "engaged:42");
    }

    BOOST_AUTO_TEST_CASE(captures_as_string_view_arguments)
    {
        std::string seen;
        auto run = router_for([&seen](abstract_stream &, request &, std::string_view customer_id = {}) {
            seen = customer_id;
        });
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "42");
    }

    BOOST_AUTO_TEST_CASE(parsed_route_last_with_request)
    {
        std::string seen;
        auto run = router_for([&seen](abstract_stream &, request &, const parsed_route &route) {
            seen = std::to_string(route.captures.size());
            for (const auto &resource : route.captures)
                seen += " " + resource;
        });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "0");
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "2 42 7");
    }

    BOOST_AUTO_TEST_CASE(parsed_route_last_without_request)
    {
        std::string seen;
        auto run = router_for([&seen](abstract_stream &, const parsed_route &route) {
            seen = captured_resource(route.captures, 0);
        });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "");
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "42");
    }

    BOOST_AUTO_TEST_CASE(captures_keep_the_route_order)
    {
        restful_router router{"/api/"};
        std::string seen;
        router.create_route("parents/{mother}/{father}/children/{name}/{age}")
            ->add_method_handler("GET", [&seen](abstract_stream &, request &, const parsed_route &route) {
                for (const auto &resource : route.captures)
                    seen += resource + " ";
                // past the end is an empty string, never a crash
                seen += "[" + captured_resource(route.captures, 99) + "]";
            });
        fake_stream stream;
        request req;
        auto session = router.create_handler("/api/parents/Anna/George/children/Jonny/14", "GET");
        BOOST_REQUIRE(bool(session));
        session(stream, req);
        BOOST_CHECK_EQUAL(seen, "Anna George Jonny 14 []");
    }

    BOOST_AUTO_TEST_CASE(optional_captures_are_nullopt_when_absent)
    {
        std::string seen;
        const auto show = [](const std::optional<std::string> &value) {
            return value ? "\"" + *value + "\"" : std::string{"nullopt"};
        };
        auto run = router_for([&seen, &show](abstract_stream &, request &,
                                            const std::optional<std::string> &customer_id = {},
                                            const std::optional<std::string> &license_id = {}) {
            seen = show(customer_id) + " " + show(license_id);
        });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "nullopt nullopt");
        run("/test/rest/v1/customers/42");
        BOOST_CHECK_EQUAL(seen, "\"42\" nullopt");
        run("/test/rest/v1/customers/42/licenses");
        BOOST_CHECK_EQUAL(seen, "\"42\" nullopt");
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "\"42\" \"7\"");
    }

    BOOST_AUTO_TEST_CASE(optional_captures_without_a_request)
    {
        std::string seen;
        auto run = router_for([&seen](abstract_stream &, const std::optional<std::string> &customer_id = {}) {
            seen = customer_id.value_or("nullopt");
        });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "nullopt");
        run("/test/rest/v1/customers/42");
        BOOST_CHECK_EQUAL(seen, "42");
    }

    BOOST_AUTO_TEST_CASE(mixing_optional_and_string_captures_falls_back_to_strings)
    {
        // Documented limitation: the shapes are picked as a whole, so a mix is
        // served as strings and the optional is always engaged
        std::string seen;
        auto run = router_for([&seen](abstract_stream &, request &,
                                     const std::string &customer_id = {},
                                     const std::optional<std::string> &license_id = {}) {
            seen = customer_id + "|" + (license_id ? "engaged:" + *license_id : std::string{"nullopt"});
        });
        run("/test/rest/v1/customers/42");
        BOOST_CHECK_EQUAL(seen, "42|engaged:");
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "42|engaged:7");
    }

    BOOST_AUTO_TEST_CASE(neither_a_request_nor_captures)
    {
        std::string seen;
        auto run = router_for([&seen](abstract_stream &) { seen = "streamOnly"; });
        run("/test/rest/v1/customers");
        BOOST_CHECK_EQUAL(seen, "streamOnly");
        // a route with captures reaches it just as well, they are simply dropped
        run("/test/rest/v1/customers/42/licenses/7");
        BOOST_CHECK_EQUAL(seen, "streamOnly");
    }

    BOOST_AUTO_TEST_CASE(a_request_but_no_captures)
    {
        std::string seen;
        auto run = router_for([&seen](abstract_stream &, request &req) {
            seen = req.method().empty() ? "noMethod" : req.method();
        });
        run("/test/rest/v1/customers/42");
        BOOST_CHECK_EQUAL(seen, "noMethod"); // the fake request was never parsed
    }

    BOOST_AUTO_TEST_CASE(every_shape_on_the_same_route)
    {
        // What the test plugin does: each method of one route picks a different
        // handler shape, and each has to be dispatched on its own merits
        restful_router router{"/api/"};
        std::string seen;
        router.create_route("customers/{customer_id}")
            ->add_method_handler("GET", [&seen](abstract_stream &, request &, const std::string &customer_id = {}) {
                seen = "captures+request:" + customer_id;
            })
            .add_method_handler("POST", [&seen](abstract_stream &, const std::string &customer_id = {}) {
                seen = "captures:" + customer_id;
            })
            .add_method_handler("DELETE", [&seen](abstract_stream &, request &, const parsed_route &route) {
                seen = "route+request:" + captured_resource(route.captures, 0);
            })
            .add_method_handler("PATCH", [&seen](abstract_stream &, const parsed_route &route) {
                seen = "route:" + captured_resource(route.captures, 0);
            })
            .add_method_handler("OPTIONS", [&seen](abstract_stream &) { seen = "streamOnly"; });

        fake_stream stream;
        request req;
        const auto run = [&](const std::string &method) {
            seen.clear();
            auto session = router.create_handler("/api/customers/42", method);
            BOOST_REQUIRE_MESSAGE(bool(session), "no route matched " << method);
            session(stream, req);
            return seen;
        };
        BOOST_CHECK_EQUAL(run("GET"), "captures+request:42");
        BOOST_CHECK_EQUAL(run("POST"), "captures:42");
        BOOST_CHECK_EQUAL(run("DELETE"), "route+request:42");
        BOOST_CHECK_EQUAL(run("PATCH"), "route:42");
        BOOST_CHECK_EQUAL(run("OPTIONS"), "streamOnly");
    }

    BOOST_AUTO_TEST_CASE(a_hand_written_session_factory_still_works)
    {
        std::string seen;
        // The long way round: a ReturnType(parsed_route) factory which builds the
        // session itself. add_method_handler() takes it as it always did.
        const auto factory = [&seen](parsed_route route) -> http_session {
            return [&seen, route = std::move(route)](abstract_stream &, request &) {
                seen = captured_resource(route.captures, 0);
            };
        };
        restful_router router{"/api/"};
        router.create_route("customers/{customer_id}")->add_method_handler("GET", factory);
        fake_stream stream;
        request req;
        auto session = router.create_handler("/api/customers/99", "GET");
        BOOST_REQUIRE(bool(session));
        session(stream, req);
        BOOST_CHECK_EQUAL(seen, "99");
    }

    BOOST_AUTO_TEST_SUITE_END()

}
