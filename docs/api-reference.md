# API reference

The whole public API is header only, under
[`include/dracon/`](../include/dracon), in namespace `Dracon`. A plugin links
the `dracon::dracon` CMake target and depends on nothing else of Dracon's.

Types under `dracon::detail` are implementation and can change without notice.

- [plugin.h](#pluginh)
- [http.h](#httph)
- [stream.h](#streamh)
- [restful.h](#restfulh)
- [thread_worker.h](#threadworkerh)
- [exceptions.h](#exceptionsh)
- [logging.h](#loggingh)
- [utils.h](#utilsh)

## plugin.h

The plugin contract. See [Writing a plugin](writing-a-plugin.md).

| | |
|---|---|
| `PLUGIN_EXPORT` | `extern "C"` plus default visibility. Every entry point needs it |
| `http_session` | `std::function<void(abstract_stream &, request &)>`, the callable which answers one request |
| `init_plugin_type` | `bool (*)(const std::string &conf_dir)` |
| `plugin_order_type` | `uint32_t (*)()` |
| `create_session_type` | `http_session (*)(const request &)` |
| `destory_plugin_type` | `void (*)()`. The spelling is a typo which is now part of the ABI |

## http.h

### fields

`std::unordered_map<std::string, std::string>`, the headers. Both `request` and
`response` derive from it.

request header names are canonicalized while parsing, so `content-length`
arrives as `Content-Length` and is looked up that way. It is a map, so a header
which was sent twice is in there once, with the value which arrived last.

### chunked_data

`std::numeric_limits<size_t>::max()`. A body whose length is not known:
`response::set_content_length(chunked_data)` picks chunked encoding, and
`request::content_length()` returns it for a request which arrived chunked.

### request

| | |
|---|---|
| `state()` | How far the parser got, see `request::state` below |
| `url()` | The request target as it was sent, escapes and query string included |
| `method()` | The method, as it was sent |
| `keep_alive()` | Whether the client asked for the connection to be reused |
| `content_length()` | The announced body length, or `chunked_data` when there is no usable `Content-Length` |
| `append_body_callback(cb, max_size)` | Says a body is expected and where it goes. call it before reading |
| `max_body_size()` | The limit `append_body_callback()` set, 0 when no body was asked for |
| `append_body(view)` | Called by the server while parsing. Throws a 400 response when no callback was registered |

`body_callback` is `std::function<void(std::string_view)>`. It is called with the
pieces the socket and the chunked decoder produced, and the view is only valid
for the duration of the call.

`request::state` runs `uninitialized`, `processing_url`, `processing_header`,
`headers_completed`, `processing_body`, `completed`. A session is created at
`headers_completed`, or at `completed` when the request has no body.

`set_state()`, `set_url()`, `set_method()` and `set_keep_alive()` exist for the
parser. A session has no reason to call them.

### response

Constructed as `response{status_code = 500, body = {}, fields = {}}`. The
setters chain.

| | |
|---|---|
| `set_status_code(uint16_t)` / `status_code()` | An unknown code is sent as a 500 |
| `set_body(std::string)` / `body()` | Also sets the content length |
| `set_content_length(size_t)` / `content_length()` | Announces a body the session writes itself, and drops the one it held. `chunked_data` announces chunked encoding |
| `set_keep_alive(seconds)` / `keep_alive()` | Overrides the connection reuse for this response. 0 closes it, and the default of -1 leaves the server's own value |
| `to_string(keep_alive_override)` | Serializes it. Writing the response to a stream does this |

`Content-Length`, `Transfer-Encoding`, `Connection` and `Keep-Alive` are
computed and cannot be set as headers. Every other header goes out with CR, LF
and NUL replaced by a space. An informational (1xx) or 204 response is
serialized without a body and without the connection headers.

### Operators and literals

| | |
|---|---|
| `stream << response` | Serializes and sends it, applies its keep alive, and stretches the session timeout to fit the body |
| `stream >> request` | Reads the rest of the request, answers an `Expect: 100-continue` |
| `literals::operator""_http` | `204_http` is a bodyless `response{204}` |

## stream.h

### const_buffer

A pointer and a length, laid out exactly like an `iovec`. Implicitly
constructed from `std::string_view` and `std::string`. It does not own the
memory, so what it points at has to outlive the write.

### abstract_stream

The connection a session is given. Reads and writes yield to the event loop and
resume when the socket is ready; they throw a `std::system_error` when the
connection dies.

| | |
|---|---|
| `read(request &)` | Reads the rest of a request. `stream >> req` is the usual spelling |
| `write(const_buffer)` | Writes a buffer |
| `write(std::span<const const_buffer>)` | Writes several, in one syscall where the socket allows it |
| `yield()` | Suspends the session. Returns an empty error code when it was woken up, and the reason otherwise |
| `wakeupper()` | The handle another thread uses to resume a session sitting in `yield()` |
| `keep_alive()` / `set_keep_alive(seconds)` | How long the connection stays open after the response |
| `session_timeout()` / `set_session_timeout(seconds)` | The deadline of the whole session, counted from now |
| `peer_address()` | The numeric address of the peer on the socket |
| `is_secured_connection()` | True on the https port |
| `socket_read_size()` / `set_socket_read_size(int)` | `SO_RCVBUF` |
| `socket_write_size()` / `set_socket_write_size(int)` | `SO_SNDBUF` |

`abstract_wakeupper` is the nested interface `wakeupper()` returns, with one
method, `wakeup()`. Keep the `shared_ptr` alive for as long as another thread
may still call it; waking a session which has already ended is safe.

### next_layer_stream

Forwards every call to the stream underneath. Derive from it to change how the
body is written, overriding only what the layer does. The next layer is held by
reference and has to outlive the wrapper.

### chunked_stream

Frames every write as a chunk and writes the terminating chunk when it is
destroyed. Empty writes are dropped. An exception thrown in its scope still
emits the terminating chunk, so a truncated body can look complete to the
client.

### ostream_buffer

A `std::streambuf` over a stream, for formatted output. Buffers up to the
socket send buffer size. Its destructor flushes but swallows a failing write;
call `pubsync()` where that matters.

### Constants

| | |
|---|---|
| `end_of_chunked_stream` | `"0\r\n\r\n"`, the terminating chunk |
| `end_of_chuncked_stream` | Deprecated, the old misspelling |
| `crlf_string` | `"\r\n"` |

## restful.h

See [Routing a REST API](routing.md).

| | |
|---|---|
| `restful_router<ReturnType = http_session, Args...>` | `create_route(route)`, `create_handler(url, method, args...)`. `ReturnType` defaults to `http_session`, so a plugin writes plain `restful_router` and, where the arguments cannot be deduced, `restful_router<>` |
| `restful_route<ReturnType, Args...>` | `add_method_handler(method, handler)`. Created by `create_route()`, never on its own |
| `parsed_route` | `captures`, `queries`, `all_but_options_node_methods` |
| `captured_resources` | `std::vector<std::string>`, the captures in route order |
| `captured_resource(captures, i)` | Reads one without going out of bounds |
| `query_strings` | `std::vector<std::pair<std::string, std::string>>`, decoded, in URL order |
| `restful_route_method_handler<ReturnType, Args...>` | What a route stores per method. Only written out by hand for a router which returns something other than an `http_session` |
| `restfull_route`, `restful_router_type`, `res_tful_router_type`, `res_tful_route_method_handler` | Deprecated spellings, kept for one release |

`create_handler()` returns `{}` when nothing matched, throws a 405 with an
`Allow` header when the path matched but the method did not, and a 400 on a
malformed query string.

`parsed_route::capturedResources` and `parsed_route::queryStrings` were renamed to
`captures` and `queries`. A data member cannot be aliased, so unlike the
spellings above this one has no deprecation path: it is a compile error in a
plugin which used them, fixed by the rename.

## thread_worker.h

### thread_worker

A thread pool and a task queue, for work a session must not do itself.

| | |
|---|---|
| `thread_worker(workers = 1)` | Starts the threads |
| `insert_task(task)` | Queues it and returns straight away |

A task which throws is dropped and the pool moves on, so a failure has to be
recorded by the task. The destructor asks the threads to quit and joins them
without draining the queue, so tasks still waiting are never run.

The waiting pattern is in [Threads and slow work](concurrency.md).

## exceptions.h

`segmentation_fault_error` and `floating_point_error`, both `std::runtime_error`.
The server throws them in place of a SIGSEGV and a SIGFPE raised inside a
handler, which needs the plugin to be compiled with `-fnon-call-exceptions`.
Like any exception leaving a handler, `what()` is sent to the client.

## logging.h

See [Logging](logging.md).

| | |
|---|---|
| `tagged_logger<T = severity_logger_mt>` | A logger which stamps every record with a `Tag` attribute |
| `severity_logger_mt` / `severity_logger` | The Boost.Log loggers behind it, with and without locking |
| `TRACE` `DEBUG` `INFO` `WARNING` `ERROR` `FATAL` | The severity macros. The first two are compiled out unless `ENABLE_TRACE_LOG` / `ENABLE_DEBUG_LOG` |
| `LOG` | `BOOST_LOG_SEV`, for an explicit severity |
| `dummy_stream` | What `TRACE()` and `DEBUG()` become when compiled out |

These are not in a namespace.

## utils.h

Odds and ends the server uses and plugins may as well.

| | |
|---|---|
| `split(str, ch, count = npos)` | Splits on a character, dropping empty pieces, returning views into `str`. `count` limits how many times it splits, so what is left after the last one comes back whole |
| `split_vector` | `std::vector<std::string_view>`, what `split()` returns |
| `unescape_url(view)` | Decodes percent escapes and turns `+` into a space. Throws `std::invalid_argument` on a malformed escape |
| `from_hex(char)` | One hexadecimal digit to its value. Throws `std::invalid_argument` otherwise |
| `address_text(const sockaddr_storage &)` | The numeric form of an address, empty for a family which is neither AF_INET nor AF_INET6 |
| `lru_cache<K, V>` | Fixed size cache, drops the least recently used. Not thread safe, and a read reorders it |
| `buffer<T>` / `char_buffer` | A fixed capacity array with a read cursor, which is what sockets are read into |
| `spin_lock` | A Lockable mutex which spins. Only for a critical section a few instructions long which can never be suspended |
| `simple_timer` | Calls a function on a thread of its own until it is destroyed |

`simple_timer` is one thread per timer, which is right for the handful a plugin
needs and wrong for anything per connection. Its callback runs off the event
loops, so what it touches needs the same locking a handler would take.
