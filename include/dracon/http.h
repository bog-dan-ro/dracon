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

#include <algorithm>
#include <charconv>
#include <chrono>
#include <functional>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <dracon/stream.h>

namespace dracon {

namespace detail {

// https://www.w3.org/protocols/rfc2616/rfc2616-sec10.html
inline constexpr std::pair<uint16_t, std::string_view> status_codes[] = {
    {100, "100 Continue\r\n"},
    {101, "101 Switching Protocols\r\n"},
    {200, "200 OK\r\n"},
    {201, "201 Created\r\n"},
    {202, "202 Accepted\r\n"},
    {203, "203 Non-Authoritative Information\r\n"},
    {204, "204 No Content\r\n"},
    {205, "205 Reset Content\r\n"},
    {206, "206 Partial Content\r\n"},
    {300, "300 Multiple Choices\r\n"},
    {301, "301 Moved Permanently\r\n"},
    {302, "302 Found\r\n"},
    {303, "303 See Other\r\n"},
    {304, "304 Not Modified\r\n"},
    {305, "305 Use Proxy\r\n"},
    {306, "306 Switch Proxy\r\n"},
    {307, "307 Temporary Redirect\r\n"},
    {400, "400 Bad Request\r\n"},
    {401, "401 Unauthorized\r\n"},
    {402, "402 Payment Required\r\n"},
    {403, "403 Forbidden\r\n"},
    {404, "404 Not Found\r\n"},
    {405, "405 Method Not Allowed\r\n"},
    {406, "406 Not Acceptable\r\n"},
    {407, "407 Proxy Authentication Required\r\n"},
    {408, "408 Request Timeout\r\n"},
    {409, "409 Conflict\r\n"},
    {410, "410 Gone\r\n"},
    {411, "411 Length Required\r\n"},
    {412, "412 Precondition Failed\r\n"},
    {413, "413 Request Entity Too Large\r\n"},
    {414, "414 Request-URI Too Long\r\n"},
    {415, "415 Unsupported Media Type\r\n"},
    {416, "416 Requested Range Not Satisfiable\r\n"},
    {417, "417 Expectation Failed\r\n"},
    {431, "431 Request Header Fields Too Large\r\n"},
    {500, "500 Internal Server Error\r\n"},
    {501, "501 Not Implemented\r\n"},
    {502, "502 Bad Gateway\r\n"},
    {503, "503 Service Unavailable\r\n"},
    {504, "504 Gateway Timeout\r\n"},
    {505, "505 HTTP Version Not Supported\r\n"},
};
inline constexpr auto status_code_of = &std::pair<uint16_t, std::string_view>::first;
static_assert(std::ranges::is_sorted(status_codes, {}, status_code_of));

inline std::string_view status_code_string(uint16_t status)
{
    const auto it = std::ranges::lower_bound(status_codes, status, {}, status_code_of);
    if (it != std::ranges::end(status_codes) && it->first == status)
        return it->second;
    return "500 Internal Server Error\r\n"sv;
}

inline bool response_header_name_equals(std::string_view lhs, std::string_view rhs)
{
    return lhs.size() == rhs.size() &&
            std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
                if (a >= 'A' && a <= 'Z')
                    a += 'a' - 'A';
                if (b >= 'A' && b <= 'Z')
                    b += 'a' - 'A';
                return a == b;
            });
}

/*!
 * \brief append_header_part
 *
 * Appends \a part to \a out with every CR, LF and NUL replaced by a space.
 * Header names and values often carry data which came from the request, and
 * without this a single "\r\n" in them would let the client append headers of
 * its own, or a whole extra response (response splitting).
 */
inline void append_header_part(std::string &out, std::string_view part)
{
    const auto begin = out.size();
    out += part;
    for (auto i = begin; i < out.size(); ++i) {
        if (out[i] == '\r' || out[i] == '\n' || out[i] == '\0')
            out[i] = ' ';
    }
}

inline void append_number(std::string &out, size_t value)
{
    char buff[24];
    const auto res = std::to_chars(buff, buff + sizeof(buff), value);
    out.append(buff, res.ptr);
}

} // namespace detail

/*!
 * \brief The length of a body which is not known in advance.
 *
 * response::set_content_length(chunked_data) picks Transfer-Encoding: chunked
 * instead of a Content-Length, and request::content_length() returns it for a
 * request which arrived chunked.
 */
static constexpr auto chunked_data = std::numeric_limits<size_t>::max();

/*!
 * \brief The headers of a request or of a response.
 *
 * Both request and response derive from it, so a header is read with
 * req["Content-Type"] or req.find("Range"), and set with
 * res["Location"] = url.
 *
 * Names are canonicalized on the way in, "content-length" is parsed as
 * "Content-Length", so look a request header up in that spelling. The lookup
 * itself is a plain case sensitive hash: nothing corrects the spelling of a
 * name a plugin invents.
 *
 * Being a map, it holds one value per name. A request which repeats a header
 * keeps the last one.
 */
using fields = std::unordered_map<std::string, std::string>;

/*!
 * \brief The response class
 *
 * The status code, the headers and, for a short answer, the body of a
 * response. Writing it to a stream serializes it and sends it:
 * \code
 *     stream << dracon::response{200, "hello\n", {{"Content-Type", "text/plain"}}};
 * \endcode
 *
 * Content-Length, Transfer-Encoding, Connection and Keep-Alive are computed
 * from the body and from the state of the connection; setting them as headers
 * has no effect, they are dropped while serializing. CR, LF and NUL in any
 * other header are replaced by a space, so a value taken from the request
 * cannot inject a header of its own.
 *
 * A body which is produced as it goes does not belong in the body field. Send
 * the head with set_content_length(chunked_data) and write the rest through a
 * dracon::chunked_stream.
 *
 * A response is also how a session reports a failure it wants to shape itself:
 * throwing one sends exactly that status, those headers and that body, as long
 * as nothing has been written yet.
 */
class response: public fields
{
public:
    /*!
     * \brief response
     * \param status_code of the response, 500 by default so that a response
     * which was never filled in cannot be mistaken for a success
     * \param body of the response, which also sets the content length
     * \param fields the headers
     */
    response(uint16_t status_code = 500, std::string_view body = {}, fields &&flds = {})
        : fields(std::move(flds))
        , m_status_code(status_code)
        , m_body(body)
        , m_content_length(body.size())
    {}

    /// Sets the HTTP status. An unknown code is sent as a 500.
    response &set_status_code(uint16_t status_code)
    {
        m_status_code = status_code;
        return *this;
    }
    uint16_t status_code() const {return m_status_code;}

    /*!
     * \brief set_content_length
     *
     * Announces a body of \a length bytes which the session writes itself, and
     * drops the body this response held. chunked_data announces
     * Transfer-Encoding: chunked instead, for a body whose length is not known
     * when the head goes out.
     */
    response &set_content_length(size_t length)
    {
        m_content_length = length;
        m_body.clear();
        return *this;
    }
    size_t content_length() const {return m_content_length;}

    /*!
     * \brief set_keep_alive
     *
     * Overrides how long the connection stays open after this response. 0
     * closes it, and the default of -1 leaves the timeout the server worked
     * out for the session.
     */
    response &set_keep_alive(std::chrono::seconds seconds)
    {
        m_keep_alive = seconds;
        return *this;
    }
    std::chrono::seconds keep_alive() const {return m_keep_alive;}

    /// Sets the body, and the content length along with it
    response &set_body(std::string body)
    {
        m_body = std::move(body);
        m_content_length = m_body.size();
        return *this;
    }
    const std::string &body() const {return m_body;}

    /*!
     * \brief to_string
     *
     * Serializes the response: the status line, the headers, the computed
     * Content-Length or Transfer-Encoding and Connection headers, and the
     * body. An informational (1xx) or 204 response is serialized without a
     * body and without the connection headers, as the protocol requires.
     *
     * \param keep_alive_override how long to keep the connection alive, taken
     * from the response itself when it is left at -1. The stream passes its
     * own value here, which is why writing a response is usually enough and
     * this is rarely called by hand.
     */
    std::string to_string(std::chrono::seconds keep_alive_override = std::chrono::seconds{-1}) const
    {
        const uint16_t status_code = m_status_code ? m_status_code : uint16_t(500);
        const auto status_line = detail::status_code_string(status_code);
        const bool informational = status_code >= 100 && status_code < 200;
        const bool body_less = informational || status_code == 204;

        size_t size = status_line.size() + sizeof("HTTP/1.1 ") + 96;
        for (const auto &kv : *this)
            size += kv.first.size() + kv.second.size() + 4;
        if (!body_less)
            size += m_body.size();

        std::string res;
        res.reserve(size);
        res += "HTTP/1.1 "sv;
        res += status_line;
        for (const auto &kv : *this) {
            if (detail::response_header_name_equals(kv.first, "Content-Length"sv) ||
                    detail::response_header_name_equals(kv.first, "Transfer-Encoding"sv) ||
                    detail::response_header_name_equals(kv.first, "Keep-Alive"sv) ||
                    detail::response_header_name_equals(kv.first, "Connection"sv)) {
                continue;
            }
            detail::append_header_part(res, kv.first);
            res += ": "sv;
            detail::append_header_part(res, kv.second);
            res += crlf_string;
        }

        if (keep_alive_override.count() == -1)
            keep_alive_override = m_keep_alive;
        if (!body_less) {
            if (m_content_length == chunked_data) {
                res += "Transfer-Encoding: chunked\r\n"sv;
            } else {
                res += "Content-Length: "sv;
                detail::append_number(res, m_content_length);
                res += crlf_string;
            }
        }
        if (!informational) {
            if (keep_alive_override.count() > 0) {
                res += "Keep-Alive: timeout="sv;
                detail::append_number(res, size_t(keep_alive_override.count()));
                res += crlf_string;
                res += "Connection: keep-alive\r\n"sv;
            } else {
                res += "Connection: close\r\n"sv;
            }
        }
        res += crlf_string;
        if (!body_less)
            res += m_body;
        return res;
    }

private:
    uint16_t m_status_code;
    std::string m_body;
    size_t m_content_length = 0;
    std::chrono::seconds m_keep_alive{-1};
};

/*!
 * Sends \a res on \a stream.
 *
 * The keep alive of the response, when it set one, is applied to the
 * connection, and the session timeout is stretched to fit the body: ten
 * seconds plus a second for every 512 KB, so a large response is not cut off
 * by the timeout of the request which asked for it.
 */
inline abstract_stream &operator << (abstract_stream &stream, const response &res)
{
    using namespace std::chrono_literals;
    if (res.keep_alive().count() != -1)
        stream.set_keep_alive(res.keep_alive());
    if (res.content_length() != chunked_data)
        stream.set_session_timeout(std::max(stream.session_timeout(),
                                        10s + std::chrono::seconds(res.content_length() / (512 * 1024))));
    stream.write(res.to_string(stream.keep_alive()));
    return stream;
}

/*!
 * \brief The request class
 *
 * One request: the method, the URL, the headers, and the way to read the body.
 *
 * A plugin sees it twice. create_session() gets it with the headers parsed and
 * nothing read of the body, and only routes on it. The session then gets it
 * again, registers a body callback if it expects one, and reads the rest with
 * "stream >> req".
 *
 * Header names are canonicalized while parsing, so they are looked up in their
 * usual spelling:
 * \code
 *     if (auto it = req.find("Content-Type"); it != req.end() && it->second != "application/json")
 *         throw 415;
 * \endcode
 *
 * The body is never buffered by the server. It is handed to the callback
 * append_body_callback() registered, in the pieces it arrives in, which is what
 * lets a session stream an upload straight to its destination without ever
 * holding it whole.
 */
class request : public fields
{
public:
    /*!
     * \brief How far the parser got.
     *
     * A session is created at headers_completed, or at completed when the
     * request has no body at all, and "stream >> req" drives it to completed.
     */
    enum class parse_state {
        uninitialized,     ///< nothing has been parsed yet
        processing_url,     ///< the request line is still incomplete
        processing_header,  ///< the method and URL are known, headers are still arriving
        headers_completed,  ///< all the headers are in, the body is not
        processing_body,    ///< the body is being handed to the callback
        completed,         ///< the whole request has been read
    };

    /*!
     * \brief Receives the body of a request, one piece at a time.
     *
     * The pieces are the ones the socket and the chunked decoder produced, so
     * they carry no meaning: a callback which parses has to be able to resume
     * where the previous piece ended, or append until the request is complete.
     * The view is only valid for the duration of the call.
     */
    using body_callback = std::function<void(std::string_view)>;
public:
    request() = default;

    /// \return how far the parser got with this request
    parse_state state() const noexcept { return m_state; }
    void set_state(parse_state s) noexcept { m_state = s; }

    /*!
     * \brief url
     * \return the request target, as it was sent and including the query
     * string. It is not unescaped, and it is not resolved against anything: a
     * plugin which turns it into a path has to reject "..", or reject the
     * resulting path.
     */
    const std::string &url() const noexcept { return m_url; }
    void set_url(std::string &&url) noexcept { m_url = std::move(url); }

    /// \return the request method, uppercase as it was sent
    const std::string &method() const noexcept { return m_method; }
    void set_method(std::string &&method) noexcept { m_method = std::move(method); }

    /// \return true when the client asked for the connection to be reused
    bool keep_alive() const noexcept { return m_keep_alive; }
    void set_keep_alive(bool keep) noexcept { m_keep_alive = keep; }

    /*!
     * \brief append_body_callback
     *
     * Says that this session expects a body, and where it goes. call it before
     * reading the request:
     * \code
     *     std::string body;
     *     req.append_body_callback([&body](std::string_view part) { body += part; },
     *                            1024 * 1024);
     *     stream >> req;
     * \endcode
     *
     * A request which carries a body without a callback is answered with a
     * 400, and one whose Content-Length is over \a max_size with a 413, before
     * any of it is read. The default limit is effectively unbounded, so a
     * session which does not want to hold an arbitrarily large upload in
     * memory has to pass one it can afford.
     *
     * \param callback which receives the body, in the pieces it arrives in
     * \param max_size the largest body this session accepts
     */
    void append_body_callback(const body_callback &callback, size_t max_size = std::numeric_limits<size_t>::max() - 1) noexcept
    {
        m_max_body_size = max_size;
        m_callback = callback;
    }

    /*!
     * \brief append_body
     *
     * Hands \a body to the callback the session registered. Called by the
     * server while parsing, and throws a 400 response when the session did not
     * register one.
     */
    void append_body(std::string_view body) noexcept(false)
    {
        if (!m_callback)
            throw response(400, "unexpected body"sv);
        m_callback(body);
    }

    /*!
     * \brief content_length
     * \return the announced length of the body, or chunked_data when the
     * request has no usable Content-Length, which is the case for a chunked
     * request and for one with no body at all.
     */
    size_t content_length() const
    {
        auto it = find("Content-Length");
        if (it != end()) {
            char *end;
            auto data = it->second.data();
            auto len = std::strtoull(it->second.data(), &end, 10);
            if (end == data + it->second.size())
                return len;
        }
        return dracon::chunked_data;
    }

    /// \return the limit append_body_callback() set, 0 when the session did not
    /// ask for a body
    size_t max_body_size() const noexcept
    {
        return m_max_body_size;
    }

private:
    bool m_keep_alive = false;
    std::string m_url;
    std::string m_method;
    body_callback m_callback;
    parse_state m_state = parse_state::uninitialized;
    size_t m_max_body_size = 0;
};

/*!
 * Reads the rest of \a req from \a stream, and leaves it in
 * request::parse_state::completed.
 *
 * A request which asked for an Expect: 100-continue is answered here: with a
 * 100 when the session can take the body, and with a 417 when the announced
 * length is over the limit append_body_callback() set, in which case the body is
 * never sent.
 *
 * Every session has to read the request out, even one which ignores the body.
 * An unread body is indistinguishable from the next request on the connection,
 * so the server closes the connection instead of reusing it.
 */
inline abstract_stream &operator >> (abstract_stream &stream, request &req)
{
    auto it = req.find("Expect");
    if (it != req.end() && it->second == "100-continue") {
        size_t content_length = req.content_length();
        if (content_length != chunked_data && req.max_body_size() < content_length)
            throw 417; // Expectation Failed
        else
            stream << response{100};
    }

    stream.read(req);
    return stream;
}

namespace literals {
/*!
 * A bodyless response with the given status:
 * \code
 *     using namespace dracon::literals;
 *     stream << 204_http;
 * \endcode
 */
inline response operator ""_http(unsigned long long int status)
{
    return response{uint16_t(status)};
}
} // namespace literals

} // namespace Dracon
