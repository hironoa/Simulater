import type { LiveRecord } from "./liveRecord";

/**
 * ライブ波形の描画。Canvas 2D に直接描く。
 *
 * ECharts の setOption でレーンの数だけ grid / 軸 / series を更新すると、20 本あまり・2,000 点あまりで 1 回の描画に
 * 100 ms 前後かかり（利用者の PC での実測 112.5 ms）、波形が 1 秒に数回しか進まずカクカクする。ここでは:
 *   - 点の数が画面の横幅より多いレーンは、画面の 1 列ごとに「最初・最小・最大・最後」へまとめて描く
 *     （描く線の数が横幅で頭打ちになり、列の中の山と谷も消えない）
 *   - 値は階段状（次の点まで前の値を保つ）に描く。周期ごとの標本をそのまま見せるため
 *   - 軸の目盛りは見やすい数（1・2・5 × 10^n）に丸める
 */

export interface LiveCanvasSeries {
  sig: string;
  color: string;
  label: string;
  conv: { lsb: number; offset: number };
  yMin: number | null;
  yMax: number | null;
  lineWidth: number;
}

export interface LiveCanvasLane {
  top: number;
  height: number;
  series: LiveCanvasSeries[];
  /**
   * 重ね表示で縦軸をそろえるか（グループの syncRange）。偽なら系列ごとに自分の範囲で描き、目盛りは先頭の系列の範囲で描く。
   * 省略時は真（共通の範囲）。止まっているときの表示（chartOptionsBuilder）と同じ規則。
   */
  syncRange?: boolean;
  /**
   * 重ねずに並べるグループで縦軸をそろえるとき（syncGroup）の、範囲を求める系列とそのまとまりの名前。
   * 同じ syncKey のレーンは、1 フレームで 1 回だけ求めた同じ範囲を使う。
   */
  syncKey?: string | null;
  rangeSeries?: LiveCanvasSeries[];
}

/** 1 系列だけの範囲（固定範囲があればそれ、なければ窓の中の値）。 */
export function liveSeriesRange(record: LiveRecord, s: LiveCanvasSeries, start: number, end: number) {
  return laneRange(record.column(s.sig), start, end, s.conv, s.yMin, s.yMax);
}

/**
 * レーンに共通の範囲。固定範囲の系列がある場合はその和、なければ窓内の全系列の範囲。
 * syncGroup のレーンは、そのまとまりの全系列（rangeSeries）から求める。
 */
export function liveLaneRange(record: LiveRecord, lane: LiveCanvasLane, start: number, end: number) {
  const all = lane.rangeSeries ?? lane.series;
  const fixed = all.filter(s => s.yMin !== null || s.yMax !== null);
  let lo = Infinity, hi = -Infinity;
  for (const s of fixed.length ? fixed : all) {
    const range = laneRange(record.column(s.sig), start, end, s.conv, s.yMin, s.yMax);
    if (range) { lo = Math.min(lo, range.lo); hi = Math.max(hi, range.hi); }
  }
  return lo === Infinity ? null : { lo, hi };
}

export interface LiveCanvasStyle {
  axisColor: string;
  splitColor: string;
  /** 波形の領域の左右の余白（px）。左はレーン名と目盛りの値を書く場所。 */
  left: number;
  right: number;
}

/** 描画に必要な Canvas 2D の機能だけ（テストで差し替えられるようにする）。 */
export type LiveCanvasContext = Pick<CanvasRenderingContext2D,
  "beginPath" | "moveTo" | "lineTo" | "stroke" | "fillText" | "measureText" | "clearRect" | "save" | "restore" | "rect" | "clip" | "setTransform"
> & {
  strokeStyle: string | CanvasGradient | CanvasPattern;
  fillStyle: string | CanvasGradient | CanvasPattern;
  lineWidth: number;
  font: string;
  textAlign: CanvasTextAlign;
  textBaseline: CanvasTextBaseline;
  lineJoin: CanvasLineJoin;
};

/** [min, max] を覆う見やすい目盛り（1・2・5 × 10^n 刻み、おおよそ target 本）。min === max なら 1 本。 */
export function niceTicks(min: number, max: number, target: number): number[] {
  if (!Number.isFinite(min) || !Number.isFinite(max)) return [];
  if (max < min) [min, max] = [max, min];
  if (max === min) return [min];
  const raw = (max - min) / Math.max(1, target);
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  const step = (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10) * mag;
  // 目盛りは「整数 × 間隔」で作る（足し込むと誤差がたまる）。0.1 × 3 のような誤差は、間隔の小数の桁数で丸めて消す。
  // 有効桁数で丸めると、大きな基準値の近くの小さな間隔（1e12 + 1, 1e12 + 2）がつぶれる
  // toFixed は 20 桁まで。それより細かい間隔（1e-24 など）は丸めずに元の値を使う
  const need = step < 1 ? Math.ceil(-Math.log10(step) - 1e-9) : 0;
  const decimals = need <= 20 ? need : 0;
  const k0 = Math.ceil(min / step - 1e-9), k1 = Math.floor(max / step + 1e-9);
  const out: number[] = [];
  for (let k = k0; k <= k1 && out.length < 100; k++) {
    const v = k === 0 ? 0 : k * step;   // -0 を出さない
    out.push(decimals > 0 ? Number(v.toFixed(decimals)) : v);
  }
  return out;
}

/**
 * 目盛りの値の表記。step（目盛りの間隔）が分かれば、隣の目盛りと区別できる桁数まで出す
 * （小数 6 桁で切ると、1e-6 未満の間隔の目盛りがすべて同じ表記になる）。大きな値は桁区切りを付ける。
 */
export function formatTick(v: number, step?: number): string {
  const r = v;
  const a = Math.abs(r);
  // 間隔から決まる小数の桁数（間隔 0.5 → 1 桁、0.0000002 → 7 桁）
  const digits = step && step > 0 && Number.isFinite(step) ? Math.max(0, Math.ceil(-Math.log10(step) - 1e-9)) : 6;
  if (a >= 1e15 || (r !== 0 && a < 1e-6) || digits > 9) {
    // 指数表記でも、間隔を区別できる有効桁数を保つ
    const sig = step && step > 0 && a > 0 ? Math.min(15, Math.max(1, Math.ceil(Math.log10(a / step) + 1e-9) + 1)) : 4;
    return r.toExponential(sig - 1);
  }
  return r.toLocaleString("en-US", { maximumFractionDigits: Math.min(20, digits) });
}

/** 目盛りの並びをまとめて表記にする（間隔は並びから取る）。 */
export function formatTicks(ticks: readonly number[]): string[] {
  const step = ticks.length > 1 ? Math.abs(ticks[1] - ticks[0]) : undefined;
  return ticks.map((t) => formatTick(t, step));
}

/** 縦軸の範囲。固定の指定がなければ、窓の中の値に少し余白を付ける。値がなければ null。 */
export function laneRange(
  col: Float64Array | undefined, start: number, end: number,
  conv: { lsb: number; offset: number }, yMin: number | null, yMax: number | null,
): { lo: number; hi: number } | null {
  let lo = Infinity, hi = -Infinity;
  if (col) {
    for (let i = start; i < end; i++) {
      const v = col[i];
      if (v !== v) continue; // NaN
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
  }
  if (lo === Infinity) {
    if (yMin === null || yMax === null) return null;
    return { lo: yMin, hi: yMax };
  }
  // 物理値への変換は一次式なので、端の 2 点だけ変換すれば足りる（lsb が負なら大小が入れ替わる）
  let a = lo * conv.lsb + conv.offset, b = hi * conv.lsb + conv.offset;
  if (a > b) [a, b] = [b, a];
  if (yMin !== null) a = yMin;
  if (yMax !== null) b = yMax;
  if (!(b > a)) {
    // 平らな信号: 線が枠の真ん中に来るように上下へ広げる
    const pad = Math.abs(a) > 1 ? Math.abs(a) * 0.1 : 1;
    return { lo: a - pad, hi: a + pad };
  }
  const pad = (b - a) * 0.08;
  return { lo: yMin !== null ? a : a - pad, hi: yMax !== null ? b : b + pad };
}

/** 文字列を幅 maxWidth に収める（収まらなければ末尾を … にする）。 */
export function fitText(ctx: Pick<LiveCanvasContext, "measureText">, text: string, maxWidth: number): string {
  if (ctx.measureText(text).width <= maxWidth) return text;
  let lo = 0, hi = text.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (ctx.measureText(text.slice(0, mid) + "…").width <= maxWidth) lo = mid; else hi = mid - 1;
  }
  return lo > 0 ? text.slice(0, lo) + "…" : "…";
}

/**
 * 1 レーンの波形を階段状に描く（beginPath から stroke の手前まで）。描いた線分の数を返す。
 * 同じ画面の列に入る点は「最初・最小・最大・最後」にまとめる。NaN で線を切る。
 */
export function traceStepLine(
  ctx: Pick<LiveCanvasContext, "moveTo" | "lineTo">,
  steps: Float64Array, col: Float64Array, start: number, end: number,
  xOf: (step: number) => number, yOf: (raw: number) => number,
): number {
  let segs = 0;
  let pen = false;          // 線がつながっているか
  let curCol = NaN;         // いままとめている画面の列（整数の px）
  let cMin = 0, cMax = 0, cLast = 0;
  const flush = () => {
    // 列の中の上下の振れを縦線で描き、最後の値の高さで次へ渡す
    if (cMin !== cMax) { ctx.lineTo(curCol, cMin); ctx.lineTo(curCol, cMax); segs += 2; }
    ctx.lineTo(curCol, cLast);
    segs++;
  };
  for (let i = start; i < end; i++) {
    const v = col[i];
    if (v !== v) {
      if (pen) { flush(); pen = false; curCol = NaN; }
      continue;
    }
    const px = Math.round(xOf(steps[i]));
    const py = yOf(v);
    if (!pen) {
      ctx.moveTo(px, py);
      pen = true; curCol = px; cMin = cMax = cLast = py;
      continue;
    }
    if (px === curCol) {
      if (py < cMin) cMin = py;
      if (py > cMax) cMax = py;
      cLast = py;
      continue;
    }
    flush();
    // 階段: 前の値のまま新しい列まで横へ進み、そこで新しい値へ移る
    ctx.lineTo(px, cLast);
    segs++;
    curCol = px; cMin = cMax = cLast = py;
  }
  if (pen) flush();
  return segs;
}

export interface LiveDrawResult { points: number; xMin: number; xMax: number }

/**
 * 記録の直近 n 点を描く。width / height は CSS px、dpr は devicePixelRatio。
 * 点数が窓に満たないうちも窓の幅で横軸を固定する（左から伸び、窓を満たしたら流れる）。
 */
export function drawLive(
  ctx: LiveCanvasContext, width: number, height: number, dpr: number,
  record: LiveRecord, lanes: readonly LiveCanvasLane[], n: number, periodMs: number, style: LiveCanvasStyle,
): LiveDrawResult {
  const { start, end } = record.window(n);
  const steps = record.steps();
  const count = end - start;
  const xMin = count ? steps[start] * periodMs : 0;
  const xMax = count ? Math.max(steps[end - 1] * periodMs, xMin + (n - 1) * periodMs) : periodMs * Math.max(1, n - 1);
  const plotL = style.left, plotR = Math.max(style.left + 1, width - style.right);
  const plotW = plotR - plotL;
  const xSpan = xMax > xMin ? xMax - xMin : 1;
  const xOf = (step: number) => plotL + ((step * periodMs - xMin) / xSpan) * plotW;

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.lineJoin = "round";

  const xTicks = niceTicks(xMin, xMax, Math.max(2, Math.floor(plotW / 110)));
  let points = 0;

  // syncGroup の範囲は、まとまりごとに 1 フレームで 1 回だけ求める
  const groupRanges = new Map<string, { lo: number; hi: number } | null>();

  lanes.forEach((lane, li) => {
    const top = lane.top, bottom = lane.top + lane.height;
    const perSeries = lane.syncRange === false && lane.series.length > 1;
    let range: { lo: number; hi: number } | null;
    if (perSeries) range = liveSeriesRange(record, lane.series[0], start, end);
    else if (lane.syncKey) {
      if (!groupRanges.has(lane.syncKey)) groupRanges.set(lane.syncKey, liveLaneRange(record, lane, start, end));
      range = groupRanges.get(lane.syncKey)!;
    } else range = liveLaneRange(record, lane, start, end);
    const toY = (r: { lo: number; hi: number } | null) => {
      const lo = r ? r.lo : 0, hi = r ? r.hi : 1;
      const span = hi > lo ? hi - lo : 1;
      return (p: number) => bottom - ((p - lo) / span) * lane.height;
    };
    const yPhys = toY(range);
    const lo = range ? range.lo : 0, hi = range ? range.hi : 1;

    // 枠と目盛り線
    ctx.strokeStyle = style.splitColor;
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (const t of xTicks) {
      const x = Math.round(plotL + ((t - xMin) / xSpan) * plotW) + 0.5;
      ctx.moveTo(x, top); ctx.lineTo(x, bottom);
    }
    const yTicks = range ? niceTicks(lo, hi, lane.height >= 60 ? 3 : 2) : [];
    for (const t of yTicks) {
      const y = Math.round(yPhys(t)) + 0.5;
      ctx.moveTo(plotL, y); ctx.lineTo(plotR, y);
    }
    ctx.moveTo(plotL + 0.5, top); ctx.lineTo(plotL + 0.5, bottom);
    ctx.moveTo(plotL, Math.round(bottom) + 0.5); ctx.lineTo(plotR, Math.round(bottom) + 0.5);
    ctx.stroke();

    // 縦軸の値
    ctx.fillStyle = style.axisColor;
    ctx.font = "10px sans-serif";
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    let tickW = 0;
    const yLabels = formatTicks(yTicks);
    yTicks.forEach((t, k) => {
      tickW = Math.max(tickW, ctx.measureText(yLabels[k]).width);
      ctx.fillText(yLabels[k], plotL - 5, yPhys(t));
    });

    // レーン名（値の左。重ならない幅に切り詰める）
    ctx.font = "bold 11px sans-serif";
    const labelRight = plotL - 5 - tickW - 8;
    const labels = lane.series.slice(0, Math.floor(lane.height / 12));
    labels.forEach((series, i) => {
      ctx.fillStyle = series.color;
      ctx.fillText(fitText(ctx, series.label, Math.max(20, labelRight - 4)), labelRight,
        (top + bottom) / 2 + (i - (labels.length - 1) / 2) * 12);
    });

    // 横軸の値（いちばん下のレーンだけ）
    if (li === lanes.length - 1) {
      ctx.fillStyle = style.axisColor;
      ctx.font = "10px sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "top";
      const xLabels = formatTicks(xTicks);
      xTicks.forEach((t, k) => ctx.fillText(xLabels[k], plotL + ((t - xMin) / xSpan) * plotW, bottom + 4));
      ctx.textAlign = "right";
      ctx.fillText("ms", plotR, bottom + 16);
    }

    // 波形
    for (const [k, series] of lane.series.entries()) {
      const col = record.column(series.sig);
      const own = perSeries && k > 0 ? liveSeriesRange(record, series, start, end) : range;
      if (!col || !own || count === 0) continue;
      const { lsb, offset } = series.conv;
      const ySeries = own === range ? yPhys : toY(own);
      const yOf = (raw: number) => ySeries(raw * lsb + offset);
      ctx.save();
      ctx.beginPath();
      ctx.rect(plotL, top, plotW, lane.height);
      ctx.clip();
      ctx.strokeStyle = series.color;
      ctx.lineWidth = series.lineWidth;
      ctx.beginPath();
      traceStepLine(ctx, steps, col, start, end, xOf, yOf);
      ctx.stroke();
      ctx.restore();
      points += count;
    }
  });
  return { points, xMin, xMax };
}
