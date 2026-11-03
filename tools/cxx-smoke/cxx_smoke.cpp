// cxx-smoke: the C++ runtime Mesa needs (libc++, libc++abi, libunwind built
// against mlibc by tools/build-libcxx.sh), working in InstantOS: containers
// and strings, exceptions thrown and caught across frames, thread_local,
// std::thread with std::mutex, std::sort, std::function, unique_ptr.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int failures;

static void check(bool ok, const char* what) {
    std::printf("cxx-smoke: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

static thread_local int perThread = 7;

[[gnu::noinline]] static int deep(int n) {
    if (n == 0) {
        throw std::runtime_error("from the bottom");
    }
    return deep(n - 1) + 1;
}

int main() {
    std::vector<int> v = {5, 3, 9, 1, 7};
    std::sort(v.begin(), v.end());
    check(v == std::vector<int>({1, 3, 5, 7, 9}), "std::vector and std::sort");
    std::map<std::string, int> m;
    m["three"] = 3;
    m["one"] = 1;
    check(m.begin()->first == "one" && m.size() == 2, "std::map with std::string keys");
    std::string s = "Instant";
    s += "OS";
    check(s == "InstantOS" && s.find("OS") == 7, "std::string");
    auto owned = std::make_unique<std::vector<int>>(1000, 4);
    check(owned->size() == 1000 && (*owned)[999] == 4, "std::unique_ptr");
    std::function<int(int)> twice = [](int x) { return 2 * x; };
    check(twice(21) == 42, "std::function");

    bool caught = false;
    try {
        deep(20);
    } catch (const std::exception& e) {
        caught = std::string(e.what()) == "from the bottom";
    }
    check(caught, "an exception thrown 20 frames down is caught (libunwind)");

    std::mutex lock;
    int total = 0;
    int seen[4] = {};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            perThread = t * 100;
            for (int i = 0; i < 1000; ++i) {
                std::lock_guard<std::mutex> guard(lock);
                ++total;
            }
            seen[t] = perThread;
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    check(total == 4000, "4 std::threads under a std::mutex");
    check(seen[0] == 0 && seen[1] == 100 && seen[2] == 200 && seen[3] == 300 && perThread == 7,
          "thread_local: one per thread, the main thread's untouched");

    std::printf(failures == 0 ? "cxx-smoke: all checks passed\n" : "cxx-smoke: some checks FAILED\n");
    return failures == 0 ? 0 : 1;
}
