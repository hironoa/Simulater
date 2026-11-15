#!/usr/bin/env python3
"""CA78K0R .lmf (COFF 系) ダンプ/検証ツール。仕様は docs/LMF_FORMAT.md（本ツールがリファレンス実装）。

使い方 (カレントを入力フォルダにしないこと。`python3 -I` で実行する):
  python3 -I tools/lmfdump.py FILE.lmf [--sections] [--symbols] [--modules] [--lines] [--tags] [--vars]
        [--hex FILE.hex | --mot FILE.mot] [--sym FILE.sym] [--map FILE.map] [--check-types]
オプション無しなら全部表示。比較オプションは結果と件数を出し、不一致があれば終了コード 1。

  --tags         構造体・共用体・enum のタグを展開して表示（メンバのオフセット、ビットフィールド、列挙子）
  --vars         全変数の宣言・サイズ・アドレス・初期値（メンバ展開）を表示
  --mot / --hex  メモリイメージを Motorola S レコード / Intel HEX と照合
  --map          .map のセグメント・入力モジュール行に加え、-kp -kl つきならシンボル一覧（公開・ローカル）も照合
  --check-types  型の整合性検査。試験用ソース probe.c をビルドした lmf（シンボル `_g_outer` がある）なら、
                 probe.c の宣言から手で求めた期待値（サイズ・メンバのオフセット・初期値・関数の型）とも照合する
"""
import argparse
import re
import struct
import sys

SYM = 20          # シンボル/補助エントリの大きさ
SCN = 52          # セクションヘッダの大きさ

# 記憶クラス (実測)
C_AUTO, C_STAT, C_REG, C_ARG, C_REGPARM = 0x01, 0x03, 0x04, 0x09, 0x11
C_MOS, C_STRTAG, C_MOU, C_UNTAG, C_ENTAG, C_MOE, C_FIELD = 0x08, 0x0A, 0x0B, 0x0C, 0x0F, 0x10, 0x12
C_EQU, C_PUB, C_PUBBIT = 0x20, 0x26, 0x27
C_BLOCK, C_FCN, C_EOS, C_FILE, C_MODULE, C_SEG = 0x64, 0x65, 0x66, 0x67, 0x77, 0x78
TAG_CLASSES = (C_STRTAG, C_UNTAG, C_ENTAG)
MEMBER_CLASSES = (C_MOS, C_MOU, C_MOE, C_FIELD)
KNOWN_CLASSES = {C_AUTO, C_STAT, C_REG, C_ARG, C_REGPARM, C_EQU, C_PUB, C_PUBBIT, C_BLOCK, C_FCN, C_EOS, C_FILE,
                 C_MODULE, C_SEG} | set(TAG_CLASSES) | set(MEMBER_CLASSES)

T_VOID, T_CHAR, T_SHORT, T_INT, T_LONG, T_FLOAT, T_DOUBLE, T_STRUCT, T_UNION, T_ENUM = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
BASE = {0: 'void?', 1: 'void', 2: 'char', 3: 'short', 4: 'int', 5: 'long', 6: 'float',
        7: 'double', 8: 'struct', 9: 'union', 10: 'enum', 11: 'moe', 12: 'unsigned char',
        13: 'unsigned short', 14: 'unsigned int', 15: 'unsigned long'}
BASE_SIZE = {2: 1, 3: 2, 4: 2, 5: 4, 6: 4, 7: 4, 12: 1, 13: 2, 14: 2, 15: 4}
BASE_SIGNED = {2: True, 3: True, 4: True, 5: True, 12: False, 13: False, 14: False, 15: False}
DT_PTR, DT_FCN, DT_ARY = 1, 2, 3
PTR_NEAR, PTR_FAR = 1, 2            # 補助エントリ [18] の 2 ビット欄（派生型の段ごと）
Q_VOLATILE = 0x1000
Q_CONST = 0x5000                    # const は 0x5000 全体（0x4000 だけの組は未観測）


class Section:
    pass


class Sym:
    def __init__(self, idx):
        self.idx = idx
        self.aux = []

    def __repr__(self):
        return f'<Sym {self.idx} {self.name} cls={self.cls:#x}>'

    def au16(self, off):
        return struct.unpack_from('<H', self.aux[0], off)[0] if self.aux else 0

    def au32(self, off):
        return struct.unpack_from('<I', self.aux[0], off)[0] if self.aux else 0


class Tag:
    """構造体/共用体/enum のタグ定義 (C_STRTAG/C_UNTAG/C_ENTAG … C_EOS)。"""


class Ty:
    """シンボルから復元した型。chain は「シンボルに近い側から」の派生型の列。"""


class Module:
    pass


class Func:
    pass


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
        self.by_idx = {}
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
            self.by_idx[i] = s
            i += 1 + s.naux

    def file_aux_name(self, s):
        a = s.aux[0]
        if a[:4] == b'\0\0\0\0':
            return self._str(struct.unpack_from('<I', a, 4)[0])
        return a[:14].split(b'\0')[0].decode('latin1')

    # ---- タグ (構造体/共用体/enum) ---------------------------------
    def _parse_tag(self, S, i):
        """S[i] がタグ。メンバを C_EOS まで読み、(Tag, 次の i) を返す。"""
        s = S[i]
        t = Tag()
        t.sym = s
        t.idx = s.idx
        t.name = s.name
        t.kind = {C_STRTAG: 'struct', C_UNTAG: 'union', C_ENTAG: 'enum'}[s.cls]
        t.size = s.au16(6)
        t.endndx = s.au32(12)
        t.members = []
        t.eos = None
        t.anon = s.name.startswith('.') and s.name.endswith('fake')
        j = i + 1
        while j < len(S) and S[j].cls != C_EOS:
            t.members.append(S[j])
            j += 1
        if j < len(S):
            t.eos = S[j]
            t.end_ok = t.endndx == S[j].idx + 1 + S[j].naux
            t.eos_ok = S[j].au32(0) == t.idx and S[j].val == t.size and S[j].au16(6) == t.size
            j += 1
        else:
            t.end_ok = t.eos_ok = False
        self.tags[t.idx] = t
        self.tag_order.append(t)
        return t, j

    # ---- 型 ---------------------------------------------------------
    @staticmethod
    def derived(x):
        """x (u16) を 2 ビットずつ、シンボルに近い側から並べる。"""
        out = []
        while x:
            out.append(x & 3)
            x >>= 2
        return out

    def ty(self, s):
        """シンボルの型 (Ty) を復元する。関数シンボルは chain[0]=('f',) で、戻り値の型は残りの段。
        chain の要素は ('p', far, 生の2ビット) / ('a', 要素数, 0) / ('f', None, 0)。"""
        t = Ty()
        t.sym = s
        t.base = s.typ & 0xF
        t.quals = s.typ & 0xFFF0
        t.tag = None
        t.bitwidth = None
        der = self.derived(s.x)
        is_func = bool(der) and der[0] == DT_FCN and s.cls in (C_PUB, C_STAT)
        t.chain = []
        a = s.aux[0] if s.aux else None
        if a is not None and not is_func:
            t.aux_size = struct.unpack_from('<H', a, 6)[0]
            dims = list(struct.unpack_from('<4H', a, 8))
            pk = struct.unpack_from('<H', a, 18)[0]
        else:
            t.aux_size = None
            dims = []
            pk = 0
        ai = 0
        for k, lv in enumerate(der):
            if lv == DT_PTR:
                code = (pk >> (2 * k)) & 3
                t.chain.append(('p', code == PTR_FAR, code))
            elif lv == DT_ARY:
                t.chain.append(('a', dims[ai] if ai < len(dims) else None, 0))
                ai += 1
            else:
                t.chain.append(('f', None, 0))
        if t.base in (T_STRUCT, T_UNION, T_ENUM) and a is not None:
            t.tag = self.tags.get(struct.unpack_from('<I', a, 0)[0])
        if s.cls == C_FIELD and a is not None:
            t.bitwidth = struct.unpack_from('<H', a, 6)[0]
        return t

    def base_size(self, t):
        if t.base in (T_STRUCT, T_UNION, T_ENUM):
            return t.tag.size if t.tag else None
        return BASE_SIZE.get(t.base)

    def size_of(self, t, skip=0):
        """chain[skip:] を適用した型の大きさ。関数そのものは None（関数へのポインタは 2/4）。"""
        n = self.base_size(t)
        for el in reversed(t.chain[skip:]):
            if el[0] == 'p':
                n = 4 if el[1] else 2
            elif el[0] == 'a':
                if n is None or el[1] is None:
                    return None
                n *= el[1]
            else:
                n = None
        return n

    def base_name(self, t):
        q = ''
        if t.quals & 0xE000 != 0xE000:
            if t.quals & Q_CONST == Q_CONST:
                q = 'const '
            elif t.quals & Q_VOLATILE:
                q = 'volatile '
        if t.sym.typ == 0x034C:
            return 'bit'
        if t.base in (T_STRUCT, T_UNION, T_ENUM):
            kind = BASE[t.base]
            if t.tag is None:
                return q + kind + ' ?'
            nm = t.tag.name[1:] if t.tag.name.startswith('_') else t.tag.name
            return q + kind + ' ' + nm
        b = BASE.get(t.base, 'base%d' % t.base)
        if t.base == T_VOID and t.quals & 0xE000 == 0xE000:
            b = 'void /*interrupt*/'
        return q + b

    def decl(self, t, name='', params=None):
        """C 風の宣言文字列。params は関数シンボルの引数並び（文字列）。"""
        s = name
        prev = None
        for el in t.chain:
            if el[0] == 'p':
                s = ('__far *' if el[1] else '*') + s
            elif el[0] == 'a':
                if prev == 'p':
                    s = '(' + s + ')'
                s += '[%s]' % ('?' if el[1] is None else el[1])
            else:
                if prev == 'p':
                    s = '(' + s + ')'
                s += '(%s)' % (params if params is not None else '')
            prev = el[0]
        r = (self.base_name(t) + ' ' + s).strip()
        if t.bitwidth is not None:
            r += ' : %d' % t.bitwidth
        return r

    # 旧 API 互換
    def type_info(self, s):
        t = self.ty(s)
        dims = [el[1] for el in t.chain if el[0] == 'a'] or None
        return t.base, t.quals, [{'p': DT_PTR, 'a': DT_ARY, 'f': DT_FCN}[e[0]] for e in t.chain], dims, t.aux_size

    def type_str(self, s, name=None):
        return self.decl(self.ty(s), name if name is not None else '')

    def type_size(self, s):
        return self.size_of(self.ty(s))

    # ---- モジュール/関数/公開シンボル --------------------------------
    def _modules(self):
        S = self.syms
        self.modules = []
        self.publics = []
        self.functions = []
        self.other = []
        self.tags = {}
        self.tag_order = []
        self.unhandled = []
        cur = None
        i = 0
        n = len(S)

        def note(m, s):
            if m is not None:
                m.seq.append(s)

        def tag_at(i):
            tg, j = self._parse_tag(S, i)
            tg.module = cur
            cur.tags.append(tg)
            note(cur, tg.sym)
            for m in tg.members:
                note(cur, m)
            return j

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
                cur.tags = []
                cur.seq = []          # マップのローカルシンボル一覧に対応する、モジュール内のシンボルの並び
                self.modules.append(cur)
                i += 1
            elif s.cls == C_MODULE:
                cur.name = s.name
                i += 1
            elif s.cls == C_SEG:
                cur.contrib.append((s.scn, s.val, s.name))
                i += 1
            elif s.cls in TAG_CLASSES:
                i = tag_at(i)
            elif (s.cls in (C_PUB, C_STAT) and s.x & 3 == DT_FCN and i + 1 < n
                  and S[i + 1].cls == C_FCN and S[i + 1].name == '.bf'):
                f = Func()
                f.sym = s
                f.module = cur
                f.params, f.locals, f.blocks, f.statics = [], [], [], []
                f.static = s.cls == C_STAT
                if f.static:
                    note(cur, s)
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
                        note(cur, t)
                    elif t.cls in TAG_CLASSES:
                        i = tag_at(i)
                        continue
                    elif t.cls == C_STAT:
                        t.depth = depth
                        f.statics.append(t)
                        note(cur, t)
                    else:
                        t.depth = depth
                        f.locals.append(t)
                        note(cur, t)
                    i += 1
                cur.funcs.append(f)
                self.functions.append(f)
            elif s.cls in (C_PUB, C_PUBBIT, C_EQU):
                self.publics.append(s)
                i += 1
            elif s.cls == C_STAT and cur is not None:
                cur.statics.append(s)
                note(cur, s)
                i += 1
            else:
                self.other.append(s)
                if s.cls not in KNOWN_CLASSES:
                    self.unhandled.append(s)
                i += 1

    def module_of_address(self, scn, addr):
        best = None
        for m in self.modules:
            for (sc, val, nm) in m.contrib:
                if sc == scn and val <= addr:
                    if best is None or val >= best[0]:
                        best = (val, m)
        return best[1] if best else None

    # ---- 変数の展開 (メンバ・配列要素・ビットフィールド) ---------------
    def enum_value(self, m):
        """列挙子の値。値は 16 ビット（int）の 2 の補数で記録される。"""
        v = m.val
        bits = 32 if m.typ & 0xF == T_LONG else 16
        v &= (1 << bits) - 1
        return v - (1 << bits) if v >= 1 << (bits - 1) else v

    def enum_signed(self, tag):
        return any(self.enum_value(m) < 0 for m in tag.members)

    def leaves(self, t, off=0, path='', skip=0):
        """型 t (chain[skip:]) をスカラの葉に展開する。
        葉: dict(path, off, size, kind('int'|'float'|'ptr'|'enum'), signed, far, bitpos, width, ty)"""
        chain = t.chain[skip:]
        if chain and chain[0][0] == 'a':
            n = chain[0][1] or 0
            esz = self.size_of(t, skip + 1)
            for i in range(n):
                yield from self.leaves(t, off + i * esz, '%s[%d]' % (path, i), skip + 1)
            return
        if chain and chain[0][0] == 'p':
            yield dict(path=path, off=off, size=4 if chain[0][1] else 2, kind='ptr', signed=False,
                       far=chain[0][1], bitpos=None, width=None, ty=t)
            return
        if chain:
            return
        if t.base in (T_STRUCT, T_UNION) and t.tag:
            for m in t.tag.members:
                if m.cls not in (C_MOS, C_MOU, C_FIELD):
                    continue
                mt = self.ty(m)
                nm = m.name[1:] if m.name.startswith('_') else m.name
                if m.cls == C_FIELD:
                    yield dict(path=path + '.' + nm, off=off, size=BASE_SIZE[mt.base], kind='int',
                               signed=BASE_SIGNED.get(mt.base, False), far=False, bitpos=m.val,
                               width=mt.bitwidth, ty=mt)
                else:
                    yield from self.leaves(mt, off + (m.val if m.cls == C_MOS else 0), path + '.' + nm)
            return
        if t.base == T_ENUM and t.tag:
            yield dict(path=path, off=off, size=t.tag.size, kind='enum', signed=self.enum_signed(t.tag), far=False,
                       bitpos=None, width=None, ty=t)
            return
        yield dict(path=path, off=off, size=self.base_size(t),
                   kind='float' if t.base in (T_FLOAT, T_DOUBLE) else 'int',
                   signed=BASE_SIGNED.get(t.base, False), far=False, bitpos=None, width=None, ty=t)

    # ---- 初期値の位置 -------------------------------------------------
    INIT_PAIRS = {'@@INIT': '@@R_INIT', '@@INITL': '@@RLINIT', '@@INIS': '@@R_INIS'}

    def init_location(self, s):
        """初期値つき変数 s の初期値が入っているイメージ上のアドレス。無ければ None。"""
        sec = self.section(s.scn)
        if sec is None:
            return None
        if sec.rom:
            return s.val
        rn = self.INIT_PAIRS.get(sec.name)
        if rn is None:
            return None
        rs = [x for x in self.sections if x.name == rn and x.size]     # R_INIT 系は INIT 系と 1:1 で並ぶ
        return rs[0].paddr + (s.val - sec.paddr) if rs else None


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


def read_mot(path):
    """Motorola S レコード (S1/S2/S3 データ。S0 ヘッダと S7/S8/S9 は読み飛ばす) を {アドレス: バイト} に。
    チェックサム (バイト数+アドレス+データの和の 1 の補数) とバイト数を検査し、不一致は ValueError。"""
    mem = {}
    alen = {'1': 2, '2': 3, '3': 4}
    for ln in open(path, 'r', errors='replace'):
        ln = ln.strip()
        if len(ln) < 4 or ln[0] != 'S':
            continue
        t = ln[1]
        b = bytes.fromhex(ln[2:])
        if ((~sum(b[:-1])) & 0xFF) != b[-1] or b[0] != len(b) - 1:
            raise ValueError('mot checksum/length error: ' + ln)
        if t in alen:
            n = alen[t]
            a = int.from_bytes(b[1:1 + n], 'big')
            for k, v in enumerate(b[1 + n:-1]):
                mem[a + k] = v
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


def read_map_symbols(path):
    """.map の `*** Public symbol list ***` / `*** Local symbol list ***`（リンカ -kp -kl）を読む。
    戻り値 (public, local)。一覧が無ければそれぞれ None。
      public: [(モジュール, 属性, 値, 名前)]
      local : {モジュール: [('seg', 属性, 名前) | ('sym', 属性, 値, 名前)]}（出現順。MOD 行は除く）"""
    lines = open(path, 'rb').read().decode('cp932', 'replace').splitlines()
    pub, loc = None, None
    mode = None
    mod = None
    for l in lines:
        if l.startswith('*** Public symbol list'):
            mode, pub, mod = 'pub', [], None
            continue
        if l.startswith('*** Local symbol list'):
            mode, loc, mod = 'loc', {}, None
            continue
        if l.startswith('***'):
            mode = None
            continue
        if mode is None or not l.strip() or l.startswith('MODULE'):
            continue
        if not l.startswith(' '):
            mod = l.strip()
            if mode == 'loc':
                loc.setdefault(mod, [])
            continue
        if re.match(r'^ {1,9}\S', l):          # 一覧の後ろの ' Target chip : …' など
            mode = None
            continue
        m = re.match(r'^ {10}([A-Z]{3,4})?\s+(?:([0-9A-F]{5})H\s+)?(\S+)\s*$', l)
        if not m:
            raise ValueError('map symbol line not understood: ' + l)
        attr, val, name = m.group(1), m.group(2), m.group(3)
        if mode == 'pub':
            pub.append((mod, attr or '', int(val, 16), name))
        elif attr == 'MOD':
            continue
        elif attr in ('CSEG', 'DSEG', 'BSEG') and val is None:
            loc[mod].append(('seg', attr, name))
        else:
            loc[mod].append(('sym', attr or '', int(val, 16), name))
    return pub, loc


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
def pname(s):
    return s.name[1:] if s.name.startswith('_') else s.name


def proto(lm, f):
    """関数の宣言（戻り値の型・引数の名前と型）。"""
    ps = ', '.join(lm.decl(lm.ty(p), pname(p)) for p in f.params) or 'void'
    return ('static ' if f.static else '') + lm.decl(lm.ty(f.sym), pname(f.sym), params=ps)


def where(lm, s):
    sec = lm.section(s.scn)
    return '%s:%#07x' % (sec.name, s.val & 0xFFFFF) if sec else '-'


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
        print('%-22s file=%-20s %s contrib=%d funcs=%d tags=%d' % (m.name, m.file, 'C' if m.is_c else 'asm',
                                                                  len(m.contrib), len(m.funcs), len(m.tags)))


def show_symbols(lm):
    print('--- functions')
    for f in lm.functions:
        s = f.sym
        size = struct.unpack_from('<I', s.aux[0], 4)[0]
        print('%#07x %s  size=%d file=%s' % (s.val, proto(lm, f), size, f.module.file))
        for p in f.params:
            print('      param %-14s %-34s cls=%#x loc=%#x' % (p.name, lm.decl(lm.ty(p)), p.cls, p.val))
        for v in f.locals:
            print('      local %-14s %-34s cls=%#x loc=%d depth=%d' % (v.name, lm.decl(lm.ty(v)), v.cls, v.val,
                                                                    v.depth))
        for v in f.statics:
            print('      static %-13s %-34s cls=%#x at %s depth=%d' % (v.name, lm.decl(lm.ty(v)), v.cls,
                                                                      where(lm, v), v.depth))
    print('--- public symbols')
    for s in lm.publics:
        sec = lm.section(s.scn)
        mod = lm.module_of_address(s.scn, s.val) if (sec and s.cls == C_PUB and not sec.rom) else None
        print('%#07x %-18s %-34s sec=%-8s mod=%s' % (
            s.val, s.name, lm.decl(lm.ty(s)) if s.typ or s.x else '-',
            sec.name if sec else s.scn, mod.name if mod else '-'))
    print('--- module statics')
    for m in lm.modules:
        for s in m.statics:
            print(m.name, s.name, hex(s.val), lm.decl(lm.ty(s)))


def show_tags(lm):
    print('--- tags (%d)' % len(lm.tag_order))
    for t in lm.tag_order:
        nm = t.name[1:] if t.name.startswith('_') else t.name
        print('%s %s  /* sym#%d size=%d end#%d%s */ {' % (t.kind, nm, t.idx, t.size, t.endndx,
                                                          ' anonymous' if t.anon else ''))
        for m in t.members:
            if t.kind == 'enum':
                print('    %s = %d,' % (pname(m), lm.enum_value(m)))
                continue
            mt = lm.ty(m)
            if m.cls == C_FIELD:
                print('    bit %-3d  %s;' % (m.val, lm.decl(mt, pname(m))))
            else:
                off = m.val if m.cls == C_MOS else 0
                print('    +%-4d    %-40s /* size %s */' % (off, lm.decl(mt, pname(m)) + ';', lm.size_of(mt)))
        print('}')


def read_leaf(mem, base, lf):
    """葉 lf の値を mem[base + …] から読む。ビットフィールドは構造体先頭からのビット位置で取り出す。"""
    off = base + lf['off']
    if lf['bitpos'] is not None:
        b0 = lf['bitpos'] >> 3
        nb = ((lf['bitpos'] & 7) + lf['width'] + 7) >> 3
        v = int.from_bytes(bytes(mem[off + b0 + k] for k in range(nb)), 'little') >> (lf['bitpos'] & 7)
        return v & ((1 << lf['width']) - 1)
    raw = bytes(mem[off + k] for k in range(lf['size']))
    if lf['kind'] == 'float':
        return struct.unpack('<f', raw)[0]
    if lf['kind'] == 'ptr' and lf['far']:
        return (raw[2] << 16) | (raw[1] << 8) | raw[0]       # {オフセット16, ページ8, 未使用8}
    v = int.from_bytes(raw, 'little')
    if lf['signed'] and v >= 1 << (8 * lf['size'] - 1):
        v -= 1 << (8 * lf['size'])
    return v


def all_variables(lm):
    """(所属の説明, シンボル) の一覧。公開変数・モジュール内 static・関数内 static。"""
    out = []
    for s in lm.publics:
        sec = lm.section(s.scn)
        if s.cls == C_PUB and sec and (s.typ & 0xF) and not s.x & 3 == DT_FCN:
            out.append(('', s))
    for m in lm.modules:
        for s in m.statics:
            if s.scn > 0 and (s.typ & 0xF) and not s.x & 3 == DT_FCN:
                out.append(('static', s))
        for f in m.funcs:
            for s in f.statics:
                out.append((pname(f.sym) + '()', s))
    return out


def show_vars(lm, mem):
    print('--- variables')
    for who, s in sorted(all_variables(lm), key=lambda e: (e[1].scn, e[1].val)):
        t = lm.ty(s)
        sz = lm.size_of(t) or 0
        print('%#07x %4d %-8s %s  /* %s */' % (s.val & 0xFFFFF, sz, lm.section(s.scn).name,
                                              lm.decl(t, pname(s)), who or 'public'))
        loc = lm.init_location(s)
        if loc is None or any(loc + k not in mem for k in range(sz)):
            continue
        for lf in lm.leaves(t):
            v = read_leaf(mem, loc, lf)
            pos = ('+%d' % (lf['off'] + (lf['bitpos'] >> 3)) + '.bit%d/%d' % (lf['bitpos'] & 7, lf['width'])
                   if lf['bitpos'] is not None else '+%d' % lf['off'])
            print('        %-26s %-10s = %s' % (pname(s) + lf['path'], pos,
                                               '%#x' % v if isinstance(v, int) else v))


def cmp_mem(lm, ref, tag):
    img = lm.image()
    bad = [a for a, v in img.items() if ref.get(a) != v]
    extra = [a for a, v in ref.items() if a not in img and v != 0xFF]
    print('[%s] lmf bytes=%d %s bytes=%d mismatch=%d %s-nonFF-not-in-lmf=%d'
          % (tag, len(img), tag, len(ref), len(bad), tag, len(extra)))
    return not bad and not extra


def cmp_hex(lm, path):
    return cmp_mem(lm, read_hex(path), 'hex')


def cmp_mot(lm, path):
    return cmp_mem(lm, read_mot(path), 'mot')


def cmp_sym(lm, path):
    pub, loc, order = read_sym(path)
    mine = set()
    for s in lm.publics:
        mine.add((sym_attr(lm, s), sym_addr(lm, s), s.name))
    for f in lm.functions:
        if not f.static:
            mine.add((1, f.sym.val, f.sym.name))
    lab = lambda st: {t for t in st if re.fullmatch(r'L\d{4}', t[2])}
    # コンパイラ生成のラベル L0000 形式は .sym ではどのモジュールに属するか lmf から決まらないので全体で比較
    lab_sym = lab(set(pub)) | {t for v in loc.values() for t in lab(set(v))}
    lab_mine = {(sym_attr(lm, s), s.val & 0xFFFFF, s.name) for m in lm.modules for s in m.statics
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
            for s in f.statics:                      # 関数内 static 変数も .sym のローカルに出る
                mine_l.add((sym_attr(lm, s), s.val & 0xFFFFF, s.name))
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
    ok &= cmp_map_symbols(lm, path)
    return ok


def cmp_map_symbols(lm, path):
    """リンカ -kp -kl のシンボル一覧と lmf のシンボルの照合（一覧が無い .map では何もしない）。"""
    pub, loc = read_map_symbols(path)
    if pub is None and loc is None:
        print('[map] no symbol lists (linker -kp/-kl not given): skipped')
        return True
    ok = True
    if pub is not None:
        mine = [(s.val & 0xFFFFF, s.name) for s in lm.publics] + \
               [(f.sym.val & 0xFFFFF, f.sym.name) for f in lm.functions if not f.static]
        ref = [(v, n) for (_, a, v, n) in pub]
        eq = sorted(mine) == sorted(ref)
        ok &= eq
        print('[map] public symbols: lmf=%d map=%d equal=%s%s' % (
            len(mine), len(ref), eq, '' if eq else ' only-lmf=%s only-map=%s' % (
                sorted(set(mine) - set(ref))[:4], sorted(set(ref) - set(mine))[:4])))
        # 公開データ変数のモジュール所属（アドレスから決める規則）と map の MODULE 欄の一致
        mp = {(v, n): m for (m, a, v, n) in pub}
        tot = good = 0
        for s in lm.publics:
            sec = lm.section(s.scn)
            if s.cls == C_PUB and sec and not sec.rom and (s.typ & 0xF):
                tot += 1
                mo = lm.module_of_address(s.scn, s.val)
                good += bool(mo and mp.get((s.val & 0xFFFFF, s.name)) == mo.name)
        if tot:
            print('[map] public data variables whose module (by address rule) equals map MODULE: %d/%d' % (good, tot))
            ok &= good == tot
    if loc is not None:
        nsym = nbad = nseg = 0
        for m in lm.modules:
            mine = [('seg', n) for (_, _, n) in m.contrib] + [('sym', s.val & 0xFFFFF, s.name) for s in m.seq]
            ref = [('seg', e[2]) if e[0] == 'seg' else ('sym', e[2], e[3]) for e in loc.get(m.name, [])]
            if mine != ref:
                ok = False
                nbad += 1
                d = [(a, b) for a, b in zip(mine, ref) if a != b][:3]
                print('[map] local list mismatch in module %s: lmf=%d map=%d first-diff=%s' % (
                    m.name, len(mine), len(ref), d))
            nseg += sum(1 for e in ref if e[0] == 'seg')
            nsym += sum(1 for e in ref if e[0] == 'sym')
        print('[map] local symbol lists: modules=%d (mismatch %d) symbols=%d segments=%d' % (
            len(lm.modules), nbad, nsym, nseg))
    return ok


# ---- 型の整合性検査 -------------------------------------------------
def check_types(lm):
    """型サイズの整合: 補助エントリの全体サイズと型から求めたサイズ、隣接変数とのアドレス差、タグの整合。"""
    bad = 0
    n = 0
    rows = []
    for who, s in all_variables(lm):
        t = lm.ty(s)
        sz = lm.size_of(t)
        n += 1
        if t.aux_size is not None and sz is not None and t.aux_size != sz:
            bad += 1
            print('size mismatch', s.name, t.aux_size, sz)
        if sz is None:
            bad += 1
            print('size unknown', s.name, lm.decl(t, s.name))
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
    ok = bad == 0 and gaps == 0
    ok &= check_tags(lm)
    if lm.unhandled:
        print('[types] unhandled symbol classes:', sorted({hex(s.cls) for s in lm.unhandled}))
        ok = False
    else:
        print('[types] unhandled symbol classes: none')
    if any(s.name == '_g_outer' for s in lm.publics):
        ok &= check_probe(lm)
    return ok


def check_tags(lm):
    """タグの整合: 終端リンク、.eos、メンバがタグの大きさに収まり重ならない、入れ子の大きさ。"""
    if not lm.tag_order:
        print('[types] tags: none')
        return True
    ok = True
    nm = nb = 0
    kinds = {}
    for t in lm.tag_order:
        kinds[t.kind] = kinds.get(t.kind, 0) + 1
        if not (t.end_ok and t.eos_ok):
            ok = False
            print('tag link/eos mismatch', t.name, t.end_ok, t.eos_ok)
        if t.kind == 'enum':
            continue
        spans = []
        for m in t.members:
            mt = lm.ty(m)
            nm += 1
            if m.cls == C_FIELD:
                if m.val + mt.bitwidth > 8 * t.size:
                    ok = False
                    print('bitfield outside', t.name, m.name)
                spans.append((m.val, m.val + mt.bitwidth, m.name))
            else:
                off = m.val if m.cls == C_MOS else 0
                sz = lm.size_of(mt)
                if sz is None or mt.aux_size is not None and mt.aux_size != sz:
                    ok = False
                    print('member size', t.name, m.name, sz, mt.aux_size)
                    continue
                if off + sz > t.size:
                    ok = False
                    print('member outside', t.name, m.name)
                spans.append((8 * off, 8 * (off + sz), m.name))
        if t.kind == 'struct':
            spans.sort()
            for a, b in zip(spans, spans[1:]):
                if b[0] < a[1]:
                    ok = False
                    nb += 1
                    print('member overlap', t.name, a, b)
    print('[types] tags=%s members=%d links/eos-ok=%s member-overlaps=%d' % (kinds, nm, ok, nb))
    return ok


# ---- probe.c の期待値（C の宣言から手で求めた値。lmf からは導いていない） ------------
# (宣言, サイズ)。char と signed char は同じコード 2 になる。typedef は展開される（`.1fake` は無名 struct の仮名）。
PROBE_VARS = {
    'g_c': ('char g_c', 1), 'g_sc': ('char g_sc', 1), 'g_uc': ('unsigned char g_uc', 1),
    'g_s': ('short g_s', 2), 'g_us': ('unsigned short g_us', 2), 'g_i': ('int g_i', 2),
    'g_ui': ('unsigned int g_ui', 2), 'g_l': ('long g_l', 4), 'g_ul': ('unsigned long g_ul', 4),
    'g_f': ('float g_f', 4), 'g_d': ('double g_d', 4),
    'g_se': ('enum small_e g_se', 1), 'g_ne': ('enum neg_e g_ne', 1), 'g_be': ('enum big_e g_be', 2),
    'g_outer': ('struct outer g_outer', 34), 'g_outer_arr': ('struct outer g_outer_arr[2]', 68),
    'g_uni': ('union uni g_uni', 4),
    'g_bits8': ('struct bits8 g_bits8', 1), 'g_bits16': ('struct bits16 g_bits16', 4),
    'g_bitsm': ('struct bits_mixed g_bitsm', 8),
    'g_mat2': ('unsigned char g_mat2[2][3]', 6), 'g_mat3': ('unsigned int g_mat3[2][3][4]', 48),
    'g_iarr': ('int g_iarr[5]', 10),
    'g_p_uc': ('unsigned char *g_p_uc', 2), 'g_p_ui': ('unsigned int *g_p_ui', 2),
    'g_p_outer': ('struct outer *g_p_outer', 2), 'g_p_inner': ('struct inner *g_p_inner', 2),
    'g_pp_uc': ('unsigned char **g_pp_uc', 2), 'g_p_const': ('unsigned char *g_p_const', 2),
    'g_fp_uc': ('unsigned char __far *g_fp_uc', 4), 'g_fp_outer': ('struct outer __far *g_fp_outer', 4),
    'g_parr': ('unsigned char *g_parr[3]', 6), 'g_p_arr': ('unsigned char (*g_p_arr)[3]', 2),
    'g_td_u8': ('unsigned char g_td_u8', 1), 'g_td_u16': ('unsigned int g_td_u16', 2),
    'g_td_u32': ('unsigned long g_td_u32', 4), 'g_td_pt': ('struct .1fake g_td_pt', 8),
    'g_td_pp': ('struct .1fake *g_td_pp', 2),
    'c_tbl': ('const unsigned char c_tbl[4]', 4), 'c_wtbl': ('const unsigned int c_wtbl[2]', 4),
    'c_inner': ('const struct inner c_inner', 6), 'g_vol': ('volatile unsigned char g_vol', 1),
    'g_bss_uc': ('unsigned char g_bss_uc', 1), 'g_bss_ui': ('unsigned int g_bss_ui', 2),
    'g_bss_outer': ('struct outer g_bss_outer', 34),
    'g_fn': ('unsigned int (__far *g_fn)()', 4), 'g_fn_tbl': ('void (__far *g_fn_tbl[2])()', 8),
}
# static: (所属関数 or None, 名前) -> (宣言, サイズ, 関数内のブロックの深さ)
PROBE_STATICS = {
    (None, 's_file_uc'): ('unsigned char s_file_uc', 1, 0), (None, 's_file_ui'): ('unsigned int s_file_ui', 2, 0),
    (None, 's_file_in'): ('struct inner s_file_in', 6, 0), (None, 's_file_bss'): ('unsigned char s_file_bss', 1, 0),
    ('f_static1', 'cnt'): ('unsigned int cnt', 2, 0), ('f_static1', 'buf'): ('unsigned char buf[4]', 4, 0),
    ('f_static2', 'cnt'): ('unsigned int cnt', 2, 0), ('f_static2', 'keep'): ('struct inner keep', 6, 0),
    ('f_static2', 'nested'): ('unsigned char nested', 1, 1),
    (None, 'L0027'): ('struct inner L0027', 6, 0),          # f_ret_struct の戻り値用にコンパイラが作る 6 バイトの一時領域
}
# タグ: 名前 -> (種別, サイズ, [メンバ])。構造体のメンバは (名前, バイトオフセット（ビットフィールドはビット位置）, 宣言)
PROBE_TAGS = {
    'small_e': ('enum', 1, [('SE_A', 0), ('SE_B', 1), ('SE_C', 100)]),
    'neg_e': ('enum', 1, [('NE_M', -3), ('NE_Z', 0), ('NE_P', 7)]),
    'big_e': ('enum', 2, [('BE_A', 0), ('BE_B', 1000)]),
    'inner': ('struct', 6, [('ia', 0, 'unsigned char ia'), ('ib', 2, 'unsigned int ib'), ('ic', 4, 'unsigned char ic')]),
    'outer': ('struct', 34, [('oa', 0, 'unsigned char oa'), ('in', 2, 'struct inner in'), ('ol', 8, 'unsigned long ol'),
                             ('arr', 12, 'unsigned char arr[3]'), ('warr', 16, 'unsigned int warr[2]'),
                             ('ins', 20, 'struct inner ins[2]'), ('tail', 32, 'unsigned char tail')]),
    '.0fake': ('struct', 4, [('lo', 0, 'unsigned char lo'), ('hi', 1, 'unsigned char hi'), ('w', 2, 'unsigned int w')]),
    'uni': ('union', 4, [('ul', 0, 'unsigned long ul'), ('ui', 0, 'unsigned int ui[2]'), ('uc', 0, 'unsigned char uc[4]'),
                         ('parts', 0, 'struct .0fake parts')]),
    'bits8': ('struct', 1, [('b0', 0, 'unsigned char b0 : 1'), ('b1', 1, 'unsigned char b1 : 1'),
                            ('n2', 2, 'unsigned char n2 : 3'), ('n5', 5, 'unsigned char n5 : 3')]),
    'bits16': ('struct', 4, [('f0', 0, 'unsigned int f0 : 1'), ('f1', 1, 'unsigned int f1 : 4'),
                             ('f5', 5, 'unsigned int f5 : 7'), ('f12', 12, 'unsigned int f12 : 4'),
                             ('g0', 16, 'unsigned int g0 : 9'), ('g9', 25, 'unsigned int g9 : 3')]),
    'bits_mixed': ('struct', 8, [('head', 0, 'unsigned char head'), ('m0', 8, 'unsigned char m0 : 2'),
                                 ('m2', 10, 'unsigned char m2 : 6'), ('w0', 16, 'unsigned int w0 : 5'),
                                 ('w5', 21, 'unsigned int w5 : 11'),
                                 ('s0', 32, 'unsigned int s0 : 4'),    # 元は signed int だが lmf は unsigned int(14) と記録
                                 ('tail', 6, 'unsigned char tail')]),
    '.1fake': ('struct', 8, [('x', 0, 'unsigned char x'), ('y', 2, 'unsigned int y'), ('z', 4, 'unsigned long z')]),
}
PROBE_FUNCS = {
    'f_void': 'void f_void(void)',
    'f_u8': 'unsigned char f_u8(unsigned char a)',
    'add2': 'unsigned int add2(unsigned int a, unsigned int b)',
    'f_u32': 'unsigned long f_u32(unsigned long a)',
    'f_mix': 'unsigned long f_mix(unsigned char a, unsigned long b, unsigned int c, unsigned char d)',
    'f_long2': 'long f_long2(long a, long b)',
    'f_float': 'float f_float(float a, float b)',
    'f_ptr': 'unsigned int f_ptr(unsigned char *p, unsigned int n)',
    'f_farptr': 'unsigned char f_farptr(unsigned char __far *p)',
    'f_struct_ptr': 'unsigned int f_struct_ptr(struct outer *p)',
    'f_ret_struct': 'struct inner f_ret_struct(unsigned char a)',
    'f_enum': 'enum small_e f_enum(enum small_e e)',
    'f_static1': 'unsigned int f_static1(void)',
    'f_static2': 'unsigned int f_static2(void)',
    'f_file_static': 'static unsigned char f_file_static(unsigned char a)',
    'f_locals': 'unsigned int f_locals(unsigned int a)',
    'main': 'void main(void)',
}
# 初期値 (変数 -> {葉のパス: 値})。値は probe.c の初期化子。ポインタは ('addr', 変数名, 加算) = &変数 + 加算。
_A = lambda n, k=0: ('addr', n, k)
PROBE_INIT = {
    'g_c': {'': 0x11}, 'g_sc': {'': -0x12}, 'g_uc': {'': 0x13}, 'g_s': {'': 0x1415}, 'g_us': {'': 0x1617},
    'g_i': {'': 0x1819}, 'g_ui': {'': 0x1A1B}, 'g_l': {'': 0x1C1D1E1F}, 'g_ul': {'': 0x20212223},
    'g_f': {'': 1.5}, 'g_d': {'': 2.25}, 'g_se': {'': 100}, 'g_ne': {'': -3}, 'g_be': {'': 1000},
    'g_outer': {'.oa': 0x41, '.in.ia': 0x31, '.in.ib': 0x3233, '.in.ic': 0x34, '.ol': 0x42434445,
                '.arr[0]': 0x46, '.arr[1]': 0x47, '.arr[2]': 0x48, '.warr[0]': 0x494A, '.warr[1]': 0x4B4C,
                '.ins[0].ia': 0x51, '.ins[0].ib': 0x5253, '.ins[0].ic': 0x54,
                '.ins[1].ia': 0x55, '.ins[1].ib': 0x5657, '.ins[1].ic': 0x58, '.tail': 0x4D},
    'g_uni': {'.ul': 0x61626364, '.ui[0]': 0x6364, '.ui[1]': 0x6162, '.uc[0]': 0x64, '.uc[1]': 0x63, '.uc[2]': 0x62,
              '.uc[3]': 0x61, '.parts.lo': 0x64, '.parts.hi': 0x63, '.parts.w': 0x6162},
    'g_bits8': {'.b0': 1, '.b1': 0, '.n2': 5, '.n5': 2},
    'g_bits16': {'.f0': 1, '.f1': 9, '.f5': 0x55, '.f12': 3, '.g0': 0x1AA, '.g9': 5},
    'g_bitsm': {'.head': 0x71, '.m0': 2, '.m2': 0x2A, '.w0': 0x15, '.w5': 0x3FF, '.s0': -3 & 0xF, '.tail': 0x72},
    'g_mat2': {'[0][0]': 0x81, '[0][1]': 0x82, '[0][2]': 0x83, '[1][0]': 0x84, '[1][1]': 0x85, '[1][2]': 0x86},
    'g_iarr': {'[0]': 0x0101, '[1]': 0x0202, '[2]': 0x0303, '[3]': 0x0404, '[4]': 0x0505},
    'g_p_uc': {'': _A('g_uc')}, 'g_p_ui': {'': _A('g_ui')}, 'g_p_outer': {'': _A('g_outer')},
    'g_p_inner': {'': _A('g_outer', 2)}, 'g_pp_uc': {'': _A('g_p_uc')}, 'g_p_const': {'': _A('g_uc')},
    'g_fp_uc': {'': _A('g_uc')}, 'g_fp_outer': {'': _A('g_outer')},
    'g_parr': {'[0]': _A('g_uc'), '[1]': _A('g_uc'), '[2]': _A('g_uc')}, 'g_p_arr': {'': _A('g_mat2', 3)},
    'g_td_u8': {'': 0x91}, 'g_td_u16': {'': 0x9293}, 'g_td_u32': {'': 0x94959697},
    'g_td_pt': {'.x': 0x98, '.y': 0x999A, '.z': 0x9B9C9D9E}, 'g_td_pp': {'': _A('g_td_pt')},
    'c_tbl': {'[0]': 0xA1, '[1]': 0xA2, '[2]': 0xA3, '[3]': 0xA4}, 'c_wtbl': {'[0]': 0xA5A6, '[1]': 0xA7A8},
    'c_inner': {'.ia': 0xA9, '.ib': 0xAAAB, '.ic': 0xAC}, 'g_vol': {'': 0xAD},
    'g_fn': {'': _A('add2')},
    's_file_uc': {'': 0xB1}, 's_file_ui': {'': 0xB2B3}, 's_file_in': {'.ia': 0xB4, '.ib': 0xB5B6, '.ic': 0xB7},
    'f_static1.cnt': {'': 0xC1C2}, 'f_static2.cnt': {'': 0xC3C4},
    'f_static2.keep': {'.ia': 0xC5, '.ib': 0xC6C7, '.ic': 0xC8}, 'f_static2.nested': {'': 0xC9},
}


def check_probe(lm):
    """probe.c の宣言から手で求めた期待値との照合。"""
    ok = True
    pubs = {pname(s): s for s in lm.publics}
    addr = {k: v.val for k, v in pubs.items()}
    for f in lm.functions:
        addr[pname(f.sym)] = f.sym.val

    def fail(*a):
        nonlocal ok
        ok = False
        print('  NG', *a)

    # 変数: 宣言とサイズ
    for name, (d, sz) in PROBE_VARS.items():
        s = pubs.get(name)
        if s is None:
            fail('missing variable', name)
            continue
        t = lm.ty(s)
        if lm.decl(t, name) != d or lm.size_of(t) != sz:
            fail('variable', name, repr(lm.decl(t, name)), lm.size_of(t), 'expected', repr(d), sz)
    # static（ファイル内、関数内、ブロック内）
    st = {}
    for m in lm.modules:
        for s in m.statics:
            if s.scn > 0 and (s.typ & 0xF) and not s.x & 3 == DT_FCN:
                st[(None, pname(s))] = (s, 0)
        for f in m.funcs:
            for s in f.statics:
                st[(pname(f.sym), pname(s))] = (s, s.depth)
    for key, (d, sz, depth) in PROBE_STATICS.items():
        e = st.get(key)
        if e is None:
            fail('missing static', key)
            continue
        s, dp = e
        t = lm.ty(s)
        if lm.decl(t, pname(s)) != d or lm.size_of(t) != sz or dp != depth:
            fail('static', key, repr(lm.decl(t, pname(s))), lm.size_of(t), dp, 'expected', repr(d), sz, depth)
    if len(st) != len(PROBE_STATICS):
        fail('static count', len(st), len(PROBE_STATICS))
    # タグ
    tags = {(t.name[1:] if t.name.startswith('_') else t.name): t for t in lm.tag_order}
    if set(tags) != set(PROBE_TAGS):
        fail('tag set', sorted(set(tags) ^ set(PROBE_TAGS)))
    nmem = 0
    for name, (kind, size, mem) in PROBE_TAGS.items():
        t = tags.get(name)
        if t is None:
            continue
        if t.kind != kind or t.size != size or len(t.members) != len(mem):
            fail('tag', name, t.kind, t.size, len(t.members), 'expected', kind, size, len(mem))
            continue
        for m, e in zip(t.members, mem):
            nmem += 1
            if kind == 'enum':
                if (pname(m), lm.enum_value(m)) != e:
                    fail('enumerator', name, pname(m), lm.enum_value(m), e)
                continue
            mt = lm.ty(m)
            pos = m.val if m.cls in (C_MOS, C_FIELD) else 0
            if (pname(m), pos, lm.decl(mt, pname(m))) != e:
                fail('member', name, pname(m), pos, lm.decl(mt, pname(m)), 'expected', e)
    # 関数の引数と戻り値
    fn = {pname(f.sym): f for f in lm.functions}
    for name, p in PROBE_FUNCS.items():
        f = fn.get(name)
        if f is None or proto(lm, f) != p:
            fail('function', name, proto(lm, f) if f else None, 'expected', p)
    if len(fn) != len(PROBE_FUNCS):
        fail('function count', len(fn), len(PROBE_FUNCS))
    print('[probe] declarations+sizes: variables=%d statics=%d tags=%d members=%d functions=%d  ok=%s'
          % (len(PROBE_VARS), len(PROBE_STATICS), len(tags), nmem, len(PROBE_FUNCS), ok))
    # 初期値: メモリイメージに実際に入っている値を、型から求めた位置（メンバのオフセット・ビット位置）で読んで照合
    img = lm.image()
    nval = nbad = 0
    allv = dict(pubs)
    for key, (s, dp) in st.items():
        allv[(key[0] + '.' + key[1]) if key[0] else key[1]] = s
    for name, expect in PROBE_INIT.items():
        s = allv.get(name)
        if s is None:
            fail('init: missing', name)
            continue
        loc = lm.init_location(s)
        if loc is None:
            fail('init: no location', name)
            continue
        seen = set()
        for lf in lm.leaves(lm.ty(s)):
            exp = expect.get(lf['path'])
            seen.add(lf['path'])
            if exp is None:
                fail('init: unexpected leaf', name, lf['path'])
                continue
            v = read_leaf(img, loc, lf)
            if isinstance(exp, tuple):                  # ポインタ: 指す先のアドレス（near は下位 16 ビット）
                tgt = addr[exp[1]] + exp[2]
                exp = tgt & 0xFFFFF if lf['far'] else tgt & 0xFFFF
            nval += 1
            if v != exp:
                nbad += 1
                fail('init value', name, lf['path'], v, 'expected', exp)
        if seen != set(expect):
            fail('init: missing leaves', name, sorted(set(expect) - seen))
    print('[probe] initial values compared: %d, mismatches: %d' % (nval, nbad))
    # RAM の変数が、C モジュールの寄与範囲を隙間なく（偶数境界の 1 バイト以外）埋めること
    objs = {}
    for who, s in all_variables(lm):
        if not lm.section(s.scn).rom:
            objs.setdefault(s.scn, []).append((s.val, lm.size_of(lm.ty(s))))
    for scn, lst in sorted(objs.items()):
        lst.sort()
        sec = lm.section(scn)
        start = [v for m in lm.modules if m.is_c for (sc, v, nm) in m.contrib if sc == scn][0]
        end = sec.paddr + sec.size
        cur = start
        for a, sz in lst:
            if a < cur or a - cur > 1:
                fail('tiling gap/overlap in', sec.name, hex(cur), hex(a))
            cur = a + sz
        if end - cur > 1:
            fail('tiling end', sec.name, hex(cur), hex(end))
        print('[probe] %s: %d objects fill %#x..%#x (gaps <= 1 byte)' % (sec.name, len(lst), start, end))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lmf')
    ap.add_argument('--sections', action='store_true')
    ap.add_argument('--symbols', action='store_true')
    ap.add_argument('--modules', action='store_true')
    ap.add_argument('--lines', action='store_true')
    ap.add_argument('--tags', action='store_true')
    ap.add_argument('--vars', action='store_true')
    ap.add_argument('--hex')
    ap.add_argument('--mot')
    ap.add_argument('--sym')
    ap.add_argument('--map')
    ap.add_argument('--check-types', action='store_true')
    a = ap.parse_args()
    lm = Lmf(open(a.lmf, 'rb').read())
    sel = a.sections or a.symbols or a.modules or a.lines or a.tags or a.vars
    cmp_ = a.hex or a.mot or a.sym or a.map or a.check_types
    show_header(lm)
    if a.sections or not sel and not cmp_:
        show_sections(lm)
    if a.modules or not sel and not cmp_:
        show_modules(lm)
    if a.symbols or not sel and not cmp_:
        show_symbols(lm)
    if a.tags or not sel and not cmp_:
        show_tags(lm)
    if a.vars:
        show_vars(lm, lm.image())
    if a.lines:
        for s in lm.sections:
            if s.nlnno:
                print('lines', s.name, lm.line_numbers(s)[:20])
    ok = True
    if a.hex:
        ok &= cmp_hex(lm, a.hex)
    if a.mot:
        ok &= cmp_mot(lm, a.mot)
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
