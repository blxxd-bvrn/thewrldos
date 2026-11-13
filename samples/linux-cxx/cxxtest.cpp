// C++ runtime test for THE WRLD OS's Linux layer: the features a browser
// engine like WebKit relies on, each reported PASS or FAIL.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>

static int passed = 0, failed = 0;
static void check(const char *what, bool ok) {
    std::cout << (ok ? "PASS  " : "FAIL  ") << what << std::endl;
    (ok ? passed : failed)++;
}

struct Base { virtual ~Base() = default; virtual int kind() const { return 1; } };
struct Derived : Base { int kind() const override { return 2; } };
struct Boom : std::runtime_error { int code; Boom(int c) : std::runtime_error("boom"), code(c) {} };

static int deep(int n) { if (!n) throw Boom(42); return deep(n - 1) + 1; }
static int &counter() { static int c = 0; return c; }             // thread-safe static
static thread_local int tls_value = 7;

int main() {
    std::cout << "THE WRLD OS - C++ runtime test (libstdc++, glibc)" << std::endl;

    // exceptions: through many frames, with destructors running, rethrow, std types
    {
        int unwound = 0;
        struct Guard { int &u; ~Guard() { u++; } };
        try { Guard g{unwound}; deep(50); check("exception thrown through 50 frames", false); }
        catch (const Boom &b) { check("exception thrown through 50 frames", b.code == 42 && unwound == 1); }
        try { try { throw std::out_of_range("x"); } catch (...) { throw; } }
        catch (const std::logic_error &e) { check("rethrow and catch by base class", std::string(e.what()) == "x"); }
        try { std::vector<int> v; (void)v.at(3); check("std::vector::at throws", false); }
        catch (const std::out_of_range &) { check("std::vector::at throws", true); }
    }
    // RTTI
    {
        std::unique_ptr<Base> p = std::make_unique<Derived>();
        check("virtual call and dynamic_cast", p->kind() == 2 && dynamic_cast<Derived *>(p.get()) != nullptr);
        check("typeid names", std::string(typeid(*p).name()).find("Derived") != std::string::npos);
    }
    // containers, strings, streams
    {
        std::map<std::string, int> m; std::unordered_map<int, std::string> u;
        for (int i = 0; i < 1000; i++) { m[std::to_string(i)] = i; u[i] = std::to_string(i * 2); }
        std::ostringstream os; os << m["999"] << ' ' << u[500] << ' ' << 3.25;
        check("map, unordered_map, ostringstream", os.str() == "999 1000 3.25");
        std::function<int(int)> f = [k = 3](int x) { return x * k; };
        check("std::function and lambdas", f(14) == 42);
    }
    // threads, mutex, condition variable, atomics, thread_local, statics
    {
        std::mutex mu; std::condition_variable cv; int ready = 0; std::atomic<int> sum{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; t++) ts.emplace_back([&, t] {
            tls_value = t;                                      // each thread its own copy
            for (int i = 0; i < 10000; i++) sum++;
            { std::lock_guard<std::mutex> g(mu); counter() += tls_value; ready++; }
            cv.notify_one();
        });
        { std::unique_lock<std::mutex> l(mu); cv.wait(l, [&] { return ready == 4; }); }
        for (auto &t : ts) t.join();
        check("4 threads, atomics, condition variable", sum == 40000);
        check("thread_local and function statics", counter() == 0 + 1 + 2 + 3 && tls_value == 7);
        check("hardware_concurrency reported", std::thread::hardware_concurrency() >= 1);
    }
    // time
    {
        auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check("sleep_for and steady_clock", ms >= 45 && ms < 1000);
    }
    // files and the filesystem library
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories("/tmp/cxxtest/sub", ec);
        { std::ofstream o("/tmp/cxxtest/sub/a.txt"); o << "hello\n" << 12345 << "\n"; }
        std::ifstream i("/tmp/cxxtest/sub/a.txt"); std::string w; int n = 0; i >> w >> n;
        check("ofstream and ifstream", w == "hello" && n == 12345);
        int files = 0; for (auto &e : fs::recursive_directory_iterator("/tmp/cxxtest", ec)) if (e.is_regular_file()) files++;
        check("std::filesystem directory walk", !ec && files == 1 && fs::file_size("/tmp/cxxtest/sub/a.txt") == 12);
        fs::remove_all("/tmp/cxxtest", ec);
        check("std::filesystem remove_all", !fs::exists("/tmp/cxxtest"));
    }
    std::cout << passed << " passed, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}
