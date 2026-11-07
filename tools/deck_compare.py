#!/usr/bin/env python3
"""Compare Steam Deck runs from their frame logs: one column per log, side by side.

    python -I tools/deck_compare.py [--skip-seconds 30] A/grawyeti-frames.N.csv B/grawyeti-frames.M.csv

Each log's sensors-*.csv and threads-*.csv are found beside it, by the time they cover (t_ms is the
same clock in all three), and add the GPU and CPU clocks, power, temperatures and the busiest threads.
A row's gpu_* columns describe the frame before it (docs/PERF.md), so the GPU figures by draws use the
next row's. Logs older than a column show "-" for it. With a level column in the log, it also prints
one summary per level.
"""
import argparse
import bisect
import csv
import glob
import os
import statistics

BANDS = [(0, 300), (300, 1000), (1000, 1500), (1500, 2000), (2000, 2500), (2500, 3000), (3000, 10**9)]


def num(row, column):
    try:
        return float(row[column])
    except (KeyError, TypeError, ValueError):
        return None


def median(values, scale=1.0):
    values = [v for v in values if v is not None]
    return statistics.median(values) * scale if values else None


def show(value, digits=1):
    return "-" if value is None else f"{value:.{digits}f}"


def read(path):
    """The rows of a CSV log with a time; a last row cut short when the game died is left out."""
    with open(path, newline="") as f:
        return [r for r in csv.DictReader(f) if num(r, "t_ms") is not None]


def frames(path):
    """A frame log's rows, those with a frame time."""
    return [r for r in read(path) if num(r, "frame_ms") is not None]


def beside(log, pattern, begin, end):
    """The file matching pattern beside the log that covers most of the log's time, read in full."""
    best, best_overlap = [], 0
    for path in glob.glob(os.path.join(os.path.dirname(log) or ".", pattern)):
        rows = read(path)
        if rows:
            overlap = min(end, num(rows[-1], "t_ms")) - max(begin, num(rows[0], "t_ms"))
            if overlap > best_overlap:
                best, best_overlap = rows, overlap
    return best


def frame_stats(rows):
    times = [num(r, "frame_ms") for r in rows]
    worst = sorted(times)[-max(1, len(times) // 100):]
    return [
        ("frames", f"{len(rows)}"),
        ("minutes", show(sum(times) / 60000)),
        ("fps, average", show(1000 * len(times) / sum(times), 2)),
        ("frame ms, median", show(statistics.median(times))),
        ("frame ms, 99th pct", show(sorted(times)[int(len(times) * 0.99)])),
        ("1% low fps", show(1000 / (sum(worst) / len(worst)))),
        ("over 34 ms %", show(100 * sum(t > 34 for t in times) / len(times), 2)),
        ("over 40 ms %", show(100 * sum(t > 40 for t in times) / len(times), 2)),
        ("over 100 ms, frames", f"{sum(t > 100 for t in times)}"),
        ("draws, median", show(median(num(r, "draws") for r in rows), 0)),
        ("pipelines compiled", show(sum(num(r, "pso_created") or 0 for r in rows), 0)),
        ("pipeline wait, s", show(sum(num(r, "pso_wait_us") or 0 for r in rows) / 1e6)),
        ("all threads CPU ms/frame", show(median(num(r, "process_cpu_ms") for r in rows))),
    ]


def band_stats(rows, lo, hi):
    picked = [i for i in range(len(rows) - 1) if lo <= (num(rows[i], "draws") or -1) < hi]
    if not picked:
        return None
    times = [num(rows[i], "frame_ms") for i in picked]
    return [
        ("frames", f"{len(picked)}"),
        ("over 40 ms %", show(100 * sum(t > 40 for t in times) / len(times), 2)),
        # 0 in a log from before Vulkan's timestamps.
        ("GPU ms", show(median(num(rows[i + 1], "gpu_prev_ms") or None for i in picked))),
        ("GPU idle ms", show(median(num(rows[i + 1], "gpu_gap_prev_ms") for i in picked))),
        ("game thread CPU ms", show(median(num(rows[i], "guest_cpu_ms") for i in picked))),
        ("render thread CPU ms", show(median(num(rows[i], "render_cpu_ms") for i in picked))),
        ("all threads CPU ms", show(median(num(rows[i], "process_cpu_ms") for i in picked))),
        ("renderer us/draw", show(median(num(rows[i], "us_per_draw") for i in picked), 2)),
    ]


def sensor_stats(sensors, begin, end):
    picked = [s for s in sensors if begin <= num(s, "t_ms") <= end]
    return [
        (label, show(median(num(s, column) for s in picked), digits))
        for label, column, digits in [
            ("GPU MHz, median", "gpu_mhz", 0),
            ("CPU MHz, median", "cpu_mhz_avg", 0),
            ("APU watts", "gpu_watts", 1),
            ("CPU C", "cpu_c", 0),
            ("APU C", "apu_c", 0),
            ("fan rpm", "fan_rpm", 0),
        ]
    ]


def thread_table(threads, begin, end, seconds):
    """Share of a core per thread name, summed over the threads sharing that name, busiest first."""
    totals = {}
    for row in threads:
        if begin <= num(row, "t_ms") <= end:
            entry = totals.setdefault(row["name"], [0.0, 0.0, 0.0])
            entry[0] += num(row, "cpu_ms")
            entry[1] += num(row, "sys_ms")
            entry[2] += num(row, "vol")
    return sorted(((name, 100 * v[0] / (seconds * 1000), 100 * v[1] / (seconds * 1000), v[2] / seconds)
                   for name, v in totals.items()), key=lambda t: -t[1])


def table(title, names, columns):
    labels = []
    for column in columns:
        labels += [label for label, _ in column if label not in labels]
    width = max(len(label) for label in labels) + 2
    print(f"\n{title}")
    print(" " * width + "".join(f"{name:>14}" for name in names))
    for label in labels:
        cells = [dict(column).get(label, "-") for column in columns]
        print(f"{label:<{width}}" + "".join(f"{cell:>14}" for cell in cells))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--skip-seconds", type=float, default=30, help="drop each log's start (loading)")
    args = parser.parse_args()

    runs = []
    names = []
    for log in args.logs:
        rows = frames(log)
        if not rows:
            raise SystemExit(f"{log}: no frames")
        start = num(rows[0], "t_ms")
        rows = [r for r in rows if num(r, "t_ms") - start >= args.skip_seconds]
        if not rows:
            raise SystemExit(f"{log}: nothing after the first {args.skip_seconds} s")
        begin, end = num(rows[0], "t_ms"), num(rows[-1], "t_ms")
        runs.append((rows, beside(log, "sensors-*.csv", begin, end),
                     beside(log, "threads-*.csv", begin, end), begin, end))
        names.append(os.path.basename(log).replace("grawyeti-frames", "").strip(".").replace(".csv", "")
                     or "first")
        print(f"{names[-1]}: {log}")

    table("Whole run", names, [frame_stats(r[0]) + sensor_stats(r[1], r[3], r[4]) for r in runs])
    for lo, hi in BANDS:
        columns = [band_stats(r[0], lo, hi) or [] for r in runs]
        if any(columns):
            table(f"{lo}-{hi if hi < 10**9 else ''} draws, medians", names, columns)

    levels = []
    for rows, *_ in runs:
        levels += [l for l in dict.fromkeys(r.get("level") or "" for r in rows) if l and l not in levels]
    for level in levels:
        table(f"Level {level}", names,
              [frame_stats(picked) if picked else [] for picked in
               ([x for x in r[0] if x.get("level") == level] for r in runs)])

    # Sensor seconds by load: is the GPU clock up when the frame is heavy?
    for name, (rows, sensors, *_) in zip(names, runs):
        if not sensors:
            continue
        times = [num(r, "t_ms") for r in rows]
        by_band = {}
        for s in sensors:
            t = num(s, "t_ms")
            a, b = bisect.bisect_left(times, t - 1000), bisect.bisect_left(times, t)
            if b - a >= 10:
                draws = statistics.median(num(r, "draws") for r in rows[a:b])
                key = next(band for band in BANDS if band[0] <= draws < band[1])
                by_band.setdefault(key, []).append(s)
        print(f"\nGPU clock by load, {name}: draws, seconds, median MHz, 90th pct MHz, watts")
        for key in sorted(by_band):
            mhz = sorted(num(s, "gpu_mhz") for s in by_band[key])
            print(f"  {key[0]:>5}+ {len(mhz):>6} {statistics.median(mhz):>6.0f} {mhz[int(len(mhz) * 0.9)]:>6.0f} "
                  f"{median(num(s, 'gpu_watts') for s in by_band[key]):>6.1f}")

    for name, (_, _, threads, begin, end) in zip(names, runs):
        if threads:
            print(f"\nThreads, {name}: name, % of a core, of it in the kernel %, voluntary switches/s")
            for thread, cpu, kernel, switches in thread_table(threads, begin, end, (end - begin) / 1000)[:16]:
                print(f"  {thread:<20} {cpu:>7.1f} {kernel:>7.1f} {switches:>9.0f}")


if __name__ == "__main__":
    main()
