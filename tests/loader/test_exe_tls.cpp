// A guest executable built with /GA, which finds its thread-local data in slot 0 of the TLS array
// and never reads its TLS index (tests/fixtures/exe_tls), under a host that lends it that slot:
// this test executable links runtime/loader/support/exe_tls.c, so its own TLS block starts with
// an area the loader gives the guest. The guest only sees its own data when it got index 0. The
// host's own thread_local data sits behind the area and must keep working. Design:
// docs/execution-model.md, "TLS and the TEB".

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>

#include <fmt/format.h>

#include "g6/loader/loader.h"
#include "g6/log.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6test.h"
#include "ga_guest_api.h"
#include "scan.h"

using g6::loader::Loader;
using g6::loader::LoaderOptions;
using g6::loader::Module;

// From exe_tls.c, and the index Windows wrote for this executable.
extern "C" const void* g6_exe_tls_area_template(void);
extern "C" ULONG g6_exe_tls_area_capacity(void);
extern "C" ULONG _tls_index; // NOLINT(bugprone-reserved-identifier)

namespace {

constexpr int kHostValue = 41;
constexpr size_t kHostTextLength = 40;

std::string make_host_text() {
    // Not braced: std::string{n, c} would be the two characters n and c.
    return std::string(kHostTextLength, 'h'); // NOLINT(modernize-return-braced-init-list)
}

// One thread_local with a constant initializer and one initialized at run time, through the
// C runtime's TLS callback that exe_tls.c's callback array must still hold.
thread_local int t_host_value = kHostValue;
thread_local std::string t_host_text = make_host_text();

bool host_thread_locals_intact() {
    return t_host_value == kHostValue && t_host_text == make_host_text();
}

const std::filesystem::path& guest_folder() {
    static const std::filesystem::path folder(G6_EXE_TLS_DIR);
    return folder;
}

LoaderOptions make_options() {
    LoaderOptions options;
    options.search_paths.push_back(guest_folder());
    options.imports.forward_dll("kernel32.dll");
    options.imports.forward_dll("api-ms-win-core-*");
    options.imports.forward_dll("ntdll.dll");
    return options;
}

template <typename T>
T guest_function(const Module& module, std::string_view name) {
    void* address = module.find_export(name);
    if (address == nullptr) {
        g6::test::report_failure(__FILE__, __LINE__,
                                 fmt::format("{} doesn't export {}", module.name(), name));
        throw g6::test::RequireFailed{};
    }
    return reinterpret_cast<T>(address);
}

struct Guest {
    explicit Guest(const Module& exe)
        : get(guest_function<int (*)()>(exe, "g6_ga_get")),
          set(guest_function<void (*)(int)>(exe, "g6_ga_set")),
          name(guest_function<const char* (*)()>(exe, "g6_ga_name")),
          bump(guest_function<int (*)()>(exe, "g6_ga_bump")),
          tls_index(guest_function<ULONG (*)()>(exe, "g6_ga_tls_index")) {}

    int (*get)();
    void (*set)(int);
    const char* (*name)();
    int (*bump)();
    ULONG (*tls_index)();

    // The guest's view of its TLS on the calling thread, untouched since the load.
    bool fresh() const {
        return get() == G6_GA_INITIAL && std::strcmp(name(), G6_GA_NAME) == 0 && bump() == 1;
    }
};

class LogCapture {
public:
    LogCapture() { g6::log::set_sink(&LogCapture::sink, this); }
    ~LogCapture() { g6::log::set_sink(nullptr, nullptr); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;

    size_t count_containing(std::string_view text) const {
        const std::scoped_lock lock(mutex_);
        size_t count = 0;
        for (const std::string& line : lines_) {
            count += line.find(text) != std::string::npos ? 1 : 0;
        }
        return count;
    }

private:
    static void sink(g6::log::Level, std::string_view, std::string_view message, void* context) {
        auto* self = static_cast<LogCapture*>(context);
        const std::scoped_lock lock(self->mutex_);
        self->lines_.emplace_back(message);
    }

    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

} // namespace

TEST_CASE("the host executable's TLS block starts with the area it lends") {
    CHECK_EQ(_tls_index, ULONG{0});
    CHECK_EQ(size_t{g6_exe_tls_area_capacity()}, size_t{G6_TLS_DONOR_CAPACITY});

    const auto* image = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image, sizeof(dos));
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, image + dos.e_lfanew, sizeof(nt));
    const IMAGE_DATA_DIRECTORY entry = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    REQUIRE(entry.VirtualAddress != 0);
    IMAGE_TLS_DIRECTORY64 directory{};
    std::memcpy(&directory, image + entry.VirtualAddress, sizeof(directory));
    CHECK_EQ(directory.StartAddressOfRawData,
             reinterpret_cast<uint64_t>(g6_exe_tls_area_template()));
    // The host's own data follows the area.
    CHECK(directory.EndAddressOfRawData - directory.StartAddressOfRawData >
          uint64_t{G6_TLS_DONOR_CAPACITY});
    CHECK(directory.AddressOfCallBacks != 0);

    // Its thread_local data works on this thread and on a new one, including the variable the C
    // runtime initializes through a TLS callback.
    CHECK(host_thread_locals_intact());
    bool other = false;
    std::thread([&] { other = host_thread_locals_intact(); }).join();
    CHECK(other);
}

TEST_CASE("a /GA guest executable gets TLS index 0 and its own data on every thread") {
    const LogCapture log;
    Loader loader(make_options());

    // A thread that starts before the load and reads the guest's TLS afterwards.
    std::atomic<const Guest*> shared{nullptr};
    std::atomic<int> early_result{-1};
    std::thread early([&] {
        const Guest* guest = nullptr;
        while ((guest = shared.load()) == nullptr) {
            std::this_thread::yield();
        }
        early_result = guest->fresh() && host_thread_locals_intact() ? 1 : 0;
    });

    const Module exe = loader.load("ga_guest.exe");
    CHECK(exe.is_executable());
    CHECK_EQ(log.count_containing("is an executable with static TLS, but gets TLS index"),
             size_t{0});
    const Guest guest(exe);
    shared = &guest;
    early.join();
    CHECK_EQ(early_result.load(), 1);

    // The loader wrote 0 into the guest's index variable, and the code that ignores it sees the
    // guest's template.
    CHECK_EQ(guest.tls_index(), ULONG{0});
    CHECK(guest.fresh());
    CHECK_EQ(guest.bump(), 2);

    // Each thread has its own copy.
    guest.set(1234);
    int seen_there = 0;
    bool fresh_there = false;
    std::thread([&] {
        fresh_there = guest.fresh();
        guest.set(5678);
        seen_there = guest.get();
    }).join();
    CHECK(fresh_there);
    CHECK_EQ(seen_there, 5678);
    CHECK_EQ(guest.get(), 1234);

    // The entry point, on a thread of its own.
    CHECK_EQ(loader.run_entry_point(exe), uint32_t{G6_GA_ENTRY_CHECKS});

    // The host's own thread_local data behind the area is untouched.
    CHECK(host_thread_locals_intact());
    bool host_there = false;
    std::thread([&] { host_there = host_thread_locals_intact(); }).join();
    CHECK(host_there);
}

TEST_CASE("the executable's TLS callbacks get every later thread's attach and detach") {
    // As under a Windows launch: the callbacks run once for process attach at the load, then on
    // each thread that starts afterwards, the entry point's included, with that thread's block
    // already filled (docs/execution-model.md, "TLS and the TEB").
    Loader loader(make_options());
    const Module exe = loader.load("ga_guest.exe");
    const Guest guest(exe);
    const auto attached = guest_function<int (*)()>(exe, "g6_ga_attached");
    const auto process_attaches = guest_function<LONG (*)()>(exe, "g6_ga_process_attaches");
    const auto marked_detaches = guest_function<LONG (*)()>(exe, "g6_ga_marked_detaches");
    CHECK_EQ(process_attaches(), LONG{1});
    // The loading thread existed before the executable, so it got no thread attach.
    CHECK_EQ(attached(), 0);

    int attached_there = -1;
    std::thread([&] {
        attached_there = attached();
        guest.set(G6_GA_DETACH_MARK);
    }).join();
    CHECK_EQ(attached_there, 1);
    CHECK_EQ(marked_detaches(), LONG{1});

    // The entry point checks its own attach and marks its thread.
    CHECK_EQ(loader.run_entry_point(exe), uint32_t{G6_GA_ENTRY_CHECKS});
    CHECK_EQ(marked_detaches(), LONG{2});
    CHECK_EQ(process_attaches(), LONG{1});
}

TEST_CASE("index 0 is lent again after the guest executable is unloaded") {
    for (int round = 0; round < 2; ++round) {
        INFO(fmt::format("round {}", round));
        Loader loader(make_options());
        const Guest guest(loader.load("ga_guest.exe"));
        // The previous round left its writes in this thread's block; the load refills it.
        CHECK(guest.fresh());
        guest.set(round + 100);
        guest.bump();
    }
}

TEST_CASE("the guest's code reads a fixed TLS slot") {
    // What makes the fixture worth testing: the scan finds code that reads slot 0 of the TLS array
    // directly in the functions that use the guest's thread-local data.
    const g6::pe::Image image = g6::pe::Image::load_file(guest_folder() / "ga_guest.exe");
    const g6::recomp::ScanReport report = g6::recomp::scan_module(image);
    uint32_t get_rva = 0;
    for (const g6::pe::Export& entry : g6::pe::read_exports(image).exports) {
        if (entry.name == "g6_ga_get") {
            get_rva = entry.rva;
        }
    }
    REQUIRE(get_rva != 0);
    bool in_get = false;
    for (const uint32_t site : report.tls_slots.fixed_slot.walked) {
        in_get = in_get || (site >= get_rva && site < get_rva + 0x20);
    }
    CHECK(in_get);
}
