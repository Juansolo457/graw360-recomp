# License texts: where they come from

The root `LICENSE` (MIT) covers this project's own files. The texts here are the third-party
licenses that apply to code this repository carries or patches; `NOTICE` says which files each
one covers. They are byte-for-byte the same files as fear360-recomp's `LICENSES/`.

| File | Copied verbatim from | Applies to |
|---|---|---|
| `GPL-3.0.txt` | `LICENSE` of [fear-nx](https://github.com/infriker/fear-nx) at commit `acad80d`, which fear-nx copied verbatim from [Project8Recomp](https://github.com/R-drg/project8-sw) (commit `3393f2615deb7e8ac11ce51d2cd0e9defded5669`). [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) ships the same text. | fear-nx's renderer and its lineage (Project8Recomp, nfsmw-nx), which fear-nx licenses GPL-3.0-or-later, and what is derived from it here |
| `ReXGlue-BSD-3-Clause.txt` | `LICENSE` of the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) at commit `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518` (unchanged since the SDK's initial commit; identical to fear-nx's `LICENSES/ReXGlue-BSD-3-Clause.txt`). It carries both ReXGlue's and the Xenia project's copyright notices. | ReXGlue code: `rexglue init` scaffolding, the SDK source in `tools/patches/`, code taken from the SDK |
| `Xenia-BSD-3-Clause.txt` | `LICENSE` of [Xenia](https://github.com/xenia-project/xenia) (master, retrieved 2026-10-24) | Code that originates in Xenia, reached through ReXGlue or fear-nx (for example the texture cache and geometry shader code in `port-yeti-nx/src/native/`) |

These are license texts and notices only. They don't change which license a file is under;
`NOTICE` and each file's own header do.
