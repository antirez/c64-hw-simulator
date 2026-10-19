# c64-hw-sim

c64-hw-sim simulates the motherboards of the Commodore 64 (ASSY 250407)
and of the ZX Spectrum 48K (board Issue 3B), pin by pin. You can lift the
leg of a chip, short it to ground or to +5V, pull a chip out of its
socket, or open a resistor. Then you watch what the machine does: the
picture on the monitor, the keyboard, every signal on the board.

The program knows the board's netlist (the electrical connections on the
PCB) and how each chip behaves at its pins. It knows nothing about what a
broken Commodore 64 or Spectrum looks like: when you simulate a broken
part and the screen fills with garbage, that garbage was computed from
the circuit, not chosen by someone who knew what to expect.

Here is an example. Lift pin 17 (A7) of the C64 color RAM before power
on, and the machine boots and looks healthy. The CPU writes and the VIC
reads with the same wrong address, so the RAM simply aliases. Put the leg
back, and the half of the color RAM that was never written shows up as
random colors. Lift it again, and the screen is clean. A real 2114 does
the same.

With this program you can do mainly two things:

1. Learn how the Commodore 64 and the Spectrum work at a more intimate
   level, in a way that is hard to reproduce on real hardware without the
   risk of damaging it (and on most boards many chips are soldered, not
   socketed).
2. If you have a broken C64 or Spectrum and some hypothesis about what
   could be broken, simulate it with this program and perhaps gain clues,
   so that you can desolder chips in the most logical order, and desolder
   as few of them as possible (desoldering is a boring process, and every
   time it risks damaging the board...). A matching symptom is a clue,
   not a proof: different faults can look the same, and the simulation
   has its limits (see below).

Please note that **even on fast computers** this program struggles to
simulate these machines at real speed. A real C64 is simulated at 31% of
its speed on an Apple M3 Max, 39% on an AMD Ryzen AI Max+ PRO 495 (Gorgon
Halo), and 48% on an Apple M5 Max. The ZX Spectrum is simulated at 23%,
28% and 34% of its speed on the same computers. This is not a problem for
the goals of this program, but be warned: it is not an emulator to play
games with.

Why is the Spectrum slower to simulate than the C64, even though its
board is simpler? Because of the time resolution. The ULA is simulated
at a very low level, so that the bus contention just emerges from the
simulation. Its signals are timed to the half period of its 14 MHz
oscillator, so the whole board settles 28 million times per simulated
second, against about 6 million for the C64.

![The character ROM pulled out while a BASIC program runs, then put back](docs/char-rom.gif)

Above: the classic C64 `10 PRINT CHR$(205.5+RND(1));:GOTO 10` maze is
running when the character ROM (U5) is pulled out of its socket, and
later put back. The CPU, the RAM and the program keep working; only the
shapes of the characters are lost, as repairers know well. The scope is
on the data line D0 of the ROM socket: with the socket empty, the bus
floats (the dotted segments) whenever the VIC fetches a character shape.
Shown at C64 speed.

## Acknowledgements

I (antirez) got the idea for this program while using and observing Andre
Weissflog's (floooh) chips, emulators of the 6502, VIC-II, CIA, SID and
friends that use pins as their interface. The next step was to take this
approach, with his code as a starting point, to its logical extreme:
pin-level models of every other part of the C64, the RAMs, the ROMs, the
logic chips, the PLA, and so forth.

The chip cores are floooh's chips, taken from c64-llm (zlib license). The
board comes from bwack's C64-250407-Replica-KiCad (CERN OHL). The PLA
equations come from the 82S100 read-out by Mark Smith and Marko Mäkelä.

For the ZX Spectrum: the Z80 is floooh's too, altered to run on the edges
of its clock. The board comes from liveboxandy's KiCad files of the Issue
3B, redrawn from PABB's gerbers (CC0). The ULA model is checked against
the gate netlist of the 6C001 recovered from the die by the emu-russia/ula
project (CC0). The ROM is Amstrad's, who allow its distribution. The ZX
Spectrum Diagnostics ROM is by Brendan Alford and its contributors (LGPL).

## Building

    make
    ./c64-hw-sim [program.prg]

It runs on macOS, Linux (X11) and Windows, with OpenGL. On Windows, build
with MinGW-w64 gcc from a POSIX shell (Git Bash or MSYS2), for example
`mingw32-make`, then run `c64-hw-sim.exe`. The graphics library (sokol)
and the interface library (nuklear) are in `third_party/`.

## Using it

The window shows the board, the monitor, a control panel and an
oscilloscope. The two buttons at the top of the control panel switch
between the Commodore 64 and the ZX Spectrum 48K: the machine that runs
is switched off, the other one on.

Pins show their state as a logic probe would: red high, blue low, yellow
pulsing, gray floating, magenta when two outputs fight. Zoom in and the
chips show references, address ranges and pin numbers; zoom in more and
the logic chips show their gates, the others their pin names. Hovering a
pin tells what it is: connector pins have their function ("CN4.3 ATN
(SERIAL)").

Click a pin to put it on the oscilloscope and to break it: floating,
shorted to GND, or shorted to +5V. Click a chip to pull it out, or to
inspect it. Click a resistor, a ferrite bead or the crystal to open it.
The control panel lists the faults, each with a repair button.

The inspector draws the chip large, like a datasheet: the pins numbered
and colored, the nets outside, the gates inside, wired to their pins. A
wire shows what is on it; the output of a gate shows what the gate itself
drives, so a gate driving high into a net that stays low is plain to see.

Open (Ctrl+O, Cmd+O on the Mac) shows a file browser, starting in
`carts/`. It lists folders and the files the running machine takes, and
says what each file is, from its header, or why it can't be used. Type to
narrow the list, or type a path and press Enter to go there. Arrows and
Enter, or a double click, open a file; Backspace goes up; Esc closes.
Recent files stay in the control panel, one click away, and files can be
dropped on the window too.

## Commodore 64

Chips are boxes named by role:

- RAM1 to RAM8 are the 4164 DRAMs. Each one holds a single bit of every
  byte: RAMn has data bit n-1.
- VRAM is the 2114 color RAM.
- BASIC, KERNAL and CHAR are the ROMs.
- PLA is the address decoder.

In the inspector, the output mark of a 7406 is the open collector
transistor: blue while it pulls the line low, gray while it is off and
the pull-up decides. For the PLA the inspector lists its product terms,
lit while they are true, with every input colored by its level: which
term selects what, and which input keeps a term from firing.

RESET works like a reset cartridge: it shorts /RESET to GND on the user
port. Hold it, or keep it pressed to look at the board in reset. The CPU,
the CIAs and the SID reset; the VIC, which has no reset pin, and the RAM
don't. Power cycle switches the C64 off and on again.

Programs (.prg) are loaded and RUN, or only loaded with Shift+Enter; the
browser says if one is BASIC or machine code, and its address. They are
written straight into the DRAM chips, as a freezer cartridge would do it,
so loading works on a broken board too.

Cartridges (.crt) are plugged into the expansion port (the C64 is
switched off and on, as you would do); the browser shows a cartridge's
kind and name. The plain ones are supported, 8K, 16K and Ultimax, which
is what the diagnostic cartridges are: Dead Test replaces the KERNAL and
runs on boards with dead RAM or ROMs. A cartridge only drives the
expansion port pins, everything else is the board: a dirty contact on
the port is a fault on a CN6 pin.

Dead Test gives the flash codes of its manual (one flash for U12, bit 7,
up to eight for U21, bit 0). For the Diagnostic 586220 cartridge you can
plug in its test harness, the dongles that loop back the user port, the
cassette port, the serial port, both control ports and the keyboard (as
built by Sven Petersen, an open hardware copy of Commodore's). With it a
healthy board passes every test, and a broken contact, chip or clock
shows where Commodore's manual says it should. Both cartridges are in
`carts/` (they are Commodore's, see `carts/LICENSE`).

Type on your keyboard as usual. ESC is RUN/STOP, PgUp is RESTORE, TAB is
CTRL, and left ALT is C=.

## ZX Spectrum 48K

`--machine zx48` starts with the Spectrum, and so does a snapshot or a
ROM given on the command line.

The board is the Issue 3B. Its chips:

- ULA is the Ferranti 6C001. It makes the picture, makes the CPU clock,
  drives the RAS and CAS of the lower RAM, and is the port 0xFE (keyboard,
  border, MIC, EAR).
- CPU is the Z80A, and ROM is the 23128.
- LRAM0 to LRAM7 are the 4116 DRAMs of the lower 16K, where the screen is.
  LRAMn has data bit n.
- URAM0 to URAM7 are the 4532s of the upper 32K: 64K chips with one bad
  half. The links next to them tell the board which half is good; here TI
  chips are fitted.
- MUX are the 74LS157 address multiplexers. OR and NAND, a 74LS32 and a
  74LS00, select the upper RAM and time its RAS and CAS, with two RC
  delays.
- PAL is the LM1889 video modulator. It is analog and the one chip that
  is not simulated: the monitor shows the ULA's picture.

The CPU and the ULA share the lower 16K, and this is what makes the
Spectrum interesting at the pin level. They sit on the two sides of a row
of resistors: 470 ohm on the data bus, 330 ohm on the address lines after
the multiplexers. When both drive the bus, each side sees its own value.
When the CPU wants the lower 16K while the ULA fetches the screen, the ULA
stops the CPU's clock: the famous contention. Here that happens because
the ULA holds its clock pin, not because a table of delays says so. On the
simulated board, the delays follow the 6,5,4,3,2,1,0,0 pattern known from
the real machine. The first contended T-state comes 14337 T-states after
/INT falls. Emulators put it at 14335 or 14336, counting from their own
frame start.

Keys: ESC is BREAK, TAB is SYMBOL SHIFT, left SHIFT is CAPS SHIFT. RESET
shorts /RESET to GND on the edge connector. Open loads 48K snapshots
(.sna, .z80): they are written straight into the DRAM chips, like the C64
programs. A 16K ROM (.rom) is plugged into the edge connector like a ROM
cartridge: it holds /ROMCS high and answers in place of the internal ROM.
The ZX Spectrum Diagnostics ROM is in `carts/` (LGPL). On a healthy board
it passes its tests; on a broken one it says what it found.

The power supply is modeled by what it does. The 7805 makes +5V. The
DC-DC converter (TR4, TR5 and the coil T1) makes +12V, and its charge
pump makes -5V. The 4116s need all three: break TR4, or D12 of the -5V
pump, and the lower RAM stops working. Without the crystal X1 there is no
clock at all.

`tests/zx-faults.sh` runs 13 common faults. As on the C64, the expected
symptom of each one was written down before running the simulator. A
missing lower RAM chip leaves the ROM's RAM test pattern on the screen,
and a missing upper RAM chip gives a 16K Spectrum. `tests/zx-diag.sh`
runs the Diagnostics ROM on a healthy board and with bad RAM chips: it
finds the failing data bit (so the chip) where its manual says it does.

### The ULA

The ULA is not an emulator core: it is a model at its pins, at the
resolution of its 14 MHz oscillator. Every half period it decides /RAS,
/CAS, the address pins, the CPU clock, /INT, what it puts on the data bus
for a port read, and the colour of the pixel. Its logic and its timing
come from the die: the emu-russia/ula project recovered the 6C001's
netlist, 661 NOR gates.

`tools/ulacheck.c` keeps the model honest. The simulator logs what the
ULA saw on its input pins at every half period (`ZXHW_ULALOG=file`), and
the checker runs the 661 gates on the same inputs. Then it compares every
output, at every half period, and the colour of every pixel. It has been
run on a Spectrum booting, on the ROM's editor, and on a BASIC program
that changes the border colour as fast as it can. There are three
documented simplifications, listed at the top of `src/ula.c`, none of
which matters to the chips on the board. Apart from them, every output
and every pixel are the same. The gates also taught a detail: a new
border colour shows only from the next 8-pixel boundary, because it goes
through the same latch as the attributes. The netlist is not in this
repository; `tools/ulacheck.c` says where to get it.

### The Z80

floooh's Z80 runs one T-state per call and puts its pins out when it suits
an emulator. The ULA needs more: it looks at the address and /MREQ at the
right half of a T-state to decide whether to stop the clock, and it can
stop the clock in either phase. So `chips/z80.h` is now stepped on every
edge of its clock, and its pins follow the Zilog timing diagrams. Its
instructions, flags and timings are those of the original core.
`tests/z80` has the tests: the pin waveforms of every kind of bus cycle,
and ZEXDOC and ZEXALL. `chips/codegen` has the code generator.

## Command line

    ./c64-hw-sim --headless 200 --text --shot out.png --fault U17:removed

This runs four seconds (200 frames) without a window. It prints the text
read from the monitor picture, as a person would read it, and saves the
picture. A fault is written `REF:what` or `REF.PIN:what`:

    U17:removed   U21.14:gnd   U12.14:vcc   U9.2:open   CN6.B:open   RP4:short

A .crt file given on the command line is plugged in before power on,
`--harness` plugs the 586220 test harness.

Other options:

- `--at 150:U6.17:open --at 300:repair` breaks and repairs parts while the
  C64 runs; `RESET:pressed` and `RESET:released` work there too.
- `--machine zx48` runs the ZX Spectrum; fault specs use its references
  (`IC10:removed`, `D12:open`, `TI-3:open`).
- `--nets` reports the nets where outputs fought.
- `C64HW_TYPE=$'PRINT 1+1\n'` types text after boot.
- `ZXHW_ULALOG=file[:cycle]` logs the ULA's pins for `tools/ulacheck.c`;
  `ZXHW_CONTENTION=1` prints every contended T-state and its delay;
  `ZXHW_TRACE=FROM:COUNT` prints the buses at every half period of COUNT
  T-states from T-state FROM.
- `C64HW_CHECK=1` checks every KERNAL read and every VIC fetch against the
  ideal memory map.
- `--record 900:DIR --frames 90` (graphical mode) saves the window 90
  times, every 4 emulated frames from frame 900 on. This is how the GIF
  above was made.
- `--inspect U8`, `--browse`, `--view X,Y,ZOOM`, `--mouse X,Y`,
  `--down X,Y@F`, `--up X,Y@F`, `--key NAME@F` and `--chars TEXT@F`
  (graphical mode) open the inspector or the file browser, aim the board
  view, click the mouse and press keys at UI frame F, for screenshots and
  tests of the interface.

`tests/faults.sh` runs 31 common faults. The expected symptom of each one
was written down before the simulator was run.

`tests/diag.sh` runs the diagnostic cartridges and checks what they report
against their manuals: Dead Test's flash code for every DRAM, the 586220
with its harness on a healthy board and with faults. It takes a few
minutes, so it runs only when you start it. `--flashes` (headless) counts
Dead Test's flash series.

## How it works

`tools/kicad2c.py` turns bwack's KiCad replica of the 250407 into C tables:
216 parts, their pads, and 249 nets. It does the same for the Spectrum's
Issue 3B.

Every pin drives its net strongly, weakly (a pull-up), from a supply short,
or not at all. The strongest drive wins. Two equal drives fighting are a
contention, and low wins.

A net that nobody drives keeps its charge, and then drifts:

- to about 1.4V within microseconds, if TTL inputs load it;
- toward 0V in a fraction of a second, if only MOS inputs are on it.

MOS inputs compare that voltage, plus some noise, with their own threshold,
so a floating line can read erratically.

Power faults depend on the chip:

- A chip without VCC is dead.
- A chip without GND lets its outputs float high.
- A CMOS chip keeps running on phantom power, which reaches it through the
  protection diodes of its pins.

The noise is seeded, so a run can be repeated exactly.

`src/parts.c` models these parts at their pins:

- the glue logic;
- the ROMs;
- the DRAMs, which latch the row on /RAS and the column on /CAS;
- the color RAM;
- the PLA, with its real logic equations.

The 6510, VIC-II, CIAs and SID are cycle-accurate cores from floooh's
chips, with a few additions (marked in their headers): the CIAs' time of
day clock, serial port and /PC handshake, the SID's paddle registers. In
`src/board.c` each core sits in a socket that sees only its pins.
Each half cycle runs in three steps: the row address, then /RAS falls,
then /CAS falls. The whole board settles after each step.

On the Spectrum everything is timed by the ULA's oscillator: the board
settles at every half period of it, eight times per CPU T-state. The ULA
steps, the board settles, and the Z80 gets an edge of its clock if there
was one. Then the memories see their strobes, and the board settles
again.

PLAN.md explains the reasoning behind these choices.

## Limits

It is slow, between a third and half of real time on the computers listed
above (a fifth to a third for the Spectrum), because every pin of every
chip is simulated.

The floating and power models have the right shape, but their numbers
(thresholds, drift times, crosstalk) are guesses. They are constants in
`src/sim.c`.

These analog parts are not simulated: the video output, the SID filter,
the power supply, and the dot clock PLL. On the Spectrum: the video
modulator, the sound, and the tape input. Three of them are modeled by what
they do, because the logic depends on them: the 50 Hz from the mains that
drives the time of day clocks, the cassette motor driver, and the SID's
paddle A/D converters (the time the 1800 pF capacitor takes to charge
through the paddle).

Faults inside a chip, such as bad DRAM cells or slow parts, and bridges
between two nets are planned but not done.
