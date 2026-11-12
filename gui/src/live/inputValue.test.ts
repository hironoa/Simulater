import { describe, expect, it } from "vitest";
import { cellNumber, dropPending, inputValue, upsertPending } from "./inputValue";

describe("cellNumber", () => {
  it("10 進・16 進・小数・文字をセルの文字列から数値にする", () => {
    expect(cellNumber("7")).toBe(7);
    expect(cellNumber(" -3 ")).toBe(-3);
    expect(cellNumber("0x78")).toBe(0x78);
    expect(cellNumber("-0X10")).toBe(-16);
    expect(cellNumber("1.5")).toBe(1.5);
    expect(cellNumber("1e3")).toBe(1000);
    expect(cellNumber("'A'")).toBe(65);
    expect(cellNumber(4)).toBe(4);
  });

  it("数値にできないものは null", () => {
    expect(cellNumber("'XYZ'")).toBeNull();
    expect(cellNumber("1|2|3")).toBeNull();
    expect(cellNumber("")).toBeNull();
    expect(cellNumber("$4")).toBeNull();
    expect(cellNumber(undefined)).toBeNull();
    expect(cellNumber(Number.NaN)).toBeNull();
  });
});

describe("inputValue", () => {
  it("反映待ち → 固定の値 → メモリの現在値 の順に採る", () => {
    const holds = [{ name: "a", value: 5 }];
    expect(inputValue("a", { pending: [{ name: "a", value: "1" }], holds }, 0)).toEqual({ value: 1, source: "pending" });
    expect(inputValue("a", { pending: [{ name: "b", value: "1" }], holds }, 0)).toEqual({ value: 5, source: "hold" });
    expect(inputValue("a", { pending: [], holds: [] }, 9)).toEqual({ value: 9, source: "current" });
    expect(inputValue("a", { pending: [], holds: [] }, undefined)).toEqual({ value: null, source: "current" });
  });

  it("数値にできない反映待ち・固定は飛ばす", () => {
    const sim = { pending: [{ name: "s", value: "'XYZ'" }], holds: [{ name: "s", value: "1|2" }] };
    expect(inputValue("s", sim, 3)).toEqual({ value: 3, source: "current" });
  });
});

describe("upsertPending / dropPending", () => {
  it("同じ名前は 1 個に置き換え、除く", () => {
    const l = upsertPending(upsertPending([], "a", "1"), "b", "2");
    expect(upsertPending(l, "a", "0")).toEqual([{ name: "b", value: "2" }, { name: "a", value: "0" }]);
    expect(dropPending(l, "a")).toEqual([{ name: "b", value: "2" }]);
    expect(dropPending(l, "zz")).toBe(l);
  });
});
