#!/usr/bin/env bash
# Merge a PGO training run's counts into the profile the next build uses (from fear360-recomp): the .profraw
# files a GRAW_PGO=generate build wrote (pgo-raw/ in each game's folder, src/pgo.cpp) into
# pgo/<project>/default.profdata, which tools/build-linux.sh then uses by default.
#   tools/pgo-merge.sh --for deck --project port-yeti  <folder or .profraw files>...
#   tools/pgo-merge.sh --for deck --project port       <folder or .profraw files>...
# llvm-profdata must be the version of the compiler that builds with the profile, so the builds
# it's for are named:
#   --for deck   tools/build-deck.sh's: the container's llvm-profdata-20 (the image must exist:
#                tools/build-deck.sh builds it)
#   --for host   tools/build-linux.sh on this host: clang's own llvm-profdata-<major>
#   GRAW_PGO_PROFDATA=<tool> instead of --for: that tool
# The script says which it used.
# Needs python3 (tools/pgo_prepare.py).
# pgo/ stays out of git: the counts come from runs of the game's code.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image=graw360-deck-build
project=""
target=""
inputs=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --project) project="$2"; shift ;;
    --for) target="$2"; shift ;;
    *) inputs+=("$1") ;;
  esac
  shift
done
case "$project" in
  port|port-mp|port-yeti) ;;
  *) echo "--project port, port-mp or port-yeti" >&2; exit 2 ;;
esac
if [[ -z "${GRAW_PGO_PROFDATA:-}" ]]; then
  case "$target" in
    deck|host) ;;
    *) echo "--for deck or --for host (the builds the profile is for), or GRAW_PGO_PROFDATA" >&2
       exit 2 ;;
  esac
fi
files=()
for i in "${inputs[@]}"; do
  if [[ -d "$i" ]]; then
    for f in "$i"/*.profraw; do [[ -f "$f" ]] && files+=("$(readlink -f "$f")"); done
  elif [[ -f "$i" ]]; then
    files+=("$(readlink -f "$i")")
  fi
done
[[ ${#files[@]} -gt 0 ]] || { echo "no .profraw files in ${inputs[*]:-(nothing given)}" >&2; exit 1; }

out_dir="$root/pgo/$project"
mkdir -p "$out_dir"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Each session's total (the last snapshot of a file older training builds appended to), merged,
# then without the functions the training never reached (tools/pgo_prepare.py has why).
python3 -I "$root/tools/pgo_prepare.py" last-snapshot "$work/raw" "${files[@]}"
if [[ -n "${GRAW_PGO_PROFDATA:-}" ]]; then
  tool=$(command -v "$GRAW_PGO_PROFDATA") || { echo "$GRAW_PGO_PROFDATA not found" >&2; exit 1; }
  echo "llvm-profdata: $tool (GRAW_PGO_PROFDATA)"
  profdata() { "$tool" "$@"; }
elif [[ "$target" == deck ]]; then
  command -v podman >/dev/null && podman image exists "$image" || {
    echo "the Deck build's image ($image) is missing: tools/build-deck.sh builds it" >&2; exit 1; }
  echo "llvm-profdata: the Deck build's container (llvm-profdata-20), for Deck builds"
  profdata() {
    podman run --rm --userns=keep-id -v "$work:$work" -v "$out_dir:$out_dir" "$image" \
      llvm-profdata-20 "$@"
  }
else
  llvm=$("${CC:-clang}" -dumpversion | cut -d. -f1)
  # clang's own: llvm-profdata-<major>, or a plain llvm-profdata of that major version.
  tool=$(command -v "llvm-profdata-$llvm") || {
    tool=$(command -v llvm-profdata) &&
      [[ "$("$tool" --version | sed -nE 's/.*LLVM version ([0-9]+).*/\1/p' | head -1)" == "$llvm" ]]
  } || {
    echo "no llvm-profdata of clang $llvm's version: set GRAW_PGO_PROFDATA" >&2; exit 1; }
  echo "llvm-profdata: $tool, for builds on this host (clang $llvm)"
  profdata() { "$tool" "$@"; }
fi
profdata merge --text -o "$work/all.proftext" "$work"/raw/*.profraw
python3 -I "$root/tools/pgo_prepare.py" drop-unreached "$work/all.proftext" "$work/reached.proftext"
profdata merge -o "$out_dir/default.profdata" "$work/reached.proftext"
echo "Merged ${#files[@]} run(s) into pgo/$project/default.profdata"
