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


// Boost.test provides main(): this is the only translation unit which defines
// BOOST_TEST_MODULE. The whole run shares one server, started before the first
// test case and stopped after the last one.
#define BOOST_TEST_MODULE dracon_server_tests
#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <string>

#include <dracon_server.h>

extern std::string huge_data;

namespace {
struct server_fixture
{
    server_fixture()
    {
        const auto argv = boost::unit_test::framework::master_test_suite().argv;
        dracon::test::start_server(std::filesystem::canonical(std::filesystem::path(argv[0]))
                                      .parent_path().append("dracon").string());
        huge_data.reserve(50 * 1024 * 1024);
        for (int i = 0; i < 50 * 1024 * 1024; ++i)
            huge_data += char(33 + (i % 93));
    }
    ~server_fixture() { dracon::test::terminate_server(); }
};
} // namespace

BOOST_TEST_GLOBAL_FIXTURE(server_fixture);
