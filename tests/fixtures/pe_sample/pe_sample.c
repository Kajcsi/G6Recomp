// Original test fixture for the PE readers and function discovery. It is built with the
// project's own compiler and linker so the tests see real linker output: exports, regular and
// delay-load imports, .pdata with an exception handler, a TLS callback, the CFG function table,
// relocated pointers into code, and assembly leaves that have no unwind data.

#include <windows.h>

#define FIXTURE_EXPORT __declspec(dllexport)

// Leaves written in assembly, so no compiler can give them .pdata entries.
// fixture_asm_fall increments its argument and falls straight into fixture_asm_after.
__asm__(".intel_syntax noprefix\n"
        ".text\n"
        ".p2align 4\n"
        ".globl fixture_asm_leaf\n"
        "fixture_asm_leaf:\n"
        "    lea eax, [rcx + rdx]\n"
        "    ret\n"
        ".p2align 4\n"
        ".globl fixture_asm_fall\n"
        "fixture_asm_fall:\n"
        "    inc ecx\n"
        ".globl fixture_asm_after\n"
        "fixture_asm_after:\n"
        "    mov eax, ecx\n"
        "    ret\n");

// A switch dispatch in the form MSVC emits: the image base comes from a RIP-relative lea, the
// table sits in .text after the function, and its entries are image-relative.
__asm__(".intel_syntax noprefix\n"
        ".text\n"
        ".p2align 4\n"
        ".globl fixture_asm_switch\n"
        "fixture_asm_switch:\n"
        "    cmp ecx, 3\n"
        "    ja .Lfixture_switch_default\n"
        "    mov eax, ecx\n"
        "    lea rdx, [rip + __ImageBase]\n"
        "    mov ecx, dword ptr [rdx + 4*rax + fixture_asm_switch_table@IMGREL]\n"
        "    add rcx, rdx\n"
        "    jmp rcx\n"
        ".Lfixture_switch_0:\n"
        "    mov eax, 10\n"
        "    ret\n"
        ".Lfixture_switch_1:\n"
        "    mov eax, 20\n"
        "    ret\n"
        ".Lfixture_switch_2:\n"
        "    mov eax, 30\n"
        "    ret\n"
        ".Lfixture_switch_3:\n"
        "    mov eax, 40\n"
        "    ret\n"
        ".Lfixture_switch_default:\n"
        "    mov eax, -1\n"
        "    ret\n"
        ".p2align 2\n"
        "fixture_asm_switch_table:\n"
        "    .long .Lfixture_switch_0@IMGREL\n"
        "    .long .Lfixture_switch_1@IMGREL\n"
        "    .long .Lfixture_switch_2@IMGREL\n"
        "    .long .Lfixture_switch_3@IMGREL\n");

int fixture_asm_leaf(int a, int b);
int fixture_asm_fall(int a);
int fixture_asm_after(int a);
int fixture_asm_switch(unsigned selector);

#pragma comment(linker, "/export:fixture_asm_fall")
#pragma comment(linker, "/export:fixture_asm_switch")

// A dense switch the compiler turns into a jump table. Every case computes something different
// from `x`, so it can't become a lookup table of constants.
FIXTURE_EXPORT __declspec(noinline) int fixture_switch(int selector, int x) {
    switch (selector) {
    case 0: return x * 3;
    case 1: return x + 11;
    case 2: return x ^ 0x5A;
    case 3: return x >> 2;
    case 4: return -x;
    case 5: return x * x;
    case 6: return ~x;
    case 7: return x - 100;
    default: return 0;
    }
}

// A non-leaf function: it calls, so it needs stack space and gets a .pdata entry.
FIXTURE_EXPORT int fixture_calls_leaf(int a, int b) {
    return fixture_asm_leaf(a, b) * 3 + 1;
}

// clang-cl only keeps a __try region around calls (a bare faulting load would need /EHa), so
// the guarded read goes through a function that is never inlined.
__declspec(noinline) static int fixture_read(volatile int* pointer) {
    return *pointer;
}

FIXTURE_EXPORT int fixture_seh(volatile int* pointer) {
    __try {
        return fixture_read(pointer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

FIXTURE_EXPORT unsigned long long fixture_ticks(void) {
    return GetTickCount64();
}

// user32 is delay-loaded (see the link options), so this goes through the delay-load helper.
FIXTURE_EXPORT int fixture_metrics(void) {
    return GetSystemMetrics(SM_CXSCREEN);
}

// A table of code pointers in read-only data: each entry gets a DIR64 relocation.
typedef int (*FixtureUnary)(int);
FIXTURE_EXPORT const FixtureUnary fixture_table[2] = {fixture_asm_fall, fixture_asm_after};

FIXTURE_EXPORT void* fixture_asm_leaf_address(void) {
    return (void*)fixture_asm_leaf;
}

FIXTURE_EXPORT void* fixture_asm_after_address(void) {
    return (void*)fixture_asm_after;
}

// Thread-local data and a TLS callback.
__declspec(thread) int fixture_tls_value = 7;

static void NTAPI fixture_tls_callback(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    (void)reserved;
    if (reason == DLL_THREAD_ATTACH) {
        fixture_tls_value += 1;
    }
}

#pragma comment(linker, "/include:_tls_used")
#pragma comment(linker, "/include:fixture_tls_callback_entry")
#pragma section(".CRT$XLB", read)
__declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK fixture_tls_callback_entry =
    fixture_tls_callback;

FIXTURE_EXPORT void* fixture_tls_callback_address(void) {
    return (void*)fixture_tls_callback;
}

FIXTURE_EXPORT int fixture_tls_read(void) {
    return fixture_tls_value;
}
