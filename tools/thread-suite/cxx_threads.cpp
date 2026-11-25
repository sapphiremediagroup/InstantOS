// cxx-threads: the C++ standard library's threads on mlibc in InstantOS (libc++
// from tools/build-libcxx.sh), and the compiler's guards of function statics.
// Run by tools/run-thread-suite.sh.
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
static int total, failed;
static void check(bool ok, const char* what) {
    ++total;
    if (!ok) {
        ++failed;
        std::printf("  FAIL: %s\n", what);
    }
    std::fflush(stdout);
}

struct Pool {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    bool stop = false;
    std::vector<std::thread> threads;
    explicit Pool(int n) {
        for (int i = 0; i < n; ++i) {
            threads.emplace_back([this] {
                for (;;) {
                    std::unique_lock lock(m);
                    cv.wait(lock, [this] { return stop || !jobs.empty(); });
                    if (jobs.empty()) return;
                    auto job = std::move(jobs.front());
                    jobs.pop_front();
                    lock.unlock();
                    job();
                }
            });
        }
    }
    ~Pool() {
        {
            std::lock_guard lock(m);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : threads) t.join();
    }
    void add(std::function<void()> job) {
        {
            std::lock_guard lock(m);
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }
};

struct Slow {
    Slow() {
        std::this_thread::sleep_for(50ms);
        ++made;
    }
    static inline std::atomic<int> made{0};
};
static Slow& slow() {
    static Slow s;   // __cxa_guard: the other threads wait for the first
    return s;
}
static thread_local std::unique_ptr<int> perThread;
static std::atomic<int> tlsGone{0};
struct Gone {
    ~Gone() { ++tlsGone; }
};

int main() {
    using clock = std::chrono::steady_clock;
    {
        std::atomic<long> sum{0};
        {
            Pool pool(4);
            for (int i = 1; i <= 5000; ++i) pool.add([&sum, i] { sum += i; });
        }
        check(sum == 5000L * 5001 / 2, "thread pool: mutex, condition_variable, function");
        for (int round = 0; round < 100; ++round) {
            Pool pool(3);
            pool.add([] {});
        }
        check(true, "100 pools made and taken down");
    }
    {
        std::vector<std::thread> t;
        for (int i = 0; i < 8; ++i) t.emplace_back([] { slow(); });
        for (auto& x : t) x.join();
        check(Slow::made == 1, "a function static is made once by 8 threads");
    }
    {
        std::mutex m;
        std::condition_variable cv;
        std::unique_lock lock(m);
        auto t0 = clock::now();
        check(cv.wait_for(lock, 30ms) == std::cv_status::timeout, "wait_for times out");
        auto took = clock::now() - t0;
        check(took >= 29ms && took < 130ms, "after 30 ms");
        std::timed_mutex tm;
        tm.lock();
        std::thread t([&] { check(!tm.try_lock_for(20ms), "timed_mutex: try_lock_for times out"); });
        t.join();
        tm.unlock();
        std::recursive_mutex r;
        r.lock();
        r.lock();
        r.unlock();
        r.unlock();
        check(true, "recursive_mutex");
    }
    {
        std::shared_mutex sm;
        std::atomic<int> readers{0}, bad{0};
        int value = 0;
        std::vector<std::thread> t;
        for (int i = 0; i < 6; ++i) {
            t.emplace_back([&, i] {
                for (int k = 0; k < 2000; ++k) {
                    if ((k + i) % 8) {
                        std::shared_lock l(sm);
                        ++readers;
                        --readers;
                    } else {
                        std::unique_lock l(sm);
                        if (readers) ++bad;
                        ++value;
                    }
                }
            });
        }
        for (auto& x : t) x.join();
        check(bad == 0 && value == 6 * 250, "shared_mutex");
    }
    {
        auto f = std::async(std::launch::async, [] {
            std::this_thread::sleep_for(20ms);
            return 42;
        });
        check(f.get() == 42, "async and future");
        std::promise<int> p;
        std::thread t([&p] {
            try {
                throw std::runtime_error("thrown in a thread");
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        });
        bool caught = false;
        try {
            p.get_future().get();
        } catch (const std::runtime_error&) {
            caught = true;
        }
        t.join();
        check(caught, "an exception crosses threads in a promise");
    }
    {
        std::counting_semaphore<8> sem(0);
        std::latch latch(5);
        std::atomic<int> got{0};
        std::vector<std::thread> t;
        for (int i = 0; i < 5; ++i) {
            t.emplace_back([&] {
                sem.acquire();
                ++got;
                latch.count_down();
            });
        }
        std::this_thread::sleep_for(20ms);
        sem.release(5);
        latch.wait();
        check(got == 5, "counting_semaphore and latch");
        for (auto& x : t) x.join();
        auto t0 = clock::now();
        check(!sem.try_acquire_for(30ms) && clock::now() - t0 >= 29ms, "try_acquire_for times out");
        int phases = 0;
        std::barrier bar(4, [&phases]() noexcept { ++phases; });
        t.clear();
        for (int i = 0; i < 4; ++i) {
            t.emplace_back([&] {
                for (int k = 0; k < 500; ++k) bar.arrive_and_wait();
            });
        }
        for (auto& x : t) x.join();
        check(phases == 500, "barrier with a completion");
    }
    {
        std::atomic<int> turn{0};
        auto t0 = clock::now();
        auto play = [&](int me) {
            for (int i = 0; i < 5000; ++i) {
                int want = 2 * i + me;
                for (int v; (v = turn.load()) != want;) turn.wait(v);
                turn = want + 1;
                turn.notify_one();
            }
        };
        std::thread a(play, 0), b(play, 1);
        a.join();
        b.join();
        check(turn == 10000, "atomic wait and notify, 10000 turns");
        std::printf("  (%lld ns a turn)\n", (long long)(std::chrono::nanoseconds(clock::now() - t0).count() / 10000));
        // a waiter must not keep a CPU busy: see how soon it notices, and that
        // a wait of any width works
        std::atomic<char> small{0};
        std::atomic<long long> big{0};
        std::thread c([&] { small.wait(0); });
        std::thread d([&] { big.wait(0); });
        std::this_thread::sleep_for(30ms);
        small = 1;
        small.notify_all();
        big = 1;
        big.notify_all();
        c.join();
        d.join();
        check(true, "atomic wait on 1 and 8 bytes");
    }
    {
        std::vector<std::thread> t;
        for (int i = 0; i < 10; ++i) {
            t.emplace_back([i] {
                static thread_local Gone gone;
                (void)&gone;
                perThread = std::make_unique<int>(i);
                if (*perThread != i) ++failed;
            });
        }
        for (auto& x : t) x.join();
        check(tlsGone == 10 && !perThread, "thread_local objects are destroyed with their thread");
        std::once_flag flag;
        int ran = 0;
        std::vector<std::thread> u;
        for (int i = 0; i < 6; ++i) u.emplace_back([&] { std::call_once(flag, [&] { ++ran; }); });
        for (auto& x : u) x.join();
        check(ran == 1, "call_once");
    }
    std::printf("cxx-threads: %d of %d passed\n", total - failed, total);
    return failed ? 1 : 0;
}
