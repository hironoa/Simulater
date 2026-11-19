import { beforeEach, describe, expect, it, vi } from "vitest";
import yaml from "js-yaml";
import toast from "react-hot-toast";
import { useWaveformStore as store } from "./useWaveformStore";
import { useLiveStore } from "../live/liveStore";
import { useVariantsStore } from "../live/variantsStore";
import { emptyDefs, emptyView } from "./signalDefs";
import { LiveRecord } from "../live/liveRecord";

const initial = store.getState();
const csv = (names: string[]) => `#COMMENT,${names.join(",")}\n1,${names.map(() => 1).join(",")}\n`;
const config = `version: 2
variants: [国内, EU]
signals:
  SPEED: {lsb: 0.01, offset: 0, unit: km/h, undefined: [65535]}
  MODE:
    states: {0: 停止, 1: 走行}
    variants:
      EU: {states: {2: 回生, 1: null}, unit: "-"}
  OTHER: {lsb: 2, states: {1: 別}}
view:
  layoutMode: manual
  groups:
    車速: {expanded: false, visible: true, layout: lane, syncRange: false}
    空: {expanded: false, visible: false, layout: overlap, syncRange: true}
  signals:
    MODE: {group: 車速, color: '#00f', weight: 2}
    SPEED: {color: '#f00', checked: false}
    OTHER: {group: 空, alias: 別信号, yMin: -1, yMax: 5}
inactive: [{signal: MODE, when: [0], targets: [SPEED]}]
`;
const state = () => store.getState();
const read = (text = config) => state().parseMappingData(text, "yaml", false);
const output = () => yaml.load(state().getConfigYAML()) as any;

beforeEach(() => {
  vi.stubGlobal("localStorage", { getItem: () => null, setItem: vi.fn() });
  store.setState({ ...initial, signalDefs: emptyDefs(), signalView: emptyView() });
  useLiveStore.setState({ mode: "offline" });
});

describe("信号情報 v2", () => {
  it("v2 の意味・見た目・inactive を往復する", () => {
    state().parseWaveformCSV(csv(["SPEED", "MODE"]), "a.csv");
    read();
    const first = output();
    read(state().getConfigYAML());
    expect(output()).toEqual(first);
    expect(first.signals.SPEED).toEqual({ lsb: 0.01, offset: 0, unit: "km/h", undefined: [65535] });
    expect(state().getConfigYAML()).toContain("0: 停止");
    expect(state().getConfigYAML()).not.toContain("'0':");
  });
  it("v1 を base にして v2 で保存する", () => {
    read("X:\n  lsb: 2^-2\n  offset: 0\n  unit: V\n  1: ON\n  color: '#f00'\n");
    expect(output().version).toBe(2);
    expect(output().signals.X).toEqual({ lsb: "2^-2", offset: 0, unit: "V", states: { 1: "ON" } });
    expect(output().view.signals.X.color).toBe("#f00");
    expect(state().signalDefs.variants).toEqual({});
  });
  it("CSV 形式の対応表も base に読み込む", () => {
    state().parseMappingData("X,lsb,2\nX,1,ON\n", "csv");
    expect(output().signals.X).toEqual({ lsb: 2, states: { 1: "ON" } });
  });
  it("EU の states の追加・削除と unit の置換を反映し共通に戻す", () => {
    read();
    state().setSelectedVariant("EU");
    expect(state().modeMapping.MODE).toEqual({ 0: "停止", 2: "回生" });
    expect(state().unitMapping.MODE).toBe("-");
    state().setSelectedVariant("");
    expect(state().modeMapping.MODE).toEqual({ 0: "停止", 1: "走行" });
    expect(state().unitMapping.MODE).toBeUndefined();
  });
  it("上書きがある信号は差分へ、ない信号は base へ書き戻す", () => {
    read();
    state().setSelectedVariant("EU");
    state().setSignalConversion("MODE", { states: { 0: "停止", 3: "EU" }, unit: "EU" });
    state().setSignalConversion("SPEED", { unit: "m/s" });
    expect(state().signalDefs.variants.EU.MODE).toEqual({ states: { 1: null, 3: "EU" }, unit: "EU" });
    expect(state().signalDefs.base.SPEED.unit).toBe("m/s");
    state().setSelectedVariant("国内");
    expect(state().modeMapping.MODE).toEqual({ 0: "停止", 1: "走行" });
    state().setSelectedVariant("EU");
    expect(state().modeMapping.MODE).toEqual({ 0: "停止", 3: "EU" });
    expect(state().unitMapping.SPEED).toBe("m/s");
  });
  it("base と同じ差分は消し、空の上書きエントリは残す", () => {
    read(); state().setSelectedVariant("EU");
    state().setSignalConversion("MODE", { states: { 0: "停止", 1: "走行" }, unit: "" });
    expect(state().signalDefs.variants.EU.MODE).toEqual({});
    read(state().getConfigYAML());
    expect(state().signalDefs.variants.EU.MODE).toEqual({});
  });
  it("上書きオンは実効値を維持し、オフは base に戻す", () => {
    read(); state().setSelectedVariant("EU");
    state().setVariantOverride("SPEED", true);
    expect(state().lsbMapping.SPEED).toBe("0.01");
    state().setSignalConversion("SPEED", { lsb: "3" });
    expect(state().signalDefs.variants.EU.SPEED).toEqual({ lsb: 3 });
    state().setVariantOverride("SPEED", false);
    expect(state().lsbMapping.SPEED).toBe("0.01");
    expect(state().signalDefs.variants.EU.SPEED).toBeUndefined();
  });
  it("スカラー・除外値を空にした上書きも切替で失わない", () => {
    read(); state().setSelectedVariant("EU"); state().setVariantOverride("SPEED", true);
    state().setSignalConversion("SPEED", { lsb: "", unit: "", undefinedValues: [] });
    state().setSelectedVariant("");
    expect(state().undefinedMapping.SPEED).toEqual([65535]);
    state().setSelectedVariant("EU");
    expect(state().lsbMapping.SPEED).toBeUndefined();
    expect(state().undefinedMapping.SPEED).toBeUndefined();
  });
  it.each(["YAML", "CSV"])("共通の定義がない仕向だけの単位を %s の再読込で共通へコピーしない", (source) => {
    state().parseWaveformCSV(csv(["X"]), "first.csv");
    state().setSelectedVariant("EU");
    state().setVariantOverride("X", true);
    state().setSignalConversion("X", { unit: "A" });
    expect(state().signalDefs.base.X).toBeUndefined();
    expect(output().signals.X).toEqual({ variants: { EU: { unit: "A" } } });

    if (source === "YAML") read(state().getConfigYAML());
    else state().parseWaveformCSV(csv(["X"]), "next.csv");

    expect(state().unitMapping.X).toBe("A");
    expect(state().signalDefs.base.X).toEqual({});
    state().setSelectedVariant("");
    expect(state().unitMapping.X).toBeUndefined();
    expect(output().signals.X).toEqual({ variants: { EU: { unit: "A" } } });
    state().setSelectedVariant("EU");
    expect(state().unitMapping.X).toBe("A");
  });
  it("一覧にない意味・見た目・空グループを残し、現在の一覧順を先にする", () => {
    state().parseWaveformCSV(csv(["SPEED"]), "one.csv"); read();
    const doc = output();
    expect(Object.keys(doc.view.signals)).toEqual(["SPEED", "MODE", "OTHER"]);
    expect(doc.signals.OTHER.states).toEqual({ 1: "別" });
    expect(doc.view.signals.OTHER).toEqual({ group: "空", alias: "別信号", yMin: -1, yMax: 5 });
    expect(doc.view.groups.空).toEqual({ expanded: false, visible: false, layout: "overlap", syncRange: true });
    expect(doc.signals.MODE.variants.EU.states[1]).toBeNull();
  });
  it("別の CSV にグループ・並び・見た目・意味を再適用する", () => {
    state().parseWaveformCSV(csv(["SPEED"]), "one.csv"); read();
    state().updateSignalSetting("SPEED", { alias: "編集後" });
    state().parseWaveformCSV(csv(["OTHER", "SPEED", "MODE"]), "next.csv");
    expect(state().signalGroups.map(g => [g.name, g.signals])).toEqual([["車速", ["MODE"]], ["空", ["OTHER"]]]);
    expect(state().unassignedSignals).toEqual(["SPEED"]);
    expect(state().signalSettings.SPEED.alias).toBe("編集後");
    expect(state().signalSettings.OTHER.alias).toBe("別信号");
    expect(state().lsbMapping.OTHER).toBe("2");
  });
  it("ライブで後から現れた信号にもファイルのグループを適用する", () => {
    read();
    state().ensureLiveSignals(["MODE", "SPEED"], new Set(["MODE"]));
    expect(state().signalGroups[0].signals).toEqual(["MODE"]);
    expect(state().signalSettings.SPEED.checked).toBe(false);
  });
  it("停止済み記録があっても追加したライブ信号の一覧・グループを保持する", () => {
    const record = new LiveRecord();
    record.append(1, ["X"], [[1]]);
    state().commitLiveRecord({ record });
    read();
    state().ensureLiveSignals(["X", "MODE", "SPEED"], new Set(["X", "MODE"]));
    expect(state().signalGroups.find(g => g.name === "車速")?.signals).toEqual(["MODE"]);
    expect(state().unassignedSignals).toEqual(["X", "SPEED"]);
    expect(state().signalSettings.MODE.color).toBe("#00f");
    expect(state().signalSettings.SPEED.checked).toBe(false);
    record.append(2, ["X", "MODE", "SPEED"], [[2, 1, 10]]);
    state().commitLiveRecord({ record });
    expect(state().signalGroups.find(g => g.name === "車速")?.signals).toEqual(["MODE"]);
    expect(state().unassignedSignals).toEqual(["X", "SPEED"]);
    expect(Object.keys(state().chartData!.seriesMap)).toEqual(["X", "MODE", "SPEED"]);
  });
  it("別の CSV を開くと前の CSV だけにある信号は一覧に残さない", () => {
    state().parseWaveformCSV(csv(["MODE", "SPEED", "X"]), "one.csv"); read();
    state().parseWaveformCSV(csv(["OTHER", "Y"]), "next.csv");
    expect(state().signalGroups.flatMap(g => g.signals)).toEqual(["OTHER"]);
    expect(state().unassignedSignals).toEqual(["Y"]);
    expect(state().signalView.signals.MODE.group).toBe("車速");
  });
  it("手で読む YAML は意味を置き換え、未指定の見た目を保持する", () => {
    state().parseWaveformCSV(csv(["SPEED"]), "a.csv"); read();
    read("version: 2\nsignals: {SPEED: {unit: V}}\nview: {signals: {SPEED: {}}}\n");
    expect(state().signalDefs.base.MODE).toBeUndefined();
    expect(state().lsbMapping.SPEED).toBeUndefined();
    expect(state().signalSettings.SPEED.checked).toBe(false);
    expect(state().layoutMode).toBe("uniform");
    expect(state().signalGroups).toEqual([]);
  });
  it(".wcp 用 viewState.variant を往復する（v1 viewState も維持）", () => {
    read(); state().setSelectedVariant("EU");
    const text = state().getConfigYAML(true);
    expect((yaml.load(text) as any).viewState.variant).toBe("EU");
    state().setSelectedVariant(""); read(text);
    expect(state().selectedVariant).toBe("EU");
    expect(state().modeMapping.MODE[2]).toBe("回生");
    read("viewState: {showAlias: true}\nX: {1: ON}\n");
    expect(state().showAlias).toBe(true);
  });
  it.each(["live", "detecting"] as const)("%s は viewState.variant より右パネルを優先し名前を合わせて保存", (mode) => {
    useLiveStore.setState({ mode });
    useVariantsStore.setState({ doc: { version: 1, selected: "EU", variants: [{ name: "北米", values: [] }] } });
    read(config + "viewState: {variant: 国内}\n");
    expect(state().selectedVariant).toBe("EU");
    expect(output().variants).toEqual(["国内", "EU", "北米"]);
    expect(localStorage.setItem).not.toHaveBeenCalled();
    state().setSelectedVariant("国内");
    expect(localStorage.setItem).not.toHaveBeenCalled();
  });
  it.each(["broken: [", "version: 2\nsignals: {MODE: {states: [oops]}}\n"])("不正 YAML は通知して状態を変更しない: %s", (text) => {
    read(); const before = state();
    const notify = vi.spyOn(toast, "error").mockImplementation(() => "test");
    try {
      expect(() => read(text)).not.toThrow();
      expect(state()).toBe(before);
      expect(notify).toHaveBeenCalledWith(expect.stringMatching(/^YAML を読めません: .+/));
    } finally { notify.mockRestore(); }
  });
  it("CSV の読み込みとライブ信号の追加で表示状態を維持する", () => {
    read();
    store.setState({ showMode: "bg-only", theme: "dark", showAlias: true, showTable: true });
    state().parseWaveformCSV(csv(["SPEED"]), "a.csv");
    expect(state()).toMatchObject({ showMode: "bg-only", theme: "dark", showAlias: true, showTable: true });
    state().ensureLiveSignals(["MODE", "SPEED"], new Set(["MODE"]));
    expect(state()).toMatchObject({ showMode: "bg-only", theme: "dark", showAlias: true, showTable: true });
  });
  it("内部再適用では viewState のテーマ・仕向・表示状態も適用しない", () => {
    read();
    store.setState({ showMode: "bg-only", theme: "dark", selectedVariant: "EU", showAlias: true });
    state().parseMappingData(config + "viewState: {showMode: full, theme: light, variant: 国内, showAlias: false}\n", "yaml", false, { reapply: true });
    expect(state()).toMatchObject({ showMode: "bg-only", theme: "dark", selectedVariant: "EU", showAlias: true });
  });
  it("YAML にない信号の色・太さ・高さを保持する", () => {
    state().parseWaveformCSV(csv(["X", "Y"]), "a.csv");
    state().updateSignalSetting("X", { color: "#123456", lineWidth: 4, weight: 3, manualWeight: 3, checked: false });
    const before = { ...state().signalSettings.X };
    read("Y: {unit: V}\n");
    expect(state().signalSettings.X).toEqual(before);
  });
  it("現在のグループを先にし、一覧にないグループは読み込み順・設定を保存する", () => {
    state().parseWaveformCSV(csv(["SPEED"]), "a.csv"); read();
    store.setState({ signalGroups: [{ id: "g", name: "現在", signals: [], expanded: true, visible: true, layout: "lane", syncRange: false }] });
    const groups = output().view.groups;
    expect(Object.keys(groups)).toEqual(["現在", "車速", "空"]);
    expect(groups.空).toEqual({ expanded: false, visible: false, layout: "overlap", syncRange: true });
    expect(groups.車速).toEqual({ expanded: false, visible: true, layout: "lane", syncRange: false });
  });
  it("グループを改名すると保持設定と一覧外の信号も改名する", () => {
    state().parseWaveformCSV(csv(["MODE"]), "one.csv"); read();
    const group = state().signalGroups.find(g => g.name === "車速")!;
    store.setState({ signalView: { ...state().signalView, signals: {
      ...state().signalView.signals, HIDDEN: { group: "車速", alias: "保持" },
    } } });
    state().toggleGroupExpand(group.id);
    state().renameGroup(group.id, "new");
    expect(output().view.groups).not.toHaveProperty("車速");
    expect(output().view.groups.new.expanded).toBe(true);
    expect(state().signalView.groups.new.expanded).toBe(true);
    expect(output().view.signals.MODE.group).toBe("new");
    expect(output().view.signals.HIDDEN).toEqual({ group: "new", alias: "保持" });
    expect(output().view.groups).toHaveProperty("空");
  });
  it("グループを削除すると保持設定と一覧外の信号の所属も削除する", () => {
    state().parseWaveformCSV(csv(["MODE"]), "one.csv"); read();
    store.setState({ signalView: { ...state().signalView, signals: {
      ...state().signalView.signals, HIDDEN: { group: "車速", alias: "保持" },
    } } });
    state().setGroups(state().signalGroups.filter(g => g.name !== "車速"), ["MODE"]);
    expect(output().view.groups).not.toHaveProperty("車速");
    expect(output().view.signals.MODE).not.toHaveProperty("group");
    expect(output().view.signals.HIDDEN).toEqual({ alias: "保持" });
    expect(output().view.groups).toHaveProperty("空");
  });
  it("監視だけの信号は意味に書かず、見た目と空の仕向上書きは残す", () => {
    state().ensureLiveSignals(["X", "Y"], new Set(["X", "Y"]));
    expect(output().signals).toEqual({});
    expect(Object.keys(output().view.signals)).toEqual(["X", "Y"]);
    state().setSelectedVariant("EU"); state().setVariantOverride("X", true);
    expect(output().signals).toEqual({ X: { variants: { EU: {} } } });
    read(state().getConfigYAML());
    expect(output().signals).toEqual({ X: { variants: { EU: {} } } });
  });
});
