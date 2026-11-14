#!/usr/bin/env python3
"""CA78K0R .lmf (COFF 系) ダンプ/検証ツール。仕様は research/lmf_format.md。

使い方 (カレントを入力フォルダにしないこと):
  python3 -I lmfdump.py FILE.lmf [--sections] [--symbols] [--modules] [--lines]
        [--hex FILE.hex] [--sym FILE.sym] [--map FILE.map] [--check-types]
オプション無しなら全部表示。比較オプションは結果と件数を出し、不一致があれば終了コード 1。
"""
import argparse
import re
import struct
import sys

SYM = 20          # シンボル/補助エントリの大きさ
SCN = 52          # セクションヘッダの大きさ

# 記憶クラス (実測)
C_AUTO, C_STAT, C_REG, C_ARG, C_REGPARM = 0x01, 0x03, 0x04, 0x09, 0x11
C_EQU, C_PUB, C_PUBBIT = 0x20, 0x26, 0x27
C_BLOCK, C_FCN, C_FILE, C_MODULE, C_SEG = 0x64, 0x65, 0x67, 0x77, 0x78

BASE = {0: 'void?', 1: 'void', 2: 'char', 3: 'short', 4: 'int', 5: 'long', 6: 'float',
        7: 'double', 8: 'struct', 9: 'union', 10: 'enum', 11: 'moe', 12: 'unsigned char',
        13: 'unsigned short', 14: 'unsigned int', 15: 'unsigned long'}
BASE_SIZE = {2: 1, 3: 2, 4: 2, 5: 4, 6: 4, 7: 4, 12: 1, 13: 2, 14: 2, 15: 4}
DT_PTR, DT_FCN, DT_ARY = 1, 2, 3
Q_REGISTER = 0x0100
Q_VOLATILE = 0x1000


class Section:
    pass


class Sym:
    def __init__(self, idx):
        self.idx = idx
        self.aux = []

    def __repr__(self):
        return f'<Sym {self.idx} {self.name} cls={self.cls:#x}>'


class Lmf:
    def __init__(self, data):
        self.d = data
        d = data
        (self.magic, self.nscns, self.timdat, self.symptr, self.nsyms, self.opthdr,
         self.flags) = struct.unpack_from('<HHIIIHH', d, 0)
        if self.magic != 0x1148:
            raise ValueError('not an lmf (magic %#x)' % self.magic)
        self.opt = d[20:20 + self.opthdr]
        self.device = self.opt[24:40].split(b'\0')[0].decode('latin1')
        self._sections()
        self._strtab()
        self._symbols()
        self._modules()

    # ---- セクション -------------------------------------------------
    def _sections(self):
        d = self.d
        off = 20 + self.opthdr
        self.sections = []
        for i in range(self.nscns):
            b = d[off + i * SCN: off + (i + 1) * SCN]
            s = Section()
            s.num = i + 1
            s.name = b[:8].split(b'\0')[0].decode('latin1')
            (s.paddr, s.vaddr, s.size, s.fsize, s.ptrA, s.ptrB, s.ptrC, s.ptrD, s.nA,
             s.nlnno, s.k1, s.k2, s.z) = struct.unpack_from('<IIIIIIIIIHBBI', b, 8)
            s.data = d[s.ptrB:s.ptrB + s.fsize] if s.fsize else b''
            s.rom = s.k1 == 0x20
            self.sections.append(s)

    def section(self, scn):
        return self.sections[scn - 1] if 1 <= scn <= len(self.sections) else None

    def image(self):
        """ROM 側の内容を {アドレス: バイト} で返す。"""
        img = {}
        for s in self.sections:
            for j, x in enumerate(s.data):
                img[s.paddr + j] = x
        return img

    def line_numbers(self, s):
        out = []
        for j in range(s.nlnno):
            a, ln = struct.unpack_from('<II', self.d, s.ptrD + 8 * j)
            out.append((a, ln))
        return out

    # ---- 文字列表・シンボル ----------------------------------------
    def _strtab(self):
        self.stroff = self.symptr + self.nsyms * SYM
        self.strlen = struct.unpack_from('<I', self.d, self.stroff)[0] if self.stroff < len(self.d) else 0

    def _str(self, off):
        a = self.stroff + off
        return self.d[a:self.d.index(b'\0', a)].decode('latin1')

    def _name8(self, b):
        if b[:4] == b'\0\0\0\0':
            return self._str(struct.unpack_from('<I', b, 4)[0])
        return b.split(b'\0')[0].decode('latin1')

    def _symbols(self):
        d = self.d
        self.syms = []
        i = 0
        while i < self.nsyms:
            e = d[self.symptr + i * SYM: self.symptr + (i + 1) * SYM]
            s = Sym(i)
            s.name = self._name8(e[:8])
            s.val, s.scn, s.typ, s.cls, s.naux, s.x = struct.unpack_from('<IhHBBH', e, 8)
            for k in range(s.naux):
                a = i + 1 + k
                s.aux.append(d[self.symptr + a * SYM: self.symptr + (a + 1) * SYM])
            self.syms.append(s)
            i += 1 + s.naux

    def file_aux_name(self, s):
        a = s.aux[0]
        if a[:4] == b'\0\0\0\0':
            return self._str(struct.unpack_from('<I', a, 4)[0])
        return a[:14].split(b'\0')[0].decode('latin1')

    # ---- 型 ---------------------------------------------------------
    @staticmethod
    def derived(x):
        """x (u16) を 2 ビットずつ、シンボルに近い側から並べる。"""
        out = []
        while x:
            out.append(x & 3)
            x >>= 2
        return out

    def type_info(self, s):
        """(base, quals, derived 列, 配列次元 or None, 集約サイズ) を返す。"""
        base = s.typ & 0xF
        quals = s.typ & 0xFFF0
        der = self.derived(s.x)
        dims = None
        size = None
        if s.aux and s.cls not in (C_FILE,) and der and der[0] != DT_FCN:
            a = s.aux[0]
            size = struct.unpack_from('<H', a, 6)[0]
            if der[0] == DT_ARY:
                dims = [v for v in struct.unpack_from('<4H', a, 8) if v]
        return base, quals, der, dims, size

    def type_str(self, s, name=None):
        base, quals, der, dims, size = self.type_info(s)
        decl = name if name is not None else ''
        di = 0
        for k, dt in enumerate(der):
            nxt = der[k + 1] if k + 1 < len(der) else None
            if dt == DT_PTR:
                decl = '*' + decl
            elif dt == DT_ARY:
                if nxt == DT_PTR:
                    pass
                n = dims[di] if dims and di < len(dims) else '?'
                di += 1
                if k > 0 and der[k - 1] == DT_PTR:
                    decl = '(' + decl + ')'
                decl += '[%s]' % n
            elif dt == DT_FCN:
                if k > 0 and der[k - 1] == DT_PTR:
                    decl = '(' + decl + ')'
                decl += '()'
        b = BASE.get(base, 'base%d' % base)
        if base == 1 and quals & 0xE000 == 0xE000:
            b = 'void /*interrupt*/'
        pre = ''
        if quals & Q_VOLATILE and not der:
            pre += 'volatile '
        elif quals & 0xF000 and s.typ != 0xE001 and not (quals & 0xE000 == 0xE000):
            pre += '/*q%04x*/ ' % (quals & 0xF000)
        if s.typ == 0x034C:
            b = 'bit'
        return (pre + b + ' ' + decl).strip()

    def type_size(self, s):
        base, quals, der, dims, size = self.type_info(s)
        if der and der[0] == DT_PTR:
            return 2
        if der and der[0] == DT_ARY:
            n = 1
            for v in dims or []:
                n *= v
            rest = der[1:]
            if rest and rest[0] == DT_PTR:
                return n * 2
            return n * BASE_SIZE.get(base, 0)
        if der:
            return None
        return BASE_SIZE.get(base)

    # ---- モジュール/関数/公開シンボル --------------------------------
    def _modules(self):
        S = self.syms
        self.modules = []
        self.publics = []
        self.functions = []
        self.other = []
        cur = None
        i = 0
        n = len(S)
        while i < n:
            s = S[i]
            if s.cls == C_FILE:
                cur = Module()
                cur.file = self.file_aux_name(s)
                cur.is_c = s.typ == 0x3F
                cur.name = None
                cur.contrib = []
                cur.statics = []
                cur.funcs = []
                self.modules.append(cur)
                i += 1
            elif s.cls == C_MODULE:
                cur.name = s.name
                i += 1
            elif s.cls == C_SEG:
                cur.contrib.append((s.scn, s.val, s.name))
                i += 1
            elif (s.cls in (C_PUB, C_STAT) and s.x & 3 == DT_FCN and i + 1 < n
                  and S[i + 1].cls == C_FCN and S[i + 1].name == '.bf'):
                f = Func()
                f.sym = s
                f.module = cur
                f.params, f.locals, f.blocks = [], [], []
                f.static = s.cls == C_STAT
                i += 1
                depth = 0
                while i < n:
                    t = S[i]
                    if t.cls == C_FCN and t.name == '.bf':
                        f.bf = t
                    elif t.cls == C_FCN and t.name == '.ef':
                        f.ef = t
                        i += 1
                        break
                    elif t.cls == C_BLOCK:
                        if t.name == '.bb':
                            depth += 1
                            f.blocks.append((t.val, None, depth))
                        else:
                            depth -= 1
                    elif t.cls in (C_ARG, C_REGPARM):
                        f.params.append(t)
                    else:
                        t.depth = depth
                        f.locals.append(t)
                    i += 1
                cur.funcs.append(f)
                self.functions.append(f)
            elif s.cls in (C_PUB, C_PUBBIT, C_EQU):
                self.publics.append(s)
                i += 1
            elif s.cls == C_STAT and cur is not None:
                cur.statics.append(s)
                i += 1
            else:
                self.other.append(s)
                i += 1

    def module_of_address(self, scn, addr):
        best = None
        for m in self.modules:
            for (sc, val, nm) in m.contrib:
                if sc == scn and val <= addr:
                    if best is None or val >= best[0]:
                        best = (val, m)
        return best[1] if best else None


class Module:
    pass


class Func:
    pass


# ---- 外部ファイル ---------------------------------------------------
def read_hex(path):
    """Intel HEX (拡張セグメント/リニア両対応) を {アドレス: バイト} に。"""
    mem = {}
    base = 0
    for ln in open(path, 'r', errors='replace'):
        ln = ln.strip()
        if not ln.startswith(':'):
            continue
        b = bytes.fromhex(ln[1:])
        if sum(b) & 255:
            raise ValueError('hex checksum error: ' + ln)
        n, a, t = b[0], (b[1] << 8) | b[2], b[3]
        if t == 0:
            for k in range(n):
                mem[base + a + k] = b[4 + k]
        elif t == 2:
            base = ((b[4] << 8) | b[5]) << 4
        elif t == 4:
            base = ((b[4] << 8) | b[5]) << 16
    return mem


def read_sym(path):
    """.sym -> (publics[(attr, addr, name)], {module: [(attr, addr, name)]}, モジュール順)"""
    pub = []
    loc = {}
    order = []
    section = None
    for ln in open(path, 'rb').read().decode('cp932', 'replace').splitlines():
        ln = ln.rstrip('\x1a\r')
        if ln.startswith('#') or ln.startswith('=') or not ln.strip():
            continue
        if ln.startswith(';FF'):
            nm = ln[3:].strip()
            section = nm
            if nm != 'PUBLIC':
                order.append(nm)
                loc[nm] = []
            continue
        # '<' はモジュールのローカル群の先頭にだけ付き、以降の行も同じモジュールのローカル
        if ln.startswith('<'):
            ln = ln[1:]
        local = section not in (None, 'PUBLIC')
        attr, addr, name = int(ln[:2], 16), int(ln[2:7], 16), ln[7:]
        (loc[section] if local else pub).append((attr, addr, name))
    return pub, loc, order


def read_map_segments(path):
    """.map のメモリマップ -> [(出力セグメント, base, size, 属性)] と入力モジュール行 [(seg, module, base, size)]"""
    out, inp = [], []
    lines = open(path, 'rb').read().decode('cp932', 'replace').splitlines()
    pend = None
    cur = None
    for l in lines:
        m = re.match(r'^ {9}(\S+)\s+([0-9A-F]{5})H\s+([0-9A-F]{5})H\s+(\S.*)$', l)
        if m and not l.startswith(' ' * 18):
            cur = (m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip())
            out.append(cur)
            continue
        m = re.match(r'^ {18}(\S+)\s+(\S+)\s+([0-9A-F]{5})H\s+([0-9A-F]{5})H\s*$', l)
        if m:
            inp.append((m.group(1), m.group(2), int(m.group(3), 16), int(m.group(4), 16)))
            pend = None
            continue
        m = re.match(r'^ {18}(\S+)\s+(\S+)\s*$', l)
        if m and not re.search(r'[0-9A-F]{5}H', l):
            pend = (m.group(1), m.group(2))
            continue
        m = re.match(r'^ {30,}([0-9A-F]{5})H\s+([0-9A-F]{5})H\s*$', l)
        if m and pend:
            inp.append((pend[0], pend[1], int(m.group(1), 16), int(m.group(2), 16)))
            pend = None
    return out, inp


def sym_attr(lm, s):
    if s.cls == C_EQU:
        return 0
    if s.cls == C_PUBBIT:
        return 3
    sec = lm.section(s.scn)
    if sec is None:
        return 1
    return 1 if sec.rom else 2


def sym_addr(lm, s):
    if s.cls == C_PUBBIT:
        return (s.val - 0xFFE00) << 3     # ビット位置は 0 と仮定（検証は 1 件のみ）
    return s.val & 0xFFFFF


# ---- 出力 -----------------------------------------------------------
def show_header(lm):
    print('magic=%#06x nscns=%d timdat=%#x symptr=%#x nsyms=%d opthdr=%d flags=%#x'
          % (lm.magic, lm.nscns, lm.timdat, lm.symptr, lm.nsyms, lm.opthdr, lm.flags))
    print('device=%s strtab@%#x len=%d' % (lm.device, lm.stroff, lm.strlen))


def show_sections(lm):
    print('--- sections')
    for s in lm.sections:
        print('%3d %-9s addr=%#08x size=%#06x fsize=%#06x k1=%#04x k2=%#04x nlnno=%d z=%#x %s'
              % (s.num, s.name, s.paddr, s.size, s.fsize, s.k1, s.k2, s.nlnno, s.z,
                 'ROM' if s.rom else 'RAM/BIT'))


def show_modules(lm):
    print('--- modules (link order)')
    for m in lm.modules:
        print('%-22s file=%-20s %s contrib=%d funcs=%d' % (m.name, m.file, 'C' if m.is_c else 'asm',
                                                          len(m.contrib), len(m.funcs)))


def show_symbols(lm):
    print('--- functions')
    for f in lm.functions:
        s = f.sym
        a = s.aux[0]
        size = struct.unpack_from('<I', a, 4)[0]
        ret = lm.type_str(s, '')
        ps = ', '.join(lm.type_str(p, p.name[1:] if p.name.startswith('_') else p.name)
                       for p in f.params) or 'void'
        print('%#07x %-4s %s %s(%s)  size=%d file=%s' % (
            s.val, 'stat' if f.static else 'ext', ret.replace('()', '').strip(),
            s.name, ps, size, f.module.file))
        for p in f.params:
            print('      param %-14s %s  cls=%#x loc=%d' % (p.name, lm.type_str(p), p.cls, p.val))
        for v in f.locals:
            print('      local %-14s %s  cls=%#x loc=%d' % (v.name, lm.type_str(v), v.cls, v.val))
    print('--- public symbols')
    for s in lm.publics:
        sec = lm.section(s.scn)
        mod = lm.module_of_address(s.scn, s.val) if (sec and s.cls == C_PUB and not sec.rom) else None
        print('%#07x %-18s %-34s sec=%-8s mod=%s' % (
            s.val, s.name, lm.type_str(s, '').strip() if s.typ or s.x else '-',
            sec.name if sec else s.scn, mod.name if mod else '-'))
    print('--- module statics')
    for m in lm.modules:
        for s in m.statics:
            print(m.name, s.name, hex(s.val), lm.type_str(s))


def cmp_hex(lm, path):
    ref = read_hex(path)
    img = lm.image()
    bad = [a for a, v in img.items() if ref.get(a) != v]
    extra = [a for a, v in ref.items() if a not in img and v != 0xFF]
    print('[hex] lmf bytes=%d hex bytes=%d mismatch=%d hex-nonFF-not-in-lmf=%d'
          % (len(img), len(ref), len(bad), len(extra)))
    return not bad and not extra


def cmp_sym(lm, path):
    pub, loc, order = read_sym(path)
    mine = set()
    for s in lm.publics:
        mine.add((sym_attr(lm, s), sym_addr(lm, s), s.name))
    for f in lm.functions:
        if not f.static:
            mine.add((1, f.sym.val, f.sym.name))
    lab = lambda st: {t for t in st if re.fullmatch(r'L\d{4}', t[2])}
    # コンパイラ生成の定数ラベル L0000 形式は .sym ではどのモジュールに属するか lmf から決まらないので全体で比較
    lab_sym = lab(set(pub)) | {t for v in loc.values() for t in lab(set(v))}
    lab_mine = {(1, s.val & 0xFFFFF, s.name) for m in lm.modules for s in m.statics
                if re.fullmatch(r'L\d{4}', s.name)}
    print('[sym] L-labels: lmf=%d sym=%d equal=%s' % (len(lab_mine), len(lab_sym), lab_mine == lab_sym))
    mine -= lab(mine)
    ref = set(pub) - lab(set(pub))
    ok = mine == ref and lab_mine == lab_sym
    print('[sym] public: lmf=%d sym=%d only-lmf=%s only-sym=%s'
          % (len(mine), len(ref), sorted(mine - ref)[:5], sorted(ref - mine)[:5]))
    mods = [m.name for m in lm.modules]
    print('[sym] module order equal: %s (%d vs %d)' % (mods == order, len(mods), len(order)))
    ok &= mods == order
    nl = 0
    for m in lm.modules:
        mine_l = set()
        for f in m.funcs:
            if f.static:
                mine_l.add((1, f.sym.val, f.sym.name))
        for s in m.statics:
            if not re.fullmatch(r'L\d{4}', s.name):
                mine_l.add((sym_attr(lm, s), s.val & 0xFFFFF, s.name))
        ref_l = set(loc.get(m.name, [])) - lab(set(loc.get(m.name, [])))
        nl += len(ref_l)
        if mine_l != ref_l:
            ok = False
            print('[sym] local mismatch in', m.name, sorted(mine_l ^ ref_l)[:5])
    print('[sym] local symbols checked: %d' % nl)
    return ok


def cmp_map(lm, path):
    out, inp = read_map_segments(path)
    ok = len(out) == len(lm.sections)
    print('[map] output segments: lmf=%d map=%d' % (len(lm.sections), len(out)))
    for s, (nm, base, size, attr) in zip(lm.sections, out):
        if (s.name, s.paddr & 0xFFFFF, s.size) != (nm, base, size):
            ok = False
            print('  mismatch', s.name, hex(s.paddr), hex(s.size), (nm, hex(base), hex(size)))
    # 入力モジュール行 vs 0x78 エントリ
    mine = {}
    for m in lm.modules:
        for (sc, val, nm) in m.contrib:
            mine.setdefault((nm, m.name), []).append(val & 0xFFFFF)
    bad = 0
    for (seg, mod, base, size) in inp:
        v = mine.get((seg, mod))
        if v is None or base not in v:
            bad += 1
    print('[map] input-module rows=%d not matched by C_SEG entries=%d' % (len(inp), bad))
    ok &= bad == 0
    return ok


def check_types(lm):
    """型サイズの整合: 集約サイズ(aux) と型から求めたサイズ、隣接変数とのアドレス差。"""
    bad = 0
    n = 0
    rows = []
    for s in lm.publics:
        sec = lm.section(s.scn)
        if s.cls != C_PUB or not sec or sec.rom or not (s.typ & 0xF):
            continue
        sz = lm.type_size(s)
        base, q, der, dims, aux_sz = lm.type_info(s)
        n += 1
        if aux_sz is not None and sz is not None and aux_sz != sz:
            bad += 1
            print('size mismatch', s.name, aux_sz, sz)
        rows.append((s.scn, s.val, sz, s.name))
    rows.sort()
    gaps = 0
    exact = pad = 0
    for a, b in zip(rows, rows[1:]):
        if a[0] == b[0] and a[2] is not None:
            d = b[1] - a[1]
            if d < a[2]:
                gaps += 1
                print('overlap', a, b)
            elif d == a[2]:
                exact += 1
            elif d == a[2] + 1:
                pad += 1          # 偶数境界への 1 バイトのパディング
    print('[types] data vars=%d aux-size mismatches=%d address-overlaps=%d '
          'adjacent: exact=%d pad1=%d' % (n, bad, gaps, exact, pad))
    return bad == 0 and gaps == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lmf')
    ap.add_argument('--sections', action='store_true')
    ap.add_argument('--symbols', action='store_true')
    ap.add_argument('--modules', action='store_true')
    ap.add_argument('--lines', action='store_true')
    ap.add_argument('--hex')
    ap.add_argument('--sym')
    ap.add_argument('--map')
    ap.add_argument('--check-types', action='store_true')
    a = ap.parse_args()
    lm = Lmf(open(a.lmf, 'rb').read())
    sel = a.sections or a.symbols or a.modules or a.lines
    show_header(lm)
    if a.sections or not sel and not (a.hex or a.sym or a.map or a.check_types):
        show_sections(lm)
    if a.modules or not sel and not (a.hex or a.sym or a.map or a.check_types):
        show_modules(lm)
    if a.symbols or not sel and not (a.hex or a.sym or a.map or a.check_types):
        show_symbols(lm)
    if a.lines:
        for s in lm.sections:
            if s.nlnno:
                print('lines', s.name, lm.line_numbers(s)[:20])
    ok = True
    if a.hex:
        ok &= cmp_hex(lm, a.hex)
    if a.sym:
        ok &= cmp_sym(lm, a.sym)
    if a.map:
        ok &= cmp_map(lm, a.map)
    if a.check_types:
        ok &= check_types(lm)
    print('RESULT', 'OK' if ok else 'NG')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
