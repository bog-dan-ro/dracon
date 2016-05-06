# Logging

[`<dracon/logging.h>`](../include/dracon/logging.h) is a thin layer over
Boost.Log. A component declares one tagged logger and writes through the
severity macros:

```cpp
#include <dracon/logging.h>

namespace {
tagged_logger<> logger{"customers"};
}

INFO(logger) << "serving " << req.url();
```

The tag is what tells the lines of one plugin apart in the server log. Where
they go, and how they are formatted and filtered, is set in
`server_logging.conf` and not in the plugin.

## The macros

| | |
|---|---|
| `TRACE(logger)` | Compiled out unless the build sets `ENABLE_TRACE_LOG` |
| `DEBUG(logger)` | Compiled out unless the build sets `ENABLE_DEBUG_LOG` |
| `INFO(logger)` | |
| `WARNING(logger)` | |
| `ERROR(logger)` | |
| `FATAL(logger)` | Writes the record and nothing else. Shutting down is up to you |

Each of them prepends the enclosing function. When `TRACE()` and `DEBUG()` are
compiled out they become a `dummy_stream` which swallows everything written to
it, so their arguments are never evaluated and an expensive trace costs nothing
in a normal build.

## tagged_logger

`tagged_logger<>` is the multi threaded logger, which is what a plugin wants:
handlers run on every event loop thread and normally share one logger.
`tagged_logger<severity_logger>` drops the locking, for a logger only ever
written to from one thread.

The tag is held as a `boost::string_view`, so it has to outlive the logger. A
string literal is the usual choice.

## What not to log

The log is not a place for what the response already carries, and it is a
place for what the response deliberately does not:

```cpp
catch (const std::exception &e) {
    ERROR(logger) << "loading " << path << " failed: " << e.what();
    throw dracon::response{500};
}
```

An exception which escapes a session becomes the body of the response, so the
detail belongs in the log and a status code in the answer. See [Reporting
errors](errors.md).

Logging on every request costs a lock and a formatted line per request. At the
connection rates this server is built for that is measurable, so `INFO()` on a
hot path is worth a second thought, and `DEBUG()` is free.

## Configuring the sinks

`server_logging.conf` is fed to Boost.Log's `init_from_settings`, so it takes
whatever that takes: sinks, formats and filters, including a filter on the
`Tag` attribute. It is included from the `logging` section of `server.conf`.
