# Boot

What stands between the 1.0.2.1 executable's entry point and its first frame, in the order the
game reaches it. Prose, export names, flags, slot numbers and counts only: no addresses or code
from the executable.

Each stop says where it comes from. **Reported** means another project measured it and we
haven't reached it yet; **checked here** means our own run got there.

## Leonida's retail run

**Reported** by Leonida (`86fd2fda`, `docs/retail_boot_chain.md`): one run of the 1.0.2.1
executable under its own runtime on Windows 11 with an RTX 4090, on 2026-11-23. Each stop is
the first failure once every fix before it is in place, so the list shows the order of the
problems, not all of them. Most of Leonida's fixes are local stand-ins, tested only as far as
"the game goes on to the next call".

| # | What stops the game | What got it further upstream | Ours |
|---|---|---|---|
| 1 | The first `XMemVirtualAlloc` calls: two 64 KiB reserves, then 2 MiB pages | Accepting those forms | To do: `XMemVirtualAlloc` isn't bound yet ([hle-status.md](hle-status.md)) |
| 2 | The 2 MiB reserve-and-commit calls carry an attribute word and `PAGE_GRAPHICS_*` bits (0x800 to 0x40000) on top of `PAGE_READWRITE` | Accepting the attribute word, dropping the graphics bits and keeping the CPU protection; the seven start-up allocations then succeed | To do, the same way, with the graphics bits recorded for `memory_tracker` |
| 3 | The Game Runtime's import thunks treat a missing `XErrorReport` as fatal | Providing it | Done: our `xgameruntime.dll` has it ([gdk.md](gdk.md)) |
| 4 | The game's allocator reads its static TLS from module index 0 (it is built with `/GA`) | Giving the executable index 0 | Done: the runner links `g6exetls` ([execution-model.md](execution-model.md), "TLS and the TEB") |
| 5 | A fatal assert when `DStorageGetFactory` returns null | A DirectStorage factory | To do: `runtime/io` |
| 6 | `XMemAllocatePhysicalPages` is missing, and the game asserts when it returns FALSE | A CPU-only stand-in (below) | To do |
| 7 | `D3D12XboxCreateDevice` succeeds, then device slot 26 is called with the return buffer first | Swapping the first two arguments (below) | M3: `runtime/gpu` |
| 8 | Device slot 48 is forwarded to the PC's slot 48, which is something else | Creating a command queue (below) | M3 |
| 9 | Device slot 73 is forwarded to the PC's slot 73, and the game then reads through a null list | Creating a command list (below) | M3 |
| 10 | An `XMemVirtualAlloc` reserve of about 185 MiB, with allocation type `0x42000`, attribute 7 and `PAGE_NOACCESS`, fails with `ERROR_INVALID_PARAMETER` | Reading `0x40000` as `MEM_RESERVE_PLACEHOLDER`, which only `VirtualAlloc2` accepts, and reserving a placeholder | To do |
| 11 | The game carves five ranges out of that placeholder with `MEM_RESERVE \| MEM_REPLACE_PLACEHOLDER \| MEM_PHYSICAL \| MEM_LARGE_PAGES` (`0x20406000`), then allocates more physical pages | A physical reserve inside a placeholder stays a placeholder, which page maps split later; any other replacing reserve splits the placeholder and allocates the range in place | To do |
| 12 | Device slot 133 is called through a proxy vtable of 128 slots, so it jumps through a null pointer | A 192-slot table and a stand-in for slot 133 (below) | M3 |
| 13 | Right after the first `CreateDescriptorHeap`, the game calls the heap's slot 9 (`GetCPUDescriptorHandleForHeapStart` on the PC) and asserts that the handle equals a value it already holds | Open. Leonida's guess: a console descriptor heap starts at an address the game chose or can predict | M3 |

By the last stop the game had made 1,185 D3D12 calls: the device, 5 command queues, 5 fences, 7
command allocators, 581 command lists created and closed, a shader cache session, one failed
`CreateReservedResource2`, the first sampler, one committed resource and the first descriptor
heap.

Without a debugger attached, the `int3` of a fatal assert ended the process with `C0000374`
(heap corruption) or `C0000005` while it tore down, not with `80000003`. So an exit code of
`C0000374` can be an assert rather than a heap bug. Our runner's crash report names the
`int3` itself, and `--skip-int3` steps over soft asserts
([execution-model.md](execution-model.md), "The runner").

### Physical pages

The call shapes, as Leonida read them from the call sites and the checks after them:

* `XMemAllocatePhysicalPages(attributes, page_count, page_array)`: `page_count` is in and out,
  in 64 KiB pages, and the game checks both the result and the count written back. Attribute
  words seen: `0x280` and `0x380`.
* The range for the pages is reserved with `XMemVirtualAlloc` and
  `MEM_RESERVE | MEM_PHYSICAL | MEM_LARGE_PAGES` (`0x20402000`), without `MEM_COMMIT`.
* `XMemMapPhysicalPages(address, page_count, page_array)` maps them; a null array unmaps.
  Success is a non-null result.
* `XMemMapPhysicalPagesScatter(addresses, page_count, page_array)` is the same with one address
  per page.
* `XMemFreePhysicalPages(page_count, page_array)` frees them.

Leonida's stand-in is CPU only: one pagefile-backed section per 64 KiB page, with page numbers
that index a table of those sections. The physical reserve is a placeholder; each map splits
the placeholder and maps the page's section into it with `MapViewOfFile3`, and each unmap puts
the placeholder back. That is also the plan here, with the sections owned by `memory_tracker`
so the GPU side can follow at M3.

### D3D12 device calls that differ from the PC

For `runtime/gpu` at M3 ([d3d12x.md](d3d12x.md)):

* **Slot 26** (`GetCustomHeapProperties` on the PC) returns a structure. The console call passes
  the return buffer first and the device second; the PC method expects the device first. In the
  swapped call the first argument isn't the device, which is how a proxy can tell. Other methods
  that return structures probably follow the same rule; only slot 26 was reached.
* **Slot 48** creates a command queue from a description that starts with the PC's
  `D3D12_COMMAND_QUEUE_DESC` and goes on with console words. Four queues come from it, the first
  a compute queue; the first queue of all comes from slot 8, as on the PC. On the PC, slot 48 is
  `OpenExistingHeapFromAddress`.
* **Slot 73** creates a command list from a 16-byte description whose first word is the list
  type, an allocator, a pipeline state, and an interface ID the PC SDK doesn't have.
* **Slot 133** places a buffer at a virtual address: the start of a physical-page range the game
  just reserved, with a description of that range's size, console bits in its flags above the
  PC's, and an initial state of `0xFFFFFFFF`, which the PC doesn't define. Its shape matches the
  console's `CreatePlacedResourceX`. A real version has to back the resource with the pages at
  that address.
* A proxy vtable has to be longer than the highest slot the game calls. A short one leaves a
  null pointer, not an error.

## Our runs

None yet. The first comes with `XMemVirtualAlloc` and the physical-page functions: each run
goes as far as it can with `--skip-int3`, and its first stop is recorded here.
