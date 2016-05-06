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

// Standalone throughput / latency benchmark. It starts its own Dracon
// instance and drives the plaintext port with raw sockets, so it measures the
// server and its parser, not libcurl. Not part of `ctest` (it is a tool, not a
// pass/fail gate); run it by hand:
//
//     ./dracon_benchmark [seconds] [connections]
//
// It reports request throughput and latency for small keep-alive GETs, the
// same pipelined, and the bandwidth of a 50 MB response.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <dracon_server.h>
#include <raw_client.h>

using namespace std::chrono;
using dracon::test::raw_client;

namespace {

double to_sec(steady_clock::duration d)
{
    return duration<double>(d).count();
}

void print_latency(std::vector<double> &us)
{
    if (us.empty())
        return;
    std::sort(us.begin(), us.end());
    auto pct = [&](double p) { return us[std::min(us.size() - 1, size_t(p * us.size()))]; };
    printf("    latency (us): p50=%.1f p90=%.1f p99=%.1f max=%.1f\n",
           pct(0.50), pct(0.90), pct(0.99), us.back());
}

// Small keep-alive GETs, one persistent connection per worker thread.
void bench_keep_alive(int connections, std::chrono::seconds dur)
{
    printf("[keep-alive GET /test0] %d connections, %llds\n", connections, (long long)dur.count());
    std::atomic<uint64_t> total{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    std::vector<std::vector<double>> lat(connections);
    const auto begin = steady_clock::now();
    for (int t = 0; t < connections; ++t) {
        threads.emplace_back([&, t] {
            try {
                raw_client c(8080, 5s);
                uint64_t n = 0;
                auto &samples = lat[t];
                while (!stop.load(std::memory_order_relaxed)) {
                    const auto t0 = steady_clock::now();
                    if (!c.send("GET /test0 HTTP/1.1\r\nHost: x\r\n\r\n"))
                        break;
                    if (c.recv_responses(1).empty())
                        break;
                    samples.push_back(duration<double, std::micro>(steady_clock::now() - t0).count());
                    ++n;
                }
                total.fetch_add(n, std::memory_order_relaxed);
            } catch (const std::exception &e) {
                fprintf(stderr, "  worker %d: %s\n", t, e.what());
            }
        });
    }
    std::this_thread::sleep_for(dur);
    stop.store(true);
    for (auto &th : threads)
        th.join();
    const double elapsed = to_sec(steady_clock::now() - begin);
    std::vector<double> all;
    for (auto &v : lat)
        all.insert(all.end(), v.begin(), v.end());
    printf("    %.0f req/s (%llu requests in %.2fs)\n", total.load() / elapsed, (unsigned long long)total.load(), elapsed);
    print_latency(all);
}

// depth-64 pipelined GETs per connection: stresses the parser's pipelining path.
void bench_pipelined(int connections, std::chrono::seconds dur)
{
    printf("[pipelined GET /test0 x64] %d connections, %llds\n", connections, (long long)dur.count());
    constexpr int depth = 64;
    std::string batch;
    for (int i = 0; i < depth; ++i)
        batch += "GET /test0 HTTP/1.1\r\nHost: x\r\n\r\n";
    std::atomic<uint64_t> total{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    const auto begin = steady_clock::now();
    for (int t = 0; t < connections; ++t) {
        threads.emplace_back([&] {
            try {
                raw_client c(8080, 5s);
                uint64_t n = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    if (!c.send(batch))
                        break;
                    if (raw_client::response_count(c.recv_responses(depth)) < depth)
                        break;
                    n += depth;
                }
                total.fetch_add(n, std::memory_order_relaxed);
            } catch (...) {}
        });
    }
    std::this_thread::sleep_for(dur);
    stop.store(true);
    for (auto &th : threads)
        th.join();
    printf("    %.0f req/s\n", total.load() / to_sec(steady_clock::now() - begin));
}

// Bandwidth of a large response body.
void bench_large_body(int connections, int iterations)
{
    printf("[GET /test50m] %d connections x %d\n", connections, iterations);
    std::atomic<uint64_t> bytes{0};
    std::vector<std::thread> threads;
    const auto begin = steady_clock::now();
    for (int t = 0; t < connections; ++t) {
        threads.emplace_back([&] {
            try {
                uint64_t b = 0;
                for (int i = 0; i < iterations; ++i) {
                    // One fresh connection per transfer with Connection: close, so
                    // recv_all() drains the whole body to EOF in O(n) (recv_responses
                    // only counts heads and would return after the first bytes).
                    raw_client c(8080, 30s);
                    if (!c.send("GET /test50m HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"))
                        break;
                    b += c.recv_all().size();
                }
                bytes.fetch_add(b, std::memory_order_relaxed);
            } catch (...) {}
        });
    }
    for (auto &th : threads)
        th.join();
    const double elapsed = to_sec(steady_clock::now() - begin);
    printf("    %.1f MB/s (%.1f MB in %.2fs)\n",
           bytes.load() / elapsed / (1024 * 1024), bytes.load() / double(1024 * 1024), elapsed);
}

} // namespace

int main(int argc, char **argv)
{
    const std::chrono::seconds dur{argc > 1 ? std::max(1, atoi(argv[1])) : 3};
    const int connections = argc > 2 ? std::max(1, atoi(argv[2])) : int(std::thread::hardware_concurrency());

    const auto exe = std::filesystem::canonical(std::filesystem::path(argv[0])).parent_path() / "dracon";
    dracon::test::start_server(exe.string());
    std::this_thread::sleep_for(500ms); // let the test plugin build its buffers

    printf("Dracon benchmark — %d connections\n\n", connections);
    bench_keep_alive(connections, dur);
    bench_pipelined(connections, dur);
    bench_large_body(std::min(connections, 4), 4);

    dracon::test::terminate_server();
    return 0;
}
