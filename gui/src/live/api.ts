import type { InitValue, VariantsDoc, VariantsResult } from "./variants";
// `sim gui` の HTTP API クライアント（docs/DESIGN.md「5. GUI モード」）。

export interface SignalsResult {
  path: string;
  exists: boolean;
  text: string | null;
  error: string;
}

export interface SimState {
  running: boolean;
  /** 失敗後、ターゲットを初期状態に戻すまで実行・書き込みを拒否する */
  needReset: boolean;
  /** 実行後に once の一覧が変わり、リセットでの反映待ち */
  initStale: boolean;
  step: number;
  speed: number;
  inits?: InitValue[];
  holds: { name: string; value: number | string }[];
  overrun: boolean;
  /** 次の周期の開始時に書く単発書き込み（反映待ち）。名前ごとに 1 個 */
  pending: PendingWrite[];
  /** ステップ時の書き込み・関数呼び出しなどの誤り（無ければ空文字列） */
  error: string;
}

/** 反映待ちの単発書き込み。`value` は書いたセルの文字列（`"7"`、`"'A'"`、`"0x78"` など）。 */
export interface PendingWrite {
  name: string;
  value: string;
}

/** 関数の変更などで解決し直せずにサーバが外した固定・監視（直近の `/api/control`・`/api/load-csv` の分）。 */
export interface DroppedItem {
  name: string;
  kind: "hold" | "watch" | "init";
  message: string;
}

/** `state`（SSE の `event: state`、`/api/control` の応答、`/api/info` の `state`）を検証したもの。 */
export type StateMessage = Partial<SimState> & { periodMs?: number; func?: string; dropped?: DroppedItem[] };

const isObject = (v: unknown): v is Record<string, unknown> => !!v && typeof v === "object" && !Array.isArray(v);

/**
 * `dropped` を読む。項目が無ければ空、配列でなければ undefined（不正な形は無視する）。
 * 名前のない項目や kind が hold / watch でない項目は捨てる。
 */
export function parseDropped(v: unknown): DroppedItem[] | undefined {
  if (v === undefined || v === null) return [];
  if (!Array.isArray(v)) return undefined;
  return v.flatMap((d) => {
    if (!isObject(d) || typeof d.name !== "string" || !d.name) return [];
    if (d.kind !== "hold" && d.kind !== "watch" && d.kind !== "init") return [];
    return [{ name: d.name, kind: d.kind, message: typeof d.message === "string" ? d.message : "" }];
  });
}

/**
 * `pending` を読む。項目が無ければ空、配列でなければ undefined（不正な形は無視する）。
 * 名前のない項目は捨て、同じ名前は後のものを採る。
 */
export function parsePending(v: unknown): PendingWrite[] | undefined {
  if (v === undefined || v === null) return [];
  if (!Array.isArray(v)) return undefined;
  const byName = new Map<string, string>();
  for (const p of v) {
    if (!isObject(p) || typeof p.name !== "string" || !p.name) continue;
    if (typeof p.value !== "string" && typeof p.value !== "number") continue;
    byName.delete(p.name);
    byName.set(p.name, String(p.value));
  }
  return [...byName].map(([name, value]) => ({ name, value }));
}

/** state の形をした値を検証する。オブジェクトでなければ null。 */
export function parseState(msg: unknown): StateMessage | null {
  if (!isObject(msg)) return null;
  const out: StateMessage = { needReset: msg.needReset === true };
  if (typeof msg.initStale === "boolean") out.initStale = msg.initStale;
  if (typeof msg.running === "boolean") out.running = msg.running;
  if (typeof msg.step === "number") out.step = msg.step;
  if (typeof msg.speed === "number") out.speed = msg.speed;
  if (typeof msg.overrun === "boolean") out.overrun = msg.overrun;
  if (typeof msg.periodMs === "number") out.periodMs = msg.periodMs;
  if (typeof msg.func === "string") out.func = msg.func;
  if (Array.isArray(msg.holds)) {
    out.holds = msg.holds.filter(isObject).map((h) => ({
      name: String(h.name),
      value: typeof h.value === "number" || typeof h.value === "string" ? h.value : String(h.value),
    }));
  }
  if (Array.isArray(msg.inits)) out.inits = msg.inits.filter((v): v is InitValue => isObject(v) && typeof v.name === "string" && typeof v.value === "string" && (v.mode === "once" || v.mode === "every"));
  if (typeof msg.error === "string") out.error = msg.error;
  const dropped = parseDropped(msg.dropped);
  if (dropped) out.dropped = dropped;
  const pending = parsePending(msg.pending);
  if (pending) out.pending = pending;
  return out;
}

/** `/api/control` の応答から state を取り出す（state そのものでも `{"state":{...}}` でも読む）。 */
export function parseControlResponse(res: unknown): StateMessage | null {
  if (!isObject(res)) return null;
  if (isObject(res.state)) return parseState(res.state);
  // state の項目が 1 つもなければ state ではない（`{"ok":true}` など）
  const keys = ["running", "step", "speed", "holds", "overrun", "periodMs", "func", "dropped", "pending", "error", "needReset", "initStale", "inits"];
  return keys.some((k) => k in res) ? parseState(res) : null;
}

export interface SimInfo {
  version: string;
  periodMs: number;
  func: string;
  functions: string[];
  state: StateMessage;
}

export type SymbolKind = "int" | "uint" | "float" | "ptr" | "struct" | "array";

export interface SymbolEntry {
  name: string;
  type: string;
  size: number;
  kind: SymbolKind;
  writable: boolean;
  members?: string[];
}

export interface ControlRequest {
  cmd?: "run" | "pause" | "step" | "reset";
  steps?: number;
  speed?: number;
  periodMs?: number;
  func?: string;
}

export interface WatchResult {
  errors?: { name: string; message: string }[];
}

export interface LoadCsvResult {
  rows: number;
  names: string[];
  /** CSV の列名と、同じ変数を指す既存の監視名の組 */
  same?: { name: string; existing: string }[];
  /** CSV の `mod` ブロックの数（2 つ以上のときだけサーバが付ける。再生するのは先頭のブロックだけ） */
  blocks?: number;
  /** 読み込みは成功したが利用者に知らせること（複数ブロックのうち先頭だけを再生する、など） */
  warning?: string;
}

/**
 * `/api/load-csv` の応答を読む。`rows` は数でなければ 0、`names` は文字列だけを残す。
 * `blocks`（1 以上の整数）と `warning`（空でない文字列）は、無ければ・不正な形なら付けない。
 */
export function parseLoadCsvResult(res: unknown): LoadCsvResult {
  const r = isObject(res) ? res : {};
  const out: LoadCsvResult = {
    rows: typeof r.rows === "number" && Number.isFinite(r.rows) ? r.rows : 0,
    names: Array.isArray(r.names) ? r.names.filter((n): n is string => typeof n === "string") : [],
  };
  if (typeof r.blocks === "number" && Number.isInteger(r.blocks) && r.blocks >= 1) out.blocks = r.blocks;
  if (typeof r.warning === "string" && r.warning.trim()) out.warning = r.warning;
  if (Array.isArray(r.same)) {
    const same = r.same.flatMap(pair =>
      isObject(pair) && typeof pair.name === "string" && pair.name.trim()
        && typeof pair.existing === "string" && pair.existing.trim() && pair.name !== pair.existing
        ? [{ name: pair.name, existing: pair.existing }] : [],
    );
    if (same.length) out.same = same;
  }
  return out;
}

export class ApiError extends Error {
  constructor(message: string, readonly status: number, readonly errors: { name: string; message: string }[] = []) {
    super(message);
    this.name = "ApiError";
  }
}

type FetchLike = typeof fetch;

async function readError(res: Response): Promise<string> {
  try {
    const text = await res.text();
    try {
      const json = JSON.parse(text);
      if (json && typeof json.error === "string") return json.error;
      if (json && typeof json.message === "string") return json.message;
    } catch { /* JSON でない本文はそのまま使う */ }
    return text.trim() || `HTTP ${res.status}`;
  } catch {
    return `HTTP ${res.status}`;
  }
}

export function createApi(base = "", fetchImpl: FetchLike = (input, init) => fetch(input, init)) {
  const url = (path: string) => base + path;

  async function json<T>(path: string, init?: RequestInit): Promise<T> {
    const res = await fetchImpl(url(path), init);
    if (!res.ok) {
      const detail = await res.clone().json().catch(() => ({}));
      throw new ApiError(await readError(res), res.status, Array.isArray(detail.errors) ? detail.errors : []);
    }
    const text = await res.text();
    return (text ? JSON.parse(text) : {}) as T;
  }

  const post = <T>(path: string, body: unknown) =>
    json<T>(path, {
      method: "POST",
      headers: { "Content-Type": "application/json; charset=utf-8" },
      body: JSON.stringify(body),
    });

  return {
    getVariants: () => json<VariantsResult>("/api/variants"),
    saveVariants: (doc: VariantsDoc) => post<{ ok: boolean; path: string }>("/api/variants", doc),
    getSignals: () => json<SignalsResult>("/api/signals"),
    saveSignals: (text: string) => json<{ ok: boolean; path: string }>("/api/signals", {
      method: "POST",
      headers: { "Content-Type": "text/yaml; charset=utf-8" },
      body: text,
    }),
    setInit: (values: InitValue[]) => post<{ ok: boolean }>("/api/init", { values }),
    info: (signal?: AbortSignal) => json<SimInfo>("/api/info", { signal }),
    symbols: (q: string, limit = 200, signal?: AbortSignal) =>
      json<SymbolEntry[]>(`/api/symbols?q=${encodeURIComponent(q)}&limit=${limit}`, { signal }),
    /** 応答の state（読めなければ null） */
    control: async (req: ControlRequest) => parseControlResponse(await post<unknown>("/api/control", req)),
    watch: (names: string[]) => post<WatchResult>("/api/watch", { names }),
    write: (name: string, value: number | string, hold = false) =>
      post<unknown>("/api/write", { name, value, hold }),
    release: (name: string) => post<unknown>("/api/write", { name, release: true }),
    /** CSV はバイト列のまま送る（Shift_JIS のテスト CSV をそのまま再生するため）。 */
    loadCsv: async (body: Blob | ArrayBuffer | string) =>
      parseLoadCsvResult(await json<unknown>("/api/load-csv", {
        method: "POST",
        headers: { "Content-Type": "text/csv" },
        body,
      })),
    exportUrl: () => url("/api/export.csv"),
    streamUrl: () => url("/api/stream"),
  };
}

export type SimApi = ReturnType<typeof createApi>;

/** 同一オリジンに `sim gui` がいるかを確かめる。file:// で開いたときや応答がなければ null。 */
export async function detectServer(
  api: SimApi,
  protocol: string = typeof location !== "undefined" ? location.protocol : "file:",
  timeoutMs = 1500,
): Promise<SimInfo | null> {
  if (protocol !== "http:" && protocol !== "https:") return null;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const info = await api.info(controller.signal);
    if (!info || typeof info !== "object" || typeof info.periodMs !== "number") return null;
    return {
      version: String(info.version ?? ""),
      periodMs: info.periodMs,
      func: String(info.func ?? ""),
      functions: Array.isArray(info.functions) ? info.functions.map(String) : [],
      state: parseState(info.state) ?? {},
    };
  } catch {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

/** 構造体・配列の `members` を完全な変数名にする。サーバが子の完全名を返しても、メンバ名だけを返しても扱えるようにする。 */
export function memberPath(parent: string, member: string): string {
  if (member.startsWith(parent) && /^[.[\-]/.test(member.slice(parent.length))) return member;
  if (member.startsWith("[")) return parent + member;
  if (/^\d+$/.test(member)) return `${parent}[${member}]`;
  if (member.startsWith(".") || member.startsWith("->")) return parent + member;
  return `${parent}.${member}`;
}
