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

#include <boost/log/attributes.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/utility/string_view.hpp>

/*!
 * \file logging.h
 *
 * Logging for plugins, on top of Boost.Log.
 *
 * A component declares one tagged logger and writes through the severity
 * macros:
 * \code
 *     namespace {
 *     dracon::tagged_logger<> logger{"customers"};
 *     }
 *
 *     INFO(logger) << "serving " << req.url();
 * \endcode
 *
 * The tag is what tells the lines of a plugin apart in the server log; the
 * sinks and the filters themselves are configured in server_logging.conf, not
 * here.
 *
 * TRACE() and DEBUG() compile to nothing unless the server was built with
 * ENABLE_TRACE_LOG or ENABLE_DEBUG_LOG, so the arguments of a trace which is
 * off are never evaluated. Every level prepends the enclosing function.
 */

/// Writes to \a logger at an explicit severity. The severity macros below are
/// the usual way in.
#define LOG BOOST_LOG_SEV

/// Swallows everything written to it, which is what TRACE() and DEBUG() become
/// when they are compiled out
struct dummy_stream
{
    template<typename T>
    inline dummy_stream &operator << (const T &) {return *this;}
};

/// Multi Thread Severity Logger
using severity_logger_mt = boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level>;
/// Single Thread Severity Logger
using severity_logger = boost::log::sources::severity_logger<boost::log::trivial::severity_level>;

/*!
 * \brief The tagged_logger class
 *
 * A logger which stamps every record with a "Tag" attribute, so that a sink
 * can format it and a filter can select on it.
 *
 * The default is the multi threaded logger, which is what a plugin wants:
 * handlers run on all the event loop threads and normally share one logger.
 * tagged_logger<severity_logger> drops the locking, for a logger which is only
 * ever written to from one thread.
 */
template <typename T = severity_logger_mt>
class tagged_logger : public T
{
public:
    /// \param value the tag, which has to outlive the logger. A string literal
    /// is the usual choice.
    tagged_logger(boost::string_view value)
    {
        T::add_attribute("Tag", boost::log::attributes::make_constant(value));
    }
};

#ifdef ENABLE_TRACE_LOG
# define TRACE(logger) LOG(logger, boost::log::trivial::trace) << __PRETTY_FUNCTION__ << " : "
#else
/// Traces through \a logger, compiled out unless ENABLE_TRACE_LOG is set
# define TRACE(logger) dummy_stream{}
#endif

#ifdef ENABLE_DEBUG_LOG
#define DEBUG(logger) LOG(logger, boost::log::trivial::debug) << __PRETTY_FUNCTION__ << " : "
#else
/// Logs a debug line through \a logger, compiled out unless ENABLE_DEBUG_LOG
/// is set
# define DEBUG(logger)  dummy_stream{}
#endif

/// Logs an informational line through \a logger
#define INFO(logger) LOG(logger, boost::log::trivial::info) << __PRETTY_FUNCTION__ << " : "
/// Logs a warning through \a logger
#define WARNING(logger) LOG(logger, boost::log::trivial::warning) << __PRETTY_FUNCTION__ << " : "
/// Logs an error through \a logger
#define ERROR(logger) LOG(logger, boost::log::trivial::error) << __PRETTY_FUNCTION__ << " : "
/// Logs a fatal error through \a logger. It only writes the record, shutting
/// down is up to the caller.
#define FATAL(logger) LOG(logger, boost::log::trivial::fatal) << __PRETTY_FUNCTION__ << " : "
