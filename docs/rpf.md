# RPF9 archives

How the game's archives are laid out, as far as the project has checked, and what is still
open. Read for analysis only: the game's own code opens its archives when it runs, through
`runtime/io`, so the runtime never parses them itself. Nothing here decrypts anything. The tool
never extracts files, and everything it prints or writes about the user's archives stays on
their machine (`CLAUDE.md`, rules 1 and 2).

Code: `common/include/g6/rpf9.h` (the reader, `g6::rpf9`), `tools/rpfscan` (the command-line
census), `tests/unit/test_rpf9.cpp` (an archive built from made-up files). The layout comes
from RageForge's notes ([related-work.md](related-work.md)); our code is our own.

```
out\windows-release\rpfscan.exe info <archive>
out\windows-release\rpfscan.exe scan <archive or folder> [--threads n] [--out <dir under private/>]
```

## Layout

* **Header.** 64 bytes, magic `9FPR`. **Checked here** on all 22 archives. The word at 0x04 is
  the size of the table of contents that follows the header. The other words have names other
  readers gave them (`rpf9.h`), but only the table-of-contents size is relied on here.
* **Table of contents.** **Reported:** encrypted with AES-CBC under a key that isn't public.
  We don't decrypt it and don't need to. Archive keys stay out of the repo whatever happens
  (`CLAUDE.md`).
* **Data area.** **Reported:** starts at the first 16-byte boundary after the table of
  contents. **Checked here** only loosely: carving from that boundary works in all 22 archives,
  but the first file always starts later, often on a 4 KiB boundary (0x6000 in `anim.rpf`,
  0x1D000 in `levels.rpf`). The bytes between are a gap. It starts with zeros in 18 archives
  and with other bytes in four (`appdata0`, `common`, `core_restofworld`, `story000_bvh4`).
  What those bytes are is open.
* **Files.** **Checked here:**
  * A file is a run of zlib streams (CMF 0x78), each starting on a 16-byte boundary.
  * Each stream but the last inflates to exactly 64 KiB, and each starts at the 16-byte boundary
    after the one before.
  * Every stream's Adler-32 matches.
* **Stored data.** Some files aren't compressed at all. **Checked here:**
  * `movies.rpf` holds no zlib streams; all 8.23 GiB is stored.
  * Streams cover only 0.1% of `audio.rpf`. **Reported:** the rest is AWC sound banks.
  * In `shaders.rpf`, the shader records are stored ([shaders.md](shaders.md)).

## Carving

Without the table of contents, `carve` finds files by walking the data area:

1. Each thread takes 64 MiB of the data area.
2. At every 16-byte boundary, it tries to inflate whatever starts with a zlib header.
3. A stream counts only if it inflates and its Adler-32 matches. The thread then jumps to the
   boundary after the stream.
4. The segments are merged in order. A stream that starts inside an earlier one is dropped.
5. A stream that starts right where a 64 KiB stream ended continues that stream's file.
   Anything else starts a new file.
6. Whatever no stream covers, apart from the padding up to each boundary, is reported as a gap.

Limits:

* **Merged files.** A file whose size is an exact multiple of 64 KiB, followed directly by
  another file, can't be told apart from one longer file. Carving merges the two. Only the
  table of contents could split them.
* **Gaps.** A gap is either padding or stored data, and the carve doesn't say which.
  `rpfscan` prints the first four bytes of each gap to help tell them apart.
* **No names.** Carved files have offsets, not names. The names are in the table of contents.

`classify` labels each file by its first bytes:

* a paged resource: `0DRP` at 0x10, then its type;
* a paged object: a system-page pointer (0x5xxxxxxx) at 0x08 and no `0DRP` header;
* the driver's pipeline cache: `PS\0\t`;
* a reflection schema: `PSIN`;
* `RBF0`;
* XML, GFX, DDS, PNG, or text;
* otherwise "other".

## Census

**Checked here** with `rpfscan scan` over the whole 1.0.2.1 install, counts only:

* **Totals:** 22 archives, 150.23 GiB.
* **Streams and files:** 2,241,409 streams carved into 2,127,667 files. Those files take
  98.92 GiB and inflate to 339.95 GiB.
* **Gaps:** 51.29 GiB in all, mostly `audio.rpf` (26.57 GiB), `story000.rpf` (12.22 GiB), and
  `movies.rpf` (8.23 GiB).
* **Kinds:**

  | Kind | Files |
  |---|---|
  | other | 931,944 |
  | paged object | 884,709 |
  | paged resource | 290,294 |
  | reflection schema | 18,640 |
  | RBF0 | 1,278 |
  | XML | 545 |
  | text | 169 |
  | GFX | 87 |
  | pipeline cache | 1 |

  * Every paged resource has type 0x7A.
* **Largest archives:**
  * `levels.rpf`: 70.92 GiB, 1,049,877 files.
  * `core_bvh4.rpf`: 7.73 GiB, 565,590 files. They inflate from 7.16 GiB to 121.01 GiB, the
    highest ratio in the install.

The whole scan took about seven minutes here, 263 s of it on `levels.rpf`.

## Open questions

* **Header word 0x24.** Other readers call it the entry count. It is far below the number of
  files carved: 265 against 115,118 in `anim.rpf`, 2,078 against 1,049,877 in `levels.rpf`,
  and 6,534 against 2,127,667 summed over the install. It may count directories or
  table-of-contents blocks rather than files.
* **zlib-looking gaps.** In `data.rpf`, 101 gaps start with a zlib header and hold a complete
  DEFLATE stream. Their 4-byte trailer isn't the Adler-32 of what they inflate to.
  * Every one follows a file whose size is a multiple of 64 KiB.
  * About half of them inflate to 64 KiB or more.
  * Ruled out for the trailer:
    * an Adler-32 carried over from the file before;
    * the Adler-32 in little-endian order;
    * an Adler-32 started at zero;
    * the Adler-32 of only the first 64 KiB;
    * CRC-32.
  * Another 112 zlib-headed gaps in `data.rpf` aren't DEFLATE at all.
* **The first gap.** What lies between the data area's start and the first file.
* **Compression.** Whether any archive data uses a format besides zlib that console hardware
  decompresses. The carve sees zlib and stored data only, and it would report anything else as
  a gap. The gaps are known to hold AWC banks, Bink video, and the shader records, but not all
  of them are explained.
