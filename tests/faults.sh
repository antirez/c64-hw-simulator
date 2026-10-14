#!/bin/sh
# tests/faults.sh - run common C64 damages and save what the monitor shows.
#
#   tests/faults.sh [outdir]
#
# Each scenario boots the board for 4 seconds with the given fault(s), then
# saves the picture (PNG) and the text read from the picture. The EXPECTED
# column is what repair experience says a real 250407 shows; it was written
# BEFORE running the simulator, and the simulator has no code that knows
# about any of these symptoms: whatever it shows comes from the netlist and
# the chip models.
OUT=${1:-/tmp/c64hw-faults}
BIN=./c64-hw-sim
FRAMES=200
mkdir -p "$OUT"

run() {     # name "expected" faults...
    name=$1; expected=$2; shift 2
    args=""
    for f in "$@"; do args="$args --fault $f"; done
    {
        echo "== $name"
        echo "faults:   $*"
        echo "expected: $expected"
        $BIN --headless $FRAMES --text --shot "$OUT/$name.png" $args
    } > "$OUT/$name.txt" 2>&1 &
}

run healthy        "READY screen, 38911 BYTES FREE"
run cpu_removed    "black screen"                                U7:removed
run pla_removed    "black screen"                                U17:removed
run kernal_removed "black screen"                                U4:removed
run basic_removed  "screen cleared to blue, no banner text"      U3:removed
run char_removed   "boots, cursor blinks, garbage characters"    U5:removed
run colorram_removed "text in random flickering colors"          U6:removed
run cia1_removed   "READY screen but no blinking cursor"         U1:removed
run cia2_removed   "READY screen, looks normal"                  U2:removed
run sid_removed    "normal READY screen"                         U18:removed
run vic_removed    "no picture at all (no sync)"                 U19:removed
run dram_d0_removed "black screen"                               U21:removed
run dram_d0_q_gnd  "black screen (D0 stuck low for everybody)"   U21.14:gnd
run dram_d7_q_vcc  "black screen (D7 stuck high for everybody)"  U12.14:vcc
run ls373_removed  "CPU works, screen garbage (VIC addresses)"   U26:removed
run cd4066_removed "colors don't follow: power-on color garbage" U16:removed
run cia1_cs_high   "READY but no cursor, keyboard dead"          U15.12:vcc
run ls7406_removed "black screen"                                U8:removed
run ls257_removed  "black screen or wrong BYTES FREE"            U13:removed
run crystal_broken "black and white picture"                     Y1:open
run vco_removed    "no picture at all (no sync)"                 U31:removed
run timer556_removed "black screen (reset held or missing)"      U20:removed
run pla_casram_high "black screen"                               U17.18:vcc
run pla_charom_high "boots, garbage characters"                  U17.15:vcc
run game_to_gnd    "black screen (Ultimax mode, no cartridge)"   CN6.8:gnd
run sid_d3_gnd     "black screen (SID shorts the data bus)"      U18.18:gnd
run cpu_a15_open   "black screen"                                U7.23:open
run dram_d0_din_open "black screen or garbage (RAM bit 0 can't be written)" U21.2:open
run ls258_removed  "CPU works, VIC shows wrong memory (garbage screen)" U14:removed
run ls08_removed   "black screen (RDY/AEC/color RAM select lost)" U27:removed
run ls139_removed  "VIC/CIA/SID never selected: black screen, CPU runs" U15:removed
wait
for f in "$OUT"/*.txt; do cat "$f"; echo; done
