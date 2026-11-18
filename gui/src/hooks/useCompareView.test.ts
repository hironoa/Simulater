import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { liveRecord } from "../live/liveRecord";
import { useWaveformStore } from "../store/useWaveformStore";
import { useCompareView } from "./useCompareView";

const initial = useWaveformStore.getInitialState();
const original = { ...initial };

function readView() {
  let view: ReturnType<typeof useCompareView>;
  function Probe() {
    view = useCompareView();
    return null;
  }
  renderToStaticMarkup(createElement(Probe));
  return view!;
}

beforeEach(() => {
  liveRecord.clear();
  // サーバ描画は Zustand の初期スナップショットを読む。
  Object.assign(initial, {
    chartData: null, compareChartData: null, compareEnabled: false,
    signalGroups: [{ id: "g", name: "G", expanded: true, visible: true,
      layout: "lane", syncRange: true, signals: ["grouped", "missing-grouped"] }],
    unassignedSignals: ["loose", "missing-loose"],
  });
});
afterEach(() => {
  Object.assign(initial, original);
  liveRecord.clear();
});

describe("useCompareView", () => {
  it("chartData が null でも記録の列を残し、どちらにもない信号は除く", () => {
    liveRecord.ensureColumn("grouped");
    liveRecord.ensureColumn("loose");
    const view = readView();
    expect(view.groups[0].signals).toEqual(["grouped"]);
    expect(view.unassigned).toEqual(["loose"]);
    expect(view.signals).toEqual(["grouped", "loose"]);
    expect(initial.signalGroups[0].signals).toEqual(["grouped", "missing-grouped"]);
  });

  it("記録の列が増えたあとの描画で一覧に反映する", () => {
    expect(readView().signals).toEqual([]);
    liveRecord.ensureColumn("loose");
    expect(readView().unassigned).toEqual(["loose"]);
    expect(readView().signals).toEqual(["loose"]);
  });

  it("CSV と停止後の chartData の信号を引き続き残す", () => {
    initial.chartData = { steps: [1], seriesMap: { grouped: [1], loose: [2] } };
    expect(readView().signals).toEqual(["grouped", "loose"]);
    liveRecord.ensureColumn("grouped");
    liveRecord.ensureColumn("loose");
    expect(readView().signals).toEqual(["grouped", "loose"]);
  });

  it("比較表示では記録の有無でグループと未分類を絞り込まない", () => {
    initial.compareEnabled = true;
    initial.compareChartData = { steps: [1], seriesMap: { aOnly: [3] } };
    liveRecord.ensureColumn("grouped");
    const view = readView();
    expect(view.groups[0].signals).toEqual(["grouped", "missing-grouped"]);
    expect(view.unassigned).toEqual(["loose", "missing-loose", "aOnly"]);
    expect(view.signals).toEqual(["grouped", "missing-grouped", "loose", "missing-loose", "aOnly"]);
  });
});
