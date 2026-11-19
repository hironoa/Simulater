import { beforeEach, describe, expect, it, vi } from "vitest";
import type { SignalGroup, SignalSetting } from "../types";
import { useWaveformStore } from "../store/useWaveformStore";
import { initialLiveState, useLiveStore } from "./liveStore";
import { createLiveController } from "./controller";
import { LiveRecord } from "./liveRecord";
import { buildLiveLanes } from "./liveLayout";
import { liveLaneRange } from "./liveCanvas";
import { activeDisplaySignals, buildDisplayBlocks, layoutBlocks } from "../utils/displayBlocks";

const setting = (checked = true, weight = 1): SignalSetting => ({
  id: "sig", color: "#f00", checked, weight, expanded: false, yAuto: true, yMin: null, yMax: null,
});
const group = (signals: string[], layout: SignalGroup["layout"] = "lane"): SignalGroup => ({
  id: "g", name: "G", expanded: true, visible: true, layout, syncRange: true, signals,
});
const initialWaveform = useWaveformStore.getState();
beforeEach(() => {
  useLiveStore.setState({ ...initialLiveState, watch: [], inputs: [] });
  useWaveformStore.setState({ ...initialWaveform, signalSettings: {}, signalGroups: [], unassignedSignals: [] });
});
const setup = () => {
  useWaveformStore.setState({
    signalSettings: { a: setting(true, 2), b: setting(true, 4), c: setting(), off: setting(false), missing: setting() },
    layoutMode: "manual", signalGroups: [group(["b", "a"], "overlap")], unassignedSignals: ["off", "c", "missing"],
  });
  return useWaveformStore.getState();
};

describe("共有レイアウト", () => {
  it("グループ、重ね表示、未分類の順と同期範囲を保持する", () => {
    const groups = [group(["b", "a"], "overlap"), group(["c", "d"])];
    const blocks = buildDisplayBlocks(["a", "b", "c", "d", "e"], groups, ["e"], group([]));
    expect(blocks).toEqual([
      { type: "group_overlap", name: "G", signals: ["b", "a"], syncRange: true },
      { type: "single", sig: "c", syncGroup: ["c", "d"] },
      { type: "single", sig: "d", syncGroup: ["c", "d"] },
      { type: "single", sig: "e", syncGroup: ["e"] },
    ]);
    expect(buildDisplayBlocks(["a", "b"], [], ["b", "a"], group([], "overlap")))
      .toEqual([{ type: "group_overlap", name: "未分類", signals: ["b", "a"], syncRange: true }]);
  });
  it.each(["manual", "optimized"] as const)("%s は従来の余白・間隔と weight を使う", mode => {
    const state = setup();
    const blocks = buildDisplayBlocks(["a", "b", "c"], state.signalGroups, ["c"], state.unassignedGroupConfig);
    const positions = layoutBlocks(blocks, state.signalSettings, mode, 700);
    // top=35px=5%, bottom=8%, gap=2%, weights=(2+4)/2:1
    expect(positions).toEqual([{ top: 5, height: 63.75 }, { top: 70.75, height: 21.25 }]);
  });
  it("uniform は weight にかかわらず等分する", () => {
    const state = setup();
    const blocks = buildDisplayBlocks(["a", "b", "c"], state.signalGroups, ["c"], state.unassignedGroupConfig);
    expect(layoutBlocks(blocks, state.signalSettings, "uniform", 700))
      .toEqual([{ top: 5, height: 42.5 }, { top: 49.5, height: 42.5 }]);
    expect(layoutBlocks([], {}, "uniform", 600)).toEqual([]);
  });
  it("チェックと通常表示と同じ表示フィルターを適用する", () => {
    expect(activeDisplaySignals(["a", "b", "c"], { a: setting(), b: setting(false), c: setting() }, s => s !== "c"))
      .toEqual(["a"]);
  });
});

describe("ライブのレーン", () => {
  it.each(["uniform", "manual", "optimized"] as const)("%s の top と height は停止時の既定レイアウトと一致する", layoutMode => {
    const state = { ...setup(), layoutMode };
    const names = ["a", "b", "c"];
    const blocks = buildDisplayBlocks(names, state.signalGroups, ["c"], state.unassignedGroupConfig);
    const expected = layoutBlocks(blocks, state.signalSettings, layoutMode, 700);
    const lanes = buildLiveLanes(state, names, 700);
    expect(lanes.map(l => ({ top: l.top, height: l.height })))
      .toEqual(expected.map(l => ({ top: l.top * 7, height: l.height * 7 })));
  });
  it("高さ200pxでは最下段の下に横軸用の30pxを残す", () => {
    const lanes = buildLiveLanes(setup(), ["a", "b", "c"], 200);
    const last = lanes[lanes.length - 1];
    expect(last.top + last.height + 30).toBeLessThanOrEqual(200);
    expect(last.top + last.height).toBeCloseTo(170);
  });
  it("高さ600pxでは既定の8%の配置を維持する", () => {
    const state = setup();
    const names = ["a", "b", "c"];
    const blocks = buildDisplayBlocks(names, state.signalGroups, ["c"], state.unassignedGroupConfig);
    const expected = layoutBlocks(blocks, state.signalSettings, state.layoutMode, 600);
    const lanes = buildLiveLanes(state, names, 600);
    lanes.forEach((lane, i) => {
      expect(lane.top).toBeCloseTo(expected[i].top * 6);
      expect(lane.height).toBeCloseTo(expected[i].height * 6);
    });
    const last = lanes[lanes.length - 1];
    expect(last.top + last.height).toBeCloseTo(552);
  });
  it.each([0, 1, 20])("高さ%dpxでもレーン高が負にならない", height => {
    const lanes = buildLiveLanes(setup(), ["a", "b", "c"], height);
    expect(lanes.length).toBeGreaterThan(0);
    expect(lanes.every(lane => Number.isFinite(lane.height) && lane.height >= 0)).toBe(true);
  });
  it("単位と別名を付け、raw と名前非表示も停止時と揃える", () => {
    const state = { ...setup(), showSignalNames: true, showAlias: true, unitMapping: { a: "rpm" }, modeMapping: {} };
    state.signalSettings.a = { ...state.signalSettings.a, alias: "回転数" };
    expect(buildLiveLanes(state, ["a"], 600)[0].series[0].label).toBe("回転数 [rpm]");
    expect(buildLiveLanes({ ...state, valueDisplayMode: "raw" }, ["a"], 600)[0].series[0].label).toBe("回転数 [raw]");
    expect(buildLiveLanes({ ...state, showSignalNames: false }, ["a"], 600)[0].series[0].label).toBe("");
  });
  it("系列に状態名を載せ、設定のない系列はundefinedにする", () => {
    const state = setup();
    const mapping = { 0: "停止", 1: "運転" };
    const lanes = buildLiveLanes({ ...state, modeMapping: { a: mapping } }, ["a", "b"], 600);
    expect(lanes[0].series[0].modeMapping).toBeUndefined();
    expect(lanes[0].series[1].modeMapping).toBe(mapping);
    // yaml が作る空の表は状態名なしとして扱う
    const empty = buildLiveLanes({ ...state, modeMapping: { a: mapping, b: {} } }, ["a", "b"], 600);
    expect(empty[0].series[0].modeMapping).toBeUndefined();
  });
  it("yaml の順に重ね表示し、未チェックと記録にない信号を除く", () => {
    const state = setup();
    const lanes = buildLiveLanes(state, ["c", "off", "a", "b"], 600);
    expect(lanes.map(l => l.series.map(s => s.sig))).toEqual([["b", "a"], ["c"]]);
    expect(lanes[0].height / lanes[1].height).toBeCloseTo(3);
    expect(buildLiveLanes(state, ["a", "b", "c"], 600, s => s !== "c")).toHaveLength(1);
  });
  it("40 ブロックを高さ 600px の中に収める", () => {
    const names = Array.from({ length: 40 }, (_, i) => `s${i}`);
    useWaveformStore.getState().ensureLiveSignals(names, new Set(names));
    const lanes = buildLiveLanes(useWaveformStore.getState(), names, 600);
    expect(lanes).toHaveLength(40);
    for (const lane of lanes) {
      expect(lane.top).toBeGreaterThanOrEqual(35);
      expect(lane.height).toBeGreaterThan(0);
      expect(lane.top + lane.height).toBeLessThanOrEqual(600 * 0.92 + 1e-9);
    }
    expect(lanes[39].top + lanes[39].height).toBeCloseTo(552);
  });
  it("重ね表示の範囲は窓内の全系列、固定範囲があればその和を使う", () => {
    const state = setup();
    const record = new LiveRecord();
    record.append(0, ["a", "b"], [[-9999, 9999], [1, 10], [2, 20]]);
    const lane = buildLiveLanes(state, record.names, 600)[0];
    const range = liveLaneRange(record, lane, 1, 3)!;
    expect(range.lo).toBeLessThan(1);
    expect(range.lo).toBeGreaterThan(-1);
    expect(range.hi).toBeGreaterThan(20);
    lane.series[0].yMin = 0; lane.series[0].yMax = 5;
    expect(liveLaneRange(record, lane, 1, 3)).toEqual({ lo: 0, hi: 5 });
    lane.series[1].yMin = -2; lane.series[1].yMax = 3;
    expect(liveLaneRange(record, lane, 1, 3)).toEqual({ lo: -2, hi: 5 });
  });
  it("縦軸の同期の設定（重ね表示の syncRange、並べる表示の syncGroup）をレーンに渡す", () => {
    const state = setup();
    const unsynced = { ...group(["b", "a"], "overlap"), syncRange: false };
    expect(buildLiveLanes({ ...state, signalGroups: [unsynced] }, ["a", "b"], 600)[0].syncRange).toBe(false);
    expect(buildLiveLanes(state, ["a", "b"], 600)[0].syncRange).toBe(true);
    const lanes = buildLiveLanes({ ...state, signalGroups: [group(["b", "a"])] }, ["a", "b", "c"], 600);
    expect(lanes.map(l => l.syncKey)).toEqual(["b,a", "b,a", null]);
    expect(lanes[0].rangeSeries?.map(s => s.sig)).toEqual(["b", "a"]);
    const noSync = buildLiveLanes({ ...state, signalGroups: [{ ...group(["b", "a"]), syncRange: false }] }, ["a", "b"], 600);
    expect(noSync.map(l => [l.syncKey, l.rangeSeries])).toEqual([[null, undefined], [null, undefined]]);
  });
  it("raw 表示では固定の物理範囲を系列の変換に合わせる", () => {
    const state = setup();
    state.signalSettings.a = { ...setting(), yAuto: false, yMin: -15, yMax: 5 };
    const lane = buildLiveLanes({ ...state, valueDisplayMode: "raw", lsbMapping: { a: "-2" }, offsetMapping: { a: "5" } }, ["a"], 600)[0];
    expect(lane.series[0].yMin).toBeCloseTo(0);
    expect(lane.series[0]).toMatchObject({ yMax: 10, conv: { lsb: 1, offset: 0 } });
  });
  it("記録の列構成だけを通知する（点の追記では通知しない）", () => {
    const record = new LiveRecord();
    const listener = vi.fn();
    const unsub = record.subscribeNames(listener);
    record.append(0, ["a"], [[1]]);
    const names = record.names;
    record.append(1, ["a"], [[2]]);
    expect(listener).toHaveBeenCalledTimes(1);
    expect(record.names).toBe(names);
    record.clear();
    expect(listener).toHaveBeenCalledTimes(2);
    unsub();
    record.ensureColumn("b");
    expect(listener).toHaveBeenCalledTimes(2);
  });
});

describe("チェックの所有と同期", () => {
  const controller = () => createLiveController({ api: { watch: vi.fn().mockResolvedValue({}) } as never });
  it("既存の checked と yaml の所属を保持し、新しい監視だけ plot を使う", () => {
    useWaveformStore.setState({ signalSettings: { a: setting(false), b: setting(true) }, signalGroups: [group(["a"])], unassignedSignals: ["b"] });
    useWaveformStore.getState().ensureLiveSignals(["a", "b", "c", "d"], new Set(["a", "c"]));
    const state = useWaveformStore.getState();
    expect(Object.fromEntries(Object.entries(state.signalSettings).map(([n, s]) => [n, s.checked])))
      .toEqual({ a: false, b: true, c: true, d: false });
    expect(state.signalGroups[0].signals).toEqual(["a"]);
    expect(state.unassignedSignals).toEqual(["b", "c", "d"]);
  });
  it("入力変数は既定で表示し、監視変数は plot に従う", () => {
    const live = controller();
    live.addInput("input"); live.addWatch("off", false); live.addWatch("on", true);
    const settings = useWaveformStore.getState().signalSettings;
    expect(settings.input.checked).toBe(true);
    expect(settings.off.checked).toBe(false);
    expect(settings.on.checked).toBe(true);
    live.stop();
  });
  it("モニタ・左の個別・一括・グループのチェックが双方向に揃う", () => {
    const live = controller();
    live.addWatch("a", true);
    live.updateWatch("a", { plot: false });
    expect(useWaveformStore.getState().signalSettings.a.checked).toBe(false);
    useWaveformStore.getState().toggleSignalCheck("a");
    expect(useLiveStore.getState().watch[0].plot).toBe(true);
    useWaveformStore.getState().toggleAllSignals(false);
    expect(useLiveStore.getState().watch[0].plot).toBe(false);
    live.updateWatch("a", { plot: true });
    expect(useWaveformStore.getState().signalSettings.a.checked).toBe(true);
    useWaveformStore.setState({ signalGroups: [group(["a"])] });
    useWaveformStore.getState().toggleGroupVisibility("g", false);
    expect(useLiveStore.getState().watch[0].plot).toBe(false);
    live.stop();
  });
  it("既存信号のチェックは監視追加時の plot より優先する", () => {
    useWaveformStore.setState({ signalSettings: { a: setting(false) } });
    const live = controller();
    live.addWatch("a", true);
    expect(useWaveformStore.getState().signalSettings.a.checked).toBe(false);
    expect(useLiveStore.getState().watch[0].plot).toBe(false);
    live.stop();
  });
});
