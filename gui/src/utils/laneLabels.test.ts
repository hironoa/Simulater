import { describe, expect, it } from "vitest";
import { signalTitle, getTextWidth, maxTitleWidth, truncateTitle, leftMargin, rightMargin, rightAxisWidth, laneSplitNumber, laneLabelTop, signalLineType } from "./laneLabels";
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
  it("タイトルの上限は幅の3割・最低40px", () => {
    expect(maxTitleWidth(600)).toBe(180);
    expect(maxTitleWidth(100)).toBe(40);
    expect(maxTitleWidth(0)).toBe(40);
  });
  it("幅に収まる名前は維持し、長い名前は省略記号込みで収める", () => {
    expect(truncateTitle("abc", 18)).toBe("abc");
    expect(truncateTitle("a".repeat(100), 180)).toBe("a".repeat(27) + "…");
    expect(getTextWidth(truncateTitle("状態名".repeat(30), 180))).toBeLessThanOrEqual(180);
    expect(truncateTitle("状態名", Infinity)).toBe("状態名");
    expect(truncateTitle("状態名", 0)).toBe("");
  });
  it("右軸の幅と余白を桁数に応じて広げる", () => {
    expect(rightAxisWidth()).toBe(35);
    expect(rightAxisWidth(4)).toBe(35);
    expect(rightAxisWidth(10)).toBe(68);
    expect(rightMargin(0, 10)).toBe(15);
    expect(rightMargin(2, 10)).toBe(151);
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

describe("停止時の右余白", () => {
  it("10桁の右軸2本のoffsetと全gridのrightを広げる", () => {
    const names = ["a", "b", "c"];
    const ranges = [[0, 2], [1000000000, 2000000000], [1000000000, 2000000000]];
    const option = buildChartOptions({ steps: [1, 2], seriesMap: Object.fromEntries(names.map((sig, i) => [sig, ranges[i]])) }, 600,
      names, [{ name: "G", signals: names, layout: "overlap", syncRange: false }], [], { layout: "lane", syncRange: false },
      Object.fromEntries(names.map((sig, i) => [sig, { color: "#f00", weight: 1, yAuto: false, yMin: ranges[i][0], yMax: ranges[i][1], yBreak: false }])),
      {}, {}, {}, {}, {}, 1, "uniform", false, false, "off", false, "off", [], { start: 0, end: 100 }, "light");
    expect(option.yAxis.slice(0, 3).map(axis => axis.offset)).toEqual([0, 0, rightAxisWidth(10)]);
    expect(option.grid.every(grid => grid.right === rightMargin(2, 10))).toBe(true);
    expect(option.grid[0].left).toBe(leftMargin(false, 0, 1));
  });
});

describe("停止時の長いタイトル", () => {
  it.each([undefined, "aOnly", "bOnly"] as const)("幅の上限と省略を適用し、比較の2行目を保つ（%s）", presence => {
    const alias = "a".repeat(100);
    const option = buildChartOptions({ steps: [1, 2], seriesMap: { a: [0, 2] } }, 600,
      ["a"], [], ["a"], { layout: "lane", syncRange: false },
      { a: { alias, color: "#f00", weight: 1, yAuto: false, yMin: 0, yMax: 2, yBreak: false } },
      {}, {}, {}, {}, {}, 1, "uniform", true, true, "off", false, "off", [], { start: 0, end: 100 }, "light", "physical", undefined,
      presence ? { data: { steps: [1, 2], seriesMap: { a: [0, 2] } }, summary: { a: { presence, valueCount: 0, timingCount: 0 } }, diffs: [], diffFilter: "all", focusedDiffIndex: -1 } : undefined, 600);
    expect(option.grid[0].left).toBe(leftMargin(true, 180, 1));
    expect(option.title[0].textStyle).toMatchObject({ width: 180, overflow: "truncate", ellipsis: "…" });
    const lines = String(option.title[0].text).split("\n");
    expect(lines[0]).toBe(truncateTitle(alias, 180));
    expect(lines.slice(1)).toEqual(presence ? [presence === "aOnly" ? "A のみ" : "B のみ"] : []);
  });
  it("幅が未取得の場合は上限をかけない", () => {
    const alias = "a".repeat(100);
    const option = buildChartOptions({ steps: [1, 2], seriesMap: { a: [0, 2] } }, 600,
      ["a"], [], ["a"], { layout: "lane", syncRange: false },
      { a: { alias, color: "#f00", weight: 1, yAuto: false, yMin: 0, yMax: 2, yBreak: false } },
      {}, {}, {}, {}, {}, 1, "uniform", true, true, "off", false, "off", [], { start: 0, end: 100 }, "light");
    expect(option.grid[0].left).toBe(leftMargin(true, 600, 1));
    expect(option.title[0].text).toBe(alias);
    expect(option.title[0].textStyle).not.toHaveProperty("width");
  });
});
