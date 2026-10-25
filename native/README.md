# native/: the campaign's native D3D12 renderer

See [docs/NATIVE_RENDERER.md](../docs/NATIVE_RENDERER.md) for the design and progress.

## Provenance and licenses

Most files here are ported from **pgr4-native** (https://github.com/shaunsingh/pgr4-native),
commit `8ea20d5` (2026-10-25), `src/native/`. Its license (MIT) is in
[LICENSE.pgr4-native](LICENSE.pgr4-native). The changes for GRAW:
- renamed: namespace `pgr4::native` to `graw::native`, options `pgr4_native*` to `graw_native*`,
  log prefix `PGR4 native` to `GRAW native`;
- GRAW's own D3D front end (`graw_*` files), which replaces pgr4-native's device-shadow tracker
  and PGR4 hook table.

pgr4-native ports code from the ReXGlue SDK and, through it, from Xenia (BSD-3-Clause; the
ported functions are named in each file's header): among others `draw`, `render_targets`,
`resolve`, `geometry_shader` and the texture code. The ReXGlue SDK's license, with Xenia's
notice, applies to those parts:

```
Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>

Portions of this software are derived from the Xenia project:
Copyright (c) 2022, Ben Vanik and Xenia project contributors
https://xenia.jp

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
