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

#include <dracon/http.h>
#include <dracon/logging.h>
#include <dracon/plugin.h>
#include <dracon/restful.h>
#include <dracon/thread_worker.h>

#include <cassert>
#include <iostream>
#include <optional>

using namespace std::chrono_literals;

namespace {
using namespace std::string_literals;
using namespace std::string_view_literals;

const auto test100response = "100XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"s;
std::string test50mresponse;

dracon::restful_router s_test_root_restful("/test/rest/v1/");
dracon::thread_worker s_thread_worker{10};

tagged_logger<> logger{"test"};

/*!
 * One handler for all four customer routes: the captured resources arrive as
 * arguments, in the order the routes declare them, and a route which captures
 * fewer of them hands over empty strings.
 * @see dracon::restful_route::add_method_handler
 */
void customers(dracon::abstract_stream &stream, dracon::request &req,
               const std::string &customer_id = {}, const std::string &license_id = {})
{
    stream >> req;
    stream << dracon::response{200, {}, {{"Content-Type", "text/plain"}}}.set_content_length(dracon::chunked_data);
    dracon::chunked_stream chunked_stream{stream};
    dracon::ostream_buffer buff{chunked_stream};
    std::ostream str{&buff};
    str << "customer_id: " << customer_id << "\n";
    str << "license_id: " << license_id << "\n";
}

/*!
 * The same routes with std::optional captures. A route which doesn't declare a
 * capture hands over a nullopt instead of an empty string, which is what to
 * take when "this route has no such resource" and "the resource is empty" have
 * to read differently.
 *
 * All the captures of a handler have to be optional for this: mixed with plain
 * strings they all behave as strings and are always engaged.
 * @see dracon::restful_route::add_method_handler
 */
void orders(dracon::abstract_stream &stream, dracon::request &req,
            const std::optional<std::string> &order_id = {},
            const std::optional<std::string> &item_id = {})
{
    stream >> req;
    stream << dracon::response{200, {}, {{"Content-Type", "text/plain"}}}.set_content_length(dracon::chunked_data);
    dracon::chunked_stream chunked_stream{stream};
    dracon::ostream_buffer buff{chunked_stream};
    std::ostream str{&buff};
    str << "order_id: " << order_id.value_or("<none>") << "\n";
    str << "item_id: " << item_id.value_or("<none>") << "\n";
}

/*!
 * OPTIONS answers out of the route itself, so this one takes no request &: the
 * parameter is optional in every handler shape. Leaving it out also means not
 * reading the request, which is right here (an OPTIONS carries no body) but
 * would make the server drop the connection instead of answering 400 if a
 * client did send one.
 */
void customers_options(dracon::abstract_stream &stream, const dracon::parsed_route &route)
{
    stream << dracon::response{204, {}, {{"Allow", "OPTIONS, " + route.all_but_options_node_methods}}};
}

/// The same routes through the parsed_route form, which is what to use when the
/// query strings or the allowed methods are needed as well
void customers_route(dracon::abstract_stream &stream, dracon::request &req, const dracon::parsed_route &route)
{
    stream >> req;
    stream << dracon::response{200, {}, {{"Content-Type", "text/plain"}}}.set_content_length(dracon::chunked_data);
    dracon::chunked_stream chunked_stream{stream};
    dracon::ostream_buffer buff{chunked_stream};
    std::ostream str{&buff};
    str << "Got " << route.captures.size() << " captured resources\n";
    str << "and " << route.queries.size() << " queries\n";
    str << "All methods but OPTIONS " << route.all_but_options_node_methods << " \n";
    for (const auto &resource : route.captures)
        str << "Resource: " << resource << "\n";
    for (const auto &query : route.queries)
        str << "Query name: " << query.first << "  value: " << query.second << "\n";
}

} // namespace

PLUGIN_EXPORT dracon::http_session create_session(const dracon::request &req)
{
    using namespace dracon::literals;
    auto &url = req.url();
    if (url == "/test0"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            stream << 200_http;
        };

    if (url == "/test100"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            stream << dracon::response{200, test100response};
        };

    if (url == "/test100Chunked"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            stream << dracon::response{200}.set_content_length(dracon::chunked_data);
            dracon::chunked_stream{stream}.write(test100response);
        };

    if (url == "/test50m"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            stream << dracon::response{200}.set_content_length(test50mresponse.size())
                   << test50mresponse;
        };

    if (url == "/test50mChunked"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            stream << dracon::response{200}.set_content_length(dracon::chunked_data);
            dracon::chunked_stream chuncked_stream{stream};
            uint32_t pos = 0;
            do {
                uint32_t chunk_size = 1 + rand() % (1024 * 1024);
                chunk_size = std::min<uint32_t>(chunk_size, test50mresponse.size() - pos);
                chuncked_stream.write({test50mresponse.c_str() + pos, chunk_size});
                pos += chunk_size;
            } while (pos < test50mresponse.size());
        };

    if (url == "/testWorker"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            stream << dracon::response{200}.set_content_length(dracon::chunked_data);
            auto wakeupper = stream.wakeupper();
            auto wait = std::make_shared<std::atomic_bool>();
            auto buffer = std::make_shared<std::string>();
            uint32_t size = 0;
            dracon::chunked_stream chuncked_stream{stream};
            do {
                wait->store(true);
                s_thread_worker.insert_task([=]{
                    // simulate some heavy work
                    std::this_thread::sleep_for(15ms);
                    uint32_t chunk_size = 1000 + (rand() % 4) * 1000;
                    buffer->resize(chunk_size);
                    for (uint32_t i = 0; i < chunk_size; ++i)
                        (*buffer)[i] = '0' + i % 10;
                    wait->store(false);
                    wakeupper->wakeup();
                });
                do {
                    if (auto ec = stream.yield())
                        throw ec;
                } while (wait->load());
                chuncked_stream << *buffer;
                size += buffer->size();
            } while (size < 100000);
        };

    if (url == "/test50ms"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            std::vector<dracon::const_buffer> vec;
            vec.resize(51);
            for (int i = 1; i < 51; ++i) {
                vec[i].ptr = (void*)(test50mresponse.c_str() + 1024 * 1024 * (i - 1));
                vec[i].length = 1024 * 1024;
            }
            auto res = dracon::response{200}.set_content_length(test50mresponse.size()).to_string(stream.keep_alive());
            vec[0].ptr = res.data();
            vec[0].length = res.length();
            stream.write(vec);
        };

    if (url == "/echoTest"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream.set_session_timeout(10s);
            std::string body;
            req.append_body_callback([&](std::string_view buff){
                body.append(buff);
            });
            stream >> req;
            if (std::strtoull(req["Content-Length"].data(), nullptr, 10) != body.size()) {
                throw 400;
            }
            stream << dracon::response{200}.set_content_length(dracon::chunked_data);
            dracon::chunked_stream chunked_stream{stream};
            dracon::ostream_buffer buff{chunked_stream};
            std::ostream res{&buff};
            res << "~~~~ ContentLength: " << req["Content-Length"] << std::endl;
            res << "~~~~ Headers:\n";
            for (const auto &kv : req)
                res << kv.first << " : " << kv.second << std::endl;
            res << "~~~~ Body:\n" << body;
        };

    if (url == "/secureOnly"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            if (!stream.is_secured_connection())
                throw dracon::response{403, "Only secured connections allowed", {{"ErrorKey1","Value1"}, {"ErrorKey2","Value2"}}};
            stream >> req;
            stream << 200_http;
        };

    if (url == "/testExpectation"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            if (req["Expect"] == "100-continue"sv) {
                if (req["X-Continue"] != "100"sv) {
                    throw dracon::response{417};
                }
            }
            req.append_body_callback([](std::string_view buff){
                (void)buff;
            });
            stream >> req;
            stream << 200_http;
        };

    if (url == "/testThowFromRequestComplete"sv)
        return [&](dracon::abstract_stream&, dracon::request&){
            throw 412;
        };

    if (url == "/testThowFromBody"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            req.append_body_callback([](std::string_view){
                throw dracon::response{400, "Body too big, lose some weight",
                                        {{"BodyKey1", "Value1"},
                        {"BodyKey2", "Value2"}}};
            });
            stream >> req;
            stream << 200_http;
        };

    if (url == "/testThowFromWriteResponse"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            throw dracon::response{409, "Throw from WriteResponse", {{"WriteRes1","Value1"}, {"WriteRes2","Value2"}}};
        };

    if (url == "/testThowFromWriteResponseStd"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            throw std::runtime_error{"Throw from WriteResponseStd"};
        };

    if (url == "/testThowFromWriteResponseAfterWrite"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;
            stream << dracon::response{200}.set_content_length(dracon::chunked_data);
            throw std::runtime_error{"Unexpected error"};
        };

    if (url == "/testThrowAfterWakeup"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            stream >> req;

            auto wakeupper = stream.wakeupper();
            auto wait = std::make_shared<std::atomic_bool>();
            wait->store(true);
            s_thread_worker.insert_task([=]{
                // simulate some heavy work
                std::this_thread::sleep_for(100ms);
                wait->store(false);
                wakeupper->wakeup();
            });
            do {
                if (auto ec = stream.yield())
                    throw ec;
            } while (wait->load());
            throw 404;
        };

    // PPP stands for post, put, patch
    if (url == "/testPPP"sv)
        return [&](dracon::abstract_stream& stream, dracon::request& req){
            std::string body;
            req.append_body_callback([&](std::string_view buff){
                body.append(buff);
            });
            stream >> req;
            if (std::strtoull(req["Content-Length"].data(), nullptr, 10) != test50mresponse.size())
                throw dracon::response{400, "Invaid body size"};
            if (body != test50mresponse)
                throw dracon::response{400, "Invaid body"};
            stream << dracon::response{200}.set_content_length(body.length()) << body;
        };

    return s_test_root_restful.create_handler(url, req.method());
}

PLUGIN_EXPORT bool init_plugin(const std::string &/*conf_dir*/)
{
    for (int i = 0; i < 50 * 1024 * 1024; ++i)
        test50mresponse += char(33 + (i % 93));
    for (const auto route : {"customers",
                             "customers/{customer_id}",
                             "customers/{customer_id}/licenses",
                             "customers/{customer_id}/licenses/{license_id}"}) {
        s_test_root_restful.create_route(route)
                ->add_method_handler("GET", customers)
                .add_method_handler("DELETE", customers_route)
                .add_method_handler("OPTIONS", customers_options);
    }
    for (const auto route : {"orders",
                             "orders/{order_id}",
                             "orders/{order_id}/items/{item_id}"}) {
        s_test_root_restful.create_route(route)->add_method_handler("GET", orders);
    }
    return true;
}

PLUGIN_EXPORT uint32_t plugin_order()
{
    return 9999999;
}

PLUGIN_EXPORT void destory_plugin()
{}
