# Dracon documentation

Dracon is an HTTP/HTTPS server for Linux. The server itself only speaks the
protocol: everything it answers comes from a plugin, a shared library it loads
at start up. This is the documentation for writing those plugins, and for
running the server which loads them.

The public API is header only and lives in [`include/dracon/`](../include/dracon).
A plugin includes those headers, links the `dracon::dracon` CMake target, and
depends on nothing else of Dracon's.

## Guides

| | |
|---|---|
| [Building and running](building.md) | Dependencies, the build tree, the command line |
| [Writing a plugin](writing-a-plugin.md) | The entry points, the lifecycle, the build flags |
| [Requests and responses](requests-and-responses.md) | Reading a request, writing an answer |
| [Streams](streams.md) | chunked bodies, formatted output, the stream interface |
| [Routing a REST API](routing.md) | Routes, captures, query strings, methods |
| [Reporting errors](errors.md) | What to throw, and what the client ends up seeing |
| [Threads and slow work](concurrency.md) | Why a handler must not block, and what to do instead |
| [Logging](logging.md) | Tagged loggers and the severity macros |
| [Configuration](configuration.md) | `server.conf` and the files it includes |
| [API reference](api-reference.md) | Every public type, header by header |

## The shortest plugin

```cpp
#include <dracon/http.h>
#include <dracon/plugin.h>

PLUGIN_EXPORT uint32_t plugin_order() { return 0; }

PLUGIN_EXPORT dracon::http_session create_session(const dracon::request &req)
{
    if (req.url() != "/hello")
        return {};      // not ours, the server asks the next plugin

    return [](dracon::abstract_stream &stream, dracon::request &req) {
        stream >> req;                              // read the rest of the request
        stream << dracon::response{200, "hello\n"}; // answer it
    };
}
```

Build it as a shared library, drop it in the plugins directory, and it answers
`/hello` on the next start.

## How a request gets to that lambda

The main thread accepts the connection and hands it to the event loop which
has the fewest sessions on it. There is one event loop per hardware thread by
default.

That loop reads the request head and asks every loaded plugin, in ascending
`plugin_order()`, whether it wants the request. The first plugin to return a
session wins, and the server answers 503 if none of them does.

The session then runs inside a coroutine of its own. Every read and write which
would block yields back to the event loop, and the loop resumes the session
when the socket is ready again. That is what lets a handler be written as
ordinary sequential code while a few threads carry a very large number of
connections, and it is also the reason a handler must never block: see
[Threads and slow work](concurrency.md).

## Status of the API

Dracon is beta. The headers, the plugin entry points and the configuration
format can still change between releases.
