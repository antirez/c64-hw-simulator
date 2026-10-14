# c64-hw-sim — Plan

A board-level, pin-accurate simulator of the Commodore 64 **ASSY 250407**
("breadbin") motherboard, built on top of the `../c64-llm` emulator, whose
purpose is **simulating broken machines**. Put a pin in the open, short it
to GND, kill a DRAM, pull a chip out of its socket, make the PLA slow... and
see the screen and behavior a real broken C64 would show.

The core idea is to stop treating the C64 as "CPU + memory map + chips" and
instead treat it as what it physically is: **32 ICs, 644 IC pins and ~250
nets**. The netlist is data, taken from a real board design. The glue chips
(PLA, multiplexers, latch, decoders, gates, analog switches) and memories
(ROMs, DRAMs, color SRAM) become real pin-level parts. The big chips (6510,
VIC-II, CIAs, SID) keep their existing cycle-accurate cores but get a thin
electrical "socket" around them. Symptoms are never special-cased: they must
**emerge** from the netlist plus the part models.

## Status (v1, October 2026)

A first version is implemented (see README.md):

- the netlist generated from the KiCad replica;
- pin-level models of all the glue chips and memories;
- electrical sockets around the 6510, VIC-II, CIA and SID cores;
- the GUI (board, fault menus, monitor, scope);
- headless testing (`tests/faults.sh`).

Simplifications compared to this plan:

- Drive strengths with low-wins contention instead of the conductance model.
- Floating inputs: hold, drift and noise as in §4.1, with a single
  pseudo-voltage per net and guessed constants; lifted MOS legs add
  crosstalk from their package neighbours. Open power pins follow the chip
  technology (NMOS/TTL off without VCC, outputs up without GND, CMOS on
  phantom power).
- No DRAM cell, bridge or delay faults in the UI yet.
- Speed is about 35% of real time.

---

## 1. Goals, non-goals, principles

### Goals

1. Healthy board: boots and runs like `c64-llm` (and like a real C64),
   cycle for cycle, except where the old model was wrong (see §8.3: open bus,
   color RAM high nibble, etc.).
2. Faults: any IC pin or net can be opened, stuck at 0/1, bridged to another
   net, weakened. Any chip can be removed or marked dead. Memories can have
   cell, row, column or bit faults. Glue logic can have internal faults
   (PLA product terms, a single gate). There are timing faults (slow chip)
   and intermittent/thermal faults.
3. Observability like a repair bench: a logic probe (H/L/pulsing/floating/bad
   level) on any pin, logic analyzer capture to VCD, "what does the monitor
   show", per-net contention counters, and the Dead Test / Diagnostic
   cartridges.
4. Everything drivable from the existing LLM socket protocol, so an agent (or
   a human) can do "broken C64 in, diagnosis out" experiments, and from
   scenario files for regression tests.

### Non-goals (for now)

- Analog video (RF modulator, luma/chroma levels, sync amplitudes), PSU
  analog behavior, SID analog filter defects. These get only *coarse
  symptom-level hooks* (§9).
- Continuous-time timing simulation. We use discrete sub-cycle evaluation
  points; timing faults are approximated (§4.4, §9).
- C64C boards (250466, 250469 with the 251715 SuperPLA, 8565/8580, 41464 DRAM),
  NTSC (6567), the 1541 drive. The design must not prevent them (§10, M9).

### Principles

- **No symptom hacks.** If "PLA dead → black screen" happens, it must
  be because the CPU really fetched garbage from a floating bus at $FFFC, not
  because some code said so.
- **Netlist is data.** Board wiring lives in a checked-in netlist file
  generated from a KiCad design and cross-checked against the Commodore
  schematic. The C code knows part *types*, not board wiring.
- **Electrical levels at chip boundaries.** Inside the chip cores we keep
  floooh's logical pin flags (`M6502_RDY` = "halt", `M6502_IRQ` =
  "asserted"). At the socket boundary everything is converted to physical
  levels (RDY high = run, /IRQ low = asserted, AEC high = CPU owns the bus).
- **Deterministic.** Power-on state, intermittent faults, noise on floating
  inputs: all come from a seeded PRNG. A scenario + seed always reproduces.
- **Correctness first, speed second.** A batch fault run at 0.2× realtime
  is fine. Interactive realtime is a later milestone (M8).

---

## 2. Starting point: what `c64-llm` gives us and where it stops

`c64-llm` is floooh's `chips` C64 (zlib license), with notable accuracy work
by us: VIC timing and sprite crunch for *Nine*, IRQ timing during stalled
fetches, SID waveforms, and a powerful harness (SDL + LLM socket, screenshots,
breakpoints, profiler, `--bas`).

| Area | Today in `c64-llm` | Gap for hardware simulation |
|---|---|---|
| CPU `m6502.h` | Cycle-stepped 6510, 64-bit logical pin mask, IO port via callbacks with `io_pullup`/`io_floating` hacks | AEC is ignored (no tri-state). RES is a sticky edge, not a level. Port pins are not electrical. The data input is whatever glue code wrote |
| VIC `m6569.h` | Cycle-accurate renderer, BA/AEC as logical flags, memory via `fetch_cb(addr14) → data12` | No row/column address pins, no RAS/CAS, refresh cycles (11–15) are not real accesses, phi1/phi2 accesses collapsed into one tick, no PHI0 output, reset exists (the real 6569 has **no reset pin**) |
| CIA `m6526.h` | Timers, ICR, ports as `reg \| (inp & ~ddr)` | Port drive strength (strong low / weak high), PB6/PB7 as real outputs, **TOD not implemented** (pin defined, unused), SP/CNT not implemented, clocking not tied to PHI2 |
| SID `m6581.h` | Good audio core, bus via CS/RW | POT inputs not tied to paddles/4066, clock not tied to PHI2 |
| Glue `c64.h` | `if` chain address decoder, `mem_t` page tables, color RAM as an array, VIC bank from CIA2 PA in code | **Everything here is replaced** by the netlist + part models |
| Keyboard `kbd.h` | Host key → matrix bitmask | Becomes a switch matrix between CIA1 PA/PB nets (ghosting and stuck keys emerge) |
| Harness `c64-llm.c` | SDL, LLM socket, TEXT/MEMGET via `mem_rd` | Memory inspection must become explicit "backdoor" access (§4.6); add fault/probe commands |

What we keep untouched in spirit: the CPU instruction core, the VIC
graphics/sprite/border pipeline and its timing state machine, CIA timer/ICR
logic, SID synthesis, the SDL frontend, the LLM protocol and the tests.

---

## 3. Target hardware: ASSY 250407 (PAL)

### 3.1 IC inventory and how each one is simulated

| Ref | Part | Function | Sim model |
|---|---|---|---|
| U1 | 6526 CIA #1 | keyboard, joysticks, paddle select, IRQ | `m6526` core + electrical socket |
| U2 | 6526 CIA #2 | VIC bank (/VA14,/VA15), IEC, user port, NMI | `m6526` core + electrical socket |
| U3 | 2364 (901226-01) | BASIC ROM | pin-level ROM |
| U4 | 2364 (901227-03) | KERNAL ROM | pin-level ROM |
| U5 | 2332 (901225-01) | character ROM | pin-level ROM (CS1 = /CHAROM, CS2 tied +5V) |
| U6 | 2114 | color RAM 1K×4 | pin-level SRAM |
| U7 | 6510 | CPU | `m6502` core + electrical socket |
| U8 | 7406 | hex inverter, open collector | TTL part |
| U9–U12, U21–U24 | 4164 | DRAM 64K×1 (×8) | pin-level DRAM (RAS/CAS) |
| U13, U25 | 74LS257 | CPU address → DRAM multiplexer | TTL part |
| U14 | 74LS258 | VIC A6/A7 + bank bits multiplexer (inverting) | TTL part |
| U15 | 74LS139 | I/O chip-select decoder | TTL part |
| U16 | 4066 | color RAM nibble ↔ D0–D3 switch | bidirectional switch |
| U17 | 82S100 / 906114-01 | PLA | fuse-map model (JEDEC) |
| U18 | 6581 | SID | `m6581` core + electrical socket |
| U19 | 6569 (PAL) | VIC-II, also the **clock master** | `m6569` core, refactored bus interface |
| U20 | 556 | power-on reset + RESTORE monostable | behavioral timer |
| U26 | 74LS373 | latch: VIC row address → A0–A7 | TTL part |
| U27 | 74LS08 | CAEC, RDY, color RAM /CS, TOD shaping | TTL part |
| U28 | 4066 | paddle multiplexer | bidirectional switch |
| U29 | 74LS74 | clock: ÷2 | clock block |
| U30 | 74LS193 | clock: color clock ÷9 (PAL preload) | clock block |
| U31 | 74LS629 | dot clock VCO | clock block |
| U32 | MC4044 | PLL phase detector | clock block |

The 250425 is identical except that the PLL (U29–U32) is replaced by the
8701 clock generator. It is a cheap later variant (M9).

### 3.2 Board facts verified from the netlist

These were extracted from the open KiCad replica of the 250407 (§3.4) and
match the Commodore documentation where we could check them. They are the
non-obvious parts of the machine, and the reason a netlist-level simulation
reproduces real symptoms that the `c64.h` glue cannot.

**Bus ownership (AEC).** The VIC's AEC (pin 16) is low when the VIC owns the
bus: every phi1, plus stolen phi2 cycles on badlines and sprite DMA.

- U8 (7406) gate 2: AEC → **/AEC** (pin 4). /AEC drives the PLA (pin 25,
  "I10") and the /G enables of U13/U25. *A dead U8 gate blacks the screen.*
- U27 (74LS08): **CAEC = AEC · /DMA** → CPU AEC (pin 5).
  **RDY = BA · /DMA** → CPU RDY (pin 2).
  **color RAM /CS = AEC · /COLOR** → U6 pin 8, so the color RAM is always
  selected during VIC cycles.
  The fourth gate shapes 9 VAC into the **50 Hz TOD** clock (CIA pin 19).
- U26 (74LS373): D = VA0–VA7, Q = A0–A7, **LE = /RAS**, **/OE = AEC**.
  During VIC cycles it latches the VIC's row address and drives it onto the
  CPU address bus, so the char ROM, color RAM and decoders see a static
  address.
- During VIC cycles **A8–A11 come straight from the VIC pins** (32–34, 23).
  **A12–A15 are driven by nobody**: they read high through RP4 (3K3).
- U13/U25 (74LS257): **S = /CAS**, **/G = /AEC**. Row phase (/CAS high) puts
  A0–A7 on VA0–VA7; column phase puts A8–A15. So for the CPU, **DRAM row =
  A0–A7 and column = A8–A15**. A dead column hits a whole 256-byte page; a
  dead row hits the same offset in every page.
- U14 (74LS258, inverting): **S = /CAS**, **/G = AEC** (enabled only in VIC
  cycles). Sections 3 and 4 have both inputs tied and work as inverters on
  the VIC's A6/A7 (nets VA6'/VA7'). Sections 1 and 2 output VA6/VA7: in the
  row phase that is VIC A6/A7; in the column phase it is the inverted bank
  bits (/VA14,/VA15 from CIA2 PA0/PA1, giving VA14/VA15).
- VA0–VA7 reach the DRAM address pins (MA0–MA7) through RP2 (33 Ω).
  /CASRAM reaches the DRAMs through R42 (82 Ω). These series resistors are
  "openable links" for faults.

**PLA (U17).**

- Inputs: /CAS (pin 9, from the VIC); /LORAM, /HIRAM, /CHAREN (pins 8, 7, 6,
  3K3 pull-ups); /VA14 (pin 5); A12–A15; BA (pin 26); /AEC (pin 25); R/W
  (pin 24); /GAME, /EXROM (3K3 pull-ups).
- Pins 20/21, called "VA12/VA13" in the equations, are physically wired to
  **VA4/VA5**. So they carry A12/A13 only during the column phase. This is
  why we need sub-cycle evaluation points (§4.4).
- /OE (pin 19) is grounded; FE (pin 1) is not connected.
- Outputs: /ROMH (10), /ROML (11), /I/O (12), GR/W (13, color RAM /WE),
  /CHAROM (15), /KERNAL (16), /BASIC (17), /CASRAM (18).
- The I/O product terms include **BA** for CPU reads. I/O is not selected
  while the CPU is being stalled, so a repeated stalled read of a CIA ICR
  has no side effects. Today `c64.h` special-cases this in code. Here it
  emerges from the PLA.

**U15 (74LS139) I/O decoding.**

- Half 1: /G = /I/O, inputs A10/A11 → /VIC, /SID, /COLOR, /CIAS.
- Half 2: /G = /CIAS, inputs A8/A9 → /CIA1, /CIA2, /I/O1, /I/O2 (to the
  expansion port).

**Color RAM.**

- U6 (2114) A0–A9 come from the system bus. /WE = PLA GR/W.
- Its I/O1–4 connect to the VIC's D8–D11 (pins 35–38). They reach the CPU's
  D0–D3 **only through U16 (4066), whose four controls are AEC**.
- Consequences:
  - A CPU read of $D800 leaves D4–D7 undriven. The upper nibble is whatever
    charge is left on the bus.
  - A dead U16 means the CPU can't write colors while the VIC still reads
    them.

**DRAM bit map.**

- D0=U21, D1=U9, D2=U22, D3=U10, D4=U23, D5=U11, D6=U24, D7=U12.
- Each 4164 has D (pin 2) **and** Q (pin 14) on the same data line, so a
  shorted Q kills that data bit for the whole system, ROM fetches included.
- WE = R/W, /RAS straight from the VIC, /CAS = /CASRAM'.

**Pull-ups and floating nets.**

- These have 3K3 pull-ups: A12–A15, /IRQ, /NMI, /GAME, /EXROM, /DMA,
  /LORAM, /HIRAM, /CHAREN (R45, R44, R43). R/W has 1K5 (R51), so it reads
  high (read) while the CPU is tri-stated in phi1.
- The keyboard row/column nets have no board pull-ups; they rely on the
  CIA's internal ones.
- **D0–D7 have no pull-ups.** The data bus is a capacitor: undriven, it
  holds the last driven value. That is the "open bus" behavior (reading
  $DE00 returns the VIC's last phi1 fetch).

**Resets and interrupts.**

- /RESET is driven by U8 (open collector) from U20 (556), with a 1K pull-up.
  It goes to the CPU, both CIAs, the SID, the user port, IEC and the
  expansion port. **The VIC has no reset pin.**
- /NMI is wired-OR: CIA2 /IRQ, plus RESTORE → U20 → U8, plus the expansion
  port.
- /IRQ is wired-OR: VIC, CIA1, expansion port.

**Clocks.**

- The dot clock comes from U31 (74LS629 VCO) in a PLL: U32 (MC4044) compares
  the VIC's PHI0 against the color crystal ÷9 (U30) ÷2 (U29).
- The VIC divides the dot clock by 8 to make **PHI0** (pin 17 → CPU pin 1).
- The **CPU's PHI2 output** (pin 39) clocks both CIAs, the SID and the
  expansion port.
- Consequences: a dead VIC stops everything; a dead CPU also stops the CIA
  timers.

**IEC.**

- CIA2 PA3/PA4/PA5 drive ATN/CLK/DATA through U8 (open collector).
- PA6/PA7 read CLK/DATA back.

**CIA1 ports.**

- PA = keyboard columns, control port 2 (CN8), and paddle select (PA6/PA7 →
  U28).
- PB = keyboard rows and control port 1 (CN9). Port 1 fire (PB4) is also the
  VIC LP input.
- FLAG = cassette read (also IEC SRQ).

**ROM pins.**

- 2364: pin 20 = /CS, pin 21 = A12.
- 2332 (char ROM): pin 20 = /CS1 (/CHAROM), pin 21 = CS2 tied to +5V.

### 3.3 Anatomy of one bus cycle, and why 4 evaluation points per cycle

One PAL cycle (≈1.015 µs) has two halves: phi1 (PHI0 low, VIC) and phi2
(PHI0 high, CPU, or VIC when it steals). In **each** half the VIC generates
a full DRAM cycle: **/RAS falls** (row address), then **/CAS falls** (column
address), then both rise.

So each half has two electrically distinct moments:

- **R (row):** /RAS low, /CAS high.
  - U13/U25/U14 select their row inputs.
  - U26 latches (LE = /RAS falling).
  - The PLA's pins 20/21 see A4/A5.
- **C (column):** /CAS low.
  - The muxes select columns.
  - The PLA's pins 20/21 now carry A12/A13.
  - /CASRAM may go low, the DRAM drives Q or latches D, and ROMs/I/O respond.
  - The consumer (VIC at the end of phi1, CPU at the end of phi2) samples the
    data bus.

This gives four evaluation points per cycle: **φ1R, φ1C, φ2R, φ2C**. That
is ≈3.94 M evaluation points per emulated second. It is the minimum
granularity at which these behaviors appear naturally:

- the CHAROM decode for the VIC;
- the 373 latching;
- DRAM addressing;
- CASRAM gating;
- "slow PLA" faults.

### 3.4 Netlist source and caveats

- Source: [bwack/C64-250407-Replica-KiCad](https://github.com/bwack/C64-250407-Replica-KiCad)
  (CERN OHL v1.2).
- A ~40-line Python script over `250407_.kicad_pcb` (pad → net) already
  extracts **216 components, 32 ICs, 644 IC pins, 249 nets**. Every fact in
  §3.2 came from that extraction.
- **Caveat:** the author says the schematic was derived from the 250407 Rev.B
  schematic and *modified to match the KU-14194HB layout*. We already see one
  revision difference:
  - The netlist puts CAEC on U27 pin 6, RDY on pin 8 and color CS on pin 11.
  - A scope reference measured on a real board lists CAEC on pin 3, RDY on
    pin 11 and color CS on pin 6.
- The logic is the same, but **pin numbers matter** for faults like "U27
  pin 11 stuck". So the netlist is per-revision data, and M0 includes
  cross-checking it against the Commodore schematic 251138 (250407 Rev A/B)
  and the service manual 314001.

---

## 4. Simulation architecture

### 4.1 Signal model

Each net has a resolved **level** and **strength**.

| Strength | Who produces it |
|---|---|
| `SUPPLY` | Shorts to GND/+5V (faults), tied pins (2332 CS2 → +5V), connector pins tied to GND (joystick switches, harness loopbacks to GND) |
| `STRONG` | Push-pull outputs: TTL totem-pole, NMOS outputs, DRAM Q, ROM data, CIA port low |
| `PULL` | Pull-up resistors (3K3, 1K), NMOS depletion loads, CIA port "high" (weak, so a key or joystick can pull it low), the open-collector "off" state with its pull-up |
| `CHARGE` | Nobody drives the net: it keeps its previous level (bus capacitance) |

**Resistors in the netlist.** Every resistor is classified when the netlist
is generated.

- **To a rail (pull-up/pull-down):** a `PULL` driver with its real
  conductance. On the 250407 every such resistor on a digital net is a
  pull-up; there are no digital pull-downs (the model still supports them,
  for cartridges and harnesses).
  - RP4 3K3 bussed: A12–A15, /IRQ, /GAME, /EXROM, /DMA.
  - RP3 3K3 bussed: /NMI, cassette read (CIA1 FLAG), /FLAG2, CNT1/2,
    SP1/2.
  - R43–R45 3K3: /CHAREN, /HIRAM, /LORAM.
  - R51 1K5: R/W.
  - R36 1K: /RESET (U8 open collector).
  - R31 1K: /AEC (U8 open collector).
  - R28–R30 1K: IEC ATN/DATA/CLK.
  - R1 3K3: cassette sense.
  - R41 1M: RESTORE.
- **Bussed packs** share one common pin. A fault on it (`RP4.1 open`) drops
  all of the pack's pull-ups at once.
- **Series resistors** (RP1/RP2 33 Ω VA→MA, R42 82 Ω /CASRAM) are links
  (§5).
- **Analog resistors** (556 timing, PLL loop filter, TOD shaping, video, SID,
  cassette transistor bias) are absorbed into behavioral parts.
- **Pull-ups inside chips** (CIA port lines) belong to the chip models.

**Resolution by conductance.**

- Each active driver is a (voltage, conductance) pair:
  - a supply short ≈ 0 Ω;
  - a push-pull output: tens to hundreds of Ω, asymmetric (NMOS/TTL pull
    down harder than they pull up);
  - a pull-up: its real value;
  - a resistive bridge fault: its value.
- The net voltage is `ΣGᵢVᵢ / ΣGᵢ`. With no active driver
  (`ΣG = 0`) the floating model below applies.
- The strength classes in the table are just conductance ranges.
- Contention becomes a voltage divider. "Low wins" emerges from the
  asymmetry instead of being a rule, and a fight can land in the undefined
  band where receivers disagree.
- Every contention is counted, so the probe can report "BAD LEVEL", like
  the "2 V levels are bad" a scope shows.
- Nets with a single active driver (the vast majority) skip the arithmetic.

**Floating nets: a decaying pseudo-voltage, not a fixed rule.**

The physics is "it holds for a while, then drifts, and near the threshold it
reads randomly". So every net carries a pseudo-voltage `v`:

- **Driven:** `v` = 0 V or 5 V. Under contention it gets an intermediate
  value derived from the strengths involved.
- **Undriven:**
  - `v` keeps its value and drifts exponentially toward a rest voltage
    `v_rest` with time constant `τ`, plus noise `σ`. All three come from the
    net class (what is attached).
  - A net with LS-TTL inputs on it rests around 1.3–1.5 V (the open-input
    voltage of TTL).
  - A pure-MOS net leaks toward ~0 V with `τ` in the 100 ms range.
  - An open pin's internal node (tiny capacitance) has a short `τ`.
- **MOS/CMOS receivers** (6510, VIC, CIA, SID, ROM, DRAM, 2114, 4066 control,
  and the CSG 906114-01 PLA, which is NMOS) read `v + noise > Vth`.
  - `Vth` comes from the technology, plus a per-instance offset from the
    seed.
  - Near the threshold the reads are therefore random but reproducible, and
    two chips on the same net can disagree.
- **LS-TTL/bipolar receivers** (glue chips, the 82S100) read an undriven net
  as **1**. That comes from the input structure, not a voltage compare. An
  optional small glitch probability covers the fact that they are
  noise-sensitive.

Three regimes fall out of this single mechanism:

1. **Short float (< ~1 µs): hold, deterministic.** This is the dominant
   C64 case, because the data bus is re-driven every half-cycle:
   - open bus ($DE00 = the VIC's phi1 fetch);
   - the color RAM high nibble;
   - a dead DRAM Q pin;
   - an open CPU address pin that holds what U26 drove in phi1.
2. **Long float with a clean rest level: deterministic decay.** Example: the
   6510 port bits 6/7 keep their last output value, then fall to 0 after
   ~350 ms. VICE uses 350,000 cycles and notes the value depends strongly on
   the chip and on temperature.
3. **Long float near the threshold: erratic.** Reads are sometimes 0,
   sometimes 1, and receivers disagree. Cases: lifted NMOS input legs, a
   floating 4066 control (the switch may also sit half-on, modeled as a
   resistive switch), a net whose only driver was removed.

`τ`, `v_rest` and `σ` are guesses until calibrated against:

- the VICE open-bus tests;
- the 6510 fall-off;
- real-board measurements (scope a line after pulling the chip that drives
  it, probe a lifted leg).

The extra cost only applies to nets that are actually undriven or
contended.

Example: "CIA2 pulled from its socket" still gives a bank-0 screen.
/VA14 and /VA15 float, and their only receivers (U14 74LS258, the 82S100 PLA)
are TTL/bipolar, so they read 1. On a board with the NMOS CSG PLA, the PLA
side of that net would instead behave as in regime 3.

**Bidirectional switches** (4066, keyboard keys, joystick contacts, harness
loopbacks, bridge faults) merge nets. The merge is done with union-find,
rebuilt only when a switch changes state. A merged group resolves as one
net. Closed-switch on-resistance can be modeled by demoting `STRONG` to
`PULL` across a "resistive" switch (used for bridge faults that are not
dead shorts).

### 4.2 Net engine

- Parts are evaluated in **levelized order** computed from the netlist:
  sources (clock block, VIC/CPU outputs) → glue → memories → data bus
  consumers.
- At each evaluation point the engine iterates until no net changes (cap ~8
  passes). Fault-created loops that don't settle are flagged as
  **oscillation**: the nets go X and are resolved by policy and counted.
- Bus nets (A0–A15, D0–D7, VA0–7, MA0–7, color nibble) are represented as
  packed words with per-bit strength masks. The common case then needs a
  handful of bitwise operations instead of per-bit loops.

### 4.3 Scheduler and clocks

- Clock nets are real nets: DOT_CLOCK, PHI_COLOR, PHI0, PHI2, /RAS, /CAS.
- Parts declare what they react to:
  - Combinational parts react to any input change.
  - Edge parts react to specific edges: 373 LE, DRAM /RAS and /CAS, 74LS74,
    74LS193, CIA/SID on PHI2, CPU on PHI0, VIC on DOT_CLOCK.
- In practice the clock block emits DOT_CLOCK in 8-tick bursts. The VIC
  core still runs once per cycle internally and is the authority for its
  PHI0/RAS/CAS/AEC/BA outputs at the 4 evaluation points.
- If DOT_CLOCK stops (dead clock block), the VIC doesn't advance, there is
  no PHI0, and nothing runs. The video output shows "no sync" (§7).

### 4.4 Part interface (sketch)

```c
typedef struct part part_t;
typedef struct {
    const char *type;                      // "74LS257", "DRAM4164", ...
    int npins;
    const pin_desc_t *pins;                // datasheet name, direction, receiver kind
    void (*eval)(part_t *p, sim_t *s);     // read input levels, set output drives
    void (*edge)(part_t *p, sim_t *s, int pin, int rising);
    void (*power_on)(part_t *p, sim_t *s, rng_t *r);
} part_type_t;

struct part {
    const part_type_t *type;
    const char *ref;                       // "U17"
    net_id_t *pin_net;                     // pin index -> net (after faults)
    drive_t *drive;                        // pin index -> {level, strength} or Z
    uint32_t delay;                        // 0 = normal, k = outputs lag k eval points
    void *state;                           // DRAM cells, ROM image, core state...
};
```

**Timing faults.** A part with `delay = k` publishes the outputs it computed
k evaluation points ago. This is a crude but useful propagation-delay model:

- A slow PLA computes /CASRAM in φ1C from φ1R inputs (still /CAS high), so
  the DRAM never gets CAS in that half.
- A slow DRAM drives Q too late for the sampling point, so the consumer reads
  the bus charge (the previous value).

### 4.5 Sockets around the existing chip cores

Each existing core gets an adapter part that converts between electrical
pins and the core's logical 64-bit mask, and that **owns tri-state**.

**6510 (U7).**

- Call `m6502_tick` at the start of φ2 to get address, R/W and write data.
- Drive A0–A15, R/W and D0–D7 only if CAEC is high (otherwise Z).
- The data input is the resolved D0–D7 at the end of φ2C.
- RDY electrical high → clear the logical `M6502_RDY` (inverted polarity).
- /IRQ and /NMI are active low (NMI edge detect stays in the core).
- /RESET becomes a **level**: the CPU is held while low, and the reset
  sequence runs on release. Today RES is a sticky "request" bit.
- The P0–P5 port pins drive per DDR (strong both ways). DDR=0 means Z, so
  the board pull-ups give LORAM=HIRAM=CHAREN=1 at reset. This replaces the
  `io_pullup` / `io_floating` hacks.
- PHI2 output pin 39 follows PHI0 while the CPU is alive.
- A CPU removed or dead → no PHI2 → the CIAs and SID stop.

**6569 VIC (U19)** is the biggest refactor:

- Every access becomes an explicit bus transaction: c, g, p, s, i and the 5
  refresh accesses per line (cycles 11–15, address $3Fxx with the 8-bit
  refresh counter). Refresh is currently skipped; it matters for open-bus
  values and for the DRAM refresh model.
- Each transaction knows its half (phi1, or phi2 when stolen) and its kind.
- The adapter turns the 14-bit address into pin drives per sub-step:
  - φR: VA0–VA5 = A0–A5, VA6'/VA7' = A6/A7, A8–A11 static.
  - φC: VA0–VA5 = A8–A13.
- The data returned to the core is the resolved D0–D7 plus D8–D11 at the end
  of the C point.
- Register reads and writes become a separate call driven by the resolved
  /VIC, R/W and A0–A5 in φ2.
- The VIC drives D0–D7 only on register reads.
- Outputs:
  - **real-polarity AEC** (low = VIC);
  - BA;
  - /IRQ open drain;
  - PHI0, /RAS, /CAS as nets.
  The core's `M6569_AEC` is today a logical "VIC steals" flag (inverted).
- **No reset.** `power_on` initializes registers (zeros by default, random
  as an option).
- LP input comes from the BTNA/LP net.

**6526 CIA (U1, U2).**

- Ports become electrical: DDR=1 and bit 0 → STRONG low; DDR=1 and bit 1 →
  PULL high; DDR=0 → PULL high (internal pull-up).
- PB6/PB7 timer outputs when enabled.
- Reads return actual pin levels (already the core's semantics).
- /IRQ is open drain.
- Ticks on PHI2.
- **Implement TOD**, driven from the U27 50 Hz net. It is needed for the
  Diagnostic cartridge and for "U27 TOD gate dead" faults.
- SP/CNT: initially a stub with correct electrical pins.
- **To verify:** the exact 6526 port output structure (pull-up strength),
  against the datasheet and VICE's keyboard/port model.

**6581 SID (U18).**

- Bus adapter (A0–A4, /CS, R/W, D0–D7), /RESET, ticks on PHI2.
- POTX/POTY from paddle models through U28. Paddles become RC "charge time"
  values selected by the switch state.

### 4.6 Backdoor vs bus access

`c64-llm` features like `MEMGET`/`MEMSET`, `TEXT`, quickload and `--bas`
currently use `mem_rd`/`mem_wr`. In a broken machine, "the byte at $1234"
is ambiguous. So we split two things:

- **Bus access**: what the CPU actually sees. Used by breakpoints, traces
  and the CPU itself.
- **Backdoor access**: direct reads and writes of chip storage (DRAM cell
  arrays, ROM images, color SRAM, chip registers), using the *healthy*
  address mapping.
  - CPU address → row = A0–A7, column = A8–A15, bit *n* in the chip that owns
    Dn.
  - Quickload, `MEMGET` and `TEXT` use the backdoor and say so.
  - A new `BUSREAD` performs a real (side-effecting) bus read.

### 4.7 Power-on and determinism

`POWER` (cold start) and `RESET` are different things. `RESET` asserts
/RESET through the 556 path.

`POWER` reinitializes:

- **DRAM**: today's 64×$00/64×$FF pattern by default; other patterns or
  random as options.
- **VIC registers**: zero by default.
- **Floating nets**: initial charge.

All randomness (power-on, noise, intermittents) comes from one seeded PRNG
that is part of the scenario.

### 4.8 Performance

- Realtime is 985,248 cycles/s × 4 evaluation points ≈ 254 ns per point on
  one core. Approach:
  1. **M1–M5:** a straightforward interpreter; measure.
  2. Packed bus words.
  3. **Event-driven evaluation**: re-evaluate a part only if one of its input
     nets changed. Most glue is quiet most of the time.
  4. **Per-part fast paths when no fault touches the part.**
     - The PLA is a **64 KB lookup table** (16 inputs → 8 outputs), rebuilt
       from the fuse map whenever a PLA fault changes.
     - ROM/DRAM/SRAM read paths are direct array indexing.
  5. If needed, a code generator that emits straight-line C for the healthy
     board, with fault hooks as masks.
- Target: ≥1× realtime on a healthy board on Apple Silicon. Faulty boards may
  be slower.

---

## 5. Fault model

Faults are attached to **pins**, **nets**, **links** (series resistors,
ferrites, traces) or **parts**. They are composable, can be enabled and
disabled at runtime, and are reproducible with the seed.

| Class | Fault | Semantics |
|---|---|---|
| Pin | `open` | Pin disconnected from its net (lifted leg, cracked joint, bad socket contact). The chip's input becomes its own tiny floating node: TTL/bipolar reads 1; MOS follows the §4.1 floating model, with a short `τ`, so it soon reads erratically. The chip's output no longer drives the net |
| Pin | `stuck0` / `stuck1` | Pin shorted to GND/+5V: a `SUPPLY` driver on the net. It affects every chip on that net |
| Pin | `weak` | The output drives at `PULL` instead of `STRONG`, so it loses contention (degraded output stage) |
| Pin | `invert`, `noise p=` | Test and flaky behaviors |
| Net | `stuck0` / `stuck1` / `float` | Whole net forced, or every driver removed |
| Net | `bridge A B [resistive]` | Solder bridge or internal short between two nets: a static closed switch |
| Link | `open` | Series resistor or ferrite open (RP2 33 Ω on MA*n*, R42 on /CASRAM, FB on PHI0/PHI2...). The two sides become independent nets |
| Part | `removed` | Empty socket: all pins open |
| Part | `dead` | All outputs Z, inputs present ("no life") |
| Part | `dead-short` | All outputs stuck low (a chip that shorts its bus) |
| Part | `delay k` | Outputs lag k evaluation points (slow PLA, slow DRAM, marginal TTL) |
| Part | `after=T`, `until=T`, `period/duty`, `p=` | Thermal ("fails after 2 minutes warm") and intermittent activation for any fault |
| ROM | `flip addr bit`, `image file`, `swap U3 U4`, `blank` | Bit rot, wrong revision (e.g. KERNAL -02), swapped sockets, erased chip |
| DRAM | `cell addr stuck0/1`, `row r`, `col c`, `random rate`, `retention ms`, `coupling` | Cell, decoder and refresh faults. Row/column are in the chip's coordinates (CPU: row = A0–A7, column = A8–A15) |
| SRAM | `cell`, `bit` | Color RAM cell or bit faults |
| PLA | `pterm n open`, `pterm n stuck`, `fuse ...`, `output n polarity` | Fuse-level faults on the 48 product terms. A shared product term fails for **every output that uses it**: losing the VIC char-ROM term turns off CHAROM *and* stops excluding RAM, so the VIC reads DRAM at $1000 instead of floating |
| TTL | `gate n stuck0/1/open` | One section of a 7406/74LS08/139/257... |
| Core chips | `internal <unit> dead` (chosen case by case) | E.g. CIA timer A dead, CIA port bit dead, VIC sprite unit dead, SID voice dead, 6510 port bit dead |
| Clock | `dotclock off`, `colorclock off`, `pll unlocked` | No dot clock → nothing runs, no sync; no color clock → B/W picture (§7); unlocked → unstable picture (coarse) |
| Power | `rail12 off`, `rail5 low` | 12 V missing → VIC and SID dead (VIC dead = machine dead). Low 5 V → random bit errors (coarse) |

### Spec syntax

Used by CLI `--fault`, scenario files and the LLM `FAULT ADD` command:

```
U17.18 stuck1                 # PLA /CASRAM shorted to +5V
U21.14 stuck0                 # DRAM U21 Q (D0) shorted to GND
U9.2 open                     # DRAM U9 D input lifted
U26 dead                      # 74LS373 never drives
U17.CASRAM stuck1             # pins addressable by number or datasheet name
net:A5 bridge net:A6          # solder bridge
link:RP2.MA3 open             # series resistor open
U4 rom flip $1FFC bit 3
U4 rom image roms/kernal-901227-02.bin
U12 dram col $9F stuck0       # whole page $9Fxx bad in bit D7
U17 pterm 14 open
U17 delay 1                   # slow PLA
U1 internal timerA dead
U8.4 stuck1 after=120s        # thermal
net:D3 noise p=0.001          # flaky trace
U5 removed
```

---

## 6. Part-by-part work list

1. **Net engine** (`net/`): levels, strengths, union-find switches,
   resolution, levelized eval, iteration/oscillation, contention counters,
   decay, delay queues, fault application.
2. **TTL library** (`parts/ttl.h`): 7406 (OC), 74LS08, 74LS139, 74LS257,
   74LS258 (inverting), 74LS373 (latch + OE), 74LS74, 74LS193. Per-gate
   faults. Truth-table unit tests.
3. **4066 analog switch**: control → merge/split of nets. U16 (color RAM)
   and U28 (paddles).
4. **82S100 PLA**:
   - Fuse-map evaluation from the 906114-01 JEDEC (Mark Smith's read-out,
     with the ABEL equations in the same document). Both go in `roms/`.
   - 48 P-terms × (true/complement per 16 inputs), OR matrix, per-output
     polarity. /OE pin supported.
   - The healthy LUT is checked **exhaustively (2^16 inputs) against the ABEL
     equations**.
   - Faults: P-term, fuse, output stuck, delay.
5. **ROMs**:
   - 2364 (/CS, A0–A12) and 2332 (/CS1, CS2, A0–A11). Q is Z when not
     selected.
   - Images from `c64-roms.h`; faults as in §5.
6. **DRAM 4164 ×8**:
   - Row latch on /RAS fall, column latch on /CAS fall.
   - Early write (WE low at CAS fall: Q stays Z, D latched). Read: Q driven
     while /RAS and /CAS are low.
   - Any RAS cycle refreshes its row. Lazy decay per row with configurable
     retention (default ∞, i.e. off).
   - Power-on patterns; cell/row/column faults.
7. **SRAM 2114**: /CS, /WE, A0–A9, 4 bidirectional I/O pins.
8. **556 (U20)**: power-on reset pulse (ms scale, released after the
   configured time) and RESTORE monostable → U8 → /NMI. Faults: reset stuck
   active or never asserted, RESTORE stuck.
9. **Clock block** (U29–U32, behavioral): produces DOT_CLOCK and PHI_COLOR.
   Fault knobs for dot clock off, color clock off and unlocked. A per-part
   breakdown (e.g. "U30 dead" → no reference → PLL free-runs, still a dot
   clock but drifting) comes after the basics.
10. **Socket adapters**: 6510, VIC, CIA×2, SID (§4.5). Changes to the cores:
    - `m6502.h`: RES as a level, AEC awareness in the adapter, port as
      electrical pins.
    - `m6569.h`: bus-transaction API, refresh and idle accesses as real
      accesses, real AEC/RAS/CAS/PHI0 outputs, register access split out,
      power-on instead of reset.
    - `m6526.h`: electrical ports, TOD, PHI2-driven tick.
    - `m6581.h`: POT inputs.
11. **Connectors and peripherals**:
    - **Keyboard CN1**: 8×8 switch matrix between the PA/PB nets plus RESTORE
      (to U20). A host key press closes switches, so ghosting, "stuck key"
      and "shorted matrix line" faults are emergent. `kbd.h` keeps only the
      host key → matrix mapping.
    - **Control ports CN8/CN9**: joystick contacts to GND, fire buttons,
      paddles (resistance → POT time).
    - **Expansion port CN6**: cartridge models drive /GAME, /EXROM and
      respond to /ROML, /ROMH, /I/O1, /I/O2, plus /DMA, /IRQ, /NMI, /RESET.
      Generic 8K/16K/Ultimax cartridges, **Dead Test 781220** (Ultimax), and
      **Diagnostic 586220** (8K at $8000). Images are user-supplied.
    - **User port CN2, IEC CN4, cassette CN3**: electrical pins, plus
      "harness" definitions (loopback wiring as data) for the 586220. The
      datasette and the 1541 come later.
    - **Video CN5**: the VIC color index stream → renderer (§7).
12. **Board assembly** (`boards/250407.*`):
    - The netlist file (generated, checked in, hand-annotated with
      pin/receiver kinds).
    - Part instance list (types, images, PLA JEDEC).
    - The power-on configuration.
    - The healthy-board fast-path switches.
13. **Harness** (`c64-hw-sim.c`, forked from `c64-llm.c`): replace `c64_t`
    with the board, keep SDL, the LLM protocol and screenshots, and add the
    commands of §7.

---

## 7. Observability and interfaces

### Video output stage

- The VIC still produces a color index per pixel. A small "monitor model"
  turns it into RGB.
- If PHI_COLOR is missing → luma only (B/W picture, a real symptom).
- If the VIC isn't producing sync (dead, no dot clock, no 12 V) → a "no
  signal" frame (noise or black, configurable). This is distinct from "black
  screen with sync".

### Monitor OCR

- `SCREENTEXT` matches each 8×8 cell of the **rendered** picture against the
  known-good character ROM glyphs (both sets, reversed).
- It returns what a human would read on the monitor, not what is in screen
  RAM. This is essential for judging broken machines automatically.

### Symptom report

`STATUS` returns a structured summary:

- sync/chroma present;
- border/background colors;
- OCR text;
- cursor blinking (from frame-to-frame cell diffs);
- CPU state (running / halted by RDY / held in reset / JAMmed / looping,
  with a PC histogram);
- IRQ/NMI rates;
- sound RMS;
- per-net contention/oscillation counters.

This gives LLM agents and regression tests a vocabulary. Values: `NO_SIGNAL`,
`BLACK`, `BORDER_ONLY`, `READY`, `GARBAGE_CHARS`, `WRONG_COLORS`,
`NO_CURSOR`, `BW`, `FROZEN`, `RESET_LOOP`...

### Logic probe

`PROBE U27.8` reports `HIGH` / `LOW` / `PULSING f≈985kHz duty 50%` /
`FLOATING` / `BAD LEVEL (contention 12%)`, just like the real tool.

### Logic analyzer

`TRACE START <nets|pins...>` / `TRACE STOP file.vcd`, at the evaluation-point
resolution. Viewable in GTKWave, Surfer or PulseView, and comparable with
real scope captures.

### Inspection commands

- `NET <name>`: drivers, loads, level, strength, active faults.
- `PART U17`: all pins with their states.
- `PARTS`: inventory.

### Fault commands

- `FAULT ADD <spec>`, `FAULT DEL <id>`, `FAULT LIST`, `FAULT CLEAR`.
- `POWER` (cold boot), `RESET`, `SEED <n>`.
- `SCENARIO LOAD file` (faults + seed + cartridge + harness + expected
  symptoms).

### CLI

```
./c64-hw-sim --fault "U17.18 stuck1" --fault "U5 removed"
./c64-hw-sim --scenario scenarios/pla-casram-stuck.txt
./c64-hw-sim --cart deadtest.bin
```

### Repair-trainer mode (stretch)

`MYSTERY [difficulty]` picks a random fault from a curated list and hides
it. The user, or an agent, diagnoses with `PROBE`/`TRACE`/`STATUS` and swaps
chips (`SWAP U17`) until the machine works.

---

## 8. Validation

### 8.1 Part unit tests

- TTL truth tables, including Z/OE behavior.
- 4066 merges.
- 373 latch timing across R/C points.
- DRAM row/column/early-write/refresh sequences.
- **PLA: exhaustive comparison of the fuse-map evaluation vs the ABEL
  equations over all 65,536 inputs.**

### 8.2 Healthy-board differential testing vs `c64-llm`

- Add a tiny trace hook to `c64-llm` (one line per cycle: addr, R/W, data;
  plus a framebuffer hash per frame). `c64-hw-sim` emits the same format.
- Use the same DRAM power-on pattern and a deterministic start.
- A compare tool reports the first divergence.
- Run:
  - boot to READY;
  - `make test` equivalents (`tests/basic`, `cpu-timing`, `vic-timing`);
  - the Lorenz suite and VICE tests listed in `c64.h`;
  - *Nine* and *Rodents* (already used to validate VIC timing).
- Divergences must be either fixed or on a whitelist of known improvements
  (§8.3).

### 8.3 Emergent "healthy quirks" checklist

These must appear **without being coded explicitly**. Today `c64.h` gets
most of them wrong or hardcodes them.

- Reading $DE00–$DFFF (open I/O) returns the VIC's phi1 fetch of the same
  cycle.
- Reading color RAM: the upper nibble is the bus charge, also from the VIC.
- Writes to ROM areas land in the RAM underneath (CASRAM is asserted on
  writes).
- The VIC sees the char ROM at $1000/$9000 only in banks 0/2. Ultimax
  maps ROMH for the VIC at $3000 in every bank. This is how the Dead Test
  draws its own font.
- The 6510 port: unconnected bits, pull-up behavior at DDR=0, the
  cassette sense line.
- No I/O side effects on stalled CPU reads (the PLA BA term).
- Keyboard ghosting; joystick port 1 interfering with keyboard scanning.
- A CIA2 removed still gives a bank-0 screen.

### 8.4 Fault → symptom golden table (initial hypotheses)

These come from repair practice. Each one becomes a scenario file with
expected `STATUS` fields. Rows marked *verify* need confirmation on real
hardware or a solid reference before they become pass/fail tests.

| Fault | Expected symptom | Mechanism that must emerge |
|---|---|---|
| U7 CPU removed/dead | Black screen | No CPU writes; VIC power-on state; no PHI2 → CIAs stop |
| U17 PLA removed | Black screen | No chip selects; KERNAL vector read from a floating bus |
| U17 /CASRAM stuck high | Black screen | No RAM at all: stack and zero page broken |
| U17 /KERNAL stuck high | Black screen | Reset vector and code fetched from bus charge |
| U17 /CHAROM stuck high, or U5 removed | Boots, cursor blinks, garbage characters; blind typing works | VIC g-accesses read bus charge |
| U17 P-term "VIC CHAROM" open | Characters look like RAM patterns (stripes from the power-on pattern) | Shared P-term: CHAROM off **and** RAM no longer excluded |
| U5 one data pin open | Readable text with a column of pixels missing or doubled | Bit *n* of each glyph byte = pixel column 7−*n* |
| U6 color RAM removed/dead | Text in random, flickering colors | Floating D8–D11 |
| U6 one bit stuck | Colors wrong in a consistent way (e.g. all colors with bit 3 set) | Stuck nibble bit |
| U16 4066 stuck open | Colors don't follow writes: text in power-on colors (*verify*) | CPU writes can't reach color RAM; VIC reads still work |
| U26 373 dead | CPU fine (blind typing works); screen characters and colors scrambled | VIC low address for ROM/color RAM undriven |
| U13/U25 one output stuck | Reduced BYTES FREE or black screen, depending on the bit | DRAM aliasing for CPU accesses; KERNAL RAMTAS stops at the first failure |
| U14 output stuck | CPU fine; VIC shows the wrong memory (wrong bank / scrambled screen) | VIC DRAM addressing |
| U15 /CIA1 stuck high | READY screen, no cursor blink, dead keyboard | No CIA1 → no jiffy IRQ |
| U15 /VIC stuck high | CPU runs; screen stays at the VIC power-on state (black) | VIC never programmed |
| U27 RDY output stuck low | Black screen | CPU halted on its first read |
| U27 color RAM /CS stuck high | Random colors | VIC reads a floating color nibble; CPU can't access color RAM |
| U27 TOD gate dead | Boots normally; Diagnostic reports a TOD error | TOD never counts |
| U8 /AEC stuck low | Black or garbage screen | U13/U25 drive VA during VIC cycles: contention |
| U8 IEC DATA stuck | `LOAD"$",8` → `?DEVICE NOT PRESENT` or hang | Serial line held |
| U20 reset stuck active | Black screen | CPU/CIA/SID held in reset |
| U1 CIA1 removed | READY, no cursor blink, keyboard dead | No IRQ source, no keyboard |
| U2 CIA2 removed | READY screen normal (*verify*); IEC dead | /VA14,/VA15 float high at TTL receivers → bank 0 |
| U18 SID removed | Normal boot, no sound | — |
| U18 one data pin shorted (dead-short) | Black screen | Shared data bus |
| U21 Q pin shorted to GND | Black screen; even the Dead Test can't run correctly | D0 stuck for the whole system |
| U21 cell array dead (Q still driven) | Black screen; Dead Test: 1 flash (*verify*: flashes = bit+1, U21…U12) | RAM test fails in bit D0 |
| U12 column faults only in $C0xx–$FFxx | Normal boot, 38911 BYTES FREE (outside the RAMTAS range) | Fault outside $0400–$9FFF |
| U12 column fault at page $50 | Boot with BYTES FREE ≈ $5000 − $0801 = 18431 | RAMTAS: MEMTOP = first failing address |
| /GAME shorted to GND at the expansion port | Black screen (Ultimax with no ROMH) | PLA maps $E000 to the empty cartridge |
| PHI_COLOR missing | Stable B/W picture | Monitor model |
| Dot clock missing / VIC removed | No sync, nothing runs | VIC is the clock master |
| Keyboard row–column short | Phantom key, continuous typing | Switch matrix |

### 8.5 Diagnostic cartridges

- **Dead Test 781220** (Ultimax, its own font via ROMH): runs with no RAM
  needed; reports RAM failures by flash count and then by on-screen chip
  references.
- **Diagnostic 586220** (needs the loopback harness): tests ROM checksums,
  RAM, color RAM, CIAs (timers, TOD, ports through loopbacks), SID, serial,
  cassette.
- Both are end-to-end tests of the netlist (especially connectors and
  harnesses). For every golden-table fault where they apply, the reported
  chip must match. This is the most "real world" automated check we can
  have.

### 8.6 Real-hardware corpus

- The best ground truth is broken machines actually repaired: symptom
  (photo/video), probe readings, confirmed culprit.
- Each case becomes a scenario. A mismatch means a model gap and gets a fix
  or a documented limitation (§9).

---

## 9. Fidelity boundaries

| Phenomenon | Plan |
|---|---|
| Stuck/open/bridged digital pins and nets, dead or removed chips, bus contention, floating bus, chip-select/addressing faults, memory cell faults | **Emergent**, simulated faithfully |
| Marginal timing (slow PLA/DRAM, replacement PLAs "too fast/too slow", sparkle) | Approximated with `delay k` at evaluation-point granularity. A finer time model would need more points per half-cycle (possible later, at a speed cost) |
| What a floating MOS input reads; contention outcome | Pseudo-voltage model (§4.1): hold → decay → erratic near the threshold, per-receiver thresholds, contention as a conductance divider. The parameters (`τ`, `v_rest`, `σ`, `Vth` spread) are guesses that the 6510 port fall-off, the VICE open-bus tests and real-board measurements must calibrate |
| DRAM retention, temperature | Lazy decay model, off by default; thermal faults use activation times |
| Analog video (luma levels, RF modulator, color carrier phase) | Symptom hooks only: B/W, no sync, (later) "rainbow/unstable" approximations |
| SID analog (filter caps, output stage), paddle RC | Coarse parameters; out of scope for fault realism at first |
| PSU (ripple, 7805/7812 failures, the famous killer brick) | Rail flags: 12 V off, 5 V low → random bit errors. "Overvoltage killed the RAM/PLA/CPU" is modeled as the resulting chip faults, not as a process |
| Internal failures of the VIC/CIA/SID/6510 | Only selected, named internal faults, added case by case from real failure reports |

---

## 10. Milestones

Each milestone has an exit criterion. Nothing moves on until it passes.

**M0: Bootstrap.**
- Import `c64-llm` sources into this repo (chips, ROMs, harness, tests,
  Makefile) and build them unchanged.
- `tools/kicad2net.py`: generate `boards/250407.net` from the replica.
- Annotate part types and receiver kinds.
- Cross-check against schematic 251138 (U27 gate/pin assignment, U14/U26
  pins, connector pins). Write down any revision differences.
- *Exit:* `make test` passes; the netlist checker confirms that every IC pin
  is accounted for, rails are right, there are no unexpected floating
  inputs, and every fact in §3.2 holds.

**M1: Net engine + simple parts.**
- Signal model, union-find switches, levelized eval, sub-step scheduler,
  fault framework (pin/net/link/part).
- TTL library, 4066, pull-ups.
- *Exit:* unit tests, plus a mini netlist test bench (e.g. 257 + 373 + 139)
  with faults behaving as specified.

**M2: Memory subsystem.**
- PLA (fuse map + LUT), ROMs, 4164 ×8, 2114.
- A synthetic driver produces VIC-like φ1 and CPU-like φ2 cycles. There is
  no real VIC yet: the "clock master" is a stub that generates PHI0, /RAS,
  /CAS, AEC and BA.
- *Exit:* the PLA exhaustive test passes; read/write of every address in
  every banking mode (including Ultimax) matches `c64.h`'s memory map; the
  open-bus quirks of §8.3 appear.

**M3: CPU socket.**
- 6510 adapter on the M2 subsystem with the stub clock master.
- *Exit:* the KERNAL runs reset → RAMTAS → IOINIT with a bus trace identical
  to `c64-llm` up to the first VIC/CIA dependency.

**M4: VIC socket.**
- `m6569` refactor (bus transactions, refresh, real AEC/RAS/CAS/PHI0,
  register split, power-on).
- *Exit:* boot to READY, with a framebuffer and per-cycle bus trace
  identical to `c64-llm` (plus whitelisted improvements); `vic-timing`
  tests, *Nine* and *Rodents* still correct.

**M5: The rest of the healthy board.**
- CIAs (electrical ports, TOD), SID, keyboard switch matrix, joysticks,
  paddles via U28, 556, clock block, expansion port and cartridges, user
  port/IEC/cassette pins.
- *Exit:* everything in §8.2 passes; the §8.3 checklist all emerges; Dead
  Test and Diagnostic 586220 (with harness) report **all OK** on the healthy
  board.

**M6: Fault UX.**
- Fault spec parser, LLM commands, CLI, scenario files.
- `PROBE`, `TRACE`/VCD, `NET`, `PART`, `STATUS`, monitor OCR, video output
  stage.
- *Exit:* each fault class in §5 is exercised by at least one scenario.

**M7: Fault realism pass.**
- Turn §8.4 into automated scenarios, run the diagnostic cartridges against
  them, iterate on models and policies (floating, contention).
- Start the real-hardware corpus.
- *Exit:* every non-*verify* row of §8.4 reproduces; the *verify* rows are
  resolved or documented.

**M8: Performance.**
- Event-driven evaluation, packed buses, PLA LUT, fast paths, optional
  codegen.
- *Exit:* ≥1× realtime on a healthy board; healthy behavior unchanged
  (differential tests).

**M9: More boards and variants.**
- 250425 (8701 clock), NTSC 6567 + NTSC clock preload.
- Then the C64C family: 250466 (old chipset, 4464 DRAM); 250469 (251715
  SuperPLA, 8565/8580, 2× 41464 DRAM — the netlist is also in bwack's work).
- 1541 on IEC, datasette.

---

## 11. Decisions taken (defaults) and open questions

**Defaults:**

- **PAL** (6569, 985,248 Hz), like `c64-llm`; NTSC in M9.
- **250407** as the reference board, from bwack's KiCad netlist cross-checked
  with Commodore's 251138.
- **C**, header-style parts, same build style as `c64-llm`. Python only for
  dev-time netlist generation; the generated netlist is checked in.
- `c64-llm` stays a separate, frozen **reference** (process-level
  differential testing via trace files). This avoids linking two copies of
  the chip cores and keeps one machine model in this repo.
- Floating nets use the pseudo-voltage model of §4.1:
  - short floats hold;
  - long floats decay;
  - near-threshold floats read randomly, seeded per receiver.
  Contention resolves as a conductance divider, so low-wins emerges from the
  output asymmetry. All parameters are configurable per net class and per
  part type.

**Open questions** (resolved during M0/M7):

- Exact 6526 and 6510 port output drive structure (affects ghosting and
  contention realism).
- The exact VIC AEC/BA timing in edge cases (some VICE `dmadelay`/`irqdma`
  tests fail today). It matters more now, because AEC drives real tri-states.
- Which board revision(s) of the 250407 to treat as canonical when pin
  assignments differ (U27 example).
- Dead Test / Diagnostic ROM images and the 586220 harness wiring: from the
  official manuals (user-supplied images).

---

## 12. References

- `../c64-llm`: base emulator and harness (floooh `chips` derivative).
- bwack, *C64 250407 Replica (KiCad)*, https://github.com/bwack/C64-250407-Replica-KiCad (netlist source).
- Commodore, *C64 Service Manual* PN-314001; schematic 251138 (250407).
- *C64 PLA logic equations*: Jens Schönfeld's dump, Marko Mäkelä's equations,
  Mark Smith's JEDEC + ABEL read-out of the 82S100,
  https://sx64.net/downloads/C64%20PLA%20Logic%20Equations.txt.
- Thomas "skoe" Giesel, *The C64 PLA Dissected* (rev 1.1, 2012).
- C64-Wiki: *Motherboard* (IC positions per ASSY), *PLA (C64 chip)*.
- *Scope on the C64* (tech.guitarsite.de): reference waveforms and signal
  roles (AEC, /AEC, CAEC, RDY, color RAM CS, PLL clock chain).
- C64-Wiki.de: RAM chip ↔ data bit table (D0=U21 … D7=U12).
- C64 Dead Test 781220 and Diagnostic 586220 manuals.
