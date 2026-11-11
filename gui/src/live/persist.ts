// 入力パネルと監視一覧の構成を localStorage に保存する。プライベートウィンドウなどで使えなくても動くようにする。

import { DEFAULT_MAX_SAMPLES } from "./liveRecord";

export type InputWidget = "number" | "slider" | "toggle" | "button";

export interface InputItem {
  name: string;
  widget: InputWidget;
  min: number;
  max: number;
  step: number;
  hold: boolean;
  /** 数値入力・スライダの最後の値（復元用） */
  value?: number;
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

export function sanitizeLayout(raw: unknown): LiveLayout {
  const doc = raw && typeof raw === "object" ? (raw as Record<string, unknown>) : {};
  const widgets: InputWidget[] = ["number", "slider", "toggle", "button"];
  const inputs = Array.isArray(doc.inputs) ? doc.inputs : [];
  const watch = Array.isArray(doc.watch) ? doc.watch : [];
  const seenIn = new Set<string>();
  const seenW = new Set<string>();
  return {
    inputs: inputs.flatMap((it: any) => {
      if (!it || typeof it.name !== "string" || !it.name || seenIn.has(it.name)) return [];
      seenIn.add(it.name);
      const item: InputItem = {
        name: it.name,
        widget: widgets.includes(it.widget) ? it.widget : "number",
        min: num(it.min, 0),
        max: num(it.max, 100),
        step: num(it.step, 1) > 0 ? num(it.step, 1) : 1,
        hold: !!it.hold,
      };
      if (typeof it.value === "number" && Number.isFinite(it.value)) item.value = it.value;
      return [item];
    }),
    watch: watch.flatMap((it: any) => {
      if (!it || typeof it.name !== "string" || !it.name || seenW.has(it.name)) return [];
      seenW.add(it.name);
      return [{ name: it.name, plot: it.plot !== false, radix: it.radix === "hex" ? "hex" : "dec" } as WatchItem];
    }),
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
