#!/bin/sh
# tests/zx-diag.sh - the ZX Spectrum Diagnostics ROM (carts/) in the edge
# connector, on a healthy board and with bad RAM chips. The EXPECTED lines
# are what the ROM's documentation says it reports. Its small font can't be
# read by --text: look at the pictures in the output directory.
#
#   tests/zx-diag.sh [outdir]
#
# The ROM tests the lower 16K, then asks for the model: the "1" typed over
# and over (ignored until then) chooses the 48K tests of the upper RAM.
OUT=${1:-/tmp/zxhw-diag}
BIN=./c64-hw-sim
ROM=carts/zx-diagnostics-v0.37.rom
mkdir -p "$OUT"
KEYS=111111111111111111111111111111111111111111111111111111111111

run() {     # name frames "expected" faults...
    name=$1; frames=$2; expected=$3; shift 3
    args=""
    for f in "$@"; do args="$args --fault $f"; done
    {
        echo "EXPECTED: $expected"
        C64HW_TYPE=$KEYS $BIN --machine zx48 $ROM --headless $frames --shot "$OUT/$name.png" $args
    } > "$OUT/$name.txt" 2>&1 &
}

run healthy 3000 "every test PASS, 48K RAM Tests Passed, Testing Completed"
run lram_ic8 1500 "lower RAM: RAM FAIL 2, the stripe of bit 2 red (IC8 is D2)" IC8:removed
run lram_ic13 1500 "lower RAM: RAM FAIL 7, the stripe of bit 7 red (IC13 is D7)" IC13:removed
run uram_ic17 3000 "upper RAM tests FAIL, failures found in bit 2, red border (IC17 is D2)" IC17:removed
run uram_ic22 3000 "upper RAM tests FAIL, failures found in bit 7, red border (IC22 is D7)" IC22:removed
wait
for f in "$OUT"/*.txt; do
    echo "== $(basename "$f" .txt): $OUT/$(basename "$f" .txt).png"
    head -1 "$f"
done
