import { beforeEach, describe, expect, it, vi } from "vitest";
import { listedSignals } from "../utils/displaySignalName";
import { useWaveformStore } from "./useWaveformStore";

const initial = useWaveformStore.getState();
const setting = { id: "s", color: "#f00", checked: true, weight: 1, expanded: false, yAuto: true, yMin: null, yMax: null };

beforeEach(() => {
  // parseMappingData はテーマを DOM に当てる。node の試験では classList だけあればよい
  vi.stubGlobal("document", { documentElement: { classList: { add() {}, remove() {} } } });
  useWaveformStore.setState({ ...initial, chartData: null, signalSettings: {}, signalGroups: [], unassignedSignals: [] });
});

describe("listedSignals", () => {
  it("グループ → 未分類の順で、重複を除く", () => {
    expect(listedSignals({ signalGroups: [{ signals: ["b", "a"] }], unassignedSignals: ["c", "a"] })).toEqual(["b", "a", "c"]);
  });
});

describe("yaml の読み込み", () => {
  it("viewState を信号の設定にしない", () => {
    useWaveformStore.getState().parseMappingData("viewState:\n  showAlias: true\nx:\n  color: '#00f'\n", "yaml");
    const s = useWaveformStore.getState();
    expect(s.signalSettings.viewState).toBeUndefined();
    expect(s.signalSettings.x.color).toBe("#00f");
    expect(s.showAlias).toBe(true);
  });

  it("ライブの実行中（データなし）でも、左に並んでいる信号を yaml の並び・グループに入れる", () => {
    useWaveformStore.setState({ signalSettings: { a: { ...setting }, b: { ...setting }, c: { ...setting } }, unassignedSignals: ["a", "b", "c"] });
    useWaveformStore.getState().parseMappingData("c:\n  group: G\nb: {}\nold.c/a: {}\n", "yaml");
    const s = useWaveformStore.getState();
    expect(s.signalGroups.map(g => [g.name, g.signals])).toEqual([["G", ["c"]]]);
    expect(s.unassignedSignals).toEqual(["b", "a"]);
  });
});
