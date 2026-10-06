#!/usr/bin/env python3
"""Transport benchmarks for TapeXPlayer, driven through the player's remote control.

  txp_bench.py phases MEDIA              pause, 1x, 0.5x, 8x, 24x, pause: CPU, UI rate, new pictures/s,
                                         longest picture gap, Betacam per-pass costs
  txp_bench.py reverse MEDIA             mouse-shuttle reversals +A -> -A -> +A: pictures/s and the
                                         longest gap, second by second
  txp_bench.py play-after-shuttle MEDIA  12x shuttle, stop, play: pictures/s and gaps after PLAY

The player runs with FSTP_RENDER_STATS=1 under its own HOME (default ~/.cache/txp-bench-home),
so your settings, resume points and proxies stay untouched; the proxy built on the first run is
reused afterwards. Linux and macOS (remote control file /tmp/tapexplayer_control). To measure in
power-saver on Linux:  powerprofilesctl launch -p power-saver python3 txp_bench.py phases clip.mp4
See tools/README.md.
"""
import argparse, glob, os, platform, re, signal, struct, subprocess, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTROL = "/tmp/tapexplayer_control"
# FSTPRemote.h RemoteCommand::Type
SEEK, PLAY, STOP, SET_SPEED, MOUSE_SHUTTLE = 1, 2, 3, 4, 7


def settings_path(home):
    if platform.system() == "Darwin":
        return os.path.join(home, "Library/Preferences/com.tapexplayer.settings")
    return os.path.join(home, ".config/tapexplayer/settings")


def log_dir(home):
    if platform.system() == "Darwin":
        return os.path.join(home, "Library/Logs/TapeXPlayer")
    return os.path.join(home, ".local/state/TapeXPlayer/logs")


def set_betacam(home, on):
    """Sets [Video] betacam_effect_enabled in the bench HOME's settings file."""
    path = settings_path(home)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    lines = open(path).read().splitlines() if os.path.exists(path) else []
    value = f"betacam_effect_enabled={1 if on else 0}"
    for i, line in enumerate(lines):
        if line.startswith("betacam_effect_enabled="):
            lines[i] = value
            break
    else:
        if "[Video]" in lines:
            lines.insert(lines.index("[Video]") + 1, value)
        else:
            lines += ["", "[Video]", value]
    open(path, "w").write("\n".join(lines) + "\n")


class Player:
    def __init__(self, args):
        self.home = os.path.expanduser(args.home)
        os.makedirs(self.home, exist_ok=True)
        if args.betacam is not None:
            set_betacam(self.home, args.betacam == "on")
        for old in glob.glob(os.path.join(log_dir(self.home), "*.log")):
            os.remove(old)
        env = dict(os.environ, HOME=self.home, FSTP_RENDER_STATS="1")
        for kv in args.env:
            key, value = kv.split("=", 1)
            env[key] = value
        # Own process group: stop() ends the player together with any proxy encoder it spawned.
        self.proc = subprocess.Popen([args.binary, args.media], env=env, start_new_session=True,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        start = time.time()
        while "Loading complete" not in self.log():
            if self.proc.poll() is not None:
                sys.exit(f"player exited during load (code {self.proc.returncode})")
            if time.time() - start > args.load_timeout:
                self.stop()
                sys.exit("player did not finish loading")
            time.sleep(0.2)
        # First run of a clip in this HOME: the proxy is still converting in the background.
        # Measuring now would count the encoder; stopping now would throw the proxy away.
        if "Proxy decoder deferred" in self.log():
            print("building this clip's proxy (first run in this HOME)...", flush=True)
            start = time.time()
            while "Tape threaded" not in self.log():
                text = self.log()
                if "proxy conversion FAILED" in text or "Proxy decoder setup failed" in text:
                    print("proxy failed; measuring on the full-res decoder only", flush=True)
                    break
                if self.proc.poll() is not None:
                    sys.exit(f"player exited while building the proxy (code {self.proc.returncode})")
                if time.time() - start > args.proxy_timeout:
                    self.stop()
                    sys.exit("proxy not ready in time (raise --proxy-timeout)")
                time.sleep(0.5)
        time.sleep(args.settle)   # let the background proxy/audio work after load calm down
        self.send(STOP)
        time.sleep(0.3)

    def log(self):
        logs = glob.glob(os.path.join(log_dir(self.home), "*.log"))
        return open(logs[0], errors="replace").read() if logs else ""

    def send(self, command, value=0.0):
        """Writes a RemoteCommand (int32 type, double value, int32 status) and waits until the
        player marks it processed (status 2)."""
        with open(CONTROL, "r+b") as f:
            f.seek(4)
            f.write(struct.pack("<d", value))
            f.seek(0)
            f.write(struct.pack("<i", command))
            f.seek(12)
            f.write(struct.pack("<i", 0))
        for _ in range(50):
            with open(CONTROL, "rb") as f:
                f.seek(12)
                if struct.unpack("<i", f.read(4))[0] == 2:
                    return
            time.sleep(0.005)

    def cpu_seconds(self):
        pid = self.proc.pid
        if os.path.exists(f"/proc/{pid}/stat"):
            fields = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
            return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
        out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
        days, _, clock = out.rpartition("-")
        seconds = 0.0
        for part in clock.split(":"):
            seconds = seconds * 60 + float(part)
        return seconds + (int(days) * 86400 if days else 0)

    def stop(self):
        for sig, wait in ((signal.SIGTERM, 10), (signal.SIGKILL, 5)):
            try:
                os.killpg(self.proc.pid, sig)
            except ProcessLookupError:
                return
            try:
                self.proc.wait(wait)
                os.killpg(self.proc.pid, signal.SIGTERM)   # leftover children, if any
                return
            except subprocess.TimeoutExpired:
                continue
            except ProcessLookupError:
                return


def render_stats(text):
    return [dict(re.findall(r"([a-z_/]+)=([0-9.]+)", line))
            for line in text.splitlines() if "RENDER STATS" in line]


def phases(player, args):
    player.send(SEEK, args.start)
    time.sleep(2)

    def phase(name, secs):
        offset = len(player.log())
        cpu0, t0 = player.cpu_seconds(), time.time()
        time.sleep(secs)
        cpu1, t1 = player.cpu_seconds(), time.time()
        segment = player.log()[offset:]
        rows = render_stats(segment)
        mean = lambda key: sum(float(r.get(key, 0)) for r in rows) / max(1, len(rows))
        gap = max([int(float(r.get("max_picture_gap", 0))) for r in rows[1:]] or [0])
        passes = {}
        for line in segment.splitlines():
            if "FX STATS" not in line:
                continue
            for key, value in re.findall(r" ([a-z]+)=([0-9.]+)ms", line):
                passes.setdefault(key, []).append(float(value))
            for label, pattern in (("updates/s", r"updates/s=([0-9.]+)"),
                                   ("rows%", r"rows_uploaded=([0-9.]+)%"),
                                   ("fx cpu ms/s", r"cpu/s=([0-9.]+)")):
                m = re.search(pattern, line)
                if m:
                    passes.setdefault(label, []).append(float(m.group(1)))
        detail = "  ".join(f"{k}={sum(v) / len(v):.1f}" for k, v in passes.items())
        print(f"  {name:<11} CPU {100 * (cpu1 - cpu0) / (t1 - t0):4.0f}% | ui {mean('loops/s'):4.1f}/s "
              f"new {mean('new_frames/s'):4.1f}/s gap {gap:4d}ms")
        if detail:
            print(f"  {'':<11} {detail}", flush=True)

    phase("pause", 4)
    player.send(PLAY); phase("play 1x", 5)
    player.send(SET_SPEED, 0.5); phase("slow 0.5x", 5)
    player.send(SET_SPEED, 8); phase("shuttle 8x", 4)
    player.send(SET_SPEED, 24); phase("shuttle 24x", 4)
    player.send(STOP); phase("pause after", 3)


def reverse(player, args):
    player.send(SEEK, args.start)
    time.sleep(3)
    offset, start = len(player.log()), time.time()
    duration = args.cycles * args.period
    while time.time() - start < duration:
        ph = ((time.time() - start) % args.period) / args.period          # 0..1
        speed = args.amplitude * (1 - 4 * ph) if ph < 0.5 else args.amplitude * (-3 + 4 * ph)
        if abs(speed) < 0.1:
            speed = 0.1 if speed >= 0 else -0.1
        player.send(MOUSE_SHUTTLE, speed)
    player.send(MOUSE_SHUTTLE, 0.1)
    player.send(STOP)
    time.sleep(1.5)
    segment = player.log()[offset:]
    # Only whole seconds of sweeping: the first stats line still covers the standstill before the
    # sweeps, the one after the last covers the stop.
    rows = render_stats(segment)[1:int(duration)]
    pictures = [float(r.get("new_frames/s", 0)) for r in rows]
    gaps = [int(float(r.get("max_picture_gap", 0))) for r in rows]
    print(f"new pictures/s per second: {' '.join(f'{x:.0f}' for x in pictures)}")
    print(f"longest gap per second ms: {' '.join(str(g) for g in gaps)}")
    print(f"avg {sum(pictures) / max(1, len(pictures)):.1f} pictures/s, worst gap {max(gaps) if gaps else -1} ms; "
          f"segment loads: {segment.count('Segment [')}, full-res big seeks: {segment.count('Large time jump')}")


def play_after_shuttle(player, args):
    for run in range(args.repeats):
        player.send(SEEK, args.start + 60.0 * run)
        time.sleep(3)
        player.send(MOUSE_SHUTTLE, 12.0)
        time.sleep(2.0)
        player.send(MOUSE_SHUTTLE, 0.1)
        player.send(STOP)
        time.sleep(args.pause)
        offset = len(player.log())
        player.send(PLAY)
        time.sleep(4.2)
        player.send(STOP)
        time.sleep(0.5)
        segment = player.log()[offset:]
        rows = render_stats(segment)[:4]
        pictures = " ".join(f"{float(r.get('new_frames/s', 0)):.0f}" for r in rows)
        gaps = " ".join(str(int(float(r.get("max_picture_gap", 0)))) for r in rows)
        print(f"  run {run + 1}: new pictures/s after PLAY: {pictures}   longest gap/s ms: {gaps}   "
              f"(full-res seeks {segment.count('Large time jump')})")
    print(f"  (the first second's gap also holds the {args.pause:g} s stop before PLAY)")


def main():
    default_binary = os.path.join(REPO, "builds/binaries/TapeXPlayer_linux")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenario", choices=["phases", "reverse", "play-after-shuttle"])
    parser.add_argument("media", help="clip to play (needs an audio track)")
    parser.add_argument("--binary", default=default_binary, help=f"player binary (default {default_binary})")
    parser.add_argument("--home", default="~/.cache/txp-bench-home", help="HOME for the player under test")
    parser.add_argument("--betacam", choices=["on", "off"], help="set the Betacam effect in the bench HOME")
    parser.add_argument("--start", type=float, default=30.0, help="clip position to start from, seconds")
    parser.add_argument("--settle", type=float, default=6.0, help="seconds to wait after load")
    parser.add_argument("--load-timeout", type=float, default=60.0)
    parser.add_argument("--proxy-timeout", type=float, default=900.0,
                        help="seconds to wait for a first-run proxy build")
    parser.add_argument("--amplitude", type=float, default=6.0, help="reverse: peak shuttle speed")
    parser.add_argument("--cycles", type=int, default=4, help="reverse: number of +A/-A sweeps")
    parser.add_argument("--period", type=float, default=2.0, help="reverse: seconds per sweep")
    parser.add_argument("--repeats", type=int, default=2, help="play-after-shuttle: runs")
    parser.add_argument("--pause", type=float, default=0.2,
                        help="play-after-shuttle: stop before PLAY, s (a quick stop/play by hand)")
    parser.add_argument("--env", action="append", default=[], metavar="K=V", help="extra player environment")
    args = parser.parse_args()
    if not os.path.exists(args.binary):
        sys.exit(f"no player binary at {args.binary} (build it or pass --binary)")

    player = Player(args)
    try:
        {"phases": phases, "reverse": reverse, "play-after-shuttle": play_after_shuttle}[args.scenario](player, args)
    finally:
        player.stop()


if __name__ == "__main__":
    main()
