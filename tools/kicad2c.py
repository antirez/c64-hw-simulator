#!/usr/bin/env python3
"""
kicad2c.py - turn a KiCad PCB file into plain C tables for c64-hw-sim.

Usage:
    python3 tools/kicad2c.py 250407_.kicad_pcb c64 > src/c64_board_data.c
    python3 tools/kicad2c.py Issue3B.kicad_pcb zx48 > src/zx48_board_data.c

The second argument names the board: the tables become the board_data_t
bd_<name>. Sources (see BOARDS below):
  c64   https://github.com/bwack/C64-250407-Replica-KiCad (CERN OHL v1.2)
  zx48  https://github.com/liveboxandy/ZX-Spectrum-48K-Issue-3B---Based-on-
        Gerbers-by-PABB-on-PCBWay (CC0 1.0)

We extract only what the simulator needs:
  - every footprint (reference, value, footprint name),
  - every pad with its absolute position (mm), size and net,
  - the list of net names (KiCad escapes cleaned up),
  - the board outline bounding box (Edge.Cuts layer).
Nothing else (tracks, zones, silkscreen) is used.
"""
import math
import re
import sys


def tokenize(text):
    """Split a KiCad s-expression file into tokens: '(', ')', atoms, strings."""
    tok_re = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+')
    for m in tok_re.finditer(text):
        t = m.group(0)
        if t.startswith('"'):
            yield ('str', t[1:-1].replace('\\"', '"'))
        else:
            yield ('sym', t)


def parse(tokens):
    """Build nested Python lists from the token stream."""
    stack = [[]]
    for kind, t in tokens:
        if kind == 'sym' and t == '(':
            stack.append([])
        elif kind == 'sym' and t == ')':
            node = stack.pop()
            stack[-1].append(node)
        else:
            stack[-1].append(t)
    return stack[0][0]


def children(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]


def child(node, name):
    c = children(node, name)
    return c[0] if c else None


# What each board is and where it comes from, for the generated file.
BOARDS = {
    'c64': ('C64 ASSY 250407 geometry and netlist, from bwack\'s KiCad replica',
            '(https://github.com/bwack/C64-250407-Replica-KiCad, CERN OHL v1.2).'),
    'zx48': ('ZX Spectrum 48K Issue 3B geometry and netlist, from liveboxandy\'s KiCad',
             'files (github.com/liveboxandy/ZX-Spectrum-48K-Issue-3B---Based-on-Gerbers-\n'
             ' * by-PABB-on-PCBWay, CC0 1.0), redrawn from PABB\'s gerbers of the board.'),
}


# Active low signals on the Spectrum board are named with underscores:
# '_M_REQ' is /MREQ, '_IO_RQULA' /IORQULA.
UNDERSCORE_BAR = False


def clean_net(name):
    """KiCad net names -> readable names: '/sheet/~{AEC}' -> '/AEC'."""
    name = re.sub(r'^/250407_sheet2/', '', name)
    name = re.sub(r'^/', '', name)
    name = name.replace('\\\\', '\\')
    # ~{X} means "X with an overbar" (active low): render as /X
    name = re.sub(r'~\{([^}]*)\}', r'/\1', name)
    name = name.replace('\\', '')
    if UNDERSCORE_BAR and name.startswith('_'):
        name = '/' + name[1:].replace('_', '')
    return name


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in BOARDS:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    board = sys.argv[2]
    global UNDERSCORE_BAR
    UNDERSCORE_BAR = board == 'zx48'
    pcb = parse(tokenize(open(sys.argv[1], encoding='utf-8').read()))

    # Net table (index 0 is the unconnected net in KiCad). KiCad 10 has no
    # table: pads name their net, numbered here in order of appearance.
    nets = {}
    for n in children(pcb, 'net'):
        nets[int(n[1])] = clean_net(n[2])
    by_name = {}

    def net_index(net):
        if not net:
            return 0
        if len(net) > 2 or net[1].isdigit():    # (net 12 "name")
            return int(net[1])
        name = clean_net(net[1])                # (net "name")
        if name.startswith('unconnected-('):    # a pad on its own: no net
            return 0
        if name not in by_name:
            by_name[name] = len(nets) + len(by_name) + 1
            nets[by_name[name]] = name
        return by_name[name]

    # Board outline: bounding box of everything on Edge.Cuts.
    xs, ys = [], []
    for kind in ('gr_line', 'gr_rect', 'gr_arc'):
        for g in children(pcb, kind):
            layer = child(g, 'layer')
            if not layer or layer[1] != 'Edge.Cuts':
                continue
            for key in ('start', 'end', 'mid'):
                p = child(g, key)
                if p:
                    xs.append(float(p[1]))
                    ys.append(float(p[2]))

    comps = []
    for fp in children(pcb, 'footprint'):
        at = child(fp, 'at')
        fx, fy = float(at[1]), float(at[2])
        frot = float(at[3]) if len(at) > 3 else 0.0
        ref = value = '?'
        for prop in children(fp, 'property'):
            if prop[1] == 'Reference':
                ref = prop[2]
            elif prop[1] == 'Value':
                value = prop[2]
        if ref.startswith('REF'):          # mounting holes
            continue
        a = math.radians(frot)
        pads = []
        for pad in children(fp, 'pad'):
            pat = child(pad, 'at')
            px, py = float(pat[1]), float(pat[2])
            # KiCad: pad offsets are in footprint space, rotate them by the
            # footprint angle (counter-clockwise on screen, y axis down).
            ax = fx + px * math.cos(a) + py * math.sin(a)
            ay = fy - px * math.sin(a) + py * math.cos(a)
            size = child(pad, 'size')
            w, h = float(size[1]), float(size[2])
            if round(frot) % 180 == 90:
                w, h = h, w
            netidx = net_index(child(pad, 'net'))
            pads.append((pad[1], ax, ay, w, h, netidx))
        comps.append((ref, value, child(fp, 'footprint') or fp[1], pads))

    # Renumber nets densely, dropping the "no net" entry.
    used = sorted({p[5] for c in comps for p in c[3] if p[5] != 0})
    remap = {old: i for i, old in enumerate(used)}

    out = sys.stdout
    out.write('/* %s_board_data.c - GENERATED by tools/kicad2c.py, do not edit.\n' % board)
    out.write(' * %s\n * %s */\n' % BOARDS[board])
    out.write('#include "board_data.h"\n\n')
    out.write('static const char *const nets[] = {\n')
    for old in used:
        out.write('    "%s",\n' % nets[old].replace('"', '\\"'))
    out.write('};\n\n')

    out.write('static const bd_pad_t pads[] = {\n')
    first = []
    n = 0
    for ref, value, fpname, pads in comps:
        first.append(n)
        for name, x, y, w, h, net in pads:
            out.write('    { "%s", %.3ff, %.3ff, %.3ff, %.3ff, %d },\n' %
                      (name, x, y, w, h, remap[net] if net else -1))
            n += 1
    out.write('};\n\n')

    out.write('static const bd_comp_t comps[] = {\n')
    for i, (ref, value, fpname, pads) in enumerate(comps):
        fpname = fpname.split(':')[-1]
        out.write('    { "%s", "%s", "%s", %d, %d },\n' %
                  (ref, value.replace('"', '\\"'), fpname, first[i], len(pads)))
    out.write('};\n\n')
    out.write('const board_data_t bd_%s = {\n' % board)
    out.write('    { %.3ff, %.3ff, %.3ff, %.3ff },\n' % (min(xs), min(ys), max(xs), max(ys)))
    out.write('    nets, %d, pads, %d, comps, %d,\n};\n' % (len(used), n, len(comps)))


if __name__ == '__main__':
    main()
