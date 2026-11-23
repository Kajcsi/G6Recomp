/* Hand-written functions for the pre-flight scan tests (docs/scan.md), written for these tests.
 * Each one holds an instruction or access pattern the scan has to find. The DLL is only ever
 * read as a file and never loaded or run: some of these instructions fault in user mode, and
 * some aren't on every host. Functions that need a .pdata range carry SEH directives. */

/* clang-format off */
__asm__(
    ".intel_syntax noprefix\n"

    ".data\n"
    ".p2align 3\n"
    /* A relocated pointer to code in the middle of scan_pointed. */
    "scan_pointer_slot: .quad .Lscan_pointed_target\n"

    ".text\n"

    /* ---- CPU features ---- */

    ".globl scan_adx\n"
    ".def scan_adx; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_adx:\n"
    "    adcx rax, rcx\n"
    "    ret\n"

    ".globl scan_sha\n"
    ".def scan_sha; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_sha:\n"
    "    sha1rnds4 xmm0, xmm1, 0\n"
    "    ret\n"

    ".globl scan_sse4a\n"
    ".def scan_sse4a; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_sse4a:\n"
    "    extrq xmm0, 8, 0\n"
    "    ret\n"

    ".globl scan_clwb\n"
    ".def scan_clwb; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_clwb:\n"
    "    clwb byte ptr [rcx]\n"
    "    ret\n"

    /* AVX-512, which Zen 2 doesn't have. */
    ".globl scan_avx512\n"
    ".def scan_avx512; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_avx512:\n"
    "    vpaddd zmm0, zmm1, zmm2\n"
    "    ret\n"

    /* endbr64 is a no-op without CET, so it doesn't count as a feature. */
    ".globl scan_cet\n"
    ".def scan_cet; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_cet:\n"
    "    endbr64\n"
    "    xor eax, eax\n"
    "    ret\n"

    /* ---- System instructions, traps, timing ---- */

    ".globl scan_syscall\n"
    ".def scan_syscall; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_syscall:\n"
    "    mov r10, rcx\n"
    "    mov eax, 0x55\n"
    "    syscall\n"
    "    ret\n"

    ".globl scan_int2e\n"
    ".def scan_int2e; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_int2e:\n"
    "    int 0x2e\n"
    "    ret\n"

    ".globl scan_traps\n"
    ".def scan_traps; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_traps:\n"
    "    test ecx, ecx\n"
    "    jz 1f\n"
    "    int 0x2c\n"
    "1:\n"
    "    ud2\n"

    ".globl scan_timing\n"
    ".def scan_timing; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_timing:\n"
    "    rdtsc\n"
    "    shl rdx, 32\n"
    "    or rax, rdx\n"
    "    ret\n"

    ".globl scan_cpu\n"
    ".def scan_cpu; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_cpu:\n"
    "    xor ecx, ecx\n"
    "    cpuid\n"
    "    xgetbv\n"
    "    ret\n"

    /* ---- fs: and gs: ---- */

    ".globl scan_fs\n"
    ".def scan_fs; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_fs:\n"
    "    mov rax, qword ptr fs:[0x30]\n"
    "    ret\n"

    ".globl scan_peb\n"
    ".def scan_peb; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_peb:\n"
    "    mov rax, qword ptr gs:[0x60]\n"
    "    ret\n"

    /* ClientId.UniqueThread, a TEB field the side DLLs don't read. */
    ".globl scan_thread_id\n"
    ".def scan_thread_id; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_thread_id:\n"
    "    mov eax, dword ptr gs:[0x48]\n"
    "    ret\n"

    /* LastErrorValue, through the TEB's Self pointer. */
    ".globl scan_last_error\n"
    ".def scan_last_error; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_last_error:\n"
    "    mov rax, qword ptr gs:[0x30]\n"
    "    mov eax, dword ptr [rax + 0x68]\n"
    "    ret\n"

    /* A TlsSlots entry picked by a register. */
    ".globl scan_computed_slot\n"
    ".def scan_computed_slot; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_computed_slot:\n"
    "    mov rax, qword ptr gs:[0x1480 + rcx*8]\n"
    "    ret\n"

    /* The TLS array's first slot, assuming the module's TLS index is 0. */
    ".globl scan_fixed_slot\n"
    ".def scan_fixed_slot; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_fixed_slot:\n"
    "    mov rax, qword ptr gs:[0x58]\n"
    "    mov rax, qword ptr [rax]\n"
    "    mov eax, dword ptr [rax + 16]\n"
    "    ret\n"

    /* The TLS array indexed by a value that isn't this module's TLS index. */
    ".globl scan_other_index\n"
    ".def scan_other_index; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_other_index:\n"
    "    mov rax, qword ptr gs:[0x58]\n"
    "    mov rax, qword ptr [rax + rcx*8]\n"
    "    mov eax, dword ptr [rax + 16]\n"
    "    ret\n"

    /* The TLS array pointer is overwritten before anything reads through it. */
    ".globl scan_lost_array\n"
    ".def scan_lost_array; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_lost_array:\n"
    "    mov rax, qword ptr gs:[0x58]\n"
    "    mov rax, rcx\n"
    "    mov eax, dword ptr [rax]\n"
    "    ret\n"

    /* ---- Code as data ---- */

    /* Sums four bytes of scan_adx through a register, then reads scan_sha directly. */
    ".globl scan_checksum\n"
    ".def scan_checksum; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_checksum:\n"
    "    lea rdx, [rip + scan_adx]\n"
    "    xor eax, eax\n"
    "    xor ecx, ecx\n"
    "2:\n"
    "    movzx r8d, byte ptr [rdx + rcx]\n"
    "    add eax, r8d\n"
    "    inc ecx\n"
    "    cmp ecx, 4\n"
    "    jb 2b\n"
    "    add eax, dword ptr [rip + scan_sha]\n"
    "    ret\n"

    ".globl scan_tiny\n"
    ".def scan_tiny; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_tiny:\n"
    "    xor eax, eax\n"
    "    ret\n"

    /* Overwrites the first byte of scan_tiny. */
    ".globl scan_patch\n"
    ".def scan_patch; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_patch:\n"
    "    mov byte ptr [rip + scan_tiny], 0xc3\n"
    "    ret\n"

    /* Code reached only through an address the function takes itself: the walk stops at the
     * indirect jump, and the scan walks again from the taken address. */
    ".globl scan_continuation\n"
    ".def scan_continuation; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_continuation:\n"
    ".seh_proc scan_continuation\n"
    "    sub rsp, 40\n"
    ".seh_stackalloc 40\n"
    ".seh_endprologue\n"
    "    lea rax, [rip + 3f]\n"
    "    jmp rax\n"
    "3:\n"
    "    rdpid rax\n"
    "    add rsp, 40\n"
    "    ret\n"
    ".seh_endproc\n"

    /* Code reached only through the relocated pointer in scan_pointer_slot. */
    ".globl scan_pointed\n"
    ".def scan_pointed; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_pointed:\n"
    ".seh_proc scan_pointed\n"
    "    sub rsp, 40\n"
    ".seh_stackalloc 40\n"
    ".seh_endprologue\n"
    "    mov rax, qword ptr [rip + scan_pointer_slot]\n"
    "    jmp rax\n"
    ".Lscan_pointed_target:\n"
    "    clzero\n"
    "    add rsp, 40\n"
    "    ret\n"
    ".seh_endproc\n"

    /* .pdata code nothing reaches: only the sweep decodes the syscall and hlt after the jump. */
    ".globl scan_sweep\n"
    ".def scan_sweep; .scl 2; .type 32; .endef\n"
    ".p2align 4, 0xcc\n"
    "scan_sweep:\n"
    ".seh_proc scan_sweep\n"
    "    sub rsp, 40\n"
    ".seh_stackalloc 40\n"
    ".seh_endprologue\n"
    "    add rsp, 40\n"
    "    jmp rcx\n"
    "    syscall\n"
    "    hlt\n"
    "    ret\n"
    ".seh_endproc\n"

    ".att_syntax prefix\n");
/* clang-format on */
