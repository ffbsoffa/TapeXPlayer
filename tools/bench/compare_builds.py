#!/usr/bin/env python3
"""Runs txp_bench.py phases for several player builds on one clip and prints the CPU of each
phase side by side, so a change is judged by numbers from the same machine, clip and session.

  compare_builds.py CLIP before=path/to/TapeXPlayer after=path/to/TapeXPlayer
                    [--betacam on off] [--repeats 2] [--home DIR]

Each build/Betacam setting runs --repeats times, alternating builds so slow drift (thermals,
background load) hits all of them alike. Prints per phase: mean CPU %, mean new pictures/s and
the worst picture gap over the repeats, then the per-thread split of the last run.
All builds share one HOME (default ~/.cache/txp-bench-home), so a clip's proxy is built once.
"""
import argparse
import os
import re
import subprocess
import sys

BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "txp_bench.py")
PHASE = re.compile(r"^  (pause after|pause|play 1x|slow 0\.5x|shuttle 8x|shuttle 24x)\s+CPU\s+(\d+)% \| "
                   r"ui\s+([\d.]+)/s new\s+([\d.]+)/s gap\s+(\d+)ms")
ORDER = ["pause", "play 1x", "slow 0.5x", "shuttle 8x", "shuttle 24x", "pause after"]


def run(binary, clip, betacam, home):
    cmd = [sys.executable, BENCH, "phases", clip, "--binary", binary, "--betacam", betacam, "--home", home]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    result, threads, current = {}, {}, None
    for line in out.splitlines():
        m = PHASE.match(line)
        if m:
            current = m.group(1)
            result[current] = (int(m.group(2)), float(m.group(4)), int(m.group(5)))
        elif current and line.strip().startswith("threads:"):
            threads[current] = line.strip()[len("threads:"):].strip()
    if not result:
        print(out, file=sys.stderr)
    return result, threads


def main():
    p = argparse.ArgumentParser()
    p.add_argument("clip")
    p.add_argument("builds", nargs="+", help="label=binary")
    p.add_argument("--betacam", nargs="+", default=["on", "off"])
    p.add_argument("--repeats", type=int, default=2)
    p.add_argument("--home", default=os.path.expanduser("~/.cache/txp-bench-home"))
    args = p.parse_args()
    builds = [b.split("=", 1) for b in args.builds]

    for betacam in args.betacam:
        runs = {label: [] for label, _ in builds}
        last_threads = {}
        for r in range(args.repeats):
            for label, binary in (builds if r % 2 == 0 else list(reversed(builds))):
                result, threads = run(binary, args.clip, betacam, args.home)
                runs[label].append(result)
                last_threads[label] = threads
                print(f"  .. betacam {betacam} run {r + 1} {label} done", file=sys.stderr, flush=True)
        print(f"\n=== {os.path.basename(args.clip)} — Betacam {betacam}, {args.repeats} runs each "
              f"(CPU % of one core · new pictures/s · worst gap ms)")
        print(f"  {'phase':<12}" + "".join(f"{label:>28}" for label, _ in builds))
        for phase in ORDER:
            cells = []
            for label, _ in builds:
                vals = [r[phase] for r in runs[label] if phase in r]
                if not vals:
                    cells.append(f"{'—':>28}")
                    continue
                cpu = sum(v[0] for v in vals) / len(vals)
                new = sum(v[1] for v in vals) / len(vals)
                gap = max(v[2] for v in vals)
                cells.append(f"{cpu:>9.0f}% {new:>6.1f}/s {gap:>6d}ms")
            print(f"  {phase:<12}" + "".join(cells))
        for label, _ in builds:
            print(f"  threads, {label} (last run):")
            for phase in ORDER:
                if phase in last_threads.get(label, {}):
                    print(f"    {phase:<12} {last_threads[label][phase]}")


if __name__ == "__main__":
    main()
