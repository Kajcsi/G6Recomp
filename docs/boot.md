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
| 1 | The first `XMemVirtualAlloc` calls: two 64 KiB reserves, then 2 MiB pages | Accepting those forms | Done: `XMemVirtualAlloc` goes through `memory_tracker` ([hle-status.md](hle-status.md)) |
| 2 | The 2 MiB reserve-and-commit calls carry an attribute word and `PAGE_GRAPHICS_*` bits (0x800 to 0x40000) on top of `PAGE_READWRITE` | Accepting the attribute word, dropping the graphics bits and keeping the CPU protection; the seven start-up allocations then succeed | Done, the same way: `memory_tracker` records the graphics bits with the range |
| 3 | The Game Runtime's import thunks treat a missing `XErrorReport` as fatal | Providing it | Done: our `xgameruntime.dll` has it ([gdk.md](gdk.md)) |
| 4 | The game's allocator reads its static TLS from module index 0 (it is built with `/GA`) | Giving the executable index 0 | Done: the runner links `g6exetls` ([execution-model.md](execution-model.md), "TLS and the TEB") |
| 5 | A fatal assert when `DStorageGetFactory` returns null | A DirectStorage factory | Done: `runtime/io` serves the factory, files, queues and status arrays ([io.md](io.md), "DirectStorage") |
| 6 | `XMemAllocatePhysicalPages` is missing, and the game asserts when it returns FALSE | A CPU-only stand-in (below) | Done: `memory_tracker` backs the pages (below) |
| 7 | `D3D12XboxCreateDevice` succeeds, then device slot 26 is called with the return buffer first | Swapping the first two arguments (below) | M3: `runtime/gpu` |
| 8 | Device slot 48 is forwarded to the PC's slot 48, which is something else | Creating a command queue (below) | M3 |
| 9 | Device slot 73 is forwarded to the PC's slot 73, and the game then reads through a null list | Creating a command list (below) | M3 |
| 10 | An `XMemVirtualAlloc` reserve of about 185 MiB, with allocation type `0x42000`, attribute 7 and `PAGE_NOACCESS`, fails with `ERROR_INVALID_PARAMETER` | Reading `0x40000` as `MEM_RESERVE_PLACEHOLDER`, which only `VirtualAlloc2` accepts, and reserving a placeholder | Done in `memory_tracker`, not reached yet |
| 11 | The game carves five ranges out of that placeholder with `MEM_RESERVE \| MEM_REPLACE_PLACEHOLDER \| MEM_PHYSICAL \| MEM_LARGE_PAGES` (`0x20406000`), then allocates more physical pages | A physical reserve inside a placeholder stays a placeholder, which page maps split later; any other replacing reserve splits the placeholder and allocates the range in place | Done in `memory_tracker`, not reached yet |
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

Later findings change the backing ([related-work.md](related-work.md)):

* **Page numbers.** xcl and Leonida (`042c5f37`) agree that a page number counts 4 KiB frames:
  a 64 KiB page's number is a nonzero multiple of 16, and 0 in a map call unmaps that page. The
  game stores numbers shifted right by 4 and checks they stay below 2^25.
* **Scale.** xcl saw about 93,000 pages, about 5.7 GiB, allocated in one call, so a section per
  page is too many. Leonida now backs each allocation call with one pagefile-backed section and
  maps each page as its own view (`MapViewOfFile3` into a 64 KiB piece of the placeholder), so a
  page keeps its contents across remaps and can be mapped at two addresses.
* **Protection.** A mapped page takes the protection of the window it lands in. Two windows the
  game reserves are `PAGE_NOACCESS`, and their pages still have to be readable and writable.
* **Release.** The game frees its windows with plain `VirtualFree`, and splits and coalesces
  placeholders itself with `VirtualAlloc` and `VirtualFree` (xcl). Those imports have to go
  through `memory_tracker` too, and the first placeholder has to lie below 4 GiB.

`memory_tracker` now does the first three of these, ported from Leonida (`dde82f27`,
`runtime/shims/xmem.cpp`; [NOTICES.md](../NOTICES.md)):

* Each allocation call gets one pagefile-backed section of its pages. Page numbers are 4 KiB
  frame numbers, 16 per page, and the number of a freed page is handed out again.
* A map splits a 64 KiB piece out of the range's placeholder and maps the page's 64 KiB of the
  section there as its own view, so a page keeps its contents across remaps and can be mapped at
  two addresses. A view takes its range's protection, or read-write where the range has none or
  `PAGE_NOACCESS`. An unmap turns the piece back into a placeholder.
* A page freed while mapped keeps its memory until its views go, and releasing a whole range
  unmaps its views.

Maps work only in ranges reserved for physical pages, at 64 KiB addresses. The fourth point, the
`VirtualAlloc` and `VirtualFree` imports and the first placeholder below 4 GiB, is still to do.
The sections and the page table belong to `memory_tracker`, so the GPU side can follow at M3.

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
  that address. Correction (xcl, Xbox2Windows): `0xFFFFFFFF` is a layout, not a state. It is
  `D3D12_BARRIER_LAYOUT_UNDEFINED`, so the call uses the enhanced-barrier form.
* A proxy vtable has to be longer than the highest slot the game calls. A short one leaves a
  null pointer, not an error.

## Our runs

**Checked here.** Each run goes as far as it can with `--skip-int3`, on Windows 11 with the
debug build. The table gives what each run stopped on and what came before the next one. The
logs stay in `private/runs/`.

| Run | First stop | What came next |
|---|---|---|
| 1 | `D3D12ConfigureVirtualMemoryX` and `XMemVirtualAlloc` are unbound, and the game crashes on the null result | `D3D12ConfigureVirtualMemoryX` logs its block and succeeds. The block is zero apart from the DWORD at +4, which is 4 here. The two reserves that follow are 24 GiB each, of type `0x20402000` (without commit), with attribute and protection words 0x21 and 4, then 3 and 0x801, as Xbox2Windows reported (checked at debug level on run 6). By Xbox2Windows's static read, the executable keeps the first as its CPU range and the second as the GPU's view of it once it has a device. Then come 2 MiB-page commits. `XMemVirtualAlloc` goes through `memory_tracker` |
| 2 | The app-state notification functions (`api-ms-win-core-psm-appnotify`) are unbound | Forwarded to the host |
| 3 | `GameInputCreate` (GameInput ordinal 1) is unbound, and the game asserts | A stand-in `IGameInput` with no devices: slot 3, the timestamp, works, and every other slot is logged and fails |
| 4 | `DStorageGetFactory` is unbound, and the game asserts (Leonida's stop 5). It called `IGameInput` slot 9 first and went on after the failure | A stand-in factory |
| 5 | The factory's slot 8 (`SetStagingBufferSize`, 32 MiB) and slot 4 (`OpenFile`) fail; the game asserts after each, then crashes in `memset` with a null destination | The factory, files, queues and status arrays of [io.md](io.md), "DirectStorage" |
| 6 | `XMemAllocatePhysicalPages` is unbound (Leonida's stop 6); after the assert the game reads through a null pointer | The physical-page functions (above) |

From run 2 on, the game also asks `QueryApiImpl` for Game Runtime services we don't serve yet:
XNetworkingImpl, XGameImpl, XSystemImpl, XSystemAnalyticsImpl, and services `af406016`,
`f4faf4d4` and `03f0fe74`. It also asks XUserImpl for the class ID as an interface. Each miss is
logged and reported through `XErrorReport`, and the game goes on. Run 6 also called
`RegisterClassExA`, which is still a stub, before its stop.
