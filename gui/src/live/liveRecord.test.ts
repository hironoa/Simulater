import { describe, expect, it } from "vitest";
import { LiveRecord } from "./liveRecord";

describe("LiveRecord", () => {
  it("samples の行を列ごとに追記し、読めない値は null にする", () => {
    const r = new LiveRecord(1000, 4);
    r.append(0, ["a", "b"], [[1, 10], [2, null], [3, 30]]);
    expect(r.length).toBe(3);
    expect(r.names).toEqual(["a", "b"]);
    expect([...r.steps()]).toEqual([0, 1, 2]);
    expect(r.valueAt("b", 1)).toBeNull();
    expect(r.latest("a")).toBe(3);
    expect(r.latest("b")).toBe(30);
  });

  it("samples に同じ名前が重なっても、その列にはステップごとに 1 点だけ入る", () => {
    const r = new LiveRecord(1000, 4);
    r.append(0, ["P1", "P1"], [[7, 7], [8, 8]]);
    expect(r.names).toEqual(["P1"]);
    expect(r.toChartData()).toEqual({ seriesMap: { P1: [7, 8] }, steps: [1, 2] });
  });

  it("容量を超えたら伸ばし、途中から増えた列の過去分は null で埋める", () => {
    const r = new LiveRecord(1000, 16);
    const rows = Array.from({ length: 40 }, (_, i) => [i]);
    r.append(0, ["a"], rows);
    r.append(40, ["a", "c"], [[40, 7]]);
    expect(r.length).toBe(41);
    expect(r.valueAt("c", 0)).toBeNull();
    expect(r.valueAt("c", 40)).toBe(7);
    expect(r.valueAt("a", 39)).toBe(39);
  });

  it("監視から外れた列には NaN を入れて記録を保つ", () => {
    const r = new LiveRecord();
    r.append(0, ["a", "b"], [[1, 2]]);
    r.append(1, ["a"], [[3]]);
    expect(r.valueAt("b", 0)).toBe(2);
    expect(r.valueAt("b", 1)).toBeNull();
  });

  it("ステップが巻き戻ったら（リセット）記録を作り直す", () => {
    const r = new LiveRecord();
    r.append(100, ["a"], [[1], [2]]);
    const res = r.append(0, ["a"], [[9]]);
    expect(res.restarted).toBe(true);
    expect(r.length).toBe(1);
    expect(r.firstStep).toBe(0);
    expect(r.latest("a")).toBe(9);
  });

  it("上限を超えたら古い側から上限の 10% をまとめて捨てる", () => {
    const r = new LiveRecord(1000);
    const rows = Array.from({ length: 1001 }, (_, i) => [i]);
    const res = r.append(0, ["a"], rows);
    expect(res.dropped).toBe(101);
    expect(r.length).toBe(900);
    expect(r.firstStep).toBe(101);
    expect(r.valueAt("a", 0)).toBe(101);
    expect(r.droppedTotal).toBe(101);
  });

  it("window(n) は直近 n 点の範囲を返す", () => {
    const r = new LiveRecord();
    r.append(0, ["a"], Array.from({ length: 10 }, (_, i) => [i]));
    expect(r.window(4)).toEqual({ start: 6, end: 10 });
    expect(r.window(100)).toEqual({ start: 0, end: 10 });
  });

  it("toChartData は Viewer 向けにステップを +1 し、値を null 付き配列にする", () => {
    const r = new LiveRecord();
    r.append(5, ["a", "b"], [[1, null], [2, 3]]);
    const cd = r.toChartData();
    expect(cd.steps).toEqual([6, 7]);
    expect(cd.seriesMap).toEqual({ a: [1, 2], b: [null, 3] });
  });

  it("revision は追記と消去のたびに増える", () => {
    const r = new LiveRecord();
    const v0 = r.revision;
    r.append(0, ["a"], [[1]]);
    expect(r.revision).toBe(v0 + 1);
    r.append(1, ["a"], []);
    expect(r.revision).toBe(v0 + 1);
    r.clear();
    expect(r.revision).toBe(v0 + 2);
  });
});
