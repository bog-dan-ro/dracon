#include "utils.h"

std::string url(const std::string &type, const std::string &path)
{
    if (type == "http")
        return "http://localhost:8080" + path;
    return "https://localhost:8443" + path;
}

/*!
 * \brief duration
 *
 * Budget, in milliseconds, for a handful of sequential requests.
 *
 * Keep these generous. What they are here to catch is a *stall*: a lost wakeup
 * used to cost five minutes, a session which is not resumed costs a five second
 * timeout, a connection which is not reused costs a reconnect. none of that
 * needs a tight budget, and a tight one only buys flaky runs on a loaded
 * machine or inside a container.
 */
long duration(const std::string &type, long time)
{
    if (type == "https")
        return time + 200; // SSL handshake takes a lot of time :(
    return time;
}
