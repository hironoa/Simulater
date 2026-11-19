import { FONT_MONO, RIGHT_AXIS_WIDTH, laneLabelTop, laneSplitNumber, signalLineType } from "../utils/laneLabels";
import { fitLabel } from "../utils/fitLabel";
import { getDarkerColor } from "../utils/helpers";
import type { AppState, SignalSetting } from "../types";
import { getStateColor, type StateMapping } from "../utils/stateNames";
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
  lineType?: SignalSetting["lineType"];
  modeMapping?: StateMapping;
}

export interface LiveCanvasLane {
  top: number;
  height: number;
  series: LiveCanvasSeries[];
  /**
   * 重ね表示で縦軸をそろえるか（グループの syncRange）。偽なら系列ごとに自分の範囲で描き、各系列の目盛りを左右に描く。
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
  dark?: boolean;
  /** 右軸1本の幅。省略時は35px。 */
  rightAxisWidth?: number;
  stateOpacity?: number;
  axisColor: string;
  splitColor: string;
  /** 波形の領域の左右の余白（px）。左はレーン名と目盛りの値を書く場所。 */
  left: number;
  right: number;
}

/** 描画に必要な Canvas 2D の機能だけ（テストで差し替えられるようにする）。 */
export type LiveCanvasContext = Pick<CanvasRenderingContext2D,
  "strokeText" | "setLineDash" | "fillRect" | "globalAlpha" | "beginPath" | "moveTo" | "lineTo" | "stroke" | "fillText" | "measureText" | "clearRect" | "save" | "restore" | "rect" | "clip" | "setTransform"
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
 * （小数 6 桁で切ると、1e-6 未満の間隔の目盛りがすべて同じ表記になる）。既定では大きな値に桁区切りを付ける。
 */
export function formatTick(v: number, step?: number, useGrouping = true): string {
  const r = v;
  const a = Math.abs(r);
  // 間隔から決まる小数の桁数（間隔 0.5 → 1 桁、0.0000002 → 7 桁）
  const digits = step && step > 0 && Number.isFinite(step) ? Math.max(0, Math.ceil(-Math.log10(step) - 1e-9)) : 6;
  if (a >= 1e15 || (r !== 0 && a < 1e-6) || digits > 9) {
    // 指数表記でも、間隔を区別できる有効桁数を保つ
    const sig = step && step > 0 && a > 0 ? Math.min(15, Math.max(1, Math.ceil(Math.log10(a / step) + 1e-9) + 1)) : 4;
    return r.toExponential(sig - 1);
  }
  return r.toLocaleString("en-US", { maximumFractionDigits: Math.min(20, digits), useGrouping });
}

/** 目盛りの並びをまとめて表記にする（間隔は並びから取る）。 */
export function formatTicks(ticks: readonly number[], useGrouping = true): string[] {
  const step = ticks.length > 1 ? Math.abs(ticks[1] - ticks[0]) : undefined;
  return ticks.map((t) => formatTick(t, step, useGrouping));
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

export interface LiveDrawResult { points: number; xMin: number; xMax: number; maxTickCharacters: number; maxRightTickCharacters: number }

/**
 * 記録の直近 n 点を描く。width / height は CSS px、dpr は devicePixelRatio。
 * 点数が窓に満たないうちも窓の幅で横軸を固定する（左から伸び、窓を満たしたら流れる）。
 */
export function drawLive(
  ctx: LiveCanvasContext, width: number, height: number, dpr: number,
  record: LiveRecord, lanes: readonly LiveCanvasLane[], n: number, periodMs: number, style: LiveCanvasStyle,
  showMode: AppState["showMode"] = "off",
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
  let points = 0, maxTickCharacters = 0, maxRightTickCharacters = 0;

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

    // 範囲は軸と波形で使い回し、同期しない系列も 1 回だけ走査する。
    const ranges = lane.series.map((series, k) => perSeries && k > 0
      ? liveSeriesRange(record, series, start, end) : range);
    ctx.setLineDash([]);
    ctx.globalAlpha = 1;
    ctx.strokeStyle = style.axisColor;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(plotL, Math.round(bottom) + 0.5); ctx.lineTo(plotR, Math.round(bottom) + 0.5);
    ctx.stroke();

    lane.series.forEach((series, k) => {
      if (k > 0 && !perSeries) return;
      const axisX = k === 0 ? plotL : plotR + (k - 1) * (style.rightAxisWidth ?? RIGHT_AXIS_WIDTH);
      ctx.strokeStyle = series.color;
      ctx.lineWidth = 2;
      ctx.globalAlpha = 0.4;
      ctx.beginPath();
      ctx.moveTo(axisX, top); ctx.lineTo(axisX, bottom);
      ctx.stroke();
      ctx.globalAlpha = 1;
      const own = ranges[k];
      const ticks = own ? niceTicks(own.lo, own.hi, laneSplitNumber(lane.height)) : [];
      const labels = formatTicks(ticks, false);
      let axisCharacters = own ? Math.max(
        String(Math.round(own.lo)).length, String(Math.round(own.hi)).length) : 0;
      const yAxis = toY(own);
      ctx.fillStyle = series.color;
      ctx.font = `10px ${FONT_MONO}`;
      ctx.textAlign = k === 0 ? "right" : "left";
      ctx.textBaseline = "middle";
      ticks.forEach((t, i) => {
        axisCharacters = Math.max(axisCharacters, labels[i].length);
        ctx.fillText(labels[i], axisX + (k === 0 ? -4 : 4), yAxis(t));
      });
      if (k === 0) maxTickCharacters = Math.max(maxTickCharacters, axisCharacters);
      else maxRightTickCharacters = Math.max(maxRightTickCharacters, axisCharacters);
    });

    // 信号名の束を中央に置く。停止時と同じ幅の余白に全文を描く。
    ctx.font = "bold 10px sans-serif";
    ctx.textAlign = "left";
    ctx.textBaseline = "top";
    lane.series.forEach((series, i) => {
      if (!series.label) return;
      const y = laneLabelTop(top, lane.height, lane.series.length, i);
      ctx.fillStyle = series.color;
      if (!style.dark) {
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 2;
        ctx.strokeText(series.label, 5, y);
      }
      ctx.fillText(series.label, 5, y);
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

    // 状態の帯は生の値で区切り、重ね表示でも最初の状態系列だけを波形の下に描く。
    const stateSeries = showMode !== "off" ? lane.series.find(s => s.modeMapping !== undefined) : undefined;
    const stateCol = stateSeries && record.column(stateSeries.sig);
    if (stateSeries && stateCol && count > 0) {
      let segments = 1;
      for (let i = start + 1; i < end; i++) {
        if (stateCol[i] !== stateCol[i - 1]) segments++;
      }
      const dense = segments > plotW;
      ctx.save();
      ctx.font = "11px sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      for (let i = start; i < end;) {
        const value = stateCol[i];
        let j = i + 1;
        while (j < end && stateCol[j] === value) j++;
        const left = Math.max(plotL, xOf(steps[i]));
        const right = Math.min(plotR, xOf(steps[j < end ? j : end - 1]));
        const bandWidth = right - left;
        const name = stateSeries.modeMapping?.[value];
        if (Number.isFinite(value) && name !== undefined && bandWidth > 0 && (!dense || bandWidth >= 1)) {
          ctx.fillStyle = getStateColor(value);
          ctx.globalAlpha = style.stateOpacity ?? 0.15;
          ctx.fillRect(left, top, bandWidth, lane.height);
          ctx.globalAlpha = 1;
          const fitted = showMode === "full" ? fitLabel(name, bandWidth - 8, lane.height - 4) : null;
          if (fitted) {
            const color = getStateColor(value);
            ctx.fillStyle = style.dark ? color : getDarkerColor(color, 0.6);
            ctx.font = `600 ${fitted.fontSize}px ${FONT_MONO}`;
            ctx.textBaseline = "top";
            const lines = fitted.text.split("\n");
            const textTop = (top + bottom - lines.length * fitted.lineHeight) / 2;
            lines.forEach((line, k) => {
              const y = textTop + k * fitted.lineHeight;
              if (!style.dark) {
                ctx.strokeStyle = "#ffffff";
                ctx.lineWidth = 2;
                ctx.strokeText(line, (left + right) / 2, y);
              }
              ctx.fillText(line, (left + right) / 2, y);
            });
          }
        }
        i = j;
      }
      ctx.restore();
    }

    // 波形
    for (const [k, series] of lane.series.entries()) {
      const col = record.column(series.sig);
      const own = ranges[k];
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
      const lineType = signalLineType(series.sig, series.lineType);
      ctx.setLineDash((lineType === "dashed" ? [5, 5] : lineType === "dotted" ? [1, 3] : [])
        .map(length => length * series.lineWidth));
      ctx.beginPath();
      traceStepLine(ctx, steps, col, start, end, xOf, yOf);
      ctx.stroke();
      ctx.restore();
      points += count;
    }
  });
  return { points, xMin, xMax, maxTickCharacters, maxRightTickCharacters };
}
