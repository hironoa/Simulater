import { describe, expect, it } from "vitest";
import { DEFAULT_LAYOUT, loadLayout, sanitizeLayout, saveLayout, STORAGE_KEY } from "./persist";

class MemoryStorage {
  data = new Map<string, string>();
  getItem(k: string) { return this.data.get(k) ?? null; }
  setItem(k: string, v: string) { this.data.set(k, v); }
}

describe("構成の保存と復元", () => {
  it("保存したものを読み戻せる", () => {
    const s = new MemoryStorage();
    const layout = {
      inputs: [{ name: "g_gain", widget: "slider" as const, min: -10, max: 200, step: 5, hold: true, value: 120 }],
      watch: [{ name: "g_sine", plot: true, radix: "hex" as const }],
      windowSize: 3000,
      maxSamples: 50_000,
    };
    saveLayout(layout, s);
    expect(JSON.parse(s.getItem(STORAGE_KEY)!)).toEqual(layout);
    expect(loadLayout(s)).toEqual(layout);
  });

  it("数値入力の文字列の値は書いた表記のまま保存して読み戻す（2^53 を超える整数を丸めない）", () => {
    const s = new MemoryStorage();
    const layout = {
      inputs: [
        { name: "g_u64", widget: "number" as const, min: 0, max: 100, step: 1, hold: true, value: "9007199254740993" },
        { name: "g_hex", widget: "number" as const, min: 0, max: 100, step: 1, hold: false, value: "0x20000000000001" },
      ],
      watch: [],
      windowSize: 2000,
      maxSamples: DEFAULT_LAYOUT.maxSamples,
    };
    saveLayout(layout, s);
    expect(s.getItem(STORAGE_KEY)).toContain('"value":"9007199254740993"');
    expect(loadLayout(s)).toEqual(layout);
    // 空・改行を含む文字列は捨て、前後の空白は除く
    const got = sanitizeLayout({ inputs: [{ name: "a", value: "  " }, { name: "b", value: "1\n2" }, { name: "c", value: " 0x10 " }] });
    expect(got.inputs.map((i) => i.value)).toEqual([undefined, undefined, "0x10"]);
  });

  it("監視から外した入力（monitor: false）を読み戻せる", () => {
    const got = sanitizeLayout({ inputs: [{ name: "@rpm", monitor: false }, { name: "b", monitor: "no" }] });
    expect(got.inputs.map((i) => i.monitor)).toEqual([false, undefined]);
  });

  it("壊れた内容・重複・範囲外の値は直して読む", () => {
    const got = sanitizeLayout({
      inputs: [{ name: "a", widget: "???", step: -1 }, { name: "a" }, { nope: 1 }],
      watch: [{ name: "w", plot: false }, { name: "w" }, "x"],
      windowSize: 1,
      maxSamples: "many",
    });
    expect(got.inputs).toEqual([{ name: "a", widget: "number", min: 0, max: 100, step: 1, hold: false }]);
    expect(got.watch).toEqual([{ name: "w", plot: false, radix: "dec" }]);
    expect(got.windowSize).toBe(50);
    expect(got.maxSamples).toBe(DEFAULT_LAYOUT.maxSamples);
  });

  it("localStorage が使えなくても例外を出さない", () => {
    const broken = {
      getItem: () => { throw new Error("SecurityError"); },
      setItem: () => { throw new Error("QuotaExceeded"); },
    };
    expect(loadLayout(broken)).toEqual(DEFAULT_LAYOUT);
    expect(() => saveLayout(DEFAULT_LAYOUT, broken)).not.toThrow();
    const s = new MemoryStorage();
    s.setItem(STORAGE_KEY, "{not json");
    expect(loadLayout(s)).toEqual(DEFAULT_LAYOUT);
  });
});

describe("入力の後方互換", () => {
  it("印のない古いvalueは生の値で、holdも変更しない", () => {
    const layout = sanitizeLayout({ inputs: [
      { name: "temperature", hold: false, value: "651" },
      { name: "mode", widget: "state", hold: true, value: "1" },
      { name: "new", hold: true, holdPending: true },
    ] });
    expect(layout.inputs.map(i => i.value)).toEqual(["651", "1", undefined]);
    expect(layout.inputs.map(i => i.hold)).toEqual([false, true, true]);
    expect(layout.inputs[2].holdPending).toBe(true);
    const s = new MemoryStorage(); saveLayout(layout, s);
    expect(loadLayout(s)).toEqual(layout);
  });
});
