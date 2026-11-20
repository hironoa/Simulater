// 入力パネルと監視一覧の構成を localStorage に保存する。プライベートウィンドウなどで使えなくても動くようにする。

import { DEFAULT_MAX_SAMPLES } from "./liveRecord";

export type InputWidget = "number" | "slider" | "toggle" | "button" | "state";

export interface InputItem {
  name: string;
  widget: InputWidget;
  min: number;
  max: number;
  step: number;
  hold: boolean;
  /** 新規入力で、まだサーバに固定値を書いていない。 */
  holdPending?: boolean;
  /**
   * 最後に書いた生の値（復元用。換算設定が変わっても意味を保つ）。数値入力は書いた文字列のまま（`"0x20000000000001"` など。Number に通して丸めない）
   */
  value?: number | string;
  /**
   * false なら現在値を監視しない（関数の変更でサーバが監視から外したとき）。
   * 書き込めたとき・入力に追加し直したときに監視へ戻す。省略は監視する。
   */
  monitor?: boolean;
}

export interface WatchItem {
  name: string;
  plot: boolean;
  radix: "dec" | "hex";
}

export interface LiveLayout {
  inputs: InputItem[];
  watch: WatchItem[];
  windowSize: number;
  maxSamples: number;
}

export const STORAGE_KEY = "sim-gui:layout:v1";

export const DEFAULT_LAYOUT: LiveLayout = { inputs: [], watch: [], windowSize: 2000, maxSamples: DEFAULT_MAX_SAMPLES };

type StorageLike = Pick<Storage, "getItem" | "setItem">;

function storage(): StorageLike | null {
  try {
    return typeof localStorage !== "undefined" ? localStorage : null;
  } catch {
    return null;
  }
}

const num = (v: unknown, fallback: number) => (typeof v === "number" && Number.isFinite(v) ? v : fallback);

export function sanitizeInputs(raw: unknown, keepPending = false): InputItem[] {
  const widgets: InputWidget[] = ["number", "slider", "toggle", "button", "state"];
  const seen = new Set<string>();
  return (Array.isArray(raw) ? raw : []).flatMap((it: any) => {
    if (!it || typeof it.name !== "string" || !it.name || seen.has(it.name)) return [];
    seen.add(it.name);
    const step = num(it.step, 1);
    const item: InputItem = {
      name: it.name,
      widget: widgets.includes(it.widget) ? it.widget : "number",
      min: num(it.min, 0),
      max: num(it.max, 100),
      step: step > 0 ? step : 1,
      hold: !!it.hold,
    };
    if (typeof it.value === "number" && Number.isFinite(it.value)) item.value = it.value;
    else if (typeof it.value === "string" && it.value.trim() && !/[\r\n]/.test(it.value)) item.value = it.value.trim();
    if (keepPending && it.holdPending === true && item.hold && item.value === undefined) item.holdPending = true;
    if (it.monitor === false) item.monitor = false;
    return [item];
  });
}

export function sanitizeWatch(raw: unknown): WatchItem[] {
  const seen = new Set<string>();
  return (Array.isArray(raw) ? raw : []).flatMap((it: any) => {
    if (!it || typeof it.name !== "string" || !it.name || seen.has(it.name)) return [];
    seen.add(it.name);
    return [{ name: it.name, plot: it.plot !== false, radix: it.radix === "hex" ? "hex" : "dec" } as WatchItem];
  });
}

export function sanitizeLayout(raw: unknown): LiveLayout {
  const doc = raw && typeof raw === "object" ? (raw as Record<string, unknown>) : {};
  return {
    inputs: sanitizeInputs(doc.inputs, true),
    watch: sanitizeWatch(doc.watch),
    windowSize: Math.min(1_000_000, Math.max(50, Math.floor(num(doc.windowSize, DEFAULT_LAYOUT.windowSize)))),
    maxSamples: Math.min(5_000_000, Math.max(1000, Math.floor(num(doc.maxSamples, DEFAULT_LAYOUT.maxSamples)))),
  };
}

export function loadLayout(store: StorageLike | null = storage()): LiveLayout {
  try {
    const text = store?.getItem(STORAGE_KEY);
    if (!text) return { ...DEFAULT_LAYOUT };
    return sanitizeLayout(JSON.parse(text));
  } catch {
    return { ...DEFAULT_LAYOUT };
  }
}

export function saveLayout(layout: LiveLayout, store: StorageLike | null = storage()): void {
  try {
    store?.setItem(STORAGE_KEY, JSON.stringify(layout));
  } catch {
    /* 保存できなくても動作は続ける */
  }
}
