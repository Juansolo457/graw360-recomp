#!/usr/bin/env bash
# Build a GRAW program for the Steam Deck (#38): tools/setup-linux.sh and tools/build-linux.sh
# inside the Ubuntu 22.04 container of tools/linux/deck.Containerfile (podman), so the programs
# need glibc 2.35 at most; a build made on the host needs the host's (2.43 on Ubuntu 26.04), which
# SteamOS doesn't have. The container's SDK checkout and build folders are its own
# (third_party/rexglue-sdk-bd833a2-deck, <project>/out/build/deck-amd64-<config>), so host builds
# in the same tree are left alone. Then every binary is checked for its glibc requirement.
#   tools/build-deck.sh                      # the campaign
#   tools/build-deck.sh --project port --jobs 12
#   tools/build-deck.sh --rebuild-image      # rebuild the container image first
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image=graw360-deck-build
glibc_max=2.35
project=port-yeti
config=RelWithDebInfo
jobs=$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))
rebuild=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --project) project="$2"; shift ;;
    --config) config="$2"; shift ;;
    --jobs) jobs="$2"; shift ;;
    --rebuild-image) rebuild=1 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
command -v podman >/dev/null || { echo "podman is not installed (docs/SETUP.md, Linux)" >&2; exit 1; }

if [[ $rebuild -eq 1 ]] || ! podman image exists "$image"; then
  podman build -t "$image" -f "$root/tools/linux/deck.Containerfile" "$root/tools/linux"
fi

sdk=/src/third_party/rexglue-sdk-bd833a2-deck
build_rel="$project/out/build/deck-amd64-${config,,}"
# The disc files: a project's assets may be a link out of the tree (port-yeti/assets ->
# port/assets); the link's target is mounted read-only at the same path so it resolves.
mounts=(-v "$root:/src")
for assets in "$root"/port*/assets; do
  [[ -e "$assets" ]] || continue
  real="$(readlink -f "$assets")"
  case "$real" in "$root"/*) continue ;; esac
  # Once each: the projects' links usually share one folder.
  [[ " ${mounts[*]} " == *" $real:$real:ro "* ]] || mounts+=(-v "$real:$real:ro")
done
podman run --rm --userns=keep-id "${mounts[@]}" -w /src -e HOME=/tmp \
  -e GRAW_SDK_DIR="$sdk" -e GRAW_BUILD_DIR="/src/$build_rel" "$image" \
  bash -c "tools/setup-linux.sh --jobs $jobs && tools/build-linux.sh --project $project --config $config --jobs $jobs"

# The newest glibc symbol version each binary needs, and libstdc++'s for the record (libstdc++ 13
# here: GLIBCXX_3.4.32 at most, which a Deck's libstdc++ has to provide).
fail=0
for f in "$root/$build_rel"/*; do
  [[ -f "$f" && ( -x "$f" || "$f" == *.so ) ]] || continue
  file -b "$f" | grep -q '^ELF' || continue
  need=$(objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
  cxx=$(objdump -T "$f" | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1)
  if [[ -n "$need" && "$(printf '%s\n%s\n' "$need" "$glibc_max" | sort -V | tail -1)" != "$glibc_max" ]]; then
    echo "$(basename "$f"): needs glibc $need, above $glibc_max" >&2
    fail=1
  else
    echo "$(basename "$f"): glibc ${need:-none}${cxx:+, $cxx}"
  fi
done
[[ $fail -eq 0 ]] || exit 1
echo "Deck build: $build_rel"
