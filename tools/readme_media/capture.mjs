// tools/readme_media/capture.mjs - README.html の画像と動画を撮り直す（macOS / Linux）
//
//   ./build.sh
//   node tools/readme_media/capture.mjs
//
// 同梱のルネサス公式サンプル（tests/fixtures/renesas/g22_adc）で sim gui を起動し、
// headless の Chrome を CDP で操作して docs/readme/ に PNG と MP4 を書く。
// 要るもの: Node 22 以上（WebSocket を使う）、Google Chrome、ffmpeg。
// 環境変数 CHROME で Chrome の場所を変えられる。

import { spawn, execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, copyFileSync, readFileSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const SIM = join(ROOT, 'build', 'sim', 'sim');
const OUT = join(ROOT, 'docs', 'readme');
const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const W = 1440, H = 860;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

if (!existsSync(SIM)) { console.error('先に ./build.sh で sim をビルドしてください'); process.exit(1); }
mkdirSync(OUT, { recursive: true });

// ---------------------------------------------------------------- sim gui
function startSim(port) {
  const dir = mkdtempSync(join(tmpdir(), 'readme-sim-'));
  mkdirSync(join(dir, 'DefaultBuild'));
  copyFileSync(join(ROOT, 'tests/fixtures/renesas/g22_adc/r01an6640-rl78g22-adc.abs'), join(dir, 'DefaultBuild/project.abs'));
  // テスト CSV は先頭の mod ブロックだけにする（GUI が再生するのは先頭のブロックだけ）
  const csv = readFileSync(join(ROOT, 'tests/rl78/csv/g22_adc.csv'), 'utf8').split('\n');
  const end = csv.findIndex((l, i) => i > 0 && l.startsWith('mod,'));
  writeFileSync(join(dir, 'test.csv'), csv.slice(0, end).join('\n') + '\n');
  const p = spawn(SIM, ['gui', '--no-browser', '--abs', 'DefaultBuild/project.abs',
    '--func', 'R_Config_ADC_Set_ADChannel', '--period-ms', '5', '--port', String(port)], { cwd: dir, stdio: 'ignore' });
  return { dir, proc: p, url: `http://127.0.0.1:${port}/` };
}

// ---------------------------------------------------------------- CDP
async function startChrome(port) {
  const prof = mkdtempSync(join(tmpdir(), 'readme-chrome-'));
  const proc = spawn(CHROME, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${prof}`,
    '--no-first-run', '--no-default-browser-check', '--hide-scrollbars', '--force-color-profile=srgb',
    '--lang=ja-JP', `--window-size=${W},${H}`, 'about:blank'], { stdio: 'ignore' });
  let targets;
  for (let i = 0; i < 100; i++) {
    try { targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); if (targets.length) break; } catch { /* まだ */ }
    await sleep(100);
  }
  const page = targets.find((t) => t.type === 'page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r) => ws.addEventListener('open', r, { once: true }));
  let id = 0; const wait = new Map(); const handlers = [];
  ws.addEventListener('message', (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && wait.has(msg.id)) { const { res, rej } = wait.get(msg.id); wait.delete(msg.id); msg.error ? rej(new Error(msg.error.message)) : res(msg.result); }
    else if (msg.method) handlers.forEach((h) => h(msg));
  });
  const send = (method, params = {}) => new Promise((res, rej) => { const i = ++id; wait.set(i, { res, rej }); ws.send(JSON.stringify({ id: i, method, params })); });
  return { proc, prof, send, on: (h) => handlers.push(h), close: () => { ws.close(); proc.kill(); } };
}

// 動画用: マウスの位置とクリックを見えるようにする
const CURSOR_JS = `
addEventListener('DOMContentLoaded', () => {
  const c = document.createElement('div');
  c.style.cssText = 'position:fixed;left:0;top:0;width:22px;height:22px;z-index:2147483647;pointer-events:none;transform:translate(-100px,-100px);transition:transform .02s';
  c.innerHTML = '<svg width="22" height="22" viewBox="0 0 22 22"><path d="M3 2l14 9-6.2 1.3L14 19l-2.6 1.2-3.2-6.6L3 18z" fill="#111" stroke="#fff" stroke-width="1.4" stroke-linejoin="round"/></svg>';
  const r = document.createElement('div');
  r.style.cssText = 'position:fixed;width:36px;height:36px;margin:-18px 0 0 -18px;border-radius:50%;border:3px solid #0e8fae;z-index:2147483646;pointer-events:none;opacity:0';
  document.body.append(r, c);
  addEventListener('mousemove', (e) => { c.style.transform = 'translate(' + (e.clientX - 3) + 'px,' + (e.clientY - 2) + 'px)'; }, true);
  addEventListener('mousedown', (e) => {
    r.style.left = e.clientX + 'px'; r.style.top = e.clientY + 'px';
    r.animate([{ opacity: .9, transform: 'scale(.4)' }, { opacity: 0, transform: 'scale(1.3)' }], { duration: 500 });
  }, true);
});`;

function ui(cdp) {
  const ev = async (expr) => (await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  let mx = W / 2, my = H / 2;
  const move = async (x, y, steps = 12) => {
    for (let i = 1; i <= steps; i++) {
      await cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: mx + (x - mx) * i / steps, y: my + (y - my) * i / steps });
      await sleep(16);
    }
    mx = x; my = y;
  };
  const click = async (x, y) => {
    await move(x, y);
    await cdp.send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await cdp.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
    await sleep(250);
  };
  // 条件に合う要素の中心（見つからなければ例外）
  const at = async (finder, label) => {
    for (let i = 0; i < 50; i++) {
      const r = await ev(`(() => { const e = (${finder})(); if (!e) return null; e.scrollIntoView({block:'nearest'}); const b = e.getBoundingClientRect(); return {x:b.x+b.width/2,y:b.y+b.height/2,w:b.width,l:b.x}; })()`);
      if (r) return r;
      await sleep(100);
    }
    throw new Error('見つかりません: ' + label);
  };
  const byText = (sel, text, nth = 0) => `() => [...document.querySelectorAll('${sel}')].filter(e => e.innerText.trim() === ${JSON.stringify(text)} && e.offsetParent)[${nth}]`;
  const byPh = (ph) => `() => document.querySelector('input[placeholder=${JSON.stringify(ph)}]')`;
  const clickOn = async (finder, label) => { const r = await at(finder, label); await click(r.x, r.y); return r; };
  const type = async (text, delay = 60) => { for (const ch of text) { await cdp.send('Input.insertText', { text: ch }); await sleep(delay); } };
  const key = async (k) => {
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: k, code: k, windowsVirtualKeyCode: k === 'Tab' ? 9 : 13 });
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code: k, windowsVirtualKeyCode: k === 'Tab' ? 9 : 13 });
  };
  const selectAll = async () => {
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'a', code: 'KeyA', modifiers: process.platform === 'darwin' ? 4 : 2, commands: ['selectAll'] });
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'a', code: 'KeyA', modifiers: process.platform === 'darwin' ? 4 : 2 });
  };
  // React の select に値を入れる（headless ではドロップダウンが描かれないため）
  const choose = async (finder, value) => ev(`(() => { const s = (${finder})(); const set = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set; set.call(s, ${JSON.stringify(value)}); s.dispatchEvent(new Event('change', {bubbles:true})); })()`);
  const drag = async (x0, x1, y, ms = 900) => {
    await move(x0, y);
    await cdp.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: x0, y, button: 'left', clickCount: 1 });
    const n = Math.max(2, Math.round(ms / 30));
    for (let i = 1; i <= n; i++) { await cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: x0 + (x1 - x0) * i / n, y, buttons: 1 }); await sleep(30); }
    await cdp.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: x1, y, button: 'left', clickCount: 1 });
    mx = x1; my = y;
  };
  const shot = async (name, clip) => {
    const r = await cdp.send('Page.captureScreenshot', { format: 'png', ...(clip ? { clip: { ...clip, scale: 1 } } : {}) });
    writeFileSync(join(OUT, name), Buffer.from(r.data, 'base64'));
    console.log('  ' + name);
  };
  return { ev, click, clickOn, at, byText, byPh, type, key, selectAll, choose, drag, shot, move };
}

async function openPage(cdp, url, cursor) {
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  await cdp.send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
  await cdp.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'light' }] });
  if (cursor) await cdp.send('Page.addScriptToEvaluateOnNewDocument', { source: CURSOR_JS });
  await cdp.send('Page.navigate', { url });
  await sleep(2500);
}

// 動画の撮影（Page.startScreencast のフレームを ffmpeg で MP4 にする）
function recorder(cdp) {
  const frames = [];
  cdp.on((m) => {
    if (m.method !== 'Page.screencastFrame') return;
    frames.push({ data: m.params.data, t: m.params.metadata.timestamp });
    cdp.send('Page.screencastFrameAck', { sessionId: m.params.sessionId });
  });
  return {
    start: () => cdp.send('Page.startScreencast', { format: 'jpeg', quality: 90, maxWidth: W, maxHeight: H, everyNthFrame: 1 }),
    async stop(name, tailSec = 1.5) {
      await cdp.send('Page.stopScreencast');
      const dir = mkdtempSync(join(tmpdir(), 'readme-frames-'));
      let list = '';
      frames.forEach((f, i) => {
        const fn = join(dir, `f${String(i).padStart(5, '0')}.jpg`);
        writeFileSync(fn, Buffer.from(f.data, 'base64'));
        const d = i + 1 < frames.length ? frames[i + 1].t - f.t : tailSec;
        list += `file '${fn}'\nduration ${Math.max(d, 0.001).toFixed(3)}\n`;
      });
      list += `file '${join(dir, `f${String(frames.length - 1).padStart(5, '0')}.jpg`)}'\n`;
      writeFileSync(join(dir, 'list.txt'), list);
      execFileSync('ffmpeg', ['-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', join(dir, 'list.txt'),
        '-vf', 'fps=25,scale=1280:-2:flags=lanczos', '-c:v', 'libx264', '-preset', 'slow', '-crf', '26',
        '-pix_fmt', 'yuv420p', '-movflags', '+faststart', '-an', join(OUT, name)]);
      rmSync(dir, { recursive: true, force: true });
      console.log(`  ${name}（${frames.length} フレーム）`);
    },
  };
}

// ---------------------------------------------------------------- 撮影 1: ライブ実行（画像 2 枚と動画）
async function live() {
  const sim = startSim(18801);
  const cdp = await startChrome(19301);
  try {
    await openPage(cdp, sim.url, true);
    const u = ui(cdp);
    await u.ev(`document.querySelectorAll('div').forEach(()=>{})`);
    await u.shot('gui_first.png');

    const rec = recorder(cdp);
    await rec.start();
    const t0 = Date.now();
    const marks = [];
    const mark = (label) => marks.push({ t: +((Date.now() - t0) / 1000).toFixed(1), label });
    await sleep(800);
    mark('入力を追加');
    // 入力パネル: @channel を追加し、スライダにして上限を 31 にする
    await u.clickOn(u.byPh('入力にする変数を検索'), '入力の検索');
    await u.type('channel');
    await sleep(600);
    await u.clickOn(u.byText('button', '入力に追加', 0), '入力に追加');
    await sleep(500);
    await u.choose(`() => [...document.querySelectorAll('select')].find(s => [...s.options].some(o => o.value === 'slider'))`, 'slider');
    await sleep(700);
    const maxBox = await u.at(`() => { const r = document.querySelector('input[type=range]'); return r && r.parentElement.querySelectorAll('input[type=number],input:not([type])')[1]; }`, 'スライダの上限');
    await u.click(maxBox.x, maxBox.y);
    await u.selectAll();
    await u.type('31');
    await u.key('Tab');
    await sleep(500);
    // 変数モニタ: 戻り値と ADS（0xFFF31）を監視する
    mark('監視を追加');
    await u.clickOn(u.byPh('監視する変数を検索'), '監視の検索');
    await u.type('channel');
    await sleep(600);
    await u.clickOn(u.byText('button', '監視', 1), '戻り値の監視');
    await sleep(400);
    await u.clickOn(u.byPh('監視する変数を検索'), '監視の検索');
    await u.selectAll();
    await u.type('0xFFF31:1');
    await sleep(600);
    await u.clickOn(u.byText('button', '監視', 0), '0xFFF31 の監視');
    await sleep(500);
    // 実行して、スライダを動かす
    mark('実行');
    await u.clickOn(u.byText('button', '実行'), '実行');
    await sleep(1500);
    const sl = await u.at(`() => document.querySelector('input[type=range]')`, 'スライダ');
    const xs = (v) => sl.l + 8 + (sl.w - 16) * v / 31;
    mark('入力を変える');
    await u.drag(xs(0), xs(7), sl.y);
    await sleep(1800);
    await u.drag(xs(7), xs(12), sl.y, 600);   // 範囲外: 戻り値 0x81、ADS は 7 のまま
    await sleep(1800);
    await u.drag(xs(12), xs(18), sl.y, 600);
    await sleep(1800);
    await u.drag(xs(18), xs(30), sl.y, 900);
    await sleep(2200);
    await u.clickOn(u.byText('button', '一時停止'), '一時停止');
    await sleep(1200);
    await u.move(W - 40, H - 40);
    await sleep(600);
    await rec.stop('gui_live.mp4');
    console.log('  区切り: ' + marks.map((m) => `${m.t}s ${m.label}`).join(' / '));
    await u.ev(`document.querySelectorAll('div[style*="2147483647"],div[style*="2147483646"]').forEach(e => e.remove())`);
    await u.shot('gui_live.png');
    // 入力・監視のパネルだけ
    const panel = await u.ev(`(() => { const b = [...document.querySelectorAll('button')].find(e => e.innerText.trim() === '入力・監視'); const box = b.parentElement; const p = box.getBoundingClientRect(); const bottom = Math.max(...[...box.querySelectorAll('*')].filter(e => e.offsetParent).map(e => e.getBoundingClientRect().bottom)); return {x:p.x,y:p.y,width:p.width,height:Math.min(bottom + 10, ${H}) - p.y}; })()`);
    await u.shot('gui_panel.png', panel);
  } finally { cdp.close(); sim.proc.kill(); }
}

// ---------------------------------------------------------------- 撮影 2: テスト CSV の再生
async function csvPlay() {
  const sim = startSim(18802);
  const cdp = await startChrome(19302);
  try {
    await openPage(cdp, sim.url, false);
    const u = ui(cdp);
    await cdp.send('DOM.enable');
    const o = await cdp.send('Runtime.evaluate', { expression: `[...document.querySelectorAll('input[type=file]')].find(e => e.accept === '.csv,text/csv')` });
    if (!o.result.objectId) throw new Error('テスト CSV 再生の入力が見つかりません');
    await cdp.send('DOM.setFileInputFiles', { objectId: o.result.objectId, files: [join(sim.dir, 'test.csv')] });
    await sleep(6000);
    await u.shot('gui_csv.png');
  } finally { cdp.close(); sim.proc.kill(); }
}

console.log('docs/readme/ に書きます');
if (!process.argv.includes('--csv-only')) await live();
await csvPlay();
