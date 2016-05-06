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

#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "server_logger.h"
#include "server_session.h"
#include "sessions_event_loop.h"

using namespace std::chrono_literals;

namespace dracon::internal {

namespace {
const uint32_t events_size = 10000;
}

/*!
 * \brief sessions_event_loop::sessions_event_loop
 *
 * Creates a new EPOLL event loop
 */
namespace {
constexpr unsigned long min_shared_buffer_size = 64ul * 1024;
constexpr unsigned long max_shared_buffer_size = 4ul * 1024 * 1024;
}

static unsigned long read_proc(const char *path)
{
    unsigned long value = max_shared_buffer_size;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%lu", &value) != 1)
            value = max_shared_buffer_size;
        fclose(f);
    }
    return std::clamp(value, min_shared_buffer_size, max_shared_buffer_size);
}

sessions_event_loop::sessions_event_loop()
{
    unsigned long rmem_max = read_proc("/proc/sys/net/core/rmem_max");

    // This buffer is shared by all basic_server_sessions which are server
    // by this event loop to read the incoming data without
    // allocating any memory
    shared_read_buffer.resize(rmem_max);
    m_shared_write_buffer = std::make_shared<dracon::char_buffer>(read_proc("/proc/sys/net/core/wmem_max"));

    m_epoll_handler = epoll_create1(EPOLL_CLOEXEC);
    if (m_epoll_handler == -1)
        throw std::runtime_error{"Can't create epool handler"};

    m_event_fd = eventfd(0, EFD_NONBLOCK);
    epoll_event event;
    event.data.ptr = nullptr;
    event.data.fd = m_event_fd;
    event.events = EPOLLHUP | EPOLLERR | EPOLLIN | EPOLLET;
    if (epoll_ctl(m_epoll_handler, EPOLL_CTL_ADD, m_event_fd, &event))
        throw std::runtime_error{"Can't register the event handler"};

    m_loop_thread = std::thread([this]{loop();});

//    // set insane priority ?
//    sched_param sch;
//    sch.sched_priority = sched_get_priority_max(SCHED_RR);
//    pthread_setschedparam(m_loop_thread.native_handle(), SCHED_RR, &sch);
    TRACE(server_logger) << this << " shared buffer mem_max: " << rmem_max << " eventfd = " << m_event_fd;
}

sessions_event_loop::~sessions_event_loop()
{
    shutdown();
    try {
        // Quit event loop
        m_loop_thread.join();

        // Destroy all registered sessions
        std::unique_lock<std::mutex> lock{m_sessions_mutex};
        auto it = m_sessions.begin();
        while (it != m_sessions.end()) {
            auto session = it++;
            lock.unlock();
            delete (*session);
            lock.lock();
        }
        close(m_event_fd);
    } catch (...) {}
    TRACE(server_logger) << this;
}

/*!
 * \brief sessions_event_loop::register_session
 *
 * Register a new basic_server_session to this even loop
 *
 * \param session to register
 * \param events the events that epoll will listen for
 */
void sessions_event_loop::register_session(basic_server_session *session, uint32_t events)
{
    TRACE(server_logger) << session << " events" << events << active_sessions();
    {
        std::unique_lock<std::mutex> lock{m_sessions_mutex};
        m_sessions.insert(session);
    }
    epoll_event event;
    event.data.ptr = session;
    event.events = events;
    if (epoll_ctl(m_epoll_handler, EPOLL_CTL_ADD, session->sock(), &event))
        throw std::runtime_error{"Can't register session"};
    ++m_active_sessions;
}

/*!
 * \brief sessions_event_loop::update_session
 *
 * Updates a registered basic_server_session
 *
 * \param session to update
 * \param events new epoll events
 */
void sessions_event_loop::update_session(basic_server_session *session, uint32_t events)
{
    TRACE(server_logger) << session << " events:" << events;
    epoll_event event;
    event.data.ptr = session;
    event.events = events;
    if (epoll_ctl(m_epoll_handler, EPOLL_CTL_MOD, session->sock(), &event))
        throw std::runtime_error{"Can't change the session"};
}

/*!
 * \brief sessions_event_loop::unregister_session
 *
 * \param session to unregister
 */
void sessions_event_loop::unregister_session(basic_server_session *session)
{
    TRACE(server_logger) << session << " activeSessions:" << active_sessions();
    {
        std::unique_lock<std::mutex> lock{m_sessions_mutex};
        if (!m_sessions.erase(session))
            return;
    }
    if (epoll_ctl(m_epoll_handler, EPOLL_CTL_DEL, session->sock(), nullptr)) {
        ERROR(server_logger) << "Can't remove " << session << " socket " << session->sock() << "error " << strerror(errno);
        throw std::make_error_code(std::errc(errno));
    }
    --m_active_sessions;
}

/*!
 * \brief sessions_event_loop::delete_later
 *
 * Postpones a session for deletion
 *
 * \param session to delete later
 */
void sessions_event_loop::delete_later(basic_server_session *session) noexcept
{
    if (session->is_deleting())
        return;
    try {
        unregister_session(session);
    } catch (...) {}
    // Idea "stolen" from Qt :)
    std::unique_lock<dracon::spin_lock> lock{m_delete_later_mutex};
    try {
        m_delete_later_objects.insert(session);
    } catch (...) {}
}

void sessions_event_loop::wakeup_session(basic_server_session *session) noexcept
{
    {
        std::unique_lock<dracon::spin_lock> lock{m_wakeup_mutex};
        try {
            m_wakeup_sessions.insert(session);
        } catch (...) {
            return;
        }
    }
    eventfd_write(m_event_fd, 1);
}

void sessions_event_loop::notify_deadline(time_point deadline) noexcept
{
    if (deadline == time_point{})
        return;
    const auto value = deadline.time_since_epoch().count();
    auto current = m_earliest_deadline.load(std::memory_order_relaxed);
    while (value < current &&
           !m_earliest_deadline.compare_exchange_weak(current, value, std::memory_order_relaxed))
        ;
}

void sessions_event_loop::shutdown() noexcept
{
    m_quit.store(true);
    eventfd_write(m_event_fd, 1);
}

std::shared_ptr<dracon::char_buffer> sessions_event_loop::shared_write_buffer(size_t size) const
{
    if (m_loop_thread.get_id() == std::this_thread::get_id() && size <= m_shared_write_buffer->size())
        return m_shared_write_buffer;
    return std::make_shared<dracon::char_buffer>(size);
}

void sessions_event_loop::set_workload_balancing(bool on)
{
    m_workload_balancing = on;
}

/*!
 * \brief sessions_event_loop::shared_read_buffer
 *
 * Quite useful buffer, used by all basic_server_session to temporary read all sock data.
 */

/*!
 * \brief sessions_event_loop::active_sessions
 *
 * \return returns the number of active sessions on this loop
 */


/*!
 * \brief sessions_event_loop::shutdown
 *
 * Asynchronously quits the event loop.
 */


void sessions_event_loop::loop()
{
    using ms = std::chrono::milliseconds;
    auto events = std::make_unique<epoll_event[]>(events_size);
    ms timeout(-1ms); // Initial timeout
    std::vector<basic_server_session *> sessions;
    while (!m_quit) {
        bool wokeup = false;
        TRACE(server_logger) << "timeout = " << timeout.count();
        int triggered_events = epoll_wait(m_epoll_handler, events.get(), events_size, timeout.count());
        if (triggered_events < 0)
            continue;
        if (!m_workload_balancing) {
            for (int i = 0 ; i < triggered_events; ++i) {
                auto &event = events[i];
                if (event.data.fd != m_event_fd)
                    reinterpret_cast<basic_server_session *>(event.data.ptr)->process_events(event.events);
                else
                    wokeup = true;
            }
        } else {
            std::vector<std::pair<basic_server_session *, uint32_t>> session_events;
            session_events.reserve(triggered_events);
            for (int i = 0 ; i < triggered_events; ++i) {
                auto &event = events[i];
                if (event.data.fd != m_event_fd) {
                    auto ptr = reinterpret_cast<basic_server_session *>(event.data.ptr);
                    auto evs = event.events;
                    std::pair<basic_server_session *, uint32_t> ev{ptr, evs};
                    session_events.insert(std::upper_bound(session_events.begin(),
                                                          session_events.end(),
                                                          ev,
                                                          [](auto a, auto b) {
                                                                return a.first->order() < b.first->order();
                                                          }),
                                         ev);
                } else {
                    wokeup = true;
                }
            }
            for (auto event : session_events)
                event.first->process_events(event.second);
        }

        // Resume the sessions which were woken up from another thread
        if (wokeup) {
            uint64_t data;
            while (eventfd_read(m_event_fd, &data) == 0) // drain the signal
                ;
            std::unordered_set<basic_server_session *> wokeup_sessions;
            {
                std::unique_lock<dracon::spin_lock> lock{m_wakeup_mutex};
                wokeup_sessions.swap(m_wakeup_sessions);
            }
            for (auto session : wokeup_sessions) {
                // The wakeupper may outlive its session, only resume the ones
                // which are still registered
                bool registered;
                {
                    std::unique_lock<std::mutex> lock{m_sessions_mutex};
                    registered = m_sessions.find(session) != m_sessions.end();
                }
                if (registered)
                    session->wakeup();
            }
        }

        // Some session(s) have timeout. Scanning every session is O(sessions),
        // so it's done only when the earliest known deadline is due, not on
        // every single event.
        auto now = clock::now();
        if (now >= time_point{clock::duration{m_earliest_deadline.load(std::memory_order_relaxed)}}) {
            m_earliest_deadline.store(time_point::max().time_since_epoch().count(), std::memory_order_relaxed);
            {
                std::unique_lock<std::mutex> lock{m_sessions_mutex};
                sessions.assign(m_sessions.begin(), m_sessions.end());
            } // Allow the server to insert new connections

            auto earliest = time_point::max();
            for (auto session : sessions) {
                auto session_timeout = session->next_timeout();
                if (session_timeout == time_point{})
                    continue;
                if (session_timeout <= now)
                    session->timeout();
                else
                    earliest = std::min(earliest, session_timeout);
            }
            sessions.clear();
            notify_deadline(earliest);
            now = clock::now();
        }
        const time_point earliest{clock::duration{m_earliest_deadline.load(std::memory_order_relaxed)}};
        timeout = earliest == time_point::max()
                      ? -1ms // maximum timeout
                      : std::max(1ms, std::chrono::duration_cast<ms>(earliest - now) + 50ms);

        std::unordered_set<basic_server_session *> delete_later_objects;
        {
            std::unique_lock<dracon::spin_lock> lock{m_delete_later_mutex};
            delete_later_objects.swap(m_delete_later_objects);
        }
        for (auto obj : delete_later_objects)
            delete obj;
    }
}

} // namespace dracon::internal
