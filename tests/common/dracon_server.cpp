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

#include "dracon_server.h"

#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>

namespace dracon {
namespace test {

static FILE * s_dracon_handle = nullptr;
static pid_t s_dracon_pid = -1;

static pid_t pidof(const char *name)
{
    std::string cmd{"pidof "};
    cmd += name;
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp)
        return 0;
    char buff[200];
    const auto read = fread(buff, 1, sizeof(buff) - 1, fp);
    buff[read] = 0;
    pclose(fp);
    return atoi(buff);
}

void start_server(const std::string &path)
{
    if (pidof("dracon"))
        return;

    s_dracon_handle = popen((path + " --pid").c_str(), "r");
    if (!s_dracon_handle) {
        std::cerr << "Can't start Dracon\n";
        exit(1);
    }
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    using clock = std::chrono::system_clock;
    auto start = clock::now();
    // wait until dracon starts
    while (fgets(buf, sizeof(buf), s_dracon_handle)) {
        std::string output = buf;
        if (s_dracon_pid == -1) {
            // extract PID
            auto pos = output.find('\n');
            if (pos != std::string::npos && output.substr(0, 4) == "pid:") {
                s_dracon_pid = std::strtoll(output.substr(4, pos - 4).c_str(), nullptr, 10);
                break;
            }
        }
        using namespace std::chrono_literals;
        if (clock::now() - start > 10s) {
            std::cerr << "Can't start Dracon\n";
            exit(1);
        }
    }
}

void terminate_server()
{
    if (s_dracon_pid != -1) {
        kill(s_dracon_pid, SIGTERM);
        char buf[1024];
        while (fgets(buf, sizeof(buf), s_dracon_handle));
        pclose(s_dracon_handle);
    }
}

} // namespace test
} // namespace Dracon
