# Notices

G6Recomp is licensed under the GNU General Public License, version 3 or (at your option) any
later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE).

Some code may come from other open-source projects under licences compatible with ours. Every
file that contains copied or ported code keeps the original copyright and licence notice at
the top, and is listed below. The rules for what may be taken are in `CLAUDE.md` (Conventions)
and [docs/related-work.md](docs/related-work.md).

## Copied or ported code

| Path in this repo | Source project | Source commit | Source path | Licence |
|---|---|---|---|---|
| `tools/shaderrecomp/src/rdna2.h` | [xbox-emulator](https://github.com/omonkeyz/xbox-emulator) | `51f5319` | `crates/tombox-xshader/src/rdna2.rs` | GPL-3.0-or-later |
| `tools/shaderrecomp/src/rdna2.cpp` | [xbox-emulator](https://github.com/omonkeyz/xbox-emulator) | `51f5319` | `crates/tombox-xshader/src/rdna2.rs` | GPL-3.0-or-later |
| `tools/shaderrecomp/src/rdna2_names.cpp` | [xbox-emulator](https://github.com/omonkeyz/xbox-emulator) | `51f5319` | `crates/tombox-xshader/src/rdna2/mnem.rs` | GPL-3.0-or-later |
| `tools/shaderrecomp/src/rdna2.h` | [TomBox](https://github.com/drycheetah84/TomBox), through xbox-emulator | `7e8064f` | `crates/tombox-xshader/src/rdna2.rs` | MIT OR Apache-2.0, used under MIT |
| `tools/shaderrecomp/src/rdna2.cpp` | [TomBox](https://github.com/drycheetah84/TomBox), through xbox-emulator | `7e8064f` | `crates/tombox-xshader/src/rdna2.rs` | MIT OR Apache-2.0, used under MIT |
| `tools/shaderrecomp/src/rdna2_names.cpp` | [TomBox](https://github.com/drycheetah84/TomBox), through xbox-emulator | `7e8064f` | `crates/tombox-xshader/src/rdna2/mnem.rs` | MIT OR Apache-2.0, used under MIT |

Each file was ported from Rust to C++ and extended; its header lists what changed.

xbox-emulator continues TomBox, and most of the two Rust files we ported are TomBox's code: 483
of the 519 lines of `rdna2.rs` and 267 of the 410 lines of `rdna2/mnem.rs` are unchanged from
TomBox `7e8064f`, the commit xbox-emulator names as its base. TomBox has no licence file, but
its `tombox-xshader` crate declares `MIT OR Apache-2.0` in its `Cargo.toml`, from the crate's
first commit (`b8fdbe4`) on. We use that code under MIT, whose notice follows.

> MIT License
>
> Copyright (c) 2026 the TomBox developers
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software
> and associated documentation files (the "Software"), to deal in the Software without
> restriction, including without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
> Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
> BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
> DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Libraries

Libraries under `thirdparty/` are git submodules and keep their own licence files.

| Library | Licence |
|---|---|
| [Zydis](https://github.com/zyantific/zydis) | MIT |
| [fmt](https://github.com/fmtlib/fmt) | MIT |
