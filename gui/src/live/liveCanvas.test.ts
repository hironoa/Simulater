import { describe, it, expect } from "vitest";
import { niceTicks, formatTick, formatTicks, laneRange, fitText, traceStepLine, drawLive, type LiveCanvasContext, type LiveCanvasLane } from "./liveCanvas";
import { LiveRecord } from "./liveRecord";

const id = { lsb: 1, offset: 0 };

describe("niceTicks", () => {
  it("1・2・5 刻みで範囲の中の目盛りを返す", () => {
    expect(niceTicks(0, 10000, 3)).toEqual([0, 5000, 10000]);
    expect(niceTicks(0, 2000, 2)).toEqual([0, 1000, 2000]);
    expect(niceTicks(-1.5, -0.5, 2)).toEqual([-1.5, -1, -0.5]);
  });
  it("小数の誤差を値に残さない", () => {
    expect(niceTicks(0, 0.3, 3)).toEqual([0, 0.1, 0.2, 0.3]);
  });
  it("大きな基準値の近くの小さな間隔もつぶさない", () => {
    const t = niceTicks(1e12 + 1, 1e12 + 2, 3);
    expect(t).toEqual([1e12 + 1, 1e12 + 1.5, 1e12 + 2]);
    expect(new Set(formatTicks(t)).size).toBe(3);
    expect(niceTicks(1e12 + 1, 1e12 + 5, 4)).toEqual([1e12 + 1, 1e12 + 2, 1e12 + 3, 1e12 + 4, 1e12 + 5]);
  });
  it("1e-20 より細かい間隔でも目盛りを失わない", () => {
    const t = niceTicks(1e-24, 2e-24, 3);
    expect(t.length).toBeGreaterThan(1);
    expect(t.every((v) => v >= 1e-24 * 0.999 && v <= 2e-24 * 1.001)).toBe(true);
    expect(new Set(formatTicks(t)).size).toBe(t.length);
  });
  it("幅のない範囲は 1 本、数でない範囲は空", () => {
    expect(niceTicks(5, 5, 3)).toEqual([5]);
    expect(niceTicks(NaN, 1, 3)).toEqual([]);
  });
});

describe("formatTick", () => {
  it("桁区切りを付ける", () => {
    expect(formatTick(10000)).toBe("10,000");
    expect(formatTick(-0.5)).toBe("-0.5");
  });
  it("間隔の小さい目盛りも区別できる桁数で書く", () => {
    const labels = formatTicks(niceTicks(0.000001, 0.0000014, 3));
    expect(new Set(labels).size).toBe(labels.length);
    expect(labels.length).toBeGreaterThan(1);
    expect(formatTicks([0.1, 0.2, 0.3])).toEqual(["0.1", "0.2", "0.3"]);
    expect(formatTicks([1.00001, 1.00002])).toEqual(["1.00001", "1.00002"]);
    expect(formatTicks([0, 5000, 10000])).toEqual(["0", "5,000", "10,000"]);
    const tiny = formatTicks([1e-9, 2e-9, 3e-9]);
    expect(new Set(tiny).size).toBe(3);
  });
});

describe("laneRange", () => {
  it("窓の中の値に余白を付ける", () => {
    const r = laneRange(Float64Array.from([0, 100, 50]), 0, 3, id, null, null)!;
    expect(r.lo).toBeCloseTo(-8);
    expect(r.hi).toBeCloseTo(108);
  });
  it("窓の外の値と NaN は見ない", () => {
    const r = laneRange(Float64Array.from([999, 1, NaN, 3, -999]), 1, 4, id, null, null)!;
    expect(r.lo).toBeLessThan(1);
    expect(r.hi).toBeGreaterThan(3);
    expect(r.hi).toBeLessThan(4);
  });
  it("平らな信号は上下に広げて真ん中に描く", () => {
    const r = laneRange(Float64Array.from([3000, 3000]), 0, 2, id, null, null)!;
    expect((r.lo + r.hi) / 2).toBeCloseTo(3000);
    expect(r.hi).toBeGreaterThan(r.lo);
  });
  it("固定の範囲はそのまま使う", () => {
    expect(laneRange(Float64Array.from([1, 2]), 0, 2, id, 0, 10)).toEqual({ lo: 0, hi: 10 });
  });
  it("物理値に変換した範囲を返す（lsb が負でも lo < hi）", () => {
    const r = laneRange(Float64Array.from([0, 10]), 0, 2, { lsb: -2, offset: 5 }, null, null)!;
    expect(r.lo).toBeLessThan(-15);
    expect(r.hi).toBeGreaterThan(5);
  });
  it("値がひとつもなければ null", () => {
    expect(laneRange(Float64Array.from([NaN]), 0, 1, id, null, null)).toBeNull();
    expect(laneRange(undefined, 0, 0, id, null, null)).toBeNull();
  });
});

describe("fitText", () => {
  const ctx = { measureText: (s: string) => ({ width: s.length * 10 }) as TextMetrics };
  it("収まればそのまま、収まらなければ末尾を … にする", () => {
    expect(fitText(ctx, "abc", 30)).toBe("abc");
    expect(fitText(ctx, "abcdef", 40)).toBe("abc…");
  });
});

describe("traceStepLine", () => {
  const rec = () => {
    const ops: string[] = [];
    return { ops, moveTo: (x: number, y: number) => { ops.push(`M${x},${y}`); }, lineTo: (x: number, y: number) => { ops.push(`L${x},${y}`); } };
  };
  it("階段状に描く（次の点まで前の値を保つ）", () => {
    const c = rec();
    traceStepLine(c, Float64Array.from([0, 1, 2]), Float64Array.from([5, 7, 7]), 0, 3, (s) => s * 10, (v) => v);
    expect(c.ops).toEqual(["M0,5", "L0,5", "L10,5", "L10,7", "L20,7", "L20,7"]);
  });
  it("同じ列に入る点は 最小・最大・最後 にまとめる", () => {
    const c = rec();
    // 10 点が 1px に入る: 列 0 に 0..9、列 1 に 10
    const steps = Float64Array.from({ length: 11 }, (_, i) => i);
    const col = Float64Array.from([5, 9, 1, 4, 4, 4, 4, 4, 4, 3, 8]);
    traceStepLine(c, steps, col, 0, 11, (s) => (s < 10 ? 0 : 1), (v) => v);
    expect(c.ops).toEqual(["M0,5", "L0,1", "L0,9", "L0,3", "L1,3", "L1,8"]);
  });
  it("NaN で線を切る", () => {
    const c = rec();
    traceStepLine(c, Float64Array.from([0, 1, 2, 3]), Float64Array.from([1, NaN, 2, 2]), 0, 4, (s) => s * 10, (v) => v);
    expect(c.ops).toEqual(["M0,1", "L0,1", "M20,2", "L20,2", "L30,2", "L30,2"]);
  });
  it("窓の外の点は描かない", () => {
    const c = rec();
    traceStepLine(c, Float64Array.from([0, 1, 2]), Float64Array.from([1, 2, 3]), 1, 2, (s) => s * 10, (v) => v);
    expect(c.ops).toEqual(["M10,2", "L10,2"]);
  });
});

describe("drawLive", () => {
  const fake = () => {
    const calls: string[] = [];
    const f = (name: string) => (...a: unknown[]) => { calls.push(`${name}(${a.join(",")})`); };
    const ctx = {
      fillRect: f("fillRect"), globalAlpha: 1,
      beginPath: f("beginPath"), moveTo: f("moveTo"), lineTo: f("lineTo"), stroke: f("stroke"), fillText: f("fillText"),
      measureText: (s: string) => ({ width: s.length * 6 }) as TextMetrics, clearRect: f("clearRect"), save: f("save"), restore: f("restore"),
      rect: f("rect"), clip: f("clip"), setTransform: f("setTransform"),
      strokeStyle: "", fillStyle: "", lineWidth: 1, font: "", textAlign: "left", textBaseline: "top", lineJoin: "round",
    } as unknown as LiveCanvasContext;
    return { ctx, calls };
  };
  const style = { axisColor: "#000", splitColor: "#ccc", left: 150, right: 20 };
  const lane = (sig: string) => ({ top: 10, height: 100, series: [{ sig, color: "#f00", label: sig, conv: id, yMin: null, yMax: null, lineWidth: 1.5 }] });

  const stateLane = (): LiveCanvasLane => ({ ...lane("a"), series: [{ ...lane("a").series[0],
    conv: { lsb: 10, offset: 100 }, modeMapping: { 0: "停止", 1: "運転" } }] });
  const states = (values = [0, 0, 1, 1, 1]) => {
    const r = new LiveRecord();
    r.append(0, ["a"], values.map(v => [v]));
    return r;
  };
  const bands = (calls: string[]) => calls.filter(c => c.startsWith("fillRect("));
  const stateText = (calls: string[]) => calls.filter(c => /^fillText\((停止|運転),/.test(c));

  it.each(["bg-only", "full"] as const)("%s: 生の値の連続区間に帯を2つ描き、fullだけ状態名を描く", mode => {
    const { ctx, calls } = fake();
    drawLive(ctx, 600, 200, 1, states(), [stateLane()], 5, 1, style, mode);
    expect(bands(calls)).toEqual(["fillRect(150,10,215,100)", "fillRect(365,10,215,100)"]);
    expect(stateText(calls)).toHaveLength(mode === "full" ? 2 : 0);
    expect(calls.indexOf(bands(calls)[1])).toBeLessThan(calls.indexOf("clip()"));
  });
  it("off は帯も状態名も描かない", () => {
    const { ctx, calls } = fake();
    drawLive(ctx, 600, 200, 1, states(), [stateLane()], 5, 1, style, "off");
    expect(bands(calls)).toHaveLength(0);
    expect(stateText(calls)).toHaveLength(0);
  });
  it("状態名にない値やNaNの区間は描かない", () => {
    const { ctx, calls } = fake();
    drawLive(ctx, 600, 200, 1, states([0, 0, 9, NaN, 1]), [stateLane()], 5, 1, style, "full");
    expect(bands(calls)).toEqual(["fillRect(150,10,215,100)"]);
    expect(stateText(calls)).toHaveLength(1);
  });
  it("重ね表示は状態名のある最初の系列だけに帯を描く", () => {
    const { ctx, calls } = fake();
    const l = { ...stateLane(), series: [...lane("missing").series, ...stateLane().series, ...stateLane().series] };
    drawLive(ctx, 600, 200, 1, states(), [l], 5, 1, style, "full");
    expect(bands(calls)).toHaveLength(2);
    expect(stateText(calls)).toHaveLength(2);
  });
  it("窓の外の値は見ず、文字が入らない帯では名前を省略する", () => {
    const { ctx, calls } = fake();
    drawLive(ctx, 180, 200, 1, states([9, 9, 0, 0, 1, 1, 1]), [stateLane()], 5, 1, style, "full");
    expect(bands(calls)).toEqual(["fillRect(150,10,5,100)", "fillRect(155,10,5,100)"]);
    expect(stateText(calls)).toHaveLength(0);
  });
  it("区間数が横幅を超えるときは1px未満の帯を描かない", () => {
    const { ctx, calls } = fake();
    drawLive(ctx, 174, 200, 1, states([0, 1, 0, 1, 0, 0, 0]), [stateLane()], 7, 1, style, "bg-only");
    expect(bands(calls)).toHaveLength(1);
    expect(Number(bands(calls)[0].split(",")[2])).toBeGreaterThanOrEqual(1);
  });

  it("直近 n 点を描き、横軸は窓の幅で固定する", () => {
    const r = new LiveRecord();
    r.append(0, ["a"], Array.from({ length: 10 }, (_, i) => [i]));
    const { ctx, calls } = fake();
    const d = drawLive(ctx, 600, 200, 2, r, [lane("a")], 4, 5, style);
    expect(d).toEqual({ points: 4, xMin: 30, xMax: 45 });
    expect(calls[0]).toBe("setTransform(2,0,0,2,0,0)");
    // レーン名は枠の左、縦の中央に書く
    expect(calls.some((c) => /^fillText\(a,\d+,60\)$/.test(c))).toBe(true);
  });

  it("点数が窓に満たないときも横軸の幅は窓の分を取り、記録にない信号は描かない", () => {
    const r = new LiveRecord();
    r.append(0, ["a"], [[1], [null]]);
    const { ctx } = fake();
    const d = drawLive(ctx, 600, 200, 1, r, [lane("a"), lane("zz")], 100, 1, style);
    expect(d.xMax).toBe(99);
    expect(d.points).toBe(2);
  });

  it("重ね表示では同じレーンに全系列の線と色付きラベルを描く", () => {
    const r = new LiveRecord();
    r.append(0, ["a", "b"], [[1, 10], [2, 20]]);
    const { ctx, calls } = fake();
    const overlap = { ...lane("a"), series: [...lane("a").series, ...lane("b").series] };
    expect(drawLive(ctx, 600, 200, 1, r, [overlap], 2, 1, style).points).toBe(4);
    expect(calls.filter(c => c === "clip()")).toHaveLength(2);
    expect(calls.some(c => c.startsWith("fillText(a,"))).toBe(true);
    expect(calls.some(c => c.startsWith("fillText(b,"))).toBe(true);
  });

  // 系列ごとの線の y の範囲（clip の後の moveTo / lineTo から stroke まで）
  const lineSpans = (calls: string[]) => {
    const spans: { min: number; max: number }[] = [];
    let cur: number[] | null = null;
    for (const c of calls) {
      if (c === "clip()") cur = [];
      else if (cur && /^(moveTo|lineTo)\(/.test(c)) cur.push(Number(c.slice(c.indexOf(",") + 1, -1)));
      else if (cur && c === "stroke()") { spans.push({ min: Math.min(...cur), max: Math.max(...cur) }); cur = null; }
    }
    return spans;
  };
  const twoScales = () => {
    const r = new LiveRecord();
    r.append(0, ["a", "b"], [[0, 0], [1, 1000]]);
    return r;
  };
  const overlapAB = (syncRange: boolean) => ({ ...lane("a"), series: [...lane("a").series, ...lane("b").series], syncRange });

  it("同期しない重ね表示は、系列ごとの範囲でレーンの高さいっぱいに描く", () => {
    const { ctx, calls } = fake();
    drawLive(ctx, 600, 200, 1, twoScales(), [overlapAB(false)], 2, 1, style);
    const [a, b] = lineSpans(calls);
    expect(a.max - a.min).toBeGreaterThan(80);
    expect(b.max - b.min).toBeGreaterThan(80);
  });

  it("同期しない重ね表示で、片方の固定範囲がもう片方を切らない", () => {
    const { ctx, calls } = fake();
    const l = overlapAB(false);
    l.series[0] = { ...l.series[0], yMin: 0, yMax: 1 };
    drawLive(ctx, 600, 200, 1, twoScales(), [l], 2, 1, style);
    const b = lineSpans(calls)[1];
    expect(b.min).toBeGreaterThanOrEqual(10);
    expect(b.max).toBeLessThanOrEqual(110);
    expect(b.max - b.min).toBeGreaterThan(80);
  });

  it("同期する重ね表示は、全系列に共通の範囲で描く", () => {
    const { ctx, calls } = fake();
    drawLive(ctx, 600, 200, 1, twoScales(), [overlapAB(true)], 2, 1, style);
    const [a, b] = lineSpans(calls);
    expect(a.max - a.min).toBeLessThan(1);
    expect(b.max - b.min).toBeGreaterThan(80);
  });

  it("同じ syncGroup の並べたレーンは同じ範囲、syncGroup のないレーンは自分の範囲", () => {
    const group = [lane("a").series[0], lane("b").series[0]];
    const synced = (sig: string) => ({ ...lane(sig), syncKey: "a,b", rangeSeries: group });
    const { ctx, calls } = fake();
    drawLive(ctx, 600, 200, 1, twoScales(), [synced("a"), synced("b")], 2, 1, style);
    const [a, b] = lineSpans(calls);
    expect(a.max - a.min).toBeLessThan(1);
    expect(b.max - b.min).toBeGreaterThan(80);
    const own = fake();
    drawLive(own.ctx, 600, 200, 1, twoScales(), [lane("a"), lane("b")], 2, 1, style);
    expect(lineSpans(own.calls)[0].max - lineSpans(own.calls)[0].min).toBeGreaterThan(80);
  });

  it("記録が空でも落ちない", () => {
    const { ctx } = fake();
    expect(drawLive(ctx, 600, 200, 1, new LiveRecord(), [lane("a")], 100, 5, style)).toEqual({ points: 0, xMin: 0, xMax: 495 });
  });
});
