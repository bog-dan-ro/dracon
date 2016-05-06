# Reporting errors

A session reports a failure by throwing. The server catches it and turns it
into a response, as long as nothing has been written to the socket yet.

| Thrown | Sent |
|---|---|
| `throw 404;` | `404 Not Found`, empty body |
| `throw dracon::response{403, "no", {{"K","V"}}};` | Exactly that status, those headers and that body |
| `throw std::runtime_error{"boom"};` | `500`, **with `what()` as the body** |
| `throw someErrorCode;` | `500`, with `ec.message()` as the body |
| anything else | `500`, empty body |

An `int` outside the range of a status code is sent as a 500.

## Exception messages reach the client

`what()` becoming the body is convenient while developing and is a leak the
rest of the time. An exception message which carries a file system path, an
`errno` string, a query, or a host name hands those internals to whoever made
the request, and most of the messages the standard library and third party
libraries produce carry exactly that.

Throw a `dracon::response` with a body you chose, and log the details:

```cpp
try {
    load(path);
} catch (const std::exception &e) {
    ERROR(logger) << "loading " << path << " failed: " << e.what();
    throw dracon::response{500};
}
```

The bundled `staticContent` plugin answers a bare 404 for every filesystem
failure for the same reason. Telling "no such file" from "outside the root"
would turn it into a way to probe the filesystem.

## Once something has been written

The server can only replace a response it has not started sending. After the
head, or a chunk, has gone out, an exception is logged and the connection is
closed, and the client sees a truncated response. That is worth keeping in mind
when a body is produced as it goes: anything which can fail is better done
before the head is written.

A `dracon::chunked_stream` complicates this, because its destructor writes the
terminating chunk while the exception unwinds and tells the client the body is
complete. See [Streams](streams.md).

## Signals

A null pointer dereference (SIGSEGV) and an integer division by zero (SIGFPE)
inside a handler are converted into `dracon::segmentation_fault_error` and
`dracon::floating_point_error`, so one broken handler answers a 500 instead of
taking the server down with it.

That conversion is why a plugin must be compiled with `-fnon-call-exceptions`,
which linking `dracon::dracon` takes care of. Their messages are deliberately
generic; the stack trace goes to the server log.

This is a way to survive a bug, not a way to handle one. The session which
tripped it is unwound from the middle of whatever it was doing, and any lock it
held is released by unwinding but whatever it was protecting is in whatever
state it was left in.

## Errors which are not the session's fault

A connection which dies, or a session which runs out of time, surfaces as a
`std::error_code` returned from `stream.yield()` or thrown by a read or a
write. There is nothing left to answer at that point:

```cpp
if (auto ec = stream.yield())
    throw ec;
```

Throwing it unwinds the session so that its destructors run, and the server
logs it and closes the connection.

## Before the session

Errors the parser finds are answered before any plugin sees the request: 400
for a malformed request, 413 for a body over the limit the session allowed, 431
for headers which are too large, 501 for an unsupported transfer encoding, and
505 for an unsupported HTTP version.

The server answers 503 when no plugin claims a URL, and the
[router](routing.md) throws a 405 with an `Allow` header when a URL matches a
route but not with that method.
