/*
    Copyright (C) 2022 by BogDan Vatra <bogdan@kde.org>

    Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <charconv>
#include <filesystem>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <dracon/http.h>
#include <dracon/logging.h>
#include <dracon/plugin.h>
#include <dracon/restful.h>
#include <dracon/stream.h>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

tagged_logger<> g_logger{"MyCoolProject"};

namespace {
using namespace std::string_literals;
using namespace std::string_view_literals;

dracon::restful_router s_restull_v1_root_node("/v1/");
std::vector<std::string> s_devices;
std::shared_mutex s_mutex; // dracon is a highly concurrent HTTP server,
                           // therefore all resources must be protected properly

/// Biggest request body this plugin is willing to buffer
constexpr size_t max_body_size = 512 * 1024;

/*!
 * \brief device_index
 *
 * The captured resources are raw URL pieces, anything at all can be in them,
 * so they must be validated instead of parsed with something which throws:
 * std::stoul() would end up as a 500 (and its message in the response body),
 * while a client which asks for /v1/devices/bla made a bad *request*.
 */
size_t device_index(const std::string &device)
{
    size_t index = 0;
    const auto res = std::from_chars(device.data(), device.data() + device.size(), index);
    if (res.ec != std::errc{} || res.ptr != device.data() + device.size())
        throw dracon::response{400, "Invalid device id"sv};
    return index;
}

/*!
 * \brief parse_body
 *
 * Same story for the body: nlohmann throws on malformed input, and letting that
 * exception out would answer 500 with the parser's message (which quotes the
 * body) in it. @see include/dracon/plugin.h "ERROR HANDLING"
 */
json parse_body(std::string_view body)
{
    try {
        return json::parse(body);
    } catch (const std::exception &e) {
        DEBUG(g_logger) << "invalid json body: " << e.what();
        throw dracon::response{400, "Invalid JSON body"sv};
    }
}

/// Reads the request body, at most \a max_body_size of it
std::string read_body(dracon::abstract_stream &stream, dracon::request &req)
{
    std::string body;
    req.append_body_callback([&body](std::string_view buff) {
        body.append(buff);
    }, max_body_size); // the server answers 413 by itself if it's bigger
    stream >> req;
    return body;
}

json device_json(size_t index, const std::string &name)
{
    return {{"id", index}, {"name", name}};
}
} // namespace

/*!
 * Serves both /v1/devices and /v1/devices/{device}: the captured resource
 * arrives as an argument.
 *
 * It is a std::optional here so that "the route which matched has no device"
 * reads differently from "the device is an empty string": a plain
 * const std::string & would give an empty string for both. All the captures of
 * a handler have to be optional for that, mixed with plain strings they all
 * behave as strings.
 * @see dracon::restful_route::add_method_handler
 */
void get_devices(dracon::abstract_stream &stream, dracon::request &req,
                const std::optional<std::string> &device = {})
{
    // The request at this point is partial, next line will read the rest of the request
    stream >> req;

    // Validate before taking the lock: device_index() throws on a bad id
    const auto index = device ? device_index(*device) : 0;

    json res = json::array();
    {
        std::shared_lock lock{s_mutex};
        if (!device) {
            for (size_t i = 0; i < s_devices.size(); ++i)
                res.push_back(device_json(i, s_devices[i]));
        } else {
            if (index >= s_devices.size())
                throw dracon::response{404};
            res.push_back(device_json(index, s_devices[index]));
        }
    } // don't keep the mutex locked while we're sending the data

#ifndef USE_CHUNKED
    stream << dracon::response{200 /* res code */,
                               res.dump(), /* body */
                               {{"Content-Type","application/json"}} /* headers */
                              };
#else
    // the following code is doing the same as the above one
    // is here only to show how to do chunked data transfer
    // (build with -DUSE_CHUNKED to get this branch)
    // more examples, including processing the data in an worker
    // thread, you can find in Dracon's test plugin
    stream << dracon::response{200, {}, {{"Content-Type","application/json"}}}.set_content_length(dracon::chunked_data);
    dracon::chunked_stream chunked_stream{stream};
    dracon::ostream_buffer buff{chunked_stream};
    std::ostream str{&buff};
    str << res.dump();
#endif
}

void post_devices(dracon::abstract_stream &stream, dracon::request &req)
{
    // The request at this point is partial,
    // next lines will read the rest of the request including the body
    const auto jb = parse_body(read_body(stream, req));
    if (!jb.is_array())
        throw dracon::response{400, "Expected an array of device names"sv};

    // Validate everything *before* touching the shared state, or a bad element
    // in the middle would leave it half updated
    std::vector<std::string> devices;
    devices.reserve(jb.size());
    for (const auto &jv : jb) {
        if (!jv.is_string())
            throw dracon::response{400, "Expected an array of device names"sv};
        devices.push_back(jv.get<std::string>());
    }

    {
        std::unique_lock lock{s_mutex};
        s_devices = std::move(devices);
    } // don't keep the mutex locked while we're sending the data

    stream << dracon::response{200};
}

void patch_device(dracon::abstract_stream &stream, dracon::request &req, const std::string &device)
{
    // The request at this point is partial,
    // next lines will read the rest of the request including the body
    const auto index = device_index(device);
    const auto jb = parse_body(read_body(stream, req));
    const auto name = jb.find("name");
    if (name == jb.end() || !name->is_string())
        throw dracon::response{400, "Expected a \"name\" string"sv};

    {
        std::unique_lock lock{s_mutex};
        if (index >= s_devices.size())
            throw dracon::response{404};
        s_devices[index] = name->get<std::string>();
    } // don't keep the mutex locked while we're sending the data

    stream << dracon::response{200};
}

void delete_device(dracon::abstract_stream &stream, dracon::request &req, const std::string &device)
{
    // The request at this point is partial, next line will read the rest of the request
    stream >> req;

    const auto index = device_index(device);
    {
        std::unique_lock lock{s_mutex};
        if (index >= s_devices.size())
            throw dracon::response{404};
        s_devices.erase(s_devices.begin() + index);
    } // don't keep the mutex locked while we're sending the data
    stream << dracon::response{200};
}

std::string g_token_algorithm;
std::string g_token_secret;

PLUGIN_EXPORT bool init_plugin(const std::string &conf_dir)
{
    try {
        INFO(g_logger) << "Initializing REST API plugin ...";
        const std::unordered_set<std::string> supported_algorithms{"HS256", "HS384", "HS512"};

        namespace pt = boost::property_tree;
        pt::ptree properties;
        const auto conf_path = std::filesystem::path(conf_dir).append("MyCoolProject.conf");
        DEBUG(g_logger) << "Loading conf file from " << conf_path;

        pt::read_info(conf_path.string(), properties);
        g_token_algorithm = properties.get<std::string>("signing.algorithm", "HS256");
        boost::to_upper(g_token_algorithm);
        if (supported_algorithms.find(g_token_algorithm) == supported_algorithms.end()) {
            FATAL(g_logger) << "Invalid algorithm " << g_token_algorithm;
            throw std::runtime_error{"Invalid algorithm"};
        }
        g_token_secret = properties.get<std::string>("signing.secret", std::string{});
        if (g_token_secret.empty()) {
            // A signing secret must come from the system entropy pool. A
            // pseudo random generator (mt19937 & friends) is reproducible from
            // a handful of its own outputs, which is exactly what an attacker
            // who holds a few of the tokens it signed has.
            std::random_device rd;
            std::uniform_int_distribution<unsigned> byte{0, 255};
            g_token_secret.resize(32);
            for (auto &c : g_token_secret)
                c = char(byte(rd));
        }
        // do something with properties.get<std::string>("postgresql.connection_string")

        // v1 routes, here you can create highly complicated routes.

        // devices
        s_restull_v1_root_node.create_route("devices")
                ->add_method_handler("GET", get_devices)
                .add_method_handler("POST", post_devices);

        // devices/{device} - get_devices serves this one too, {device} lands in
        // its third argument. The "devices" route above has no capture, so the
        // same argument arrives there as a nullopt.
        s_restull_v1_root_node.create_route("devices/{device}")
                ->add_method_handler("GET", get_devices)
                .add_method_handler("PATCH", patch_device)
                .add_method_handler("DELETE", delete_device);

    } catch (const std::exception &e) {
        FATAL(g_logger) << e.what();
        return false;
    } catch (...) {
        FATAL(g_logger) << "Unknown fatal error";
        return false;
    }
    INFO(g_logger) << " ... completed";
    return true;
}

PLUGIN_EXPORT uint32_t plugin_order()
{
    // The server calls this function to get the plugin order
    return 0;
}

// The signature must match dracon::create_session_type, the server resolves it
// with dlsym() and there is nothing to typecheck it against
PLUGIN_EXPORT dracon::http_session create_session(const dracon::request &req)
{
    return s_restull_v1_root_node.create_handler(req.url(), req.method());
}

PLUGIN_EXPORT void destory_plugin()
{
    // This function is called by the server when it closes. The plugin should wait in this function until it finishes the clean up.
}
