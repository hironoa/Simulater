import { describe, expect, it } from "vitest";
import { buildChartOptions } from "./chartOptionsBuilder";
import type { ChartData, SignalGroup, SignalSetting } from "../types";

const baseSetting = (id: string, color = "#3b82f6"): SignalSetting => ({
  id,
  color,
  weight: 1,
  expanded: false,
  yAuto: true,
  yMin: null,
  yMax: null,
  checked: true,
  yBreak: true,
  yBreakStart: null,
  yBreakEnd: null,
});

describe("buildChartOptions", () => {
  it("ignores active signals not yet present in chartData.seriesMap without throwing", () => {
    const chartData: ChartData = {
      steps: [1, 2, 3],
      seriesMap: {
        g_gain: [100, 100, 100],
      },
    };
    const groups: SignalGroup[] = [
      {
        id: "g1",
        name: "制御",
        expanded: true,
        visible: true,
        layout: "lane",
        syncRange: false,
        signals: ["g_gain", "g_temp"],
      },
    ];
    const settings: Record<string, SignalSetting> = {
      g_gain: baseSetting("sig_0", "#ef4444"),
      g_temp: baseSetting("sig_1", "#10b981"),
      g_sine: baseSetting("sig_2", "#3b82f6"),
    };

    const option = buildChartOptions(
      chartData,
      400,
      ["g_gain", "g_temp", "g_sine"],
      groups,
      ["g_sine"],
      { layout: "lane", syncRange: false },
      settings,
      {},
      {},
      {},
      {},
      {},
      5,
      "uniform",
      true,
      false,
      "full",
      true,
      "full",
      [],
      { start: 0, end: 100 },
      "light",
    );

    const seriesNames = (option.series as { name?: string }[]).map((s) => s.name);
    expect(seriesNames).toContain("g_gain");
    expect(seriesNames).not.toContain("g_temp");
    expect(seriesNames).not.toContain("g_sine");
  });

  it("handles when none of activeSignals exist in chartData.seriesMap yet", () => {
    const chartData: ChartData = {
      steps: [1, 2],
      seriesMap: { g_gain: [10, 20] },
    };
    const settings: Record<string, SignalSetting> = {
      g_temp: baseSetting("sig_1", "#10b981"),
    };

    const option = buildChartOptions(
      chartData,
      400,
      ["g_temp"],
      [],
      ["g_temp"],
      { layout: "lane", syncRange: false },
      settings,
      {},
      {},
      {},
      {},
      {},
      5,
      "uniform",
      true,
      false,
      "full",
      true,
      "full",
      [],
      { start: 0, end: 100 },
      "light",
    );

    expect((option.series as { name?: string }[]).map((s) => s.name)).toEqual(["dummy_for_pointer"]);
  });
});
