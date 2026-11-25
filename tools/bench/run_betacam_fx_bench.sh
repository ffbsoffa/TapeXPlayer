#!/usr/bin/env bash
# Builds betacam_fx_bench.cpp against the production FSTPBetacamEffect.cpp and runs it with the
# physical shuttle and with the old comb (FSTP_BETACAM_PHYS=0). Args: [width height].
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
modules=$(cd "$here/../../source/modules" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
CXX=${CXX:-c++}
flags=(-std=c++23 -O2 -Wno-unused-parameter -I"$modules/FSTPVideoModule" -I"$modules/FSTPMainModule/WSGUI")
case "$(uname -m)" in x86_64|amd64) flags+=(-msse2 -msse4.1) ;; esac
read -r -a cf <<< "$(pkg-config --cflags sdl2 libavutil)"
read -r -a lf <<< "$(pkg-config --libs sdl2 libavutil)"
"$CXX" "${flags[@]}" "${cf[@]}" "$here/betacam_fx_bench.cpp" "$modules/FSTPMainModule/WSGUI/FSTPBetacamEffect.cpp" \
    "$modules/FSTPVideoModule/FSTPSIMDOptimizations.cpp" -o "$out/fx" "${lf[@]}"
"$out/fx" "$@"
FSTP_BETACAM_PHYS=0 "$out/fx" "$@"
