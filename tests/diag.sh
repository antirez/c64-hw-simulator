#!/bin/sh
# tests/diag.sh - run Commodore's diagnostic cartridges (carts/) on the
# simulated board and check that they report what their manuals say a
# real C64 reports. Not run by the other tests: it takes a few minutes
# (the cases run in parallel, about 4 minutes on a fast machine).
#
#   tests/diag.sh [outdir]
#
# Dead Test (rev 781220): on a healthy board the test screen comes up;
# every DRAM pulled out gives the flash code of the manual's "RAM Error
# Flash Code Chart" (1 flash U12, bit 7 ... 8 flashes U21, bit 0); without
# the PLA nothing comes up ("initial startup failure").
#
# Diagnostic 586220 with its test harness: a healthy board passes every
# test; a broken user port or keyboard contact, the paddle switch U28
# pulled out, or the mains cut (no 50 Hz for the TOD clocks) give the
# failures the manual describes. Without the harness the keyboard is OPEN.
#
# The simulator knows nothing about these cartridges: what they report
# comes from the netlist and the chip models.
OUT=${1:-/tmp/c64hw-diag}
BIN=./c64-hw-sim
DT=carts/deadtest-781220.crt
DIAG=carts/diag-586220.crt
mkdir -p "$OUT"

dead() {    # name faults...
    name=$1; shift
    args=""
    for f in "$@"; do args="$args --fault $f"; done
    $BIN --headless 1500 --flashes --shot "$OUT/$name.png" $args $DT > "$OUT/$name.txt" 2>&1 &
}

diag() {    # name harness faults...
    name=$1; harness=$2; shift 2
    args=""
    [ "$harness" = 1 ] && args="--harness"
    for f in "$@"; do args="$args --fault $f"; done
    $BIN --headless 3000 --text --shot "$OUT/$name.png" $args $DIAG > "$OUT/$name.txt" 2>&1 &
}

dead dt_healthy
for u in U12 U24 U11 U23 U10 U22 U9 U21; do dead dt_$u $u:removed; done
dead dt_pla U17:removed
diag d_healthy 1
diag d_userport 1 CN2.C:open
diag d_u28 1 U28:removed
diag d_keyboard 1 CN1.13:open
diag d_mains 1 R100:open
diag d_noharness 0
wait

fail=0
check() {   # what, then a command that succeeds if it is right
    what=$1; shift
    if "$@"; then echo "ok    $what"; else echo "FAIL  $what"; fail=1; fi
}
has() { grep -q "$2" "$OUT/$1.txt"; }
hasnt() { ! grep -q "$2" "$OUT/$1.txt"; }
# Every complete series (the last one may be cut by the end of the run)
# has n flashes, and there are at least two of them.
flashes() {
    awk -v n="$2" '/^flash series:/ {
        if (NF < 5) exit 1
        for (i = 3; i < NF; i++) if ($i != n) exit 1
        ok = 1
    } END { exit !ok }' "$OUT/$1.txt"
}

check "Dead Test, healthy: no flashes, the test screen is up" has dt_healthy "flash series: none"
check "Dead Test, healthy: the screen is lit" has dt_healthy "screen at the end: lit"
n=1
for u in U12 U24 U11 U23 U10 U22 U9 U21; do
    check "Dead Test, $u pulled out: $n flashes" flashes dt_$u $n
    n=$((n + 1))
done
check "Dead Test, PLA pulled out: no flashes" has dt_pla "flash series: none"
check "Dead Test, PLA pulled out: the screen stays dark" has dt_pla "screen at the end: dark"
check "586220 + harness, healthy: no BAD" hasnt d_healthy "BAD"
check "586220 + harness, healthy: no OPEN" hasnt d_healthy "OPEN"
check "586220 + harness, healthy: USER PORT OK" has d_healthy "USER PORT *OK"
check "586220, user port PB0 contact open: USER PORT BAD" has d_userport "USER PORT *BAD"
check "586220, U28 pulled out: CONTROL PORT BAD" has d_u28 "CONTROL PORT *BAD"
check "586220, U28 pulled out: the SID is blamed" has d_u28 "6581 U18 *BAD"
check "586220, keyboard COL0 contact open: KEYBOARD BAD" has d_keyboard "KEYBOARD *BAD"
check "586220, mains cut: INTERRUPT BAD (no TOD alarm)" has d_mains "INTERRUPT *BAD"
check "586220, mains cut: the clocks don't move" has d_mains "00:00:00AM 00:00:00PM"
check "586220 without harness: KEYBOARD OPEN" has d_noharness "KEYBOARD *OPEN"

[ $fail = 0 ] && echo "all the diagnostic checks passed (pictures and text in $OUT)"
exit $fail
