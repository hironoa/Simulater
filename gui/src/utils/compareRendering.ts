import type * as echarts from "echarts";
import type { ChartData, WaveformDiff, SignalDiffSummary } from "../types";
import { getDarkerColor, toPhysical } from "./helpers";
import { getStateColor } from "./stateNames";
import { fitLabel } from "./fitLabel";
import type { InactiveResult } from "./inactiveRanges";

export interface CompareRendering {
  data: ChartData;
  summary?: Record<string, SignalDiffSummary>;
  diffs: WaveformDiff[];
  diffFilter: "all" | "value";
  focusedDiffIndex: number;
  inactive?: InactiveResult;
}

type Rect = { x: number; y: number; width: number; height: number };
type Sample = { time: number; index: number; raw: number | null; value: number | null };
const FONT_MONO = '"BIZ UDGothic", "JetBrains Mono", monospace';

export interface CompareDeltaTrack {
  top: number;
  height: number;
  segments: { start: number; end: number; delta: number }[];
}

/** Shared pixel scale for the delta waveform and its hover point, including zoom. */
export function compareDeltaYScale(track: CompareDeltaTrack, chartHeight: number,
  xAt: (index: number) => number, left: number, right: number) {
  let maxAbs = 0;
  for (const s of track.segments) if (xAt(s.end) >= left && xAt(s.start) <= right) {
    maxAbs = Math.max(maxAbs, Math.abs(s.delta));
  }
  const top = (track.top + track.height / 12) * chartHeight;
  const height = track.height * 5 / 6 * chartHeight;
  return (value: number) => top + height / 2 - value / (maxAbs || 1) * height / 2;
}

/** Comparison primitives live in the option, so getOption()/image export retain them.
 * Values are physical (or the existing raw display mode); only X coordinates are
 * interpolated. Y coordinates, including axis breaks, always go through ECharts.
 */
export function addCompareRendering(
  option: { grid: any[]; xAxis: any[]; yAxis: any[]; series: echarts.SeriesOption[] },
  b: ChartData,
  comparison: CompareRendering,
  signals: string[],
  settings: any,
  modes: Record<string, Record<number, string>>,
  conversion: (signal: string) => { lsb: number; offset: number },
  showMode: "off" | "bg-only" | "full",
  theme: "light" | "dark",
  containerHeight: number,
  taskPeriod: number,
) {
  if (!b.steps.length || containerHeight <= 0) return;
  const a = comparison.data;
  const filtered = comparison.diffs.filter(d => comparison.diffFilter === "all" || d.type === "value");
  const focused = filtered[comparison.focusedDiffIndex];
  const stateSignal = (sig: string) => Object.values(modes[sig] ?? {}).some(name => name.trim() !== "");
  const baseSeries = new Map(signals.map(sig => [sig,
    option.series.find(s => s.name === sig) as echarts.LineSeriesOption | undefined]));
  const lanes = new Map<number, string[]>();
  for (const sig of signals) {
    const series = baseSeries.get(sig);
    if (!series) continue;
    const grid = option.yAxis[series.yAxisIndex as number].gridIndex as number;
    if (!lanes.has(grid)) lanes.set(grid, []);
    lanes.get(grid)!.push(sig);
  }
  const percent = (px: number) => `${px / containerHeight * 100}%`;
  const pixels = (value: string | number) => typeof value === "string"
    ? parseFloat(value) * containerHeight / 100 : value;
  const timesB = b.steps.map(step => step * taskPeriod);
  // Diff times use step * period; the existing axis labels use (step - 1) * period.
  // Working in B indices preserves that existing axis/cursor contract.
  const indexAt = (time: number) => {
    if (timesB.length === 1) return 0;
    let lo = 0, hi = timesB.length - 1;
    while (lo + 1 < hi) {
      const mid = (lo + hi) >>> 1;
      if (timesB[mid] <= time) lo = mid; else hi = mid;
    }
    return lo + (time - timesB[lo]) / (timesB[hi] - timesB[lo]);
  };
  const samples = (data: ChartData, sig: string): Sample[] => {
    let raw: number | null = null;
    const conv = conversion(sig);
    return data.steps.map((step, i) => {
      if (i >= (data.sourceSampleCount ?? data.steps.length)) raw = null;
      if (data.seriesMap[sig]?.[i] != null) raw = data.seriesMap[sig][i];
      const time = step * taskPeriod;
      return { time, index: indexAt(time), raw, value: raw === null ? null : toPhysical(raw, conv) };
    });
  };
  const text = (x: number, y: number, label: string, fill = "#94a3b8", align = "center"): any => ({
    type: "text", style: { x, y, text: label, fill, fontSize: 10, align, verticalAlign: "middle" },
  });
  const line = (x1: number, y1: number, x2: number, y2: number, style: any): any => ({
    type: "line", shape: { x1, y1, x2, y2 }, style,
  });
  const polygon = (points: number[][], fill: string): any => ({
    type: "polygon", shape: { points }, style: { fill, stroke: null },
  });
  const custom = (name: string, xAxisIndex: number, yAxisIndex: number, z: number,
    render: (rect: Rect, api: echarts.CustomSeriesRenderItemAPI) => any,
    compareDeltaTrack?: CompareDeltaTrack) => {
    option.series.push({
      name, type: "custom", xAxisIndex, yAxisIndex, z, silent: true,
      animation: false, clip: false, encode: { x: -1, y: -1 }, data: [0],
      // Fractions of chart height keep hover placement aligned with this row.
      ...(compareDeltaTrack ? { compareDeltaTrack } : {}),
      renderItem: (params, api) => render(params.coordSys as unknown as Rect, api),
    });
  };
  // Ordinal axes round fractional indices. Interpolate their pixel positions instead.
  const xAt = (api: echarts.CustomSeriesRenderItemAPI, index: number) => {
    if (b.steps.length === 1) return api.coord([0, 0])[0];
    const lo = Math.max(0, Math.min(b.steps.length - 2, Math.floor(index)));
    const left = api.coord([lo, 0])[0], right = api.coord([lo + 1, 0])[0];
    return left + (right - left) * (index - lo);
  };
  // Empty groups must replace old children too: ECharts otherwise retains the
  // previous viewport's primitives when every item has moved out of view.
  const clipped = (rect: Rect, children: any[]): any => ({
    type: "group", $mergeChildren: false, clipPath: { type: "rect", shape: rect }, children,
  });

  for (const [gridIndex, group] of lanes) {
    const grid = option.grid[gridIndex];
    const top = pixels(grid.top), fullHeight = pixels(grid.height);
    const rows = new Map<string, { delta: number; timing: number }>();
    let reserved = 0;
    for (const sig of group) {
      const diffs = filtered.filter(d => d.signal === sig);
      const delta = !stateSignal(sig) && diffs.some(d => d.type === "value") ? 24 : 0;
      const timing = diffs.some(d => d.type === "timing") ? 28 : 0;
      rows.set(sig, { delta, timing });
      reserved += delta + timing;
    }
    // Keep a waveform area even with many overlapping signals in a small viewport.
    const rowScale = reserved ? Math.min(1, Math.max(0, fullHeight - 16) / reserved) : 1;
    const height = Math.max(1, fullHeight - reserved * rowScale);
    grid.height = percent(height);
    // The last time axis stays below the auxiliary tracks.
    const base = baseSeries.get(group[0])!;
    const xIndex = base.xAxisIndex as number;
    if (option.xAxis[xIndex].axisLabel?.show) {
      option.xAxis[xIndex].axisLabel = { ...option.xAxis[xIndex].axisLabel, margin: reserved * rowScale + 8 };
    }
    let rowTop = top + height;
    for (const sig of group) {
      const series = baseSeries.get(sig)!;
      const yIndex = series.yAxisIndex as number;
      const diffs = filtered.filter(d => d.signal === sig);
      const state = stateSignal(sig);
      const pointsA = a.seriesMap[sig] ? samples(a, sig) : [];
      const pointsB = samples(b, sig);
      const hiddenA = new Set<number>();
      if (comparison.inactive?.show && !comparison.inactive.conditions.has(sig)) {
        for (const [start, end] of comparison.inactive.union)
          for (let i = start; i < end; i++) hiddenA.add(i);
      }
      const { delta, timing } = rows.get(sig)!;
      if (delta || timing) option.yAxis[yIndex].axisLabel = {
        ...option.yAxis[yIndex].axisLabel, showMinLabel: false,
      };
      const deltaTop = rowTop, timingTop = rowTop + delta * rowScale;
      rowTop += (delta + timing) * rowScale;
      let aX = xIndex, aY = yIndex;
      if (state && pointsA.length) {
        // Separate grids give both rows the same real Y scale and native breaks.
        const rowGap = Math.min(3, height / 3);
        const rowHeight = (height - rowGap) / 2;
        const bTop = top + (height + rowGap) / 2;
        const cloneRow = (rowY: number) => {
          const gi = option.grid.length, xi = option.xAxis.length, yi = option.yAxis.length;
          option.grid.push({ ...grid, top: percent(rowY), height: percent(rowHeight) });
          option.xAxis.push({ ...option.xAxis[xIndex], gridIndex: gi, show: false });
          option.yAxis.push({ ...option.yAxis[yIndex], gridIndex: gi, show: false });
          return [xi, yi];
        };
        [aX, aY] = cloneRow(top);
        const [bX, bY] = cloneRow(bTop);
        for (const item of option.series) {
          if ("yAxisIndex" in item && item.yAxisIndex === yIndex) {
            item.xAxisIndex = bX;
            item.yAxisIndex = bY;
          }
        }
        option.yAxis[yIndex].show = false;
        custom(sig + "_compare_row_names", xIndex, yIndex, 16, rect => ({
          type: "group", $mergeChildren: false, children: [text(rect.x - 6, top + rowHeight / 2, "A", undefined, "right"),
            text(rect.x - 6, bTop + rowHeight / 2, "B", undefined, "right")].map(label => ({
              ...label, style: { ...label.style, fontFamily: FONT_MONO },
            })),
        }));
      }
      if (pointsA.length) {
        custom(sig + "_compare_a", aX, aY, 10, (rect, api) => {
          const bands: any[] = [], lines: any[] = [], labels: any[] = [];
          let segmentStart = 0;
          for (let i = 0; i < pointsA.length - 1; i++) {
            const p = pointsA[i], next = pointsA[i + 1];
            const opacity = (state ? 0.6 : 0.45) * (hiddenA.has(i) ? 0.3 : 1);
            const x = xAt(api, p.index), endX = xAt(api, next.index);
            if (p.value !== null && endX >= rect.x && x <= rect.x + rect.width) {
              const y = api.coord([0, p.value])[1];
              lines.push(line(x, y, endX, y, { stroke: settings[sig].color,
                lineWidth: state ? 1.2 : 2.4, opacity,
                ...(state ? {} : { lineDash: [6, 4] }) }));
              if (next.value !== null) lines.push(line(endX, y, endX, api.coord([0, next.value])[1], {
                stroke: settings[sig].color, lineWidth: state ? 1.2 : 2.4,
                opacity, ...(state ? {} : { lineDash: [6, 4] }),
              }));
            }
            if (next.raw !== p.raw || hiddenA.has(i) !== hiddenA.has(i + 1) || i === pointsA.length - 2) {
              const startX = Math.max(rect.x, xAt(api, pointsA[segmentStart].index));
              const right = Math.min(rect.x + rect.width, endX);
              const name = p.raw === null ? undefined : modes[sig]?.[p.raw];
              if (state && showMode !== "off" && name !== undefined && !hiddenA.has(i) && right > startX) {
                const color = getStateColor(p.raw!);
                bands.push({ type: "rect", shape: { x: startX, y: rect.y, width: right - startX, height: rect.height },
                  style: { fill: color, opacity: theme === "dark" ? 0.3 : 0.15 } });
                const fitted = showMode === "full" ? fitLabel(name, right - startX - 8, rect.height - 4) : null;
                if (fitted) labels.push({ type: "text", style: {
                  x: (startX + right) / 2, y: rect.y + rect.height / 2,
                  text: fitted.text, fill: theme === "dark" ? color : getDarkerColor(color, 0.6),
                  align: "center", verticalAlign: "middle", fontSize: fitted.fontSize,
                  lineHeight: fitted.lineHeight, fontWeight: 600,
                  fontFamily: FONT_MONO,
                  stroke: theme === "dark" ? "transparent" : "#ffffff", lineWidth: 2,
                } });
              }
              segmentStart = i + 1;
            }
          }
          return clipped(rect, [...bands, ...lines, ...labels]);
        });
      }
      if (!state && pointsA.length && b.seriesMap[sig] &&
        (!comparison.summary?.[sig] || comparison.summary[sig].presence === "both")) {
        custom(sig + "_compare_end_labels", xIndex, yIndex, 16, (rect, api) => {
          const right = rect.x + rect.width;
          const labelHeight = Math.min(14, rect.height / 2);
          if (labelHeight < 8 || rect.width < 32) return clipped(rect, []);
          const clampY = (y: number) => Math.max(rect.y + labelHeight / 2,
            Math.min(rect.y + rect.height - labelHeight / 2, y));
          const labels: { name: string; y: number; opacity: number }[] = [];
          for (const [name, points, count] of [
            ["A", pointsA, a.sourceSampleCount ?? pointsA.length],
            ["B", pointsB, b.sourceSampleCount ?? pointsB.length],
          ] as const) {
            // Use the step value at the current viewport edge, without extending
            // either recording beyond its last sample. Coordinates update on zoom/pan.
            const end = Math.min(count, points.length);
            if (!end || right < xAt(api, points[0].index) - 0.01 ||
              right > xAt(api, points[end - 1].index) + 0.01) continue;
            let lo = 0, hi = end;
            while (lo < hi) {
              const mid = (lo + hi) >>> 1;
              if (xAt(api, points[mid].index) <= right + 0.01) lo = mid + 1;
              else hi = mid;
            }
            const point = points[lo - 1];
            if (point?.value == null || !Number.isFinite(point.value)) continue;
            const y = api.coord([0, point.value])[1];
            if (!Number.isFinite(y) || y < rect.y || y > rect.y + rect.height) continue;
            labels.push({ name, y: clampY(y),
              opacity: name === "A" ? 0.45 * (hiddenA.has(lo - 1) ? 0.3 : 1) : 1 });
          }
          if (labels.length === 2) {
            labels.sort((left, right) => left.y - right.y);
            const gap = Math.min(labelHeight + 2, rect.height - labelHeight);
            if (labels[1].y - labels[0].y < gap) {
              const upper = Math.max(rect.y + labelHeight / 2,
                Math.min(rect.y + rect.height - labelHeight / 2 - gap,
                  (labels[0].y + labels[1].y - gap) / 2));
              labels[0].y = upper;
              labels[1].y = upper + gap;
            }
          }
          // Keep these compact tags inside the waveform, above the delta track
          // and inset from the edge where cursor labels can extend outside it.
          return clipped(rect, labels.map(label => ({
            type: "text", style: {
              x: right - 16, y: label.y, text: label.name,
              fontFamily: FONT_MONO, fontSize: Math.min(10, labelHeight - 2), fontWeight: 600,
              align: "center", verticalAlign: "middle",
              fill: settings[sig].color, opacity: label.opacity,
              backgroundColor: theme === "dark" ? "#0f172a" : "#ffffff",
              borderColor: settings[sig].color, borderWidth: 1, borderRadius: 2,
              padding: [1, 3],
            },
          })));
        });
      }
      // Pair only exact common timestamps; never extrapolate missing samples.
      const aByTime = new Map(pointsA.map(p => [p.time, p]));
      const valueDiffs = diffs.filter(d => d.type === "value");
      const segments: { start: number; end: number; a: number; b: number; delta: number }[] = [];
      let diffCursor = 0;
      if (!state) for (let i = 0; i < pointsB.length - 1; i++) {
        const p = pointsB[i], pa = aByTime.get(p.time), next = pointsB[i + 1];
        if (pa?.value == null || p.value == null || !aByTime.has(next.time)) continue;
        while (diffCursor < valueDiffs.length && valueDiffs[diffCursor].end <= p.time) diffCursor++;
        for (let j = diffCursor; j < valueDiffs.length && valueDiffs[j].start < next.time; j++) {
          const diff = valueDiffs[j];
          const start = Math.max(p.time, diff.start), end = Math.min(next.time, diff.end);
          if (end > start) segments.push({ start: indexAt(start), end: indexAt(end),
            a: pa.value, b: p.value, delta: p.value - pa.value });
        }
      }
      if (segments.length) custom(sig + "_compare_value_fill", xIndex, yIndex, 8, (rect, api) => {
        const children = segments.map(s => {
          const left = xAt(api, s.start), right = xAt(api, s.end);
          const ya = api.coord([0, s.a])[1], yb = api.coord([0, s.b])[1];
          return polygon([[left, ya], [right, ya], [right, yb], [left, yb]], "rgba(239,68,68,0.35)");
        });
        return clipped(rect, children);
      });
      const deltaTrack: CompareDeltaTrack = { top: deltaTop / containerHeight,
        height: delta * rowScale / containerHeight,
        segments: segments.map(({ start, end, delta }) => ({ start, end, delta })) };
      if (delta) custom(sig + "_compare_delta", xIndex, yIndex, 14, (rect, api) => {
        const trackHeight = 20 * rowScale, trackTop = deltaTop + 2 * rowScale;
        const yOf = compareDeltaYScale(deltaTrack, containerHeight, index => xAt(api, index), rect.x, rect.x + rect.width);
        const zero = yOf(0);
        const children: any[] = [line(rect.x, zero, rect.x + rect.width, zero, { stroke: "#94a3b8", lineWidth: 0.6 })];
        for (const s of segments) {
          const left = Math.max(rect.x, xAt(api, s.start)), right = Math.min(rect.x + rect.width, xAt(api, s.end));
          if (right <= left) continue;
          const y = yOf(s.delta);
          children.push(polygon([[left, zero], [left, y], [right, y], [right, zero]], "rgba(239,68,68,0.45)"));
          children.push(line(left, y, right, y, { stroke: "#ef4444", lineWidth: 1 }));
        }
        // Beside the axis like the A/B row names, clear of the zero line.
        const label = text(rect.x - 6, trackTop + trackHeight / 2, "Δ", undefined, "right");
        children.push({ ...label, style: { ...label.style, fontFamily: FONT_MONO } });
        return { type: "group", $mergeChildren: false, children };
      }, deltaTrack);
      custom(sig + "_compare_annotations", xIndex, yIndex, 14, (rect, api) => {
        const children: any[] = [];
        const drawnTimingPairs = new Set<string>();
        for (const diff of diffs) {
          const x1 = xAt(api, diff.startIndex), x2 = xAt(api, diff.endIndex);
          const left = Math.max(rect.x, x1), right = Math.min(rect.x + rect.width, x2);
          if (right <= left) continue;
          if (state && pointsA.length) {
            const hatch: any[] = [];
            for (let x = left - height; x < right; x += 6) hatch.push(line(x, top + height, x + height, top, {
              stroke: "#ef4444", lineWidth: 1, opacity: 0.3,
            }));
            children.push(clipped({ x: left, y: top, width: right - left, height }, hatch));
            children.push({ type: "rect", shape: { x: x1, y: top, width: x2 - x1, height },
              style: { fill: null, stroke: "#f87171", lineWidth: 0.8, opacity: 0.6 } });
          }
          const timingKey = diff.type === "timing" ? JSON.stringify([diff.tA, diff.tB]) : "";
          if (diff.type === "timing" && !drawnTimingPairs.has(timingKey)) {
            drawnTimingPairs.add(timingKey);
            const xa = xAt(api, indexAt(diff.tA)), xb = xAt(api, indexAt(diff.tB));
            const y = timingTop + 8 * rowScale;
            for (const x of [xa, xb]) {
              if (x >= rect.x && x <= rect.x + rect.width) children.push(line(x, top + height, x, y + 4 * rowScale,
                { stroke: "#d97706", lineWidth: 1, lineDash: [3, 2] }));
            }
            const timingLeft = Math.min(xa, xb), timingRight = Math.max(xa, xb);
            const visibleLeft = Math.max(rect.x, timingLeft), visibleRight = Math.min(rect.x + rect.width, timingRight);
            children.push(line(visibleLeft, y, visibleRight, y, { stroke: "#b45309", lineWidth: 1.2 }));
            if (timingLeft >= rect.x) children.push(polygon([[timingLeft, y], [timingLeft + 4, y - 3], [timingLeft + 4, y + 3]], "#b45309"));
            if (timingRight <= rect.x + rect.width) children.push(polygon([[timingRight, y], [timingRight - 4, y - 3], [timingRight - 4, y + 3]], "#b45309"));
            children.push(text((xa + xb) / 2, y + 13 * rowScale,
              `${Number(Math.abs(diff.shiftMs).toPrecision(7))} ms ${diff.shiftMs > 0 ? "遅れ" : "早い"}`, theme === "dark" ? "#fbbf24" : "#92400e"));
          }
          if (diff === focused) children.push({ type: "rect",
            shape: { x: left, y: top - 2, width: Math.max(2, right - left), height: Math.max(top + height, timingTop + timing * rowScale) - top + 4 },
            style: { fill: null, stroke: theme === "dark" ? "#e2e8f0" : "#1e293b", lineWidth: 1.5, lineDash: [4, 2] } });
        }
        return clipped({ x: rect.x, y: top - 2, width: rect.width,
          height: Math.max(top + height, timingTop + timing * rowScale) - top + 4 }, children);
      });
    }
  }
  const firstSeries = baseSeries.get(signals[0]);
  // Draw each diff range inside the slider (bottom: 5, height: 16), which always
  // shows the whole recording, so the bands stay aligned while zooming. They fill
  // the data area below the 7px move handle, and z 3 keeps the slider (z 4) on top.
  if (firstSeries) custom("_compare_zoom_marks", firstSeries.xAxisIndex as number, firstSeries.yAxisIndex as number, 3, rect => {
    const last = b.steps.length - 1;
    if (!last) return { type: "group", $mergeChildren: false, children: [] };
    const xOf = (index: number) => rect.x + Math.max(0, Math.min(1, index / last)) * rect.width;
    return { type: "group", $mergeChildren: false, children: filtered.map(diff => {
      const left = xOf(diff.startIndex), width = Math.max(2, xOf(diff.endIndex) - left);
      return { type: "rect", shape: { x: Math.min(left, rect.x + rect.width - width),
        y: containerHeight - 14, width, height: 14 },
      style: { fill: diff.type === "value" ? "#ef4444" : "#f59e0b", opacity: 0.55 } };
    }) };
  });

}
