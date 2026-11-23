#pragma once

// Stubs for imports that bind to no implementation (docs/execution-model.md, "Import policy").
//
// Each stub is a small thunk generated at load time. It passes its own record and the caller's
// return address to a common handler, which logs the DLL, the symbol, and the caller the first
// time the stub is called, sets the last error if the stub has one, and returns the stub's
// value straight to the guest. A stub never looks like success.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "g6/loader/import_registry.h"

namespace g6::loader {

class StubHeap {
public:
    // Turns a guest address into text for the log, such as "game.exe+0x1234".
    using Describe = std::string (*)(uint64_t address, void* context);

    StubHeap(Describe describe, void* context);
    ~StubHeap();
    StubHeap(const StubHeap&) = delete;
    StubHeap& operator=(const StubHeap&) = delete;

    // Writes a thunk and returns its address. Thunks become executable on seal().
    const void* create(std::string dll, std::string symbol, StubResult result, bool offline);

    // Makes every thunk written so far executable. Later thunks go on fresh pages, so pages
    // that guest code may be running never change protection again.
    void seal();

    struct Stub {
        std::string dll;
        std::string symbol;
        StubResult result;
        bool offline = false;
        std::atomic<bool> logged{false};
        StubHeap* heap = nullptr;
    };

private:
    static uint64_t handle(Stub* stub, uint64_t caller) noexcept;

    Describe describe_;
    void* context_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<Stub>> stubs_;
    uint8_t* region_ = nullptr;
    size_t committed_ = 0; // bytes committed from region_
    size_t sealed_ = 0;    // bytes made executable
    size_t used_ = 0;      // bytes written
};

} // namespace g6::loader
