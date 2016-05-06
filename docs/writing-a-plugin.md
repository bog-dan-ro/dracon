# Writing a plugin

A plugin is a shared object in the server's plugins directory which exports a
few C functions. The server `dlopen()`s every `.so` it finds there at start up,
in no particular order, and from then on asks them for sessions in the order
they declare.

## The entry points

```cpp
#include <dracon/http.h>
#include <dracon/plugin.h>

PLUGIN_EXPORT bool init_plugin(const std::string &conf_dir);
PLUGIN_EXPORT uint32_t plugin_order();
PLUGIN_EXPORT dracon::http_session create_session(const dracon::request &req);
PLUGIN_EXPORT void destory_plugin();
```

`PLUGIN_EXPORT` is `extern "C"` plus default visibility. The server looks these
up by name with `dlsym()`, so the spelling is the interface.

Only `create_session()` and `plugin_order()` are required. The other two are
called if the plugin exports them and skipped if it does not.

`destory_plugin` really is spelled that way. It is a typo which is now part of
the ABI, and correcting it would silently stop the clean up of every plugin
built so far.

### init_plugin

Called once, right after the library is loaded, with the server's
configuration directory. Read the configuration, build the routes, open
whatever the plugin needs, and return `true`. Returning `false` aborts the
loading of that plugin; the server logs it and carries on with the others.

```cpp
PLUGIN_EXPORT bool init_plugin(const std::string &conf_dir)
{
    boost::property_tree::ptree conf;
    boost::property_tree::info_parser::read_info(conf_dir + "/customers.conf", conf);
    s_databaseUrl = conf.get<std::string>("database");

    for (const auto route : {"customers", "customers/{customer_id}"})
        s_router.create_route(route)->add_method_handler("GET", customers);
    return true;
}
```

This runs on the main thread, before any connection is accepted, so nothing
here needs a lock.

### plugin_order

Where the plugin sits in the chain `create_session()` is called along. Lower
runs first. Two plugins with the same order are asked in an unspecified order,
so give a plugin which needs to see a URL before another one a lower number.

Some of the range is spoken for: the built in `/server_status` endpoint sits at
`UINT32_MAX / 2`, and the bundled `staticContent` plugin at `UINT32_MAX`, so
that a plugin which serves files off disk is always the last one asked.

### create_session

Called for every request, with the head parsed and none of the body read. It
routes and nothing else: return the callable which will answer the request, or
`{}` to let the next plugin have a look. The server answers 503 when no plugin
claims the request.

```cpp
PLUGIN_EXPORT dracon::http_session create_session(const dracon::request &req)
{
    if (!req.url().starts_with("/api/"))
        return {};
    return s_router.create_handler(req.url(), req.method());
}
```

The request is in `request::state::headers_completed`, or `completed` when it
has no body at all. Its header names have been canonicalized, so look them up
the way they are usually written: `req["Content-Type"]`, never
`req["content-type"]`.

`create_session()` runs on the event loop thread of the connection, along with
every other one, so it must be as cheap as a URL comparison. Anything which can
throw belongs in the session, where the server can turn it into a response.

### destory_plugin

Called while the server shuts down, before the library is unloaded. Stop the
threads the plugin started and finish what is in flight before returning:
anything still running when this returns is running in a library which is about
to disappear.

## The session

The session is where the request is answered. It runs inside the coroutine of
its connection, so it reads as sequential code even though every read and write
yields to the event loop:

```cpp
return [](dracon::abstract_stream &stream, dracon::request &req) {
    std::string body;
    req.append_body_callback([&body](std::string_view part) { body += part; },
                           64 * 1024);
    stream >> req;
    stream << dracon::response{200, process(body)};
};
```

Three rules cover most of what goes wrong:

Register the body callback before reading. A request which carries a body the
session never asked for is answered with a 400, and one over the limit with a
413, before any of it is read.

Read the request out, even when the body is of no interest. An unread body
cannot be told apart from the next request on the connection, so the server
closes the connection instead of reusing it.

Never block, and never hold a lock across a write. Both stall every other
session on the same event loop thread. [Threads and slow
work](concurrency.md) is about what to do instead.

## Building it

```cmake
find_package(Dracon REQUIRED)

# MODULE and not SHARED: a plugin is dlopen()ed, never linked against
add_library(MyPlugin MODULE my_plugin.cpp)
target_link_libraries(MyPlugin PRIVATE dracon::dracon Boost::log Threads::Threads)
target_link_options(MyPlugin PRIVATE -Wl,--no-undefined)
set_target_properties(MyPlugin PROPERTIES
                      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/dracon/plugins)
install(TARGETS MyPlugin LIBRARY DESTINATION lib/dracon/plugins)
```

`dracon::dracon` carries `-fnon-call-exceptions`, which a plugin has to be
compiled with. Without it a null pointer dereference in a handler kills the
server instead of becoming a `dracon::segmentation_fault_error`.

`-Wl,--no-undefined` is worth adding by hand. Plugins are loaded with
`RTLD_NOW`, so an unresolved symbol is a load failure at start up rather than a
crash later, but it is better still as a link error.

Plugins are loaded with `RTLD_DEEPBIND`, so a plugin's own copy of a symbol
wins over the server's. That is what lets two plugins link different versions
of the same library without either of them noticing.

## Configuration

A plugin reads its own file from the configuration directory `init_plugin()`
was given, conventionally `<plugin name>.conf`. It is the same Boost
property_tree INFO format the server uses, described in
[Configuration](configuration.md).

## Starting from the template

`template/` in this repository is a working project to copy: a REST plugin
using nlohmann/json, its configuration file, and tests which start a real
server. It finds an installed Dracon and fetches one when there is none, so it
builds on its own.
