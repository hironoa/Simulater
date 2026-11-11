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
