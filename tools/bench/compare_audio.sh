#!/usr/bin/env bash
# Decodes a clip's whole soundtrack with two player builds and compares the int16 playback
# buffers byte for byte — for changes to the audio decode/convert path that must not change the
# sound (e.g. a new SIMD kernel). Linux only: the buffer is an unlinked temp file, read through
# /proc/<pid>/fd while the player runs.
#
# usage: compare_audio.sh PLAYER_A PLAYER_B CLIP
# Both runs share one HOME (default ~/.cache/txp-bench-home, override with TXP_BENCH_HOME), so
# the proxy is built once. Each player runs in its own process group, which is stopped as a whole.
set -euo pipefail
[ $# -eq 3 ] || { echo "usage: $0 PLAYER_A PLAYER_B CLIP" >&2; exit 2; }
home=${TXP_BENCH_HOME:-$HOME/.cache/txp-bench-home}
logs="$home/.local/state/TapeXPlayer/logs"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
mkdir -p "$home"

dump() {   # dump PLAYER OUTFILE CLIP
    rm -f "$logs"/*.log
    HOME=$home setsid "$1" "$3" >/dev/null 2>&1 &
    local pid=$! log="" i
    for i in $(seq 1 600); do   # up to 5 minutes for the full decode
        log=$(ls "$logs"/*.log 2>/dev/null | head -1 || true)
        if [ -n "$log" ] && grep -qE "Full buffer ready|full decode done|Background decoding completed" "$log"; then
            break
        fi
        kill -0 "$pid" 2>/dev/null || { echo "player exited early: $1" >&2; return 1; }
        sleep 0.5
    done
    sleep 1
    local fd
    fd=$(ls -l "/proc/$pid/fd" | awk '/fstp_audio_.*\(deleted\)/ {print $9; exit}')
    [ -n "$fd" ] || { kill -- -"$pid"; echo "no audio buffer found for $1" >&2; return 1; }
    cp "/proc/$pid/fd/$fd" "$2"
    kill -- -"$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "$(basename "$1"): $(stat -c %s "$2") bytes"
}

dump "$1" "$out/a.raw" "$3"
dump "$2" "$out/b.raw" "$3"
if cmp -s "$out/a.raw" "$out/b.raw"; then
    echo "IDENTICAL decoded audio"
else
    cmp "$out/a.raw" "$out/b.raw" | head -1
    echo "DIFFERENT decoded audio"
    exit 1
fi
