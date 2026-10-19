#-------------------------------------------------------------------------------
#   z80_gen.py
#
#   Code generator for the half-clock (edge-stepped) variant of z80.h.
#
#   Based on z80_gen.py from https://github.com/floooh/chips (zlib license,
#   Copyright (c) 2021 Andre Weissflog). ALTERED for pin-accurate bus timing:
#   every T-state is now split into two decoder steps, one executed on the
#   rising clock edge ('H' half) and one on the falling clock edge ('L' half),
#   and the bus control pins are driven at the exact half-cycles shown in the
#   Zilog Z80 CPU User Manual (UM0080) timing diagrams.
#
#   The instruction descriptions in z80_desc.yml are unchanged except for the
#   special decoder blocks (DD/FD+CB, (IX+d) displacement loading, interrupt
#   and NMI tails), see the comments there.
#-------------------------------------------------------------------------------
import yaml, copy, re
import templ

DESC_PATH  = 'z80_desc.yml'
INOUT_PATH  = '../z80.h'
TAB_WIDTH  = 4

# a machine cycle description
class MCycle:
    def __init__(self, type, tcycles, items):
        self.type = type
        self.tcycles = tcycles
        self.items = items

# an opcode description
class Op:
    def __init__(self, name, cond, flags):
        self.name = name
        self.cond = cond
        self.cond_compiled = compile(cond, '<string>', 'eval')
        self.flags = flags
        self.opcode = -1
        self.prefix = ''
        self.multiple = False
        self.multiple_first_op_index = -1
        self.layout = None
        self.mcycles = []

OP_DESCS = []

# special decoder blocks which are not reached through an opcode index,
# but through a '#define Z80_xxx_STEP' (name in yml => define name)
SPECIAL_OPS = {
    'cb':         'CB_STEP',
    'cbhl':       'CBHL_STEP',
    'ddfdcb':     'DDFDCB_STEP',
    'ddfd_d':     'DDFD_D_STEP',
    'ddfd_ldhln': 'DDFD_LDHLN_STEP',
    'int_im1':    'INT_IM1_STEP',
    'int_im2':    'INT_IM2_STEP',
    'nmi':        'NMI_STEP',
}

OPS = [None for _ in range(0,2*256 + len(SPECIAL_OPS))]

# a fetch machine cycle is processed as 2 parts because it overlaps
# with the 'action' of the previous instruction
FETCH_TCYCLES = 3
OVERLAPPED_FETCH_TCYCLES = 1
MEM_TCYCLES = 3
IO_TCYCLES = 4

# In the original (one step per T-state) generator, the 'action' of a machine
# cycle runs in this T-state of the machine cycle. This is only needed to
# translate the relative '$NEXTSTEP+n' skips of the instruction descriptions
# into absolute T-states, so that a skip lands exactly on the same T-state as
# in the original core (and the instruction timing stays identical).
ORIG_ACTION_TINDEX = { 'mread': 2, 'mwrite': 1, 'ioread': 3, 'iowrite': 2, 'generic': 0 }
# ...and this is the T-state where the action runs in this generator (memory
# and IO write actions moved to the end of the bus cycle, the falling edge of
# T3, so that a skip never cuts a bus cycle short). The original core executes
# T-states [0..action] + [action+1+n..end] when skipping, this generator
# executes [0..my_action] + [action+1+n+delta..end] with delta = my_action -
# action, so the instruction length is identical.
MY_ACTION_TINDEX = { 'mread': 2, 'mwrite': 2, 'ioread': 3, 'iowrite': 3, 'generic': 0 }

# register mapping tables, see: http://www.z80.info/decoding.htm
r_comment   = [ 'B', 'C', 'D', 'E', 'H', 'L', '(HL)', 'A' ]
rp_comment  = [ 'BC', 'DE', 'HL', 'SP' ]
rp2_comment = [ 'BC', 'DE', 'HL', 'AF' ]
alu_comment = [ 'ADD', 'ADC', 'SUB', 'SBC', 'AND', 'XOR', 'OR', 'CP' ]
rot_comment = [ 'RLC', 'RRC', 'RL', 'RR', 'SLA', 'SRA', 'SLL', 'SRL' ]
cc_comment  = [ 'NZ', 'Z', 'NC', 'C', 'PO', 'PE', 'P', 'M' ]

r_map    = [ 'cpu->b', 'cpu->c', 'cpu->d', 'cpu->e', 'cpu->hlx[cpu->hlx_idx].h', 'cpu->hlx[cpu->hlx_idx].l', 'XXX', 'cpu->a' ]
rr_map   = [ 'cpu->b', 'cpu->c', 'cpu->d', 'cpu->e', 'cpu->h', 'cpu->l', 'XXX', 'cpu->a' ]
rp_map   = [ 'cpu->bc', 'cpu->de', 'cpu->hlx[cpu->hlx_idx].hl', 'cpu->sp' ]
rrp_map  = [ 'cpu->bc', 'cpu->de', 'cpu->hl', 'cpu->sp' ]
rpl_map  = [ 'cpu->c', 'cpu->e', 'cpu->hlx[cpu->hlx_idx].l', 'cpu->spl']
rrpl_map = [ 'cpu->c', 'cpu->e', 'cpu->l', 'cpu->spl']
rph_map  = [ 'cpu->b', 'cpu->d', 'cpu->hlx[cpu->hlx_idx].h', 'cpu->sph']
rrph_map  = [ 'cpu->b', 'cpu->d', 'cpu->h', 'cpu->sph']
rp2_map  = [ 'cpu->bc', 'cpu->de', 'cpu->hlx[cpu->hlx_idx].hl', 'cpu->af' ]
rp2l_map = [ 'cpu->c', 'cpu->e', 'cpu->hlx[cpu->hlx_idx].l', 'cpu->f']
rp2h_map = [ 'cpu->b', 'cpu->d', 'cpu->hlx[cpu->hlx_idx].h', 'cpu->a']
cc_map   = [ '_cc_nz', '_cc_z', '_cc_nc', '_cc_c', '_cc_po', '_cc_pe', '_cc_p', '_cc_m' ]
alu_map  = [ '_z80_add8(cpu,',
             '_z80_adc8(cpu,',
             '_z80_sub8(cpu,',
             '_z80_sbc8(cpu,',
             '_z80_and8(cpu,',
             '_z80_xor8(cpu,',
             '_z80_or8(cpu,',
             '_z80_cp8(cpu,' ]
rot_map  = [ '_z80_rlc(cpu,',
             '_z80_rrc(cpu,',
             '_z80_rl(cpu,',
             '_z80_rr(cpu,',
             '_z80_sla(cpu,',
             '_z80_sra(cpu,',
             '_z80_sll(cpu,',
             '_z80_srl(cpu,' ]
im_map = [ '0', '0', '1', '2', '0', '0', '1', '2' ]

def err(msg: str):
    raise BaseException(msg)

def map_comment(inp, y, z, p, q):
    return inp\
        .replace('$RY', r_comment[y])\
        .replace('$RZ', r_comment[z])\
        .replace('$ALU', alu_comment[y])\
        .replace('$ROT', rot_comment[y])\
        .replace('$RP2', rp2_comment[p])\
        .replace('$RP', rp_comment[p])\
        .replace('$CC-4', cc_comment[y-4])\
        .replace('$CC', cc_comment[y])\
        .replace('$IMY', im_map[y])\
        .replace('$Y*8', f'{y*8:X}h')\
        .replace('$Y', f'{y}')

def map_cpu(inp, y, z, p, q):
    return inp\
        .replace('$ADDR', 'cpu->addr')\
        .replace('$ALU(', alu_map[y])\
        .replace('$ROT(', rot_map[y])\
        .replace('$RRPL', rrpl_map[p])\
        .replace('$RRPH', rrph_map[p])\
        .replace('$RRP', rrp_map[p])\
        .replace('$RRY', rr_map[y])\
        .replace('$RRZ', rr_map[z])\
        .replace('$RY', r_map[y])\
        .replace('$RZ', r_map[z])\
        .replace('$RP2L', rp2l_map[p])\
        .replace('$RP2H', rp2h_map[p])\
        .replace('$RP2', rp2_map[p])\
        .replace('$RPL', rpl_map[p])\
        .replace('$RPH', rph_map[p])\
        .replace('$RP', rp_map[p])\
        .replace('$CC-4', cc_map[y-4])\
        .replace('$CC', cc_map[y])\
        .replace('$PCL', 'cpu->pcl')\
        .replace('$PCH', 'cpu->pch')\
        .replace('$PC', 'cpu->pc')\
        .replace('$AF', 'cpu->af')\
        .replace('$BC', 'cpu->bc')\
        .replace('$DE', 'cpu->de')\
        .replace('$HL', 'cpu->hlx[cpu->hlx_idx].hl')\
        .replace('$SP', 'cpu->sp')\
        .replace('$WZL', 'cpu->wzl')\
        .replace('$WZH', 'cpu->wzh')\
        .replace('$WZ', 'cpu->wz')\
        .replace('$DLATCH', 'cpu->dlatch')\
        .replace('$A', 'cpu->a')\
        .replace('$C', 'cpu->c')\
        .replace('$B', 'cpu->b')\
        .replace('$E', 'cpu->e')\
        .replace('$D', 'cpu->d')\
        .replace('$L', 'cpu->hlx[cpu->hlx_idx].l')\
        .replace('$H', 'cpu->hlx[cpu->hlx_idx].h')\
        .replace('$IMY', im_map[y])\
        .replace('$IM', 'cpu->im')\
        .replace('$Y*8', f'0x{y*8:02X}')\

def flag(op, flag):
    if flag in op.flags:
        return op.flags[flag]
    else:
        return False

def parse_opdescs():
    with open(DESC_PATH, 'r') as fp:
        desc = yaml.load(fp, Loader=yaml.FullLoader) # type: ignore
        for (op_name, op_desc) in desc.items():
            if 'cond' not in op_desc:
                op_desc['cond'] = 'True'
            if 'mcycles' not in op_desc:
                err(f"op '{op_name}' has no mcycles!")
            flags = {}
            if 'flags' in op_desc:
                flags = op_desc['flags']
            op = Op(op_name,op_desc['cond'], flags)
            if 'prefix' in op_desc:
                op.prefix = op_desc['prefix']
            num_fetch = 0
            num_terminal = 0
            for mc_desc in op_desc['mcycles']:
                mc_type = mc_desc['type']
                if mc_type == 'fetch':
                    num_fetch += 1
                elif mc_type in ('overlapped', 'dispatch'):
                    num_terminal += 1
                if 'tcycles' in mc_desc:
                    mc_tcycles = mc_desc['tcycles']
                else:
                    default_tcycles = {
                        'fetch': FETCH_TCYCLES,
                        'mread': MEM_TCYCLES,
                        'mwrite': MEM_TCYCLES,
                        'ioread': IO_TCYCLES,
                        'iowrite': IO_TCYCLES,
                        'overlapped': OVERLAPPED_FETCH_TCYCLES,
                        'dispatch': 0,
                        'generic': -1
                    }
                    mc_tcycles = default_tcycles[mc_type]
                if mc_tcycles == -1:
                    err(f'generic mcycles must have explicit length (op: {op_name})')
                if mc_type != 'dispatch' and ((mc_tcycles < 1) or (mc_tcycles > 7)):
                    err(f'invalid mcycle len: {mc_tcycles}')
                if mc_type == 'fetch':
                    if mc_tcycles != 4:
                        err('fetch ticks must have exactly 4 tcycles')
                    mc_tcycles -= OVERLAPPED_FETCH_TCYCLES
                if mc_type == 'overlapped' and mc_tcycles != 1:
                    err('overlapped ticks must have exactly 1 tcycle!')
                if mc_type in ('mread', 'mwrite') and mc_tcycles < MEM_TCYCLES:
                    err(f'memory cycles need at least 3 tcycles (op: {op_name})')
                if mc_type in ('ioread', 'iowrite') and mc_tcycles < IO_TCYCLES:
                    err(f'io cycles need at least 4 tcycles (op: {op_name})')
                mc = MCycle(mc_type, mc_tcycles, mc_desc)
                op.mcycles.append(mc)
            if num_fetch == 0:
                op.mcycles.insert(0, MCycle('fetch', FETCH_TCYCLES, {}))
            if num_terminal == 0:
                op.mcycles.append(MCycle('overlapped', OVERLAPPED_FETCH_TCYCLES, {}))
            OP_DESCS.append(op)

def find_opdesc(name):
    for op_desc in OP_DESCS:
        if op_desc.name == name:
            return op_desc
    err(f"opdesc not found for '{name}'")
    return None

def find_op(name):
    for op in OPS:
        if op.name == name:
            return op
    err(f"op not found for '{name}'")
    return None

def stampout_mcycle_items(mcycle_items, y, z, p, q):
    res = {}
    for key,val in mcycle_items.items():
        if type(val) == str:
            res[key] = map_cpu(val, y, z, p, q)
    return res

def stampout_op(prefix, opcode, op_index, op_desc):
    #  76 543 210
    # |xx|yyy|zzz|
    #    |ppq|
    y = (opcode >> 3) & 7
    z = opcode & 7
    p = y >> 1
    q = y & 1
    if op_desc.multiple_first_op_index == -1:
        op_desc.multiple_first_op_index = op_index
    op = copy.deepcopy(op_desc)
    op.name = map_comment(op.name, y, z, p, q)
    op.prefix = prefix
    op.opcode = opcode
    op.index = op_index
    if flag(op, 'multiple') and op.multiple_first_op_index != op_index:
        op.flags['redundant'] = True
    for mcycle in op.mcycles:
        mcycle.items = stampout_mcycle_items(mcycle.items, y, z, p, q)
    OPS[op_index] = op

def expand_optable():
    for oprange,prefix in enumerate(['', 'ed']):
        for opcode in range(0,256):
            x = opcode >> 6 # type: ignore (generated unused warning, but x is needed in 'eval')
            y = (opcode >> 3) & 7
            z = opcode & 7 # type: ignore
            p = y >> 1 # type: ignore
            q = y & 1 # type: ignore
            op_index = oprange * 256 + opcode
            for op_desc_index,op_desc in enumerate(OP_DESCS):
                if not flag(op_desc, 'special'):
                    if eval(op_desc.cond_compiled) and op_desc.prefix == prefix:
                        if OPS[op_index] is not None:
                            err(f"Condition collission for opcode {op_desc_index:02X} and '{op_desc.name}'")
                        stampout_op(prefix, opcode, op_index, op_desc)
    for name in SPECIAL_OPS.keys():
        op_index += 1
        stampout_op('', -1, op_index, find_opdesc(name))

#-------------------------------------------------------------------------------
#   Expansion of machine cycles into T-states with a rising-edge (H) and a
#   falling-edge (L) half. Code strings may contain these tokens which are
#   resolved when the decoder steps are numbered:
#
#   $WAIT   => sample the WAIT pin; if active, repeat this T-state (a TW state)
#   @Tn@    => decoder step of the H half of T-state n (skip targets)
#
#   Bus timing implemented here (see UM0080 Figures 5, 6, 7):
#
#   memory read  T1 H: address out, all control pins inactive
#                T1 L: MREQ+RD active
#                T2 L: WAIT sampled (a TW repeats T2 with unchanged outputs)
#                T3 L: data bus sampled, MREQ+RD inactive
#   memory write T1 H: address out
#                T1 L: MREQ active, data bus driven (DOE)
#                T2 L: WR active, WAIT sampled
#                T3 L: MREQ+WR inactive (data is held until the next rising edge)
#   io read      T1 H: address out
#                T2 H: IORQ+RD active
#                TW L: WAIT sampled (TW is the automatic IO wait state)
#                T3 L: data bus sampled, IORQ+RD inactive
#   io write     T1 H: address out
#                T1 L: data bus driven
#                T2 H: IORQ+WR active
#                TW L: WAIT sampled
#                T3 L: IORQ+WR inactive (data is held until the next rising edge)
#
#   Internal T-states keep the last address on the address bus (as the real
#   chip does, this is what the ZX Spectrum ULA contention depends on).
#
#   The opcode fetch machine cycle (and the interrupt acknowledge cycles) are
#   hand-written in the template part of z80.h.
#-------------------------------------------------------------------------------
class TState:
    def __init__(self, h, l, kind, has_action=False):
        self.h = h              # code of the rising edge half
        self.l = l              # code of the falling edge half (None for the overlapped T-state)
        self.kind = kind        # 'bus', 'mw_t3', 'internal', 'overlapped'
        self.has_action = has_action
        self.l_goto = None      # if set: explicit jump target at the end of the L half

def skip_subst(action, t_orig_action, delta):
    # translate '$NEXTSTEP+n' (relative to the original action T-state) into
    # an absolute T-state reference
    def repl(m):
        return f'@T{t_orig_action + 1 + int(m.group(1)) + delta}@'
    res = re.sub(r'\$NEXTSTEP\+(\d+)', repl, action)
    if '$NEXTSTEP' in res:
        err(f'unsupported $NEXTSTEP usage: {action}')
    return res

def expand_tstates(op):
    ts = []
    for mc in op.mcycles:
        if mc.type == 'fetch':
            continue
        items = mc.items
        t0 = len(ts)
        action = (f"{items['action']};" if 'action' in items else '')
        # optional code at the start of the machine cycle's first rising edge
        pre = (f"{items['pre']};" if 'pre' in items else '')
        if mc.type in ORIG_ACTION_TINDEX:
            action = skip_subst(action, t0 + ORIG_ACTION_TINDEX[mc.type], MY_ACTION_TINDEX[mc.type] - ORIG_ACTION_TINDEX[mc.type])
        if mc.type == 'mread':
            addr = items['ab']
            store = (f"{items['dst']}=_gd();" if 'dst' in items else '')
            ts.append(TState(f'{pre}_brq();_sactl({addr},0);', '_ctl(Z80_MREQ|Z80_RD);', 'bus'))
            ts.append(TState('', '$WAIT', 'bus'))
            ts.append(TState('', f'{store}_ctl(0);{action}', 'bus', action != ''))
            for _ in range(3, mc.tcycles):
                ts.append(TState('', '', 'internal'))
        elif mc.type == 'mwrite':
            addr = items['ab']
            data = items['db']
            ts.append(TState(f'{pre}_brq();_sactl({addr},0);', f'_sdctl({data},Z80_MREQ|Z80_DOE);', 'bus'))
            ts.append(TState('', '_ctl(Z80_MREQ|Z80_WR|Z80_DOE);$WAIT', 'bus'))
            ts.append(TState('', f'_ctlclr(Z80_MREQ|Z80_WR);{action}', 'mw_t3', action != ''))
            for i in range(3, mc.tcycles):
                # stop driving the data bus after the write cycle
                ts.append(TState('_ctl(0);' if i == 3 else '', '', 'internal'))
        elif mc.type == 'ioread':
            addr = items['ab']
            store = (f"{items['dst']}=_gd();" if 'dst' in items else '')
            ts.append(TState(f'{pre}_brq();_sactl({addr},0);', '', 'bus'))
            ts.append(TState('_ctl(Z80_IORQ|Z80_RD);', '', 'bus'))
            ts.append(TState('', '$WAIT', 'bus'))
            ts.append(TState('', f'{store}_ctl(0);{action}', 'bus', action != ''))
            for _ in range(4, mc.tcycles):
                ts.append(TState('', '', 'internal'))
        elif mc.type == 'iowrite':
            addr = items['ab']
            data = items['db']
            ts.append(TState(f'{pre}_brq();_sactl({addr},0);', f'_sdctl({data},Z80_DOE);', 'bus'))
            ts.append(TState('_ctl(Z80_IORQ|Z80_WR|Z80_DOE);', '', 'bus'))
            ts.append(TState('', '$WAIT', 'bus'))
            ts.append(TState('', f'_ctlclr(Z80_IORQ|Z80_WR);{action}', 'bus', action != ''))
            for i in range(4, mc.tcycles):
                ts.append(TState('_ctl(0);' if i == 4 else '', '', 'internal'))
        elif mc.type == 'generic':
            # internal machine cycle, the action runs on the first rising edge,
            # all control pins inactive (ends RFSH after M1, DOE after writes)
            ts.append(TState(f'{pre}_ctl(0);{action}', '', 'internal', action != ''))
            for _ in range(1, mc.tcycles):
                ts.append(TState('', '', 'internal'))
        elif mc.type == 'overlapped':
            # last T-state of the instruction == T1 rising edge of the next opcode fetch
            if 'post_action' in items:
                code = f"_brq();{action}pins=_z80_fetch(cpu,pins);{items['post_action']};goto rise_done;"
            elif 'prefix' in items:
                code = f"_brq();{action}_fetch_{items['prefix']}();goto rise_done;"
            else:
                code = f"_brq();{action}_fetch();"
            ts.append(TState(code, None, 'overlapped'))
        elif mc.type == 'dispatch':
            # continue with the decoder step in 'target' after the last T-state
            if len(ts) == 0:
                err(f'dispatch without preceding tstates in {op.name}')
            ts[-1].l_goto = items['target']
        else:
            err(f'unknown mcycle type {mc.type}')
    # check that skips land on a T-state where nothing bus-related happens
    for t in ts:
        for code in (t.h, t.l):
            if code is None:
                continue
            for m in re.finditer(r'@T(\d+)@', code):
                k = int(m.group(1))
                if k >= len(ts):
                    err(f'skip target out of range in {op.name}')
                tgt = ts[k]
                ok = (tgt.kind in ('internal', 'overlapped')) or (tgt.kind == 'mw_t3' and not tgt.has_action)
                if not ok:
                    err(f'skip in {op.name} lands in the middle of a bus cycle (T-state {k})')
    return ts

def gen_decoder():
    indent = 2
    cur_extra_step = 512 # main and ed ops
    out_lines = ''
    out_extra_lines = ''

    def tab():
        return ' ' * TAB_WIDTH * indent

    for op_index, op in enumerate(OPS):
        special = flag(op, 'special')
        redundant = flag(op, 'redundant')
        ts = expand_tstates(op)

        # assign decoder step numbers to each half T-state
        if redundant:
            layout = dict(OPS[op.multiple_first_op_index].layout)
            layout[(0,'h')] = op_index
        else:
            layout = {}
            for t, tstate in enumerate(ts):
                for half in ('h', 'l'):
                    code = tstate.h if half == 'h' else tstate.l
                    if code is None:
                        continue
                    if t == 0 and half == 'h' and not special:
                        layout[(t, half)] = op_index
                    else:
                        layout[(t, half)] = cur_extra_step
                        cur_extra_step += 1
            # A skip taken in a rising-edge half can't jump directly to the
            # target's rising-edge half, the falling edge in between needs an
            # (empty) 'trampoline' step which replaces the L half of the
            # current T-state.
            for t, tstate in enumerate(ts):
                if tstate.h is None:
                    continue
                for m in re.finditer(r'@T(\d+)@', tstate.h):
                    k = int(m.group(1))
                    if ('tramp', k) not in layout:
                        if tstate.l != '' or tstate.l_goto is not None:
                            err(f'skip in rising edge half of {op.name} T{t} with non-empty falling edge half')
                        layout[('tramp', k)] = cur_extra_step
                        cur_extra_step += 1
        op.layout = layout

        def resolve(code, t, half):
            code = code.replace('$WAIT', f"_wait({layout[(t,'h')]});")
            if half == 'h':
                code = re.sub(r'@T(\d+)@', lambda m: str(layout[('tramp', int(m.group(1)))]), code)
            else:
                code = re.sub(r'@T(\d+)@', lambda m: str(layout[(int(m.group(1)), 'h')]), code)
            code = code.replace('_goto(', '_rgoto(' if half == 'h' else '_fgoto(')
            return code

        if not redundant:
            for key, step in layout.items():
                if key[0] == 'tramp':
                    k = key[1]
                    out_extra_lines += tab() + f"case {step:4}: _fgoto({layout[(k,'h')]}); // {op.name} (skip to T{k}H)\n"

        for t, tstate in enumerate(ts):
            for half in ('h', 'l'):
                code = tstate.h if half == 'h' else tstate.l
                if code is None:
                    continue
                if redundant and not (t == 0 and half == 'h'):
                    # redundant ops share the decoder steps of the first op
                    continue
                code = resolve(code, t, half)
                if tstate.kind == 'overlapped':
                    pass # code already contains the jump
                elif half == 'h':
                    code += f"_rgoto({layout[(t,'l')]});"
                elif tstate.l_goto is not None:
                    code += f"_fgoto({tstate.l_goto});"
                else:
                    code += f"_fgoto({layout[(t+1,'h')]});"
                step = layout[(t, half)]
                line = f"case {step:4}: {code} // {op.name} (T{t}{'H' if half == 'h' else 'L'})"
                if t == 0 and half == 'h' and not special:
                    out_lines += tab() + line + '\n'
                else:
                    out_extra_lines += tab() + line + '\n'
    return { 'out_lines': out_lines + out_extra_lines, 'max_step': cur_extra_step }

def extra_step_defines_string(max_step):
    # hand-written decoder steps in the template (H: rising edge, L: falling edge)
    manual_steps = []
    def m1(prefix, last_t=4):
        manual_steps.append(f'{prefix}_T1L')
        for t in range(2, last_t+1):
            manual_steps.append(f'{prefix}_T{t}H')
            manual_steps.append(f'{prefix}_T{t}L')
    m1('M1')
    m1('DDFD_M1')
    m1('CB_M1')
    m1('ED_M1')
    m1('HALT_M1')
    m1('NMI_M1', 5)
    manual_steps += [
        'INT_M1_T1L', 'INT_M1_T2H', 'INT_M1_T2L', 'INT_M1_TW1H', 'INT_M1_TW1L',
        'INT_M1_TW2H', 'INT_M1_TW2L', 'INT_M1_T3H', 'INT_M1_T3L', 'INT_M1_T4H',
        'INT_M1_T4L', 'INT_M1_T5H', 'INT_M1_T5L',
        'BUSAK_TXL', 'BUSAK_TXH', 'BUSAK_ENDL',
        'RES_L', 'RES_H', 'RES_L2',
        'PREFETCH_L',
    ]
    res = ''
    step_index = max_step
    for step_name in manual_steps:
        res += f'#define Z80_{step_name} {step_index}\n'
        step_index += 1
    for op_name, step_name in SPECIAL_OPS.items():
        op = find_op(op_name)
        res += f"#define Z80_{step_name} {op.layout[(0,'h')]}\n"
        if op_name == 'ddfdcb':
            # entered at T1 falling edge from _z80_fetch_cb()
            res += f"#define Z80_{step_name}_T0L {op.layout[(0,'l')]}\n"
    res += f'#define Z80_NUM_STEPS {step_index}\n'
    return res

def indirect_table_string():
    res = ''
    for i in range(0, 256):
        op = OPS[i]
        if i % 16 == 0:
            res += '    '
        if flag(op, 'indirect'):
            res += '1,'
        else:
            res += '0,'
        if i % 16 == 15:
            res += '\n'
    return res

def write_result(decoder_output):
    out_lines = decoder_output['out_lines']
    max_step = decoder_output['max_step']
    with open(INOUT_PATH, 'r') as f:
        lines = f.read().splitlines()
        lines = templ.replace(lines, 'decoder', out_lines)
        lines = templ.replace(lines, 'extra_step_defines', extra_step_defines_string(max_step))
        lines = templ.replace(lines, 'indirect_table', indirect_table_string())
    out_str = '\n'.join(lines) + '\n'
    with open(INOUT_PATH, 'w') as f:
        f.write(out_str)

if __name__ == '__main__':
    parse_opdescs()
    expand_optable()
    decoder_output = gen_decoder()
    write_result(decoder_output)
