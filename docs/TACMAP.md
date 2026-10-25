# Tactical map shows no map (#3): root cause and fix

Status 2026-10-24: **fixed** in the code generator (`tools/patches/rexglue-bd833a2-vpkd3d128-alias.patch`),
verified by screenshots in Mission 1: the buildings, grid and terrain texture draw inside the
compass ring and turn with the view.

## How the map is drawn
- Each frame, the map's 3D draws (buildings, terrain, grid) render into a 768x598 viewport at
  (512, 64) and are resolved to a 1280x720 texture. This always worked: a dump of that texture
  showed the buildings.
- The next frame, one full-screen quad (vs D46836FDA9466337, ps 3C1234E688DF28DC, a
  DrawPrimitiveUP from `sub_821823A0`) composites that texture with a mask. Its camera is an
  orthographic projection, 16/9 wide and 1 high, centered, with the identity view
  (`sub_821820C8`, view `0x828F1F40`).
- The quad's rectangle comes from a render command (handler `sub_821F3D98`, built by
  `sub_821F3FC8`) that carries x, z, width and height as half floats, so the full screen is
  (-0.5, -0.5, 1, 1), x scaled by the aspect.

## What went wrong
- The quad arrived as (+0.5, +0.5, 1, 1): x 0.889..2.667, NDC 1..3, entirely off the screen.
  The command's halves were `3800 3800 3C00 3C00`; they should be `B800 B800`.
- `sub_821F3FC8` packs each float with `vpkd3d128 v0,v0,...` (float to float16). ReXGlue's
  generated code for the float16 packs writes the destination one halfword at a time and reads
  the sign from the source afterwards. With vD == vB the high halfword of element 0 is already
  replaced when its sign is read, so element 0 always comes out positive. The game packs single
  values in element 0, so every negative value packed this way loses its sign.
- The NORMSHORT4 pack has the same hazard (it writes halfwords of words it hasn't read yet).

## Fix
`build_vpkd3d128` in `src/codegen/builders/vector.cpp` (ReXGlue bd833a2): when vD and vB are the
same register, the generated code copies vB to a local `vPackSrc` first and reads from that.
37 of the campaign's 39 `vpkd3d128` sites are aliased and change.

To use it here, the patched `rexglue` CLI is built from `third_party/rexglue-sdk-bd833a2` and run
by hand before `tools/build.ps1` (the build itself runs the SDK's prebuilt CLI, which keeps the
stamp and so leaves the regenerated sources alone):

```
<sdk>/out/win-amd64/RelWithDebInfo/rexgluerd.exe codegen --ignore-stamp port-yeti/grawyeti_manifest.toml
```

The front end (`port`) and Instant Action (`port-mp`) had one such site each; both were
regenerated the same way on 2026-10-24 and boot to their menus. Players get the fix only once
ReXGlue has it or the build uses a patched CLI (the same open question as the renderer fix for #1).

## Audit for the same class of bug
`tools/check_vector_aliasing.py <generated dir>` scans every translated instruction and flags
generated code that reads bytes of a vector register after the same instruction has written
them (a destination aliasing a source). On the old code generator it finds exactly the
`vpkd3d128` sites; after the fix, all three programs are clean (13,838 vector instructions in
the campaign, 30,796 in the front end, 95 and 92 mnemonics). It doesn't check the semantics of
each instruction, only this ordering hazard.

## Probes added
`--graw_debug_map` (port/src/resolve_probe.cpp, three manifest hooks): the ortho camera's
matrices, the panel rect before the draw, and the raw panel command with its caller chain.
The renderer diagnostics gained `--graw_seq_at` (when to log a frame's draws) and a log of the
map composite's constants and vertices.
