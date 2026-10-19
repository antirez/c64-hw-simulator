# Tests of the Z80 core

`chips/z80.h` is an altered version of the cycle-stepped Z80 of
[floooh/chips](https://github.com/floooh/chips): it is stepped on every clock
edge, and its pins follow the Zilog bus timing diagrams (UM0080) at half a
clock period. The ZX Spectrum's ULA needs that: it looks at the address bus
and /MREQ before /MREQ falls, to stop the clock (contention). The opcode
behaviour, flags and the T-states of every instruction are those of the
original core. The generator that writes the core is in `chips/codegen`
(`./z80_gen.sh` there rewrites `chips/z80.h`).

- `wave.c` checks the pins, edge by edge, against tables written by hand
  from the Zilog diagrams: 24 bus cycles (M1, memory and I/O, interrupt
  acknowledge in IM0/1/2, NMI, HALT, WAIT, BUSRQ, RESET) and the exact
  edges where WAIT, INT and NMI are seen. `make test`.
- `zex.c` runs Frank Cringle's ZEXDOC and ZEXALL on a board that talks to
  the CPU only through its pins. In the `strict` mode the board puts valid
  data on the bus only on the very edge where the CPU should sample it
  (garbage on all the others) and adds random wait states. The `.com`
  files are not in this repository (they are GPL): take `zexdoc.com` and
  `zexall.com` from floooh/chips-test (`tests/roms`) into `roms/` here,
  then `make zex && ./zex zexall strict`. Each run takes about 10 minutes.

When the core was written, both passed (67/67 tests of ZEXDOC and ZEXALL,
normal and strict), and a lockstep cross-check against the original core
(every opcode of every prefix, 64 random cases each, and 128M T-states of
random programs with interrupts) found no difference in registers, memory
or bus cycles.
