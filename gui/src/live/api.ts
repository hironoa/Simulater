// `sim gui` の HTTP API クライアント（docs/DESIGN.md「5. GUI モード」）。

export interface SimState {
  running: boolean;
  step: number;
  speed: number;
  holds: { name: string; value: number | string }[];
  overrun: boolean;
}

export interface SimInfo {
  version: string;
  periodMs: number;
  func: string;
  functions: string[];
  state: Partial<SimState>;
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
}

export class ApiError extends Error {
  constructor(message: string, readonly status: number) {
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
    if (!res.ok) throw new ApiError(await readError(res), res.status);
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
    info: (signal?: AbortSignal) => json<SimInfo>("/api/info", { signal }),
    symbols: (q: string, limit = 200, signal?: AbortSignal) =>
      json<SymbolEntry[]>(`/api/symbols?q=${encodeURIComponent(q)}&limit=${limit}`, { signal }),
    control: (req: ControlRequest) => post<unknown>("/api/control", req),
    watch: (names: string[]) => post<WatchResult>("/api/watch", { names }),
    write: (name: string, value: number | string, hold = false) =>
      post<unknown>("/api/write", { name, value, hold }),
    release: (name: string) => post<unknown>("/api/write", { name, release: true }),
    /** CSV はバイト列のまま送る（Shift_JIS のテスト CSV をそのまま再生するため）。 */
    loadCsv: (body: Blob | ArrayBuffer | string) =>
      json<LoadCsvResult>("/api/load-csv", {
        method: "POST",
        headers: { "Content-Type": "text/csv" },
        body,
      }),
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
      state: info.state && typeof info.state === "object" ? info.state : {},
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
