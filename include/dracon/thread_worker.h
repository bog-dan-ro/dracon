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

#include <atomic>
#include <condition_variable>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

namespace dracon {

/*!
 * \brief The thread_worker class
 *
 * A pool of threads and a queue of tasks, for the work a session must not do
 * itself. Anything which blocks (a database round trip, a file read which
 * misses the page cache, a call to another service) stalls every other session
 * on the same event loop thread until it returns, so it belongs here instead.
 *
 * The session hands the work over, takes the wakeupper of its stream, and
 * sleeps in yield() until the task wakes it:
 * \code
 *     dracon::thread_worker worker{4};   // usually a plugin wide object
 *
 *     bool done = false;
 *     std::string result;
 *     worker.insert_task([&, wakeupper = stream.wakeupper()] {
 *         result = slow_thing();
 *         done = true;
 *         wakeupper->wakeup();
 *     });
 *     while (!done) {
 *         if (auto ec = stream.yield())
 *             throw ec;   // the client is gone, or the session timed out
 *     }
 *     stream << dracon::response{200, result};
 * \endcode
 *
 * The loop matters: yield() may return for another reason than this task, and
 * a non zero error code means the session is over.
 *
 * The session can also be gone while the task is still running, so a task must
 * not capture anything belonging to it by reference without keeping it alive.
 * Capturing the wakeupper by value does that for the stream; the result above
 * stays alive only because the loop cannot be left while the task runs.
 *
 * A task which throws is dropped, and the worker moves on to the next one, so
 * the failure has to be recorded by the task itself. The destructor asks the
 * threads to quit and joins them, but it does not drain the queue: tasks which
 * were still waiting are never run.
 */
class thread_worker
{
public:
    /*!
     * \brief thread_worker
     *
     * \param workers how many threads run the tasks, at least one. Size it
     * after what the tasks wait on, not after the cores: threads which sit in
     * a socket read cost little, and a pool which is too small turns into a
     * queue the sessions wait in.
     */
    thread_worker(uint32_t workers = 1)
    {
        workers = std::max(uint32_t(1), workers);
        m_quit.store(false);
        m_workers.reserve(workers);
        for (uint32_t i = 0; i < workers; ++i)
            m_workers.emplace_back([this]{
                while (!m_quit) {
                    try {
                        if (auto task = next_task())
                            task();
                    } catch (...) {}
                }
            });
    }

    ~thread_worker()
    {
        try {
            m_quit.store(true);
            m_wait_condition.notify_all();
            for (auto & m_worker : m_workers) {
                if (m_worker.joinable())
                    m_worker.join();
            }
        } catch (...) {}
    }

    /*!
     * \brief insert_task
     *
     * Queues \a task, to be run on one of the worker threads as soon as one
     * is free. Returns straight away, so it is safe to call from a session.
     *
     * \param task to execute
     */
    void insert_task(const std::function<void()> &task)
    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_pending_tasks.push(task);
        m_wait_condition.notify_one();
    }

private:
    /*!
     * \brief next_task
     *
     * Waits until a new task is available and then returns it to any available worker thread
     *
     * \return the next task to run
     */
    inline std::function<void()> next_task()
    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_wait_condition.wait(lock, [this]{return m_quit || !m_pending_tasks.empty();});
        if (m_quit)
            return {};
        auto ret = m_pending_tasks.front();
        m_pending_tasks.pop();
        return ret;
    }

private:
    std::atomic_bool m_quit{false};
    std::queue<std::function<void()>> m_pending_tasks;
    std::mutex m_lock;
    std::condition_variable m_wait_condition;
    std::vector<std::thread> m_workers;
};

} // namespace Dracon
