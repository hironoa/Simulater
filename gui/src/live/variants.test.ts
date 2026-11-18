import { describe, expect, it } from "vitest";
import { decodeVariantsCsv, onceSignature, parseInitFromCsv, sanitizeVariants, type Variant } from "./variants";

describe("onceSignature", () => {
  const before: Variant = { name: "国内", values: [
    { name: "a", value: "1", mode: "once" },
    { name: "b", value: "2", mode: "once" },
    { name: "c", value: "3", mode: "every" },
  ] };
  it("once の削除を検出する", () => {
    expect(onceSignature({ ...before, values: before.values.filter(v => v.name !== "a") })).not.toBe(onceSignature(before));
  });
  it("選択なしへの切替を検出する", () => {
    expect(onceSignature(undefined)).not.toBe(onceSignature(before));
  });
  it("once のない仕向への切替を検出する", () => {
    expect(onceSignature({ name: "海外", values: before.values.filter(v => v.mode === "every") })).not.toBe(onceSignature(before));
  });
  it("every だけの変更では変わらない", () => {
    expect(onceSignature({ ...before, values: before.values.map(v => v.mode === "every" ? { ...v, value: "4" } : v) })).toBe(onceSignature(before));
  });
  it("並び順だけの変更では変わらず、元の配列も変更しない", () => {
    const reversed = { ...before, values: [...before.values].reverse() };
    expect(onceSignature(reversed)).toBe(onceSignature(before));
    expect(reversed.values.map(v => v.name)).toEqual(["c", "b", "a"]);
  });
  it("両方空なら変わらない", () => {
    expect(onceSignature({ name: "海外", values: [] })).toBe(onceSignature(undefined));
  });
  it("once の名前または値の変更を検出する", () => {
    for (const row of [{ name: "d", value: "1" }, { name: "a", value: "9" }]) {
      expect(onceSignature({ ...before, values: before.values.map(v => v.name === "a" ? { ...v, ...row } : v) })).not.toBe(onceSignature(before));
    }
  });
});

describe("sanitizeVariants", () => {
  it("壊れた文書を空にする", () => {
    for (const raw of [null, [], {}, { variants: 3 }]) expect(sanitizeVariants(raw)).toEqual({ version: 1, selected: "", variants: [] });
  });
  it("壊れた行を捨て、仕向名と変数名の重複は先を採り、値の表記を保つ", () => {
    expect(sanitizeVariants({ selected: "国内", variants: [null, { name: "国内", values: [null, { name: "a", value: "0x20", mode: "once" }, { name: "a", value: "9", mode: "every" }, { name: "b", value: 3, mode: "once" }, { name: "c", value: "1", mode: "bad" }] }, { name: "国内", values: [] }] })).toEqual({ version: 1, selected: "国内", variants: [{ name: "国内", values: [{ name: "a", value: "0x20", mode: "once" }] }] });
  });
  it("存在しない選択を解除する", () => expect(sanitizeVariants({ selected: "海外", variants: [] }).selected).toBe(""));
});
describe("parseInitFromCsv", () => {
  it("BOM・大小文字・引用・カンマ・引用符を読む", () => {
    expect(parseInitFromCsv('\uFEFFmodi,f,t,0,0\r\n"#iNiTWheneverCall","a,b",c\r\n,"\'A,B\'","\'A""B\'"\r\n')).toEqual([
      { name: "a,b", value: "'A,B'", mode: "once" }, { name: "c", value: "'A\"B'", mode: "once" },
    ]);
  });
  it("空セルと空の名前を捨て、空文字列リテラルと数値表記を保つ", () => expect(parseInitFromCsv("#Init,a,b,c,,d\n,0x20,,'' ,5,   ")).toEqual([{ name: "a", value: "0x20", mode: "once" }, { name: "c", value: "'' ", mode: "once" }]));
  it("見出しなし・値行なしなら空", () => {
    expect(parseInitFromCsv("#COMMENT,a\n,1")).toEqual([]);
    expect(parseInitFromCsv("#Init,a")).toEqual([]);
  });
  it("複数見出しでも重複は先の値を採る", () => expect(parseInitFromCsv("#Init,a\n,1\n#InitOnce,a,b\n,2,3")).toEqual([{ name: "a", value: "1", mode: "once" }, { name: "b", value: "3", mode: "once" }]));
});

describe("decodeVariantsCsv", () => {
  it("UTF-8 の日本語を読む", () => {
    const bytes = new TextEncoder().encode("#Init,変数\r\n,1");
    expect(parseInitFromCsv(decodeVariantsCsv(bytes.buffer))).toEqual([{ name: "変数", value: "1", mode: "once" }]);
  });
  it("UTF-8 として不正なら Shift_JIS の日本語を読む", () => {
    // #Init,変数\r\n,1（変 = 95 CF、数 = 90 94）
    const bytes = new Uint8Array([0x23, 0x49, 0x6e, 0x69, 0x74, 0x2c, 0x95, 0xcf, 0x90, 0x94, 0x0d, 0x0a, 0x2c, 0x31]);
    expect(parseInitFromCsv(decodeVariantsCsv(bytes.buffer))).toEqual([{ name: "変数", value: "1", mode: "once" }]);
  });
});
