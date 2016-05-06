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

#include "server_plugin.h"
#include "server_logger.h"

#include <dlfcn.h>

#include <memory>
#include <stdexcept>

namespace dracon::internal {
/*!
 * \brief server_plugin::server_plugin
 *
 * Try to load a plugin file
 *
 * \param path to plugin
 */
server_plugin::server_plugin(const std::string &path, const std::string &conf_dir)
{
    TRACE(server_logger) << "ServerPlugin loading: " << path << " confDir:" << conf_dir;
    int flags = RTLD_NOW | RTLD_LOCAL;
#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    flags |= RTLD_DEEPBIND;
#endif
    m_handler = std::shared_ptr<void>(dlopen(path.c_str(), flags), [](void *ptr) {
        if (ptr) {
            auto destroy = dracon::destory_plugin_type(dlsym(ptr, "destory_plugin"));
            if (destroy)
                destroy();
            dlclose(ptr);
        }
    });

    if (!m_handler)
        throw std::runtime_error{dlerror()};

    auto init = dracon::init_plugin_type(dlsym(m_handler.get(), "init_plugin"));
    if (init && !init(conf_dir))
        throw std::runtime_error{"initPlugin failed"};

    create_session = dracon::create_session_type(dlsym(m_handler.get(), "create_session"));
    if (!create_session)
        throw std::runtime_error{"Can't find create_session function"};

    auto order = dracon::plugin_order_type(dlsym(m_handler.get(), "plugin_order"));
    if (!order)
        throw std::runtime_error{"Can't find plugin_order function"};
    m_order = order();
}

/*!
 * \brief server_plugin::server_plugin
 *
 * \param create_session function pointer
 */
server_plugin::server_plugin(dracon::create_session_type func_ptr, uint32_t order)
 : create_session(func_ptr)
 , m_order(order)
{}

} // namespace dracon::internal
