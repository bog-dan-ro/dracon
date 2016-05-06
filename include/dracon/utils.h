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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dracon {

/*!
 * \brief The spin_lock class
 *
 * A mutex which spins instead of going to sleep. It meets the Lockable
 * requirements, so std::lock_guard, std::unique_lock, std::scoped_lock and
 * std::lock all take it.
 *
 * Worth it only for a critical section which is a handful of instructions long
 * and is never taken while the holder can be descheduled. On an event loop
 * thread that rules out anything which reads or writes: a session which yields
 * while holding a spin lock burns a core until it is resumed. Use a
 * std::mutex, or a std::shared_mutex, everywhere else.
 */
class spin_lock
{
public:
    inline void lock() noexcept
    {
        while (m_lock.test_and_set(std::memory_order_acquire)) {
            // Spin on a relaxed load, so waiting doesn't keep the cache line
            // bouncing between the cores
            while (m_lock.test(std::memory_order_relaxed))
                pause();
        }
    }

    inline void unlock() noexcept
    {
        m_lock.clear(std::memory_order_release);
    }

    /// Spelled the way the standard library spells it: this is what makes the
    /// class Lockable rather than merely BasicLockable, and it is what
    /// std::lock() and a multi mutex std::scoped_lock call.
    inline bool try_lock() noexcept
    {
        return !m_lock.test_and_set(std::memory_order_acquire);
    }


private:
    static inline void pause() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#else
        std::this_thread::yield();
#endif
    }

private:
    std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
};


/*!
 * \brief from_hex
 *
 * \param ch a hexadecimal digit, in either case
 * \return its value, 0 to 15
 * \throw std::invalid_argument if \a ch is not a hexadecimal digit
 */
inline uint8_t from_hex(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F')
        return 10 + ch - 'A';

    throw std::invalid_argument{"Bad hex value"};
}

/*!
 * \brief unescape_url
 *
 * Decodes the percent escapes of \a in, and turns '+' into a space as a form
 * encoded value uses it. The router runs it over every query string it parses;
 * a URL path, which keeps its escapes, is left to the plugin.
 *
 * \param in the escaped text
 * \return the decoded text, which is bytes and not necessarily valid UTF-8
 * \throw std::invalid_argument on a truncated or malformed escape
 */
inline std::string unescape_url(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (std::string::size_type i = 0 ; i < in.size(); ++i) {
        switch (in[i]) {
        case '%':
            if (i + 2 < in.size()) {
                // it's faster than std::stoi(in.substr(i + 1, 2)) ...
                out += static_cast<char>(from_hex(in[i + 1]) << 4 | from_hex(in[i + 2]));
                i += 2;
            } else {
                throw std::invalid_argument{"Malformed URL"};
            }
            break;
        case '+':
            out += ' ';
            break;
        default:
            out += in[i];
            break;
        }
    }
    return out;
}


/// The pieces split() returns. They point into the string which was split,
/// which therefore has to outlive them.
using split_vector = std::vector<std::string_view>;

/*!
 * \brief split
 *
 * Splits \a str on \a ch. Empty pieces are dropped, so a leading, trailing or
 * doubled separator produces nothing: splitting "/api/v1/" on '/' gives
 * {"api", "v1"}.
 *
 * \param str the string to split, which has to outlive the result
 * \param ch the separator
 * \param count how many times to split at most. What is left after the last
 * split is returned whole, separators included, which is how a "key=value"
 * whose value contains '=' is split with count = 1.
 *
 * \return the pieces, as views into \a str
 */
inline split_vector split(std::string_view str, char ch, std::string::size_type count = std::string::npos)
{
    if (!str.size())
        return {};

    split_vector ret;
    std::string::size_type pos = 0;
    for (auto next_pos = str.find(ch, pos); next_pos != std::string::npos && count--; next_pos = str.find(ch, pos)) {
        // Ignore empty strings
        if (pos != next_pos) {
            auto sz = next_pos - pos;
            ret.emplace_back(str.substr(pos,sz));
        }
        pos = next_pos + 1;
    }
    if (pos < str.size())
        ret.emplace_back(str.substr(pos));
    return ret;
}

/*!
 * \brief The lru_cache class
 *
 * A fixed size key/value cache which drops the least recently used entry when
 * it overflows. put(), reference() and value() count as a use; exists() does
 * not, so a lookup which only asks whether something is cached does not
 * reorder the cache.
 *
 * It is not thread safe. Several event loop threads sharing one cache need a
 * lock around it, and even a read has to take it exclusively, since reading
 * moves the entry to the front.
 */
template <typename K, typename V>
class lru_cache
{
    using key_value = std::pair<K, V>;
    using list = std::list<key_value>;
    using iterator = typename list::iterator;
    using const_iterator = typename list::const_iterator;

public:
    /// \param cache_size how many entries to keep
    explicit lru_cache(size_t cache_size)
        : m_cache_size(cache_size) {}

    /// Inserts or replaces the entry of \a key, and evicts as needed
    inline void put(const K &key, const V &value)
    {
        auto it = m_cache_hash.find(key);
        if (it != m_cache_hash.end())
            m_cache_items.erase(it->second);
        m_cache_items.emplace_front(key, value);
        m_cache_hash[key] = m_cache_items.begin();
        clean_cache();
    }

    /*!
     * \brief reference
     * \return a reference to the cached value of \a key
     * \throw std::range_error if there is none. It is a reference into the
     * cache, so anything which inserts may evict the entry underneath it.
     */
    inline V &reference(const K &key)
    {
        auto it = m_cache_hash.find(key);
        if (it == m_cache_hash.end())
            throw std::range_error{"Invalid key"};
        m_cache_items.splice(m_cache_items.begin(), m_cache_items, it->second);
        return it->second->second;
    }

    /// \return a copy of the cached value of \a key, or a default constructed
    /// V if there is none
    inline V value(const K &key)
    {
        auto it = m_cache_hash.find(key);
        if (it == m_cache_hash.end())
            return V{};
        m_cache_items.splice(m_cache_items.begin(), m_cache_items, it->second);
        return it->second->second;
    }

    /// \return true if \a key is cached, without counting as a use of it
    inline bool exists(const K &key) const
    {
        return m_cache_hash.find(key) != m_cache_hash.end();
    }

    /// Resizes the cache, evicting straight away if it shrinks
    void cache_size(size_t size)
    {
        m_cache_size = size;
        clean_cache();
    }

    /// Drops every entry
    void clear()
    {
        m_cache_items.clear();
        m_cache_hash.clear();
    }

    /// Iterates the entries, most recently used first. Iterating is not a use,
    /// so the order is left alone.
    iterator begin()
    {
        return m_cache_items.begin();
    }

    iterator end()
    {
        return m_cache_items.end();
    }

    const_iterator begin() const
    {
        return m_cache_items.begin();
    }

    const_iterator end() const
    {
        return m_cache_items.end();
    }

    /// Drops the entry \a it points at
    /// \return an iterator to the entry after it
    iterator erase(iterator it)
    {
        m_cache_hash.erase(it->first);
        return m_cache_items.erase(it);
    }

    /// \return how many entries are cached
    size_t size() const
    {
        assert(m_cache_items.size() == m_cache_hash.size());
        return m_cache_hash.size();
    }
private:
    inline void clean_cache()
    {
        while (m_cache_hash.size() > m_cache_size) {
            auto it = m_cache_items.rbegin();
            m_cache_hash.erase(it->first);
            m_cache_items.pop_back();
        }
    }

private:
    list m_cache_items;
    std::unordered_map<K, iterator> m_cache_hash;
    size_t m_cache_size;
};

/*!
 * \brief address_text
 *
 * Formats the address in \a addr in its numeric form, "10.0.0.7" or
 * "2001:db8::1". No name lookup is done and none is wanted: this runs for
 * every accepted connection.
 *
 * \return the address, or an empty string if the family is neither AF_INET
 * nor AF_INET6
 */
inline std::string address_text(const sockaddr_storage &addr)
{
    // inet_ntop rather than getnameinfo: this runs for every accepted
    // connection and getnameinfo drags in the whole name service machinery
    // even when it's only asked for the numeric form.
    char hbuf[INET6_ADDRSTRLEN];
    const void *src = nullptr;
    switch (addr.ss_family) {
    case AF_INET:
        src = &reinterpret_cast<const sockaddr_in &>(addr).sin_addr;
        break;
    case AF_INET6:
        src = &reinterpret_cast<const sockaddr_in6 &>(addr).sin6_addr;
        break;
    default:
        return {};
    }
    if (inet_ntop(addr.ss_family, src, hbuf, sizeof(hbuf)))
        return hbuf;
    return {};
}

/*!
 * \brief The simple_timer class
 *
 * Calls a function every so often on a thread of its own, until it is
 * destroyed. Starting it starts the thread, and the destructor asks it to quit
 * and joins it, so the callback is never running once the timer is gone.
 *
 * One thread per timer, which is fine for the handful a plugin needs (expiring
 * a cache, refreshing a token) and wrong for anything per connection. The
 * callback runs off the event loops, so whatever it touches has to be locked
 * the same way the handlers lock it.
 */
class simple_timer
{
public:
    /*!
     * \param callback to run on the timer thread
     * \param timeout to wait between two calls, and before the first one
     * \param single_shot to call it once and stop
     */
    template<typename T>
    simple_timer(T callback, std::chrono::milliseconds timeout = std::chrono::seconds{1}, bool single_shot = false)
        : m_thread([callback, timeout, single_shot, this]{
        std::unique_lock<std::mutex> lock(m_lock);
        while (!m_wait_condition.wait_for(lock, timeout, [this]{return m_quit.load();})) {
            callback();
            if (__builtin_expect(single_shot, 0))
                m_quit.store(true);
        }
    })
    {}

    ~simple_timer()
    {
        m_quit.store(true);
        m_wait_condition.notify_one();
        if (m_thread.joinable())
            m_thread.join();
    }
private:
    std::condition_variable m_wait_condition;
    std::mutex m_lock;
    std::atomic_bool m_quit{false};
    std::thread m_thread;
};

/*!
 * \brief The buffer class
 *
 * A fixed capacity array with a read cursor, which is what the server reads
 * sockets into: data() and size() are the whole allocation, current_data() and
 * current_size() the part which has not been consumed yet.
 *
 * advance() moves the cursor over what was consumed, commit() moves what is
 * left back to the front so the rest of the allocation can be filled again,
 * and reset() starts over. Nothing here is bounds checked beyond advance()
 * clamping to the end, and nothing is shared: a buffer owns its memory and
 * cannot be copied.
 */
template <typename T = char>
class buffer {
public:
    buffer() = default;
    /// Allocates \a size elements. The buffer starts out empty, so
    /// set_current_size() has to say how much of it was filled.
    buffer(size_t size)
        : m_buffer(std::make_unique<T[]>(size))
        , m_size(size)
    {}

    /// Puts the cursor back at the start and marks the whole allocation as
    /// available
    void reset()
    {
        m_current = m_buffer.get();
        m_end = m_current + m_size;
    }

    /// Reallocates to \a size elements, keeping as much of the content as
    /// still fits, and resets the cursor
    void resize(size_t size)
    {
        if (size == m_size) {
            reset();
            return;
        }
        if (m_size) {
            auto tmp = std::make_unique<T[]>(size);
            std::memcpy(tmp.get(), m_buffer.get(), sizeof(T) * std::min(size, m_size));
            m_buffer = std::move(tmp);
        } else {
            m_buffer = std::make_unique<T[]>(size);
        }
        m_size = size;
        reset();
    }

    /// Moves the cursor \a size elements forward, clamped to the end
    void advance(size_t size)
    {
        m_current += size;
        if (m_current > m_end)
            m_current = m_end;
    }

    /// Moves what is left after the cursor back to the front of the buffer,
    /// so the rest of the allocation can be filled again
    void commit()
    {
        if (m_current == m_buffer.get())
            return;
        size_t size = m_end - m_current;
        std::memmove(m_buffer.get(), m_current, size);
        m_current = m_buffer.get();
        m_end = m_current + size;
    }

    /// \return the first element which has not been consumed
    const T *current_data() const
    {
        return m_current;
    }

    T *current_data()
    {
        return m_current;
    }
    /// \return how many elements are left after the cursor
    size_t current_size() const
    {
        return m_end - m_current;
    }

    /// Says that \a size elements are readable after the cursor, which is how
    /// a read of \a size bytes is reported back
    void set_current_size(size_t size)
    {
        m_end = m_current + size;
    }

    /// Puts the cursor \a size elements after the start of the buffer
    void set_current_data(size_t size)
    {
        m_current = m_buffer.get() + size;
    }

    /// \return the whole allocation, cursor or not
    const T *data() const
    {
        return m_buffer.get();
    }
    T *data()
    {
        return m_buffer.get();
    }

    /// \return the size of the whole allocation
    size_t size() const
    {
        return m_size;
    }

    /// \return a view of what is left after the cursor
    std::basic_string_view<T> current_string() const
    {
        return {m_current, size_t(m_end - m_current)};
    }

    /// \return a view of the whole allocation
    std::basic_string_view<T> string() const
    {
        return std::basic_string_view<T>{m_buffer.get(), m_size};
    }

    /// Resizes to fit \a buff exactly and copies it in
    buffer<T>& operator =(std::string_view buff)
    {
        const size_t size = buff.size();
        resize(size);
        memcpy(m_buffer.get(), buff.data(), size);
        set_current_size(size);
        return *this;
    }

    /// Copies \a buff into the buffer without shrinking it, and only
    /// reallocates when it does not fit
    buffer<T>& operator *=(std::string_view buff)
    {
        const size_t size = buff.size();
        if (m_size <= size)
            return operator=(buff);
        reset();
        memcpy(m_buffer.get(), buff.data(), size);
        advance(size);
        return *this;
    }

    /// Frees the allocation
    void clear()
    {
        m_buffer.reset();
        m_size = 0;
        reset();
    }
private:
    std::unique_ptr<T[]> m_buffer;
    size_t m_size = 0;
    T *m_current = nullptr;
    T *m_end = nullptr;
};

using char_buffer = buffer<char>;

} // namespace Dracon
