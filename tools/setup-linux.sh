#!/usr/bin/env bash
# Set up what tools/build-linux.sh needs that isn't in the repository, on Linux x86-64 (#38; the
# Linux counterpart of tools/setup.ps1):
#   1. third_party/rexglue-sdk-bd833a2: ReXGlue's source at the nightly 0.10.0.24-dev.gbd833a2's
#      commit with this project's patches (tools/patches), checked against the same patched tree
#      as on Windows, then the Linux-only ones on top (fixes in the SDK's runtime: on Windows the
#      game links the prebuilt SDK's runtime, so they can't apply there);
#   2. that SDK built and installed into its out/install/linux-amd64, RelWithDebInfo and Release:
#      the libraries the game links, the patched renderer plugin (resolves into freed memory,
#      vblank pacing, gpu_skip_draws) and the patched code generator (vpkd3d128,
#      docs/TACMAP.md), which the SDK installs from Release only.
# Needs clang (20 or later), CMake 3.25+, Ninja, git, python3 and the SDK's system libraries
# (docs/SETUP.md, "Linux"). The SDK build takes a while the first time (FFmpeg among others).
#   tools/setup-linux.sh             # all of it (skips what is there already)
#   tools/setup-linux.sh --force     # replace an existing SDK source checkout and rebuild
#   tools/setup-linux.sh --jobs 12
# GRAW_SDK_DIR puts the SDK checkout elsewhere (tools/build-deck.sh: the SDK writes its binaries
# into its own tree, so a second build environment needs a second checkout).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${GRAW_SDK_DIR:-$root/third_party/rexglue-sdk-bd833a2}"
sdk_commit=bd833a2ab69d305f99ddf57418fc38ffec8af93d
# As tools/setup.ps1, in order.
sdk_patches=(rexglue-bd833a2-vpkd3d128-alias rexglue-bd833a2-freed-page-resolve
             rexglue-bd833a2-vblank-pacing rexglue-bd833a2-gpu-skip-draws
             pgr4-native/0021-gpu-sample-computed-LOD-pixel-shader-fetches-with-sa
             pgr4-native/0022-gpu-add-a-pixel-shader-modification-for-textures-kno
             rexglue-bd833a2-implicit-gradients-switch rexglue-bd833a2-spirv-exp-adjust
             rexglue-bd833a2-spirv-implicit-gradients)
expected_tree=54ab5932e76ba1b8c9e454891c9ba52c16bfdda0  # after the last patch
# Then, in their own commit: XamLoaderLaunchTitle kept a string_view to a freed temporary (the
# requested program's path came out with glibc's free-list pointers in it). A fault no handler
# claims crashes with a host backtrace instead of re-faulting forever, and RenderDoc is used only
# when it's already in the process (SteamOS installs librenderdoc.so, so a plain dlopen loaded it
# into every run, #51).
linux_patches=(rexglue-bd833a2-launch-title-path rexglue-bd833a2-linux-crash-renderdoc)
linux_tree=88fa3e8437490f8c7c0628370326eaa4561c4788
# Checkouts made by an earlier version of this script move forward by the patches added since,
# rather than a new clone and full SDK build: "<tree> <patch>..." pairs, each tree -> the next.
upgrades=("f9d5a19f96e3f2f7ed79ee5b08155eab343a5b9c rexglue-bd833a2-spirv-exp-adjust"
          "2fd253876480df2f8bf35e394a8163c2ba93c65c rexglue-bd833a2-spirv-implicit-gradients"
          "ecaf46d0c9f324b0a43468f20edda3a6e787ab78 rexglue-bd833a2-spirv-implicit-gradients"
          "379fa50a65a1bbf7ec9e525f0316489759223e00 rexglue-bd833a2-linux-crash-renderdoc")
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
  for upgrade in "${upgrades[@]}"; do
    read -r from patches <<< "$upgrade"
    if [[ "$head" == "$from" && -z "$changes" ]]; then
      echo "SDK source: adding patches since the last setup: $patches"
      for name in $patches; do
        sed 's/\r$//' "$root/tools/patches/$name.patch" | git -C "$sdk" apply -
      done
      git -C "$sdk" add -A
      git -C "$sdk" -c user.name=graw360-recomp -c user.email=setup@localhost commit --quiet \
          -m "graw360-recomp patches: $patches"
      head=$(git -C "$sdk" rev-parse 'HEAD^{tree}')
    fi
  done
  if [[ ( "$head" != "$expected_tree" && "$head" != "$linux_tree" ) || -n "$changes" ]]; then
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
# The Linux-only patches (a checkout made before they existed gets them here).
if [[ "$(git -C "$sdk" rev-parse 'HEAD^{tree}')" == "$expected_tree" ]]; then
  echo "SDK source: applying ${#linux_patches[@]} Linux patch(es)"
  for name in "${linux_patches[@]}"; do
    sed 's/\r$//' "$root/tools/patches/$name.patch" | git -C "$sdk" apply -
  done
  git -C "$sdk" add -A
  tree=$(git -C "$sdk" write-tree)
  if [[ "$tree" != "$linux_tree" ]]; then
    echo "The SDK tree with the Linux patches is $tree, not $linux_tree" >&2
    exit 1
  fi
  git -C "$sdk" -c user.name=graw360-recomp -c user.email=setup@localhost commit --quiet \
      -m 'graw360-recomp Linux patches'
fi
# Every run, so one that stopped while fetching them picks up where it left off.
echo 'SDK source: fetching its submodules'
git -C "$sdk" submodule update --init --recursive

# 2. Build and install the SDK.
install="$sdk/out/install/linux-amd64"
# RelWithDebInfo libraries carry the SDK's "rd" postfix (librexgpu-xenosrd.so); the code generator
# is Release's bin/rexglue.
# The install records the source tree it was built from, so a new patch rebuilds it.
stamp="$install/graw360-sdk-tree"
if [[ -x "$install/bin/rexglue" && -f "$install/lib/librexruntimerd.so" && $force -eq 0 &&
      "$(cat "$stamp" 2>/dev/null)" == "$linux_tree" ]]; then
  echo "SDK: built ($install)"
else
  (cd "$sdk" && cmake --preset linux-amd64 -DREXGLUE_BUILD_TESTS=OFF)
  for config in RelWithDebInfo Release; do
    echo "SDK: building $config ($jobs jobs)"
    cmake --build "$sdk/out/build/linux-amd64" --config "$config" --parallel "$jobs"
    cmake --install "$sdk/out/build/linux-amd64" --config "$config" --prefix "$install"
  done
  [[ -x "$install/bin/rexglue" ]] || { echo "SDK build: $install/bin/rexglue is missing" >&2; exit 1; }
  echo "$linux_tree" > "$stamp"
fi
echo 'Setup done. Next: the disc files in port/assets (docs/SETUP.md), then tools/build-linux.sh.'
