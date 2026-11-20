import { createSignalsPersistence } from "../store/signalsPersist";
import { sanitizeVariants, type Variant, type VariantsDoc } from "./variants";
import { persistLayoutLimits, persistLiveLayout, setVariantsApplyError, useVariantsStore, variantLayout } from "./variantsStore";
// ライブモードの制御。サーバ検出、SSE の取り込み、監視一覧・入力の同期、構成の保存をまとめて持つ。

import { getConversion } from "../utils/helpers";
import { mappingToRows } from "../utils/stateNames";
import { seriesNames } from "../utils/waveformCSV";
import { shortSeriesName, watchedNames, watchedSeriesNames, type WatchLayout } from "./seriesNames";
import toast from "react-hot-toast";
import { LIVE_FILE_NAME, useWaveformStore } from "../store/useWaveformStore";
import { ApiError, createApi, detectServer, type ControlRequest, type DroppedItem, type SimApi, type StateMessage } from "./api";
import { dropPending, physicalInputValue, inputValue, restoreHoldText, upsertPending, type InputValue } from "./inputValue";
import { liveRecord, type LiveRecord } from "./liveRecord";
import { useLiveStore, type LiveStoreState } from "./liveStore";
import { loadLayout, type InputItem, type InputWidget, type WatchItem } from "./persist";
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
  notify?: { error: (m: string) => void; success: (m: string) => void; warn?: (m: string) => void };
}

export function createLiveController(deps: LiveControllerDeps = {}) {
  const api = deps.api ?? createApi("");
  const record = deps.record ?? liveRecord;
  const notify = deps.notify ?? {
    // サーバのメッセージ（列番号付きの 400 など）を読み切れるよう、幅を広げて少し長く出す
    error: (m: string) => toast.error(m, { duration: 8_000, style: { whiteSpace: "pre-line", maxWidth: 520 } }),
    success: (m: string) => toast.success(m),
    warn: (m: string) => toast(m, { duration: 10_000, style: { whiteSpace: "pre-line", maxWidth: 520 } }),
  };
  const valueFlushMs = deps.valueFlushMs ?? 200;
  const commitDelayMs = deps.commitDelayMs ?? 250;
  const store = useLiveStore;
  const waveform = useWaveformStore;

  let closeStream: (() => void) | null = null;
  let flushTimer: ReturnType<typeof setInterval> | null = null;
  let commitTimer: ReturnType<typeof setTimeout> | null = null;
  const pendingValues: Record<string, number | null> = {};
  let pendingDirty = false;

  /** いまの対象関数（変わったらリセットと同じく手元の記録を消す）。 */
  let knownFunc = "";
  /** 応答待ちの記録切替。SSE の巻き戻りで既に新しい記録になったかを共有する。 */
  let recordSwitch: { received: boolean } | null = null;

  function finishRecordSwitch(change: { received: boolean }) {
    if (!change.received) clearLiveRecord();
  }
  /** 最後に反映した `dropped`（JSON）。SSE と応答で同じものを受けても 1 回だけ反映・通知する。 */
  let droppedKey = "[]";
  /** 要求を送った時点の `dropped`。応答のあとに遅れて届いた古い state のものは反映しない。 */
  let staleDroppedKey: string | null = null;
  /** 応答待ちの `/api/control`・`/api/load-csv` の数（その間に届いた state の dropped は見送る）。 */
  let resolving = 0;
  /** 固定の切り替え・書き込みが応答待ちの名前（その間は state.holds に合わせない）。 */
  const holdBusy = new Map<string, number>();
  const holdRequests = new Set<Promise<unknown>>();
  /** 押している間だけ 1 のボタンで一時的に固定している名前（固定 ON とは表示しない）。 */
  const momentary = new Set<string>();
  /**
   * 固定に書いた文字列（名前 → 書いた表記）。サーバの state.holds は数値にできる値を JSON の数値で返し、
   * 2^53 を超える整数は丸まって届くので、同じ数値ならこの表記に戻して持つ（送り返す値を丸めないため）。
   */
  const heldText = new Map<string, string>();

  const set = (patch: Partial<LiveStoreState>) => store.setState(patch);

  function persist() {
    const s = store.getState();
    persistLiveLayout({ inputs: s.inputs, watch: s.watch, windowSize: s.windowSize, maxSamples: s.maxSamples }, api);
  }

  let previousLayout: WatchLayout | undefined;
  let watchSeries = new Map<string, string>();

  function watchedSeries(s: WatchLayout = store.getState()) {
    if (s.watch !== previousLayout?.watch || s.inputs !== previousLayout.inputs) {
      watchSeries = watchedSeriesNames(s);
      previousLayout = { watch: s.watch, inputs: s.inputs };
    }
    return watchSeries;
  }

  function seriesName(name: string, s?: WatchLayout) {
    return watchedSeries(s).get(name) ?? shortSeriesName(name);
  }

  let recordNames = new Map<string, string>();
  let recordWatchSeries: ReadonlyMap<string, string> | undefined;

  function recordSeries(names: readonly string[]) {
    const watched = watchedSeries();
    if (recordWatchSeries !== watched || names.some(name => !recordNames.has(name))) {
      const next = new Map<string, string>();
      const used = new Set(watched.values());
      for (const name of new Set([...recordNames.keys(), ...names])) {
        let series = watched.get(name);
        if (series === undefined) {
          // 監視から外した列や遅れて届いた列は、現行の監視列と混ぜずに保持する。
          series = shortSeriesName(name);
          if (used.has(series)) series = name;
          while (used.has(series)) series = "/" + series;
          used.add(series);
        }
        next.set(name, series);
      }
      const renames = new Map<string, string>();
      for (const [name, old] of recordNames) {
        const series = next.get(name)!;
        if (old !== series) renames.set(old, series);
      }
      recordNames = next;
      recordWatchSeries = watched;
      if (record.renameColumns(renames)) {
        const { fileName, chartData } = waveform.getState();
        if (fileName === LIVE_FILE_NAME && chartData) waveform.setState({
          chartData: { ...chartData, seriesMap: Object.fromEntries(
            Object.entries(chartData.seriesMap).map(([name, values]) => [renames.get(name) ?? name, values]),
          ) },
        });
      }
    }
    return names.map(name => recordNames.get(name)!);
  }

  let syncedSeries = new Map<string, string>();
  const usedSeries = new Set<string>();

  function syncSignals() {
    const s = store.getState();
    const series = watchedSeries(s);
    const watchNames = new Set(s.watch.map(w => w.name));
    const plotted = new Set([
      ...s.watch.filter(w => w.plot),
      ...s.inputs.filter(i => i.monitor !== false && !watchNames.has(i.name)),
    ].map(({ name }) => seriesName(name, s)));
    const carry = new Map<string, boolean>();
    for (const [name, key] of series) {
      const oldKey = syncedSeries.get(name);
      // 初めて使う系列の設定は YAML などから引き継ぐ。監視で使った系列の再割り当てでは plot を優先する。
      if (oldKey !== key && (oldKey !== undefined || usedSeries.has(key))) {
        carry.set(key, plotted.has(key));
      }
      usedSeries.add(key);
    }
    syncedSeries = series;
    waveform.getState().ensureLiveSignals([...series.values()], plotted);
    // ensureLiveSignals は既存の checked を保持し、新規設定には YAML を適用することがある。
    for (const [key, checked] of carry) {
      const state = waveform.getState();
      if (state.signalSettings[key].checked !== checked) state.toggleSignalCheck(key);
    }
    recordSeries([]);
    const current = store.getState();
    const settings = waveform.getState().signalSettings;
    const watch = current.watch.map(w => {
      const setting = settings[seriesName(w.name, current)];
      return setting && setting.checked !== w.plot ? { ...w, plot: setting.checked } : w;
    });
    if (watch.some((w, i) => w !== current.watch[i])) {
      set({ watch });
      persist();
    }
  }

  async function syncWatch() {
    syncSignals();
    const names = watchedNames(store.getState());
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

  /** 手元のライブ記録と、Viewer に載せたライブ波形を消す（ファイルから開いた波形はそのまま）。 */
  function clearLiveRecord() {
    if (commitTimer) { clearTimeout(commitTimer); commitTimer = null; }
    record.clear();
    recordNames.clear();
    recordWatchSeries = undefined;
    const live = waveform.getState().fileName === LIVE_FILE_NAME;
    waveform.setState({ cursors: [], hoverCursor: null, ...(live ? { chartData: null, zoomRange: null } : {}) });
  }

  /** 対象関数が変わったと分かったら（応答・state・hello のどれで先に知っても 1 回だけ）記録を消す。 */
  function noteFunc(func: string | undefined) {
    if (!func) return;
    if (knownFunc && func !== knownFunc && !recordSwitch) clearLiveRecord();
    knownFunc = func;
  }

  async function trackHold<T>(name: string, fn: () => Promise<T>): Promise<T> {
    holdBusy.set(name, (holdBusy.get(name) ?? 0) + 1);
    let request: Promise<T> | undefined;
    try {
      request = fn();
      holdRequests.add(request);
      return await request;
    } finally {
      if (request) holdRequests.delete(request);
      const n = (holdBusy.get(name) ?? 1) - 1;
      if (n > 0) holdBusy.set(name, n); else holdBusy.delete(name);
    }
  }

  async function applyVariantLayout(inputs: InputItem[], watch: WatchItem[], previousHolds: string[]) {
    const nextHolds = new Set(inputs.filter(i => i.hold && i.value !== undefined).map(i => i.name));
    const released = new Set([...previousHolds.filter(name => !nextHolds.has(name)), ...momentary]);
    momentary.clear();
    const revision = store.getState().layoutRevision + 1;
    set({ inputs: inputs.map(i => i.hold && i.value === undefined ? { ...i, holdPending: true } : i), watch, layoutRevision: revision });
    const settings = waveform.getState().signalSettings;
    const checked = watch.map(w => ({ name: seriesName(w.name), plot: w.plot }))
      .filter(w => settings[w.name] && settings[w.name].checked !== w.plot);
    if (checked.length) waveform.setState({ signalSettings: {
      ...settings, ...Object.fromEntries(checked.map(w => [w.name, { ...settings[w.name], checked: w.plot }])),
    } });
    const changeHold = (name: string, change: () => Promise<void>) => trackHold(name, async () => {
      try {
        await change();
      } catch (e) {
        notify.error(`固定を切り替えられませんでした: ${(e as Error).message}`);
      }
    });
    const releases = Promise.all([...released].map(name => changeHold(name, async () => {
      await api.release(name);
      noteReleased(name);
    })));
    // 解除が終わるまで待つ間も、SSE で新しい入力カードの固定表示を戻さない。
    const writes = inputs.flatMap(({ name, hold, value }) => hold && value !== undefined
      ? [changeHold(name, async () => {
        await releases;
        const state = store.getState();
        if (state.layoutRevision !== revision) return;
        const input = state.inputs.find(i => i.name === name);
        if (input?.hold !== true || input.value !== value) return;
        await writeFixed(name, value);
      })] : []);
    await Promise.all([releases, ...writes]);
    await syncWatch();
  }

  async function selectVariant(next: VariantsDoc) {
    while (holdRequests.size) await Promise.allSettled(holdRequests);
    const { doc } = useVariantsStore.getState();
    const current = store.getState();
    const layout = variantLayout(current);
    const variants = next.variants.map(v => {
      const latest = doc.variants.find(old => old.name === v.name);
      const updated = v.name === doc.selected ? { ...v, ...layout }
        : latest ? { ...latest, values: v.values } : v;
      return v.name === next.selected ? { ...updated, inputs: updated.inputs ?? layout.inputs, watch: updated.watch ?? layout.watch } : updated;
    });
    useVariantsStore.setState({ doc: { ...next, variants } });
    if (doc.selected === next.selected) return;
    const target = variants.find(v => v.name === next.selected) ?? loadLayout();
    await applyVariantLayout(target.inputs ?? layout.inputs, target.watch ?? layout.watch, current.inputs.filter(i => i.hold).map(i => i.name));
  }

  /** 入力パネルの固定表示をサーバの state.holds に合わせる（切り替え中・ボタンを押している間の名前は除く）。 */
  function reconcileHolds(holds: StateMessage["holds"]) {
    if (!holds) return;
    const held = new Set(holds.map((h) => h.name));
    const inputs = store.getState().inputs;
    let changed = false;
    const next = inputs.map((i) => {
      if (holdBusy.has(i.name) || momentary.has(i.name)) return i;
      if (i.holdPending && !held.has(i.name)) return i;
      const want = held.has(i.name);
      if (i.hold === want && !i.holdPending) return i;
      changed = true;
      return { ...i, hold: want, holdPending: undefined };
    });
    if (changed) {
      set({ inputs: next });
      persist();
    }
  }

  /** サーバが外した固定・監視を手元の構成に反映する。 */
  function applyDropped(list: DroppedItem[], announce: boolean) {
    if (!list.length) return;
    const holds = new Set(list.filter((d) => d.kind === "hold").map((d) => d.name));
    const watches = new Set(list.filter((d) => d.kind === "watch").map((d) => d.name));
    const s = store.getState();
    const inputs = s.inputs.map((i) => {
      let it = i;
      if (holds.has(i.name) && it.hold) it = { ...it, hold: false, holdPending: undefined };
      if (watches.has(i.name) && it.monitor !== false) it = { ...it, monitor: false };
      return it;
    });
    for (const n of holds) { momentary.delete(n); heldText.delete(n); }
    const watchErrors = { ...s.watchErrors };
    const values = { ...s.values };
    for (const n of watches) {
      delete watchErrors[n];
      delete values[n];
      delete pendingValues[n];
    }
    set({
      inputs,
      watch: s.watch.filter((w) => !watches.has(w.name)),
      watchErrors,
      values,
      serverWatch: s.serverWatch.filter((n) => !watches.has(n)),
    });
    persist();
    syncSignals();
    if (announce) {
      const lines = list.map((d) => `${d.name}（${d.kind === "hold" ? "固定" : d.kind === "init" ? "初期化" : "監視"}）: ${d.message || "解決できません"}`);
      (notify.warn ?? notify.error)(`今の対象関数では使えないため、次を外しました\n${lines.join("\n")}`);
    }
  }

  /** state の `dropped` を受ける。SSE と応答で同じものを何度受けても 1 回だけ反映・通知する。 */
  function onDropped(list: DroppedItem[] | undefined, fromResponse: boolean) {
    if (!list) return;
    const key = JSON.stringify(list);
    if (!fromResponse) {
      if (resolving > 0) return;
      if (staleDroppedKey !== null) {
        const stale = key === staleDroppedKey && key !== droppedKey;
        staleDroppedKey = null;
        if (stale) return;
      }
      if (key === droppedKey) return;
    }
    droppedKey = key;
    applyDropped(list, true);
  }

  /** `/api/control`・`/api/load-csv` を送る間の dropped の扱い（送る前の dropped は古いものとして見送る）。 */
  async function resolvingRequest<T>(fn: () => Promise<T>): Promise<T> {
    if (resolving === 0) staleDroppedKey = droppedKey;
    resolving++;
    try {
      return await fn();
    } finally {
      resolving--;
    }
  }

  /** state を手元に取り込む（SSE・応答で共通）。 */
  function applyState(msg: StateMessage, fromResponse: boolean) {
    const { dropped, ...rest } = msg;
    if (rest.holds) rest.holds = restoreHoldText(rest.holds, heldText);
    const prev = store.getState().sim;
    set({ sim: { ...prev, ...rest } });
    // ステップ時の書き込みの誤り（ポインタの先が NULL など）は、新しく出たときに知らせる（表示は接続バーに残る）
    if (rest.error && rest.error !== prev.error) notify.error(`シミュレータのエラー: ${rest.error}`);
    if (rest.periodMs && rest.periodMs !== prev.periodMs) waveform.getState().setTaskPeriod(rest.periodMs);
    noteFunc(rest.func);
    onDropped(dropped, fromResponse);
    reconcileHolds(rest.holds);
    return prev;
  }

  function flushValues() {
    if (!pendingDirty) return;
    pendingDirty = false;
    set({ values: { ...store.getState().values, ...pendingValues } });
  }

  const handlers: StreamHandlers = {
    hello: (msg) => {
      noteFunc(msg.func);
      const sim = store.getState().sim;
      set({
        serverWatch: msg.names,
        sim: { ...sim, periodMs: msg.periodMs ?? sim.periodMs, func: msg.func ?? sim.func },
      });
      if (msg.periodMs) waveform.getState().setTaskPeriod(msg.periodMs);
      for (const n of recordSeries(msg.names)) record.ensureColumn(n);
    },
    samples: (msg) => {
      const rewinds = msg.rows.length > 0 && record.lastStep !== null && msg.step0 <= record.lastStep;
      if (recordSwitch && msg.rows.length &&
          (msg.step0 <= 1 || rewinds)) {
        recordSwitch.received = true;
      }
      if (rewinds) recordNames.clear();
      // 既定の record は useWaveformStore.appendSamples が書く共有のライブ記録と同じもの
      const names = recordSeries(msg.names);
      const res = record === liveRecord
        ? waveform.getState().appendSamples(msg.step0, names, msg.rows)
        : record.append(msg.step0, names, msg.rows);
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
    values: (msg) => {
      // 現在値の表示だけを更新する（記録・波形・ステップ番号は変えない）
      msg.names.forEach((n, i) => { pendingValues[n] = msg.values[i] ?? null; });
      if (msg.names.length) pendingDirty = true;
    },
    state: (msg) => {
      const prev = applyState(msg, false);
      if (prev.running && msg.running === false) scheduleCommit();
    },
    error: (m) => console.warn("[sim stream]", m),
  };

  let signalsPersistence: ReturnType<typeof createSignalsPersistence> | undefined;
  async function start(): Promise<boolean> {
    signalsPersistence?.stop();
    signalsPersistence = createSignalsPersistence(api);
    set({ mode: "detecting" });
    const info = await detectServer(api, deps.protocol);
    if (!info) {
      set({ mode: "offline" });
      await signalsPersistence.start("offline");
      return false;
    }
    let selected: Variant | undefined;
    try {
      const result = await api.getVariants();
      const doc = sanitizeVariants(result.doc);
      const error = result.error || (result.exists && (!result.doc || typeof result.doc !== "object" || !Array.isArray((result.doc as Record<string, unknown>).variants)) ? "仕向ファイルの形式が不正です" : "");
      useVariantsStore.setState({ doc, saved: JSON.stringify(doc), path: result.path, error, loaded: true });
      if (!error) selected = doc.variants.find(v => v.name === doc.selected);
    } catch (e) {
      useVariantsStore.setState({ ...useVariantsStore.getInitialState(), loaded: true, error: (e as Error).message });
    }
    await signalsPersistence.start("live");
    // 選択なし・取得失敗でも、サーバに残った仕向の初期化を解除する。
    useVariantsStore.setState({ errors: {} });
    try { await api.setInit(selected?.values ?? []); }
    catch (e) { if (!(e instanceof ApiError && e.status === 404)) setVariantsApplyError(e); }
    const layout = loadLayout();
    record.setMaxSamples(layout.maxSamples);
    // 再読み込みの前に固定した文字列（保存した構成の最後の値）を、丸まって届く state.holds の表記に使う
    heldText.clear();
    for (const i of layout.inputs) if (i.hold && typeof i.value === "string") heldText.set(i.name, i.value);
    knownFunc = info.func;
    droppedKey = "[]";
    staleDroppedKey = null;
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
        holds: restoreHoldText(info.state.holds ?? [], heldText),
        overrun: !!info.state.overrun,
        pending: info.state.pending ?? [],
        error: info.state.error ?? "",
        needReset: info.state.needReset ?? false,
        initStale: info.state.initStale ?? false,
        periodMs: info.periodMs,
        func: info.func,
      },
    });
    waveform.getState().setTaskPeriod(info.periodMs);
    // 前に外された分を保存した構成にも反映する（通知は関数を変えたときだけ）
    const hasVariantLayout = selected && (selected.inputs || selected.watch);
    if (info.state.dropped?.length) {
      droppedKey = JSON.stringify(info.state.dropped);
      if (!hasVariantLayout) applyDropped(info.state.dropped, false);
    }
    if (selected && hasVariantLayout) {
      await applyVariantLayout(selected.inputs ?? layout.inputs, selected.watch ?? layout.watch, (info.state.holds ?? []).map(h => h.name));
    } else {
      reconcileHolds(info.state.holds ?? []);
      await syncWatch();
    }
    closeStream = openStream(api.streamUrl(), {
      ...handlers,
      open: () => set({ connected: true }),
      lost: () => set({ connected: false }),
    }, deps.createEventSource);
    flushTimer = setInterval(flushValues, valueFlushMs);
    return true;
  }

  function stop() {
    signalsPersistence?.stop();
    closeStream?.();
    closeStream = null;
    if (flushTimer) clearInterval(flushTimer);
    flushTimer = null;
    if (commitTimer) clearTimeout(commitTimer);
    commitTimer = null;
  }

  async function control(req: ControlRequest) {
    const change = req.cmd === "reset" || (req.func !== undefined && req.func !== knownFunc)
      ? { received: false } : null;
    if (change) recordSwitch = change;
    try {
      const res = await resolvingRequest(() => api.control(req));
      const sim = store.getState().sim;
      // 応答の state イベントを待たずに表示を合わせる
      if (req.cmd === "run") set({ sim: { ...sim, running: true } });
      if (req.cmd === "pause") set({ sim: { ...sim, running: false } });
      if (req.speed !== undefined) set({ sim: { ...store.getState().sim, speed: req.speed } });
      if (req.periodMs !== undefined) {
        set({ sim: { ...store.getState().sim, periodMs: req.periodMs } });
        waveform.getState().setTaskPeriod(req.periodMs);
      }
      // 対象関数が変わるとサーバはリセットする（記録を消し、外した固定・監視を dropped で返す）
      if (res) applyState(res, true);
      else if (req.func !== undefined) {
        set({ sim: { ...store.getState().sim, func: req.func } });
        noteFunc(req.func);
      }
      if (change) finishRecordSwitch(change);
      if (req.cmd === "pause") scheduleCommit();
    } catch (e) {
      notify.error(`操作に失敗しました: ${(e as Error).message}`);
    } finally {
      if (recordSwitch === change) recordSwitch = null;
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
    if (patch.plot !== undefined) {
      syncSignals();
      const state = waveform.getState();
      const series = seriesName(name);
      if (state.signalSettings[series]?.checked !== patch.plot) state.toggleSignalCheck(series);
    }
  }

  // ---- 入力 ----
  /** 監視から外されていた入力を監視に戻す。 */
  function remonitor(name: string) {
    const item = store.getState().inputs.find((i) => i.name === name);
    if (!item || item.monitor !== false) return;
    updateInput(name, { monitor: undefined });
    void syncWatch();
  }

  function addInput(name: string, widget?: InputWidget) {
    const s = store.getState();
    if (!name) return;
    if (s.inputs.some((i) => i.name === name)) {
      remonitor(name);
      return;
    }
    if (widget === undefined) {
      const names = [...new Set([...watchedNames(s), name])];
      const series = seriesNames(names)[names.indexOf(name)];
      widget = mappingToRows(waveform.getState().modeMapping[series]).length ? "state" : "number";
    }
    const item: InputItem = { name, widget, min: 0, max: widget === "toggle" || widget === "button" ? 1 : 100, step: 1, hold: widget !== "button", ...(widget !== "button" ? { holdPending: true } : {}) };
    set({ inputs: [...s.inputs, item] });
    persist();
    void syncWatch();
  }

  function moveInput(from: number, to: number) {
    const inputs = [...store.getState().inputs];
    if (!Number.isInteger(from) || !Number.isInteger(to) || from < 0 || to < 0 || from >= inputs.length || to >= inputs.length || from === to) return;
    inputs.splice(to, 0, inputs.splice(from, 1)[0]);
    set({ inputs });
    persist();
  }

  /** 入力を外した回数（名前ごと）。型を待つ間に外した・足し直したカードへの書き込みを取り消すために使う */
  const removals = new Map<string, number>();
  function removeInput(name: string) {
    removals.set(name, (removals.get(name) ?? 0) + 1);
    const item = store.getState().inputs.find((i) => i.name === name);
    set({ inputs: store.getState().inputs.filter((i) => i.name !== name) });
    persist();
    momentary.delete(name);
    heldText.delete(name);
    if (item?.hold) void api.release(name).then(() => noteReleased(name), () => undefined);
    void syncWatch();
  }

  function updateInput(name: string, patch: Partial<InputItem>) {
    set({ inputs: store.getState().inputs.map((i) => {
      if (i.name !== name) return i;
      const next = { ...i, ...patch };
      // まだ値を書いていない新規入力をボタンにしたら、既定の固定も外す（ボタンは押している間だけの一時的な入力）
      if (patch.widget === "button" && next.holdPending) { next.hold = false; next.holdPending = undefined; }
      return next;
    }) });
    persist();
  }

  /** 書き込みを受け付けたサーバの状態を、state を待たずに手元へ写す（続けて操作したときの元の値にするため）。 */
  function noteWritten(name: string, value: number | string, hold: boolean) {
    const sim = store.getState().sim;
    if (hold) {
      if (typeof value === "string") heldText.set(name, value); else heldText.delete(name);
      // 固定すると同じ名前の反映待ちは捨てられ、その周期からは固定の値になる
      set({ sim: { ...sim, pending: dropPending(sim.pending, name),
        holds: [...sim.holds.filter((h) => h.name !== name), { name, value }] } });
    } else {
      set({ sim: { ...sim, pending: upsertPending(sim.pending, name, String(value)) } });
    }
  }

  /** 固定を外したことを手元へ写す（その名前の反映待ちも捨てられる）。 */
  function noteReleased(name: string) {
    heldText.delete(name);
    const sim = store.getState().sim;
    set({ sim: { ...sim, pending: dropPending(sim.pending, name), holds: sim.holds.filter((h) => h.name !== name) } });
  }

  /** 入力の元にする値（反映待ち → 固定の値 → メモリの現在値）。トグルの反転やボタンの復帰に使う。 */
  function inputBase(name: string): number | null {
    return inputBaseValue(name).value;
  }

  /** 入力の元にする値（送り返してよい値 `send` を含む）。 */
  function inputBaseValue(name: string): InputValue {
    const s = store.getState();
    return inputValue(name, s.sim, s.values[name]);
  }

  const inexactMessage = (name: string) =>
    `${name} の現在値は 2^53 を超える整数のため、丸められている可能性があり正確な値が分かりません。値を入力してください`;

  /**
   * 値を書く。hold を省くと、固定中の入力は固定値の更新（hold:true）、そうでなければ単発（次のステップの開始時に反映）。
   */
  async function write(name: string, value: number | string, hold?: boolean, pressingButton = false) {
    const item = store.getState().inputs.find((i) => i.name === name);
    const h = hold ?? item?.hold ?? false;
    // 固定 OFF の入力を一時的に固定する（押している間だけ 1 のボタン）。固定 ON の表示にはしない
    const temporary = h && (pressingButton || item?.widget === "button" && !item.hold);
    if (h && item && (!item.hold || temporary)) momentary.add(name);
    try {
      await trackHold(name, () => api.write(name, value, h));
      noteWritten(name, value, h);
      if (!h) momentary.delete(name);
      // 数値入力の文字列は書いたまま残す（構成の保存・復元でも丸めない）
      if (item && !temporary) updateInput(name, { value, holdPending: undefined });
      remonitor(name);
      return true;
    } catch (e) {
      if (!store.getState().sim.holds.some((h) => h.name === name)) momentary.delete(name);
      notify.error(`${name} に書き込めませんでした: ${(e as Error).message}`);
      return false;
    }
  }

  // 型の取得は物理値を書いたときだけ。関数が変われば別のキャッシュを使う。
  const symbolKinds = new Map<string, ReturnType<typeof api.symbols>>();
  async function writePhysical(name: string, value: number | string, hold?: boolean): Promise<number | null> {
    try {
      const func = store.getState().sim.func;
      const revision = store.getState().layoutRevision;
      const removed = removals.get(name) ?? 0;
      // 換算は入力したときの設定で行う（型を待つ間に yaml などで設定が変わっても、利用者が見ていた換算のまま送る）
      const settings = waveform.getState();
      const conversion = getConversion(seriesName(name), settings.lsbMapping, settings.offsetMapping);
      const key = `${func}:${name}`;
      let request = symbolKinds.get(key);
      if (!request) {
        request = api.symbols(name, 200);
        symbolKinds.set(key, request);
        void request.catch(() => symbolKinds.delete(key));
      }
      const symbol = (await request).find(s => s.name === name);
      // 型を待つ間に対象関数が変わったら、前の関数の型で換算した値を新しい関数へ書かない
      if (store.getState().sim.func !== func) throw new Error("型を調べている間に対象関数が変わったため、書き込みを取り消しました");
      // 型を待つ間にカードを外した（足し直した）ら書かない。外したカードの固定が残り、以後の実験に入り続けるため
      if (store.getState().layoutRevision !== revision || (removals.get(name) ?? 0) !== removed || !store.getState().inputs.some(i => i.name === name)) {
        throw new Error("型を調べている間に入力から外したため、書き込みを取り消しました");
      }
      if (!symbol) { symbolKinds.delete(key); throw new Error("型を取得できません"); }
      const raw = physicalInputValue(String(value), conversion, symbol.kind);
      if (raw === null) throw new Error("物理値または換算・型が不正です");
      if (!await write(name, raw, hold)) return null;
      if (hold !== undefined) { momentary.delete(name); updateInput(name, { hold, holdPending: undefined }); }
      return raw;
    } catch (e) {
      notify.error(`${name} に書き込めませんでした: ${(e as Error).message}`);
      return null;
    }
  }

  /**
   * 押している間だけ 1 のボタンを離した: 一時的な固定を解き、押す前の値（restore。無ければ 0）を書く
   * （固定 ON の項目はその値で固定し直す）。restore が null（押す前の値が丸められていて正確に分からない）なら書かない。
   */
  async function releaseButton(name: string, restore: number | string | null = 0, revision = store.getState().layoutRevision) {
    if (revision !== store.getState().layoutRevision) return;
    await trackHold(name, async () => {
      await api.release(name).then(() => noteReleased(name), () => undefined);
      momentary.delete(name);
      const item = store.getState().inputs.find((i) => i.name === name);
      if (restore === null) {
        notify.error(`${name} を押す前の値に戻せませんでした: ${inexactMessage(name)}`);
        return;
      }
      await write(name, restore, item?.hold ?? false);
    });
  }

  async function writeFixed(name: string, value: number | string) {
    await api.write(name, value, true);
    noteWritten(name, value, true);
  }

  /**
   * 固定を切り替える。value は固定する値（文字列なら書いた表記のまま送る）。省くと、最後に書いた値 → 元にする値 → 0。
   * null は「正確な値が分からない」（丸められた現在値しかない）ことを表し、送らずに知らせる。
   */
  async function setHold(name: string, hold: boolean, value?: number | string | null) {
    momentary.delete(name);
    let v: number | string | null = 0;
    if (hold) {
      const item = store.getState().inputs.find((i) => i.name === name);
      const base = inputBaseValue(name);
      v = value === null ? null : value ?? item?.value ?? base.send ?? (base.inexact ? null : 0);
      if (v === null) {
        notify.error(`固定を切り替えられませんでした: ${inexactMessage(name)}`);
        return;
      }
    }
    updateInput(name, { hold, holdPending: undefined });
    try {
      await trackHold(name, async () => {
        if (hold) {
          await writeFixed(name, v!);
          updateInput(name, { value: v! });
        } else {
          // 固定を外すと、その名前の反映待ちも捨てられる
          await api.release(name);
          noteReleased(name);
        }
      });
      if (hold) remonitor(name);
    } catch (e) {
      // 固定できなかった（今の関数で解決できないなど）。表示はサーバの state.holds に合わせ直す
      if (hold) updateInput(name, { hold: false });
      notify.error(`固定を切り替えられませんでした: ${(e as Error).message}`);
    }
  }

  // ---- 表示 ----
  function setWindowSize(n: number) {
    set({ windowSize: Math.max(50, Math.floor(n)) });
    persistLayoutLimits(store.getState());
  }

  function setMaxSamples(n: number) {
    const max = Math.max(1000, Math.floor(n));
    record.setMaxSamples(max);
    set({ maxSamples: max });
    persistLayoutLimits(store.getState());
  }

  // ---- CSV ----
  async function loadCsv(file: Blob) {
    const change = { received: false };
    recordSwitch = change;
    try {
      const res = await resolvingRequest(() => api.loadCsv(file));
      const s = store.getState();
      const known = new Set(s.watch.map((w) => w.name));
      const added = (res.names ?? []).filter((n) => !known.has(n)).map((name) => ({ name, plot: true, radix: "dec" as const }));
      if (added.length) {
        set({ watch: [...s.watch, ...added] });
        persist();
      }
      finishRecordSwitch(change);
      syncSignals();
      notify.success(`テスト CSV を読み込みました（${res.rows} 行、${(res.names ?? []).length} 変数）`);
      // 一部だけを再生する（複数の mod ブロックのうち先頭だけ）などは、成功とは別に長く残る警告で知らせる
      if (res.warning) (notify.warn ?? notify.error)(`テスト CSV の警告: ${res.warning}`);
    } catch (e) {
      // 400（同じ名前の入力列、UTF-16 など）はサーバが何も変えないので、手元の記録・波形・入力パネルもそのまま
      notify.error(`テスト CSV を読み込めませんでした: ${(e as Error).message}`);
    } finally {
      if (recordSwitch === change) recordSwitch = null;
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
    api, record, handlers, start, stop, control, commit, syncWatch, selectVariant, seriesName,
    addWatch, removeWatch, updateWatch, addInput, moveInput, removeInput, updateInput, write, writePhysical, releaseButton, setHold, inputBase, inputBaseValue,
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
