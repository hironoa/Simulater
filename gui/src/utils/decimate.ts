/**
 * 描画用に点を絞る。ECharts は渡した点の数に比例して重くなる（15 信号 × 60,000 点で 1 回の描き直しに 2〜4 秒）ので、
 * 見えている範囲 [start, end) を画面の幅ほどの区間に分け、区間ごとに「最初・最小・最大・最後」の点だけを渡す。
 *
 * - 値の変わり目は、区間の中の最小・最大で必ず残る（1 点だけのパルスも消えない）。
 * - 値なし（null）の区切りは、区間の中で null に入る点と出る点を残すので、途切れもそのまま描ける。
 * - 範囲の外側に 1 点ずつ足し、線が画面の端まで届くようにする。
 * - 点が区間の数の 4 倍以下なら、範囲の点をすべて返す（拡大すれば全点を描く）。
 *
 * 返す点は [元の番号, 値]。番号は狭義の増加順。
 */
export type DecimatedPoint = [number, number | null];

export function decimateRange(
  values: ArrayLike<number | null>,
  start: number,
  end: number,
  buckets: number,
): DecimatedPoint[] {
  const n = values.length;
  const from = Math.max(0, Math.min(n, Math.floor(start) - 1));
  const to = Math.max(from, Math.min(n, Math.ceil(end) + 1));
  const out: DecimatedPoint[] = [];
  const count = to - from;
  const b = Math.max(1, Math.floor(buckets));
  if (count <= b * 4) {
    for (let i = from; i < to; i++) out.push([i, values[i] ?? null]);
    return out;
  }
  let last = -1;
  const push = (i: number) => {
    if (i <= last) return;
    out.push([i, values[i] ?? null]);
    last = i;
  };
  const picks: number[] = [];
  for (let k = 0; k < b; k++) {
    const s = from + Math.floor((k * count) / b);
    const e = from + Math.floor(((k + 1) * count) / b);
    if (e <= s) continue;
    picks.length = 0;
    picks.push(s);
    let min = -1, max = -1;
    let prevNull = values[s] == null;
    for (let i = s; i < e; i++) {
      const v = values[i];
      const isNull = v == null;
      if (isNull !== prevNull) {
        // null の境目は両側を残す
        picks.push(i - 1, i);
        prevNull = isNull;
      }
      if (isNull) continue;
      if (min < 0 || (v as number) < (values[min] as number)) min = i;
      if (max < 0 || (v as number) > (values[max] as number)) max = i;
    }
    if (min >= 0) picks.push(min);
    if (max >= 0) picks.push(max);
    picks.push(e - 1);
    picks.sort((x, y) => x - y);
    for (const i of picks) push(i);
  }
  return out;
}
