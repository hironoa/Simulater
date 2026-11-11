#!/usr/bin/env node
// `sim gui` の HTTP API と SSE を模擬する開発用サーバ（Node の標準モジュールだけで動く）。
// docs/DESIGN.md「5. GUI モード」の仕様に合わせている。
//
//   node dev/mock-server.mjs [--port 8765] [--period-ms 5] [--html dist/sim_gui.html]
//
// 正弦波・カウンタ・書き込みに反応する値（ゲイン×正弦、ボタンの押下回数、目標値への一次遅れなど）を流す。

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
const FUNCTIONS = ["sim_driver_main", "func1", "ctrl_task_5ms"];
let func = FUNCTIONS[0];

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
const get = (n) => values[leafIndex.get(n)];
const put = (n, v) => { values[leafIndex.get(n)] = v; };

// ---- 実行状態 ----
let running = false;
let step = 0; // 次に実行するステップ番号（記録上は 0 始まり）
let speed = 1;
let overrun = false;
let watch = ["g_counter", "g_sine", "g_output"];
const pending = new Map(); // 次の周期の前に 1 回書く
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

function writeVar(name, value) {
  const sym = byName.get(name);
  const v = typeof value === "string" ? Number(value.startsWith("0x") ? parseInt(value, 16) : value) : value;
  put(name, clampType(sym, Number.isFinite(v) ? v : 0));
}

function runOneStep() {
  if (csvPlay) {
    const row = csvPlay.rows[csvPlay.index];
    if (row) {
      for (let i = 0; i < csvPlay.nIn; i++) {
        const cell = row[i];
        const name = csvPlay.names[i];
        if (cell !== undefined && cell !== "" && byName.get(name)?.writable) writeVar(name, cell);
      }
      csvPlay.index++;
    } else {
      csvPlay = null;
      running = false;
      broadcastState();
      return false;
    }
  }
  for (const [n, v] of pending) writeVar(n, v);
  pending.clear();
  for (const [n, v] of holds) writeVar(n, v);

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
  pendingRows.push(watch.map((n) => (leafIndex.has(n) ? get(n) : null)));
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
const stateMsg = () => ({ running, step, speed, holds: [...holds].map(([name, value]) => ({ name, value })), overrun });
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
const valuesMsg = () => ({ step, names: watch, values: watch.map((n) => (leafIndex.has(n) ? get(n) : null)) });
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

function parseTestCsv(text) {
  const lines = text.split(/\r?\n/);
  const head = (lines[0] ?? "").split(",");
  const nIn = Number(head[3] ?? 0);
  const nameRow = lines.find((l) => l.startsWith("#COMMENT"));
  if (head[0] !== "mod" || !nameRow) throw new Error("mod 行または #COMMENT 行がありません");
  const names = nameRow.split(",").slice(1).filter((n) => n.trim() !== "").map((n) => n.trim());
  const start = lines.indexOf(nameRow) + 1;
  const rows = lines.slice(start).filter((l) => l.trim() && !l.startsWith(";") && !l.startsWith("mod"))
    .map((l) => l.split(",").slice(1));
  return { names, nIn: Number.isFinite(nIn) ? nIn : names.length, rows };
}

function exportCsv() {
  const ins = [...inputNames].filter((n) => leafIndex.has(n));
  const outs = watch.filter((n) => leafIndex.has(n) && !inputNames.has(n));
  const cols = [...ins, ...outs].map((n) => leafIndex.get(n));
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
      const hits = symbols.filter((s) => s.name.toLowerCase().includes(q)).slice(0, limit)
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
      if (body.func !== undefined) {
        if (!FUNCTIONS.includes(body.func)) return json(res, 400, { error: `関数 ${body.func} がありません` });
        func = body.func; broadcast("hello", helloMsg());
      }
      switch (body.cmd) {
        case "run": running = true; stepBudget = 0; rebase(); break;
        case "pause": running = false; stepBudget = 0; flushSamples(); break;
        case "step": running = true; stepBudget = Math.max(1, Math.floor(Number(body.steps ?? 1))); break;
        case "reset":
          flushSamples(); resetValues(); step = 0; record = []; pending.clear(); csvPlay = null; prevButton = 0; rebase(); markValues();
          break;
        case undefined: break;
        default: return json(res, 400, { error: `cmd ${body.cmd} は使えません` });
      }
      broadcastState();
      return json(res, 200, { ok: true, state: stateMsg() });
    }
    if (req.method === "POST" && url.pathname === "/api/watch") {
      const body = JSON.parse((await readBody(req)).toString("utf8") || "{}");
      const names = Array.isArray(body.names) ? body.names.map(String) : [];
      const errors = names.filter((n) => !leafIndex.has(n))
        .map((name) => ({ name, message: byName.has(name) ? "構造体・配列は要素を指定してください" : "変数が見つかりません" }));
      setWatch(names.filter((n) => leafIndex.has(n)));
      return json(res, 200, { errors });
    }
    if (req.method === "POST" && url.pathname === "/api/write") {
      const body = JSON.parse((await readBody(req)).toString("utf8") || "{}");
      const sym = byName.get(body.name);
      if (!sym || sym.members) return json(res, 404, { error: `変数 ${body.name} が見つかりません` });
      if (body.release) { holds.delete(body.name); broadcastState(); return json(res, 200, { ok: true }); }
      if (!sym.writable) return json(res, 400, { error: `${body.name} は書き込めません` });
      inputNames.add(body.name);
      if (body.hold) holds.set(body.name, body.value);
      else { holds.delete(body.name); pending.set(body.name, body.value); }
      if (!running) { writeVar(body.name, body.value); markValues(); } // 停止中も値が見えるように
      broadcastState();
      return json(res, 200, { ok: true });
    }
    if (req.method === "POST" && url.pathname === "/api/load-csv") {
      const text = (await readBody(req)).toString("utf8");
      const parsed = parseTestCsv(text);
      parsed.names.slice(0, parsed.nIn).forEach((n) => inputNames.add(n));
      csvPlay = { ...parsed, index: 0 };
      setWatch([...new Set([...watch, ...parsed.names.filter((n) => leafIndex.has(n))])]);
      running = true; rebase(); broadcastState();
      return json(res, 200, { rows: parsed.rows.length, names: parsed.names });
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
