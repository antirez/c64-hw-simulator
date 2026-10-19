#!/bin/sh
# regenerates ../z80.h (the decoder and step defines between the <% %> markers)
cd "$(dirname "$0")"
if [ ! -d .venv ]; then
    python3 -m venv .venv
    .venv/bin/python3 -m pip install pyyaml
fi
.venv/bin/python3 z80_gen.py
