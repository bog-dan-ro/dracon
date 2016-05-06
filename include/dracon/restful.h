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

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dracon/exceptions.h>
#include <dracon/http.h>
#include <dracon/plugin.h>
#include <dracon/utils.h>

/*!
 * \file restful.h
 *
 * URL routing for a REST API.
 *
 * A router owns a base URL and a set of routes. Each route is a path, with the
 * parts it captures written in braces, and carries one handler per method:
 * \code
 *     dracon::restful_router router{"/api/v1/"};
 *
 *     PLUGIN_EXPORT bool init_plugin(const std::string &)
 *     {
 *         router.create_route("customers/{customer_id}")
 *             ->add_method_handler("GET", get_customer)
 *              .add_method_handler("DELETE", delete_customer);
 *         return true;
 *     }
 *
 *     PLUGIN_EXPORT dracon::http_session create_session(const dracon::request &req)
 *     {
 *         return router.create_handler(req.url(), req.method());
 *     }
 * \endcode
 *
 * The handler is the session, and what the route captured arrives as ordinary
 * arguments, in the order the route declares them. restful_route::add_method_handler()
 * has the shapes a handler may take.
 *
 * A URL which matches no route gives an empty session, and create_session()
 * returning it lets the next plugin have a look. A URL which matches a route
 * but not with this method is answered with a 405 and the Allow header, which
 * the router throws on its own.
 */

namespace dracon {

/// The query string of a URL, decoded, in the order it was written. A key
/// which was repeated is in there as many times, and one with no '=' has an
/// empty value.
using query_strings = std::vector<std::pair<std::string, std::string>>;

/*!
 * \brief captured_resources
 *
 * The values a route captured, in the order the route declares them.
 *
 * Only the order matters: a handler takes the captures as plain arguments
 * (\see restful_route::add_method_handler), so the name a route gives a capture
 * is documentation for whoever reads the route, and never needed at runtime.
 */
using captured_resources = std::vector<std::string>;

/*!
 * \brief captured_resource
 * \return the \a index th captured resource, or an empty string if the matched
 * route captured fewer than that. A handler registered on several routes gets
 * empty strings for the captures its route doesn't have.
 */
inline const std::string &captured_resource(const captured_resources &captures, size_t index)
{
    static const std::string empty;
    return index < captures.size() ? captures[index] : empty;
}

/*!
 * \brief The parsed_route struct
 *
 * Everything the router worked out about one request: what the route captured,
 * the query string, and the methods the matched route answers. A handler which
 * takes it instead of the captures gets all of it.
 */
struct parsed_route
{
    /*!
     * \brief What the route captured, in the order it declares the captures.
     *
     * captured_resource(captures, i) reads one without going out of bounds.
     */
    captured_resources captures;
    /*!
     * \brief The decoded query string, as key and value pairs.
     *
     * In the order the URL wrote them, so a key which was repeated is in here
     * as many times.
     */
    query_strings queries;

    /*!
     * \brief The methods this route answers, other than OPTIONS.
     *
     * Already formatted as an Allow header value, "GET, PUT, DELETE", so an
     * OPTIONS handler can send it as it is.
     */
    std::string all_but_options_node_methods;

    bool operator ==(const parsed_route &other) const = default;
};

/*!
 * \brief What a route stores for one method.
 *
 * A factory: the router calls it with what it parsed out of the URL and it
 * returns the ReturnType of the router, which for the default http_session is
 * the session answering the request.
 *
 * A handler passed to restful_route::add_method_handler() is wrapped into one of
 * these, so this is only written out by hand when the router returns something
 * other than an http_session.
 */
template <typename ReturnType, typename ...Args>
using restful_route_method_handler = std::function<ReturnType(parsed_route parsed_route, Args ...args)>;

namespace detail {

template <typename> inline constexpr bool always_false = false;

/// A handler may take at most this many captured resources as arguments
inline constexpr size_t max_handler_captures = 8;
inline constexpr size_t no_capture_match = size_t(-1);

/*!
 * How a handler asks for its captures. A capture the matched route doesn't have
 * is an empty string...
 */
struct string_capture
{
    using Arg = const std::string &;
    static const std::string &get(const captured_resources &captures, size_t index)
    {
        return captured_resource(captures, index);
    }
};

/// ... or a nullopt, for a handler which would rather tell "absent" from "empty"
struct optional_capture
{
    using Arg = std::optional<std::string>;
    static Arg get(const captured_resources &captures, size_t index)
    {
        if (index < captures.size())
            return captures[index];
        return std::nullopt;
    }
};

template <typename Arg, size_t> using capture_arg = Arg;

template <typename Handler, typename Arg, bool with_request, size_t... I>
constexpr bool is_capture_invocable(std::index_sequence<I...>)
{
    if constexpr (with_request)
        return std::is_invocable_v<Handler &, abstract_stream &, request &, capture_arg<Arg, I>...>;
    else
        return std::is_invocable_v<Handler &, abstract_stream &, capture_arg<Arg, I>...>;
}

/*!
 * \brief capture_arity
 * \return how many captures \a Handler takes as \c Capture::Arg, or
 * no_capture_match. Never fewer than \a min.
 *
 * Widest first on purpose: a handler which defaults its trailing captures is
 * invocable with fewer arguments as well, and it should be handed everything
 * it declared.
 */
template <typename Handler, typename Capture, bool with_request, size_t min = 0,
          size_t N = max_handler_captures>
constexpr size_t capture_arity()
{
    if constexpr (is_capture_invocable<Handler, typename Capture::Arg, with_request>(std::make_index_sequence<N>{}))
        return N;
    else if constexpr (N > min)
        return capture_arity<Handler, Capture, with_request, min, N - 1>();
    else
        return no_capture_match;
}

template <typename Capture, typename Handler, size_t... I>
http_session bind_captures(Handler handler, parsed_route route, std::true_type, std::index_sequence<I...>)
{
    return [handler = std::move(handler), route = std::move(route)](abstract_stream &stream, request &req) mutable {
        handler(stream, req, Capture::get(route.captures, I)...);
    };
}

template <typename Capture, typename Handler, size_t... I>
http_session bind_captures(Handler handler, parsed_route route, std::false_type, std::index_sequence<I...>)
{
    return [handler = std::move(handler), route = std::move(route)](abstract_stream &stream, request &) mutable {
        handler(stream, Capture::get(route.captures, I)...);
    };
}

template <typename Handler>
http_session make_session(Handler handler, parsed_route route)
{
    if constexpr (std::is_invocable_v<Handler &, abstract_stream &, request &, const parsed_route &>) {
        return [handler = std::move(handler), route = std::move(route)](abstract_stream &stream, request &req) mutable {
            handler(stream, req, route);
        };
    } else if constexpr (std::is_invocable_v<Handler &, abstract_stream &, const parsed_route &>) {
        return [handler = std::move(handler), route = std::move(route)](abstract_stream &stream, request &) mutable {
            handler(stream, route);
        };
    } else if constexpr (constexpr auto captures =
                             capture_arity<Handler, optional_capture, true, 1>(); captures != no_capture_match) {
        return bind_captures<optional_capture>(std::move(handler), std::move(route), std::true_type{},
                                             std::make_index_sequence<captures>{});
    } else if constexpr (constexpr auto captures =
                             capture_arity<Handler, optional_capture, false, 1>(); captures != no_capture_match) {
        return bind_captures<optional_capture>(std::move(handler), std::move(route), std::false_type{},
                                             std::make_index_sequence<captures>{});
    } else if constexpr (constexpr auto captures =
                             capture_arity<Handler, string_capture, true>(); captures != no_capture_match) {
        return bind_captures<string_capture>(std::move(handler), std::move(route), std::true_type{},
                                           std::make_index_sequence<captures>{});
    } else if constexpr (constexpr auto captures =
                             capture_arity<Handler, string_capture, false>(); captures != no_capture_match) {
        return bind_captures<string_capture>(std::move(handler), std::move(route), std::false_type{},
                                           std::make_index_sequence<captures>{});
    } else {
        static_assert(always_false<Handler>,
                      "This handler has a shape add_method_handler() doesn't know. It has to be callable as "
                      "(abstract_stream &, [request &], captures...), with at most max_handler_captures captures, "
                      "each one taking a const std::string &, or as "
                      "(abstract_stream &, [request &], const parsed_route &)");
    }
}

} // namespace detail

template <typename ReturnType = http_session, typename ...Args> class restful_router;

/*!
 * \brief The restful_route class
 *
 * One path in a router, and the handler it has for each method. Routes are
 * created by restful_router::create_route() and never on their own.
 */
template <typename ReturnType, typename ...Args>
class restful_route
{
public:
    /*!
     * \brief add_method_handler
     *
     * Registers \a handler for \a method on this route.
     *
     * The handler is normally written as the session itself, and the captured
     * resources arrive as arguments, in the order the route declares them:
     * \code
     *     const auto handler = [](dracon::abstract_stream &stream,
     *                             dracon::request &req,
     *                             const std::string &customer_id = {},
     *                             const std::string &license_id = {}) { ... };
     *     router.create_route("customers")->add_method_handler("GET", handler);
     *     router.create_route("customers/{customer_id}")->add_method_handler("GET", handler);
     *     router.create_route("customers/{customer_id}/licenses/{license_id}")
     *         ->add_method_handler("GET", handler);
     * \endcode
     * One handler can serve several routes: the captures a matched route does
     * not have arrive as empty strings, which is what the defaults above
     * document. Any parameter a \c const std::string & converts to works, so
     * \c std::string_view is fine too.
     *
     * Ask for \c const std::optional<std::string> & instead and an absent
     * capture is \c nullopt rather than an empty string, which is worth it when
     * "not in this route" and "empty" should read differently:
     * \code
     *     [](dracon::abstract_stream &stream, dracon::request &req,
     *        const std::optional<std::string> &customer_id = {}) {
     *         if (customer_id) { ... }
     *     }
     * \endcode
     * All of a handler's captures have to be optional for that: mix them with
     * plain strings and they all behave as strings, so the optionals are always
     * engaged.
     *
     * The \c request & is optional. Leave it out when there is nothing to read
     * from the request - an OPTIONS which answers out of the route, say:
     * \code
     *     router.create_route("customers/{customer_id}")->add_method_handler("OPTIONS",
     *         [](dracon::abstract_stream &stream, const dracon::parsed_route &route) {
     *             stream << dracon::response{204, {}, {{"Allow", route.all_but_options_node_methods}}};
     *         });
     * \endcode
     * A handler which takes no request & cannot read one either, so a client
     * which does send a body gets the connection closed rather than a 400.
     *
     * These shapes are recognised, and the \c request & is optional in each:
     * \code
     *     (abstract_stream &, request &, captures...)
     *     (abstract_stream &, captures...)
     *     (abstract_stream &, request &, const parsed_route &)  // everything: names, queries, methods
     *     (abstract_stream &, const parsed_route &)
     * \endcode
     * A \c ReturnType(parsed_route, Args...) factory which builds the session
     * itself is still accepted, and is the only form a router with a
     * ReturnType other than http_session takes.
     */
    template <typename Handler>
    restful_route &add_method_handler(std::string method, Handler handler)
    {
        if constexpr (std::is_convertible_v<Handler, restful_route_method_handler<ReturnType, Args...>>) {
            return set_method_handler(std::move(method),
                                    restful_route_method_handler<ReturnType, Args...>{std::move(handler)});
        } else {
            static_assert(std::is_same_v<ReturnType, http_session> && sizeof...(Args) == 0,
                          "This router doesn't return an http_session, so it only takes a "
                          "ReturnType(parsed_route, Args...) handler");
            return set_method_handler(std::move(method),
                                    [handler = std::move(handler)](parsed_route route) mutable -> http_session {
                                        return detail::make_session(handler, std::move(route));
                                    });
        }
    }

    /// \return true if this route was declared as \a route. Capture names do
    /// not matter, only their positions, so "items/{id}" and "items/{item_id}"
    /// are the same route.
    bool operator == (std::string_view route) {
        auto other = parse_route_parts(route);
        if (other.size() != m_route_parts.size())
            return false;
        for (size_t i = 0; i < other.size(); ++i) {
            if (other[i].first != m_route_parts[i].first ||
                    other[i].second != m_route_parts[i].second) {
                return false;
            }
        }
        return true;
    }
private:
    restful_route &set_method_handler(std::string method, restful_route_method_handler<ReturnType, Args...> creator)
    {
        if (m_methods.find(method) == m_methods.end()) {
            if (method != "OPTIONS"sv)
                m_all_methods += m_all_methods.empty() ? method : ", " + method;
            m_methods.emplace(std::move(method), std::move(creator));
        } else {
            m_methods[method] = std::move(creator);
        }
        return *this;
    }

protected:
    using route_part_list = std::vector<std::pair<bool, std::string>>;
    route_part_list m_route_parts;
    std::unordered_map<std::string, restful_route_method_handler<ReturnType, Args...>> m_methods;
    std::string m_all_methods;

private:
    template <typename T, typename ...A>
    friend class restful_router;

    static route_part_list parse_route_parts(std::string_view route)
    {
        route_part_list res;
        auto parts = split(route, '/');
        for (const auto &part : parts) {
            if (part.size() < 2)
                throw response{400, "Invalid route"sv};
            if (part.front() == '{' && part.back() == '}')
                res.emplace_back(std::make_pair(true, std::string{part.substr(1, part.size() - 2)}));
            else
                res.emplace_back(std::make_pair(false, std::string{part}));
        }
        return res;
    }

    /*!
     * \brief restful_route
     *
     * \param route to match, relative to the base URL of the router, with the
     * captured parts in braces, e.g. "parents/{parent}/children/{child}"
     * \throw response 400 if a part of the route is shorter than two
     * characters
     */
    restful_route(std::string_view route)
        : m_route_parts(parse_route_parts(route))
    {}

    std::optional<std::pair<captured_resources, restful_route_method_handler<ReturnType, Args...>>>
    create_handler(const split_vector &url_parts, const std::string &method, bool &path_matched) const
    {
        if (url_parts.size() != m_route_parts.size())
            return {};
        captured_resources captures;
        for (size_t i = 0; i < url_parts.size(); ++i) {
            const auto &part = m_route_parts[i];
            if (part.first) {
                captures.emplace_back(url_parts[i]);
            } else if (part.second != url_parts[i]) {
                return  {};
            }
        }
        path_matched = true;
        auto method_it = m_methods.find(method);
        if (method_it == m_methods.end())
            return {};
        return std::make_optional(std::make_pair(std::move(captures), method_it->second));
    }
};

/*!
 * \brief The restful_router class
 *
 * Matches a URL and a method against the routes it was given, and returns what
 * the matching handler produced. ReturnType defaults to http_session, which is
 * what a plugin wants, so the plain spelling is the plugin router:
 * \code
 *     dracon::restful_router router{"/api/v1/"};        // restful_router<http_session>
 * \endcode
 * Somewhere the arguments cannot be deduced, as a template argument of its
 * own, name it restful_router<>.
 *
 * The routes are usually declared from init_plugin() and only read afterwards,
 * which is what makes a router safe to share between the event loop threads. A
 * router which gains routes while the server is answering requests needs a
 * lock of its own.
 */
template <typename ReturnType, typename ...Args>
class restful_router
{
    using restful_route_ptr = std::shared_ptr<restful_route<ReturnType, Args...>>;
public:
    /*!
     * \param base_url every route hangs off, "/api/v1/" for instance. Routes
     * are declared relative to it and create_handler() ignores a URL which does
     * not start with it.
     */
    restful_router(std::string_view base_url = {})
    {
        auto base_url_parts = split(base_url, '/');
        for (const auto &part : base_url_parts)
            m_base_url.emplace_back(std::string{part});
    }
    /*!
     * \brief create_route
     *
     * \param route to match, relative to the base URL, with the captured parts
     * in braces, e.g. "customers/{customer_id}/licenses/{license_id}"
     *
     * \return the route, so that handlers can be added to it. Declaring the
     * same route twice returns the one which is already there rather than
     * shadowing it, so the methods of a route can be registered from several
     * places.
     */
    restful_route_ptr create_route(std::string_view route)
    {
        for (auto rt : m_routes)
            if (*rt == route)
                return rt;
        return m_routes.emplace_back(restful_route_ptr{new restful_route<ReturnType, Args...>{route}});
    }

    /*!
     * \brief create_handler
     *
     * Matches \a url and \a method against the routes, and calls the handler
     * of the first route which matches with what the URL captured and with the
     * decoded query string. Any \a args are forwarded to it.
     *
     * \return what the handler returned, or {} if no route matched, which is
     * how a plugin says the request is not its own
     *
     * \throw response 405 with an Allow header if the path matched a route but
     * the method did not, and response 400 on a malformed query string
     */
    ReturnType create_handler(std::string_view url, const std::string &method, Args ...args) const
    {
        auto qpos = url.find('?');
        auto resources = split(url.substr(0, qpos), '/');
        if (resources.size() < m_base_url.size() + 1)
            return {};
        for (size_t i = 0; i < m_base_url.size(); ++i)
            if (resources[i] != m_base_url[i])
                return {};
        resources.erase(resources.begin(), resources.begin() + m_base_url.size());
        std::optional<std::string> allowed_methods;
        for (const auto &route : m_routes) {
            bool path_matched = false;
            if (auto handle = route->create_handler(resources, method, path_matched)) {
                parsed_route parsed_route;
                parsed_route.all_but_options_node_methods = route->m_all_methods;
                parsed_route.captures = std::move(handle->first);
                if (qpos != std::string::npos) {
                    auto &queries = parsed_route.queries;
                    for (const auto &kv_pair : split(url.substr(qpos + 1), '&')) {
                        // Only the first '=' separates the pair, the rest of it
                        // is the value: '=' is legal in a query value and
                        // base64 padding (a JWT, a signature, ...) relies on it
                        auto kv = split(kv_pair, '=', 1);
                        switch (kv.size()) {
                        case 1:
                            queries.emplace_back(std::make_pair(unescape_url(kv[0]), ""));
                            break;
                        case 2:
                            queries.emplace_back(std::make_pair(unescape_url(kv[0]),
                                                      unescape_url(kv[1])));
                            break;
                        default: // a pair with no key at all, e.g. "?="
                            throw response{400, "Invalid query strings"sv};
                        }
                    }
                }
                return handle->second(parsed_route, args...);
            }
            if (path_matched && !allowed_methods)
                allowed_methods = route->m_all_methods;
        }
        if (allowed_methods)
            throw response{405, {}, {{"Allow", *allowed_methods}}};
        return {};
    }

protected:
    std::vector<std::string> m_base_url;
    std::vector<restful_route_ptr> m_routes;
};





} // namespace Dracon
