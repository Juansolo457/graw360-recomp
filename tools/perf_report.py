"""Summarise a per-frame CSV from --perf_log_csv (tools/launch.ps1 -PerfLog), for #11.

    python -I tools/perf_report.py port/logs/perf-<time>~grawyeti-<time>.csv [--skip 10]

Prints frame-time percentiles and FPS, a histogram of frame times, and the other counters'
medians. Frame times bunched at 16.7 or 33.3 ms mean the game paces itself on the guest's vblank
(raise it with -RefreshRate); spread-out times mean it's simply that slow (CPU or GPU bound).
--skip drops the first N seconds (loading, shader compiles).
"""
import csv
import statistics
import sys


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


def main():
    path = sys.argv[1]
    skip = float(sys.argv[sys.argv.index('--skip') + 1]) if '--skip' in sys.argv else 10.0
    rows = list(csv.DictReader(open(path, newline='')))
    if not rows:
        raise SystemExit('no frames in ' + path)
    frames, elapsed = [], 0.0
    for r in rows:
        t = int(r.get('frame_time_us') or 0)
        elapsed += t / 1e6
        if elapsed >= skip and t > 0:
            frames.append(r)
    times = [int(r['frame_time_us']) / 1000.0 for r in frames]
    if not times:
        raise SystemExit(f'only {elapsed:.1f} s recorded; nothing after --skip {skip}')
    print(f'{path}: {len(rows)} frames, {elapsed:.1f} s; analysing {len(times)} after {skip:.0f} s')
    print(f'frame time ms: median {statistics.median(times):.2f}  p90 {pct(times, 90):.2f}  '
          f'p99 {pct(times, 99):.2f}  max {max(times):.2f}')
    print(f'fps: median {1000 / statistics.median(times):.1f}  average {len(times) / (sum(times) / 1000):.1f}  '
          f'1% low {1000 / pct(times, 99):.1f}')
    edges = [0, 8.0, 12.0, 15.5, 17.5, 25, 31.5, 35.5, 50, 1e9]
    labels = ['<8', '8-12', '12-15.5', '15.5-17.5 (60 Hz)', '17.5-25', '25-31.5', '31.5-35.5 (30 Hz)',
              '35.5-50', '>50']
    print('histogram:')
    for lo, hi, label in zip(edges, edges[1:], labels):
        n = sum(lo <= t < hi for t in times)
        if n:
            print(f'  {label:>20} ms  {n:6}  {100 * n / len(times):5.1f}%  ' + '#' * int(40 * n / len(times)))
    for column in rows[0].keys():
        if column in ('frame_time_us',):
            continue
        values = [int(r[column] or 0) for r in frames]
        if any(values):
            print(f'{column:>28}: median {statistics.median(values):.0f}  max {max(values)}')


if __name__ == '__main__':
    main()
