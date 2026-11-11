// ライブモードの制御。サーバ検出、SSE の取り込み、監視一覧・入力の同期、構成の保存をまとめて持つ。

import toast from "react-hot-toast";
import { LIVE_FILE_NAME, useWaveformStore } from "../store/useWaveformStore";
import { createApi, detectServer, type ControlRequest, type SimApi } from "./api";
import { liveRecord, type LiveRecord } from "./liveRecord";
import { useLiveStore, type LiveStoreState } from "./liveStore";
import { loadLayout, saveLayout, type InputItem, type InputWidget, type WatchItem } from "./persist";
import { openStream, type EventSourceLike, type StreamHandlers } from "./stream";

export interface LiveControllerDeps {
  api?: SimApi;
  record?: LiveRecord;
  protocol?: string;
  createEventSource?: (url: string) => EventSourceLike;
  /** 最新値を画面へ反映する間隔 */
  valueFlushMs?: number;
  /** 停止中に届いた samples（ステップ実行など）を Viewer に載せるまでの待ち */
  commitDelayMs?: number;
  notify?: { error: (m: string) => void; success: (m: string) => void };
}

const unique = (names: string[]) => [...new Set(names.filter(Boolean))];

export function createLiveController(deps: LiveControllerDeps = {}) {
  const api = deps.api ?? createApi("");
  const record = deps.record ?? liveRecord;
  const notify = deps.notify ?? { error: (m: string) => toast.error(m), success: (m: string) => toast.success(m) };
  const valueFlushMs = deps.valueFlushMs ?? 200;
  const commitDelayMs = deps.commitDelayMs ?? 250;
  const store = useLiveStore;
  const waveform = useWaveformStore;

  let closeStream: (() => void) | null = null;
  let flushTimer: ReturnType<typeof setInterval> | null = null;
  let commitTimer: ReturnType<typeof setTimeout> | null = null;
  const pendingValues: Record<string, number | null> = {};
  let pendingDirty = false;

  const set = (patch: Partial<LiveStoreState>) => store.setState(patch);

  function persist() {
    const s = store.getState();
    saveLayout({ inputs: s.inputs, watch: s.watch, windowSize: s.windowSize, maxSamples: s.maxSamples });
  }

  function watchedNames() {
    const s = store.getState();
    return unique([...s.watch.map((w) => w.name), ...s.inputs.map((i) => i.name)]);
  }

  function syncSignals() {
    const s = store.getState();
    waveform.getState().ensureLiveSignals(watchedNames(), new Set(s.watch.filter((w) => w.plot).map((w) => w.name)));
  }

  async function syncWatch() {
    syncSignals();
    const names = watchedNames();
    try {
      const res = await api.watch(names);
      const errors: Record<string, string> = {};
      for (const e of res?.errors ?? []) errors[e.name] = e.message || "解決できません";
      set({ watchErrors: errors });
    } catch (e) {
      notify.error(`監視一覧を送れませんでした: ${(e as Error).message}`);
    }
  }

  function commit() {
    if (commitTimer) { clearTimeout(commitTimer); commitTimer = null; }
    if (!record.length) return;
    waveform.getState().commitLiveRecord({ windowSize: store.getState().windowSize, record });
  }

  function scheduleCommit() {
    if (commitTimer) clearTimeout(commitTimer);
    commitTimer = setTimeout(() => { commitTimer = null; commit(); }, commitDelayMs);
  }

  function flushValues() {
    if (!pendingDirty) return;
    pendingDirty = false;
    set({ values: { ...store.getState().values, ...pendingValues } });
  }

  const handlers: StreamHandlers = {
    hello: (msg) => {
      const sim = store.getState().sim;
      set({
        serverWatch: msg.names,
        sim: { ...sim, periodMs: msg.periodMs ?? sim.periodMs, func: msg.func ?? sim.func },
      });
      if (msg.periodMs) waveform.getState().setTaskPeriod(msg.periodMs);
      for (const n of msg.names) record.ensureColumn(n);
    },
    samples: (msg) => {
      // 既定の record は useWaveformStore.appendSamples が書く共有のライブ記録と同じもの
      const res = record === liveRecord
        ? waveform.getState().appendSamples(msg.step0, msg.names, msg.rows)
        : record.append(msg.step0, msg.names, msg.rows);
      const last = msg.rows[msg.rows.length - 1];
      if (last) {
        msg.names.forEach((n, i) => { pendingValues[n] = last[i] ?? null; });
        pendingDirty = true;
        const sim = store.getState().sim;
        const step = msg.step0 + msg.rows.length - 1;
        if (step !== sim.step) set({ sim: { ...sim, step } });
      }
      if (res.restarted) waveform.setState({ cursors: [], hoverCursor: null });
      if (!store.getState().sim.running) scheduleCommit();
    },
    state: (msg) => {
      const prev = store.getState().sim;
      const next = { ...prev, ...msg };
      set({ sim: next });
      if (msg.periodMs && msg.periodMs !== prev.periodMs) waveform.getState().setTaskPeriod(msg.periodMs);
      if (prev.running && msg.running === false) scheduleCommit();
    },
    error: (m) => console.warn("[sim stream]", m),
  };

  async function start(): Promise<boolean> {
    set({ mode: "detecting" });
    const info = await detectServer(api, deps.protocol);
    if (!info) {
      set({ mode: "offline" });
      return false;
    }
    const layout = loadLayout();
    record.setMaxSamples(layout.maxSamples);
    set({
      mode: "live",
      info,
      inputs: layout.inputs,
      watch: layout.watch,
      windowSize: layout.windowSize,
      maxSamples: layout.maxSamples,
      sim: {
        running: !!info.state.running,
        step: info.state.step ?? 0,
        speed: info.state.speed ?? 1,
        holds: info.state.holds ?? [],
        overrun: !!info.state.overrun,
        periodMs: info.periodMs,
        func: info.func,
      },
    });
    waveform.getState().setTaskPeriod(info.periodMs);
    await syncWatch();
    closeStream = openStream(api.streamUrl(), {
      ...handlers,
      open: () => set({ connected: true }),
      lost: () => set({ connected: false }),
    }, deps.createEventSource);
    flushTimer = setInterval(flushValues, valueFlushMs);
    return true;
  }

  function stop() {
    closeStream?.();
    closeStream = null;
    if (flushTimer) clearInterval(flushTimer);
    flushTimer = null;
    if (commitTimer) clearTimeout(commitTimer);
    commitTimer = null;
  }

  async function control(req: ControlRequest) {
    try {
      await api.control(req);
      const sim = store.getState().sim;
      // 応答の state イベントを待たずに表示を合わせる
      if (req.cmd === "run") set({ sim: { ...sim, running: true } });
      if (req.cmd === "pause") set({ sim: { ...sim, running: false } });
      if (req.speed !== undefined) set({ sim: { ...store.getState().sim, speed: req.speed } });
      if (req.periodMs !== undefined) {
        set({ sim: { ...store.getState().sim, periodMs: req.periodMs } });
        waveform.getState().setTaskPeriod(req.periodMs);
      }
      if (req.func !== undefined) set({ sim: { ...store.getState().sim, func: req.func } });
      if (req.cmd === "reset") {
        record.clear();
        // Viewer に載せていたライブ記録も消す（ファイルから開いた波形はそのまま）
        const live = waveform.getState().fileName === LIVE_FILE_NAME;
        waveform.setState({ cursors: [], hoverCursor: null, ...(live ? { chartData: null, zoomRange: null } : {}) });
      }
      if (req.cmd === "pause") scheduleCommit();
    } catch (e) {
      notify.error(`操作に失敗しました: ${(e as Error).message}`);
    }
  }

  // ---- 監視一覧 ----
  function addWatch(name: string, plot = true) {
    const s = store.getState();
    if (!name || s.watch.some((w) => w.name === name)) return;
    set({ watch: [...s.watch, { name, plot, radix: "dec" }] });
    persist();
    void syncWatch();
  }

  function removeWatch(name: string) {
    set({ watch: store.getState().watch.filter((w) => w.name !== name) });
    persist();
    void syncWatch();
  }

  function updateWatch(name: string, patch: Partial<WatchItem>) {
    set({ watch: store.getState().watch.map((w) => (w.name === name ? { ...w, ...patch } : w)) });
    persist();
    if ("plot" in patch) syncSignals();
  }

  // ---- 入力 ----
  function addInput(name: string, widget: InputWidget = "number") {
    const s = store.getState();
    if (!name || s.inputs.some((i) => i.name === name)) return;
    const item: InputItem = { name, widget, min: 0, max: widget === "toggle" || widget === "button" ? 1 : 100, step: 1, hold: false };
    set({ inputs: [...s.inputs, item] });
    persist();
    void syncWatch();
  }

  function removeInput(name: string) {
    const item = store.getState().inputs.find((i) => i.name === name);
    set({ inputs: store.getState().inputs.filter((i) => i.name !== name) });
    persist();
    if (item?.hold) void api.release(name).catch(() => undefined);
    void syncWatch();
  }

  function updateInput(name: string, patch: Partial<InputItem>) {
    set({ inputs: store.getState().inputs.map((i) => (i.name === name ? { ...i, ...patch } : i)) });
    persist();
  }

  async function write(name: string, value: number | string, hold?: boolean) {
    const item = store.getState().inputs.find((i) => i.name === name);
    const h = hold ?? item?.hold ?? false;
    try {
      await api.write(name, value, h);
      if (item && typeof value === "number") updateInput(name, { value });
    } catch (e) {
      notify.error(`${name} に書き込めませんでした: ${(e as Error).message}`);
    }
  }

  async function setHold(name: string, hold: boolean, value?: number) {
    updateInput(name, { hold });
    try {
      if (hold) {
        const item = store.getState().inputs.find((i) => i.name === name);
        const v = value ?? item?.value ?? store.getState().values[name] ?? 0;
        await api.write(name, v, true);
      } else {
        await api.release(name);
      }
    } catch (e) {
      notify.error(`固定を切り替えられませんでした: ${(e as Error).message}`);
    }
  }

  // ---- 表示 ----
  function setWindowSize(n: number) {
    set({ windowSize: Math.max(50, Math.floor(n)) });
    persist();
  }

  function setMaxSamples(n: number) {
    const max = Math.max(1000, Math.floor(n));
    record.setMaxSamples(max);
    set({ maxSamples: max });
    persist();
  }

  // ---- CSV ----
  async function loadCsv(file: Blob) {
    try {
      const res = await api.loadCsv(file);
      const s = store.getState();
      const known = new Set(s.watch.map((w) => w.name));
      const added = (res.names ?? []).filter((n) => !known.has(n)).map((name) => ({ name, plot: true, radix: "dec" as const }));
      if (added.length) {
        set({ watch: [...s.watch, ...added] });
        persist();
      }
      record.clear();
      syncSignals();
      notify.success(`テスト CSV を読み込みました（${res.rows} 行、${(res.names ?? []).length} 変数）`);
    } catch (e) {
      notify.error(`テスト CSV を読み込めませんでした: ${(e as Error).message}`);
    }
  }

  function exportCsv() {
    const a = document.createElement("a");
    a.href = api.exportUrl();
    a.download = "sim_record.csv";
    document.body.appendChild(a);
    a.click();
    a.remove();
  }

  return {
    api, record, handlers, start, stop, control, commit, syncWatch,
    addWatch, removeWatch, updateWatch, addInput, removeInput, updateInput, write, setHold,
    setWindowSize, setMaxSamples, loadCsv, exportCsv,
    /** テスト用: 保留中の最新値を即座に反映する */
    flushValues,
  };
}

export type LiveController = ReturnType<typeof createLiveController>;

let singleton: LiveController | null = null;
export function liveController(): LiveController {
  if (!singleton) singleton = createLiveController();
  return singleton;
}
