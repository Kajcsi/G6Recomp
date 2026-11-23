/* The static TLS of an executable that hosts a guest executable, laid out so the guest can have
 * TLS index 0 (docs/execution-model.md, "TLS and the TEB").
 *
 * Windows gives the executable's TLS block index 0 on every thread, and code compiled with /GA
 * relies on that: it reads its thread's block from slot 0 of the TLS array without reading its
 * index variable. A guest executable built that way only works if slot 0 holds its data, and
 * slot 0 belongs to the host executable. So the host gives it up: this file, linked into the
 * host executable in place of the C runtime's TLS support, starts the host's TLS block with an
 * area of G6_TLS_DONOR_CAPACITY bytes that nothing of the host's uses. The loader lends that
 * area to the guest executable like a donor's block: it writes 0 into the guest's index
 * variable and copies the guest's template to offset 0 of every thread's block.
 *
 * The host's own thread_local data, if it has any, follows the area, so it keeps working
 * through index 0 as usual. The TLS directory is written out here, not taken from the C
 * runtime, because the area must start at offset 0 of the block. The C runtime's version puts
 * a marker byte there, and the order of the two inside the ".tls" section would depend on the
 * order the linker sees them in. Two definitions of the TLS directory fail the link, so the C
 * runtime's can't sneak in alongside this one. */

#include <windows.h>

#ifndef G6_TLS_DONOR_CAPACITY
#error "G6_TLS_DONOR_CAPACITY must be defined"
#endif

/* Windows writes the executable's TLS index here, which is always 0. Code that uses thread_local
 * reads it, so it must have this name. */
ULONG _tls_index = 0;

/* The linker sorts sections by name within ".tls": first the area (".tls"), then the
 * compiler's thread_local data (".tls$"), then the end marker (".tls$ZZZ"). The area's nonzero
 * first byte keeps it in initialized data, and lets the loader check that Windows set up the
 * block before any guest uses it. */
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
__declspec(allocate(".tls")) unsigned char g6_exe_tls_area[G6_TLS_DONOR_CAPACITY] = {1};
__declspec(allocate(".tls$ZZZ")) unsigned char g6_exe_tls_end = 0;

/* The TLS callback array: the C runtime's callbacks for thread_local variables with dynamic
 * initialization or destructors sit in ".CRT$XLB" to ".CRT$XLY", between these two, and the
 * null pointer in ".CRT$XLZ" ends the array. The end must be kept even though nothing refers to
 * it, so the host links with /INCLUDE:g6_exe_tls_callbacks_end. */
#pragma section(".CRT$XLA", read)
#pragma section(".CRT$XLZ", read)
__declspec(allocate(".CRT$XLA")) const PIMAGE_TLS_CALLBACK g6_exe_tls_callbacks_begin = 0;
__declspec(allocate(".CRT$XLZ")) const PIMAGE_TLS_CALLBACK g6_exe_tls_callbacks_end = 0;

#pragma section(".rdata$T", read)
__declspec(allocate(".rdata$T")) const IMAGE_TLS_DIRECTORY64 _tls_used = {
    (ULONGLONG)g6_exe_tls_area,
    (ULONGLONG)&g6_exe_tls_end,
    (ULONGLONG)&_tls_index,
    (ULONGLONG)(&g6_exe_tls_callbacks_begin + 1),
    0,
    {0},
};

/* The loader looks these up in the host executable. Their presence is the host's offer of
 * slot 0. */
__declspec(dllexport) const void* g6_exe_tls_area_template(void) {
    return g6_exe_tls_area;
}

__declspec(dllexport) ULONG g6_exe_tls_area_capacity(void) {
    return G6_TLS_DONOR_CAPACITY;
}
