/*
    Copyright (C) 2026, BogDan Vatra <bogdan@kde.org>

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

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <dracon/http.h>

namespace dracon::internal {

namespace http_parser_detail {

enum char_class : uint8_t {
    token_char = 1,  // RFC 7230 tchar
    TargetChar = 2, // printable US-ASCII, no whitespace
    value_char = 4,  // field-value: HTAB / SP / VCHAR / obs-text
    hex_char = 8
};

constexpr std::array<uint8_t, 256> make_char_table()
{
    std::array<uint8_t, 256> table{};
    for (int c = 0; c < 256; ++c) {
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (alpha || digit || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' ||
            c == '*' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~')
            table[c] |= token_char;
        if (c > 0x20 && c < 0x7f)
            table[c] |= TargetChar;
        if (c == '\t' || (c >= 0x20 && c != 0x7f))
            table[c] |= value_char;
        if (digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            table[c] |= hex_char;
    }
    return table;
}

inline constexpr auto char_table = make_char_table();

inline bool all_chars(std::string_view str, uint8_t char_class) noexcept
{
    for (unsigned char c : str)
        if (!(char_table[c] & char_class))
            return false;
    return true;
}

inline constexpr char to_lower_ascii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
inline constexpr char to_upper_ascii(char c) noexcept { return (c >= 'a' && c <= 'z') ? char(c - 32) : c; }
inline constexpr uint8_t hex_value(char c) noexcept { return c <= '9' ? c - '0' : (to_lower_ascii(c) - 'a' + 10); }

inline bool iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i]))
            return false;
    return true;
}

inline std::string_view trim_ows(std::string_view str) noexcept
{
    while (!str.empty() && (str.front() == ' ' || str.front() == '\t'))
        str.remove_prefix(1);
    while (!str.empty() && (str.back() == ' ' || str.back() == '\t'))
        str.remove_suffix(1);
    return str;
}

/// Calls \a fn for every non empty, comma separated, token of a header \a value
template <typename Fn>
void for_each_token(std::string_view value, Fn fn)
{
    for (;;) {
        const auto comma = value.find(',');
        if (const auto token = trim_ows(value.substr(0, comma)); !token.empty())
            fn(token);
        if (comma == std::string_view::npos)
            return;
        value.remove_prefix(comma + 1);
    }
}

/// "content-length" -> "Content-Length"
inline std::string canonical_name(std::string_view name)
{
    std::string res(name.size(), '\0');
    bool upper = true;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        res[i] = upper ? to_upper_ascii(c) : to_lower_ascii(c);
        upper = c == '-';
    }
    return res;
}

struct line
{
    std::string_view text; // without the line terminator
    size_t next = 0;       // position of the next line
};

/// Extracts the LF or CRLF terminated line which starts at \a pos
inline bool next_line(std::string_view data, size_t pos, line &line) noexcept
{
    const auto lf = static_cast<const char *>(std::memchr(data.data() + pos, '\n', data.size() - pos));
    if (!lf)
        return false;
    size_t end = lf - data.data();
    line.next = end + 1;
    if (end > pos && data[end - 1] == '\r')
        --end;
    line.text = data.substr(pos, end - pos);
    return true;
}

[[noreturn]] inline void fail(uint16_t status, std::string_view reason)
{
    throw dracon::response{status, reason};
}

} // namespace http_parser_detail

/*!
 * \brief The http_request_parser class
 *
 * Incremental HTTP/1.x request parser.
 *
 * It is fed the received bytes as they arrive and consumes them as soon as it
 * can, without needing the whole request buffered:
 *
 *   * parse_head() consumes complete request-line / header lines. The request's
 *     method and url are set as soon as the request line arrives (the state
 *     moves uninitialized -> processing_url -> processing_header), and each header
 *     field is decoded as its line completes. When the terminating empty line
 *     arrives the state becomes headers_completed, or completed if there is no
 *     body. It returns the number of bytes it consumed from \a data; the caller
 *     drops those and feeds the rest, plus whatever it receives next, back in.
 *   * parse_body() then decodes the body (Content-Length or chunked)
 *     incrementally, handing it to dracon::request::append_body(), and sets the
 *     state to completed once the whole body has been received.
 *
 * The same parser instance is reused across keep-alive requests: it resets its
 * per-request state whenever parse_head() is handed a fresh (uninitialized)
 * request. Header field names are canonicalized ("content-length" ->
 * "Content-Length"). Malformed input is reported by throwing a dracon::response
 * with the appropriate status code (400, 413, 431, 501 or 505).
 */
class http_request_parser
{
public:
    /// Maximum size of the request line + header fields
    static constexpr size_t max_head_size = 8 * 1024;
    /// Maximum number of header fields
    static constexpr size_t max_headers_count = 100;
    /// Maximum size of a chunk-size line (including its extensions)
    static constexpr size_t max_chunk_line_size = 1024;

    /*!
     * \brief parse_head
     *
     * Consumes as much of the request head as is available at the beginning of
     * \a data, updating \a req incrementally, and returns the number of bytes
     * consumed. The caller must drop those bytes and feed the remainder (plus
     * newly received data) on the next call. \a req state reaches
     * headers_completed (a body follows) or completed (no body) once the head is
     * fully parsed.
     */
    size_t parse_head(std::string_view data, dracon::request &req);

    /*!
     * \brief parse_body
     *
     * Decodes the request body found at the beginning of \a data and hands it
     * over to \a req body callback. Returns the number of consumed bytes; the
     * remaining bytes must be fed again with the next received data. \a req
     * state becomes completed when the entire body was received.
     */
    size_t parse_body(std::string_view data, dracon::request &req);

private:
    enum class head : uint8_t { request_line, headers, done };
    enum class body : uint8_t { none, content_length, chunked };
    enum class chunk : uint8_t { Size, Data, data_end, trailers };

    void begin_request(dracon::request &req) noexcept;
    void parse_request_line(std::string_view line, dracon::request &req);
    void parse_header_field(std::string_view line, dracon::request &req);
    void finalize_head(dracon::request &req);
    size_t parse_chunked(std::string_view data, dracon::request &req);
    void deliver(std::string_view data, dracon::request &req);

private:
    head m_head = head::request_line;
    body m_body = body::none;
    chunk m_chunk = chunk::Size;
    size_t m_head_seen = 0;      // head bytes consumed so far (bounds max_head_size)
    size_t m_headers = 0;       // header fields seen so far (bounds max_headers_count)
    size_t m_remaining = 0;     // body bytes still expected, or bytes left in the current chunk
    size_t m_received = 0;      // body bytes delivered so far
    size_t m_content_length = 0;
    bool m_http11 = false;
    bool m_has_content_length = false;
    bool m_chunked = false;
    bool m_close = false;
    bool m_keep_alive = false;
};

inline void http_request_parser::begin_request(dracon::request &req) noexcept
{
    m_head = head::request_line;
    m_body = body::none;
    m_chunk = chunk::Size;
    m_head_seen = m_headers = m_remaining = m_received = m_content_length = 0;
    m_http11 = m_has_content_length = m_chunked = m_close = m_keep_alive = false;
    req.set_state(dracon::request::parse_state::processing_url);
}

inline size_t http_request_parser::parse_head(std::string_view data, dracon::request &req)
{
    using namespace http_parser_detail;
    using state = dracon::request::parse_state;

    // A fresh request restarts the head state machine (parser is reused across
    // keep-alive requests).
    if (req.state() == state::uninitialized)
        begin_request(req);
    if (m_head == head::done)
        return 0;

    size_t pos = 0;
    line line;

    if (m_head == head::request_line) {
        // RFC 7230 3.5: ignore empty line(s) received prior to the request-line,
        // but count them towards the head-size limit so they can't be endless.
        for (;;) {
            if (!next_line(data, pos, line)) {
                if (m_head_seen + data.size() > max_head_size)
                    fail(431, "Request header fields too large"sv);
                m_head_seen += pos;
                return pos;
            }
            if (!line.text.empty())
                break;
            pos = line.next;
        }
        parse_request_line(line.text, req);
        pos = line.next;
        if (m_head_seen + pos > max_head_size)
            fail(431, "Request header fields too large"sv);
        m_head = head::headers;
        req.set_state(state::processing_header);
    }

    while (m_head == head::headers) {
        if (!next_line(data, pos, line)) {
            if (m_head_seen + data.size() > max_head_size)
                fail(431, "Request header fields too large"sv);
            break;
        }
        pos = line.next;
        // Bound a fully-received head too, not only one still being buffered.
        if (m_head_seen + pos > max_head_size)
            fail(431, "Request header fields too large"sv);
        if (line.text.empty()) {
            finalize_head(req);
            m_head = head::done;
            break;
        }
        parse_header_field(line.text, req);
    }

    m_head_seen += pos;
    return pos;
}

inline void http_request_parser::parse_request_line(std::string_view request_line, dracon::request &req)
{
    using namespace http_parser_detail;
    // request-line = method SP request-target SP HTTP-version
    const auto sp1 = request_line.find(' ');
    if (sp1 == std::string_view::npos || sp1 == 0)
        fail(400, "Invalid request line"sv);
    const auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || sp2 == sp1 + 1)
        fail(400, "Invalid request line"sv);
    const auto method = request_line.substr(0, sp1);
    const auto target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    const auto version = request_line.substr(sp2 + 1);
    if (!all_chars(method, token_char))
        fail(400, "Invalid method"sv);
    if (!all_chars(target, TargetChar))
        fail(400, "Invalid request target"sv);
    if (version.size() != 8 || !version.starts_with("HTTP/"sv) || version[6] != '.' ||
        (char_table[uint8_t(version[5])] & hex_char) == 0 || version[5] > '9' ||
        (char_table[uint8_t(version[7])] & hex_char) == 0 || version[7] > '9')
        fail(400, "Invalid HTTP version"sv);
    if (version[5] != '1')
        fail(505, "HTTP version not supported"sv);
    m_http11 = version[7] != '0';
    req.set_method(std::string{method});
    req.set_url(std::string{target});
}

inline void http_request_parser::parse_header_field(std::string_view text, dracon::request &req)
{
    using namespace http_parser_detail;
    // header-field = field-name ":" OWS field-value OWS
    if (++m_headers > max_headers_count)
        fail(431, "Too many header fields"sv);
    if (text.front() == ' ' || text.front() == '\t')
        fail(400, "Obsolete line folding is not supported"sv);
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0)
        fail(400, "Invalid header field"sv);
    const auto name = text.substr(0, colon);
    if (!all_chars(name, token_char))
        fail(400, "Invalid header field name"sv);
    const auto value = trim_ows(text.substr(colon + 1));
    if (!all_chars(value, value_char))
        fail(400, "Invalid header field value"sv);

    auto key = canonical_name(name);
    if (key == "Content-Length"sv) {
        size_t length = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), length);
        if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size() || length >= dracon::chunked_data)
            fail(400, "Invalid Content-Length"sv);
        if (m_has_content_length) {
            if (length != m_content_length)
                fail(400, "Conflicting Content-Length"sv);
            return; // identical duplicate: keep a single stored value
        }
        m_has_content_length = true;
        m_content_length = length;
    } else if (key == "Transfer-Encoding"sv) {
        // RFC 7230 3.3.3: chunked must be the final transfer coding and it's the
        // only one this server implements.
        size_t codings = m_chunked ? 1 : 0;
        std::string_view last;
        for_each_token(value, [&](std::string_view coding) { ++codings; last = coding; });
        if (!iequals(last, "chunked"sv))
            fail(400, "Invalid Transfer-Encoding"sv);
        if (codings != 1)
            fail(501, "Unsupported transfer coding"sv);
        m_chunked = true;
    } else if (key == "Connection"sv) {
        for_each_token(value, [&](std::string_view option) {
            if (iequals(option, "close"sv))
                m_close = true;
            else if (iequals(option, "keep-alive"sv))
                m_keep_alive = true;
        });
    }

    if (const auto [it, inserted] = req.try_emplace(std::move(key), value); !inserted) {
        // RFC 7230 3.2.2: combine the values of repeated fields
        it->second += ", ";
        it->second += value;
    }
}

inline void http_request_parser::finalize_head(dracon::request &req)
{
    using state = dracon::request::parse_state;
    if (m_has_content_length && m_chunked)
        http_parser_detail::fail(400, "Content-Length and Transfer-Encoding are mutually exclusive"sv);
    req.set_keep_alive(m_http11 ? !m_close : (m_keep_alive && !m_close));
    m_received = 0;
    m_chunk = chunk::Size;
    if (m_chunked) {
        m_body = body::chunked;
        m_remaining = 0;
        req.set_state(state::headers_completed);
    } else if (m_content_length) {
        m_body = body::content_length;
        m_remaining = m_content_length;
        req.set_state(state::headers_completed);
    } else {
        m_body = body::none;
        req.set_state(state::completed);
    }
}

inline size_t http_request_parser::parse_body(std::string_view data, dracon::request &req)
{
    using namespace http_parser_detail;
    using state = dracon::request::parse_state;

    if (req.state() == state::completed || m_body == body::none)
        return 0;

    req.set_state(state::processing_body);
    if (m_body == body::chunked)
        return parse_chunked(data, req);

    if (const auto max = req.max_body_size(); max && m_remaining > max - m_received)
        fail(413, "Request entity too large"sv);
    const size_t size = std::min(m_remaining, data.size());
    if (size) {
        deliver(data.substr(0, size), req);
        m_remaining -= size;
    }
    if (!m_remaining)
        req.set_state(state::completed);
    return size;
}

inline size_t http_request_parser::parse_chunked(std::string_view data, dracon::request &req)
{
    using namespace http_parser_detail;

    size_t pos = 0;
    while (pos < data.size()) {
        switch (m_chunk) {
        case chunk::Size: {
            // chunk-size [ chunk-ext ] CRLF
            line line;
            if (!next_line(data, pos, line)) {
                if (data.size() - pos > max_chunk_line_size)
                    fail(400, "Invalid chunk size"sv);
                return pos;
            }
            const auto text = line.text;
            size_t digits = 0;
            size_t size = 0;
            while (digits < text.size() && (char_table[uint8_t(text[digits])] & hex_char)) {
                if (digits == 16)
                    fail(400, "Chunk size too large"sv);
                size = (size << 4) | hex_value(text[digits++]);
            }
            if (!digits)
                fail(400, "Invalid chunk size"sv);
            // chunk extensions are ignored
            if (const auto ext = trim_ows(text.substr(digits)); !ext.empty() && (ext.front() != ';' || !all_chars(ext, value_char)))
                fail(400, "Invalid chunk extension"sv);
            pos = line.next;
            m_remaining = size;
            m_chunk = size ? chunk::Data : chunk::trailers;
            break;
        }
        case chunk::Data: {
            const size_t size = std::min(m_remaining, data.size() - pos);
            deliver(data.substr(pos, size), req);
            pos += size;
            m_remaining -= size;
            if (!m_remaining)
                m_chunk = chunk::data_end;
            break;
        }
        case chunk::data_end:
            // CRLF (or a bare LF) after the chunk-data
            if (data[pos] == '\n') {
                ++pos;
            } else if (data[pos] == '\r') {
                if (pos + 1 == data.size())
                    return pos;
                if (data[pos + 1] != '\n')
                    fail(400, "Invalid chunk"sv);
                pos += 2;
            } else {
                fail(400, "Invalid chunk"sv);
            }
            m_chunk = chunk::Size;
            break;
        case chunk::trailers: {
            // trailer fields are ignored, the empty line ends the message
            line line;
            if (!next_line(data, pos, line)) {
                if (data.size() - pos > max_head_size)
                    fail(431, "Trailer fields too large"sv);
                return pos;
            }
            pos = line.next;
            if (line.text.empty()) {
                req.set_state(dracon::request::parse_state::completed);
                return pos;
            }
            // trailers share the header budget, an endless stream of them must
            // not keep the connection (and the parser) busy for free
            if (++m_headers > max_headers_count)
                fail(431, "Too many trailer fields"sv);
            if (!all_chars(line.text, value_char))
                fail(400, "Invalid trailer field"sv);
            break;
        }
        }
    }
    return pos;
}

inline void http_request_parser::deliver(std::string_view data, dracon::request &req)
{
    m_received += data.size();
    if (const auto max = req.max_body_size(); max && m_received > max)
        http_parser_detail::fail(413, "Request entity too large"sv);
    req.append_body(data);
}

} // namespace dracon::internal
