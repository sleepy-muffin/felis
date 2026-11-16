// Regression tests for felis::log callback registration / unregistration.
//
// The backend dispatches callbacks on its worker thread, so every check waits
// for an observable side effect (an atomic counter) rather than assuming
// synchronous delivery.

#include <felis/log.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using namespace felis::log;
using namespace std::chrono_literals;

namespace
{
int g_failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

bool wait_ge(
    const std::atomic<int>& counter, int target,
    std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (counter.load() >= target) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return counter.load() >= target;
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

int main()
{
    // Keep the run quiet: everything goes to a scratch file unless a phase
    // redirects it.
    const auto scratch = std::filesystem::temp_directory_path() / "felis_log_callbacks_test.log";
    set_output_file(scratch, false);

    // ---- Phase 1: register two, unregister one --------------------------
    std::atomic<int> a{0};
    std::atomic<int> b{0};
    const callback_id a_id = register_callback([&](message_type, std::string&) {
        ++a;
        return logging_callback_result::PASS;
    });
    const callback_id b_id = register_callback([&](message_type, std::string&) {
        ++b;
        return logging_callback_result::PASS;
    });

    submit(message_type::INFO, std::string("m1"));
    expect(wait_ge(a, 1), "first callback fires on first message");
    expect(wait_ge(b, 1), "second callback fires on first message");

    expect(unregister_callback(b_id), "unregister existing id returns true");

    submit(message_type::INFO, std::string("m2"));
    expect(wait_ge(a, 2), "surviving callback fires on second message");
    std::this_thread::sleep_for(100ms);
    expect(b.load() == 1, "unregistered callback does not fire again");

    expect(unregister_callback(a_id), "unregister remaining id returns true");

    // ---- Phase 2: a callback unregisters itself during dispatch ---------
    std::atomic<int> self{0};
    callback_id self_id = 0;
    self_id = register_callback([&](message_type, std::string&) {
        ++self;
        unregister_callback(self_id);
        return logging_callback_result::PASS;
    });

    submit(message_type::INFO, std::string("m3"));
    expect(wait_ge(self, 1), "self-unregistering callback runs once (no deadlock)");

    submit(message_type::INFO, std::string("m4"));
    std::this_thread::sleep_for(100ms);
    expect(self.load() == 1, "self-unregistered callback does not run again");

    // ---- Phase 3: unregister an unknown id ------------------------------
    expect(!unregister_callback(999999), "unknown id returns false");

    // ---- Phase 4: TERMINATED suppresses the write -----------------------
    std::atomic<int> term{0};
    const callback_id t_id = register_callback([&](message_type, std::string&) {
        ++term;
        return logging_callback_result::TERMINATED;
    });

    const auto out = std::filesystem::temp_directory_path() / "felis_log_terminated_test.log";
    set_output_file(out, false);

    submit(message_type::INFO, std::string("should_be_suppressed"));
    expect(wait_ge(term, 1), "terminator callback runs");

    // The write step is skipped for a terminated message; give the worker a
    // moment to settle before inspecting the file.
    std::this_thread::sleep_for(100ms);
    expect(read_file(out).find("should_be_suppressed") == std::string::npos,
           "TERMINATED suppresses the output write");

    unregister_callback(t_id);

    kill(); // join the worker before tearing down the scratch files

    std::error_code ec;
    std::filesystem::remove(scratch, ec);
    std::filesystem::remove(out, ec);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("log callbacks: all checks passed\n");
    return 0;
}
