#!/usr/bin/env python3
"""開発時のみ: python3 -I tests/rl78/make_probe_expected.py。
LMF の参照解析器から sim list（型付き変数57件）と型サイズ/関数の期待値を作る。
CI は生成済みテキストを cmp / fc /b で比較し、Python を呼ばない。
"""
import importlib.util
from pathlib import Path
root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("lmfdump", root / "tools/lmfdump.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
lm = m.Lmf((root / "tests/ca78k0r_probe/DefaultBuild/ca78k0r_probe.lmf").read_bytes())
vars_ = {s.idx: (who, s) for who, s in m.all_variables(lm)}
assert len(vars_) == 57
rows, listing = [], []
for s in lm.syms:
    if s.idx not in vars_:
        continue
    who, s = vars_[s.idx]
    name = m.pname(s)
    if who.endswith("()"):
        name = who[:-2] + "@" + name
    ty = lm.ty(s)
    tn = lm.decl(ty).strip()
    mod = lm.module_of_address(s.scn, s.val)
    file = mod.file if mod else ""
    rows.append(f"V {s.val:05X} {lm.size_of(ty)} {name} {tn} [{file}]")
    const = " (const)" if lm.section(s.scn).rom or s.typ & 0xF000 == 0x5000 else ""
    listing.append(f"var  0x{s.val:05X} {name:40s} {tn}{const}  [{file}]")
for f in lm.functions:
    ty = lm.ty(f.sym)
    # 関数の最外段を外した戻り値。関数補助は配列寸法を持たない。
    tn = lm.base_name(ty)
    size = lm.base_size(ty) or 0
    row = f"F {f.sym.val:05X} {m.pname(f.sym)} {tn} {size}"
    for p in f.params:
        pt = lm.ty(p)
        row += f" | {m.pname(p)} {lm.decl(pt).strip()} {lm.size_of(pt)}"
    rows.append(row)
rows.append("variables 57 functions 17 warnings 0")
for name, lines in [("probe_types.txt", rows), ("probe_list.txt", listing)]:
    (root / "tests/rl78/expected" / name).write_bytes(("\n".join(lines) + "\n").encode())
