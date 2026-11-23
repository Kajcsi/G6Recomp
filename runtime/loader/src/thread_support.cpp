#include "thread_support.h"

#include <cstring>
#include <filesystem>
#include <intrin.h>
#include <string>
#include <windows.h>

#include <fmt/format.h>

#include "g6/kernel/image_loader.h"
#include "g6/log.h"

namespace g6::loader {
namespace {

// The TEB's ThreadLocalStoragePointer: the TLS array that code reaches through gs:[0x58], as
// the PE format's description of TLS shows.
constexpr size_t kTebTlsArray = 0x58;

std::filesystem::path runtime_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            throw kernel::LoadError(
                fmt::format("cannot find the runtime's own folder (error {})", GetLastError()));
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}

HMODULE load_support(const std::filesystem::path& folder, const std::wstring& name) {
    const std::filesystem::path path = folder / name;
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, 0);
    if (module == nullptr) {
        throw kernel::LoadError(fmt::format("cannot load the loader's support DLL {} (error {})",
                                            path.string(), GetLastError()));
    }
    return module;
}

template <typename T>
T lookup(HMODULE module, const char* name) {
    auto* const function = reinterpret_cast<T>(GetProcAddress(module, name));
    if (function == nullptr) {
        throw kernel::LoadError(fmt::format("a support DLL lacks {}", name));
    }
    return function;
}

} // namespace

void** current_tls_array() {
    return reinterpret_cast<void**>(__readgsqword(kTebTlsArray));
}

void** tls_array_of(void* teb) {
    void** array = nullptr;
    std::memcpy(static_cast<void*>(&array), static_cast<const uint8_t*>(teb) + kTebTlsArray,
                sizeof(array));
    return array;
}

ThreadSupport& ThreadSupport::get() {
    // A constructor that throws leaves the static uninitialized, so the next call retries.
    static ThreadSupport instance;
    return instance;
}

ThreadSupport::ThreadSupport() {
    const std::filesystem::path folder = runtime_folder();

    HMODULE threads = load_support(folder, L"g6threads.dll");
    set_callback_ = lookup<G6ThreadsSetCallback>(threads, "g6_threads_set_callback");
    for_each_ = lookup<G6ThreadsForEach>(threads, "g6_threads_for_each");
    current_ = lookup<G6ThreadsCurrent>(threads, "g6_threads_current");

    using DonorQuery = ULONG (*)();
    for (int slot = 0; slot < kDonorCount; ++slot) {
        HMODULE donor = load_support(folder, L"g6tls" + std::to_wstring(slot) + L".dll");
        const ULONG capacity = lookup<DonorQuery>(donor, "g6_tls_donor_capacity")();
        if (capacity != kDonorCapacity) {
            throw kernel::LoadError(
                fmt::format("TLS donor {} holds {:#x} bytes, but the loader was built for {:#x}",
                            slot, capacity, kDonorCapacity));
        }
        const ULONG index = lookup<DonorQuery>(donor, "g6_tls_donor_index")();
        // Windows must have given the donor a block of its own on this thread, filled from its
        // template (which starts 1, 0). If it didn't, static TLS through donors doesn't work on
        // this system, and the loader can't go on. The array is read after the load, never
        // before: when a DLL's index doesn't fit in it, Windows gives every thread a longer one
        // (see current_tls_array).
        void* const* const array = current_tls_array();
        const auto* block = array != nullptr ? static_cast<const uint8_t*>(array[index]) : nullptr;
        if (block == nullptr || block[0] != 1 || block[1] != 0) {
            throw kernel::LoadError(fmt::format(
                "TLS donor {} has no TLS block of its own (index {}); Windows didn't set up its "
                "static TLS",
                slot, index));
        }
        for (int other = 0; other < slot; ++other) {
            if (indices_[other] == index) {
                throw kernel::LoadError(
                    fmt::format("TLS donors {} and {} share TLS index {}", other, slot, index));
            }
        }
        indices_[slot] = index;
    }
    LOG_DEBUG(loader, "loaded g6threads.dll and {} TLS donors of {:#x} bytes from {}", kDonorCount,
              kDonorCapacity, folder.string());
    find_executable_slot();
}

void ThreadSupport::find_executable_slot() {
    HMODULE host = GetModuleHandleW(nullptr);
    using AreaQuery = const void* (*)();
    using CapacityQuery = ULONG (*)();
    auto* const area_query =
        reinterpret_cast<AreaQuery>(GetProcAddress(host, "g6_exe_tls_area_template"));
    auto* const capacity_query =
        reinterpret_cast<CapacityQuery>(GetProcAddress(host, "g6_exe_tls_area_capacity"));
    if (area_query == nullptr || capacity_query == nullptr) {
        LOG_DEBUG(loader, "the host executable doesn't lend TLS index 0; a guest executable gets "
                          "a donor");
        return;
    }
    // The host offers slot 0, so a guest executable will rely on it: every check failing here is
    // a misbuilt host, and an error rather than a fallback.
    const auto fail = [](std::string_view why) {
        throw kernel::LoadError(fmt::format(
            "the host executable offers its TLS block to a guest executable, but {}", why));
    };
    if (capacity_query() != kDonorCapacity) {
        fail(fmt::format("its area holds {:#x} bytes and the loader was built for {:#x}",
                         capacity_query(), kDonorCapacity));
    }
    const auto* const image = reinterpret_cast<const uint8_t*>(host);
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image, sizeof(dos));
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, image + dos.e_lfanew, sizeof(nt));
    const IMAGE_DATA_DIRECTORY entry = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (entry.VirtualAddress == 0 || entry.Size < sizeof(IMAGE_TLS_DIRECTORY64)) {
        fail("it has no TLS directory");
    }
    IMAGE_TLS_DIRECTORY64 directory{};
    std::memcpy(&directory, image + entry.VirtualAddress, sizeof(directory));
    // The area must be the start of the block, and the block at least as big as the area.
    const auto area = reinterpret_cast<uint64_t>(area_query());
    if (directory.StartAddressOfRawData != area ||
        directory.EndAddressOfRawData - directory.StartAddressOfRawData < kDonorCapacity) {
        fail("its TLS block doesn't start with the area (was the C runtime's TLS support linked "
             "instead of exe_tls.c?)");
    }
    ULONG index = 0;
    std::memcpy(&index, reinterpret_cast<const void*>(directory.AddressOfIndex), sizeof(index));
    if (index != 0) {
        fail(fmt::format("Windows gave it TLS index {}, not 0", index));
    }
    // Windows must have filled this thread's block from the template, which starts 1, 0.
    void* const* const array = current_tls_array();
    const auto* block = array != nullptr ? static_cast<const uint8_t*>(array[0]) : nullptr;
    if (block == nullptr || block[0] != 1 || block[1] != 0) {
        fail("this thread's block at index 0 isn't the host's");
    }
    lends_executable_slot_ = true;
    indices_[kExecutableSlot] = 0;
    LOG_DEBUG(loader, "the host executable lends TLS index 0 ({:#x} bytes) to a guest executable",
              kDonorCapacity);
}

void ThreadSupport::set_callback(G6ThreadCallback callback, void* context) const {
    set_callback_(callback, context);
}

void ThreadSupport::for_each_thread(G6ThreadVisitor visitor, void* context) const {
    for_each_(visitor, context);
}

G6ThreadRecord* ThreadSupport::current_thread() const {
    return current_();
}

int ThreadSupport::acquire_donor(bool executable) {
    const std::scoped_lock lock(mutex_);
    const auto lend = [&](int slot) {
        lent_[slot] = true;
        // A previous loader may have used this slot; no thread's block holds the next guest's
        // template yet.
        for_each_(
            [](G6ThreadRecord* record, void* context) {
                InterlockedExchange(&record->tls_state[*static_cast<int*>(context)], 0);
            },
            &slot);
        return slot;
    };
    if (executable && lends_executable_slot_ && !lent_[kExecutableSlot]) {
        return lend(kExecutableSlot);
    }
    for (int slot = 0; slot < kDonorCount; ++slot) {
        if (!lent_[slot]) {
            return lend(slot);
        }
    }
    throw kernel::LoadError(fmt::format(
        "all {} TLS donors are in use; another guest module with static TLS needs one more",
        kDonorCount));
}

void ThreadSupport::release_donor(int slot) {
    const std::scoped_lock lock(mutex_);
    lent_[slot] = false;
}

} // namespace g6::loader
