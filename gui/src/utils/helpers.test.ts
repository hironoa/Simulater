import { describe, expect, it } from "vitest";
import { detectUndefinedValue } from "./helpers";

describe("detectUndefinedValue", () => {
  it("十万点を超える記録でもスタックを溢れさせない（ライブ記録の一時停止で使う）", () => {
    const data = Array.from({ length: 300_000 }, (_, i) => i % 100);
    data[5] = 65535;
    expect(detectUndefinedValue(data)).toEqual([65535]);
    expect(detectUndefinedValue(data.map((v) => (v === 65535 ? 1 : v)))).toBeNull();
  });
});
