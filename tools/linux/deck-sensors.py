#!/usr/bin/env python3
# GRAW 360 recomp - Linux and Steam Deck: once a second, the APU's clocks, power and temperatures,
# next to the frame log, and each busy thread's CPU time in a second file beside it. t_ms is the same
# clock as the frame log's t_ms (CLOCK_MONOTONIC). Runs while a GRAW program does, and ends 5 s after
# the last one. A sensor this machine doesn't have reads 0.
#   deck-sensors.py <path>/sensors-<time>.csv     also writes <path>/threads-<time>.csv
#
# threads-<time>.csv has one row per thread per second, for threads that ran at least 1 ms of it:
# cpu_ms on a core, runq_ms ready but waiting for a core, user_ms and sys_ms from the kernel's coarser
# 10 ms ticks, and vol and invol, its voluntary and forced context switches. A spinning thread shows
# high cpu_ms and few switches; one that yields or sleeps very briefly shows thousands of vol.
import glob
import os
import sys
import time

GAMES = {"graw", "grawmp", "grawyeti"}
TICK_MS = 1000 / os.sysconf("SC_CLK_TCK")


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


def game_pids():
    pids = []
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                with open(f"/proc/{pid}/comm") as f:
                    if f.read().strip() in GAMES:
                        pids.append(pid)
            except OSError:
                pass
    return pids


def thread_sample(pid, tid):
    """(name, cpu_ms, runq_ms, user_ms, sys_ms, vol, invol) so far, or None if the thread ended."""
    task = f"/proc/{pid}/task/{tid}"
    try:
        with open(f"{task}/schedstat") as f:
            cpu_ns, runq_ns = f.read().split()[:2]
        with open(f"{task}/comm") as f:
            name = f.read().strip()
        with open(f"{task}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()  # the name may hold spaces
        vol = invol = 0
        with open(f"{task}/status") as f:
            for line in f:
                if line.startswith("voluntary_ctxt_switches"):
                    vol = int(line.split()[1])
                elif line.startswith("nonvoluntary_ctxt_switches"):
                    invol = int(line.split()[1])
    except (OSError, ValueError, IndexError):
        return None
    return (name, int(cpu_ns) / 1e6, int(runq_ns) / 1e6, int(fields[11]) * TICK_MS,
            int(fields[12]) * TICK_MS, vol, invol)


def write_threads(out, pids, last, now_ms):
    """One row for each thread busy since the last sample. A thread's first sample writes nothing."""
    seen = set()
    for pid in pids:
        try:
            tids = os.listdir(f"/proc/{pid}/task")
        except OSError:
            continue
        for tid in tids:
            sample = thread_sample(pid, tid)
            if sample is None:
                continue
            key = (pid, tid)
            seen.add(key)
            before = last.get(key)
            last[key] = sample
            if before and sample[1] - before[1] >= 1:
                d = [a - b for a, b in zip(sample[1:], before[1:])]
                out.write(f"{now_ms:.0f},{pid},{tid},{sample[0].replace(',', ' ')},"
                          f"{d[0]:.1f},{d[1]:.1f},{d[2]:.0f},{d[3]:.0f},{d[4]},{d[5]}\n")
    for key in set(last) - seen:
        del last[key]


def main():
    gpu = hwmon("amdgpu")
    cpu = hwmon("k10temp")
    fan = hwmon("steamdeck_hwmon")
    cores = glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq")
    folder, name = os.path.split(sys.argv[1])
    threads_path = os.path.join(folder, name.replace("sensors", "threads", 1))
    with open(sys.argv[1], "w", buffering=1) as out, open(threads_path, "w", buffering=1) as threads:
        out.write("t_ms,gpu_mhz,cpu_mhz_avg,cpu_mhz_max,gpu_watts,gpu_cap_watts,apu_c,cpu_c,fan_rpm\n")
        threads.write("t_ms,pid,tid,name,cpu_ms,runq_ms,user_ms,sys_ms,vol,invol\n")
        last_seen = time.monotonic()
        last_threads = {}
        while time.monotonic() - last_seen < 5:
            mhz = [read_int(c) / 1000 for c in cores] or [0]
            now_ms = time.monotonic() * 1000
            out.write(
                f"{now_ms:.0f},"
                f"{read_int(f'{gpu}/freq1_input') / 1e6:.0f},"
                f"{sum(mhz) / len(mhz):.0f},{max(mhz):.0f},"
                f"{read_int(f'{gpu}/power1_average') / 1e6:.2f},"
                f"{read_int(f'{gpu}/power1_cap') / 1e6:.1f},"
                f"{read_int(f'{gpu}/temp1_input') / 1000:.1f},"
                f"{read_int(f'{cpu}/temp1_input') / 1000:.1f},"
                f"{read_int(f'{fan}/fan1_input')}\n"
            )
            pids = game_pids()
            write_threads(threads, pids, last_threads, now_ms)
            time.sleep(1)
            if pids:
                last_seen = time.monotonic()


main()
