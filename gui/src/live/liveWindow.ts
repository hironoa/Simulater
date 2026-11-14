import type { LiveRecord } from "./liveRecord";
import { toPhysical } from "../utils/helpers";

export interface LiveLane {
  sig: string;
  conv: { lsb: number; offset: number };
}

export interface LiveSeriesData {
  xMin: number;
  xMax: number;
  /** レーンごとの [時刻 ms, 値] の並び。読めない値は null（線が切れる）。 */
  series: [number, number | null][][];
  points: number;
}

/** 記録の直近 n 点を、ライブ描画用の [時刻, 物理値] に変換する。 */
export function buildLiveSeriesData(record: LiveRecord, lanes: readonly LiveLane[], n: number, periodMs: number): LiveSeriesData {
  const { start, end } = record.window(n);
  const steps = record.steps();
  const count = end - start;
  const xs = new Array<number>(count);
  for (let i = 0; i < count; i++) xs[i] = steps[start + i] * periodMs;
  let points = 0;
  const series = lanes.map((lane) => {
    const col = record.column(lane.sig);
    const out = new Array<[number, number | null]>(count);
    const identity = lane.conv.lsb === 1 && lane.conv.offset === 0;
    for (let i = 0; i < count; i++) {
      const v = col ? col[start + i] : NaN;
      out[i] = [xs[i], Number.isNaN(v) ? null : identity ? v : toPhysical(v, lane.conv)];
    }
    points += count;
    return out;
  });
  const xMin = count ? xs[0] : 0;
  // 点数が窓に満たないうちも窓の幅で軸を固定する（左から伸び、窓を満たしたら流れる）
  const xMax = count ? Math.max(xs[count - 1], xMin + (n - 1) * periodMs) : periodMs * Math.max(1, n - 1);
  return { xMin, xMax, series, points };
}
