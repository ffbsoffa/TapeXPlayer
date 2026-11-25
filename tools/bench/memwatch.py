#!/usr/bin/env python3
"""Plays a clip at 1x for a while and samples the player's memory every few seconds: RSS and,
on Linux, the GPU memory its DRM file descriptors report (drm-total-* in /proc/PID/fdinfo).
Finds leaks that only show over long playback — e.g. a cache that grows with every frame.

  memwatch.py CLIP [--binary B] [--seconds 120] [--every 10] [--betacam on|off]

Uses txp_bench.py's player control (its own HOME, remote control, FSTP_RENDER_STATS).
"""
import argparse
import glob
import os
import re
import sys
import time
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import txp_bench  # noqa: E402


def rss_mib(pid):
    try:
        for line in open(f"/proc/{pid}/status"):
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) / 1024.0
    except OSError:
        pass
    out = os.popen(f"ps -o rss= -p {pid}").read().strip()   # macOS
    return int(out) / 1024.0 if out else -1.0


def gpu_mib(pid):
    """Sum of drm-total-* over the process's DRM fds (Linux; i915/xe/amdgpu report it)."""
    total_kib, seen = 0, set()
    for fd in glob.glob(f"/proc/{pid}/fd/*"):
        try:
            if not os.readlink(fd).startswith("/dev/dri/"):
                continue
            info = open(f"/proc/{pid}/fdinfo/{os.path.basename(fd)}").read()
        except OSError:
            continue
        client = re.search(r"drm-client-id:\s*(\d+)", info)
        if client and client.group(1) in seen:
            continue   # the same DRM client through another fd
        if client:
            seen.add(client.group(1))
        for value, unit in re.findall(r"drm-total-[\w-]+:\s*(\d+)\s*(\w*)", info):
            scale = {"KiB": 1, "MiB": 1024, "GiB": 1024 * 1024, "": 1 / 1024}.get(unit, 1)
            total_kib += int(value) * scale
    return total_kib / 1024.0 if seen or total_kib else -1.0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("media")
    p.add_argument("--binary", default=os.path.join(txp_bench.REPO, "builds/binaries/TapeXPlayer_linux"))
    p.add_argument("--home", default="~/.cache/txp-bench-home")
    p.add_argument("--betacam", choices=["on", "off"])
    p.add_argument("--seconds", type=float, default=120.0)
    p.add_argument("--every", type=float, default=10.0)
    p.add_argument("--start", type=float, default=30.0)
    p.add_argument("--env", action="append", default=[])
    a = p.parse_args()
    args = types.SimpleNamespace(media=a.media, binary=a.binary, home=a.home, betacam=a.betacam, env=a.env,
                                 settle=6.0, load_timeout=60.0, proxy_timeout=900.0)
    player = txp_bench.Player(args)
    try:
        pid = player.proc.pid
        player.send(txp_bench.SEEK, a.start)
        time.sleep(2)
        player.send(txp_bench.PLAY)
        t0 = time.time()
        first = None
        while True:
            t = time.time() - t0
            sample = (rss_mib(pid), gpu_mib(pid))
            first = first or sample
            print(f"  {t:6.0f} s   RSS {sample[0]:7.1f} MiB ({sample[0] - first[0]:+6.1f})   "
                  f"GPU {sample[1]:7.1f} MiB ({sample[1] - first[1]:+6.1f})", flush=True)
            if t >= a.seconds:
                break
            time.sleep(a.every)
        player.send(txp_bench.STOP)
    finally:
        player.stop()


if __name__ == "__main__":
    main()
