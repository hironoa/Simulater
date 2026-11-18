import { describe, expect, it } from "vitest";
import { decimateRange } from "./decimate";

const indices = (points: [number, number | null][]) => points.map(([i]) => i);

describe("decimateRange", () => {
  it("returns every point in the range (plus one on each side) when the range is short", () => {
    const values = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    expect(decimateRange(values, 3, 6, 10)).toEqual([[2, 2], [3, 3], [4, 4], [5, 5], [6, 6]]);
    expect(decimateRange(values, 0, 10, 10)).toEqual(values.map((v, i) => [i, v]));
  });

  it("keeps the number of points near 4 per bucket for a long range", () => {
    const values = Array.from({ length: 100_000 }, (_, i) => Math.sin(i / 50));
    const out = decimateRange(values, 0, values.length, 1000);
    expect(out.length).toBeLessThanOrEqual(4 * 1000);
    expect(out.length).toBeGreaterThan(1000);
  });

  it("returns indices in strictly increasing order with their original values", () => {
    const values = Array.from({ length: 50_000 }, (_, i) => (i * 7919) % 101);
    const out = decimateRange(values, 1234, 45_678, 300);
    for (let k = 1; k < out.length; k++) expect(out[k][0]).toBeGreaterThan(out[k - 1][0]);
    for (const [i, v] of out) expect(v).toBe(values[i]);
    expect(out[0][0]).toBe(1233);
    expect(out[out.length - 1][0]).toBe(45_678);
  });

  it("keeps a one-sample pulse inside a wide bucket", () => {
    const values = new Array<number | null>(100_000).fill(0);
    values[54_321] = 1;
    values[77_777] = -1;
    const out = decimateRange(values, 0, values.length, 100);
    expect(out).toContainEqual([54_321, 1]);
    expect(out).toContainEqual([77_777, -1]);
  });

  it("keeps the edges of gaps (null) so the line breaks in the same place", () => {
    const values: (number | null)[] = Array.from({ length: 100_000 }, (_, i) => i % 3);
    for (let i = 40_000; i < 40_010; i++) values[i] = null;
    const out = decimateRange(values, 0, values.length, 100);
    const at = indices(out);
    expect(at).toContain(39_999);
    expect(at).toContain(40_000);
    expect(at).toContain(40_009);
    expect(at).toContain(40_010);
  });

  it("keeps the first and last sample of every bucket so step edges stay within one bucket", () => {
    const values = Array.from({ length: 10_000 }, (_, i) => (Math.floor(i / 333) % 2));
    const out = decimateRange(values, 0, values.length, 50);
    // 値が変わる直前と直後の点の番号の差は、区間の幅（200）以内
    for (let k = 1; k < out.length; k++) {
      if (out[k][1] !== out[k - 1][1]) expect(out[k][0] - out[k - 1][0]).toBeLessThanOrEqual(200);
    }
  });

  it("clamps the range to the data", () => {
    expect(decimateRange([1, 2, 3], -5, 99, 10)).toEqual([[0, 1], [1, 2], [2, 3]]);
    expect(decimateRange([], 0, 0, 10)).toEqual([]);
  });
});
