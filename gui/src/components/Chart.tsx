import { activeDisplaySignals } from "../utils/displayBlocks";
import { displaySignalName } from "../utils/displaySignalName";
import { useCompareView } from "../hooks/useCompareView";
import { compareSample, compareInactive, compareDisplaySteps } from "../utils/compareView";
import { compareDeltaYScale, type CompareDeltaTrack } from "../utils/compareRendering";
import { buildChartOptions } from "../utils/chartOptionsBuilder";
import React, { useEffect, useRef, useState, useMemo, useCallback } from "react";
import * as echarts from "echarts";
import { useWaveformStore } from "../store/useWaveformStore";
import { useShallow } from "zustand/react/shallow";
import {
  calculateAutoYRange,
  getDarkerColor,
  colorPalette,
  detectUndefinedValue,
  computeAutoBreak,
  getDisplayConversion, getDisplayUnit, toPhysical, formatPhysical,
} from "../utils/helpers";
import { YBreak } from "../types";
import { Camera } from "lucide-react";
import toast from "react-hot-toast";
import { copyChartAndTableToClipboard } from "../utils/exportImage";
import { computeInactive, inactiveLabel, InactiveResult } from "../utils/inactiveRanges";
import { decimateRange, overviewValues } from "../utils/decimate";

/**
 * ECharts は渡した点の数に比例して重くなる（15 信号 × 60,000 点で、ホバー 1 回の描き直しに 3 秒前後）。
 * カテゴリ軸と同じ長さの折れ線は、見えている範囲を画面の幅ほどに絞った点（[番号, 値]）だけを渡し、
 * 拡大・スクロールのたびに作り直す。全点は fullSeries に持ち、ホバーの値はそちらから読む。
 * この点数より少なければ、従来どおり全点を渡す。
 */
const DECIMATE_MIN_POINTS = 8000;

type FullSeries = Map<number, (number | null)[]>;

function isPlainValues(data: unknown[]): boolean {
  for (const v of data) {
    if (v === null || v === undefined) continue;
    return typeof v === "number";
  }
  return true;
}

/** 絞る対象の系列（番号 → 全点）。option.series の data はまだ全点のまま。 */
function collectFullSeries(option: any): FullSeries {
  const full: FullSeries = new Map();
  const count = option?.xAxis?.[0]?.data?.length ?? 0;
  if (count < DECIMATE_MIN_POINTS) return full;
  (option.series ?? []).forEach((series: any, i: number) => {
    if (series?.type === "line" && Array.isArray(series.data) && series.data.length === count && isPlainValues(series.data)) {
      full.set(i, series.data);
    }
  });
  return full;
}

const DATA_SHADOW_TYPES = ["line", "bar", "scatter", "candlestick"];

/**
 * ズームバーの背景（データシャドウ）は、ECharts が x 軸 0 の最初の系列の元データから描く。その系列を表示範囲に
 * 絞ると背景まで表示範囲の波形になるので、全期間を絞った見えない系列を先頭に足して、背景はそれから描かせる。
 * 系列の番号が 1 つずれるので、番号で系列を指す visualMap と、絞る対象の番号も 1 つずらす。
 */
function addZoomShadowSeries(option: any, full: FullSeries, buckets: number): FullSeries {
  const source = option.series.findIndex((s: any) => (s.xAxisIndex ?? 0) === 0 && DATA_SHADOW_TYPES.includes(s.type));
  const data = source >= 0 ? full.get(source) : undefined;
  if (!data) return full;
  option.series.unshift({
    name: "_zoom_shadow",
    type: "line",
    xAxisIndex: 0,
    yAxisIndex: option.series[source].yAxisIndex,
    data: overviewValues(data, buckets),
    silent: true,
    animation: false,
    symbol: "none",
    showSymbol: false,
    lineStyle: { width: 0, opacity: 0 },
    itemStyle: { opacity: 0 },
    tooltip: { show: false },
    emphasis: { disabled: true },
  });
  for (const vm of option.visualMap ?? []) {
    if (typeof vm.seriesIndex === "number") vm.seriesIndex += 1;
  }
  return new Map([...full].map(([i, values]) => [i + 1, values]));
}

function decimateBuckets(chart: echarts.ECharts) {
  return Math.max(200, Math.round(chart.getWidth()));
}

/**
 * getOption() は設定全体（各 x 軸の目盛りの文字を含む）を毎回複製するので、長い記録ではホバーのたびに
 * 数十〜百 ms かかる。設定か表示範囲が変わるまで、同じ結果を返す。
 * 結果は読むだけにすること（画像出力の一時的な書き換えは、表示の処理が読む項目に触れない）。
 */
function cacheGetOption(chart: echarts.ECharts) {
  const c = chart as any;
  const get = c.getOption.bind(chart);
  let cached: unknown = null;
  const clear = () => { cached = null; };
  for (const name of ["setOption", "dispatchAction", "resize", "clear"]) {
    const call = c[name].bind(chart);
    c[name] = (...args: unknown[]) => { cached = null; return call(...args); };
  }
  // 内側・スライダーの操作による拡大は dispatchAction を通らないので、イベントで捨てる
  chart.on("datazoom", clear);
  c.getOption = () => (cached ??= get());
}

/** いま見えているカテゴリの範囲 [start, end)。 */
function visibleCategoryRange(chart: echarts.ECharts, count: number): [number, number] {
  const axis = (chart as any).getModel?.()?.getComponent?.("xAxis", 0)?.axis;
  const extent = axis?.scale?.getExtent?.() as number[] | undefined;
  if (!extent || !extent.every(Number.isFinite)) return [0, count];
  return [Math.max(0, Math.floor(extent[0])), Math.min(count, Math.ceil(extent[1]) + 1)];
}

export type LabelInfo = { id: string, orgX: number, orgY: number, targetY: number, y: number, text: string, color: string, isDark: boolean, dashed?: boolean, dot?: boolean, alwaysShow?: boolean, noDot?: boolean, fixedY?: boolean };

export function renderOverlayLabels(
  container: HTMLElement, 
  labelGroups: Record<number, LabelInfo[]>, 
  renderDots: boolean = false, 
  showLabels: boolean = true,
  cursors: any[] = [],
  chartInstance: echarts.ECharts | null = null,
  theme: string = 'light',
  isHoverCursor: boolean = false,
  showMarkersSetting: boolean = true
) {
  const LABEL_HEIGHT = 14; 
  for (const xStr in labelGroups) {
     // Auxiliary-track badges must not be pushed down into the next lane/time axis.
     const group = labelGroups[xStr].filter(label => !label.fixedY);
     for (const label of labelGroups[xStr]) if (label.fixedY) label.y = label.targetY;
     group.sort((a, b) => a.targetY - b.targetY);
     for (let i = 0; i < group.length; i++) {
        if (i === 0) {
           group[i].y = group[i].targetY;
        } else {
           if (group[i].targetY < group[i - 1].y + LABEL_HEIGHT) {
              group[i].y = group[i - 1].y + LABEL_HEIGHT;
           } else {
              group[i].y = group[i].targetY;
           }
        }
     }
  }

  let svgStr = `<svg style="position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; z-index: 49;">`;
  let htmlStr = '';

  const isDarkTheme = theme === 'dark';
  
  let gridLeft = 0;
  let gridRight = 99999;
  if (chartInstance) {
    const opt = chartInstance.getOption() as any;
    gridLeft = parseInt(opt.grid?.[0]?.left || "50");
    gridRight = chartInstance.getWidth() - parseInt(opt.grid?.[0]?.right || "15");
  }

  // クリップパスの定義とグループ開始
  svgStr += `
    <defs>
      <clipPath id="grid-clip">
        <rect x="${gridLeft}" y="0" width="${Math.max(0, gridRight - gridLeft)}" height="100%" />
      </clipPath>
    </defs>
    <g clip-path="url(#grid-clip)">
  `;

  // カーソルマーカーの描画
  if (chartInstance && cursors.length > 0 && !isHoverCursor && showMarkersSetting) {
    const gridObj = chartInstance.getOption() as any;
    const xAxisData = gridObj?.xAxis?.[0]?.data;
    
    if (xAxisData) {
      const cursorPxList: { idx: number, x: number, time: number }[] = [];
      cursors.forEach((c, idx) => {
        const strX = xAxisData[c.index];
        if (strX !== undefined) {
          // X座標を取得。Yは適当な値（ここでは0）で一旦取得し要素から抽出
          const pxPos = chartInstance.convertToPixel({ xAxisIndex: 0 }, c.index);
          const pxPosX = chartInstance.convertToPixel({ seriesIndex: 0 }, [strX, 0]);
          // convertToPixel({ xAxisIndex: 0 }, c.index)はX軸の座標のみを返すか、[x,y]か
          const rx = Array.isArray(pxPosX) ? pxPosX[0] : (typeof pxPos === 'number' ? pxPos : (Array.isArray(pxPos) ? pxPos[0] : null));

          if (rx !== null && !isNaN(rx)) {
            const x = Math.round(rx);
            cursorPxList.push({ idx, x, time: c.time });
          }
        }
      });

      // マーカー縦線とラベル描画
      cursorPxList.forEach(c => {
        if (c.x >= gridLeft && c.x <= gridRight) {
           const lineColor = isDarkTheme ? "#e2e8f0" : "#666";
           const lineOpacity = isDarkTheme ? 1 : 0.8;
              
           // 縦線
           svgStr += `<line x1="${c.x}" y1="15" x2="${c.x}" y2="100%" stroke="${lineColor}" stroke-dasharray="4,4" stroke-width="1.5" opacity="${lineOpacity}" />`;
              
           // C1, C2 ラベル
           const lblBg = isDarkTheme ? "rgba(71, 85, 105, 0.8)" : "rgba(85, 85, 85, 0.8)";
           const style = `
             position: absolute;
             left: ${c.x}px;
             top: 15px;
             transform: translate(-50%, -100%);
             font-family: var(--font-mono, monospace);
             font-size: 10px;
             font-weight: bold;
             color: #fff;
             background-color: ${lblBg};
             padding: 0px 4px;
             border-radius: 3px;
             pointer-events: none;
             white-space: nowrap;
             z-index: 50;
             line-height: 1.2;
           `;
           htmlStr += `<div style="${style.replace(/\n/g, '')}">C${c.idx + 1}</div>`;
        }
      });

      // 矢印と時間のラベル
      for (let i = 1; i < cursorPxList.length; i++) {
        const c1 = cursorPxList[i - 1];
        const c2 = cursorPxList[i];
        
        // どちらも画面外かつ、画面をまたがない場合は描画しない
        if (Math.max(c1.x, c2.x) < gridLeft || Math.min(c1.x, c2.x) > gridRight) {
          continue;
        }

        const dt = Number((c2.time - c1.time).toPrecision(7));
        const y = 25;
        const color = isDarkTheme ? "#e2e8f0" : "#666";
        const lblColor = isDarkTheme ? "#f87171" : "#b71c1c";

        svgStr += `
          <defs>
            <marker id="arrow-start-${c1.idx}" markerWidth="8" markerHeight="8" refX="1" refY="4" orient="0">
              <polyline points="6,1.5 1,4 6,6.5" fill="none" stroke="${color}" stroke-width="1" stroke-linecap="round" stroke-linejoin="round" />
            </marker>
            <marker id="arrow-end-${c1.idx}" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="0">
              <polyline points="2,1.5 7,4 2,6.5" fill="none" stroke="${color}" stroke-width="1" stroke-linecap="round" stroke-linejoin="round" />
            </marker>
          </defs>
          <line x1="${c1.x}" y1="${y}" x2="${c2.x}" y2="${y}" stroke="${color}" stroke-width="1" stroke-dasharray="3,3" marker-start="url(#arrow-start-${c1.idx})" marker-end="url(#arrow-end-${c1.idx})" />
        `;

        const centerX = Math.round((c1.x + c2.x) / 2);
        const textStr = `${dt} ms`;
        // フォントサイズ9pxのmonospaceで1文字約5.5px
        const estimatedWidth = textStr.length * 5.5;
        const halfWidth = estimatedWidth / 2;

        if (centerX - halfWidth >= gridLeft && centerX + halfWidth <= gridRight) {
          const timeBg = isDarkTheme ? "#100C2A" : "rgba(255, 255, 255, 0.9)";
          const style = `
            position: absolute;
            left: ${centerX}px;
            top: ${y - 8}px;
            font-family: var(--font-mono, monospace);
            font-size: 9px;
            color: ${lblColor};
            background-color: ${timeBg};
            padding: 0px 3px;
            border-radius: 2px;
            pointer-events: none;
            transform: translate(-50%, -50%);
            z-index: 50;
          `;
          htmlStr += `<div style="${style.replace(/\n/g, '')}">${dt} ms</div>`;
        }
      }
    }
  }

  for (const xStr in labelGroups) {
     const group = labelGroups[xStr];
     group.forEach((l) => {
         if (l.orgX < gridLeft || l.orgX > gridRight) return;

         const bg = l.isDark ? 'rgba(0,0,0,0.7)' : 'rgba(255,255,255,0.85)';
         const border = l.isDark ? '#0f172a' : '#fff';
         const textShadow = l.isDark ? 'none' : '0 1px 2px rgba(0,0,0,0.1)';
         
         const labelLeft = l.orgX + 10;
         const labelTop = l.y;
         const destY = labelTop + (l.fixedY ? 8 : 7);
         
         if ((showLabels || l.alwaysShow) && (Math.abs(l.orgY - destY) > 0 || Math.abs(l.orgX - labelLeft) > 0)) {
            svgStr += `<line x1="${l.orgX}" y1="${l.orgY}" x2="${labelLeft}" y2="${destY}" stroke="${l.color}" stroke-dasharray="2,2" stroke-width="1.5" opacity="0.6" />`;
         }
         
         if (!l.noDot && (renderDots || l.dot)) {
            const dotFill = l.isDark ? '#0f172a' : '#ffffff';
            svgStr += `<circle cx="${l.orgX}" cy="${l.orgY}" r="${l.fixedY ? 2 : 3.5}" fill="${dotFill}" stroke="${l.color}" stroke-width="1" ${l.dashed ? 'stroke-dasharray="2,2"' : ""} />`;
         }

         if (showLabels || l.alwaysShow) {
           const style = `
             position: absolute;
             left: ${labelLeft}px;
             top: ${labelTop}px;
             font-family: var(--font-mono);
             font-size: 10px;
             font-weight: bold;
             ${l.fixedY ? 'height: 16px; line-height: 14px; box-sizing: border-box;' : ''}
             color: ${l.color};
             background-color: ${bg};
             border: 1px solid ${border};
             padding: 0px 4px;
             border-radius: 4px;
             pointer-events: none;
             white-space: nowrap;
             box-shadow: ${textShadow};
             ${l.fixedY ? '' : 'transition: top 0.15s ease-out, left 0.1s ease-out;'}
             z-index: 50;
           `;
           htmlStr += `<div style="${style.replace(/\n/g, "")}">${l.text
             .replace(/&/g, "&amp;")
             .replace(/</g, "&lt;")
             .replace(/>/g, "&gt;")}</div>`;
         }
     });
  }
  svgStr += `</g></svg>`;
  container.innerHTML = svgStr + htmlStr;
}

// Mouse position in the slider's data area, matching the cursor lines that
// chartOptionsBuilder draws there. The slider always spans the whole x axis.
function zoomBarHoverLine(chart: echarts.ECharts, index: number, theme: string) {
  const opt = chart.getOption() as any;
  const count = opt.xAxis?.[0]?.data?.length ?? 0;
  if (count < 2) return "";
  const left = parseInt(opt.grid?.[0]?.left || "50");
  const right = chart.getWidth() - parseInt(opt.grid?.[0]?.right || "15");
  const x = Math.round(left + Math.max(0, Math.min(1, index / (count - 1))) * (right - left)) + 0.5;
  const height = chart.getHeight();
  const color = theme === "dark" ? "#94a3b8" : "#64748b";
  return `<svg style="position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none;"><line x1="${x}" y1="${height - 14}" x2="${x}" y2="${height}" stroke="${color}" stroke-width="1" stroke-dasharray="2,2" /></svg>`;
}

function appendCompareLabels(chart: echarts.ECharts, groups: Record<number, LabelInfo[]>, index: number, hover: boolean) {
  const state = useWaveformStore.getState();
  if (!state.compareEnabled || !state.compareChartData || !state.chartData) return;
  if (!hover && !state.showMarkers) return;
  const option = chart.getOption() as any;
  if (!option?.series || !option.yAxis || !option.grid) return;
  const inactiveA = compareInactive(state.compareChartData, state.inactive);
  const inactiveB = compareInactive(state.chartData, state.inactive);
  const step = compareDisplaySteps(state.chartData, state.compareChartData)[index];
  for (const sig of Object.keys(state.compareChartData.seriesMap)) {
    const aSeries = option.series.findIndex((s: any) => s.name === sig + "_compare_a");
    const bSeries = option.series.findIndex((s: any) => s.name === sig);
    if (aSeries < 0 || bSeries < 0) continue;
    const a = compareSample(state.compareChartData, sig, step), b = compareSample(state.chartData, sig, step);
    const conv = getDisplayConversion(sig, state.lsbMapping, state.offsetMapping, state.valueDisplayMode);
    const unit = getDisplayUnit(sig, state.unitMapping, state.modeMapping, state.valueDisplayMode);
    const isState = Object.values(state.modeMapping[sig] ?? {}).some(name => name.trim());
    const aInactive = a.index >= 0 && inactiveLabel(inactiveA, sig, a.index);
    const bInactive = b.index >= 0 && inactiveLabel(inactiveB, sig, b.index);
    const differ = a.raw !== b.raw || !!aInactive !== !!bInactive;
    const color = option.series[bSeries].itemStyle?.color || "#64748b";
    const format = (raw: number | null, label: string | false | null | undefined) => {
      if (label) return `— (${label})`;
      if (raw === null) return "値なし";
      let value = String(formatPhysical(toPhysical(raw, conv), conv)) + (hover && unit ? " " + unit : "");
      if (state.showCursorLabels === "full" && state.modeMapping[sig]?.[raw] !== undefined) value += ` : ${state.modeMapping[sig][raw]}`;
      return value;
    };
    const bValue = b.raw === null ? a.raw : b.raw;
    if (bValue === null) continue;
    const bPoint = chart.convertToPixel({ seriesIndex: bSeries }, [index, toPhysical(bValue, conv)]) as number[];
    const aPoint = a.raw === null ? bPoint : chart.convertToPixel({ seriesIndex: aSeries }, [index, toPhysical(a.raw, conv)]) as number[];
    if (!bPoint?.every(Number.isFinite) || !aPoint?.every(Number.isFinite)) continue;
    const x = Math.round(bPoint[0]);
    if (!groups[x]) groups[x] = [];
    const add = (id: string, point: number[], targetY: number, text: string, fill: string, extra: Partial<LabelInfo> = {}) => {
      const axis = option.yAxis[option.series[id === "a" ? aSeries : bSeries].yAxisIndex];
      const grid = axis && option.grid[axis.gridIndex];
      if (!grid) return;
      const top = typeof grid.top === "string" ? parseFloat(grid.top) / 100 * chart.getHeight() : grid.top;
      const height = typeof grid.height === "string" ? parseFloat(grid.height) / 100 * chart.getHeight() : grid.height;
      if (point[1] < top - 1 || point[1] > top + height + 1) return;
      groups[x].push({ id: `compare-${sig}-${index}-${id}`, orgX: x, orgY: point[1], targetY: Math.max(26, isState ? Math.max(top, Math.min(top + height - 14, targetY)) : targetY), y: 0,
        text, color: fill, isDark: state.theme === "dark", dot: !hover, ...extra });
    };
    if (hover && !isState && !aInactive && !bInactive && a.raw !== null && b.raw !== null && a.raw !== b.raw) {
      const deltaSeries = option.series.find((s: any) => s.name === sig + "_compare_delta");
      const track = deltaSeries?.compareDeltaTrack as CompareDeltaTrack | undefined;
      const segment = track?.segments.find(s => s.start <= index && index < s.end);
      if (track && track.height > 0 && segment && segment.delta !== 0) {
        const top = track.top * chart.getHeight(), height = track.height * chart.getHeight();
        const delta = segment.delta;
        const grid = option.grid[option.yAxis[deltaSeries.yAxisIndex].gridIndex];
        const pixels = (value: number | string) => typeof value === "string"
          ? parseFloat(value) / 100 * chart.getWidth() : value;
        const xAt = (i: number) => {
          const last = option.xAxis[deltaSeries.xAxisIndex].data.length - 1;
          const lo = Math.max(0, Math.min(last - 1, Math.floor(i)));
          const at = (n: number) => chart.convertToPixel({ xAxisIndex: deltaSeries.xAxisIndex }, n) as number;
          return last <= 0 ? at(0) : at(lo) + (at(lo + 1) - at(lo)) * (i - lo);
        };
        const pointY = compareDeltaYScale(track, chart.getHeight(), xAt,
          pixels(grid.left), chart.getWidth() - pixels(grid.right))(delta);
        // Center the 16px badge on the point, shifting upward only at the row bottom.
        const targetY = Math.min(top + height - 16, pointY - 8);
        groups[x].push({ id: `compare-${sig}-${index}-delta`, orgX: bPoint[0], orgY: pointY,
          targetY, y: targetY, fixedY: true, alwaysShow: true, dot: true,
          text: `Δ ${delta > 0 ? "+" : ""}${formatPhysical(delta, conv)}${unit ? " " + unit : ""}`,
          color: state.theme === "dark" ? "#fca5a5" : "#b91c1c", isDark: state.theme === "dark" });
      }
    }
    if (state.showCursorLabels === "off" && hover) continue;
    if (!differ) add("b", bPoint, bPoint[1] - 20, format(b.raw, bInactive), color);
    else {
      add("a", aPoint, aPoint[1] - (isState ? 34 : 20), `A ${format(a.raw, aInactive)}`, state.theme === "dark" ? "#fca5a5" : "#b91c1c", { dashed: !isState, noDot: a.raw === null });
      add("b", bPoint, bPoint[1] - 20, `B ${format(b.raw, bInactive)}`, color, { noDot: b.raw === null });
    }
  }
}

/** Shared by the live chart and the offscreen image chart. */
export function buildCursorLabelGroups(chart: echarts.ECharts, theme: string, exporting = false) {
  const option = chart.getOption() as any;
  const state = useWaveformStore.getState();
  const legacyExport = exporting && !(state.compareEnabled && state.compareChartData);
  const labelGroups: Record<number, LabelInfo[]> = {};
  const isDark = theme === "dark";
  if (!option?.series) return labelGroups;
  const inactive = !legacyExport && state.chartData
    ? compareInactive(state.chartData, state.inactive) : null;

  option.series.forEach((series: any, seriesIdx: number) => {
    if (!series.name?.endsWith("_cursor_values")) return;
    const signal = series.name.slice(0, -"_cursor_values".length);
    if (hasCompareLabels(signal)) return;
    series.data?.forEach((item: any, dataIdx: number) => {
      const [strX, physVal, text] = item.value;
      const point = chart.convertToPixel({ seriesIndex: seriesIdx }, [strX, physVal]);
      if (!point || point.length !== 2 || isNaN(point[0]) || isNaN(point[1])) return;
      if (!legacyExport && !chart.containPixel({ seriesIndex: seriesIdx }, point)) return;
      const x = Math.round(point[0]), y = point[1];
      const label = inactive && inactiveLabel(inactive, signal, state.cursors[dataIdx]?.index);
      const color = label ? "#9ca3af" : item.label?.color || item.itemStyle?.color || "#000";
      if (!labelGroups[x]) labelGroups[x] = [];
      labelGroups[x].push({ id: `${seriesIdx}-${dataIdx}`, orgX: x, orgY: y,
        targetY: Math.max(26, y - 20), y: 0, text, color, isDark });
    });
  });
  for (const cursor of state.cursors) appendCompareLabels(chart, labelGroups, cursor.index, false);
  return labelGroups;
}

function hasCompareLabels(signal: string) {
  const state = useWaveformStore.getState();
  return state.compareEnabled && !!state.compareChartData?.seriesMap[signal];
}

/** The store retains zoom requests made before the first option is installed. */
function applyRequestedCompareZoom(chart: echarts.ECharts) {
  const state = useWaveformStore.getState();
  const zoom = state.compareEnabled ? state.zoomRange : null;
  if (!zoom) return;
  const current = (chart.getOption() as any)?.dataZoom?.[0];
  if (!current || (Math.abs(current.start - zoom.start) < 0.001 &&
    Math.abs(current.end - zoom.end) < 0.001)) return;
  chart.dispatchAction({ type: "dataZoom", start: zoom.start, end: zoom.end });
}

export function Chart() {
  const view = useCompareView();
  const hadComparison = useRef(false);
  const hadExtendedAxis = useRef(false);
  const requestedZoom = useWaveformStore(s => s.compareEnabled ? s.zoomRange : null);
  const inactive = useWaveformStore((s) => s.inactive);
  const inactiveRef = useRef<InactiveResult | null>(null);
  const [contextMenu, setContextMenu] = useState<{
    x: number;
    y: number;
    signal: string;
    value: number;
    label: string;
  } | null>(null);
  const menuRef = useRef<HTMLDivElement>(null);
  const [inactiveTooltip, setInactiveTooltip] = useState<{
    x: number;
    y: number;
    text: string;
  } | null>(null);

  useEffect(() => {
    if (!contextMenu) return;
    const close = (e: PointerEvent) => {
      if (!menuRef.current?.contains(e.target as Node)) setContextMenu(null);
    };
    const key = (e: KeyboardEvent) => {
      if (e.key === "Escape") setContextMenu(null);
    };
    document.addEventListener("pointerdown", close);
    document.addEventListener("keydown", key);
    return () => {
      document.removeEventListener("pointerdown", close);
      document.removeEventListener("keydown", key);
    };
  }, [contextMenu]);
  const chartRef = useRef<HTMLDivElement>(null);
  const chartInstance = useRef<echarts.ECharts | null>(null);
  const fullSeriesRef = useRef<FullSeries>(new Map());
  const drawnRangeRef = useRef("");
  const decimateFrameRef = useRef(0);
  const prevAutoTriggerRef = useRef<number>(0);

  /** 見えている範囲が変わっていたら、絞った点を作り直して系列の data だけを差し替える。 */
  const refreshDecimation = useCallback((chart: echarts.ECharts) => {
    const full = fullSeriesRef.current;
    if (!full.size || chart.isDisposed()) return;
    const count = full.values().next().value!.length;
    const [start, end] = visibleCategoryRange(chart, count);
    const buckets = decimateBuckets(chart);
    const key = `${start}:${end}:${buckets}`;
    if (key === drawnRangeRef.current) return;
    drawnRangeRef.current = key;
    const last = Math.max(...full.keys());
    const series: any[] = [];
    for (let i = 0; i <= last; i++) {
      const data = full.get(i);
      series.push(data ? { data: decimateRange(data, start, end, buckets) } : {});
    }
    chart.setOption({ series });
  }, []);
  const lastZoomRef = useRef<{
    start: number;
    end: number;
    startValue: number | string | undefined;
    endValue: number | string | undefined;
  } | null>(null);

  const {
      
    chartData,
    compareChartData,
    compareEnabled,
    diffs,
    diffFilter,
    focusedDiffIndex,
    signalSettings,
    signalGroups,
    unassignedSignals,
    unassignedGroupConfig,
    cursors,
    modeMapping,
    lsbMapping,
    offsetMapping,
    unitMapping,
    undefinedMapping,
    taskPeriod,
    showMode,
    showSignalNames,
    showAlias,
    showMarkers,
    showCursorLabels,
    valueDisplayMode,
    addCursor,
    hoveredSignal,
    autoAdjustTrigger,
    layoutMode,
    theme,
  
    } = useWaveformStore(useShallow((state) => ({
      chartData: state.chartData,
      compareChartData: state.compareChartData,
      compareEnabled: state.compareEnabled,
      diffs: state.diffs,
      diffFilter: state.diffFilter,
      focusedDiffIndex: state.focusedDiffIndex,
      signalSettings: state.signalSettings,
      signalGroups: state.signalGroups,
      unassignedSignals: state.unassignedSignals,
      unassignedGroupConfig: state.unassignedGroupConfig,
      cursors: state.cursors,
      modeMapping: state.modeMapping,
      lsbMapping: state.lsbMapping,
      offsetMapping: state.offsetMapping,
      unitMapping: state.unitMapping,
      undefinedMapping: state.undefinedMapping,
      taskPeriod: state.taskPeriod,
      showMode: state.showMode,
      showSignalNames: state.showSignalNames,
      showAlias: state.showAlias,
      showMarkers: state.showMarkers,
      showCursorLabels: state.showCursorLabels,
      valueDisplayMode: state.valueDisplayMode,
      addCursor: state.addCursor,
      hoveredSignal: state.hoveredSignal,
      autoAdjustTrigger: state.autoAdjustTrigger,
      layoutMode: state.layoutMode,
      theme: state.theme
    })));

  const inactiveResult = useMemo(
    () => computeInactive(chartData, inactive),
    [chartData, inactive],
  );
  const compareInactiveResult = useMemo(
    () => computeInactive(compareChartData, inactive),
    [compareChartData, inactive],
  );
  inactiveRef.current = inactiveResult;

  useEffect(() => {
    setInactiveTooltip(null);
  }, [inactiveResult]);

  const [containerSize, setContainerSize] = useState<{
    width: number;
    height: number;
  }>({ width: 0, height: 0 });
  const containerHeight = containerSize.height;

  const activeSignals = useMemo(() => {
    return activeDisplaySignals(view.signals, view.settings, view.visible);
  }, [view]);

  // Handle click for cursor addition
  useEffect(() => {
    const handleMouseDown = (e: MouseEvent) => {
      if (!chartInstance.current || !chartData || chartData.steps.length === 0)
        return;

      const rect = chartRef.current?.getBoundingClientRect();
      if (!rect) return;

      const hoverCursor = useWaveformStore.getState().hoverCursor;
      if (hoverCursor) {
        addCursor(hoverCursor.time, hoverCursor.index);
      }
    };

    const dom = chartRef.current;
    if (dom) {
      dom.addEventListener("dblclick", handleMouseDown);
      return () => dom.removeEventListener("dblclick", handleMouseDown);
    }
  }, [chartData, taskPeriod, addCursor]);

  // Handle theme changes
  useEffect(() => {
    if (chartInstance.current) {
      const existingOp = chartInstance.current.getOption() as echarts.EChartsOption & {
        dataZoom?: {
          start?: number;
          end?: number;
          startValue?: number | string;
          endValue?: number | string;
        }[];
      };
      if (existingOp && existingOp.dataZoom && existingOp.dataZoom.length > 0) {
        lastZoomRef.current = {
          start: existingOp.dataZoom[0].start ?? 0,
          end: existingOp.dataZoom[0].end ?? 100,
          startValue: existingOp.dataZoom[0].startValue,
          endValue: existingOp.dataZoom[0].endValue,
        };
      }
      chartInstance.current.dispose();
      chartInstance.current = null;
    }
  }, [theme]);

  // Main render logic
  useEffect(() => {
    if (!chartRef.current) return;

    if (!chartInstance.current) {
      chartInstance.current = echarts.init(chartRef.current, theme);
      cacheGetOption(chartInstance.current);

      // Canvas のレーン名・凡例にもネイティブの title で完全名を出す。
      chartInstance.current.on("mouseover", (event: any) => {
        const option = chartInstance.current?.getOption() as any;
        const name = event.componentType === "title"
          ? option?.title?.[event.componentIndex]?.id
          : event.componentType === "legend" ? event.name : undefined;
        if (chartRef.current && typeof name === "string") chartRef.current.title = name;
      });
      chartInstance.current.on("mouseout", () => chartRef.current?.removeAttribute("title"));
      chartInstance.current.getZr().on("globalout", () => chartRef.current?.removeAttribute("title"));

      chartInstance.current.getZr().on("contextmenu", (event: any) => {
        const chart = chartInstance.current;
        const state = useWaveformStore.getState();
        if (!chart || state.showMode === "off") return;
        const option = chart.getOption() as any;
        const point = [event.offsetX, event.offsetY];
        for (let i = 0; i < option.series.length; i++) {
          const series = option.series[i];
          if (
            !series.name?.endsWith("_highlight") ||
            !chart.containPixel({ seriesIndex: i }, point)
          ) {
            continue;
          }
          const signal = series.name.slice(0, -"_highlight".length);
          const coord = chart.convertFromPixel(
            { seriesIndex: i },
            point,
          ) as number[];
          const center = chart.convertToPixel(
            { xAxisIndex: series.xAxisIndex },
            coord[0],
          ) as number;
          const next = chart.convertToPixel(
            { xAxisIndex: series.xAxisIndex },
            coord[0] + 1,
          ) as number;
          const index = coord[0] + (point[0] - center) / (next - center || 1);
          const data = state.chartData?.seriesMap[signal];
          if (!data) continue;
          let value = data[Math.floor(index)];
          for (let j = Math.floor(index) - 1; value == null && j >= 0; j--) {
            value = data[j];
          }
          if (value == null || state.modeMapping[signal]?.[value] === undefined) {
            continue;
          }
          const inBand = series.markArea?.data?.some((area: any[]) => {
            const start = option.xAxis[series.xAxisIndex].data.indexOf(
              area[0].xAxis,
            );
            const end = option.xAxis[series.xAxisIndex].data.indexOf(
              area[1].xAxis,
            );
            return index >= start && index < end;
          });
          if (!inBand) continue;
          event.event?.preventDefault();
          setContextMenu({
            x: Math.max(
              0,
              Math.min(event.offsetX, (chartRef.current?.clientWidth ?? 300) - 230),
            ),
            y: Math.max(
              0,
              Math.min(event.offsetY, (chartRef.current?.clientHeight ?? 100) - 70),
            ),
            signal,
            value,
            label: `${(state.showAlias && state.signalSettings[signal]?.alias) || displaySignalName(signal, Object.keys(state.chartData?.seriesMap ?? {}))} = ${state.modeMapping[signal][value]}`,
          });
          break;
        }
      });
      chartInstance.current.getZr().on("mousemove", (event: any) => {
        const chart = chartInstance.current;
        const result = inactiveRef.current;
        if (!chart || !result?.show) {
          setInactiveTooltip(null);
          return;
        }
        const op = chart.getOption() as any;
        const overlay = op.series.find((series: any) =>
          series.name?.startsWith("_inactive_"),
        );
        if (!overlay) {
          setInactiveTooltip(null);
          return;
        }
        const grid = op.grid[op.xAxis[overlay.xAxisIndex].gridIndex];
        const point = [event.offsetX, event.offsetY];
        const top = Number(grid.top);
        const coord = chart.convertFromPixel(
          { xAxisIndex: overlay.xAxisIndex, yAxisIndex: overlay.yAxisIndex },
          point,
        ) as number[];
        // Round-to-nearest category would move the boundary half a sample early.
        const center = chart.convertToPixel(
          { xAxisIndex: overlay.xAxisIndex },
          coord[0],
        ) as number;
        const next = chart.convertToPixel(
          { xAxisIndex: overlay.xAxisIndex },
          coord[0] + 1,
        ) as number;
        const index = coord[0] + (point[0] - center) / (next - center || 1);
        const period = useWaveformStore.getState().taskPeriod;
        const hits = result.perRule.flatMap(({ rule, ranges }) =>
          ranges
            .filter(([start, end]) => index >= start && index < end)
            .map(
              ([start, end]) =>
                `${rule.label}  ${start * period}–${end * period} ms (${((end - start) * period / 1000).toFixed(2)} s)`,
            ),
        );
        if (
          event.offsetY < top - 18 ||
          event.offsetY > top + Number(grid.height) ||
          !hits.length
        ) {
          setInactiveTooltip(null);
          return;
        }
        setInactiveTooltip({
          x: Math.min(
            event.offsetX + 10,
            Math.max(0, (chartRef.current?.clientWidth ?? 300) - 250),
          ),
          y: Math.max(0, event.offsetY - 40),
          text: hits.join("\n"),
        });
      });
      chartInstance.current.on(
        "updateAxisPointer",
        (event: { axesInfo?: { axisDim: string; value: number }[] }) => {
          if (event.axesInfo && event.axesInfo.length > 0) {
            const axisInfo = event.axesInfo.find((a) => a.axisDim === "x");
            if (axisInfo) {
              const index = axisInfo.value;
              const taskP = useWaveformStore.getState().taskPeriod;
              const state = useWaveformStore.getState();
              const steps = state.compareEnabled && state.chartData && state.compareChartData
                ? compareDisplaySteps(state.chartData, state.compareChartData) : state.chartData?.steps;
              // Steps are 1..n for CSV files; the live record keeps the simulator's step numbers.
              const time = parseFloat(((steps?.[index] !== undefined ? steps[index] - 1 : index) * taskP).toPrecision(10));
              useWaveformStore.getState().setHoverCursor({ index, time });
              
              const hoverContainer = document.getElementById('hover-overlay-container');
              if (hoverContainer && chartInstance.current) {
                const option = chartInstance.current.getOption() as any;
                const labelGroups: Record<number, LabelInfo[]> = {};
                if (option && option.series) {
                   const xStrText = option.xAxis[0].data ? option.xAxis[0].data[index] : ''; 
                   const isDark = document.documentElement.classList.contains("dark");
                   option.series.forEach((series: any, seriesIdx: number) => {
                     if (series.name && series.name.endsWith('_mouse_tracker')) {
                        if (hasCompareLabels(series.name.slice(0, -"_mouse_tracker".length))) return;
                        // 描画用に絞った系列は [番号, 値] なので、値は全点から読む
                        const data = fullSeriesRef.current.get(seriesIdx) ?? series.data;
                        if (!data || data.length <= index) return;
                        const physVal = data[index];
                        if (physVal !== undefined && physVal !== null) {
                           const pxPos = chartInstance.current?.convertToPixel({ seriesIndex: seriesIdx }, [xStrText, physVal]);
                           if (pxPos && pxPos.length === 2 && !isNaN(pxPos[0]) && !isNaN(pxPos[1])) {
                             const isInside = chartInstance.current?.containPixel({ seriesIndex: seriesIdx }, pxPos);
                             if (isInside) {
                               const x = Math.round(pxPos[0]);
                               const y = pxPos[1];
                               let color = series.emphasis?.label?.color;
                               if (!color || color === 'transparent') {
                                 const baseSeries = option.series.find((s: any) => s.name === series.name.replace('_mouse_tracker', ''));
                                 color = baseSeries?.itemStyle?.color || '#000';
                               }
                               let text = String(physVal);
                               if (series.emphasis?.label?.formatter) {
                                  try {
                                    text = series.emphasis.label.formatter({ value: physVal, dataIndex: index });
                                  } catch(e) {}
                               }
                               const signal = series.name.slice(0, -"_mouse_tracker".length);
                               const label =
                                 inactiveRef.current &&
                                 inactiveLabel(inactiveRef.current, signal, index);
                               if (label) {
                                 text = `— (${label})`;
                                 color = "#9ca3af";
                               }
                               if (!labelGroups[x]) labelGroups[x] = [];
                               labelGroups[x].push({ id: `hover-${seriesIdx}`, orgX: x, orgY: y, targetY: Math.max(26, y - 20), y: 0, text, color, isDark });
                             }
                           }
                        }
                     }
                   });
                }
        appendCompareLabels(chartInstance.current, labelGroups, index, true);
        renderOverlayLabels(hoverContainer, labelGroups, true, useWaveformStore.getState().showCursorLabels !== "off", useWaveformStore.getState().cursors, chartInstance.current, useWaveformStore.getState().theme, true, useWaveformStore.getState().showMarkers);
        hoverContainer.insertAdjacentHTML("beforeend", zoomBarHoverLine(chartInstance.current, index, useWaveformStore.getState().theme));
              }
            }
          } else {
            useWaveformStore.getState().setHoverCursor(null);
            const hoverContainer = document.getElementById('hover-overlay-container');
            if (hoverContainer) hoverContainer.innerHTML = '';
          }
        },
      );
      chartInstance.current.getZr().on("globalout", () => {
        setInactiveTooltip(null);
        useWaveformStore.getState().setHoverCursor(null);
        const hoverContainer = document.getElementById('hover-overlay-container');
        if (hoverContainer) hoverContainer.innerHTML = '';
      });

      chartInstance.current.on('datazoom', () => {
        // 拡大・スクロール中は 16 ms に 1 回だけ点を絞り直す（requestAnimationFrame は裏のタブで止まるので使わない）
        if (!decimateFrameRef.current) {
          decimateFrameRef.current = window.setTimeout(() => {
            decimateFrameRef.current = 0;
            if (chartInstance.current) refreshDecimation(chartInstance.current);
          }, 16);
        }
        const op = chartInstance.current?.getOption() as any;
        if (op && op.dataZoom && op.dataZoom.length > 0) {
          const z = op.dataZoom[0];
          useWaveformStore.getState().setZoomRange({
            start: z.start,
            end: z.end,
            startValue: z.startValue,
            endValue: z.endValue,
          });
        }
      });

      chartInstance.current.on('rendered', () => {
        const container = document.getElementById('cursor-overlay-container');
        if (!container || !chartInstance.current) return;
        
        const option = chartInstance.current.getOption() as any;
        if (!option || !option.series) return;

        const labelGroups = buildCursorLabelGroups(chartInstance.current, useWaveformStore.getState().theme);
        renderOverlayLabels(container, labelGroups, false, useWaveformStore.getState().showCursorLabels !== "off", useWaveformStore.getState().cursors, chartInstance.current, useWaveformStore.getState().theme, false, useWaveformStore.getState().showMarkers);
      });
    }

    const myChart = chartInstance.current;

    if (activeSignals.length === 0 || !chartData) {
      myChart.clear();
      if (compareEnabled || hadComparison.current) {
        for (const id of ["hover-overlay-container", "cursor-overlay-container"]) {
          const overlay = document.getElementById(id);
          if (overlay) overlay.innerHTML = "";
        }
      }
      hadComparison.current = compareEnabled;
      return;
    }

    let currentZoom: {
      start: number;
      end: number;
      startValue?: number | string;
      endValue?: number | string;
    } = {
      start: 0,
      end: 100,
      startValue: undefined,
      endValue: undefined,
    };
    const existingOp = myChart.getOption() as echarts.EChartsOption & {
      dataZoom?: {
        start?: number;
        end?: number;
        startValue?: number | string;
        endValue?: number | string;
      }[];
    };
    const savedZoom = useWaveformStore.getState().zoomRange;
    
    if (
      savedZoom &&
      (!existingOp ||
        !existingOp.dataZoom ||
        existingOp.dataZoom.length === 0 ||
        Math.abs((existingOp.dataZoom[0].start ?? 0) - savedZoom.start) > 0.001 ||
        Math.abs((existingOp.dataZoom[0].end ?? 100) - savedZoom.end) > 0.001 ||
        existingOp.dataZoom[0].startValue !== savedZoom.startValue ||
        existingOp.dataZoom[0].endValue !== savedZoom.endValue)
    ) {
      currentZoom.start = savedZoom.start;
      currentZoom.end = savedZoom.end;
      // startValue and endValue are less critical to rely on for comparison since they might be strings or indices, just copy them if present
      currentZoom.startValue = savedZoom.startValue as any;
      currentZoom.endValue = savedZoom.endValue as any;
    } else if (
      existingOp &&
      existingOp.dataZoom &&
      existingOp.dataZoom.length > 0
    ) {
      currentZoom.start = existingOp.dataZoom[0].start ?? 0;
      currentZoom.end = existingOp.dataZoom[0].end ?? 100;
      currentZoom.startValue = existingOp.dataZoom[0].startValue;
      currentZoom.endValue = existingOp.dataZoom[0].endValue;
    } else if (lastZoomRef.current) {
      currentZoom.start = lastZoomRef.current.start;
      currentZoom.end = lastZoomRef.current.end;
      currentZoom.startValue = lastZoomRef.current.startValue;
      currentZoom.endValue = lastZoomRef.current.endValue;
      lastZoomRef.current = null;
    }

    // Percentages remain valid for the new category count; old value bounds may not.
    const extended = view.chartData?.sourceSampleCount !== undefined;
    if (extended || hadExtendedAxis.current) {
      currentZoom.startValue = undefined;
      currentZoom.endValue = undefined;
    }
    hadExtendedAxis.current = extended;

    const option = buildChartOptions(
      view.chartData,
      containerHeight,
      activeSignals,
      view.groups,
      view.unassigned,
      unassignedGroupConfig,
      view.settings,
      modeMapping,
      lsbMapping,
      offsetMapping,
      undefinedMapping,
      unitMapping,
      taskPeriod,
      layoutMode,
      showSignalNames,
      showAlias,
      showMode,
      showMarkers,
      showCursorLabels,
      cursors,
      currentZoom,
      theme,
      valueDisplayMode,
      inactiveResult,
      compareEnabled && compareChartData
        ? { data: compareChartData, summary: view.summary, diffs, diffFilter, focusedDiffIndex, inactive: compareInactiveResult }
        : undefined,
    );

    setTimeout(() => {
      if (myChart.isDisposed()) return;
      myChart.resize();
      // 長い記録は、まず全体を絞った点で入れ、見えている範囲に合わせて絞り直す
      let full = collectFullSeries(option);
      drawnRangeRef.current = "";
      if (full.size) {
        const buckets = decimateBuckets(myChart);
        for (const [i, data] of full) (option.series[i] as any).data = decimateRange(data, 0, data.length, buckets);
        full = addZoomShadowSeries(option, full, buckets);
        drawnRangeRef.current = `0:${full.values().next().value!.length}:${buckets}`;
      }
      fullSeriesRef.current = full;
      myChart.setOption(option as echarts.EChartsOption, true);
      // Read the latest request, including navigation after this callback was scheduled.
      applyRequestedCompareZoom(myChart);
      refreshDecimation(myChart);
      const hoverOverlay = document.getElementById("hover-overlay-container");
      if (hoverOverlay && (compareEnabled || hadComparison.current)) hoverOverlay.innerHTML = "";
      hadComparison.current = compareEnabled;
    }, 0);
  }, [
    view,
    compareInactiveResult,
    compareChartData,
    compareEnabled,
    diffs,
    diffFilter,
    focusedDiffIndex,
    inactiveResult,
    chartData,
    activeSignals,
    signalSettings,
    signalGroups,
    unassignedSignals,
    unassignedGroupConfig,
    showMode,
    showSignalNames,
    showAlias,
    showMarkers,
    showCursorLabels,
    valueDisplayMode,
    cursors,
    lsbMapping,
    offsetMapping,
    taskPeriod,
    modeMapping,
    unitMapping,
    undefinedMapping,
    autoAdjustTrigger,
    layoutMode,
    containerSize,
    theme,
  ]);

  useEffect(() => {
    const chart = chartInstance.current;
    if (!chart || !requestedZoom) return;
    applyRequestedCompareZoom(chart);
  }, [requestedZoom]);

  // Fast hover update effect
  useEffect(() => {
    if (!chartInstance.current) return;
    const myChart = chartInstance.current;
    if (activeSignals.length === 0) return;

    const seriesUpdate = activeSignals.map((sig) => {
      const baseWidth = signalSettings[sig]?.lineWidth ?? 1.5;
      return {
        name: sig,
        lineStyle: {
          width: hoveredSignal === sig ? baseWidth + 2.0 : baseWidth,
        },
      };
    });

    setTimeout(() => {
      myChart.setOption({ series: seriesUpdate });
    }, 0);
  }, [hoveredSignal, activeSignals, signalSettings]);

  // Handle Resize
  useEffect(() => {
    if (!chartRef.current) return;
    const handleResize = () => {
      if (chartRef.current) {
        setContainerSize({
          width: chartRef.current.clientWidth,
          height: chartRef.current.clientHeight,
        });
      }
    };
    const observer = new ResizeObserver(handleResize);
    observer.observe(chartRef.current);
    handleResize(); 
    return () => observer.disconnect();
  }, []);

  const handleCopyImage = async (layout: 'only-chart' | 'vertical-normal' | 'vertical-transposed' | 'horizontal-normal') => {
    const hasMarkers = useWaveformStore.getState().cursors.length > 0;
    try {
      await toast.promise(
        copyChartAndTableToClipboard(hasMarkers, theme, layout),
        {
          loading: '画像をコピーしています...',
          success: 'クリップボードにコピーしました！',
          error: (err) => `コピーに失敗しました: ${err.message || '不明なエラー'}`,
        }
      );
    } catch (e) {
      console.error(e);
    }
  };

  return (
    <div className="relative w-full h-full overflow-hidden">
      {inactiveTooltip && (
        <div
          className="absolute z-[60] pointer-events-none whitespace-pre-line rounded bg-slate-900 text-white text-xs p-2 shadow"
          style={{ left: inactiveTooltip.x, top: inactiveTooltip.y }}
        >
          {inactiveTooltip.text}
        </div>
      )}
      {contextMenu && (
        <div
          ref={menuRef}
          role="menu"
          title={contextMenu.signal}
          className="absolute z-[70] rounded border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800 text-gray-800 dark:text-gray-200 text-xs shadow-lg p-1"
          style={{ left: contextMenu.x, top: contextMenu.y }}
        >
          <button
            className="block p-2 hover:bg-gray-100 dark:hover:bg-slate-700"
            onClick={() => {
              const state = useWaveformStore.getState();
              const found = state.inactive.rules.findIndex(
                (r) => r.signal === contextMenu.signal,
              );
              const rules =
                found < 0
                  ? [
                      ...state.inactive.rules,
                      {
                        signal: contextMenu.signal,
                        when: [contextMenu.value],
                        label: contextMenu.label,
                      },
                    ]
                  : state.inactive.rules.map((r, i) =>
                      i === found && !r.when.includes(contextMenu.value)
                        ? { ...r, when: [...r.when, contextMenu.value] }
                        : r,
                    );
              state.setInactive({ ...state.inactive, rules });
              setContextMenu(null);
            }}
          >
            この状態を無効区間にする
          </button>
          <button
            className="block p-2 hover:bg-gray-100 dark:hover:bg-slate-700"
            onClick={() => {
              useWaveformStore.getState().openInactiveEditor(true);
              setContextMenu(null);
            }}
          >
            無効区間の設定を開く…
          </button>
        </div>
      )}
      <div id="chart-container" ref={chartRef} className="w-full h-full rounded bg-white"></div>
      <div id="cursor-overlay-container" className="absolute inset-0 pointer-events-none overflow-hidden z-[40]"></div>
      <div id="hover-overlay-container" className="absolute inset-0 pointer-events-none overflow-hidden z-[45]"></div>
      {activeSignals.length > 0 && chartData && (
        <div className="absolute top-0 left-0 z-[50] flex items-center bg-white/90 dark:bg-slate-800/90 border-b border-r border-gray-300/50 dark:border-slate-600/50 rounded-br shadow-sm backdrop-blur-sm">
          <div className="w-[24px] h-[20px] border-r border-gray-300/50 dark:border-slate-600/50 flex items-center justify-center text-gray-500">
            <Camera size={12} />
          </div>
          
          {/* ① 波形のみ */}
          <button
            onClick={() => handleCopyImage('only-chart')}
            className="w-[24px] h-[20px] flex items-center justify-center text-gray-700 dark:text-gray-200 hover:bg-gray-100 dark:hover:bg-slate-700 transition-colors border-r border-gray-300/50 dark:border-slate-600/50 group"
            title="波形のみ"
          >
            <div className="w-[14px] h-[14px] border border-current bg-current flex items-center justify-center opacity-70 group-hover:opacity-100 transition-opacity">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" className="w-[10px] h-[10px] text-white">
                <polyline points="2 12 6 12 10 3 14 21 18 12 22 12"></polyline>
              </svg>
            </div>
          </button>

          {/* ② 上下レイアウト（横配列：上がヘッダ） */}
          <button
            onClick={() => handleCopyImage('vertical-normal')}
            className="w-[24px] h-[20px] flex items-center justify-center text-gray-700 dark:text-gray-200 hover:bg-gray-100 dark:hover:bg-slate-700 transition-colors border-r border-gray-300/50 dark:border-slate-600/50 group"
            title="上下レイアウト（横配列）"
          >
            <div className="flex flex-col gap-[1px] w-[14px] h-[15px] opacity-70 group-hover:opacity-100 transition-opacity">
              {/* 波形(8px) */}
              <div className="w-full h-[8px] border border-current bg-current flex items-center justify-center overflow-hidden">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" className="w-[9px] h-[9px] text-white mt-[1px]">
                  <polyline points="0 12 5 12 9 4 15 20 19 12 24 12"></polyline>
                </svg>
              </div>
              {/* 表(6px) */}
              <div className="w-full h-[6px] border border-current flex flex-col overflow-hidden bg-transparent">
                {/* ヘッダ(上部) */}
                <div className="w-full h-[2.5px] bg-current opacity-40"></div>
              </div>
            </div>
          </button>

          {/* ③ 上下レイアウト（縦配列：左がヘッダ） */}
          <button
            onClick={() => handleCopyImage('vertical-transposed')}
            className="w-[24px] h-[20px] flex items-center justify-center text-gray-700 dark:text-gray-200 hover:bg-gray-100 dark:hover:bg-slate-700 transition-colors border-r border-gray-300/50 dark:border-slate-600/50 group"
            title="上下レイアウト（縦配列）"
          >
            <div className="flex flex-col gap-[1px] w-[14px] h-[15px] opacity-70 group-hover:opacity-100 transition-opacity">
              {/* 波形(8px) */}
              <div className="w-full h-[8px] border border-current bg-current flex items-center justify-center overflow-hidden">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" className="w-[9px] h-[9px] text-white mt-[1px]">
                  <polyline points="0 12 5 12 9 4 15 20 19 12 24 12"></polyline>
                </svg>
              </div>
              {/* 表(6px) */}
              <div className="w-full h-[6px] border border-current flex overflow-hidden bg-transparent">
                {/* ヘッダ(左側部) */}
                <div className="w-[5px] h-full bg-current opacity-40"></div>
              </div>
            </div>
          </button>

          {/* ④ 左右レイアウト（表：左 縦配列、 波形：右） */}
          <button
            onClick={() => handleCopyImage('horizontal-normal')}
            className="w-[24px] h-[20px] flex items-center justify-center text-gray-700 dark:text-gray-200 hover:bg-gray-100 dark:hover:bg-slate-700 transition-colors rounded-br group"
            title="左右レイアウト（縦配列）"
          >
            <div className="flex gap-[1px] w-[15px] h-[14px] opacity-70 group-hover:opacity-100 transition-opacity">
               {/* 表(5px幅) */}
              <div className="w-[5px] h-full border border-current flex flex-col overflow-hidden bg-transparent">
                {/* ヘッダ(上部) */}
                <div className="w-full h-[4px] bg-current opacity-40"></div>
              </div>
              {/* 波形(9px幅) */}
              <div className="w-[9px] h-full border border-current bg-current flex items-center justify-center overflow-hidden">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" className="w-[10px] h-[10px] text-white scale-x-[-1] ml-[1px]">
                  <polyline points="0 12 5 12 9 4 15 20 19 12 24 12"></polyline>
                </svg>
              </div>
            </div>
          </button>
        </div>
      )}
    </div>
  );
}
