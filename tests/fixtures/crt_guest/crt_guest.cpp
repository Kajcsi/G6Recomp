// crt_guest.dll: an original fixture for test_side_dlls (docs/side-dlls.md, "Tests"). Built /MD,
// so its C++ runtime imports (vcruntime140, msvcp140, concrt140, vcomp140) bind to the
// console's copies in the user's install when the native loader maps it.
//
// Our toolset is newer than the console's 14.29 runtime, so this sticks to what 14.29 exports:
// no timed waits, no sleep_for, and the mutex constructor that isn't constexpr (the build
// defines _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR).

#define CRT_GUEST_BUILD
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <mutex>
#include <ppl.h>
#include <ppltasks.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "crt_guest_api.h"

// The OpenMP entry points vcomp140 exports. clang-cl's own OpenMP support targets LLVM's
// runtime, so the parallel region is started the way MSVC-compiled code starts one: _vcomp_fork
// calls `body` on every thread of the team with the arguments that follow.
extern "C" {
__declspec(dllimport) void __cdecl _vcomp_fork(int if_parallel, int argument_count, void* body,
                                               ...);
__declspec(dllimport) int __cdecl omp_get_thread_num();
__declspec(dllimport) int __cdecl omp_get_num_threads();
}

namespace {

class Grouping : public std::numpunct<char> {
protected:
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};

struct OpenMpWork {
    int count;
    std::atomic<int64_t> sum;
    std::atomic<int> team_size;
};

void __cdecl openmp_body(OpenMpWork* work) {
    const int threads = omp_get_num_threads();
    const int thread = omp_get_thread_num();
    work->team_size.store(threads);
    int64_t partial = 0;
    for (int i = thread + 1; i <= work->count; i += threads) {
        partial += i;
    }
    work->sum.fetch_add(partial);
}

} // namespace

CRT_GUEST_API int crt_guest_exceptions() {
    int caught = 0;
    try {
        throw std::runtime_error("inner");
    } catch (const std::exception& error) {
        caught += std::strcmp(error.what(), "inner") == 0 ? 1 : 0;
    }
    // Thrown by the runtime itself.
    try {
        const std::vector<int> empty;
        caught += empty.at(1);
    } catch (const std::out_of_range&) {
        ++caught;
    }
    try {
        caught += std::stoi("not a number");
    } catch (const std::invalid_argument&) {
        ++caught;
    }
    try {
        try {
            throw 42;
        } catch (int) {
            throw;
        }
    } catch (int value) {
        caught += value == 42 ? 1 : 0;
    }
    return caught;
}

CRT_GUEST_API void crt_guest_throw(const char* message) {
    throw std::runtime_error(message);
}

CRT_GUEST_API int64_t crt_guest_containers() {
    std::vector<std::string> words;
    words.reserve(100);
    for (int i = 0; i < 100; ++i) {
        words.push_back("word" + std::to_string(i));
    }
    std::map<std::string, size_t> lengths;
    for (const std::string& word : words) {
        lengths[word] = word.size();
    }
    std::unordered_map<size_t, int> histogram;
    int64_t total = 0;
    for (const auto& [word, length] : lengths) {
        ++histogram[length];
        total += static_cast<int64_t>(length);
    }
    total += int64_t{histogram[5]} * 100 + histogram[6];
    std::string joined;
    auto it = lengths.begin();
    for (int i = 0; i < 3; ++i, ++it) {
        joined += it->first;
    }
    total += static_cast<int64_t>(joined.size()) * 10 + static_cast<int64_t>(joined.find("word10"));
    std::ranges::sort(words, std::greater<>());
    total += words.front() == "word99" ? 1000 : 0;
    return total;
}

CRT_GUEST_API int64_t crt_guest_threads(int count) {
    std::mutex mutex;
    std::condition_variable changed;
    int arrived = 0;
    bool go = false;
    int64_t total = 0;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&, i] {
            std::unique_lock lock(mutex);
            ++arrived;
            changed.notify_all();
            changed.wait(lock, [&] { return go; });
            total += i + 1;
        });
    }
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return arrived == count; });
        go = true;
    }
    changed.notify_all();
    for (std::thread& thread : threads) {
        thread.join();
    }
    return total;
}

CRT_GUEST_API int crt_guest_format(int64_t value, char* out, size_t size) {
    const std::locale grouped(std::locale::classic(), new Grouping);
    std::ostringstream stream;
    stream.imbue(grouped);
    stream << static_cast<long long>(value) << ' ' << std::fixed << std::setprecision(2) << 1.23456;
    const std::string text = stream.str();

    std::istringstream input(text);
    input.imbue(grouped);
    long long read_back = 0;
    input >> read_back;
    if (!input || read_back != value) {
        return -1;
    }
    if (size == 0) {
        return static_cast<int>(text.size());
    }
    const size_t copied = std::min(text.size(), size - 1);
    std::memcpy(out, text.data(), copied);
    out[copied] = '\0';
    return static_cast<int>(text.size());
}

CRT_GUEST_API int crt_guest_atomic_wait(int rounds) {
    // Odd values are the main thread's turn to hand over, even values the worker's.
    std::atomic<int> state{0};
    std::thread worker([&] {
        for (int round = 0; round < rounds; ++round) {
            const int odd = 2 * round + 1;
            state.wait(odd - 1);
            state.store(odd + 1);
            state.notify_one();
        }
    });
    for (int round = 0; round < rounds; ++round) {
        const int odd = 2 * round + 1;
        state.store(odd);
        state.notify_one();
        state.wait(odd);
    }
    worker.join();
    return state.load();
}

CRT_GUEST_API int64_t crt_guest_openmp(int count, int* team_size) {
    OpenMpWork work{count, 0, 0};
    _vcomp_fork(1, 1, reinterpret_cast<void*>(&openmp_body), &work);
    *team_size = work.team_size.load();
    return work.sum.load();
}

CRT_GUEST_API int64_t crt_guest_ppl(int count) {
    const int result =
        concurrency::create_task([] { return 20; }).then([](int value) { return value + 1; }).get();
    std::atomic<int64_t> sum{0};
    concurrency::parallel_for(1, count + 1, [&](int i) { sum.fetch_add(i); });
    return int64_t{result} * 1000000 + sum.load();
}
