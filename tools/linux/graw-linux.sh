#!/bin/bash
# GRAW 360 recomp - native Linux and Steam Deck: the three programs. The campaign (grawyeti) draws
# with its native Vulkan renderer (settings in grawyeti.toml); the front end and Instant Action with
# ReXGlue's emulated renderer.
# Starts the front end (graw), which hands over to the campaign (grawyeti) and Instant Action
# (grawmp) and back, as on Windows. GRAW_PROGRAM=grawyeti starts the campaign on its own.
# Reads the disc files from ../game (the graw360 folder this sits in); GRAW_GAME=/path overrides.
# Saves, caches and logs stay in this folder (user/, cache/, logs/), apart from the Windows copy's.
# Extra arguments go to the game, e.g.  ./graw-linux.sh --no-fullscreen
here="$(cd "$(dirname "$0")" && pwd)"
game="$(cd "${GRAW_GAME:-$here/../game}" 2>/dev/null && pwd)"
program="${GRAW_PROGRAM:-graw}"
mkdir -p "$here/user" "$here/cache" "$here/logs"
# The programs a hand-over starts run in this folder too (grawyeti.toml's relative paths).
cd "$here"
stamp="$(date +%Y%m%d-%H%M%S)"
# Each program a hand-over starts writes its own log next to this one (<name>~<program>-<time>).
log="$here/logs/$program-$stamp.log"
info="$here/logs/system-$stamp.txt"

# What the logs need to explain a failed start.
{
  echo "date: $(date)"
  grep -E '^(PRETTY_NAME|VERSION_ID|BUILD_ID)=' /etc/os-release 2>/dev/null
  echo "kernel: $(uname -r)"
  echo "glibc: $(ldd --version 2>/dev/null | head -1)"
  libstdcxx="$(ldconfig -p 2>/dev/null | grep -m1 'libstdc++.so.6 ' | sed 's/.*=> //')"
  [ -z "$libstdcxx" ] && libstdcxx=/usr/lib/libstdc++.so.6
  if grep -aq GLIBCXX_3.4.32 "$libstdcxx" 2>/dev/null; then
    echo "libstdc++: $libstdcxx has GLIBCXX_3.4.32 (needed)"
  else
    echo "libstdc++: $libstdcxx LACKS GLIBCXX_3.4.32 (needed): the game can't start"
  fi
  echo "display: DISPLAY=$DISPLAY WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
  echo "game: $game"
  echo "program: $program"
  echo "--- ldd $program"
  ldd "$here/$program" 2>&1
} > "$info"

# The programs are translations of these exact executables: any other disc's won't run on them.
disc_ok=1
while read -r sum name; do
  if [ -z "$game" ] || [ ! -f "$game/$name" ]; then
    echo "$name is missing from ${GRAW_GAME:-$here/../game}: unpack your disc there (README.txt)," \
         "or set GRAW_GAME" | tee -a "$info" >&2
    disc_ok=0
  elif ! echo "$sum  $game/$name" | sha256sum --quiet -c - >/dev/null 2>&1; then
    echo "$game/$name isn't the supported version (USA/Europe disc, title 555307D5)" \
      | tee -a "$info" >&2
    disc_ok=0
  fi
done <<'EOF'
295ac4ded72c95bb86e9d9441828bb5b193f863afb5a20a993275e01144cdb83 default.xex
32735d0908bd45e965d427de6d1be2f995cb9ef44f6633b5ee7df5f34dbaadfb Yeti_Final_Xenon.exe
4d8a0abb93f3867df53e229a54f55435a57b403f65781d5737237ff8c36c023a GR3Xenon_Release.exe
EOF
[ "$disc_ok" = 1 ] || exit 1

# Once a second, the APU's clocks, power and temperatures next to the frame log. It ends itself
# after the last program; the game doesn't depend on it.
python3 -I "$here/deck-sensors.py" "$here/logs/sensors-$stamp.csv" >/dev/null 2>&1 &

# The Steam Deck's settings: the 360's 1280x720 output and 30 Hz display.
"$here/$program" --fullscreen --resolution 720p --resolution_scale 1 \
  --video_mode_refresh_rate 30 --anisotropic_override -1 --swap_post_effect none \
  --game_data_root "$game" --user_data_root "$here/user" --cache_root "$here/cache" \
  --log_file "$log" --log_flush_interval 1 --log_level debug "$@" \
  >> "$here/logs/stdout-$stamp.txt" 2>&1
status=$?
# The first program's: after a hand-over, the next one keeps running on its own.
echo "exit status ($program): $status" >> "$info"
exit $status
