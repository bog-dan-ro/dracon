# Requests and responses

Both live in [`<dracon/http.h>`](../include/dracon/http.h). `request` and
`response` derive from `dracon::fields`, which is an
`unordered_map<std::string, std::string>` of headers, so a header is read and
written like a map entry.

## Reading a request

`create_session()` gets the request with the head parsed. The method and the
URL are there, so are the headers, and none of the body has been read yet.

```cpp
const std::string &method = req.method();   // "GET", as it was sent
const std::string &url    = req.url();      // "/items/42?full=1", escapes and all
```

The URL is the request target exactly as the client wrote it. It is not
unescaped and it is not normalized, so a plugin which turns it into a file path
has to deal with `..` itself, either by rejecting the URL or by checking the
path it resolved to. `dracon::unescape_url()` decodes percent escapes when they
are wanted, and the [router](routing.md) does it for query strings.

### headers

Header names are canonicalized while parsing: `content-length` arrives as
`Content-Length`. Look them up in that spelling.

```cpp
if (auto it = req.find("Content-Type");
    it == req.end() || it->second != "application/json") {
    throw 415;
}
```

`fields` is a map, so a header which was sent twice is in there once, with the
value which arrived last.

### The body

The server never buffers a body. It hands it to the callback the session
registers, in whatever pieces the socket and the chunked decoder produced:

```cpp
std::string body;
req.append_body_callback([&body](std::string_view part) { body += part; },
                       1024 * 1024);
stream >> req;
```

The second argument is the largest body this session accepts. A request whose
`Content-Length` is over it is answered with a 413 before a byte of it is read,
which is the point: the default limit is effectively unbounded, so a session
which appends into a string without passing a limit will happily hold whatever
the client cares to send.

The pieces carry no meaning of their own. A callback which appends is trivially
correct; one which parses has to be able to resume where the previous piece
ended. The `std::string_view` is only valid for the duration of the call.

`stream >> req` reads the rest of the request and leaves it in
`state::completed`. It also answers an `Expect: 100-continue`, with a 100 when
the announced length fits under the limit and with a 417 when it does not, in
which case the client never sends the body.

call it even when the body is of no interest. A body which is left on the
connection cannot be told apart from the next request, so the server closes the
connection rather than reuse it.

### request::content_length()

Returns the announced length, or `dracon::chunked_data` when there is no usable
`Content-Length`, which covers both a chunked request and one with no body at
all.

## Writing a response

```cpp
stream << dracon::response{200, "hello\n", {{"Content-Type", "text/plain"}}};
```

The three arguments are the status code, the body, and the headers. The status
defaults to 500, so a `response` which was built and never filled in cannot be
mistaken for a success. The setters chain and return the response:

```cpp
stream << dracon::response{}
              .set_status_code(201)
              .set_body(json.dump())
              .set_keep_alive(0s);
```

For a bodyless answer there is a literal:

```cpp
using namespace dracon::literals;
stream << 204_http;
```

### What is computed and what is not

`Content-Length`, `Transfer-Encoding`, `Connection` and `Keep-Alive` are worked
out from the body and from the state of the connection. Setting them as headers
does nothing: they are dropped while serializing. Everything else goes out as
it was set, with CR, LF and NUL replaced by a space, so a header value taken
from the request cannot inject a header of its own or a second response.

An informational response (1xx) and a 204 are serialized without a body and
without the connection headers, as the protocol requires.

Writing a response also stretches the session timeout to fit the body: ten
seconds plus one for every 512 KB, so a large answer is not cut off by the
timeout of the request which asked for it.

### Keep alive

The server works out from the request and from `keepalive_timeout` how long the
connection stays open, and a session does not normally have to care.
`response::set_keep_alive()` overrides it for one response, and 0 closes the
connection after it.

### A body which is not in the response

A body which is produced as it goes does not belong in `response`. Announce it
and write it through the stream instead:

```cpp
stream << dracon::response{200}.set_content_length(file.size());
stream.write(file.data());
```

or, when the length is not known when the head goes out, with
`set_content_length(dracon::chunked_data)` and a `dracon::chunked_stream`. See
[Streams](streams.md).

## request::state

The parser advances a request through these:

| state | |
|---|---|
| `uninitialized` | Nothing parsed yet |
| `processing_url` | The request line is still incomplete |
| `processing_header` | The method and URL are known, headers are still arriving |
| `headers_completed` | All the headers are in, the body is not |
| `processing_body` | The body is being handed to the callback |
| `completed` | The whole request has been read |

A session is created at `headers_completed`, or at `completed` when the request
has no body, and `stream >> req` drives it to `completed`.
