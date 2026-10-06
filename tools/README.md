# Tools

Checks and measurements behind the "snappy on a power-saver Pentium" work. The rule they serve:
one code path for macOS, Windows and Linux, tuned on the weakest machine, decided by measurement.

| Tool | What it answers | Platforms |
|---|---|---|
| `simd/run_simd_tests.sh` | Do the SSE2, NEON and scalar kernels give bit-identical results? | Linux, macOS, Windows (MSYS2) |
| `bench/txp_bench.py` | How responsive is the player: CPU, UI rate, pictures/s, freezes, Betacam cost | Linux, macOS |
| `bench/compare_audio.sh` | Does a change leave the decoded sound byte-for-byte the same? | Linux |

## simd/ — kernel bit-exactness

All CPU-specific code lives in `source/modules/FSTPVideoModule/FSTPSIMDOptimizations` with an SSE2
path (every x86-64 CPU, Intel and AMD), a NEON path (Apple M-series, Windows on Snapdragon,
Raspberry Pi) and a scalar fallback. They must produce exactly the same pixels and samples, so
the look and the sound never depend on the machine.

```sh
tools/simd/run_simd_tests.sh
```

Needs a C++23 compiler and FFmpeg's libavutil (via `pkg-config`). The test compiles the
production `FSTPSIMDOptimizations.cpp` and compares each kernel — Betacam smear and soft-edge row,
audio S32/float/planar-float → int16 — with the scalar loop it replaced, on thousands of random
and edge-case inputs (odd sizes, NaN, ±∞, out-of-range floats).

- On x86-64 it runs three builds: SSE2, scalar, and the NEON code through `simd/neon_shim/`, a
  portable model of the NEON intrinsics. That shim is how NEON is checked on a machine with no ARM
  compiler; when a new NEON kernel uses another intrinsic, add it there with its exact semantics.
- On arm64 (e.g. an M1 Mac) it runs the real NEON path — the check the shim can't replace.

Exit status 1 on any mismatch. Run it after touching any kernel.

## bench/txp_bench.py — transport measurements

Drives the player through its remote control (`/tmp/tapexplayer_control`) with
`FSTP_RENDER_STATS=1` and reads the per-second stats from its log.

```sh
# Linux, measured in power-saver (the condition most users run in)
powerprofilesctl launch -p power-saver python3 tools/bench/txp_bench.py phases clip.mp4 --betacam on
python3 tools/bench/txp_bench.py reverse clip.mp4 --amplitude 20
python3 tools/bench/txp_bench.py play-after-shuttle clip.mp4
```

| Scenario | What it does | Watch |
|---|---|---|
| `phases` | pause 4 s, 1× 5 s, 0.5× 5 s, 8× 4 s, 24× 4 s, pause 3 s | `CPU`, `ui` (60/s = smooth), `new` pictures/s, `gap` (longest wait for a new picture), Betacam pass costs in ms and `fx cpu ms/s` |
| `reverse` | mouse-shuttle sweeps +A → −A → +A | pictures/s and the longest gap each second (freezes at reversals) |
| `play-after-shuttle` | 12× shuttle, stop, play | pictures/s and gaps in the first seconds after PLAY |

- The player runs under its own HOME (`--home`, default `~/.cache/txp-bench-home`), so your
  settings, resume points and proxy cache are untouched. The first run on a clip builds its
  proxy there and waits for it before measuring (minutes on a slow machine); later runs reuse it.
- `--betacam on|off` sets the effect in that HOME. `--binary` picks the build (default
  `builds/binaries/TapeXPlayer_linux`), `--env K=V` adds player environment
  (e.g. `--env FSTP_SHUTTLE_SEGMENTS=1` for the old segment-based shuttle, to compare).
- The clip needs an audio track.
- **Close any other TapeXPlayer first**: the remote-control file is shared, so the bench would
  steer that window too, and a second player skews the CPU numbers.
- CPU % is per process and counts all cores (100% = one core busy); it includes decoding, so it
  varies more between runs than the effect's own `fx cpu ms/s`.

Reference, GPD Pocket 3 (Pentium Gold 7505), power-saver, 720p clip, Betacam on, 2026-10-06:

| | pause | 1× | 0.5× | 8× | 24× |
|---|---|---|---|---|---|
| fx cpu ms/s | 38 | 75 | 155 | 76 | 94 |
| ui /s | 60 | 60 | 60 | 60 | 60 |

## bench/compare_audio.sh — sound unchanged

```sh
tools/bench/compare_audio.sh old/TapeXPlayer_linux builds/binaries/TapeXPlayer_linux clip.mp4
```

Decodes the whole soundtrack with both builds and compares the int16 playback buffers byte for
byte (read through `/proc`, so Linux only). Use it for any change to audio decoding or sample
conversion. Each player runs in its own process group and is stopped together with anything it
spawned.
