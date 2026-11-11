#!/usr/bin/env python3
"""Differential test: this RL78 core vs. the GNU simulator (rl78-elf-run).

Optional: runs only when RL78_GNU_BIN points to a directory containing
rl78-elf-as, rl78-elf-ld, rl78-elf-objcopy, rl78-elf-objdump and
rl78-elf-run (binutils / GDB sim).  Otherwise it prints SKIP and exits 0.
Nothing from the GNU tools is copied into this repository; the tools are
only executed.

What it does
  * Instruction cases: every form of the test assembler's encoding table
    (tests/tasm.c, which mirrors Table 5-8 of R01US0015) is instantiated
    several times with random operands, random initial register banks,
    PSW (flags, RBS, ISP, IE) and memory.  Each case = initialisation code +
    the instruction + a state dump written to SDR00 (FFF10H), which the GNU
    simulator prints to stdout.  Branch/call/return/skip instructions get
    scaffolding so that the taken/not-taken path is visible in the dump.
  * Random sequences of 8..24 register/immediate/saddr/!addr16 instructions.
  * Both simulators run the same linked binary; the dumps are compared
    byte for byte (registers A,X,C,B,E,D,L,H, PSW, ES, CS, SP and memory
    FE20-FE27, FE70-FEAF, FDF8-FE07, FEE0-FEFF (all register banks),
    FFFF0-FFFF7).
  * The disassembler is compared with rl78-elf-objdump -d after
    normalisation (case, number bases, symbol annotations).
  * Per-instruction clock counts of rl78-elf-run (-D) are compared with
    ours (ours are separately checked against the manual by run_tests).
  * Agreeing cases are written as self-contained regression vectors
    (tests/difftest_vectors.inc) that run without the GNU tools.

Usage: difftest.py [--instances N] [--seqs N] [--cores S1,S2,S3] [--seed N]
                   [--write-vectors FILE] [--keep DIR]
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RL78 = os.path.dirname(os.path.dirname(HERE))

# ----------------------------------------------------------------------------
# PRNG shared with tests/t_regress.c (xorshift32)


class Rng:
    def __init__(self, seed):
        self.s = (seed & 0xFFFFFFFF) or 0x9E3779B9

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.s = s
        return s

    def byte(self):
        return self.next() & 0xFF

    def below(self, n):
        return self.next() % n

    def choice(self, seq):
        return seq[self.below(len(seq))]


# dump layout (must match the dump code below and tests/t_regress.c)
REGIONS = [(0xFFE20, 8), (0xFFE70, 64), (0xFFDF8, 16), (0xFFEE0, 32), (0xFFFF0, 8)]
INIT_REGIONS = [(0xFFE20, 8), (0xFFE70, 64), (0xFFDF8, 16), (0xFFEE0, 32), (0xFFFF0, 8)]
REGNAMES = ['A', 'PSW', 'X', 'C', 'B', 'E', 'D', 'L', 'H', 'ES', 'CS', 'SPH', 'SPL']
DUMP_LEN = 3 + len(REGNAMES) + sum(n for _, n in REGIONS)


def init_state(seed, core):
    """Initial state derived from the seed (same algorithm in t_regress.c)."""
    r = Rng(seed)
    mem = {}
    for base, n in INIT_REGIONS:
        for i in range(n):
            mem[base + i] = r.byte()
    for bank in range(4):
        b = 0xFFEF8 - 8 * bank
        mem[b + 2] = r.byte() & 0x0F          # C
        mem[b + 3] = r.byte() & 0x0F          # B
        mem[b + 4] = 0x70 + (r.byte() & 0x0E)  # E (even: word accesses must be aligned)
        mem[b + 5] = 0xFE                     # D
        mem[b + 6] = 0x70 + (r.byte() & 0x0E)  # L (even)
        mem[b + 7] = 0xFE                     # H
    psw = r.byte()
    if core == 'S1':
        psw &= ~0x28
    return {'mem': mem, 'psw': psw, 'es': 0x0F, 'cs': 0, 'sp': 0xFE00, 'rng': r}


def bank_of(psw):
    return ((psw >> 3) & 1) | ((psw >> 4) & 2)


def reg_addr(psw, name):
    idx = 'XACBEDLH'.index(name)
    return 0xFFEF8 - 8 * bank_of(psw) + idx


# ----------------------------------------------------------------------------
# instruction instantiation from the tasm pattern table

FLOW_BRANCH = {'BR', 'BC', 'BNC', 'BZ', 'BNZ', 'BH', 'BNH', 'BT', 'BF', 'BTCLR'}
SKIPS = {'SKC', 'SKNC', 'SKZ', 'SKNZ', 'SKH', 'SKNH'}
S3_ONLY = {'MULHU', 'MULH', 'DIVHU', 'DIVWU', 'MACHU', 'MACH'}


def load_table(rl78dt):
    out = subprocess.run([rl78dt, 'table'], capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        pat, bytes_ = line.split('|')
        rows.append((pat, bytes_))
    return rows


def split_pat(pat):
    if ' ' in pat:
        mn, rest = pat.split(' ', 1)
        return mn, [o.strip() for o in rest.split(',')]
    return pat, []


SFR_SAFE = [0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFC, 0xFD, 0xFE]
SADDR_SAFE = list(range(0xFE20, 0xFE28)) + list(range(0xFEE0, 0xFF00))
ABS_SAFE = list(range(0xFE70, 0xFEB0)) + list(range(0xFE20, 0xFE28)) + list(range(0xFEE0, 0xFF00))


WORD_MN = {'MOVW', 'XCHW', 'ADDW', 'SUBW', 'CMPW', 'INCW', 'DECW'}


def gnu_operand(kind, st, r, label, es, mn):
    """Return GNU-syntax text for one pattern operand."""
    pre = 'es:' if es else ''
    word = mn in WORD_MN
    bit = ''
    m = re.match(r'^(.*)\.([0-7])$', kind)
    if m and kind not in ('A', 'X'):
        kind, bit = m.group(1), '.' + m.group(2)
    psw = st['psw']

    def regv(name):
        return st['mem'][reg_addr(psw, name)]

    if kind == '#b':
        return '#%d' % r.byte()
    if kind == '#w':
        return '#0x%x' % (r.next() & 0xFFFF)
    if kind in ('s', 'S'):
        v = r.choice(SADDR_SAFE)
        if kind == 'S':
            v &= ~1
        return '0x%x%s' % (0xF0000 | v, bit)
    if kind in ('f', 'F'):
        v = r.choice(SFR_SAFE)
        if kind == 'F':
            # rl78-elf-run aborts on 16-bit accesses above FFFF8H other than SP
            v = r.choice([0xF0, 0xF2, 0xF4, 0xF6, 0xF8])
        return '0x%x%s' % (0xFFF00 | v, bit)
    if kind == '!a':
        if mn in ('BR', 'CALL'):
            return '!' + label
        v = r.choice(ABS_SAFE)
        if mn.endswith('W') and mn not in ('ROLWC',):
            v &= ~1  # the GNU assembler requires even !addr16 for word access
        return '%s!0x%x%s' % (pre, v, bit)
    if kind == '!!A':
        return '!!' + label
    if kind == '$r':
        return '$' + label
    if kind == '$!R':
        return '$!' + label
    if kind in ('[HL+o]', '[DE+o]'):
        base = regv('L') | (regv('H') << 8) if kind[1] == 'H' else regv('E') | (regv('D') << 8)
        o = r.below(0xFEB0 - base)
        if word:
            o &= ~1
        return '%s[%s+%d]' % (pre, kind[1:3].lower(), o)
    if kind == '[SP+o]':
        return '[sp+%d]' % (r.below(8) & (~1 if word else ~0))
    if kind.startswith('W['):
        idx = kind[2:-1]
        if idx == 'B':
            off = regv('B')
        elif idx == 'C':
            off = regv('C')
        else:
            off = regv('C') | (regv('B') << 8)
        a = 0xFE70 + r.below(0x30)
        if word:
            a &= ~1
        w = (a - off) & 0xFFFF
        return '%s%d[%s]' % (pre, w, idx.lower())
    if kind.startswith('['):
        return pre + kind.lower() + bit
    # literal register / keyword
    return kind.lower() + bit


def make_case(pat, es, core, seed):
    """Build a case dict: init state, asm lines for the instruction(s), kind."""
    st = init_state(seed, core)
    r = st['rng']
    mn, ops = split_pat(pat)
    kind = 'plain'
    if mn in FLOW_BRANCH or mn == 'CALL':
        kind = 'branch'
    if mn in SKIPS:
        kind = 'branch'
    if mn in ('RET', 'RETI', 'RETB'):
        kind = 'ret'
    if mn in ('BRK', 'CALLT'):
        kind = 'vector'
    texts = []
    if mn == 'CALLT':
        ops = []
        texts = ['[0x%x]' % int(pat[7:11], 16)]
    for o in ops:
        texts.append(gnu_operand(o.replace('%', ''), st, r, 'LT%d' % seed, es and '%' in o, mn))
    line = mn.lower() + ((' ' + ', '.join(texts)) if texts else '')
    overrides = {}
    if mn == 'BR' and ops == ['AX'] or mn == 'CALL' and ops and ops[0] in ('AX', 'BC', 'DE', 'HL'):
        overrides['rp'] = ops[0]
    if mn in ('BT', 'BF', 'BTCLR') and r.below(2):
        pass
    if kind == 'ret':
        psw_saved = r.byte()
        if core == 'S1':
            psw_saved &= ~0x28
        overrides['ret_psw'] = psw_saved
    return {'seed': seed, 'core': core, 'pat': pat, 'es': es, 'lines': [line], 'kind': kind,
            'ov': overrides, 'st': st}


SEQ_KINDS_OK = re.compile(r'\[|\$|!!|^SP|SP$|W\[')


def make_seq(table, core, seed, length):
    st = init_state(seed, core)
    r = st['rng']
    lines = []
    pats = []
    cands = []
    for pat, _ in table:
        mn, ops = split_pat(pat)
        if mn in FLOW_BRANCH or mn in SKIPS or mn in ('CALL', 'CALLT', 'RET', 'RETI', 'RETB', 'BRK',
                                                       'HALT', 'STOP', 'PUSH', 'POP'):
            continue
        if core != 'S3' and mn in S3_ONLY:
            continue
        if core == 'S1' and mn == 'SEL':
            continue
        if any(SEQ_KINDS_OK.search(o.replace('%', '')) for o in ops):
            continue
        if any(o in ('F', 'f') or o.startswith('f.') or o.startswith('F.') for o in ops):
            continue  # sfr writes may change SP/PSW banks; covered by single cases
        if 'SP' in ops or mn == 'SEL':
            continue
        if mn in ('MACH', 'MACHU'):
            continue  # GNU MACR defect (classified as 'macr' in single cases)
        if any(o in ('PSW', 'ES', 'CS') for o in ops):
            continue  # would only re-expose the classified AC / ES-CS width differences
        cands.append(pat)
    for _ in range(length):
        pat = r.choice(cands)
        mn, ops = split_pat(pat)
        texts = [gnu_operand(o.replace('%', ''), st, r, 'X', False, mn) for o in ops]
        lines.append(mn.lower() + ((' ' + ', '.join(texts)) if texts else ''))
        pats.append(pat)
    return {'seed': seed, 'core': core, 'pat': 'SEQ', 'es': False, 'lines': lines, 'kind': 'seq',
            'ov': {}, 'st': st, 'pats': pats}


# ----------------------------------------------------------------------------
# program generation

def dump_inline(cid):
    """Dump without disturbing flags: A first, then the rest."""
    out = ['\tmov 0xfff10, #0xa5', '\tmov 0xfff10, #%d' % (cid & 0xFF),
           '\tmov 0xfff10, #%d' % (cid >> 8), '\tmov 0xfff10, a', '\tmov a, psw', '\tmov 0xfff10, a']
    for reg in 'xcbedlh':
        out += ['\tmov a, %s' % reg, '\tmov 0xfff10, a']
    out += ['\tmov a, es', '\tmov 0xfff10, a', '\tmov a, cs', '\tmov 0xfff10, a',
            '\tmovw ax, sp', '\tmov 0xfff10, a', '\tmov a, x', '\tmov 0xfff10, a',
            '\tmovw sp, #0xfc00', '\tcall !dump_mem']
    return out


def case_asm(c, cid, core):
    st = c['st']
    lines = ['C%d:' % cid]
    mem = dict(st['mem'])
    psw = st['psw']
    lt = 'LT%d' % c['seed']
    skip = set()
    # 1. overrides that need a label value are written through AX first
    #    (AX of whatever bank is current; the full init below rewrites it)
    rp = c['ov'].get('rp')
    if rp:
        lo = reg_addr(psw, {'AX': 'X', 'BC': 'C', 'DE': 'E', 'HL': 'L'}[rp])
        lines += ['\tmovw ax, #%s' % lt, '\tmovw !0x%x, ax' % (lo & 0xFFFF)]
        skip |= {lo, lo + 1}
    if c['kind'] == 'ret':
        lines += ['\tmovw ax, #%s' % lt, '\tmovw !0xfe00, ax']
        skip |= {0xFFE00, 0xFFE01}
        mem[0xFFE02] = 0
        mem[0xFFE03] = c['ov']['ret_psw']
    if c['kind'] == 'vector':
        lines += ['\tmovw ax, #%s' % lt, '\tmovw !0xfeb0, ax']
    # 2. memory and register banks
    for a in sorted(mem):
        if a not in skip:
            lines.append('\tmov !0x%x, #%d' % (a & 0xFFFF, mem[a]))
    # 3. special registers, PSW last
    lines += ['\tmov es, #%d' % st['es'], '\tmov cs, #%d' % st['cs'], '\tmovw sp, #0x%x' % st['sp'],
              '\tmov psw, #%d' % psw]
    lines.append('T%d:' % cid)
    for ln in c['lines']:
        lines.append('\t' + ln)
    if c['kind'] in ('branch', 'vector'):
        lines.append('\tmov !0xfe7f, #0x55')
    lines.append('E%d:' % cid)
    lines.append('%s:' % lt)
    if c['kind'] != 'vector':
        lines += dump_inline(cid)
    return lines


PROLOGUE = r'''
	.section .vec,"ax"
	.short	start
	.section .vt,"ax"
	.short	vhandler
	.section .callt,"ax"
	.rept	32
	.short	vhandler
	.endr
	.text
	.global	start
start:
	movw	sp, #0xfc00
	br	!C0
dump_mem:
	movw	hl, #0xfe20
	mov	b, #8
	call	!dmp
	movw	hl, #0xfe70
	mov	b, #64
	call	!dmp
	movw	hl, #0xfdf8
	mov	b, #16
	call	!dmp
	movw	hl, #0xfee0
	mov	b, #32
	call	!dmp
	movw	hl, #0xfff0
	mov	b, #8
	call	!dmp
	ret
dmp:
	mov	a, [hl]
	mov	0xfff10, a
	incw	hl
	dec	b
	bnz	$dmp
	ret
'''

LDSCRIPT = '''
ENTRY(start)
SECTIONS {
  .vec 0x0 : { KEEP(*(.vec)) }
  .vt 0x7e : { KEEP(*(.vt)) }
  .callt 0x80 : { KEEP(*(.callt)) }
  .text 0x100 : { *(.text) }
  /DISCARD/ : { *(*) }
}
'''


def program(cases, core):
    lines = PROLOGUE.split('\n')
    # BRK / CALLT handler: dump, then continue at the address stored in FEB0
    lines.append('vhandler:')
    lines += dump_inline(0xFFFF)
    lines += ['\tmovw ax, !0xfeb0', '\tbr ax']
    for i, c in enumerate(cases):
        lines += case_asm(c, i, core)
        if c['kind'] == 'vector':
            pass
    lines.append('C%d:' % len(cases))
    lines += ['\tmov a, #0', '\thalt']
    return '\n'.join(lines) + '\n'


# ----------------------------------------------------------------------------
# normalisation of disassembly text


def norm_gnu(text):
    t = re.sub(r'<[^>]*>', '', text).strip().lower()
    t = re.sub(r'\s+', ' ', t)
    mn, _, ops = t.partition(' ')
    out = []
    for o in [x.strip() for x in ops.split(',')] if ops else []:
        o = o.replace(' ', '')
        out.append(norm_operand_gnu(o))
    return [mn] + out


def num_tok(v):
    return 'N%x' % v


def norm_operand_gnu(o):
    es = ''
    if o.startswith('es:'):
        es, o = 'es:', o[3:]
    bit = ''
    m = re.match(r'^(.*)\.([0-7])$', o)
    if m and not re.match(r'^\d+$', o):
        o, bit = m.group(1), '.' + m.group(2)
    if o.startswith('#'):
        v = int(o[1:], 0)
        o = '#' + num_tok(v & 0xFFFF)
    elif o.startswith('!!'):
        o = '!!' + num_tok(int(o[2:], 16))
    elif o.startswith('!'):
        o = '!' + num_tok(int(o[1:], 16) & 0xFFFF)
    elif o.startswith('$!'):
        o = '$' + num_tok(int(o[2:], 16))
    elif o.startswith('$'):
        o = '$' + num_tok(int(o[1:], 16))
    elif re.match(r'^\[(hl|de|sp)\+(\d+)\]$', o):
        m = re.match(r'^\[(hl|de|sp)\+(\d+)\]$', o)
        o = '[%s+%s]' % (m.group(1), num_tok(int(m.group(2))))
    elif re.match(r'^(\d+)\[(b|c|bc)\]$', o):
        m = re.match(r'^(\d+)\[(b|c|bc)\]$', o)
        o = '%s[%s]' % (num_tok(int(m.group(1))), m.group(2))
    elif re.match(r'^\[[0-9a-f]+\]$', o) and o not in ('[de]', '[hl]'):
        o = '[%s]' % num_tok(int(o[1:-1], 16))
    elif o.startswith('0x'):
        o = num_tok(int(o, 16) & 0xFFFF)
    elif o == 'sp' or o in ('spl', 'sph', 'psw', 'cs', 'es', 'pmc', 'mem'):
        pass
    return es + o + bit


SFRNAMES = {'spl': 0xFFF8, 'sph': 0xFFF9, 'psw': 0xFFFA, 'cs': 0xFFFC, 'es': 0xFFFD, 'pmc': 0xFFFE,
            'mem': 0xFFFF}


def norm_mine(text):
    t = text.strip().lower()
    mn, _, ops = t.partition(' ')
    out = []
    for o in [x.strip() for x in ops.split(',')] if ops else []:
        out.append(norm_operand_mine(o.replace(' ', '')))
    # MOVW rp, !0FFF8H is how the GNU disassembler spells MOVW rp, SP
    if mn == 'movw' and len(out) == 2 and out[1] == '!Nfff8' and out[0] in ('bc', 'de', 'hl'):
        out[1] = 'sp'
    return [mn] + out


def hnum(s):
    return int(s[:-1], 16)


def norm_operand_mine(o):
    es = ''
    if o.startswith('es:'):
        es, o = 'es:', o[3:]
    bit = ''
    m = re.match(r'^(.*)\.([0-7])$', o)
    if m:
        o, bit = m.group(1), '.' + m.group(2)
    if o.startswith('#'):
        o = '#' + num_tok(hnum(o[1:]))
    elif o.startswith('!!'):
        o = '!!' + num_tok(hnum(o[2:]))
    elif o.startswith('!'):
        o = '!' + num_tok(hnum(o[1:]) & 0xFFFF)
    elif o.startswith('$!'):
        o = '$' + num_tok(hnum(o[2:]))
    elif o.startswith('$'):
        o = '$' + num_tok(hnum(o[1:]))
    elif re.match(r'^\[(hl|de|sp)\+([0-9a-f]+h)\]$', o):
        m = re.match(r'^\[(hl|de|sp)\+([0-9a-f]+h)\]$', o)
        o = '[%s+%s]' % (m.group(1), num_tok(hnum(m.group(2))))
    elif re.match(r'^([0-9a-f]+h)\[(b|c|bc)\]$', o):
        m = re.match(r'^([0-9a-f]+h)\[(b|c|bc)\]$', o)
        o = '%s[%s]' % (num_tok(hnum(m.group(1))), m.group(2))
    elif re.match(r'^\[[0-9a-f]+h\]$', o):
        o = '[%s]' % num_tok(hnum(o[1:-1]))
    elif re.match(r'^[0-9a-f]+h$', o):
        o = num_tok(hnum(o) & 0xFFFF)
    return es + o + bit


def canon_names(tokens):
    """map fixed SFR names and numeric forms onto one spelling"""
    out = []
    for t in tokens:
        base, dot, bit = t.partition('.')
        if base in SFRNAMES and not (tokens[0] in ('mov', 'movw') and base in ('psw', 'cs', 'es') and False):
            pass
        out.append(t)
    return out


# ----------------------------------------------------------------------------


def run(cmd, timeout=120, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, timeout=timeout, **kw)
    except subprocess.TimeoutExpired as e:
        class R:
            pass
        r = R()
        r.returncode = -999
        r.stdout = e.stdout or (b'' if 'text' not in kw else '')
        r.stderr = (e.stderr or b'') if 'text' not in kw else ''
        print('\n  timeout: %s' % ' '.join(cmd[:4]))
        return r


def parse_dumps(data):
    """split the console stream into per-case dumps"""
    dumps = collections.OrderedDict()
    i = 0
    while i + DUMP_LEN <= len(data):
        if data[i] != 0xA5:
            i += 1
            continue
        cid = data[i + 1] | (data[i + 2] << 8)
        dumps.setdefault(cid, []).append(bytes(data[i:i + DUMP_LEN]))
        i += DUMP_LEN
    return dumps


AC_MN = {'ADD', 'ADDC', 'SUB', 'SUBC', 'CMP', 'CMPS', 'DEC', 'INC', 'ADDW', 'SUBW', 'CMPW', 'SEQ'}

KNOWN = {
    'ac': 'GDB computes AC with a nibble heuristic ((after&0F) < (before&0F) and bit 4 changed), '
          'not the carry/borrow between bit 3 and bit 4 the manual defines (6.3); for ADDW/SUBW/CMPW '
          'the manual leaves AC undefined',
    'cmps': 'CMPS CY: manual 6.3 sets CY also when the result is non-zero or A or X is 0; GDB uses the borrow',
    'spflags': 'GDB changes PSW on ADDW SP,#byte / SUBW SP,#byte (manual: flags unchanged)',
    'callsp1': 'CALL/CALLT: manual Figure 3-5 shows 00H stored at SP-1; GDB writes only SP-2..SP-4',
    'pushpsw': 'PUSH/POP PSW: manual stores PSW at SP-1 and 00H at SP-2 (Figure 3-5, 4-23); GDB swaps them',
    'escs': 'ES/CS are 4-bit registers (Figure 3-8); GDB keeps all 8 bits',
    'spodd': 'SP bit 0 is always 0 (3.1.3); GDB accepts an odd SP',
    's1rbs': 'RL78-S1 has no RBS bits (Figure 3-2); GDB -M g10 lets PSW.RBS be written',
    'macr': 'MACH/MACHU: GDB reads MACR (FFFF0H-FFFF3H) incorrectly (e.g. after MOVW !0FFF2H,AX=1234H it reads 3434H)',
    'mdu8': 'G13 multiplier: product registers after 8-bit/bit writes to MDAL/MDAH (GDB waits until all '
            '4 bytes were written; this core recomputes on every write) - hardware behaviour not verified',
}


def classify(c, core, g, o):
    names = ['hdr0', 'id_lo', 'id_hi'] + REGNAMES
    for base, n in REGIONS:
        names += ['%05X' % (base + k) for k in range(n)]
    fields = {names[k] for k in range(min(len(g), len(o))) if g[k] != o[k]}
    pat = c['pat']
    mn = 'SEQ' if pat == 'SEQ' else split_pat(pat)[0]
    pswx = g[4] ^ o[4]
    bank = {f for f in fields if f.startswith('FFE') and 'FFEE0' <= f <= 'FFEFF'}
    if core == 'S1' and (pswx & 0x28):
        return 's1rbs'
    if fields == {'PSW'} and pswx == 0x10 and mn in AC_MN:
        return 'ac'
    if fields == {'PSW'} and mn == 'CMPS' and not (pswx & ~0x11):
        return 'cmps'
    if fields == {'PSW'} and mn in ('ADDW', 'SUBW') and 'SP' in pat:
        return 'spflags'
    if mn == 'CALL' and fields == {'FFDFF'}:
        return 'callsp1'
    if pat in ('PUSH PSW', 'POP PSW'):
        return 'pushpsw'
    if fields and fields <= {'ES', 'CS'}:
        return 'escs'
    if 'SPL' in fields and fields - {'SPL'} <= bank:
        return 'spodd'
    if mn == 'SEQ' and fields <= {'PSW', 'ES', 'CS'} and not (pswx & ~0x10):
        return 'ac'  # sequence: AC heuristic and/or ES/CS width, both documented above
    if mn in ('MACH', 'MACHU') and fields <= {'PSW', 'FFFF0', 'FFFF1', 'FFFF2', 'FFFF3'}:
        return 'macr'
    if core == 'S2' and fields and fields <= {'FFFF4', 'FFFF5', 'FFFF6', 'FFFF7'}:
        return 'mdu8'
    return None


def describe_diff(a, b):
    names = ['hdr0', 'id_lo', 'id_hi'] + REGNAMES
    for base, n in REGIONS:
        names += ['%05X' % (base + k) for k in range(n)]
    diffs = []
    for k in range(min(len(a), len(b))):
        if a[k] != b[k]:
            diffs.append('%s gnu=%02X ours=%02X' % (names[k], a[k], b[k]))
    return ', '.join(diffs[:12])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--instances', type=int, default=int(os.environ.get('RL78_DT_INSTANCES', '6')))
    ap.add_argument('--seqs', type=int, default=int(os.environ.get('RL78_DT_SEQS', '300')))
    ap.add_argument('--cores', default='S1,S2,S3')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--per-elf', type=int, default=90)
    ap.add_argument('--write-vectors', default=None)
    ap.add_argument('--keep', default=None)
    ap.add_argument('--rl78dt', default=os.environ.get('RL78DT', os.path.join(RL78, 'build', 'rl78dt')))
    args = ap.parse_args()

    gnu = os.environ.get('RL78_GNU_BIN')
    if not gnu or not os.path.exists(os.path.join(gnu, 'rl78-elf-run')):
        print('difftest: SKIP (set RL78_GNU_BIN to the directory with rl78-elf-as/ld/objcopy/objdump/run)')
        return 0
    tool = lambda n: os.path.join(gnu, 'rl78-elf-' + n)
    table = load_table(args.rl78dt)
    work = args.keep or tempfile.mkdtemp(prefix='rl78dt_')
    os.makedirs(work, exist_ok=True)

    stats = collections.Counter()
    state_mismatch = collections.defaultdict(list)
    known = collections.Counter()
    known_ex = {}
    dis_mismatch = collections.defaultdict(list)
    clk_mismatch = collections.defaultdict(list)
    vectors = []
    seed = args.seed * 1000003

    for core in args.cores.split(','):
        mcpu = {'S1': 'g10', 'S2': 'g13', 'S3': 'g14'}[core]
        cases = []
        for pat, _ in table:
            mn, ops = split_pat(pat)
            if mn in ('HALT', 'STOP', 'BRK', 'CALLT'):
                continue  # HALT/STOP/BRK end rl78-elf-run; its CALLT reads the table from F00xxH
            if core != 'S3' and mn in S3_ONLY:
                continue
            if core == 'S1' and mn == 'SEL':
                continue
            for es in ([False, True] if '%' in pat else [False]):
                for _k in range(args.instances):
                    seed += 1
                    cases.append(make_case(pat, es, core, seed))
        for _k in range(args.seqs):
            seed += 1
            cases.append(make_seq(table, core, seed, 8 + (seed % 17)))
        print('difftest %s: %d cases' % (core, len(cases)), flush=True)

        for start in range(0, len(cases), args.per_elf):
            chunk = cases[start:start + args.per_elf]
            base = os.path.join(work, '%s_%05d' % (core, start))
            with open(base + '.s', 'w') as f:
                f.write(program(chunk, core))
            with open(os.path.join(work, 'link.ld'), 'w') as f:
                f.write(LDSCRIPT)
            p = run([tool('as'), '-m' + mcpu, base + '.s', '-o', base + '.o'], text=True)
            if p.returncode:
                print(p.stderr[:3000])
                raise SystemExit('assembler failed for %s' % base)
            p = run([tool('ld'), '-T', os.path.join(work, 'link.ld'), base + '.o', '-o', base + '.elf'], text=True)
            if p.returncode:
                print(p.stderr[:2000])
                raise SystemExit('link failed')
            run([tool('objcopy'), '-O', 'binary', base + '.elf', base + '.bin'])
            syms = {}
            for line in run([tool('nm'), base + '.elf'], text=True).stdout.splitlines():
                parts = line.split()
                if len(parts) == 3:
                    syms[parts[2]] = int(parts[0], 16)
            # objdump of the test instructions
            od = run([tool('objdump'), '-d', base + '.elf'], text=True).stdout
            dis = {}
            for line in od.splitlines():
                m = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$', line)
                if m:
                    dis[int(m.group(1), 16)] = (bytes.fromhex(m.group(2).replace(' ', '')), m.group(3).strip())
            # GNU run
            g = run([tool('run'), '-M', mcpu, '-D', base + '.gclk', base + '.elf'], timeout=30)
            if g.returncode != 0:
                print('\n  %s: rl78-elf-run exit %d %s' % (os.path.basename(base), g.returncode,
                                                         (g.stderr or b'')[:200]))
            ours = run([args.rl78dt, 'run', core, base + '.bin', '0', '0x%x' % syms['start'],
                        '--clk', base + '.oclk'] + (['--mdu'] if core == 'S2' else []))
            if ours.returncode == 255 or ours.returncode == 254:
                print('ours stopped abnormally:', ours.stderr.decode()[:500])
            gd = parse_dumps(g.stdout)
            od_ = parse_dumps(ours.stdout)
            gclk = {}
            if os.path.exists(base + '.gclk'):
                for line in open(base + '.gclk'):
                    a, n = line.split()
                    gclk[int(a, 16)] = int(n)
            oclk = {}
            for line in open(base + '.oclk'):
                a, n = line.split()
                oclk[int(a, 16)] = int(n)
            # disassembly of our side
            addrs = []
            for i, c in enumerate(chunk):
                t0, e0 = syms['T%d' % i], syms['E%d' % i]
                a = t0
                while a < e0 and a in dis:
                    addrs.append(a)
                    a += len(dis[a][0])
            with open(base + '.addrs', 'w') as f:
                f.write('\n'.join('%x' % a for a in addrs) + '\n')
            mine = {}
            for line in run([args.rl78dt, 'dis', core, base + '.bin', '0', base + '.addrs'], text=True).stdout.splitlines():
                a, l, t = line.split(' ', 2)
                mine[int(a, 16)] = (int(l), t)

            for i, c in enumerate(chunk):
                cid = i
                key = c['pat'] + (' (ES)' if c['es'] else '')
                stats['cases'] += 1
                # disassembly / length
                t0, e0 = syms['T%d' % i], syms['E%d' % i]
                a = t0
                ninsn = len(c['lines'])
                for k in range(ninsn):
                    if a not in dis:
                        break
                    gb, gt = dis[a]
                    ml, mt = mine.get(a, (0, '?'))
                    stats['dis'] += 1
                    if ml != len(gb) or norm_gnu(gt) != norm_mine(mt):
                        dis_mismatch[key].append('%05X %s | gnu: %s | ours: %s' % (a, gb.hex(), gt, mt))
                    # clocks of the instruction under test
                    if a in gclk and a in oclk and gclk[a] != oclk[a]:
                        clk_mismatch[split_pat(c['pat'])[0] if c['kind'] != 'seq' else 'SEQ:' + gt.split()[0]].append(
                            '%s: gnu %d ours %d' % (gt, gclk[a], oclk[a]))
                    stats['clk'] += 1
                    a += len(gb)
                # state
                cidk = 0xFFFF if c['kind'] == 'vector' else cid
                gl = gd.get(cidk, [])
                ol = od_.get(cidk, [])
                if c['kind'] == 'vector':
                    # handler dumps carry id FFFF; match by order of vector cases in this chunk
                    vidx = sum(1 for cc in chunk[:i] if cc['kind'] == 'vector')
                    gl = gl[vidx:vidx + 1]
                    ol = ol[vidx:vidx + 1]
                if not gl or not ol:
                    state_mismatch[key].append('seed %d: missing dump (gnu %d, ours %d)' % (c['seed'], len(gl), len(ol)))
                    continue
                if gl[0] != ol[0]:
                    cat = classify(c, core, gl[0], ol[0])
                    if cat:
                        known[cat] += 1
                        known_ex.setdefault(cat, '%s %s: %s' % (core, ' ; '.join(c['lines'])[:80],
                                                                 describe_diff(gl[0], ol[0])))
                    else:
                        state_mismatch[key].append('%s seed %d: %s | %s' % (core, c['seed'], ' ; '.join(c['lines'])[:120],
                                                                           describe_diff(gl[0], ol[0])))
                    continue
                stats['state_ok'] += 1
                # regression vector
                code_lo = t0
                code_hi = syms['E%d' % i]
                code = bytearray()
                raw = open(base + '.bin', 'rb').read()
                code = raw[code_lo:code_hi]
                stop = syms['LT%d' % c['seed']] if c['kind'] != 'vector' else syms['vhandler']
                vectors.append((core, c, code_lo, bytes(code), stop, gl[0], syms['LT%d' % c['seed']]))
            sys.stdout.write('.')
            sys.stdout.flush()
        print()

    # ---- encodings this core treats as undefined, checked against objdump
    undef_dec = undefined_vs_objdump(tool, args.rl78dt, work)

    # ---- report
    print('\n==== difftest summary ====')
    grey = [t for k, t in undef_dec if k == 'grey']
    esx = [t for k, t in undef_dec if k == 'es']
    print('encodings undefined here but decoded by objdump (kept undefined; documented):')
    print('  [grey] %d grey cells of the instruction maps (Tables 5-9..5-12): %s' % (len(grey), '; '.join(grey)))
    print('  [es] %d ES: prefixes before instructions without an ES: form in Table 5-8, e.g. %s' %
          (len(esx), '; '.join(esx[:4])))
    print('cases: %d, state agreed: %d, disassembled insns compared: %d, clock comparisons: %d' %
          (stats['cases'], stats['state_ok'], stats['dis'], stats['clk']))
    print('known differences (GNU simulator deviations / documented interpretations):')
    for k, n in known.most_common():
        print('  [%s] %d case(s): %s\n      e.g. %s' % (k, n, KNOWN[k], known_ex[k]))
    print('UNEXPLAINED state mismatches: %d forms' % len(state_mismatch))
    for k, v in sorted(state_mismatch.items()):
        print('  [state] %s: %d case(s); e.g. %s' % (k, len(v), v[0]))
    print('disassembly mismatches: %d forms' % len(dis_mismatch))
    for k, v in sorted(dis_mismatch.items()):
        print('  [dis] %s: %d; e.g. %s' % (k, len(v), v[0]))
    print('clock mismatches (GNU sim vs ours; ours follow the manual): %d mnemonics' % len(clk_mismatch))
    for k, v in sorted(clk_mismatch.items()):
        c = collections.Counter(v)
        print('  [clk] %s: %d; %s' % (k, len(v), '; '.join('%s x%d' % (t, n) for t, n in c.most_common(3))))
    if args.write_vectors:
        n = write_vectors(args.write_vectors, vectors)
        print('wrote %d regression vectors to %s' % (n, args.write_vectors))
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    bad = len(state_mismatch) + len(dis_mismatch)
    print('difftest RESULT: %s (%d cases, %d agreed, %d known differences, %d unexplained, '
          '%d disassembly mismatches)' % ('PASS' if bad == 0 else 'FAIL', stats['cases'], stats['state_ok'],
                                          sum(known.values()), sum(len(v) for v in state_mismatch.values()),
                                          sum(len(v) for v in dis_mismatch.values())))
    return 1 if bad else 0


def full_state_from_dump(dump):
    """dump (without the 3 header bytes) -> full state with the current
    bank's slots of FFEE0-FFEFF taken from the register part (the dump
    routine itself overwrites some current-bank registers before dumping
    the bank area)."""
    regs = list(dump[:13])
    mem = list(dump[13:])
    psw = regs[1]
    bank_off = REGIONS[0][1] + REGIONS[1][1] + REGIONS[2][1]
    base = bank_off + (0x18 - 8 * bank_of(psw))
    vals = {'X': regs[2], 'A': regs[0], 'C': regs[3], 'B': regs[4], 'E': regs[5], 'D': regs[6],
            'L': regs[7], 'H': regs[8]}
    for i, n in enumerate('XACBEDLH'):
        mem[base + i] = vals[n]
    return regs + mem


def initial_full_state(c, lt):
    st = c['st']
    mem = dict(st['mem'])
    psw = st['psw']
    rp = c['ov'].get('rp')
    if rp:
        lo = reg_addr(psw, {'AX': 'X', 'BC': 'C', 'DE': 'E', 'HL': 'L'}[rp])
        mem[lo], mem[lo + 1] = lt & 0xFF, (lt >> 8) & 0xFF
    if c['kind'] == 'ret':
        mem[0xFFE00], mem[0xFFE01] = lt & 0xFF, (lt >> 8) & 0xFF
        mem[0xFFE02] = 0
        mem[0xFFE03] = c['ov']['ret_psw']
    r = lambda n: mem[reg_addr(psw, n)]
    regs = [r('A'), psw, r('X'), r('C'), r('B'), r('E'), r('D'), r('L'), r('H'), st['es'], st['cs'],
            st['sp'] >> 8, st['sp'] & 0xFF]
    out = []
    for base, n in REGIONS:
        out += [mem.get(base + i, 0) for i in range(n)]
    return regs + out


def undefined_vs_objdump(tool, rl78dt, work):
    """All 1-byte / map-prefixed 2-byte encodings and ES:-prefixed ones that
    this core rejects: list those that objdump nevertheless decodes."""
    seqs = []
    for op in range(256):
        if op in (0x61, 0x71, 0x31):
            seqs += [bytes([op, o, 0x20, 0xFE, 0x05]) for o in range(256)]
            seqs += [bytes([0x11, op, o, 0x20, 0xFE]) for o in range(256)]
        elif op != 0x11:
            seqs.append(bytes([op, 0x20, 0xFE, 0x05, 0x06]))
            seqs.append(bytes([0x11, op, 0x20, 0xFE, 0x05]))
    raw = b''.join(sq.ljust(8, b'\x00') for sq in seqs)
    binp = os.path.join(work, 'undef.bin')
    open(binp, 'wb').write(raw)
    addrs = os.path.join(work, 'undef.addrs')
    open(addrs, 'w').write('\n'.join('%x' % (i * 8) for i in range(len(seqs))) + '\n')
    ours = {}
    for line in run([rl78dt, 'dis', 'S3', binp, '0', addrs], text=True).stdout.splitlines():
        a, l, _t = line.split(' ', 2)
        ours[int(a, 16)] = int(l)
    out = []
    for i, sq in enumerate(seqs):
        if ours.get(i * 8, 1) != 0:
            continue
        p = run([tool('objdump'), '-D', '-b', 'binary', '-m', 'rl78', '--start-address=%d' % (i * 8),
                 '--stop-address=%d' % (i * 8 + 1), binp], text=True)
        m = re.search(r'^\s*0*%x:\s+((?:[0-9a-f]{2} )+)\s*(.*)$' % (i * 8), p.stdout, re.M)
        if m and m.group(2) and 'illegal' not in m.group(2) and '.byte' not in m.group(2) and 'unknown' not in m.group(2):
            kind = 'es' if sq[0] == 0x11 else 'grey'
            out.append((kind, '%s -> %s' % (sq[:2].hex(' '), m.group(2).strip())))
    return out


def write_vectors(path, vectors, per_form=2):
    """Self-contained regression vectors: seed-derived initial state (see
    init_state), case overrides, code bytes at their address, stop PC and the
    expected final state as differences from the initial state."""
    out = ['/* Generated by tests/difftest/difftest.py from cases on which this core and',
           '   the GNU RL78 simulator (rl78-elf-run) agreed.  Do not edit.',
           '   {core, seed, kind(0 plain/branch/seq, 1 ret), rp, ret_psw, code address,',
           '    stop pc, label, code length, code bytes, number of diffs, "idx,val;..."} */']
    seen = collections.Counter()
    n = 0
    for core, c, addr, code, stop, dump, lt in vectors:
        key = (core, c['pat'], c['es'])
        if c['kind'] == 'vector':
            continue
        if c['pat'] != 'SEQ' and seen[key] >= per_form:
            continue
        seen[key] += 1
        ov = c['ov']
        rp = {'AX': 0, 'BC': 1, 'DE': 2, 'HL': 3}.get(ov.get('rp'), -1)
        kind = 1 if c['kind'] == 'ret' else 0
        exp = full_state_from_dump(dump[3:])
        ini = initial_full_state(c, lt)
        diffs = ['%d,%d' % (i, v) for i, (v, w) in enumerate(zip(exp, ini)) if v != w]
        out.append('{%s,%uu,%d,%d,%d,0x%05x,0x%05x,0x%05x,%d,"%s","%s"},' % (
            core[1], c['seed'], kind, rp, ov.get('ret_psw', 0), addr, stop, lt, len(code),
            ''.join('\\x%02x' % b for b in code), ';'.join(diffs)))
        n += 1
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    return n


if __name__ == '__main__':
    sys.exit(main())
