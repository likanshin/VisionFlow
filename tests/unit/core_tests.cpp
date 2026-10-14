#include "visionflow/protocol/protocol.hpp"
#include "visionflow/concurrency/thread_pool.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace visionflow;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } check(caught, "expected rejection"); }
int main() {
    try {
        const auto h = protocol::encode({protocol::Type::image, 0x0102030405060708ULL, 65536});
        check(h[8] == 1 && h[15] == 8 && h[16] == 0 && h[17] == 1, "network byte order");
        const auto decoded = protocol::decode(h.data(), h.size(), 65536);
        check(decoded.request_id == 0x0102030405060708ULL && decoded.payload_length == 65536, "header round trip");
        rejects([&] { protocol::decode(h.data(), 23, 65536); });
        rejects([&] { protocol::decode(h.data(), 24, 65535); });
        for (int offset : {0, 5, 7, 23}) {
            auto bad = h; bad[static_cast<std::size_t>(offset)] = 255;
            rejects([&] { protocol::decode(bad.data(), bad.size(), 65536); });
        }
        auto empty = protocol::encode({protocol::Type::image, 1, 0});
        rejects([&] { protocol::decode(empty.data(), empty.size(), 65536); });
        auto huge = protocol::encode({protocol::Type::image, 1, std::numeric_limits<std::uint32_t>::max()});
        rejects([&] { protocol::decode(huge.data(), huge.size(), 65536); });
        BoundedQueue<int> queue(2, 5);
        check(queue.try_push(1, 3), "push");
        check(!queue.try_push(2, 3), "byte bound");
        check(queue.try_push(2, 2) && !queue.try_push(3, 0), "item bound");
        std::vector<int> evicted;
        check(queue.push_latest(3, 4, evicted) && evicted == std::vector<int>({1, 2}), "latest eviction");
        queue.close();
        check(!queue.try_push(4, 1) && queue.pop() == 3 && !queue.pop(), "drain close");
        BoundedQueue<int> blocked(1, 1);
        auto popper = std::async(std::launch::async, [&] { return blocked.pop(); });
        blocked.close(true);
        check(popper.wait_for(std::chrono::seconds(1)) == std::future_status::ready && !popper.get(), "wake consumer on close");
        BoundedQueue<int> full(1, 1);
        full.try_push(1, 1);
        auto pusher = std::async(std::launch::async, [&] { return full.push(2, 1); });
        full.close(true);
        check(pusher.wait_for(std::chrono::seconds(1)) == std::future_status::ready && !pusher.get(), "wake producer on close");
        check(!full.pop(), "cancel queued jobs");
        BoundedQueue<int> jobs(16, 64);
        std::atomic<int> total{0};
        ThreadPool<int> pool(jobs, 4, [&](int&& i, std::size_t) { if (i == 17) throw std::runtime_error("test"); total += i; });
        for (int i = 1; i <= 100; ++i) check(jobs.push(std::move(i), 1), "pool submit");
        jobs.close(); pool.join();
        check(total == 5050 - 17 && pool.failures() == 1, "pool exception boundary and drain");
        std::cout << "protocol, bounded queue, wakeups and thread pool passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
