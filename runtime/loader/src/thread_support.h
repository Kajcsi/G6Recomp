#pragma once

// The loader's process-wide support DLLs (see support/): g6threads.dll, which passes Windows'
// thread notifications on to the loader, and the TLS donors, each of which lends its static
// TLS block to one guest module.

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "g6threads.h"

namespace g6::loader {

class ThreadSupport {
public:
    static constexpr int kDonorCount = G6_TLS_DONOR_COUNT;
    static constexpr size_t kDonorCapacity = G6_TLS_DONOR_CAPACITY;
    // The slot for the host executable's own block, TLS index 0, when the host lends it
    // (support/exe_tls.c). It holds kDonorCapacity bytes like a donor.
    static constexpr int kExecutableSlot = kDonorCount;
    static_assert(kDonorCount + 1 <= G6_THREADS_MAX_TLS_SLOTS);

    // Loads the DLLs from the runtime's own folder on first use. Throws kernel::LoadError when
    // one is missing or doesn't behave as expected.
    static ThreadSupport& get();

    ThreadSupport(const ThreadSupport&) = delete;
    ThreadSupport& operator=(const ThreadSupport&) = delete;

    void set_callback(G6ThreadCallback callback, void* context) const;
    void for_each_thread(G6ThreadVisitor visitor, void* context) const;
    G6ThreadRecord* current_thread() const;

    // Lends a block and returns its slot, with every thread's state for that slot reset. A guest
    // executable gets kExecutableSlot when the host lends it and no other guest has it; anything
    // else gets a donor. Throws kernel::LoadError when all donors are lent.
    int acquire_donor(bool executable);
    void release_donor(int slot);
    uint32_t donor_index(int slot) const { return indices_[slot]; }
    // Whether the host executable lends its TLS block (index 0) to a guest executable.
    bool lends_executable_slot() const { return lends_executable_slot_; }

private:
    ThreadSupport();
    ~ThreadSupport() = default;

    // Checks the host executable's offer of TLS index 0, if it makes one.
    void find_executable_slot();

    G6ThreadsSetCallback set_callback_ = nullptr;
    G6ThreadsForEach for_each_ = nullptr;
    G6ThreadsCurrent current_ = nullptr;

    std::mutex mutex_;
    bool lends_executable_slot_ = false;
    bool lent_[kDonorCount + 1] = {};
    uint32_t indices_[kDonorCount + 1] = {};
};

// The calling thread's TLS array (the TEB's ThreadLocalStoragePointer), and another thread's.
// Valid only until the next DLL with static TLS loads: when its index doesn't fit, Windows gives
// every thread a longer array and the old one no longer covers every index (seen when host DLLs
// such as shell32 take the low indices; docs/execution-model.md, "TLS and the TEB"). The blocks
// themselves stay where they are, so a block pointer read from the array stays valid.
void** current_tls_array();
void** tls_array_of(void* teb);

} // namespace g6::loader
