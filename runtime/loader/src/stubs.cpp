#include "stubs.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <windows.h>

#include "g6/log.h"

namespace g6::loader {
namespace {

constexpr size_t kThunkSize = 32;
constexpr size_t kPageSize = 4096;
constexpr size_t kRegionSize = size_t{4} << 20; // room for 131072 stubs

size_t round_up(size_t value, size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// mov rcx, imm64 ; mov rdx, [rsp] ; mov rax, imm64 ; jmp rax
// The thunk leaves rsp alone, so the handler returns straight to the guest's caller and the
// thunk needs no unwind information.
void write_thunk(uint8_t* at, const void* stub, uint64_t handler) {
    const auto stub_value = reinterpret_cast<uint64_t>(stub);
    uint8_t code[kThunkSize];
    std::memset(code, 0xCC, sizeof(code));
    size_t n = 0;
    code[n++] = 0x48;
    code[n++] = 0xB9;
    std::memcpy(code + n, &stub_value, 8);
    n += 8;
    code[n++] = 0x48;
    code[n++] = 0x8B;
    code[n++] = 0x14;
    code[n++] = 0x24;
    code[n++] = 0x48;
    code[n++] = 0xB8;
    std::memcpy(code + n, &handler, 8);
    n += 8;
    code[n++] = 0xFF;
    code[n++] = 0xE0;
    std::memcpy(at, code, sizeof(code));
}

} // namespace

StubHeap::StubHeap(Describe describe, void* context) : describe_(describe), context_(context) {
    region_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRegionSize, MEM_RESERVE, PAGE_NOACCESS));
    if (region_ == nullptr) {
        throw std::runtime_error(
            fmt::format("cannot reserve memory for import stubs (error {})", GetLastError()));
    }
}

StubHeap::~StubHeap() {
    if (region_ != nullptr) {
        VirtualFree(region_, 0, MEM_RELEASE);
    }
}

const void* StubHeap::create(std::string dll, std::string symbol, StubResult result, bool offline) {
    const std::scoped_lock lock(mutex_);
    used_ = std::max(used_, sealed_);
    if (used_ + kThunkSize > committed_) {
        if (committed_ + kPageSize > kRegionSize) {
            throw std::runtime_error("out of room for import stubs");
        }
        if (VirtualAlloc(region_ + committed_, kPageSize, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
            throw std::runtime_error(
                fmt::format("cannot commit memory for import stubs (error {})", GetLastError()));
        }
        committed_ += kPageSize;
    }

    auto stub = std::make_unique<Stub>();
    stub->dll = std::move(dll);
    stub->symbol = std::move(symbol);
    stub->result = result;
    stub->offline = offline;
    stub->heap = this;

    uint8_t* const thunk = region_ + used_;
    write_thunk(thunk, stub.get(), reinterpret_cast<uint64_t>(&StubHeap::handle));
    used_ += kThunkSize;
    stubs_.push_back(std::move(stub));
    return thunk;
}

void StubHeap::seal() {
    const std::scoped_lock lock(mutex_);
    const size_t end = round_up(used_, kPageSize);
    if (end <= sealed_) {
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(region_ + sealed_, end - sealed_, PAGE_EXECUTE_READ, &old)) {
        throw std::runtime_error(
            fmt::format("cannot make import stubs executable (error {})", GetLastError()));
    }
    FlushInstructionCache(GetCurrentProcess(), region_ + sealed_, end - sealed_);
    sealed_ = end;
}

uint64_t StubHeap::handle(Stub* stub, uint64_t caller) noexcept {
    if (!stub->logged.exchange(true, std::memory_order_relaxed)) {
        // Logging writes to a file, which can change the guest's last error.
        const DWORD saved_error = GetLastError();
        try {
            const std::string where = stub->heap->describe_(caller, stub->heap->context_);
            if (stub->offline) {
                LOG_WARN(imports, "offline import {}!{} called from {}: reporting no connectivity",
                         stub->dll, stub->symbol, where);
            } else {
                LOG_WARN(imports, "unimplemented import {}!{} called from {}", stub->dll,
                         stub->symbol, where);
            }
        } catch (...) { // NOLINT(bugprone-empty-catch): a failed log line must not reach guest code
        }
        SetLastError(saved_error);
    }
    if (stub->result.last_error) {
        SetLastError(*stub->result.last_error);
    }
    return stub->result.value;
}

} // namespace g6::loader
