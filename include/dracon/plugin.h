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

#include <cstdint>
#include <functional>
#include <string>

namespace dracon {
class abstract_stream;
class request;

/// Marks a symbol as part of the plugin ABI. The server looks the plugin
/// entry points up with dlsym(), so they must keep C linkage and default
/// visibility.
#define PLUGIN_EXPORT extern "C" __attribute__ ((visibility("default")))

/*!
  A plugin is a shared object in the server's plugins directory which exports
  the entry points below as PLUGIN_EXPORT functions:

  \code
    PLUGIN_EXPORT bool init_plugin(const std::string &conf_dir)
    {
        // Called once, right after the plugin is loaded. Read the
        // configuration from conf_dir + "/<plugin name>.conf" here and return
        // false to abort loading.
        return true;
    }

    PLUGIN_EXPORT uint32_t plugin_order()
    {
        // The order in which the server asks the plugins to handle a request.
        // Lower runs first.
        return 0;
    }

    PLUGIN_EXPORT dracon::http_session create_session(const dracon::request &req)
    {
        // Called for every request, in plugin order, until a plugin claims it.
        // Return the session which will answer the request, or {} to let the
        // next plugin have a look.
        return {};
    }

    PLUGIN_EXPORT void destory_plugin()
    {
        // Called while the server shuts down. Finish the clean up before
        // returning, the library is unloaded right after.
    }
  \endcode

  Only create_session() and plugin_order() are required, init_plugin() and
  destory_plugin() are called only if the plugin exports them. The misspelled
  destory_plugin is what the server looks for and cannot be corrected without
  breaking every plugin built so far.

  Plugins are loaded with RTLD_DEEPBIND, and must be built with
  -fnon-call-exceptions and linked with -Wl,--no-undefined. Linking the
  dracon::dracon CMake target adds the first one; the second is worth adding by
  hand, so that a plugin with an unresolved symbol fails to link rather than
  failing to load.


  WRITING A SESSION

  create_session() only routes: it gets the request with its headers parsed
  (request::parse_state::headers_completed, or completed when the request has no body)
  and returns the callable which does the work. Header names are
  canonicalized, so look them up as req["Content-Type"], never lowercase.

  The session itself runs on an event loop thread, alongside many others:

  \code
    return [](dracon::abstract_stream &stream, dracon::request &req) {
        std::string body;
        req.append_body_callback([&body](std::string_view part) { body += part; },
                               64 * 1024);
        stream >> req;                              // read the rest of the request
        stream << dracon::response{200, "done\n"};  // answer
    };
  \endcode

  Register the body callback before reading: a request which carries a body
  the session never asked for is answered with a 400, and one larger than the
  callback's limit with a 413. "stream >> req" also takes care of an
  Expect: 100-continue.


  ERROR HANDLING

  A session handler reports a failure by throwing. The server turns it into a
  response, as long as nothing was written to the socket yet:

    throw 404;                          -> "404 Not Found", empty body
    throw dracon::response{403, "no",   -> that exact status, headers and body
                           {{"K","V"}}};
    throw std::runtime_error{"boom"};   -> "500", and *what() becomes the
                                           response body*
    anything else                       -> "500", empty body

  !!! The message of a std::exception is sent to the client. It is convenient
  while developing, but it also means an exception message which carries
  internals - a file system path, an errno string, a query, a host name - hands
  those internals to whoever made the request. Throw a dracon::response with a
  body you chose, and log the details instead:

    catch (const std::exception &e) {
        ERROR(logger) << "loading " << path << " failed: " << e.what();
        throw dracon::response{500};
    }

  Once the handler has written anything (a response header, a chunk), the
  server can no longer replace it: the exception is logged and the connection
  is closed, the client sees a truncated response.

  A null pointer dereference (SIGSEGV) and an integer division by zero (SIGFPE)
  inside a handler are converted into dracon::segmentation_fault_error and
  dracon::floating_point_error. Their messages are deliberately generic; the
  stack trace goes to the server log, never to the client.

  A handler which returns without reading the request out (no "stream >> req")
  leaves the unread body on the connection. The server can't tell that body
  from the next request, so it closes the connection instead of reusing it.


  THREADS

  Handlers of the same plugin run concurrently on all the event loop threads,
  so anything they share needs a lock. Never hold that lock across a write:
  writing yields back to the event loop, and until the handler is resumed
  every other session on that thread which wants the lock is stuck behind a
  connection it cannot influence. Build the response first, release the lock,
  then write.

  For the same reason a handler must never block. Hand slow work to another
  thread (dracon::thread_worker, or your own), keep stream.wakeupper() alive
  for it, and wait in a loop on stream.yield() until that thread calls
  wakeup(). yield() returns a non zero error code when the connection died in
  the meantime, which the handler should throw.
*/

/*!
 * \brief The init_plugin() entry point.
 *
 * Called once with the server's configuration directory, right after the
 * plugin is loaded. Returning false aborts the loading.
 */
using init_plugin_type = bool (*)(const std::string &);

/*!
 * \brief The plugin_order() entry point.
 *
 * Returns the position of the plugin in the chain create_session() is called
 * along. Lower runs first, and the bundled staticContent plugin sits at
 * UINT32_MAX so that it always comes last.
 */
using plugin_order_type = uint32_t (*)();

/*!
 * \brief A session, the callable which answers one request.
 *
 * It is invoked on an event loop thread with the stream of the connection and
 * the request create_session() was handed. Reads and writes on the stream
 * yield to the event loop, so the body reads as ordinary sequential code.
 */
using http_session = std::function<void(dracon::abstract_stream&, dracon::request&)>;

/*!
 * \brief The create_session() entry point.
 *
 * Called for every request with its headers parsed, in plugin order, until
 * one of the plugins returns a session. A plugin which does not handle the
 * request returns {}, and the server answers 503 when no plugin claims it.
 */
using create_session_type = http_session (*)(const dracon::request&);

/*!
 * \brief The destory_plugin() entry point.
 *
 * Called while the server shuts down, before the library is unloaded. The
 * spelling is a typo which is now part of the ABI.
 */
using destory_plugin_type = void (*)();

} // namespace Dracon
