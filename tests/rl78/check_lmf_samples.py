"""実物のみの任意検証。Python は CI の自己完結テストでは不要。"""
import importlib.util
import pathlib
import subprocess
import sys
root, samples, work, checker = map(pathlib.Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location('lmfdump', root / 'tools/lmfdump.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
files = sorted(samples.rglob('*.lmf'))
if len(files) != 5:
    raise SystemExit(f'FAIL: expected 5 samples, got {len(files)}')
counts = [0, 0, 0, 0]
fail = 0
for path in files:
    lm = m.Lmf(path.read_bytes())
    p = subprocess.run([str(checker), str(path), '--disasm'], capture_output=True, text=True)
    (work / (path.stem + '.dump')).write_text(p.stdout + p.stderr)
    if p.returncode:
        print(p.stderr); fail += 1; continue
    img, symbols = {}, set()
    for line in p.stdout.splitlines():
        if line.startswith('B '):
            _, a, v = line.split(); img[int(a, 16)] = int(v, 16)
        elif line.startswith('S '):
            _, a, name = line.split(); symbols.add((int(a, 16), name))
    ref = m.read_hex(path.with_suffix('.hex'))
    bad = sum(ref.get(a) != v for a, v in img.items())
    extra = sum(v != 255 and a not in img for a, v in ref.items())
    aok = not bad and not extra and bool(img)
    pub, loc, _ = m.read_sym(path.with_suffix('.sym'))
    # bit の位置は未確認なので比較対象から明示的に除外する。
    symref = {(a, n) for attr, a, n in pub + [x for group in loc.values() for x in group] if attr != 3}
    got = set(symbols)
    # PUBLIC と local の集合。LMF の bit シンボルは byte address なので除外。
    bits = {s.name for s in lm.syms if s.cls == m.C_PUBBIT}
    got = {(a, n) for a, n in got if n not in bits}
    bok = got == symref
    counts[0] += aok; counts[1] += bok
    print(f'{path.stem}: (a) bytes={len(img)} mismatch={bad} extra={extra}; (b) C={len(got)} sym={len(symref)} equal={bok} bit-excluded={len(bits)}')
    if not bok: print('  only C', sorted(got-symref), 'only sym', sorted(symref-got))
    print(p.stderr.strip())
    cfg = work / (path.stem + '.cfg')
    cfg.write_text('startup_max_insns = 1000000\n')
    sim = root / 'build/sim/sim'
    info = subprocess.run([str(sim), 'info', '--abs', str(path), '--cfg', str(cfg)], capture_output=True, text=True)
    cok = info.returncode == 0
    counts[2] += cok
    print(f'  (c) startup={cok} ' + (info.stderr.strip() if not cok else next((l for l in info.stdout.splitlines() if l.startswith('初期設定終了:')), '')))
    # 機械語証拠は stdout をオプションで保存できる。サンプルはコピーしない。
    if 'LMF_DISASM_OUT' in __import__('os').environ:
        out = pathlib.Path(__import__('os').environ['LMF_DISASM_OUT'])
        out.mkdir(parents=True, exist_ok=True)
        (out / (path.stem + '.txt')).write_text(p.stdout + p.stderr)
    # 発振器 SFR は模擬しない。副作用のない既存 void 関数へ置換して残りの初期化を実行する。
    cfg.write_text('startup_max_insns = 1000000\nstub = R_CGC_Create:R_CGC_Get_ResetSource\n')
    recovered = subprocess.run([str(sim), 'info', '--abs', str(path), '--cfg', str(cfg), '-v'], capture_output=True, text=True)
    rok = recovered.returncode == 0
    symok = '0 mismatched' in recovered.stderr
    print(f'  SYM sidecar mismatch-free={symok}')
    if not symok:
        print(recovered.stderr)
        counts[1] -= bok
    fail += not symok
    counts[2] -= cok
    counts[2] += rok
    print(f'  (c) with clock stub={rok} ' + (recovered.stderr.strip() if not rok else next((l for l in recovered.stdout.splitlines() if l.startswith('初期設定終了:')), '')))
    # 期待値は対応 C ソースから手で導く。無副作用の delay は書いた変数を保持することも確認。
    if path.stem == 'Sample_KeyandLED_100LG':
        case = 'mod,Getcode,segment table,1,1\n#COMMENT,@i,Getcode@@\n,0,63\n,1,6\n,9,103\n,22,0\n'
        rows = 4
    elif path.stem == 'Sample_CSI_100LG':
        case = 'mod,r_csi10_callback_sendend,sets completion flag,1,1\n#COMMENT,end_txif,end_txif\n,0,1\n'
        rows = 1
    elif path.stem == 'Sample_LED_100LG':
        case = 'mod,R_MAIN_UserInit,EI only preserves i,1,1\n#COMMENT,i,i\n,123,123\n'
        rows = 1
    elif path.stem == 'Sample_AD_100LG':
        case = 'mod,delay,delay preserves mun,2,1\n#COMMENT,@t,mun[0],mun[0]\n,3,123,123\n'
        rows = 1
    else:
        case = 'mod,AD_delay,delay preserves time_lenth,2,1\n#COMMENT,@t,time_lenth,time_lenth\n,3,123,123\n'
        rows = 1
    csv = work / (path.stem + '.csv')
    csv.write_text(case)
    run = subprocess.run([str(sim), 'run', '--abs', str(path), '--cfg', str(cfg), str(csv), '-o', str(work / 'result')], capture_output=True, text=True)
    dok = run.returncode == 0 and f'総数 {rows}, OK {rows}, NG 0' in run.stdout
    counts[3] += dok
    print(f'  (d) rows={rows} passed={dok} ' + ('' if dok else run.stdout + run.stderr))
    fail += not aok; fail += not bok; fail += not rok; fail += not dok
print(f'samples=5 a={counts[0]}/5 b={counts[1]}/5 c={counts[2]}/5 d={counts[3]}/5 failures={fail}')
sys.exit(bool(fail))
