#!/usr/bin/env python3
"""How long the player takes to open a clip: launches it on the clip and reports, from the
moment of launch, when each loading milestone first shows up in its log.

  loadtime.py CLIP [--binary B] [--home DIR] [--fresh] [--runs 3] [--timeout 120]

--fresh deletes the HOME's proxy cache first, so the first-open path (proxy to build) is timed.
Uses txp_bench.py's HOME and log location (macOS and Linux).
"""
import argparse
import glob
import os
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import txp_bench  # noqa: E402

MILESTONES = [
    ("window/UI up", ["Window manager initialized", "OSD system initialized"]),
    ("file open starts", ["Loading file with path", "Loading file from command line", "Opening file for active player"]),
    ("decode plan", ["Decode plan:"]),
    ("audio first part", ["DecodeToBuffer: seeking to", "DecodeToBuffer:"]),
    ("full-res probe", ["decodes on the GPU:", "decodes on the CPU:", "Skipping Full-Res decoder"]),
    ("file loaded", ["File successfully loaded into player instance"]),
    ("first picture", ["DisplayFrame called #1", "Using low_res_frame (proxy) for frame", "V2 frame:"]),
    ("proxy online", ["Tape threaded", "Using H.264 proxy", "Found existing proxy", "using original as proxy",
                      "Original is already"]),
]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("media")
    default_binary = os.path.join(txp_bench.REPO, "builds/binaries/TapeXPlayer" if sys.platform == "darwin"
                                  else "builds/binaries/TapeXPlayer_linux")
    p.add_argument("--binary", default=default_binary)
    p.add_argument("--home", default="~/.cache/txp-bench-home")
    p.add_argument("--fresh", action="store_true", help="delete the HOME's proxy cache before each run")
    p.add_argument("--runs", type=int, default=3)
    p.add_argument("--timeout", type=float, default=120.0)
    p.add_argument("--env", action="append", default=[])
    a = p.parse_args()
    home = os.path.expanduser(a.home)
    logdir = txp_bench.log_dir(home)

    for run in range(1, a.runs + 1):
        if a.fresh:
            shutil.rmtree(os.path.join(home, ".fstp", "cache", "proxy"), ignore_errors=True)
        for old in glob.glob(os.path.join(logdir, "*.log")):
            os.remove(old)
        env = dict(os.environ, HOME=home, FSTP_RENDER_STATS="1")
        for kv in a.env:
            k, v = kv.split("=", 1)
            env[k] = v
        t0 = time.time()
        proc = subprocess.Popen([a.binary, os.path.abspath(a.media)], env=env, start_new_session=True,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        seen = {}
        try:
            while time.time() - t0 < a.timeout and len(seen) < len(MILESTONES):
                logs = glob.glob(os.path.join(logdir, "*.log"))
                text = open(logs[0], errors="replace").read() if logs else ""
                now = time.time() - t0
                for name, markers in MILESTONES:
                    if name not in seen and any(m in text for m in markers):
                        seen[name] = now
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
        finally:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(10)
            except Exception:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except Exception:
                    pass
        order = sorted(seen.items(), key=lambda kv: kv[1])
        print(f"run {run}{' (fresh proxy cache)' if a.fresh else ''}: " +
              "  ".join(f"{name} {t:.2f}s" for name, t in order), flush=True)
        missing = [name for name, _ in MILESTONES if name not in seen]
        if missing:
            print(f"   not seen within {a.timeout:g} s: {', '.join(missing)}", flush=True)


if __name__ == "__main__":
    main()
