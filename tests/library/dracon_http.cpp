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

#include <boost/test/unit_test.hpp>

#include <string>

#include <dracon/http.h>

BOOST_AUTO_TEST_SUITE(response)

BOOST_AUTO_TEST_CASE(does_not_duplicate_framing_headers)
{
    dracon::response res{200, "body", {{"Content-Length", "999"}}};
    const std::string response = res.to_string();

    size_t count = 0;
    for (size_t pos = 0; (pos = response.find("Content-Length", pos)) != std::string::npos; ++pos)
        ++count;
    BOOST_CHECK_MESSAGE(count == 1u, "response:\n" << response);
}

BOOST_AUTO_TEST_SUITE_END()
