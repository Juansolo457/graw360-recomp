#!/usr/bin/env bash
# Set up what tools/build-linux.sh needs that isn't in the repository, on Linux x86-64 (#38; the
# Linux counterpart of tools/setup.ps1):
#   1. third_party/rexglue-sdk-bd833a2: ReXGlue's source at the nightly 0.10.0.24-dev.gbd833a2's
#      commit with this project's patches (tools/patches), checked against the same patched tree
#      as on Windows;
#   2. that SDK built and installed into its out/install/linux-amd64, RelWithDebInfo and Release:
#      the libraries the game links, the patched renderer plugin (resolves into freed memory,
#      vblank pacing, gpu_skip_draws) and the patched code generator (vpkd3d128,
#      docs/TACMAP.md), which the SDK installs from Release only.
# Needs clang (20 or later), CMake 3.25+, Ninja, git, python3 and the SDK's system libraries
# (docs/SETUP.md, "Linux"). The SDK build takes a while the first time (FFmpeg among others).
#   tools/setup-linux.sh             # all of it (skips what is there already)
#   tools/setup-linux.sh --force     # replace an existing SDK source checkout and rebuild
#   tools/setup-linux.sh --jobs 12
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="$root/third_party/rexglue-sdk-bd833a2"
sdk_commit=bd833a2ab69d305f99ddf57418fc38ffec8af93d
# As tools/setup.ps1, in order.
sdk_patches=(rexglue-bd833a2-vpkd3d128-alias rexglue-bd833a2-freed-page-resolve
             rexglue-bd833a2-vblank-pacing rexglue-bd833a2-gpu-skip-draws
             pgr4-native/0021-gpu-sample-computed-LOD-pixel-shader-fetches-with-sa
             pgr4-native/0022-gpu-add-a-pixel-shader-modification-for-textures-kno
             rexglue-bd833a2-implicit-gradients-switch)
expected_tree=4854c57ebf6d8a8d818244959764980915c96c24  # after the last patch
force=0
jobs=$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))
while [[ $# -gt 0 ]]; do
  case "$1" in
    --force) force=1 ;;
    --jobs) jobs="$2"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

for cmd in git python3 cmake ninja clang clang++; do
  command -v "$cmd" >/dev/null || { echo "$cmd is not installed (docs/SETUP.md, Linux)" >&2; exit 1; }
done

# 1. The SDK source with this project's patches.
if [[ -d "$sdk" && $force -eq 1 ]]; then rm -rf "$sdk"; fi
if [[ -d "$sdk" ]]; then
  # A finished checkout has HEAD at the expected tree and no changes of its own; anything else
  # stopped partway or was changed by hand, and replacing it is --force's job.
  head=$(git -C "$sdk" rev-parse --verify --quiet 'HEAD^{tree}' 2>/dev/null || true)
  changes=$(git -C "$sdk" status --porcelain --ignore-submodules=all 2>/dev/null || true)
  if [[ "$head" != "$expected_tree" || -n "$changes" ]]; then
    echo "SDK source: $sdk is incomplete, on another commit or changed by hand: run with --force" >&2
    exit 1
  fi
  echo "SDK source: $sdk exists already (--force replaces it)"
else
  echo 'SDK source: cloning ReXGlue'
  mkdir -p "$(dirname "$sdk")"
  git clone --no-checkout https://github.com/rexglue/rexglue-sdk.git "$sdk"
  git -C "$sdk" checkout --quiet --detach "$sdk_commit"
  echo "SDK source: applying ${#sdk_patches[@]} patches"
  for name in "${sdk_patches[@]}"; do
    # The patches are LF; a Windows checkout of this repository may have given them CRLF.
    sed 's/\r$//' "$root/tools/patches/$name.patch" | git -C "$sdk" apply -
  done
  git -C "$sdk" add -A
  tree=$(git -C "$sdk" write-tree)
  if [[ "$tree" != "$expected_tree" ]]; then
    echo "The patched SDK tree is $tree, not $expected_tree: tools/patches changed without" \
         "tools/setup-linux.sh, or a patch applied differently" >&2
    exit 1
  fi
  git -C "$sdk" -c user.name=graw360-recomp -c user.email=setup@localhost commit --quiet \
      -m 'graw360-recomp patches'
  git -C "$sdk" switch --quiet -c graw360
fi
# Every run, so one that stopped while fetching them picks up where it left off.
echo 'SDK source: fetching its submodules'
git -C "$sdk" submodule update --init --recursive

# 2. Build and install the SDK.
install="$sdk/out/install/linux-amd64"
# RelWithDebInfo libraries carry the SDK's "rd" postfix (librexgpu-xenosrd.so); the code generator
# is Release's bin/rexglue.
if [[ -x "$install/bin/rexglue" && -f "$install/lib/librexruntimerd.so" && $force -eq 0 ]]; then
  echo "SDK: built ($install)"
else
  (cd "$sdk" && cmake --preset linux-amd64 -DREXGLUE_BUILD_TESTS=OFF)
  for config in RelWithDebInfo Release; do
    echo "SDK: building $config ($jobs jobs)"
    cmake --build "$sdk/out/build/linux-amd64" --config "$config" --parallel "$jobs"
    cmake --install "$sdk/out/build/linux-amd64" --config "$config" --prefix "$install"
  done
  [[ -x "$install/bin/rexglue" ]] || { echo "SDK build: $install/bin/rexglue is missing" >&2; exit 1; }
fi
echo 'Setup done. Next: the disc files in port/assets (docs/SETUP.md), then tools/build-linux.sh.'
