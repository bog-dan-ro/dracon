# Streams

A session gets a `dracon::abstract_stream &`, which is the connection.
Everything it does with the socket goes through it, and every read and write
which would block yields back to the event loop and resumes when the socket is
ready again. That is why a handler can be written as sequential code.

The interface is in [`<dracon/stream.h>`](../include/dracon/stream.h).

## Writing

```cpp
stream.write("some bytes");
stream << "some bytes";      // the same thing
```

`std::string` and `std::string_view` convert to `dracon::const_buffer`, which is
a pointer and a length laid out exactly like an `iovec`. Nothing is copied, so
whatever is written has to stay alive until the call returns.

Several buffers can go out in one call, and in one `writev()` where the socket
allows it:

```cpp
const dracon::const_buffer parts[]{head, body};
stream.write(parts);
```

The parameter is a `std::span`, so an array, a `std::vector` or any contiguous
range works and nothing is allocated to make the call.

A write throws a `std::system_error` when the connection dies, which the
session should let escape.

## chunked responses

When the length of the body is not known when the head goes out, announce
chunked encoding and write through a `dracon::chunked_stream`. It frames every
write and emits the terminating chunk when it is destroyed:

```cpp
stream << dracon::response{200, {}, {{"Content-Type", "text/plain"}}}
              .set_content_length(dracon::chunked_data);
{
    dracon::chunked_stream chunked{stream};
    for (const auto &row : rows)
        chunked << row;
}   // the terminating chunk goes out here
```

Empty writes are dropped, since a zero sized chunk is what ends a chunked body.

An exception thrown inside that scope still runs the destructor, so the
terminating chunk still goes out and the client is told the response is
complete when it is not. Where a truncated answer must not look complete, let
the exception escape the scope of the `chunked_stream` before it is caught, or
close the connection.

## Formatted output

`dracon::ostream_buffer` is a `std::streambuf` over a stream, for a body which
is easier to produce with the standard formatting machinery:

```cpp
dracon::ostream_buffer buffer{stream};
std::ostream out{&buffer};
out << "took " << std::fixed << std::setprecision(2) << seconds << "s\n";
```

Output is collected until it fills the socket send buffer, so a formatted body
costs about as many writes as the same body written by hand. The destructor
flushes what is left but swallows the error if that write fails; call
`pubsync()` where the failure has to be noticed.

It writes straight to the stream it was given, so handing it a `chunked_stream`
makes each flush one chunk:

```cpp
stream << dracon::response{200}.set_content_length(dracon::chunked_data);
dracon::chunked_stream chunked{stream};
dracon::ostream_buffer buffer{chunked};
std::ostream out{&buffer};
out << "customer_id: " << customer_id << "\n";
```

## Layering your own

`dracon::next_layer_stream` forwards every call to the stream underneath.
Deriving from it and overriding only `write()` is how `chunked_stream` is
written, and how a compressing or a counting layer would be. The next layer is
held by reference and has to outlive the wrapper.

## Timeouts

Every session has a deadline, and the connection is closed when it is reached,
whether the session is reading, writing or waiting.

The server sets one it can work out: `headers_timeout` while the head is
arriving, then `keepalive_timeout` plus a second for every 512 KB of the
announced request body, and writing a response stretches it to fit the response
in the same way. A request with no usable `Content-Length`, which means a
chunked one, gets five minutes and a note that the session should know better:

```cpp
stream.set_session_timeout(30s);
```

`set_session_timeout()` counts from the moment it is called, so a long session
can push its deadline out as it makes progress. `session_timeout()` reads back
what is left.

## Connection details

```cpp
stream.peer_address()          // "10.0.0.7" or "::ffff:10.0.0.7"
stream.is_secured_connection()  // true on the https port
stream.socket_read_size()       // SO_RCVBUF, and set_socket_read_size() to change it
stream.socket_write_size()      // SO_SNDBUF, and set_socket_write_size()
```

`peer_address()` is the peer on the socket. Behind a proxy that is the proxy,
and the client is whatever the proxy put in a header.

## yield and wakeupper

`yield()` suspends the session and gives the thread back to the event loop.
`wakeupper()` returns the handle another thread uses to resume it. Together
they are how a session waits for something slow without blocking the loop; the
pattern is in [Threads and slow work](concurrency.md).
