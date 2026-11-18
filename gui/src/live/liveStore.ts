import { create } from "zustand";
import type { SimInfo, SimState } from "./api";
import { DEFAULT_LAYOUT, type InputItem, type WatchItem } from "./persist";

export type LiveMode = "detecting" | "live" | "offline";

export interface LiveSimState extends SimState {
  periodMs: number;
  func: string;
}

export interface LiveStoreState {
  mode: LiveMode;
  info: SimInfo | null;
  /** SSE がつながっているか */
  connected: boolean;
  sim: LiveSimState;
  /** サーバが今監視している名前（hello で届く） */
  serverWatch: string[];
  watchErrors: Record<string, string>;
  inputs: InputItem[];
  watch: WatchItem[];
  windowSize: number;
  maxSamples: number;
  /** 監視変数の最新値（数百 ms ごとにまとめて更新する） */
  values: Record<string, number | null>;
  lastError: string | null;
}

export const initialLiveState: LiveStoreState = {
  mode: "detecting",
  info: null,
  connected: false,
  sim: { initStale: false, needReset: false, running: false, step: 0, speed: 1, holds: [], overrun: false, pending: [], error: "", periodMs: 5, func: "" },
  serverWatch: [],
  watchErrors: {},
  inputs: DEFAULT_LAYOUT.inputs,
  watch: DEFAULT_LAYOUT.watch,
  windowSize: DEFAULT_LAYOUT.windowSize,
  maxSamples: DEFAULT_LAYOUT.maxSamples,
  values: {},
  lastError: null,
};

export const useLiveStore = create<LiveStoreState>(() => ({ ...initialLiveState }));

export const isLive = () => useLiveStore.getState().mode === "live";
