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

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

#include <dracon/utils.h>

namespace dracon::internal {

class basic_server_session;

using clock = std::chrono::high_resolution_clock;
using time_point = std::chrono::time_point<clock>;

/*!
 * \brief The sessions_event_loop class
 *
 * This class is used by the server to handle the sessions (sock & timer) events.
 * Usually the server creates one event loop for every CPU core on the system.
 */
class sessions_event_loop
{
public:
    sessions_event_loop();
    ~sessions_event_loop();

    void register_session(basic_server_session *session, uint32_t events);
    void update_session(basic_server_session *session, uint32_t events);
    void unregister_session(basic_server_session *session);

    void delete_later(basic_server_session *session) noexcept;

    /*!
     * \brief wakeup_session
     *
     * Asks the loop to resume \a session. Callable from any thread; the session
     * pointer is queued and the event fd is only used as a "something to do"
     * signal (an event fd counter *sums* the written values, so it cannot carry
     * the pointer itself).
     */
    void wakeup_session(basic_server_session *session) noexcept;

    /*!
     * \brief notify_deadline
     *
     * Publishes a session deadline to the loop, so it knows when the next
     * timeout scan is due. Callable from any thread.
     */
    void notify_deadline(time_point deadline) noexcept;

    inline uint32_t active_sessions() const noexcept { return m_active_sessions.load(); }
    void shutdown() noexcept;

    dracon::char_buffer shared_read_buffer;
    std::shared_ptr<dracon::char_buffer> shared_write_buffer(size_t size) const;
    void set_workload_balancing(bool on);

private:
    void loop();

private:
    std::shared_ptr<dracon::char_buffer> m_shared_write_buffer;
    int m_epoll_handler;
    bool m_workload_balancing = false;
    int m_event_fd;
    std::atomic<uint32_t> m_active_sessions{0};
    std::atomic_bool m_quit{false};
    std::thread m_loop_thread;
    std::mutex m_sessions_mutex;
    std::set<basic_server_session *> m_sessions;
    dracon::spin_lock m_delete_later_mutex;
    std::unordered_set<basic_server_session *> m_delete_later_objects;
    dracon::spin_lock m_wakeup_mutex;
    std::unordered_set<basic_server_session *> m_wakeup_sessions;
    std::atomic<time_point::rep> m_earliest_deadline{time_point::max().time_since_epoch().count()};
};

} // namespace dracon::internal
