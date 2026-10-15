// A pipeline over channels: a producer, four worker threads, a consumer that sums.
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

template <class T>
class Channel {
    std::queue<T> items;
    std::mutex m;
    std::condition_variable changed;
    size_t cap;
    bool closed = false;

public:
    explicit Channel(size_t cap) : cap(cap) {}
    void send(T x) {
        std::unique_lock lock(m);
        changed.wait(lock, [&] { return items.size() < cap; });
        items.push(x);
        changed.notify_all();
    }
    std::optional<T> receive() {
        std::unique_lock lock(m);
        changed.wait(lock, [&] { return !items.empty() || closed; });
        if (items.empty()) return std::nullopt;
        T x = items.front();
        items.pop();
        changed.notify_all();
        return x;
    }
    void close() {
        std::lock_guard lock(m);
        closed = true;
        changed.notify_all();
    }
};

int main() {
    Channel<long long> jobs(256), results(256);
    std::thread producer([&] {
        for (long long i = 1; i <= 100000; i++) jobs.send(i);
        jobs.close();
    });
    std::vector<std::thread> workers;
    for (int k = 0; k < 4; k++)
        workers.emplace_back([&] {
            while (auto n = jobs.receive()) results.send(*n * *n % 1000);
        });
    std::thread closer([&] {
        for (auto& w : workers) w.join();
        results.close();
    });
    long long total = 0;
    while (auto r = results.receive()) total += *r;
    producer.join();
    closer.join();
    std::printf("%lld\n", total);
}
