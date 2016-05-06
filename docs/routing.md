# Routing a REST API

[`<dracon/restful.h>`](../include/dracon/restful.h) matches URLs so that
`create_session()` does not have to. A router owns a base URL and a set of
routes, each route is a path with the parts it captures written in braces, and
each route carries one handler per method.

```cpp
#include <dracon/plugin.h>
#include <dracon/restful.h>

namespace {
dracon::restful_router s_router{"/api/v1/"};

void customers(dracon::abstract_stream &stream, dracon::request &req,
               const std::string &customer_id = {},
               const std::string &license_id = {})
{
    stream >> req;
    stream << dracon::response{200, "customer " + customer_id +
                                    ", license " + license_id + "\n"};
}
}

PLUGIN_EXPORT bool init_plugin(const std::string &)
{
    for (const auto route : {"customers",
                             "customers/{customer_id}",
                             "customers/{customer_id}/licenses/{license_id}"})
        s_router.create_route(route)->add_method_handler("GET", customers);
    return true;
}

PLUGIN_EXPORT uint32_t plugin_order() { return 0; }

PLUGIN_EXPORT dracon::http_session create_session(const dracon::request &req)
{
    return s_router.create_handler(req.url(), req.method());
}
```

which answers all three:

```
GET /api/v1/customers                 -> customer , license
GET /api/v1/customers/42              -> customer 42, license
GET /api/v1/customers/42/licenses/7   -> customer 42, license 7
```

## The handler is the session

There is no separate factory step. `add_method_handler()` takes the handler which
answers the request, and what the route captured arrives as ordinary arguments,
in the order the route declares them.

One handler can serve a whole family of routes, as above. A route which
captures fewer resources than the handler declares hands over empty strings,
which is what the defaults in the signature document. Any parameter a
`const std::string &` converts to works, so `std::string_view` is fine.

The name a route gives a capture never reaches the handler. Only the position
matters, and the names are there for whoever reads the route.

### Telling absent from empty

Ask for `const std::optional<std::string> &` instead and a capture the matched
route does not have arrives as `nullopt`:

```cpp
[](dracon::abstract_stream &stream, dracon::request &req,
   const std::optional<std::string> &customer_id = {}) {
    if (customer_id) { ... }
}
```

All of a handler's captures have to be optional for that. Mix them with plain
strings and they all behave as strings, which means the optionals are always
engaged.

### The request is optional

Leave it out when there is nothing to read from the request:

```cpp
s_router.create_route("customers/{customer_id}")->add_method_handler("OPTIONS",
    [](dracon::abstract_stream &stream, const dracon::parsed_route &route) {
        stream << dracon::response{204, {}, {{"Allow", "OPTIONS, " +
                                              route.all_but_options_node_methods}}};
    });
```

A handler which takes no `request &` cannot read one either, so a client which
does send a body gets the connection closed instead of a 400.

### The shapes add_method_handler accepts

```cpp
(abstract_stream &, request &, captures...)
(abstract_stream &, captures...)
(abstract_stream &, request &, const parsed_route &)
(abstract_stream &, const parsed_route &)
```

`add_method_handler()` works out which one it was given. A handler may take up to
`detail::max_handler_captures` captures, which is eight. A shape it does not
recognise is a `static_assert` at the call site, not a runtime surprise.

It also still takes a `ReturnType(parsed_route, Args...)` factory which builds
the session itself. That is the only form a router whose `ReturnType` is not
`http_session` accepts.

## parsed_route

Take it as the last argument, instead of the captures, when the query strings
or the allowed methods are wanted too:

```cpp
void search(dracon::abstract_stream &stream, dracon::request &req,
            const dracon::parsed_route &route)
{
    for (const auto &[key, value] : route.queries)
        ...
}
```

| Member | |
|---|---|
| `captures` | A `std::vector<std::string>`, what the route captured, in the order it declares them. `captured_resource(captures, i)` reads one without going out of bounds |
| `queries` | The decoded query string, as key and value pairs, in the order the URL wrote them |
| `all_but_options_node_methods` | The methods this route answers other than OPTIONS, already formatted as an `Allow` value |

Query strings are unescaped, and only the first `=` separates a key from its
value: `=` is legal in a value, and base64 padding in a token or a signature
depends on that. A key with no `=` at all gets an empty value, and a pair with
no key ("?=") is a 400.

## What the router answers on its own

A URL which matches no route gives an empty session, so `create_session()`
returning it lets the next plugin have a look, and the server answers 503 if
none of them wants it.

A URL which matches a route but not with this method throws a 405 carrying the
right `Allow` header. The handler is never involved.

A malformed query string throws a 400.

## Routes

`create_route()` returns the route, so handlers chain onto it:

```cpp
s_router.create_route("customers/{customer_id}")
    ->add_method_handler("GET", getCustomer)
     .add_method_handler("PUT", putCustomer)
     .add_method_handler("DELETE", deleteCustomer);
```

Declaring the same route twice returns the one which is already there rather
than shadowing it, so the methods of a route can be registered from several
places. Two routes are the same when their parts are, capture names aside:
`items/{id}` and `items/{item_id}` are one route.

Routes are matched in the order they were created, and the first match wins.
Every part of a route has to be at least two characters long, otherwise
`create_route()` throws a 400.

## Threading

A router is normally filled in from `init_plugin()`, before any connection is
accepted, and only read afterwards. That is what makes it safe to share between
the event loop threads. A router which gains routes while the server is
answering requests needs a lock of its own.

## Other return types

`ReturnType` defaults to `http_session`, so the plain `restful_router` is the
plugin router. The template takes any return type and any extra arguments,
which is useful for routing something other than a request:

```cpp
dracon::restful_router<std::optional<int>, int> router{"/x/"};
```

A router like that only takes the `ReturnType(parsed_route, Args...)` factory
form, since the other shapes exist to build an `http_session`.
