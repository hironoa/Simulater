import { describe, expect, it } from "vitest";
import { decodeVariantsCsv, parseInitFromCsv, sanitizeVariants } from "./variants";

describe("sanitizeVariants", () => {
  it("壊れた文書を空にする", () => {
    for (const raw of [null, [], {}, { variants: 3 }]) expect(sanitizeVariants(raw)).toEqual({ version: 1, selected: "", variants: [] });
  });
  it("壊れた行を捨て、仕向名と変数名の重複は先を採り、値の表記を保つ", () => {
    expect(sanitizeVariants({ selected: "国内", variants: [null, { name: "国内", values: [null, { name: "a", value: "0x20", mode: "once" }, { name: "a", value: "9", mode: "every" }, { name: "b", value: 3, mode: "once" }, { name: "c", value: "1", mode: "bad" }] }, { name: "国内", values: [] }] })).toEqual({ version: 1, selected: "国内", variants: [{ name: "国内", values: [{ name: "a", value: "0x20", mode: "once" }] }] });
  });
  it("存在しない選択を解除する", () => expect(sanitizeVariants({ selected: "海外", variants: [] }).selected).toBe(""));
  it("入力と監視を検査して残し、項目がない仕向と空の構成を区別する", () => {
    const doc = sanitizeVariants({ variants: [
      { name: "A", values: [], inputs: [null, { name: "", widget: "button" },
        { name: "x", widget: "slider", step: -1, hold: true, value: " 0x20000000000001 ", holdPending: true },
        { name: "x" }, { name: "y", hold: true, holdPending: true, value: "1\n2" }],
      watch: [0, { name: "z", plot: false, radix: "hex" }, { name: "z" }, { name: "q", radix: "invalid" }] },
      { name: "B", values: [] },
      { name: "C", values: [], inputs: [], watch: [] },
    ] });
    expect(doc.variants[0]).toEqual({ name: "A", values: [], inputs: [
      { name: "x", widget: "slider", min: 0, max: 100, step: 1, hold: true, value: "0x20000000000001" },
      { name: "y", widget: "number", min: 0, max: 100, step: 1, hold: true },
    ], watch: [{ name: "z", plot: false, radix: "hex" }, { name: "q", plot: true, radix: "dec" }] });
    expect(doc.variants[1].inputs).toBeUndefined();
    expect(doc.variants[1].watch).toBeUndefined();
    expect(doc.variants[2]).toMatchObject({ inputs: [], watch: [] });
  });
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
