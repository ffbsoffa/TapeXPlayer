#!/usr/bin/env bash
# Bit-exactness test of the CPU-specific kernels in FSTPSIMDOptimizations (see tools/README.md).
# x86-64 (Intel/AMD): runs the SSE2 path (with the AVX2 smear where the CPU has it, and once
# more with FSTP_SIMD_NO_AVX2=1), the scalar fallback and the NEON code through neon_shim/.
# arm64 (Apple M-series, Snapdragon, Raspberry Pi): runs the real NEON path.
# Needs a C++23 compiler and FFmpeg's libavutil (found with pkg-config). Exit status 1 on any
# mismatch.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
modules=$(cd "$here/../../source/modules" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

CXX=${CXX:-c++}
flags=(-std=c++23 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-builtin-macro-redefined
       -I"$modules/FSTPVideoModule" -I"$modules/FSTPMainModule" -I"$modules/FSTPMainModule/WSGUI")
read -r -a av_cflags <<< "$(pkg-config --cflags libavutil)"
read -r -a av_libs <<< "$(pkg-config --libs libavutil)"

run() {   # run <name> [extra flags...]
    local name=$1
    shift
    "$CXX" "${flags[@]}" ${av_cflags[@]+"${av_cflags[@]}"} "$@" "$here/simd_kernels_test.cpp" \
        -o "$out/$name" ${av_libs[@]+"${av_libs[@]}"}
    "$out/$name"
}

case "$(uname -m)" in
    x86_64|amd64|AMD64)
        flags+=(-msse2 -msse4.1)   # the app's x86 baseline (makefile)
        run sse2                              # AVX2 smear where the CPU has it (run-time choice)
        FSTP_SIMD_NO_AVX2=1 "$out/sse2"       # and the SSE2 smear on the same machine
        run scalar -DPATH_SCALAR
        run neon_shim -DPATH_NEON -I"$here/neon_shim"
        ;;
    arm64|aarch64)
        run neon
        ;;
    *)
        run native
        ;;
esac
echo "all kernel paths bit-identical"
