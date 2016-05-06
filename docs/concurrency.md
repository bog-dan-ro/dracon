# Threads and slow work

## What runs where

The main thread accepts connections and does nothing else with them. It
enforces `max_connections_per_ip` and hands each new socket to the event loop
which has the fewest sessions on it.

There is one event loop thread per hardware thread by default, and
`--workers` changes that. Every session on a loop shares that one thread: it
runs inside a coroutine, and every read and write which would block yields back
to the loop, which resumes the session when the socket is ready.

So handlers of the same plugin run concurrently on every event loop thread, and
several sessions on one thread take turns inside it. Two consequences follow,
and between them they account for most of the ways a plugin goes wrong under
load.

## A handler must not block

A handler which blocks does not block one connection, it blocks its event loop
thread and every other session on it. That covers a synchronous database call,
a file read which misses the page cache, an HTTP request to another service,
`sleep`, and a mutex held by any of those.

Hand the work to another thread, take the wakeupper of the stream, and wait in
`yield()` until the work wakes the session up:

```cpp
dracon::thread_worker s_worker{10};   // plugin wide

return [](dracon::abstract_stream &stream, dracon::request &req) {
    stream >> req;

    auto wakeupper = stream.wakeupper();
    auto done = std::make_shared<std::atomic_bool>(false);
    auto result = std::make_shared<std::string>();

    s_worker.insert_task([=] {
        *result = slowThing();
        done->store(true);
        wakeupper->wakeup();
    });

    while (!done->load()) {
        if (auto ec = stream.yield())
            throw ec;       // the client is gone, or the session timed out
    }

    stream << dracon::response{200, *result};
};
```

Three details in there matter.

The loop around `yield()`. It can return for a reason other than this task, so
one call is not enough, and a non zero error code means the session is over and
has nothing left to answer with.

The `shared_ptr`s. The task may still be running when the session is gone, so
it must not capture anything belonging to the session by reference. Capturing
the wakeupper by value keeps the handle alive, and waking a session which has
already ended is safe.

`insert_task()` returns immediately. It queues the task and returns, which is
what makes it safe to call from a session in the first place.

`dracon::thread_worker` is a thread pool and a task queue, and is described in
[the API reference](api-reference.md#threadworkerh). Its threads are yours to
size: they mostly wait, so size the pool after what the tasks wait on rather
than after the cores.

## Never hold a lock across a write

Writing yields. A session which holds a lock while it writes gives the thread
back to the event loop with the lock still held, and the next session on that
thread which wants the lock waits for a connection it has no influence over.
One slow client wedges a whole worker.

Build the response first, release the lock, then write:

```cpp
std::string body;
{
    std::shared_lock lock{s_mutex};
    body = render(s_data);
}
stream << dracon::response{200, body};
```

The same goes for anything else which yields inside a critical section,
including `stream >> req` and `yield()` itself.

`std::shared_mutex` is usually the right lock here, since plugin state is
mostly read. A `dracon::spin_lock` is only worth it for a critical section which
is a handful of instructions long and can never be suspended, which on an event
loop thread rules out anything that touches the stream.

## Shared state

Anything a plugin keeps between requests is touched by every event loop thread
and needs a lock, with two exceptions.

state built in `init_plugin()` and only read afterwards needs none, because it
is finished before the first connection is accepted. A router filled in there
is the usual case.

state a `dracon::thread_worker` task touches needs one even when only that pool
touches it, because the pool has several threads of its own.

`dracon::lru_cache` is not thread safe, and a read reorders it, so a cache
shared between threads needs the lock taken exclusively even to look something
up.

## Timeouts

Every session has a deadline and the connection is closed when it is reached,
including while it sits in `yield()`. A session which waits on slow work should
set a timeout it can live with:

```cpp
stream.set_session_timeout(30s);
```

The server's own guess is `keepalive_timeout` plus a second per 512 KB of the
announced request body, and five minutes when the length is unknown.
