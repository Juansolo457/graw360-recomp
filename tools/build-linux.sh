#!/usr/bin/env bash
# Build a GRAW program on Linux x86-64 (#38; the Linux counterpart of tools/build.ps1) against the
# SDK tools/setup-linux.sh built. Codegen (translating the disc's executable to C++) runs as part
# of the build whenever the manifest or the executable changed; the first run takes a few minutes.
# The campaign gets the native renderer's Vulkan backend (docs/VULKAN_RENDERER.md), compiled
# against the same SDK checkout; the front end and Instant Action run ReXGlue's emulated renderer.
#   tools/build-linux.sh                         # the campaign (port-yeti -> grawyeti)
#   tools/build-linux.sh --project port --jobs 12
#   tools/build-linux.sh --config Release
# GRAW_SDK_DIR: the SDK checkout tools/setup-linux.sh used; GRAW_BUILD_DIR: the build folder
# (default out/build/<preset> in the project). tools/build-deck.sh sets both.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project=port-yeti
config=RelWithDebInfo
jobs=$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))
while [[ $# -gt 0 ]]; do
  case "$1" in
    --project) project="$2"; shift ;;
    --config) config="$2"; shift ;;
    --jobs) jobs="$2"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
# Each project: its executable on the disc, that file's SHA-256, codegen target, output folder.
case "$project" in
  port) xex=default.xex sha=295ac4ded72c95bb86e9d9441828bb5b193f863afb5a20a993275e01144cdb83
        name=graw out=default ;;
  port-mp) xex=GR3Xenon_Release.exe sha=4d8a0abb93f3867df53e229a54f55435a57b403f65781d5737237ff8c36c023a
           name=grawmp out=gr3xenon_release ;;
  port-yeti) xex=Yeti_Final_Xenon.exe sha=32735d0908bd45e965d427de6d1be2f995cb9ef44f6633b5ee7df5f34dbaadfb
             name=grawyeti out=yeti_final_xenon ;;
  *) echo "unknown project $project (port, port-mp, port-yeti)" >&2; exit 2 ;;
esac
case "$config" in
  Debug|Release|RelWithDebInfo) ;;
  *) echo "unknown configuration $config" >&2; exit 2 ;;
esac

port="$root/$project"
sdk_src="${GRAW_SDK_DIR:-$root/third_party/rexglue-sdk-bd833a2}"
sdk_install="$sdk_src/out/install/linux-amd64"
[[ -x "$sdk_install/bin/rexglue" ]] || { echo "The SDK isn't built: run tools/setup-linux.sh" >&2; exit 1; }
[[ -f "$port/assets/$xex" ]] || { echo "$project/assets/$xex is missing (docs/SETUP.md)" >&2; exit 1; }
if [[ "$(sha256sum "$port/assets/$xex" | cut -d' ' -f1)" != "$sha" ]]; then
  echo "$project/assets/$xex is not the supported GRAW executable (USA/Europe, title 555307D5)" >&2
  exit 1
fi

preset="linux-amd64-${config,,}"
# x86-64-v3 as on Windows (tools/build.ps1, #11); the Steam Deck has it. The presets name
# clang-20; any clang 20 or later works.
build="${GRAW_BUILD_DIR:-$port/out/build/$preset}"
args=(--preset "$preset" -B "$build" -DCMAKE_PREFIX_PATH="$sdk_install"
      -DGRAW_REXGLUE_SDK_SRC="$sdk_src"
      -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
      -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3)
cd "$port"
cmake "${args[@]}"
first_time=0
[[ -f "generated/$out/sources.cmake" ]] || first_time=1
# Codegen first (a no-op when nothing changed), then reconfigure once if the generated sources
# didn't exist yet (the recomp target only appears once they do), then build.
cmake --build "$build" --parallel "$jobs" --target "${name}_codegen"
# The installed code generator is the patched one (vpkd3d128, docs/TACMAP.md); the check keeps a
# stock generator from bringing the bug back unnoticed.
python3 -I "$root/tools/check_vector_aliasing.py" "generated/$out" >/dev/null || {
  echo "The generated code has the vpkd3d128 aliasing bug: is the SDK patched? (docs/TACMAP.md)" >&2
  exit 1
}
if [[ $first_time -eq 1 ]]; then cmake "${args[@]}"; fi
cmake --build "$build" --parallel "$jobs"
# The program finds its shared libraries beside itself ($ORIGIN): the SDK stages the renderer
# plugin there, but its runtime and Tracy libraries only on Windows and macOS.
postfix=$(case "$config" in Debug) echo d ;; RelWithDebInfo) echo rd ;; *) echo ;; esac)
for lib in librexruntime libTracyClient; do
  cp -u "$sdk_install/lib/$lib$postfix.so" "$build/"
done
echo "Built $build/$name"
