#pragma once

// The console's DirectStorage (xcrdapi.dll), whose only export the executable imports is
// DStorageGetFactory. The console's interfaces are PC DirectStorage's in another vtable order
// and with other structure layouts, and its requests can carry the console's hardware
// decompression formats (CLAUDE.md, "I/O"). docs/io.md, "DirectStorage", has what is known and
// where each fact comes from.
//
// Files open read-only through the active FileSystem, so a request reads what the guest's own
// CreateFile would. Requests run in order on the thread that submits them. So far only
// uncompressed requests from a file run: every other kind fails with E_NOTIMPL, and the log
// shows its options word and its source's first bytes, so the next formats are implemented from
// what the executable sends.

#include <cstdint>
#include <windows.h>

namespace g6::io {

// IDStorageFactoryX, IDStorageFileX, and IDStorageQueueX as the 1.0.2.1 executable asks for
// them.
inline constexpr GUID kDStorageFactoryX = {
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
inline constexpr GUID kDStorageFileX = {
    0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
inline constexpr GUID kDStorageQueueX = {
    0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};

// Slots of the factory, file, queue, and status array that are implemented. Every other slot
// is a logging E_NOTIMPL stub (g6/com_stand_in.h).
namespace dstorage_slot {
inline constexpr size_t kFactoryCreateQueue = 3;          // (const QueueDescX*, riid, void**)
inline constexpr size_t kFactoryOpenFile = 4;             // (const wchar_t* path, riid, void**)
inline constexpr size_t kFactoryCreateStatusArray = 5;    // (uint32_t capacity, name, riid, void**)
inline constexpr size_t kFactorySetCpuAffinity = 6;       // (uint64_t mask)
inline constexpr size_t kFactorySetDebugFlags = 7;        // (uint32_t flags)
inline constexpr size_t kFactorySetStagingBufferSize = 8; // (uint32_t bytes)
inline constexpr size_t kFactoryCreateQueueX = 10; // (const QueueDescX* with `extra`, riid, void**)

inline constexpr size_t kFileClose = 3;     // void ()
inline constexpr size_t kFileGetHandle = 4; // HANDLE (): a duplicate the caller closes

inline constexpr size_t kQueueEnqueueRequest = 3;        // void (const RequestX*)
inline constexpr size_t kQueueEnqueueStatus = 4;         // void (status array, uint32_t index)
inline constexpr size_t kQueueEnqueueSignal = 5;         // void (fence, uint64_t value)
inline constexpr size_t kQueueSubmit = 6;                // void ()
inline constexpr size_t kQueueCancelRequestsWithTag = 7; // void (uint64_t mask, uint64_t value)
inline constexpr size_t kQueueClose = 8;                 // void ()
inline constexpr size_t kQueueGetErrorEvent = 9;         // HANDLE (): a duplicate the caller closes
inline constexpr size_t kQueueRetrieveErrorRecord = 10;  // void (ErrorRecordX*)
inline constexpr size_t kQueueQuery = 11;                // void (QueueInfoX*)

inline constexpr size_t kStatusIsComplete = 3; // bool (uint32_t index)
inline constexpr size_t kStatusGetHResult = 4; // HRESULT (uint32_t index)
} // namespace dstorage_slot

// A queue's description. CreateQueueX's has `extra` too (the executable passes 1).
struct QueueDescX {
    uint32_t source_type; // 0 for files, 1 for memory
    uint32_t unknown;
    uint16_t capacity;
    int8_t priority;
    uint8_t padding[5];
    const char* name;
    uint64_t extra;
};
static_assert(sizeof(QueueDescX) == 0x20);
inline constexpr size_t kQueueDescSize = 0x18;

// A request as the executable enqueues it.
struct RequestX {
    uint64_t options; // 0 for a plain read; other values aren't implemented yet
    void* destination;
    uint64_t destination_page_offset;
    uint32_t destination_size;
    uint32_t unknown;
    void* source; // an IDStorageFileX, or memory on a memory queue
    uint64_t offset;
    uint32_t source_size;
    uint32_t intermediate_size;
    uint64_t tag; // for CancelRequestsWithTag
    const char* name;
};
static_assert(sizeof(RequestX) == 0x48);

// What RetrieveErrorRecord fills in: the failure count and the first failure. The rest of the
// record, which describes the failed request, is left zero for now.
struct ErrorRecordX {
    uint32_t failure_count;
    uint32_t padding;
    HRESULT hresult;
    uint32_t command_type; // 0 request, 1 status, 2 signal; 0xFFFFFFFF with no failure
    uint8_t rest[0x280];
};
static_assert(sizeof(ErrorRecordX) == 0x290);

// What Query fills in.
struct QueueInfoX {
    uint8_t desc[kQueueDescSize];
    uint16_t empty_slots;
    uint16_t requests_until_submit;
};
static_assert(sizeof(QueueInfoX) == 0x1C);

// Hands out the one factory, as the interface the caller asked for, with a reference.
HRESULT DStorageGetFactory(const GUID* riid, void** factory);

} // namespace g6::io
