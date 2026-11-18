#!/usr/bin/env node
// `sim gui` の HTTP API と SSE を模擬する開発用サーバ（Node の標準モジュールだけで動く）。
// docs/DESIGN.md「5. GUI モード」の仕様に合わせている。
//
//   node dev/mock-server.mjs [--port 8765] [--period-ms 5] [--html dist/sim_gui.html]
//
// 正弦波・カウンタ・書き込みに反応する値（ゲイン×正弦、ボタンの押下回数、目標値への一次遅れなど）を流す。
// 対象関数ごとに `@引数` を持ち（func1 は @a @b、ctrl_task_5ms は @rpm）、関数を変えるとリセットして、
// 新しい関数にない `@引数` の固定・監視を外して state の dropped に載せる。
// `/api/write` の書き込み（単発・固定）は止まっているときもすぐにはメモリへ書かず、次のステップの開始時に書く
// （単発 → 固定 → CSV 再生 の順。単発は名前ごとに最後の 1 個で、反映待ちとして state の pending に載せる）。

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const arg = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const PORT = Number(arg("--port", process.env.PORT ?? 8765));
let periodMs = Number(arg("--period-ms", 5));
const HTML = path.resolve(arg("--html", path.join(here, "..", "dist", "sim_gui.html")));
const FUNC_ARGS = { sim_driver_main: [], func1: ["@a", "@b"], ctrl_task_5ms: ["@rpm"] };
const FUNCTIONS = Object.keys(FUNC_ARGS);
let func = FUNCTIONS[0];
/** @type {{name:string,kind:"hold"|"watch",message:string}[]} 直近の /api/control・/api/load-csv で外したもの */
let dropped = [];

// ---- 変数の表 ----
/** @type {{name:string,type:string,size:number,kind:string,writable:boolean,members?:string[],init?:number}[]} */
const symbols = [];
const leaf = (name, type, size, kind, writable, init = 0) => symbols.push({ name, type, size, kind, writable, init });
const aggregate = (name, type, size, kind, members) => symbols.push({ name, type, size, kind, writable: true, members });

leaf("g_counter", "uint16_t", 2, "uint", true);
leaf("g_sine", "float", 4, "float", false);
leaf("g_gain", "int16_t", 2, "int", true, 100);
leaf("g_output", "int32_t", 4, "int", false);
leaf("g_switch", "uint8_t", 1, "uint", true);
leaf("g_led", "uint8_t", 1, "uint", false);
leaf("g_button", "uint8_t", 1, "uint", true);
leaf("g_press_count", "uint16_t", 2, "uint", false);
leaf("g_temp", "float", 4, "float", false, 25);
leaf("g_saw", "int16_t", 2, "int", false);
aggregate("g_cmd", "struct cmd_t", 4, "struct", ["mode", "target"]);
leaf("g_cmd.mode", "uint8_t", 1, "uint", true);
leaf("g_cmd.target", "int16_t", 2, "int", true, 500);
aggregate("g_pos", "struct pos_t", 8, "struct", ["g_pos.xy", "g_pos.valid"]);
aggregate("g_pos.xy", "struct xy_t", 4, "struct", ["g_pos.xy.x", "g_pos.xy.y"]);
leaf("g_pos.xy.x", "int16_t", 2, "int", false);
leaf("g_pos.xy.y", "int16_t", 2, "int", false);
leaf("g_pos.valid", "uint8_t", 1, "uint", false);
aggregate("g_buf", "uint8_t[4]", 4, "array", ["[0]", "[1]", "[2]", "[3]"]);
for (let i = 0; i < 4; i++) leaf(`g_buf[${i}]`, "uint8_t", 1, "uint", false);
leaf("drv.c/s_tick", "uint32_t", 4, "uint", false);
leaf("sim_psw_ie", "uint8_t", 1, "uint", true, 1);
leaf("P1", "uint8_t", 1, "uint", true);
leaf("gb_result.data", "int16_t", 2, "int", false);
leaf("gb_result.ret_code", "uint8_t", 1, "uint", false);
leaf("g_ptr", "uint8_t *", 8, "ptr", true);
for (let i = 0; i < 40; i++) leaf(`g_table_${String(i).padStart(2, "0")}`, "uint16_t", 2, "uint", true, i * 10);

const leaves = symbols.filter((s) => !s.members);
const leafIndex = new Map(leaves.map((s, i) => [s.name, i]));
const byName = new Map(symbols.map((s) => [s.name, s]));
let values = new Float64Array(leaves.length);
const resetValues = () => { values = new Float64Array(leaves.map((s) => s.init ?? 0)); };
resetValues();
// 対象関数の引数（`@名前`）。関数を変えたりリセットしたりすると 0 に戻る
let argValues = new Map();
const argSym = (name) => ({ name, type: "int16_t", size: 2, kind: "int", writable: true, init: 0 });
const isArg = (n) => FUNC_ARGS[func].includes(n);
/** 今の関数で解決できる値の名前か */
const resolvable = (n) => leafIndex.has(n) || isArg(n);
const symOf = (n) => (isArg(n) ? argSym(n) : byName.get(n));
const unresolvedMessage = (n) => (n.startsWith("@") ? `引数 '${n}' は関数 ${func} にありません` : `変数 '${n}' が見つかりません`);
const get = (n) => (isArg(n) ? argValues.get(n) ?? 0 : values[leafIndex.get(n)]);
const put = (n, v) => { if (isArg(n)) argValues.set(n, v); else values[leafIndex.get(n)] = v; };

// ---- 実行状態 ----
let variantsDoc = null;
let inits = [];
let running = false;
let step = 0; // 次に実行するステップ番号（記録上は 0 始まり）
let speed = 1;
let overrun = false;
let watch = ["g_counter", "g_sine", "g_output"];
const pending = new Map(); // 次の周期の開始時に 1 回書く（名前 → 書いたセルの文字列）
const holds = new Map(); // 毎周期書く
const inputNames = new Set();
let stepBudget = 0; // ステップ実行の残り
let csvPlay = null; // { names: string[], nIn, rows: (string|undefined)[][], index }
let prevButton = 0;

const RECORD_MAX = 100_000;
let record = []; // [step, Float64Array(values)]
let pendingRows = [];
let pendingStep0 = null;

function clampType(sym, v) {
  if (sym.kind === "float") return v;
  const bits = sym.size * 8;
  v = Math.trunc(v);
  if (sym.kind === "uint" || sym.kind === "ptr") return bits >= 32 ? v >>> 0 : v & ((1 << bits) - 1);
  if (bits >= 32) return v | 0;
  const m = 1 << bits;
  v = ((v % m) + m) % m;
  return v >= m / 2 ? v - m : v;
}

/**
 * テスト CSV のセルと同じ書式（docs/DESIGN.md「3.」の値）の文字列を数値にする（模擬なので先頭の要素だけ）。
 * 10 進、0x 16 進、0b 2 進、負数、先頭の +、末尾の u/l、浮動小数、'文字列'（先頭の文字。'' は 0）、| 区切り。読めなければ null。
 */
function parseCell(text) {
  const t = String(text).trim();
  if (!t) return null;
  if (/^'.*'$/su.test(t)) return t.length > 2 ? t.slice(1).codePointAt(0) : 0;
  const first = t.includes("|") ? t.split("|")[0].trim() : t;
  if (t.includes("|")) return t.split("|").every((c) => parseCell(c) !== null) ? parseCell(first) : null;
  let m = /^([-+]?)0x([0-9a-f]+)[ul]*$/i.exec(first);
  if (m) return (m[1] === "-" ? -1 : 1) * parseInt(m[2], 16);
  m = /^([-+]?)0b([01]+)[ul]*$/i.exec(first);
  if (m) return (m[1] === "-" ? -1 : 1) * parseInt(m[2], 2);
  if (/^[-+]?\d+[ul]*$/i.test(first)) return Number(first.replace(/[ul]+$/i, ""));
  if (/^[-+]?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?$/i.test(first)) {
    const v = Number(first);
    return Number.isFinite(v) ? v : null;
  }
  return null;
}

/** state.holds の値: C 側と同じく、数値にできる値は JSON の数値で返す（2^53 を超える整数は受け手で丸まる） */
function holdJson(value) {
  if (typeof value !== "string") return value;
  const v = parseCell(value);
  return v === null ? value : v;
}

function writeVar(name, value) {
  const sym = symOf(name);
  const v = typeof value === "string" ? parseCell(value) : value;
  put(name, clampType(sym, v !== null && Number.isFinite(v) ? v : 0));
}

function runOneStep() {
  for (const row of inits) if (row.mode === "every" || step === 0) writeVar(row.name, row.value);
  // 周期の開始時に書く: 単発（反映待ち）→ 固定 → CSV 再生 の順（同じ名前は後のものが残る）
  for (const [n, v] of pending) if (resolvable(n)) writeVar(n, v);
  pending.clear();
  for (const [n, v] of holds) writeVar(n, v);
  if (csvPlay) {
    const row = csvPlay.rows[csvPlay.index];
    if (row) {
      for (let i = 0; i < csvPlay.nIn; i++) {
        const cell = row[i];
        const name = csvPlay.names[i];
        if (cell !== undefined && cell !== "" && symOf(name)?.writable) writeVar(name, cell);
      }
      csvPlay.index++;
    } else {
      csvPlay = null;
      running = false;
      broadcastState();
      return false;
    }
  }

  const t = step * periodMs;
  put("g_counter", (get("g_counter") + 1) & 0xffff);
  put("g_sine", Math.fround(100 * Math.sin((2 * Math.PI * t) / 1000)));
  put("g_output", Math.round((get("g_sine") * get("g_gain")) / 100));
  put("g_led", get("g_switch") ? Math.floor(t / 250) % 2 : 0);
  const b = get("g_button") ? 1 : 0;
  if (b && !prevButton) put("g_press_count", (get("g_press_count") + 1) & 0xffff);
  prevButton = b;
  put("g_temp", Math.fround(25 + 5 * Math.sin((2 * Math.PI * t) / 10000) + (Math.random() - 0.5) * 0.4));
  put("g_saw", ((step % 200) - 100) * 10);
  const x = get("g_pos.xy.x");
  put("g_pos.xy.x", Math.round(x + (get("g_cmd.target") - x) * 0.02));
  put("g_pos.xy.y", get("g_cmd.mode") === 1 ? Math.round(200 * Math.cos((2 * Math.PI * t) / 2000)) : 0);
  put("g_pos.valid", Math.abs(get("g_cmd.target") - get("g_pos.xy.x")) < 5 ? 1 : 0);
  for (let i = 0; i < 4; i++) put(`g_buf[${i}]`, (get("g_counter") + i * 10) & 0xff);
  put("drv.c/s_tick", (get("drv.c/s_tick") + periodMs) >>> 0);
  put("gb_result.data", get("g_output"));
  put("gb_result.ret_code", get("g_output") > 0 ? 1 : 0);
  for (const [n, v] of holds) writeVar(n, v);

  if (pendingStep0 === null) pendingStep0 = step;
  pendingRows.push(watch.map((n) => (resolvable(n) ? get(n) : null)));
  record.push([step, Float64Array.from(values)]);
  if (record.length > RECORD_MAX) record = record.slice(record.length - RECORD_MAX * 0.9);
  step++;
  return true;
}

// ---- SSE ----
const clients = new Set();
function send(res, event, data) {
  res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
}
const broadcast = (event, data) => { for (const c of clients) send(c, event, data); };
const stateMsg = () => ({
  inits, running, step, speed, holds: [...holds].map(([name, value]) => ({ name, value: holdJson(value) })), overrun,
  periodMs, func, playback: !!csvPlay, needReset: false, error: "", dropped,
  pending: [...pending].map(([name, value]) => ({ name, value })),
});
let lastStateJson = "";
function broadcastState(force = false) {
  const msg = stateMsg();
  const json = JSON.stringify({ ...msg, step: 0 });
  if (!force && json === lastStateJson) return;
  lastStateJson = json;
  broadcast("state", msg);
}
const helloMsg = () => ({ periodMs, func, names: watch });
// 監視変数の現在値（ステップは進めない）。約 50ms ごとの送信でまとめ、最後の値だけを送る
const valuesMsg = () => ({ step, names: watch, values: watch.map((n) => (resolvable(n) ? get(n) : null)) });
let valuesDirty = false;
const markValues = () => { valuesDirty = true; };
function flushSamples() {
  if (pendingRows.length) {
    broadcast("samples", { step0: pendingStep0, names: watch, rows: pendingRows });
    pendingRows = [];
    pendingStep0 = null;
  }
  if (valuesDirty) {
    valuesDirty = false;
    broadcast("values", valuesMsg());
  }
}
function setWatch(names) {
  flushSamples();
  watch = names;
  broadcast("hello", helloMsg());
  markValues();
}

function resetTarget() {
  flushSamples(); resetValues(); argValues = new Map(); step = 0; record = []; pending.clear(); prevButton = 0; rebase(); markValues();
}

/** 対象関数を変える: リセットして記録を区切り、新しい関数で解決できない固定・監視を外す。 */
function changeFunc(name) {
  func = name;
  csvPlay = null;
  resetTarget();
  for (const n of watch) if (!resolvable(n)) dropped.push({ name: n, kind: "watch", message: unresolvedMessage(n) });
  inits = inits.filter(row => {
    if (resolvable(row.name)) return true;
    dropped.push({ name: row.name, kind: "init", message: unresolvedMessage(row.name) });
    return false;
  });
  for (const n of [...holds.keys()]) {
    if (resolvable(n)) continue;
    dropped.push({ name: n, kind: "hold", message: unresolvedMessage(n) });
    holds.delete(n);
  }
  for (const n of [...inputNames]) if (!resolvable(n)) inputNames.delete(n);
  setWatch(watch.filter(resolvable));
}

// ---- 周期タイマ ----
let base = { time: performance.now(), step: 0 };
const rebase = () => { base = { time: performance.now(), step }; };
setInterval(() => {
  if (stepBudget > 0) {
    const n = Math.min(stepBudget, 5000);
    for (let i = 0; i < n; i++) if (!runOneStep()) { stepBudget = 0; break; }
    stepBudget -= n;
    if (stepBudget <= 0) { stepBudget = 0; running = false; rebase(); broadcastState(); }
    return;
  }
  if (!running) return;
  let due;
  if (speed === 0) {
    due = 2000; // 最速（模擬では 1 回の刻みに 2000 周期まで）
    overrun = false;
  } else {
    const target = base.step + Math.floor(((performance.now() - base.time) * speed) / periodMs);
    due = target - step;
    if (due > 5000) { // 追いつけない: 周期を詰めずに続行する
      overrun = true;
      due = 5000;
      rebase();
    } else if (overrun && due < 50) {
      overrun = false;
    }
  }
  for (let i = 0; i < due; i++) if (!runOneStep()) break;
  broadcastState();
}, 10);
setInterval(flushSamples, 50);
setInterval(() => broadcastState(true), 1000);

// ---- HTTP ----
function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", reject);
  });
}
function json(res, status, data) {
  res.writeHead(status, { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" });
  res.end(JSON.stringify(data));
}

/**
 * テスト CSV を読む（docs/DESIGN.md「文字コードと行の識別」「記録と CSV 保存」の要点だけを模擬）。
 * UTF-8 BOM は読み飛ばし、UTF-16・NUL バイトはエラー。再生するのは先頭の mod ブロックだけで、ブロック数を blocks に返す。
 * 同じ名前の入力列が複数あればエラー（GUI の記録は入力 1 名 = 1 列）。
 */
function parseTestCsv(buf) {
  if ((buf[0] === 0xff && buf[1] === 0xfe) || (buf[0] === 0xfe && buf[1] === 0xff)) {
    throw new Error("UTF-16 の CSV には対応していません");
  }
  let text = buf.toString("utf8");
  if (text.startsWith("﻿")) text = text.slice(1);
  const lines = text.split(/\r\n|\r|\n/);
  const nul = lines.findIndex((l) => l.includes("\0"));
  if (nul >= 0) throw new Error(`${nul + 1} 行目: NUL バイトを含む行があります（UTF-16 の CSV には対応していません）`);
  const first = (l) => l.split(",")[0].trim().replace(/^"(.*)"$/, "$1").trim().toLowerCase();
  const modAt = lines.map((l, i) => (["mod", "modi"].includes(first(l)) ? i : -1)).filter((i) => i >= 0);
  if (!modAt.length) throw new Error("mod 行がありません");
  const end = modAt[1] ?? lines.length;
  const block = lines.slice(modAt[0], end);
  const head = block[0].split(",");
  const nameAt = block.findIndex((l) => first(l) === "#comment");
  if (nameAt < 0) throw new Error("#COMMENT 行がありません");
  const names = block[nameAt].split(",").slice(1).map((n) => n.trim()).filter((n) => n !== "");
  const nIn = Number.isFinite(Number(head[3])) && head[3]?.trim() !== "" ? Number(head[3]) : names.length;
  for (let i = 0; i < nIn && i < names.length; i++) {
    const k = names.indexOf(names[i]);
    if (k < i) {
      throw new Error(`入力 ${k + 2} 列目と ${i + 2} 列目が同じ名前 '${names[i]}' です。GUI では同じ名前の入力列が複数ある CSV は`
        + "読み込めません（sim run では実行できます。左から順に書きます）");
    }
  }
  const rows = block.slice(nameAt + 1).filter((l, i) => l.trim() && !first(l).startsWith(";")
      && !first(l).startsWith("#") && !first(block[nameAt + i]).startsWith("#init"))
    .map((l) => l.split(",").slice(1));
  return { func: head[1]?.trim() ?? "", names, nIn, rows, blocks: modAt.length };
}

function exportCsv() {
  const ins = [...inputNames].filter((n) => leafIndex.has(n));
  const outs = watch.filter((n) => leafIndex.has(n) && !inputNames.has(n));
  const cols = [...ins, ...outs].map((n) => leafIndex.get(n)); // 引数は模擬の記録に残さない
  const lines = [`mod,${func},sim 記録,${ins.length},${outs.length}`, `#COMMENT,${[...ins, ...outs].join(",")}`];
  for (const [, row] of record) lines.push("," + cols.map((c) => String(row[c])).join(","));
  return lines.join("\r\n") + "\r\n";
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url ?? "/", "http://127.0.0.1");
  try {
    if (req.method === "GET" && (url.pathname === "/" || url.pathname === "/index.html")) {
      if (!fs.existsSync(HTML)) {
        res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
        return res.end(`${HTML} がありません。先に npm run build してください。`);
      }
      res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
      return fs.createReadStream(HTML).pipe(res);
    }
    if (req.method === "GET" && url.pathname === "/api/info") {
      return json(res, 200, { version: "mock-0.1", periodMs, func, functions: FUNCTIONS, state: stateMsg() });
    }
    if (req.method === "GET" && url.pathname === "/api/symbols") {
      const q = (url.searchParams.get("q") ?? "").toLowerCase();
      const limit = Number(url.searchParams.get("limit") ?? 200);
      const hits = [...FUNC_ARGS[func].map(argSym), ...symbols].filter((s) => s.name.toLowerCase().includes(q)).slice(0, limit)
        .map(({ init, ...s }) => ({ ...s, members: s.members ?? [] }));
      return json(res, 200, hits);
    }
    if (req.method === "POST" && url.pathname === "/api/control") {
      const body = JSON.parse((await readBody(req)).toString("utf8") || "{}");
      if (body.speed !== undefined) {
        const s = Number(body.speed);
        if (!(s === 0 || (s >= 0.1 && s <= 100))) return json(res, 400, { error: "speed は 0.1〜100 か 0（最速）" });
        speed = s; rebase(); overrun = false;
      }
      if (body.periodMs !== undefined) {
        if (!(Number(body.periodMs) > 0)) return json(res, 400, { error: "periodMs が不正です" });
        periodMs = Number(body.periodMs); rebase(); broadcast("hello", helloMsg());
      }
      if (body.func !== undefined && !FUNCTIONS.includes(body.func)) return json(res, 400, { error: `関数 ${body.func} がありません` });
      if (![undefined, "run", "pause", "step", "reset"].includes(body.cmd)) return json(res, 400, { error: `cmd ${body.cmd} は使えません` });
      dropped = [];
      if (body.func !== undefined && body.func !== func) changeFunc(body.func);
      switch (body.cmd) {
        case "run": running = true; stepBudget = 0; rebase(); break;
        case "pause": running = false; stepBudget = 0; flushSamples(); break;
        case "step": running = true; stepBudget = Math.max(1, Math.floor(Number(body.steps ?? 1))); break;
        case "reset": running = false; csvPlay = null; resetTarget(); break;
        default: break;
      }
      broadcastState(true);
      return json(res, 200, stateMsg()); // 応答は state と同じ形
    }
    if (req.method === "POST" && url.pathname === "/api/watch") {
      const body = JSON.parse((await readBody(req)).toString("utf8") || "{}");
      const names = Array.isArray(body.names) ? body.names.map(String) : [];
      const errors = names.filter((n) => !resolvable(n))
        .map((name) => ({ name, message: byName.has(name) ? "構造体・配列は要素を指定してください" : unresolvedMessage(name) }));
      setWatch(names.filter(resolvable));
      return json(res, 200, { errors });
    }
    if (url.pathname === "/api/variants" && req.method === "GET") return json(res, 200, { path: "(メモリ)/sim_gui_variants.json", exists: variantsDoc !== null, doc: variantsDoc, error: "" });
    if (url.pathname === "/api/variants" && req.method === "POST") {
      const body = JSON.parse(await readBody(req));
      if (!body || typeof body !== "object" || Array.isArray(body) || !Array.isArray(body.variants)) return json(res, 400, { error: "variants 配列が必要です" });
      variantsDoc = body;
      return json(res, 200, { ok: true, path: "(メモリ)/sim_gui_variants.json" });
    }
    if (url.pathname === "/api/init" && req.method === "POST") {
      const body = JSON.parse(await readBody(req));
      if (!Array.isArray(body?.values)) return json(res, 400, { error: "values 配列が必要です" });
      const errors = body.values.flatMap(row => {
        const name = row?.name ?? "";
        if (!resolvable(name) || !symOf(name)?.writable) return [{ name, message: unresolvedMessage(name) }];
        if (typeof row.value !== "string" || parseCell(row.value) === null || !["once", "every"].includes(row.mode)) return [{ name, message: "値または書き方が不正です" }];
        return [];
      });
      if (errors.length) return json(res, 400, { errors });
      inits = body.values;
      broadcastState();
      return json(res, 200, { ok: true });
    }
    if (req.method === "POST" && url.pathname === "/api/write") {
      const body = JSON.parse((await readBody(req)).toString("utf8") || "{}");
      if (typeof body.value === "string" && /[\r\n]/.test(body.value)) return json(res, 400, { error: "値に改行は使えません" });
      const sym = symOf(body.name);
      if (!sym || sym.members) return json(res, 404, { error: unresolvedMessage(String(body.name)) });
      // 固定を外すと、その名前の反映待ちも捨てる
      if (body.release) { holds.delete(body.name); pending.delete(body.name); broadcastState(); return json(res, 200, { ok: true }); }
      if (!sym.writable) return json(res, 400, { error: `${body.name} は書き込めません` });
      if (typeof body.value !== "number" && typeof body.value !== "string") return json(res, 400, { error: "value がありません" });
      // 文字列の値はテスト CSV のセルと同じ書式として書いた表記のまま持つ（記録・pending にもその表記で出す）。読めなければ 400
      if (typeof body.value === "string" && parseCell(body.value) === null) {
        return json(res, 400, { error: `${body.name}: 値 ${JSON.stringify(body.value)} を読めません（テスト CSV のセルと同じ書式で書いてください）` });
      }
      inputNames.add(body.name);
      // 停止中もメモリへはすぐに書かない（次のステップの開始時に書く）。values も変わらない
      if (body.hold) { holds.set(body.name, body.value); pending.delete(body.name); }
      else pending.set(body.name, String(body.value)); // 固定した名前への単発は、その周期も固定の値になる
      broadcastState();
      return json(res, 200, { ok: true });
    }
    if (req.method === "POST" && url.pathname === "/api/load-csv") {
      // 読めない CSV（同じ名前の入力列・UTF-16 など）や使えない関数は、何も変えずに 400
      const parsed = parseTestCsv(await readBody(req));
      const csvFunc = parsed.func;
      if (csvFunc && !FUNCTIONS.includes(csvFunc)) return json(res, 400, { error: `関数 ${csvFunc} がありません` });
      dropped = [];
      if (csvFunc && csvFunc !== func) changeFunc(csvFunc);
      parsed.names.slice(0, parsed.nIn).forEach((n) => inputNames.add(n));
      csvPlay = { ...parsed, index: 0 };
      const names = [...new Set(parsed.names)];
      setWatch([...new Set([...watch, ...names.filter(resolvable)])]);
      running = true; rebase(); broadcastState();
      const out = { rows: parsed.rows.length, names };
      if (parsed.blocks > 1) {
        // 2 つ目以降の mod ブロックは再生しない。黙って一部だけ再生しないように応答で知らせる
        Object.assign(out, {
          blocks: parsed.blocks,
          warning: `CSV に mod ブロックが ${parsed.blocks} 個あります。GUI では先頭のブロック（${csvFunc || func}）だけを再生し、`
            + "2 つ目以降は再生しません（sim run ではすべて実行します）",
        });
      }
      return json(res, 200, out);
    }
    if (req.method === "GET" && url.pathname === "/api/export.csv") {
      res.writeHead(200, { "Content-Type": "text/csv; charset=utf-8", "Content-Disposition": 'attachment; filename="sim_record.csv"' });
      return res.end(exportCsv());
    }
    if (req.method === "GET" && url.pathname === "/api/stream") {
      res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-store", Connection: "keep-alive" });
      res.write(": mock sim\n\n");
      clients.add(res);
      send(res, "hello", helloMsg());
      send(res, "state", stateMsg());
      send(res, "values", valuesMsg());
      req.on("close", () => clients.delete(res));
      return;
    }
    json(res, 404, { error: "見つかりません" });
  } catch (e) {
    json(res, 400, { error: String(e?.message ?? e) });
  }
});

server.listen(PORT, "127.0.0.1", () => {
  console.log(`mock sim gui: http://127.0.0.1:${PORT}/  (period ${periodMs} ms, html ${HTML})`);
});
