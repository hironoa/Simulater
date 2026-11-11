import { useShallow } from "zustand/react/shallow";
import { useWaveformStore } from "../../../store/useWaveformStore";

export function CompareBadge({ signal }: { signal: string }) {
  const { enabled, summary } = useWaveformStore(useShallow(s => ({
    enabled: s.compareEnabled && !!s.compareChartData, summary: s.diffSummary[signal],
  })));
  if (!enabled || !summary) return null;
  const badge = "text-[10px] px-1 rounded whitespace-nowrap";
  if (summary.presence !== "both") return <span className={`${badge} text-gray-500 dark:text-gray-400 border border-gray-300 dark:border-slate-600`}>{summary.presence === "aOnly" ? "A のみ" : "B のみ"}</span>;
  return <span className="flex gap-1">
    {summary.valueCount > 0 && <span className={`${badge} bg-red-100 text-red-700 dark:bg-red-950 dark:text-red-300`}>≠{summary.valueCount}</span>}
    {summary.timingCount > 0 && <span className={`${badge} bg-amber-100 text-amber-800 dark:bg-amber-950 dark:text-amber-300`}>⏱{summary.timingCount}</span>}
    {!summary.valueCount && !summary.timingCount && <span className={`${badge} bg-green-100 text-green-700 dark:bg-green-950 dark:text-green-300`}>=</span>}
  </span>;
}

export function CompareStatus() {
  const { enabled, diffs, summary, duration } = useWaveformStore(useShallow(s => ({
    enabled: s.compareEnabled && !!s.compareChartData, diffs: s.diffs, duration: s.totalValueDurationMs,
    summary: s.diffSummary,
  })));
  if (!enabled) return null;
  const timingCount = Object.values(summary).reduce((count, signal) => count + signal.timingCount, 0);
  // A timing pair can span several intervals when interrupted by a value difference.
  const timings = [...new Map(diffs.filter(d => d.type === "timing")
    .map(d => [JSON.stringify([d.signal, d.tA, d.tB]), d])).values()];
  const shift = timings[0]?.shiftMs;
  const uniform = timings.length > 0 && timings.every(d => d.shiftMs === shift);
  return <div className="flex-shrink-0 p-3 border-t border-gray-200 dark:border-slate-700 text-[11px] text-gray-600 dark:text-gray-300 space-y-1">
    <div className="font-bold">比較の結果</div>
    <div className="text-red-700 dark:text-red-300">値の違い {diffs.filter(d => d.type === "value").length} 区間 / {Number(duration.toPrecision(7))} ms</div>
    <div className="text-amber-800 dark:text-amber-300">タイミングのずれ {timingCount} 件{uniform && `（すべて ${Number(Math.abs(shift!).toPrecision(7))} ms ${shift! > 0 ? "遅れ" : "早い"}）`}</div>
    <div>判定: 生値 / 許容誤差 0 / 無効区間は除外</div>
  </div>;
}
