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
#include <dracon/http.h>
#include <dracon/logging.h>
#include <dracon/plugin.h>
#include <dracon/utils.h>

#include <filesystem>

#include <boost/algorithm/string.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace {
using namespace std::string_view_literals;

class file_map
{
public:
    file_map(const std::filesystem::path &path, size_t size, std::filesystem::file_time_type last_write_time)
        : m_last_write_time(last_write_time)
    {
        if (size) {
            m_mapped_file = std::make_unique<boost::iostreams::mapped_file_source>(path);
            m_size = m_mapped_file->size();
            m_data = m_mapped_file->data();
        }
    }
    ~file_map() { if (m_mapped_file) m_mapped_file->close(); }

    inline size_t size() const { return m_size; }
    inline const char* data() const { return m_data; }
    inline std::filesystem::file_time_type last_write_time() const { return m_last_write_time; }

private:
    size_t m_size = 0;
    const char* m_data = nullptr;
    std::filesystem::file_time_type m_last_write_time;
    std::unique_ptr<boost::iostreams::mapped_file_source> m_mapped_file;
};
using file_map_ptr = std::shared_ptr<file_map>;
dracon::lru_cache<std::string, file_map_ptr> s_files_cache{100};

std::mutex s_files_cache_mutex;
std::string s_default_file;
std::vector<std::pair<std::string, std::string>> s_urls;
bool s_allow_symlinks = false;
tagged_logger<> logger{"staticContent"};
std::unique_ptr<dracon::simple_timer> g_timer;
dracon::fields s_custom_fields;

// Returns a view of a literal: this used to build a std::string on every
// single request
inline std::string_view mime_type(std::string_view ext)
{
    if (ext == ".htm"sv)  return "text/html"sv;
    if (ext == ".html"sv) return "text/html"sv;
    if (ext == ".php"sv)  return "text/html"sv;
    if (ext == ".css"sv)  return "text/css"sv;
    if (ext == ".js"sv)   return "application/javascript"sv;
    if (ext == ".json"sv) return "application/json"sv;
    if (ext == ".xml"sv)  return "application/xml"sv;
    if (ext == ".png"sv)  return "image/png"sv;
    if (ext == ".jpe"sv)  return "image/jpeg"sv;
    if (ext == ".jpeg"sv) return "image/jpeg"sv;
    if (ext == ".jpg"sv)  return "image/jpeg"sv;
    if (ext == ".gif"sv)  return "image/gif"sv;
    if (ext == ".bmp"sv)  return "image/bmp"sv;
    if (ext == ".tiff"sv) return "image/tiff"sv;
    if (ext == ".tif"sv)  return "image/tiff"sv;
    if (ext == ".svg"sv)  return "image/svg+xml"sv;
    if (ext == ".svgz"sv) return "image/svg+xml"sv;
    if (ext == ".txt"sv)  return "text/plain"sv;
    if (ext == ".webp"sv)  return "image/webp"sv;
    if (ext == ".webm"sv)  return "video/webmx"sv;
    if (ext == ".weba"sv)  return "audio/webm"sv;
    if (ext == ".swf"sv)  return "application/x-shockwave-flash"sv;
    if (ext == ".flv"sv)  return "video/x-flv"sv;
    return "application/octet-stream"sv;
}

void static_content_session(const std::filesystem::path &root, const std::filesystem::path &path, bool head, dracon::abstract_stream& stream, dracon::request& req)
{
    namespace fs = std::filesystem;
    stream >> req;

    // Every filesystem error is answered with a bare 404: the error messages
    // carry the resolved absolute path, and telling "missing" apart from
    // "exists but outside the root" turns the plugin into a filesystem oracle.
    std::error_code ec;
    auto p = (root / path).lexically_normal();
    if (!s_allow_symlinks) {
        p = fs::canonical(p, ec);
        if (ec)
            throw dracon::response{404};
    }
    if (!boost::starts_with(p, root)) { // make sure we don't server files outside the root
        WARNING(logger) << "path \"" << p << "\" is outside the root \"" << root << "\"";
        throw dracon::response{404};
    }
    auto status = fs::status(p, ec);
    if (!ec && fs::is_directory(status)) {
        p /= s_default_file;
        status = fs::status(p, ec);
    }
    if (ec || !fs::is_regular_file(status))
        throw dracon::response{404};
    TRACE(logger) << "Serving " << p.string();

    const auto size = fs::file_size(p, ec);
    if (ec)
        throw dracon::response{404};
    const auto last_write_time = fs::last_write_time(p, ec);
    if (ec)
        throw dracon::response{404};

    file_map_ptr file;
    {
        // The lock *must* be released before writing anything: writing yields
        // back to the event loop, and holding it across a yield deadlocks every
        // other session served by the same loop thread.
        std::unique_lock<std::mutex> lock{s_files_cache_mutex};
        file = s_files_cache.value(p.string());
        if (!file || file->last_write_time() != last_write_time) {
            try {
                file = std::make_shared<file_map>(p, size, last_write_time);
            } catch (const std::exception &e) {
                WARNING(logger) << "can't map \"" << p << "\": " << e.what();
                throw dracon::response{404};
            }
            s_files_cache.put(p.string(), file);
        }
    }

    {
        dracon::response res{200};
        static_cast<dracon::fields&>(res) = s_custom_fields;
        res["Content-Type"] = mime_type(p.extension().string());
        res.set_content_length(file->size());
        stream << res;
    }
    if (!head)
        stream.write({file->data(), file->size()});
}

} // namespace

PLUGIN_EXPORT dracon::http_session create_session(const dracon::request &req) {
    if (req.method() != "GET"sv && req.method() != "HEAD"sv)
        return {};
    auto &url = req.url();
    for (const auto &pair : s_urls) {
        if (boost::starts_with(url, pair.first)) {
            if (boost::starts_with(pair.first, "/~"sv)) {
                auto pos = url.find('/', 1);
                if (pos == std::string::npos)
                    pos = url.size();
                std::filesystem::path root_path{pair.second};
                auto user = dracon::unescape_url(url.substr(2, pos - 2));
                if (user == "."sv || user == ".."sv)
                    break; // avoid GET /~../../etc/passwd HTTP/1.0 requests

                root_path /= user;
                root_path /= "public_html";
                std::filesystem::path file_path;
                if (url.size() - pos > 1)
                    file_path /= dracon::unescape_url(url.substr(pos + 1, url.size() - pos - 1));
                return std::bind<void>(static_content_session,
                                       root_path,
                                       file_path.lexically_normal(),
                                       req.method() == "HEAD"sv,
                                       std::placeholders::_1,
                                       std::placeholders::_2);
            } else {
                return std::bind<void>(static_content_session,
                                       std::filesystem::path{pair.second},
                                       std::filesystem::path{dracon::unescape_url(url.c_str() + pair.first.size())}.lexically_normal(),
                                       req.method() == "HEAD"sv,
                                       std::placeholders::_1,
                                       std::placeholders::_2);
            }
        }
    }
    return {};
}

PLUGIN_EXPORT bool init_plugin(const std::string &conf_dir)
{
    using namespace std::chrono_literals;
    INFO(logger) << "Initializing plugin";
    namespace pt = boost::property_tree;
    pt::ptree properties;
    pt::read_info(std::filesystem::path(conf_dir).append("static_files.conf").string(), properties);
    for (const auto &p : properties.get_child("paths")) {
        DEBUG(logger) << "Mapping \"" << p.first << "\" to \"" << p.second.get_value<std::string>() << "\"";
        s_urls.emplace_back(std::make_pair(p.first, p.second.get_value<std::string>()));
    }

    for (const auto &p : properties.get_child("custom_headers")) {
        DEBUG(logger) << "Custom header " << p.first << " : " << p.second.get_value<std::string>();
        s_custom_fields[p.first] = p.second.get_value<std::string>();
    }

    s_default_file = properties.get("default_file", "");
    s_allow_symlinks = properties.get("allow_symlinks", false);
    g_timer = std::make_unique<dracon::simple_timer>([]{
        std::unique_lock<std::mutex> lock(s_files_cache_mutex);
        for (auto it = s_files_cache.begin(); it != s_files_cache.end();) {
            if (it->second.use_count() == 1)
                it = s_files_cache.erase(it);
            else
                ++it;
        }
    }, 60s);
    return !s_urls.empty();
}

PLUGIN_EXPORT uint32_t plugin_order()
{
    return UINT32_MAX;
}

PLUGIN_EXPORT void destory_plugin()
{
    g_timer.reset();
}
