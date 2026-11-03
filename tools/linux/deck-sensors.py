#!/usr/bin/env python3
# GRAW 360 recomp - Linux and Steam Deck: once a second, the APU's clocks, power and temperatures,
# next to the frame log. t_ms is the same clock as the frame log's t_ms (CLOCK_MONOTONIC). Runs
# while a GRAW program does, and ends 5 s after the last one. A sensor this machine doesn't have
# reads 0.
#   deck-sensors.py <output.csv>
import glob
import os
import sys
import time

GAMES = {"graw", "grawmp", "grawyeti"}


def hwmon(name):
    for path in glob.glob("/sys/class/hwmon/hwmon*"):
        try:
            with open(os.path.join(path, "name")) as f:
                if f.read().strip() == name:
                    return path
        except OSError:
            pass
    return None


def read_int(path):
    try:
        with open(path) as f:
            return int(f.read())
    except (OSError, ValueError, TypeError):
        return 0


def game_running():
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                with open(f"/proc/{pid}/comm") as f:
                    if f.read().strip() in GAMES:
                        return True
            except OSError:
                pass
    return False


def main():
    gpu = hwmon("amdgpu")
    cpu = hwmon("k10temp")
    fan = hwmon("steamdeck_hwmon")
    cores = glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq")
    with open(sys.argv[1], "w", buffering=1) as out:
        out.write("t_ms,gpu_mhz,cpu_mhz_avg,cpu_mhz_max,gpu_watts,gpu_cap_watts,apu_c,cpu_c,fan_rpm\n")
        last_seen = time.monotonic()
        while time.monotonic() - last_seen < 5:
            mhz = [read_int(c) / 1000 for c in cores] or [0]
            out.write(
                f"{time.monotonic() * 1000:.0f},"
                f"{read_int(f'{gpu}/freq1_input') / 1e6:.0f},"
                f"{sum(mhz) / len(mhz):.0f},{max(mhz):.0f},"
                f"{read_int(f'{gpu}/power1_average') / 1e6:.2f},"
                f"{read_int(f'{gpu}/power1_cap') / 1e6:.1f},"
                f"{read_int(f'{gpu}/temp1_input') / 1000:.1f},"
                f"{read_int(f'{cpu}/temp1_input') / 1000:.1f},"
                f"{read_int(f'{fan}/fan1_input')}\n"
            )
            time.sleep(1)
            if game_running():
                last_seen = time.monotonic()


main()
