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
*/

#include "server_service_sessions.h"
#include "server.h"
#include "server_logger.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>

#include <dracon/http.h>

using namespace std::chrono_literals;
using namespace std::string_view_literals;
namespace server_sessions {


/*!
 * \brief write_response
 *
 * Answers /server_status.
 *
 * The endpoint is meant to be cheap and to give away nothing which could be
 * turned against the server, so:
 *   * it only reads counters (all of them lock free), it never walks the
 *     session containers nor takes a lock the accept path needs, so it can be
 *     polled as fast as the client likes without slowing the server down;
 *   * the body is only those counters. No paths, no versions, no addresses;
 *   * error paths answer a bare status code: an exception message may carry
 *     internals (a path, an errno string), and this endpoint has no
 *     authentication, so nothing but the status code goes out.
 */
static void write_response(dracon::abstract_stream &stream, dracon::request &req)
{
    bool can_write_error = true;
    try {
        stream >> req;

        const auto &server = dracon::internal::server::instance();
        const auto active_sessions = server.active_sessions();
        // the peak_sessions is updated slowly
        const auto peak = std::max(server.peak_sessions(), active_sessions);
        const auto served_sessions = server.served_sessions();

        auto seconds = server.uptime().count();
        const auto days = seconds / (60 * 60 * 24);
        seconds  -= days * (60 * 60 * 24);
        const auto hours = seconds / (60 * 60);
        seconds  -= hours * (60 * 60);
        const auto minutes = seconds / 60;
        seconds  -= minutes * 60;

        std::string body;
        body.reserve(160);
        std::format_to(std::back_inserter(body),
                       "Active sessions: {}\n"
                       "Sessions peak: {}\n"
                       "Uptime: {} days, {} hours, {} minutes and {} seconds\n"
                       "Served sessions: {}\n",
                       active_sessions, peak, days, hours, minutes, seconds, served_sessions);

        can_write_error = false;
        stream << dracon::response{200, body, {{"Refresh", "5"}, {"Content-Type", "text/plain"}}};
    } catch (const dracon::response &res) {
        if (can_write_error)
            stream << dracon::response{res.status_code()};
        WARNING(dracon::internal::server_logger) << res.status_code() << " " << res.body();
    } catch (const std::error_code &ec) {
        if (can_write_error)
            stream << dracon::response{500};
        WARNING(dracon::internal::server_logger) << ec.message();
    } catch (const std::exception &e) {
        if (can_write_error)
            stream << dracon::response{500};
        WARNING(dracon::internal::server_logger) << e.what();
    } catch (...) {
        if (can_write_error)
            stream << dracon::response{500};
        WARNING(dracon::internal::server_logger) << "Unknown error";
    }
}

dracon::http_session create_session(const dracon::request &req)
{
    if (req.url() == "/server_status"sv && req.method() == "GET"sv)
        return &server_sessions::write_response;
    return {};
}

} // namespace server_sessions
