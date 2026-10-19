# Tests of the edge-stepped, pin-accurate z80.h (chips/z80.h). Run here:
#   make test          the waveform test (a second)
#   ./zex zexdoc|zexall normal|strict    the instruction exercisers (minutes)
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: wave zex

wave: wave.c ../../chips/z80.h
	$(CC) $(CFLAGS) -Wno-missing-field-initializers -Wno-unused-parameter -o $@ wave.c

zex: zex.c ../../chips/z80.h
	$(CC) $(CFLAGS) -o $@ zex.c

test: wave
	./wave

clean:
	rm -f wave zex

.PHONY: all test clean
