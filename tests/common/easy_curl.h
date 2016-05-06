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

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <curl/curl.h>

namespace dracon {
namespace test {

class easy_curl
{
public:
    using header_map = std::unordered_map<std::string, std::string>;
    struct response
    {
        std::string status;
        header_map headers;
        std::string body;
    };
public:
    easy_curl();
    ~easy_curl();
    easy_curl &set_url(const std::string &url);
    easy_curl &set_headers(const header_map &headers);
    response request(const std::string &method, std::string upload = {}) const;
    inline response get() const { return request("GET"); }
    inline response del() const { return request("DELETE"); }
    inline response opt() const { return request("OPTIONS"); }
    inline response post(const std::string &upload) const { return request("POST", upload); }
    inline response put(const std::string &upload) const { return request("PUT", upload); }
    template<typename ...Args>
    void set_options(CURLoption option, Args ...args) const {
        if (curl_easy_setopt(m_curl, option, args...) != CURLE_OK)
            throw std::runtime_error{"Can't curl_easy_setopt"};
    }
    void ingnore_invalid_ssl_certificate() {
        set_options(CURLOPT_SSL_VERIFYPEER, 0L);
        set_options(CURLOPT_SSL_VERIFYHOST, 0L);
    }
    static std::string escape(std::string_view str);

private:
    static size_t read_callback(char *buffer, size_t size, size_t nitems, std::string *upload);
    static size_t write_callback(char *ptr, size_t size, size_t nmemb, response *self);
    static size_t header_callback(char *buffer, size_t size, size_t nitems, response *response);
private:
    CURL *m_curl;
    curl_slist *m_headers_list = nullptr;
};

} // namespace test
} // namespace Dracon
