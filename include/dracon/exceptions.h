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

namespace dracon {

/*!
 * \brief The segmentation_fault_error class
 *
 * Thrown in place of a SIGSEGV raised inside a session handler, which in
 * practice means a null pointer dereference. The server installs the signal
 * handler which does the conversion, and plugins must be compiled with
 * -fnon-call-exceptions for it to work.
 *
 * Like any other exception leaving a handler it is turned into a response, so
 * what() is sent to the client. Keep the message generic and log the details.
 */
class segmentation_fault_error : public std::runtime_error
{
public:
  explicit segmentation_fault_error(const std::string &what)
        : std::runtime_error(what){}
  explicit segmentation_fault_error(const char *what)
        : std::runtime_error(what){}
};

/*!
 * \brief The floating_point_error class
 *
 * Thrown in place of a SIGFPE raised inside a session handler, which in
 * practice means an integer division by zero. The same rules as for
 * segmentation_fault_error apply.
 */
class floating_point_error : public std::runtime_error
{
public:
  explicit floating_point_error(const std::string &what)
        : std::runtime_error(what){}
  explicit floating_point_error(const char *what)
        : std::runtime_error(what){}
};

} // namespace Dracon
