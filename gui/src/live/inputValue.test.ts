import { describe, expect, it } from "vitest";
import {
  buttonRestoreValue, cellNumber, dropPending, holdInputValue, inputValue, mayBeRounded, numberInputValue, restoreHoldText,
  upsertPending, type InputValue,
} from "./inputValue";

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
    expect(inputValue("a", { pending: [{ name: "a", value: "1" }], holds }, 0)).toEqual({ value: 1, source: "pending", send: "1", inexact: false });
    expect(inputValue("a", { pending: [{ name: "b", value: "1" }], holds }, 0)).toEqual({ value: 5, source: "hold", send: 5, inexact: false });
    expect(inputValue("a", { pending: [], holds: [] }, 9)).toEqual({ value: 9, source: "current", send: 9, inexact: false });
    expect(inputValue("a", { pending: [], holds: [] }, undefined)).toEqual({ value: null, source: "current", send: null, inexact: false });
  });

  it("数値にできない反映待ち・固定は飛ばす", () => {
    const sim = { pending: [{ name: "s", value: "'XYZ'" }], holds: [{ name: "s", value: "1|2" }] };
    expect(inputValue("s", sim, 3)).toEqual({ value: 3, source: "current", send: 3, inexact: false });
  });

  it("送り返す値は届いた表記のまま。丸められている可能性のある数値は送らない", () => {
    const big = "9007199254740993";
    expect(inputValue("a", { pending: [{ name: "a", value: big }], holds: [] }, 0))
      .toEqual({ value: 9007199254740992, source: "pending", send: big, inexact: false });
    expect(inputValue("a", { pending: [], holds: [{ name: "a", value: "0x20000000000001" }] }, 0))
      .toMatchObject({ source: "hold", send: "0x20000000000001", inexact: false });
    expect(inputValue("a", { pending: [], holds: [{ name: "a", value: 2 ** 53 }] }, 0))
      .toMatchObject({ source: "hold", send: null, inexact: true });
    expect(inputValue("a", { pending: [], holds: [] }, 2 ** 53)).toEqual({ value: 2 ** 53, source: "current", send: null, inexact: true });
    expect(inputValue("a", { pending: [], holds: [] }, 1.5)).toMatchObject({ send: 1.5, inexact: false });
    expect(inputValue("a", { pending: [], holds: [] }, Number.MAX_SAFE_INTEGER)).toMatchObject({ send: Number.MAX_SAFE_INTEGER });
  });
});

describe("入力パネルが送る値", () => {
  const base = (b: Partial<InputValue>): InputValue => ({ value: null, source: "current", send: null, inexact: false, ...b });

  it("数値入力は前後の空白だけを除いた文字列のまま。空欄は送らない", () => {
    expect(numberInputValue(" 9007199254740993 ")).toBe("9007199254740993");
    expect(numberInputValue("0x20000000000001")).toBe("0x20000000000001");
    expect(numberInputValue("'XYZ'")).toBe("'XYZ'");
    expect(numberInputValue("1|2|3")).toBe("1|2|3");
    expect(numberInputValue("12abc")).toBe("12abc"); // 書式の検証はサーバに任せる
    expect(numberInputValue("")).toBeNull();
    expect(numberInputValue("   ")).toBeNull();
  });

  it("固定: 数値入力は欄の文字列、空欄なら元にする値の表記。丸まった現在値しか無ければ null", () => {
    expect(holdInputValue("number", " 0x20000000000001 ", 0, base({}))).toBe("0x20000000000001");
    expect(holdInputValue("number", "", 0, base({ value: 2 ** 53, source: "pending", send: "9007199254740993" }))).toBe("9007199254740993");
    expect(holdInputValue("number", "  ", 0, base({ value: 7, send: 7 }))).toBe(7);
    expect(holdInputValue("number", "", 0, base({}))).toBeUndefined();
    expect(holdInputValue("number", "", 0, base({ value: 2 ** 53, inexact: true }))).toBeNull();
  });

  it("固定: スライダは元にする値に合わせただけなら元の表記、動かしたらスライダの値", () => {
    const b = base({ value: 16, source: "hold", send: "0x10" });
    expect(holdInputValue("slider", "", 16, b)).toBe("0x10");
    expect(holdInputValue("slider", "", 20, b)).toBe(20);
  });

  it("固定・ボタンの復帰: トグルとボタンは元にする値（無ければ 0、丸まった現在値なら null）", () => {
    expect(holdInputValue("toggle", "", 0, base({ value: 1, source: "pending", send: "1" }))).toBe("1");
    expect(holdInputValue("button", "", 0, base({}))).toBe(0);
    expect(holdInputValue("toggle", "", 0, base({ value: 2 ** 53, inexact: true }))).toBeNull();
    expect(buttonRestoreValue(base({ value: 2 ** 53, source: "pending", send: "9007199254740993" }))).toBe("9007199254740993");
    expect(buttonRestoreValue(base({}))).toBe(0);
    expect(buttonRestoreValue(base({ value: 2 ** 53, inexact: true }))).toBeNull();
  });
});

describe("restoreHoldText", () => {
  it("丸まって届いた固定値を、同じ数値になる書いた表記に戻す", () => {
    const known = new Map([["a", "9007199254740993"], ["b", "0x20000000000001"], ["c", "1|2|3"], ["d", "'AB'"], ["e", "5"]]);
    const holds = [
      { name: "a", value: 9007199254740992 },
      { name: "b", value: 9007199254740992 },
      { name: "c", value: 1 },
      { name: "d", value: 65 },
      { name: "e", value: 6 }, // ほかから変わった
      { name: "f", value: 3 },
      { name: "a2", value: "x" },
    ];
    expect(restoreHoldText(holds, known)).toEqual([
      { name: "a", value: "9007199254740993" },
      { name: "b", value: "0x20000000000001" },
      { name: "c", value: "1|2|3" },
      { name: "d", value: "'AB'" },
      { name: "e", value: 6 },
      { name: "f", value: 3 },
      { name: "a2", value: "x" },
    ]);
  });
});

describe("mayBeRounded", () => {
  it("安全整数の範囲を超えた整数だけ", () => {
    expect(mayBeRounded(2 ** 53)).toBe(true);
    expect(mayBeRounded(-(2 ** 53))).toBe(true);
    expect(mayBeRounded(Number.MAX_SAFE_INTEGER)).toBe(false);
    expect(mayBeRounded(1.5)).toBe(false);
    expect(mayBeRounded(null)).toBe(false);
    expect(mayBeRounded(undefined)).toBe(false);
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
