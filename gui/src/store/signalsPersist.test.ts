import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createSignalsPersistence, useSignalsPersistStore } from "./signalsPersist";
import { useWaveformStore as store } from "./useWaveformStore";
import { useLiveStore } from "../live/liveStore";
import { useVariantsStore } from "../live/variantsStore";
import { emptyDefs, emptyView } from "./signalDefs";

const initial = store.getState();
const config = "version: 2\nvariants: [EU]\nsignals: {X: {unit: V, variants: {EU: {unit: A}}}}\nview: {signals: {X: {color: '#f00'}}}\n";
const api = () => ({
  getSignals: vi.fn().mockResolvedValue({ path: "signals.yaml", exists: true, text: config, error: "" }),
  saveSignals: vi.fn().mockResolvedValue({ ok: true, path: "signals.yaml" }),
});
let local: Map<string, string>;
let persist: ReturnType<typeof createSignalsPersistence>;
const change = (unit: string) => store.getState().setSignalConversion("X", { unit });
const tick = () => vi.advanceTimersByTimeAsync(1000);
beforeEach(() => {
  vi.useFakeTimers();
  local = new Map();
  vi.stubGlobal("localStorage", { getItem: vi.fn((key: string) => local.get(key) ?? null), setItem: vi.fn((key: string, text: string) => { local.set(key, text); }) });
  store.setState({ ...initial, signalDefs: emptyDefs(), signalView: emptyView() });
  useLiveStore.setState({ mode: "live" });
  useVariantsStore.setState({ doc: { version: 1, selected: "", variants: [] } });
});
afterEach(() => { persist?.stop(); vi.useRealTimers(); vi.unstubAllGlobals(); });

describe("信号情報の自動保存", () => {
  it("読み込みが終わるまで保存しない", async () => {
    const mock = api();
    let resolve!: (value: unknown) => void;
    mock.getSignals.mockImplementation(() => new Promise(r => { resolve = r; }));
    persist = createSignalsPersistence(mock);
    const starting = persist.start("live");
    change("before"); await tick();
    expect(mock.saveSignals).not.toHaveBeenCalled();
    resolve({ path: "signals.yaml", exists: true, text: config, error: "" });
    await starting; await tick();
    expect(mock.saveSignals).not.toHaveBeenCalled();
    expect(store.getState().unitMapping.X).toBe("V");
    change("after"); await tick();
    expect(mock.saveSignals).toHaveBeenCalledTimes(1);
  });
  it.each([
    { text: config, error: "権限がありません" },
    { text: "broken: [", error: "" },
    { text: "version: 2\nsignals: []", error: "" },
  ])("取得エラーまたは不正 YAML なら保存しない: %j", async ({ text, error }) => {
    const mock = api(); mock.getSignals.mockResolvedValue({ path: "bad.yaml", exists: true, text, error });
    persist = createSignalsPersistence(mock); await persist.start("live");
    change("A"); await tick();
    expect(mock.saveSignals).not.toHaveBeenCalled();
    expect(useSignalsPersistStore.getState().error).toContain("信号情報ファイルを読めません:");
    expect(useSignalsPersistStore.getState().error).toContain("bad.yaml");
  });
  it.each(["", "  \n # コメントだけ\n  "])("空のファイルは既存の設定を保持して自動保存を有効にする: %j", async (text) => {
    change("before"); const before = store.getState().signalDefs;
    const mock = api(); mock.getSignals.mockResolvedValue({ path: "empty.yaml", exists: true, text, error: "" });
    persist = createSignalsPersistence(mock); await persist.start("live"); await tick();
    expect(store.getState().signalDefs).toBe(before);
    expect(useSignalsPersistStore.getState().error).toBe("");
    expect(mock.saveSignals).not.toHaveBeenCalled();
    change("after"); await tick();
    expect(mock.saveSignals).toHaveBeenCalledTimes(1);
    expect(mock.saveSignals.mock.calls[0][0]).toContain("version: 2");
  });
  it("取得が例外になっても自動保存しない", async () => {
    const mock = api(); mock.getSignals.mockRejectedValue(new Error("network"));
    persist = createSignalsPersistence(mock); await persist.start("live");
    change("A"); await tick();
    expect(mock.saveSignals).not.toHaveBeenCalled();
    expect(useSignalsPersistStore.getState().error).toContain("network");
  });
  it("ファイルがないときは何も読み込まず次の編集で v2 を作る", async () => {
    const mock = api(); mock.getSignals.mockResolvedValue({ path: "new.yaml", exists: false, text: null, error: "" });
    persist = createSignalsPersistence(mock); await persist.start("live"); await tick();
    expect(mock.saveSignals).not.toHaveBeenCalled();
    change("V"); await tick();
    expect(mock.saveSignals.mock.calls[0][0]).toContain("version: 2");
  });
  it("連続変更は最後の変更から1秒待ち、保存済みと同じなら送らない", async () => {
    const mock = api(); persist = createSignalsPersistence(mock); await persist.start("live");
    change("A"); await vi.advanceTimersByTimeAsync(600);
    change("B"); await vi.advanceTimersByTimeAsync(999);
    expect(mock.saveSignals).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1);
    expect(mock.saveSignals).toHaveBeenCalledTimes(1);
    change("B"); await tick();
    expect(mock.saveSignals).toHaveBeenCalledTimes(1);
  });
  it("読み込んだ v2 と同じテキストは保存しない", async () => {
    store.getState().parseMappingData(config, "yaml");
    const text = store.getState().getConfigYAML();
    const mock = api(); mock.getSignals.mockResolvedValue({ exists: true, text, path: "same.yaml", error: "" });
    persist = createSignalsPersistence(mock); await persist.start("live");
    store.setState({ signalSettings: { ...store.getState().signalSettings } });
    await tick(); expect(mock.saveSignals).not.toHaveBeenCalled();
  });
  it("毎周期の値やカーソルの更新では YAML を作らない", async () => {
    const mock = api(); persist = createSignalsPersistence(mock); await persist.start("live");
    const serialize = vi.spyOn(store.getState(), "getConfigYAML");
    store.setState({ hoverCursor: { time: 1, index: 0 }, chartData: { steps: [1], seriesMap: { X: [1] } } });
    await tick(); expect(serialize).not.toHaveBeenCalled();
    serialize.mockRestore();
  });
  it("保存失敗を表示し、次の変更で再試行する", async () => {
    const mock = api(); mock.saveSignals.mockRejectedValueOnce(new Error("disk full"));
    persist = createSignalsPersistence(mock); await persist.start("live");
    change("A"); await tick();
    expect(useSignalsPersistStore.getState().error).toContain("disk full");
    change("B"); await tick();
    expect(mock.saveSignals).toHaveBeenCalledTimes(2);
    expect(useSignalsPersistStore.getState().error).toBe("");
  });
  it("手で読んだ v1 も自動で v2 保存する", async () => {
    const mock = api(); persist = createSignalsPersistence(mock); await persist.start("live");
    store.getState().parseMappingData("X: {unit: 手動}\n", "yaml", false);
    await tick();
    expect(mock.saveSignals).toHaveBeenCalledTimes(1);
    expect(mock.saveSignals.mock.calls[0][0]).toContain("version: 2");
    expect(mock.saveSignals.mock.calls[0][0]).toContain("手動");
  });
  it("右パネルの仕向に追従し、名前を両方保存する", async () => {
    const mock = api(); persist = createSignalsPersistence(mock); await persist.start("live");
    useVariantsStore.setState({ doc: { version: 1, selected: "EU", variants: [{ name: "国内", values: [] }] } });
    expect(store.getState().selectedVariant).toBe("EU");
    expect(store.getState().unitMapping.X).toBe("A");
    await tick(); expect(mock.saveSignals.mock.calls[0][0]).toContain("国内");
  });
  it("detecting 中の読み込み後も右パネルの最新の仕向を適用する", async () => {
    useLiveStore.setState({ mode: "detecting" });
    useVariantsStore.setState({ doc: { version: 1, selected: "国内", variants: [] } });
    const mock = api();
    mock.getSignals.mockImplementation(async () => {
      useVariantsStore.setState({ doc: { version: 1, selected: "EU", variants: [] } });
      return { path: "signals.yaml", exists: true, text: config + "viewState: {variant: 国内}\n", error: "" };
    });
    persist = createSignalsPersistence(mock); await persist.start("live");
    expect(store.getState().selectedVariant).toBe("EU");
    expect(store.getState().unitMapping.X).toBe("A");
    expect(localStorage.setItem).not.toHaveBeenCalled();
  });
  it("保存中の変更も直列に保存し、古い書込みで戻さない", async () => {
    const mock = api(); let resolve!: (value: unknown) => void;
    mock.saveSignals.mockImplementationOnce(() => new Promise(r => { resolve = r; }));
    persist = createSignalsPersistence(mock); await persist.start("live");
    change("A"); await tick(); change("B"); await tick();
    expect(mock.saveSignals).toHaveBeenCalledTimes(1);
    resolve({ ok: true, path: "signals.yaml" }); await tick();
    expect(mock.saveSignals).toHaveBeenCalledTimes(2);
    expect(mock.saveSignals.mock.calls[1][0]).toContain("unit: B");
  });
  it("停止後に読み込みが完了しても適用・保存しない", async () => {
    const mock = api(); let resolve!: (value: unknown) => void;
    mock.getSignals.mockImplementationOnce(() => new Promise(r => { resolve = r; }));
    persist = createSignalsPersistence(mock); const starting = persist.start("live"); persist.stop();
    resolve({ exists: true, text: config, path: "signals.yaml", error: "" }); await starting;
    expect(store.getState().signalDefs.base).toEqual({});
    change("A"); await tick(); expect(mock.saveSignals).not.toHaveBeenCalled();
  });
});

describe("ビューアの保存", () => {
  it("localStorage から定義と仕向を読み、編集後も保存する", async () => {
    useLiveStore.setState({ mode: "offline" });
    local.set("sim-gui:signals:v1", config); local.set("sim-gui:signals-variant:v1", "EU");
    const mock = api(); persist = createSignalsPersistence(mock); await persist.start("offline");
    expect(store.getState().selectedVariant).toBe("EU");
    expect(store.getState().unitMapping.X).toBe("A");
    change("mA"); await tick();
    expect(local.get("sim-gui:signals:v1")).toContain("unit: mA");
    store.getState().setSelectedVariant("");
    expect(local.get("sim-gui:signals-variant:v1")).toBe("");
    expect(mock.getSignals).not.toHaveBeenCalled(); expect(mock.saveSignals).not.toHaveBeenCalled();
  });
  it("localStorage の読み書きが禁止でも落ちない", async () => {
    useLiveStore.setState({ mode: "offline" });
    vi.stubGlobal("localStorage", { getItem: () => { throw new Error("denied"); }, setItem: () => { throw new Error("denied"); } });
    persist = createSignalsPersistence(api()); await persist.start("offline");
    change("A"); await tick(); store.getState().setSelectedVariant("EU");
    expect(useSignalsPersistStore.getState().error).toBe("");
  });
  it("localStorage 自体の取得が禁止でも落ちない", async () => {
    useLiveStore.setState({ mode: "offline" });
    Object.defineProperty(globalThis, "localStorage", { configurable: true, get: () => { throw new Error("denied"); } });
    persist = createSignalsPersistence(api()); await persist.start("offline");
    change("A"); await tick();
    expect(useSignalsPersistStore.getState().error).toBe("");
  });
});
