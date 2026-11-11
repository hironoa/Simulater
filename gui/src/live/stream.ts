// `/api/stream`（Server-Sent Events）の取り込み。

import type { SimState } from "./api";

export interface HelloMessage { periodMs?: number; func?: string; names: string[] }
export interface SamplesMessage { step0: number; names: string[]; rows: (number | null)[][] }
export interface ValuesMessage { step: number; names: string[]; values: (number | null)[] }
export type StateMessage = Partial<SimState> & { periodMs?: number; func?: string };

export interface StreamHandlers {
  hello: (msg: HelloMessage) => void;
  samples: (msg: SamplesMessage) => void;
  state: (msg: StateMessage) => void;
  /** 監視変数の現在値（ステップは進まない。記録や波形には足さない） */
  values?: (msg: ValuesMessage) => void;
  error?: (message: string) => void;
}

const isObject = (v: unknown): v is Record<string, unknown> => !!v && typeof v === "object" && !Array.isArray(v);

/** SSE の 1 イベントを検証して該当するハンドラに渡す。壊れたメッセージは捨てて false を返す。 */
export function dispatchStreamEvent(event: string, data: string, handlers: StreamHandlers): boolean {
  let msg: unknown;
  try {
    msg = JSON.parse(data);
  } catch {
    handlers.error?.(`${event}: JSON を読めません`);
    return false;
  }
  if (!isObject(msg)) return false;
  switch (event) {
    case "hello": {
      const names = Array.isArray(msg.names) ? msg.names.map(String) : [];
      handlers.hello({
        names,
        periodMs: typeof msg.periodMs === "number" ? msg.periodMs : undefined,
        func: typeof msg.func === "string" ? msg.func : undefined,
      });
      return true;
    }
    case "samples": {
      if (typeof msg.step0 !== "number" || !Array.isArray(msg.names) || !Array.isArray(msg.rows)) {
        handlers.error?.("samples: 形式が違います");
        return false;
      }
      const rows = (msg.rows as unknown[]).filter(Array.isArray).map((row) =>
        (row as unknown[]).map((v) => (typeof v === "number" && Number.isFinite(v) ? v : null)));
      handlers.samples({ step0: msg.step0, names: msg.names.map(String), rows });
      return true;
    }
    case "values": {
      if (typeof msg.step !== "number" || !Array.isArray(msg.names) || !Array.isArray(msg.values)
        || msg.names.length !== msg.values.length) {
        handlers.error?.("values: 形式が違います");
        return false;
      }
      const values = (msg.values as unknown[]).map((v) => (typeof v === "number" && Number.isFinite(v) ? v : null));
      handlers.values?.({ step: msg.step, names: msg.names.map(String), values });
      return true;
    }
    case "state": {
      const out: StateMessage = {};
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
      handlers.state(out);
      return true;
    }
    default:
      return false;
  }
}

export interface EventSourceLike {
  addEventListener(type: string, listener: (ev: MessageEvent) => void): void;
  close(): void;
  onopen: ((ev: Event) => void) | null;
  onerror: ((ev: Event) => void) | null;
}

/** EventSource を張る。切れたら EventSource 自身が再接続する。 */
export function openStream(
  url: string,
  handlers: StreamHandlers & { open?: () => void; lost?: () => void },
  create: (url: string) => EventSourceLike = (u) => new EventSource(u) as unknown as EventSourceLike,
) {
  const es = create(url);
  for (const type of ["hello", "samples", "state", "values"]) {
    es.addEventListener(type, (ev) => dispatchStreamEvent(type, String(ev.data), handlers));
  }
  es.onopen = () => handlers.open?.();
  es.onerror = () => handlers.lost?.();
  return () => es.close();
}
