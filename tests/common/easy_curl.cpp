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

#include "easy_curl.h"

#include <iostream>
#include <cstring>
#include <boost/algorithm/string.hpp>
#include <vector>

namespace dracon {
namespace test {

easy_curl::easy_curl()
{
    m_curl = curl_easy_init();
    if (!m_curl)
        throw std::runtime_error{"Can't init CUrl"};
    set_options(CURLOPT_WRITEFUNCTION, &write_callback);
    set_options(CURLOPT_HEADERFUNCTION, &header_callback);
    set_options(CURLOPT_PATH_AS_IS, 1L);
}

easy_curl::~easy_curl()
{
    curl_slist_free_all(m_headers_list);
    curl_easy_cleanup(m_curl);
}

easy_curl &easy_curl::set_url(const std::string &url)
{
    set_options(CURLOPT_URL, url.c_str());
    return *this;
}

easy_curl &easy_curl::set_headers(const easy_curl::header_map &headers)
{
    if (m_headers_list) {
        curl_slist_free_all(m_headers_list);
        m_headers_list = nullptr;
    }

    for (const auto &kv: headers)
        m_headers_list = curl_slist_append(m_headers_list, (kv.first + ": " + kv.second).c_str());

    set_options(CURLOPT_HTTPHEADER, m_headers_list);
    return *this;
}

easy_curl::response easy_curl::request(const std::string &method, std::string upload) const
{
    response res;
    set_options(CURLOPT_WRITEDATA, &res);
    set_options(CURLOPT_HEADERDATA, &res);
    set_options(CURLOPT_CUSTOMREQUEST, method.c_str());

    if (upload.size()) {
        set_options(CURLOPT_READDATA, &upload);
        set_options(CURLOPT_READFUNCTION, &read_callback);
        set_options(CURLOPT_UPLOAD, 1L);
        set_options(CURLOPT_INFILESIZE_LARGE, curl_off_t(upload.size()));
    } else {
        set_options(CURLOPT_READDATA, nullptr);
        set_options(CURLOPT_READFUNCTION, nullptr);
        set_options(CURLOPT_UPLOAD, 0L);
        set_options(CURLOPT_INFILESIZE_LARGE, 0);
    }

    auto err = curl_easy_perform(m_curl);
    if (err != CURLE_OK) {
        std::cerr << curl_easy_strerror(err);
        throw std::runtime_error{curl_easy_strerror(err)};
    }

    return res;
}

std::string easy_curl::escape(std::string_view str)
{
    auto allocated_str = curl_escape(str.data(), str.length());
    std::string ss{allocated_str};
    curl_free(allocated_str);
    return ss;
}

size_t easy_curl::read_callback(char *buffer, size_t size, size_t nitems, std::string *upload)
{
    size_t sz = std::min(size * nitems, upload->size());
    mempcpy(buffer, upload->c_str(), sz);
    upload->erase(0, sz);
    return sz;
}

size_t easy_curl::write_callback(char *ptr, size_t size, size_t nmemb, easy_curl::response *response)
{
    response->body.append(ptr, size * nmemb);
    return size * nmemb;
}

size_t easy_curl::header_callback(char *buffer, size_t size, size_t nitems, easy_curl::response *response)
{
    if (size * nitems <= 2)
        return size * nitems;

    std::string header{buffer, size * nitems - 2};
    if (header.substr(0, 8) == "HTTP/1.1") {
        std::vector<std::string> status;
        boost::split(status, header, boost::is_any_of(" "));
        response->status = status.size() > 1 ? status[1] : "unknown";
    } else {
        std::vector<std::string> kv;
        boost::split(kv, header, boost::is_any_of(":"));
        std::string key = kv.empty() ? header : kv[0];
        std::string value = kv.size() == 2 ? kv[1].substr(1) : std::string{};
        response->headers.emplace(key, value);
    }
    return size * nitems;
}

} // namespace test
} // namespace Dracon
