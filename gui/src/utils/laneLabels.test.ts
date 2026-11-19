import { describe, expect, it } from "vitest";
import { signalTitle, getTextWidth, leftMargin, rightMargin, laneSplitNumber, laneLabelTop, signalLineType } from "./laneLabels";
import { buildChartOptions } from "./chartOptionsBuilder";

describe("レーン共通の表示計算", () => {
  const settings = { showAlias: true, unitMapping: { "file/g_counter": "rpm" }, modeMapping: {}, valueDisplayMode: "physical" as const };
  it("短縮名・別名・単位・rawを同じ規則で組み立てる", () => {
    expect(signalTitle("file/g_counter", [], {}, settings)).toBe("g_counter [rpm]");
    expect(signalTitle("file/g_counter", [], { alias: "回転数" }, settings)).toBe("回転数 [rpm]");
    expect(signalTitle("file/g_counter", [], {}, { ...settings, valueDisplayMode: "raw" })).toBe("g_counter [raw]");
    expect(signalTitle("file/g_counter", [], {}, { ...settings, valueDisplayMode: "raw", modeMapping: { "file/g_counter": { 0: "停止" } } })).toBe("g_counter");
    expect(signalTitle("file/g_counter", ["file/g_counter", "other/g_counter"], {}, settings)).toBe("file/g_counter [rpm]");
    expect(signalTitle("a", [], {}, settings)).toBe("a");
  });
  it("ASCIIと日本語の幅を見積もる", () => {
    expect(getTextWidth("abc 状態")).toBe(54);
    expect(getTextWidth("a状", 8)).toBeCloseTo(16.8);
  });
  it("左右の余白に名前・軸値・右軸の本数を含める", () => {
    expect(leftMargin(true, 90, 5)).toBe(135);
    expect(leftMargin(false, 90, 5)).toBe(50);
    expect(leftMargin(false, 90, 9)).toBe(63);
    expect(rightMargin(0)).toBe(15);
    expect(rightMargin(3)).toBe(120);
    expect(laneLabelTop(10, 100, 2, 0)).toBe(44);
  });
  it.each([[29, 1], [30, 2], [59, 2], [60, 3], [99, 3], [100, 5]])("高さ%dのsplitNumberは%d", (height, expected) => {
    expect(laneSplitNumber(height)).toBe(expected);
  });
  it("期待値の線種は明示指定を優先する", () => {
    expect(signalLineType("EXP")).toBe("dashed");
    expect(signalLineType("期待値")).toBe("dashed");
    expect(signalLineType("EXP", "solid")).toBe("solid");
    expect(signalLineType("actual")).toBe("solid");
  });
});

describe("停止時の左余白", () => {
  it.each([true, false])("長い名前と5桁の軸値を重ねない（名前表示=%s）", showNames => {
    const sig = "g_counter";
    const option = buildChartOptions({ steps: [1, 2], seriesMap: { [sig]: [10000, 10500] } }, 600,
      [sig], [], [sig], { layout: "lane", syncRange: false },
      { [sig]: { color: "#f00", weight: 1, yAuto: false, yMin: 10000, yMax: 10500, yBreak: false } },
      {}, {}, {}, {}, { [sig]: "rpm" }, 1, "uniform", showNames, false, "off", false, "off", [], { start: 0, end: 100 }, "light");
    expect(option.grid[0].left).toBe(leftMargin(showNames, showNames ? getTextWidth("g_counter [rpm]") : 0, 5));
    expect(option.yAxis[0].min).toBe(10000);
    expect(option.yAxis[0].max).toBe(10500);
  });
});
