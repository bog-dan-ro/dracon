/*
    Copyright (C) 2022 by BogDan Vatra <bogdan@kde.org>

    Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/


#define BOOST_TEST_MODULE my_cool_project_tests
#include <boost/test/unit_test.hpp>

#include <filesystem>

#include <dracon_server.h>

namespace {
struct server_fixture
{
    server_fixture()
    {
        const auto argv = boost::unit_test::framework::master_test_suite().argv;
        dracon::test::start_server(std::filesystem::canonical(argv[0]).parent_path() / "dracon");
    }
    ~server_fixture() { dracon::test::terminate_server(); }
};
} // namespace

BOOST_TEST_GLOBAL_FIXTURE(server_fixture);
