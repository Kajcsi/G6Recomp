#pragma once

// The exports of crt_guest.dll, the C++ runtime fixture for test_side_dlls (docs/side-dlls.md,
// "Tests"). The test maps it with the native loader and calls these through the export table.

#include <cstddef>
#include <cstdint>

#ifdef CRT_GUEST_BUILD
#define CRT_GUEST_API extern "C" __declspec(dllexport)
#else
#define CRT_GUEST_API extern "C"
#endif

// Throws and catches inside the fixture; returns how many were caught as expected.
CRT_GUEST_API int crt_guest_exceptions();
inline constexpr int kCrtGuestExceptions = 4;

// Throws std::runtime_error(message), for the caller to catch.
CRT_GUEST_API void crt_guest_throw(const char* message);

// Strings, vectors, ordered and unordered maps; returns a checksum.
CRT_GUEST_API int64_t crt_guest_containers();
inline constexpr int64_t kCrtGuestContainers = 2850;

// `count` threads meet at a condition variable, then each adds its 1-based index under a mutex;
// returns the total.
CRT_GUEST_API int64_t crt_guest_threads(int count);

// Formats `value` and 1.23456 in a string stream whose locale groups thousands with commas, and
// reads the number back; returns the length written to `out`, or -1 when the number read back
// differs.
CRT_GUEST_API int crt_guest_format(int64_t value, char* out, size_t size);

// Two threads take turns through std::atomic::wait and notify_one; returns the final value.
CRT_GUEST_API int crt_guest_atomic_wait(int rounds);

// Sums 1..count in an OpenMP parallel region, split by thread number; returns the sum and
// stores how many threads the team had.
CRT_GUEST_API int64_t crt_guest_openmp(int count, int* team_size);

// A PPL task with a continuation (msvcp140) and a parallel_for (concrt140); returns
// task result * 1000000 + the parallel_for sum of 1..count.
CRT_GUEST_API int64_t crt_guest_ppl(int count);
inline constexpr int64_t kCrtGuestTaskResult = 21;

using CrtGuestExceptionsFn = int (*)();
using CrtGuestThrowFn = void (*)(const char*);
using CrtGuestContainersFn = int64_t (*)();
using CrtGuestThreadsFn = int64_t (*)(int);
using CrtGuestFormatFn = int (*)(int64_t, char*, size_t);
using CrtGuestAtomicWaitFn = int (*)(int);
using CrtGuestOpenMpFn = int64_t (*)(int, int*);
using CrtGuestPplFn = int64_t (*)(int);
