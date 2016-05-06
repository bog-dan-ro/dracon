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

#include <boost/test/unit_test.hpp>
#include <dracon/thread_worker.h>
#include <dracon/utils.h>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace dracon;
using namespace std;

    BOOST_AUTO_TEST_SUITE(utils)

    BOOST_AUTO_TEST_CASE(from_hex_digits)
    {
        BOOST_CHECK_EQUAL(from_hex('0'), 0);
        BOOST_CHECK_EQUAL(from_hex('5'), 5);
        BOOST_CHECK_EQUAL(from_hex('9'), 9);

        BOOST_CHECK_EQUAL(from_hex('a'), 10);
        BOOST_CHECK_EQUAL(from_hex('d'), 13);
        BOOST_CHECK_EQUAL(from_hex('f'), 15);

        BOOST_CHECK_EQUAL(from_hex('A'), 10);
        BOOST_CHECK_EQUAL(from_hex('D'), 13);
        BOOST_CHECK_EQUAL(from_hex('F'), 15);

        // Bad hex value, should throw an exception
        BOOST_CHECK_THROW(from_hex('H'), std::invalid_argument);
    }

    BOOST_AUTO_TEST_CASE(unescape_url_decodes)
    {
        BOOST_CHECK_EQUAL(unescape_url("plainText"), "plainText");
        BOOST_CHECK_EQUAL(unescape_url("--%3D%3D+c%C3%A2nd+%229+%22+%2B+1+nu+fac+%2210%22+%3F+%3D+%21%40%23%24%25%5E%26%2A%3F%3E%3C%3A%27%5C%7C%5D%5B%60%7E+%21+%2A+%27+%28+%29+%3B+%3A+%40+%26+%3D+%2B+%24+%2C+%2F+%3F+%25+%23+%5B+%5D%3D%3D--"),
                  std::string{R"(--== când "9 " + 1 nu fac "10" ? = !@#$%^&*?><:'\|][`~ ! * ' ( ) ; : @ & = + $ , / ? % # [ ]==--)"});
        BOOST_CHECK_EQUAL(unescape_url("%20"), " ");
        BOOST_CHECK_THROW(unescape_url("plain%2hText"), std::invalid_argument);
        BOOST_CHECK_THROW(unescape_url("Text%2"), std::invalid_argument);
    }

    BOOST_AUTO_TEST_CASE(split_parts)
    {
        const string str = unescape_url("///api/v1/parents/123/children/");
        split_vector expected = {"api", "v1", "parents", "123", "children"};
        auto splitted = split(str, '/');
        BOOST_CHECK_EQUAL(splitted.size(), expected.size());
        for (size_t i = 0; i < splitted.size(); ++i)
            BOOST_CHECK_EQUAL(expected[i], splitted[i]);
        const string str1 = "user:pass,:@!%^\":><?><<<";
        split_vector expected1 = {"user", "pass,:@!%^\":><?><<<"};
        splitted = split(str1, ':', 1);
        BOOST_CHECK_EQUAL(splitted.size(), expected1.size());
        for (size_t i = 0; i < splitted.size(); ++i)
            BOOST_CHECK_EQUAL(expected1[i], splitted[i]);
    }

    BOOST_AUTO_TEST_CASE(lru_cache_evicts_oldest)
    {
        using ptr = std::shared_ptr<int>;
        dracon::lru_cache<int, ptr> cache{2};
        std::vector<ptr> all{10};
        for (auto & p : all)
            p = std::make_shared<int>();
        cache.put(2, all[2]);
        BOOST_CHECK_EQUAL(cache.reference(2).use_count(), 2);
        BOOST_CHECK_EQUAL(cache.reference(2).get(), all[2].get());

        cache.put(1, all[1]);
        BOOST_CHECK_EQUAL(cache.reference(1).use_count(), 2);
        BOOST_CHECK_EQUAL(cache.reference(1).get(), all[1].get());

        // Move 2 to the top
        BOOST_CHECK_EQUAL(cache.reference(2).get(), all[2].get());

        cache.put(0, all[0]);
        BOOST_CHECK_EQUAL(cache.reference(0).use_count(), 2);
        BOOST_CHECK_EQUAL(cache.reference(0).get(), all[0].get());

        // 1 should be out from cache
        BOOST_CHECK_EQUAL(all[1].use_count(), 1);

        cache.put(0, all[3]);
        BOOST_CHECK_EQUAL(cache.reference(0).use_count(), 2);
        BOOST_CHECK_EQUAL(cache.reference(0).get(), all[3].get());
        BOOST_CHECK_EQUAL(all[0].use_count(), 1);

        cache.clear();
        for (size_t i = 0; i <all.size(); ++i) {
            BOOST_CHECK_EQUAL(all[i].use_count(), 1);
            cache.put(i % 2, all[i]);
        }

        BOOST_CHECK_EQUAL(cache.reference(0).use_count(), 2);
        BOOST_CHECK_EQUAL(cache.reference(0).get(), all[8].get());
        BOOST_CHECK_EQUAL(cache.reference(1).use_count(), 2);
        BOOST_CHECK_EQUAL(cache.reference(1).get(), all[9].get());

        for (auto it = cache.begin(); it != cache.end();)
            it = cache.erase(it);

        BOOST_CHECK_EQUAL(cache.size(), 0);
    }

    BOOST_AUTO_TEST_CASE(simple_timer_repeats)
    {
        using namespace std::chrono_literals;
        auto start = std::chrono::system_clock::now();
        std::condition_variable time_out_wait;
        std::mutex mutex;
        simple_timer st{[&]{time_out_wait.notify_one();}, 50ms};
        std::unique_lock<std::mutex> lock(mutex);
        time_out_wait.wait(lock);
        time_out_wait.wait(lock);
        BOOST_CHECK(std::chrono::system_clock::now() >= start + 100ms);
    }

    BOOST_AUTO_TEST_CASE(simple_timer_single_shot)
    {
        using namespace std::chrono_literals;
        auto start = std::chrono::system_clock::now();
        std::condition_variable time_out_wait;
        std::mutex mutex;
        simple_timer st{[&]{time_out_wait.notify_one();}, 50ms, true};
        std::unique_lock<std::mutex> lock(mutex);
        time_out_wait.wait(lock);
        BOOST_CHECK(std::chrono::system_clock::now() >= start + 50ms);
        BOOST_CHECK(time_out_wait.wait_for(lock, 100ms) == std::cv_status::timeout);
    }

    BOOST_AUTO_TEST_SUITE_END()

    BOOST_AUTO_TEST_SUITE(thread_worker_tests)

    BOOST_AUTO_TEST_CASE(runs_tasks_in_order)
    {
        using namespace std::chrono_literals;
        thread_worker worker{1};
        std::promise<void> gate;
        auto gate_future = gate.get_future().share();

        std::mutex mutex;
        std::vector<int> order;
        std::atomic<int> done{0};

        worker.insert_task([gate_future] { gate_future.wait(); });
        std::this_thread::sleep_for(50ms);

        constexpr int count = 5;
        for (int i = 0; i < count; ++i) {
            worker.insert_task([i, &mutex, &order, &done] {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    order.push_back(i);
                }
                ++done;
            });
        }
        std::this_thread::sleep_for(20ms);
        gate.set_value();

        for (int spin = 0; done.load() < count && spin < 1000; ++spin)
            std::this_thread::sleep_for(5ms);

        BOOST_REQUIRE_EQUAL(order.size(), size_t(count));
        BOOST_CHECK((order == std::vector<int>{0, 1, 2, 3, 4}));
    }

BOOST_AUTO_TEST_SUITE_END()
}
