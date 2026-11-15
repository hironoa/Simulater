import { displaySignalName } from "./displaySignalName";
import * as echarts from "echarts";
import { addCompareRendering, CompareRendering } from "./compareRendering";
import { YBreak } from "../types";
import {
  calculateAutoYRange,
  getDarkerColor,
  getConversion,
  getDisplayConversion,
  getDisplayUnit,
  toPhysical,
  formatPhysical,
  detectUndefinedValue,
  computeAutoBreak,
} from "./helpers";
import { getStateColor } from "./stateNames";
import {
  InactiveResult,
  inactiveLabel,
  subtractInactive,
  INACTIVE_COLOR,
} from "./inactiveRanges";
import { fitLabel } from "./fitLabel";

export function buildChartOptions(
  chartData: any,
  containerHeight: number,
  activeSignals: string[],
  signalGroups: any[],
  unassignedSignals: string[],
  unassignedGroupConfig: any,
  signalSettings: any,
  modeMapping: any,
  lsbMapping: any,
  offsetMapping: any,
  undefinedMapping: any,
  unitMapping: any,
  taskPeriod: number,
  layoutMode: "uniform" | "optimized" | "manual",
  showSignalNames: boolean,
  showAlias: boolean,
  showMode: "off" | "bg-only" | "full",
  showMarkers: boolean,
  showCursorLabels: "off" | "value-only" | "full",
  cursors: any[],
  currentZoom: any,
  theme: "light" | "dark",
  valueDisplayMode: "physical" | "raw" = "physical",
  inactive?: InactiveResult,
  comparison?: CompareRendering,
) {
  const rangeCache = new Map<string, (number | null)[]>();
  const rangeData = (sig: string) => {
    if (!comparison?.data.seriesMap[sig]) return chartData.seriesMap[sig];
    if (!rangeCache.has(sig)) rangeCache.set(sig,
      [...chartData.seriesMap[sig], ...comparison.data.seriesMap[sig]]);
    return rangeCache.get(sig)!;
  };
  const hiddenRanges = (sig: string) =>
    inactive?.show && !inactive.conditions.has(sig) ? inactive.union : [];
  // Layout annotations and display ranges must never enter the store.
  signalSettings = { ...signalSettings };
  const isRaw = valueDisplayMode === "raw";
  // Synced axes share one break, so raw-mode breaks use the group leader's conversion
  // (ranges use each signal's own conversion before they are merged).
  const breakLeaders: Record<string, string> = {};
  const displayConversion = (sig: string) =>
    getDisplayConversion(sig, lsbMapping, offsetMapping, valueDisplayMode);
  const displayUnit = (sig: string) =>
    getDisplayUnit(sig, unitMapping, modeMapping, valueDisplayMode);
  const fromPhysical = (sig: string, value: number) => {
    if (!isRaw) return value;
    const conv = getConversion(sig, lsbMapping, offsetMapping);
    return (value - conv.offset) / conv.lsb;
  };
  const manualRange = (sig: string, set: any, fallback: { min: number; max: number }) => {
    const conv = getConversion(sig, lsbMapping, offsetMapping);
    const reversed = isRaw && conv.lsb < 0;
    const lower = reversed ? set.yMax : set.yMin;
    const upper = reversed ? set.yMin : set.yMax;
    const min = lower != null ? fromPhysical(sig, lower) : fallback.min;
    const max = upper != null ? fromPhysical(sig, upper) : fallback.max;
    return isRaw ? { min: Math.min(min, max), max: Math.max(min, max) } : { min, max };
  };
  const xDataText = chartData.steps.map((step) =>
    String(parseFloat(((step - 1) * taskPeriod).toPrecision(10))),
  );

  // === Layout & Config Calculation ===
  type DisplayBlock =
    | {
        type: "group_overlap";
        name: string;
        signals: string[];
        syncRange: boolean;
        sig?: never;
        syncGroup?: never;
      }
    | {
        type: "single";
        sig: string;
        syncGroup: string[] | null;
        signals?: never;
        name?: never;
        syncRange?: never;
      };

  let displayBlocks: DisplayBlock[] = [];
  signalGroups.forEach((group) => {
    const groupActiveSigs = group.signals.filter((s) =>
      activeSignals.includes(s),
    );
    if (groupActiveSigs.length === 0) return;
    if (group.layout === "overlap") {
      displayBlocks.push({
        type: "group_overlap",
        name: group.name,
        signals: groupActiveSigs,
        syncRange: group.syncRange,
      });
    } else {
      groupActiveSigs.forEach((sig) => {
        displayBlocks.push({
          type: "single",
          sig: sig,
          syncGroup: group.syncRange ? groupActiveSigs : null,
        });
      });
    }
  });

  const unassignedActiveSigs = unassignedSignals.filter((s) =>
    activeSignals.includes(s),
  );
  if (unassignedActiveSigs.length > 0) {
    if (unassignedGroupConfig.layout === "overlap") {
      displayBlocks.push({
        type: "group_overlap",
        name: "未分類",
        signals: unassignedActiveSigs,
        syncRange: unassignedGroupConfig.syncRange,
      });
    } else {
      unassignedActiveSigs.forEach((sig) => {
        displayBlocks.push({
          type: "single",
          sig: sig,
          syncGroup: unassignedGroupConfig.syncRange
            ? unassignedActiveSigs
            : null,
        });
      });
    }
  }

  const N = displayBlocks.length;
  // Fixed pixel top margin equivalent to ensure labels are not cut off
  const fixedTopMarginPx = 35;
  const topMargin =
    containerHeight > 0 ? (fixedTopMarginPx / containerHeight) * 100 : 4;
  const bottomMargin = 8;
  const availableHeight = 100 - topMargin - bottomMargin;
  const maxSpacing = 2.0;
  const spacing =
    N > 1 ? Math.min(maxSpacing, (availableHeight * 0.25) / (N - 1)) : 0;

  let totalWeight = 0;
  const blockWeights = displayBlocks.map((block) => {
    if (layoutMode === "uniform") {
      totalWeight += 1;
      return 1;
    }

    let userWeight = 1;
    if (block.type === "group_overlap") {
      userWeight =
        block.signals.reduce(
          (sum: number, s: string) => sum + (signalSettings[s].weight ?? 1),
          0,
        ) / block.signals.length || 1;
    } else {
      userWeight = signalSettings[block.sig].weight ?? 1;
    }
    totalWeight += userWeight;
    return userWeight;
  });

  let currentTop = topMargin;
  let gridGroups: { top: number; height: number }[] = [];
  let sigToGridIndex: Record<string, number> = {};
  let syncRangeCache: Record<
    string,
    { min: number; max: number; yBreak: YBreak | null }
  > = {};

  displayBlocks.forEach((block, idx) => {
    const gTop = currentTop;
    const w = blockWeights[idx];
    const gHeight =
      totalWeight > 0
        ? (availableHeight - spacing * (N - 1)) * (w / totalWeight)
        : 0;
    gridGroups.push({ top: gTop, height: gHeight });

    if (block.type === "group_overlap") {
      if (block.syncRange) {
        let gMin = Infinity,
          gMax = -Infinity;
        let allUndefinedPhys: number[] = [];
        block.signals.forEach((sig: string) => {
          const undefinedVals = detectUndefinedValue(
            rangeData(sig),
            undefinedMapping[sig],
          );
          const leaderSet = signalSettings[block.signals[0]];
          const isYBreakActive = leaderSet && leaderSet.yBreak !== false;
          let r = calculateAutoYRange(
            sig,
            rangeData(sig),
            isRaw ? undefined : lsbMapping[sig],
            isRaw ? undefined : offsetMapping[sig],
            undefinedVals || [],
            false,
          );
          const set = signalSettings[sig];
          if (set && !set.yAuto) {
            r = { ...r, ...manualRange(sig, set, r) };
          }
          if (r.min < gMin) gMin = r.min;
          if (r.max > gMax) gMax = r.max;

          if (undefinedVals) {
            const conv = displayConversion(sig);
            allUndefinedPhys.push(...undefinedVals.map((v) => toPhysical(v, conv)));
          }
        });

        let yBreak: YBreak | null = null;
        const leaderSet = signalSettings[block.signals[0]];
        const isYBreakActive = leaderSet && leaderSet.yBreak !== false;

        if (allUndefinedPhys.length > 0) {
          if (isYBreakActive) {
            yBreak = computeAutoBreak(gMin, gMax, allUndefinedPhys);
            if (yBreak) {
              let finalMax = gMax;
              let finalMin = gMin;
              let undefMin = Math.min(...allUndefinedPhys);
              let undefMax = Math.max(...allUndefinedPhys);
              const diffNormal = Math.max(gMax - gMin, 1);
              const pad = diffNormal * 0.05;
              if (undefMax > gMax) {
                finalMax = undefMax + pad;
              }
              if (undefMin < gMin) {
                finalMin = undefMin - pad;
              }
              gMin = finalMin;
              gMax = finalMax;
            }
          }
        }

        syncRangeCache[idx] = { min: gMin, max: gMax, yBreak };
      }
      block.signals.forEach((sig: string) => {
        sigToGridIndex[sig] = idx;
        if (block.syncRange) breakLeaders[sig] = block.signals[0];
        // Pass values to settings transiently for render
        signalSettings[sig] = {
          ...signalSettings[sig],
          top: gTop,
          height: gHeight,
          syncRange: block.syncRange ? syncRangeCache[idx] : null,
          overlapGroupTitle: block.name,
        };
      });
    } else {
      const sig = block.sig;
      sigToGridIndex[sig] = idx;
      let sRange = null;
      if (block.syncGroup) {
        breakLeaders[sig] = block.syncGroup[0];
        let cacheKey = block.syncGroup.join(",");
        if (!syncRangeCache[cacheKey]) {
          let gMin = Infinity,
            gMax = -Infinity;
          let allUndefinedPhys: number[] = [];
          block.syncGroup.forEach((s: string) => {
            const undefinedVals = detectUndefinedValue(
              rangeData(s),
              undefinedMapping[s],
            );
            const leaderSet =
              signalSettings[block.syncGroup ? block.syncGroup[0] : block.sig];
            const isYBreakActive = leaderSet && leaderSet.yBreak !== false;
            let r = calculateAutoYRange(
              s,
              rangeData(s),
              isRaw ? undefined : lsbMapping[s],
              isRaw ? undefined : offsetMapping[s],
              undefinedVals || [],
              false,
            );
            const set = signalSettings[s];
            if (set && !set.yAuto) {
              r = { ...r, ...manualRange(s, set, r) };
            }
            if (r.min < gMin) gMin = r.min;
            if (r.max > gMax) gMax = r.max;

            if (undefinedVals) {
              const conv = displayConversion(s);
              allUndefinedPhys.push(...undefinedVals.map((v) => toPhysical(v, conv)));
            }
          });

          let yBreak: YBreak | null = null;
          const leaderSet =
            signalSettings[block.syncGroup ? block.syncGroup[0] : block.sig];
          const isYBreakActive = leaderSet && leaderSet.yBreak !== false;

          if (allUndefinedPhys.length > 0) {
            if (isYBreakActive) {
              yBreak = computeAutoBreak(gMin, gMax, allUndefinedPhys);
              if (yBreak) {
                let finalMax = gMax;
                let finalMin = gMin;
                let undefMin = Math.min(...allUndefinedPhys);
                let undefMax = Math.max(...allUndefinedPhys);
                const diffNormal = Math.max(gMax - gMin, 1);
                const pad = diffNormal * 0.05;
                if (undefMax > gMax) {
                  finalMax = undefMax + pad;
                }
                if (undefMin < gMin) {
                  finalMin = undefMin - pad;
                }
                gMin = finalMin;
                gMax = finalMax;
              }
            }
          }

          syncRangeCache[cacheKey] = { min: gMin, max: gMax, yBreak };
        }
        sRange = syncRangeCache[cacheKey];
      }
      signalSettings[sig] = {
        ...signalSettings[sig],
        top: gTop,
        height: gHeight,
        syncRange: sRange,
        overlapGroupTitle: null,
      };
    }
    currentTop += gHeight + spacing;
  });

  const FONT_SANS =
    '"BIZ UDGothic", "Inter", "Helvetica Neue", Arial, sans-serif';
  const FONT_MONO = '"BIZ UDGothic", "JetBrains Mono", monospace';

  // Build Option
  const option = {
    visualMap: [] as echarts.VisualMapComponentOption[],
    animation: false,
    labelLayout: function (params: any) {
      if (params.seriesName && params.seriesName.endsWith('_cursor_values')) {
        return {
          moveOverlap: 'shiftY',
          hideOverlap: false
        };
      }
      return { hideOverlap: false };
    },
    textStyle: {
      fontFamily: FONT_SANS,
    },
    title: [] as echarts.TitleComponentOption[],
    tooltip: {
      trigger: "axis",
      showContent: false,
      axisPointer: { type: "line" },
    },
    axisPointer: { link: [{ xAxisIndex: "all" }] },
    legend: { show: false, formatter: (name: string) => (showAlias && signalSettings[name]?.alias) || displaySignalName(name, activeSignals) },
    dataZoom: [] as echarts.DataZoomComponentOption[],
    xAxis: [] as echarts.XAXisComponentOption[],
    yAxis: [] as echarts.YAXisComponentOption[],
    grid: [] as echarts.GridComponentOption[],
    series: [] as echarts.SeriesOption[],
  };

  let maxBottom = 0;
  activeSignals.forEach((sig) => {
    const b = signalSettings[sig].top + signalSettings[sig].height;
    if (b > maxBottom) {
      maxBottom = b;
    }
  });

  let maxRightAxesNum = 0;
  displayBlocks.forEach((block) => {
    if (block.type === "group_overlap" && !block.syncRange) {
      let numRight = block.signals.length - 1;
      if (numRight > maxRightAxesNum) maxRightAxesNum = numRight;
    }
  });
  const rightAxisWidth = 35;
  const globalRightMargin = 15 + maxRightAxesNum * rightAxisWidth;

  const getTextWidth = (t: string, fontSize: number) => {
    let w = 0;
    for (let i = 0; i < t.length; i++) {
      w += t.charCodeAt(i) > 255 ? fontSize * 1.5 : fontSize * 0.6;
    }
    return w;
  };

  let maxGridTitleWidths: Record<number, number> = {};
  let overlapSignalCounts: Record<number, number> = {};
  if (showSignalNames) {
    activeSignals.forEach((sig) => {
      const set = signalSettings[sig];
      const gridIdx = sigToGridIndex[sig];
      if (gridIdx === undefined) return;

      const displayName = (showAlias && set.alias) || displaySignalName(sig, activeSignals);
      const unit = displayUnit(sig);
      const titleText = unit ? `${displayName} [${unit}]` : displayName;

      const textWidth = getTextWidth(titleText, 10);

      maxGridTitleWidths[gridIdx] = Math.max(
        maxGridTitleWidths[gridIdx] || 0,
        textWidth,
      );

      overlapSignalCounts[gridIdx] = (overlapSignalCounts[gridIdx] || 0) + 1;
    });
  }

  let globalMaxTitleWidth = 0;
  Object.values(maxGridTitleWidths).forEach((w) => {
    if (w > globalMaxTitleWidth) globalMaxTitleWidth = w;
  });

  // 5px from edge to title + globalMaxTitleWidth + 5px spacing
  const leftMargin = showSignalNames ? 5 + globalMaxTitleWidth + 5 : 50;

  gridGroups.forEach((g, gIdx) => {
    option.grid.push({
      left: leftMargin,
      right: globalRightMargin,
      top: g.top + "%",
      height: g.height + "%",
    });
    const isBottom = gIdx === gridGroups.length - 1;
    option.xAxis.push({
      gridIndex: gIdx,
      type: "category",
      data: xDataText,
      boundaryGap: false,
      axisLabel: { show: isBottom, fontSize: 10, fontFamily: FONT_MONO },
      axisTick: { show: isBottom },
      axisPointer: {
        show: true,
        type: "line",
        lineStyle: { opacity: 0 },
        label: { show: false },
      },
    });
  });

  let realYAxisIdx = 0;
  let overlapTitleOffsets: Record<number, number> = {};
  activeSignals.forEach((sig, i) => {
    const set = signalSettings[sig];
    const gridIdx = sigToGridIndex[sig];
    if (gridIdx === undefined) return;

    const color = set.color;
    const isExp = sig.toLowerCase().includes("exp") || sig.includes("期待値");
    const conv = displayConversion(sig);
    const unit = displayUnit(sig);

    let rawDataZOH: (number | null)[] = [];
    let lastRaw: number | null = null;
    for (let i = 0; i < chartData.seriesMap[sig].length; i++) {
      let v = chartData.seriesMap[sig][i];
      if (i >= (chartData.sourceSampleCount ?? chartData.steps.length)) lastRaw = null;
      if (v !== null) lastRaw = v;
      rawDataZOH.push(lastRaw);
    }

    const undefinedVals = detectUndefinedValue(
      rangeData(sig),
      undefinedMapping[sig],
    );

    let autoRange = calculateAutoYRange(
      sig,
      rangeData(sig),
      isRaw ? undefined : lsbMapping[sig],
      isRaw ? undefined : offsetMapping[sig],
      undefinedVals || [],
      set.yBreak !== false,
    );
    if (set.syncRange) {
      autoRange = set.syncRange;
    }

    const hasUndefined = undefinedVals && undefinedVals.length > 0;
    const isYBreakActive = set.yBreak !== false;
    const undefinedPhys = hasUndefined
      ? undefinedVals!.map((v) => toPhysical(v, conv))
      : [];

    let finalMin = autoRange.min;
    let finalMax = autoRange.max;
    let activeBreak: YBreak | null = null;

    // Determine what break to use, if active.
    if (isYBreakActive) {
      if (set.yBreakStart != null && set.yBreakEnd != null) {
        activeBreak = {
          start: Math.min(
            fromPhysical(breakLeaders[sig] || sig, set.yBreakStart),
            fromPhysical(breakLeaders[sig] || sig, set.yBreakEnd),
          ),
          end: Math.max(
            fromPhysical(breakLeaders[sig] || sig, set.yBreakStart),
            fromPhysical(breakLeaders[sig] || sig, set.yBreakEnd),
          ),
        };
      } else if (set.syncRange?.yBreak) {
        activeBreak = set.syncRange.yBreak;
      } else if (!set.syncRange && hasUndefined) {
        if (autoRange.breakStart != null && autoRange.breakEnd != null) {
          activeBreak = {
            start: autoRange.breakStart,
            end: autoRange.breakEnd,
          };
        }
      }
    }

    let lastValForDisplay: number | null = null;
    let displayData = chartData.seriesMap[sig].map((v, index) => {
      if (index >= (chartData.sourceSampleCount ?? chartData.steps.length)) return null;
      if (v !== null) {
        lastValForDisplay = toPhysical(v, conv);
      }
      return lastValForDisplay;
    });

    let markAreaData: [
      { xAxis: string; itemStyle?: { color: string } },
      { xAxis: string },
    ][] = [];
    let customStateData: [number, number, string, string][] = [];
    let scatterData: {
      value: [string, number, string];
      symbol: string;
      symbolSize: number;
      itemStyle: { color: string; borderColor: string; borderWidth: number };
      label: {
        show: boolean;
        position: "right";
        distance: number;
        offset?: number[];
        formatter: (params: { value: any }) => string;
        color: string;
        fontSize: number;
        fontWeight?: "normal" | "bold" | "bolder" | "lighter" | 100 | 200 | 300 | 400 | 500 | 600 | 700 | 800 | 900;
        fontFamily: string;
        textBorderColor: string;
        textBorderWidth: number;
        backgroundColor: string;
        padding: number[];
        borderRadius: number;
      };
    }[] = [];

    if (showMode !== "off" && modeMapping[sig] && rawDataZOH.length > 0) {
      let currentRawVal = rawDataZOH[0];
      let startIndex = 0;
      const dataLen = chartData.steps.length;

      for (let j = 1; j < dataLen; j++) {
        let rawVal = rawDataZOH[j];

        if (rawVal !== currentRawVal || j === dataLen - 1) {
          if (
            currentRawVal !== null &&
            modeMapping[sig][currentRawVal] !== undefined
          ) {
            const stateColor = getStateColor(Number(currentRawVal));
            let endIndex = j;
            if (chartData.sourceSampleCount !== undefined) endIndex = Math.min(endIndex, chartData.sourceSampleCount - 1);

            for (const [visibleStart, visibleEnd] of subtractInactive(
              startIndex,
              endIndex,
              hiddenRanges(sig),
            )) {
              markAreaData.push([
                {
                  xAxis: xDataText[visibleStart],
                  itemStyle: { color: stateColor },
                },
                { xAxis: xDataText[visibleEnd] },
              ]);

              if (showMode === "full") {
                customStateData.push([
                  visibleStart,
                  visibleEnd,
                  modeMapping[sig][currentRawVal],
                  stateColor,
                ]);
              }
            }
          }
          currentRawVal = rawVal;
          startIndex = j;
        }
      }
    }

    if (showMarkers && cursors.length > 0) {
      cursors.forEach((c, cIdx) => {
        let rawVal = rawDataZOH[c.index];
        if (rawVal !== null && rawVal !== undefined) {
          let physVal = toPhysical(rawVal, conv);
          let displayVal = formatPhysical(physVal, conv);
          let str = String(displayVal);

          if (
            showCursorLabels === "full" &&
            modeMapping[sig] &&
            modeMapping[sig][rawVal] !== undefined
          ) {
            str += ` : ${modeMapping[sig][rawVal]}`;
          }

          let mappedPhysVal = physVal;
          const inactiveText = inactive && inactiveLabel(inactive, sig, c.index);
          if (inactiveText) str = `— (${inactiveText})`;
          scatterData.push({
            value: [xDataText[c.index], mappedPhysVal, str],
            symbol: "circle",
            symbolSize: 6,
            itemStyle: { color: color, borderColor: theme === "dark" ? "#0f172a" : "#fff", borderWidth: 1 },
            label: {
              show: false,
              position: "right",
              distance: 10,
              formatter: (params: { value: any }) => params.value[2],
              color: theme === "dark" ? color : getDarkerColor(color),
              fontSize: 10,
              fontWeight: "bold",
              fontFamily: FONT_MONO,
              textBorderColor: theme === "dark" ? "#0f172a" : "#ffffff",
              textBorderWidth: 2,
              backgroundColor: theme === "dark" ? "rgba(0, 0, 0, 0.6)" : "rgba(255, 255, 255, 0.8)",
              padding: [2, 4],
              borderRadius: 2,
            },
          });
        }
      });
    }

    if (showSignalNames) {
      const displayName = (showAlias && set.alias) || displaySignalName(sig, activeSignals);
      let titleText = unit ? `${displayName} [${unit}]` : displayName;

      if (overlapTitleOffsets[gridIdx] === undefined) {
        overlapTitleOffsets[gridIdx] = 0;
      }

      const totalInGrid = overlapSignalCounts[gridIdx] || 1;
      const currentIndex = overlapTitleOffsets[gridIdx];
      overlapTitleOffsets[gridIdx] = currentIndex + 1;

      const LABEL_HEIGHT_PX = 14;
      const LABEL_GAP_PX = 4;
      const gridCenterYPx =
        (containerHeight * (set.top + set.height / 2)) / 100;
      const totalLabelsHeightPx =
        totalInGrid * LABEL_HEIGHT_PX + (totalInGrid - 1) * LABEL_GAP_PX;
      const startYPx = gridCenterYPx - totalLabelsHeightPx / 2;
      const titleYPx =
        startYPx + currentIndex * (LABEL_HEIGHT_PX + LABEL_GAP_PX);

      option.title.push({
        id: sig,
        triggerEvent: true,
        text: comparison?.summary?.[sig]?.presence === "bOnly" ? `${titleText}\nB のみ`
          : comparison?.summary?.[sig]?.presence === "aOnly" ? `${titleText}\nA のみ` : titleText,
        left: 5,
        top: titleYPx,
        z: 1,
        padding: 0,
        textBaseline: "top",
        textStyle: {
          fontSize: 10,
          color: color,
          fontWeight: "bold",
          textBorderColor: theme === "dark" ? "transparent" : "#ffffff",
          textBorderWidth: 2,
        },
      });
    }

    const laneHeightPx = containerHeight * (set.height / 100);
    let splitNum = 5;
    if (laneHeightPx < 30) splitNum = 1;
    else if (laneHeightPx < 60) splitNum = 2;
    else if (laneHeightPx < 100) splitNum = 3;

    let myIdxInGrid = Object.keys(sigToGridIndex)
      .filter((k) => sigToGridIndex[k] === gridIdx)
      .indexOf(sig);

    const isFirstInGrid = myIdxInGrid === 0;
    const hideYAxis = set.syncRange && !isFirstInGrid && set.overlapGroupTitle;

    let yAxisConfig: echarts.YAXisComponentOption & {
      breaks?: { start: number; end: number }[];
      breakArea?: { show: boolean; zigzagZ?: number };
    } = {
      gridIndex: gridIdx,
      type: "value",
      position: isFirstInGrid ? "left" : "right",
      offset: isFirstInGrid ? 0 : (myIdxInGrid - 1) * rightAxisWidth,
      show: !hideYAxis,
      splitLine: { show: false },
      axisLine: {
        show: true,
        lineStyle: { color: color, width: 2, opacity: 0.4 },
      },
      minInterval: 1,
      splitNumber: splitNum,
      axisLabel: {
        show: !hideYAxis,
        fontSize: 10,
        fontFamily: FONT_MONO,
        color: color,
        margin: 4,
        hideOverlap: true,
        formatter: (value: number) => String(Math.round(value)),
      },
    };

    if (set.syncRange) {
      yAxisConfig.min = set.syncRange.min;
      yAxisConfig.max = set.syncRange.max;
    } else if (set.yAuto) {
      yAxisConfig.min = finalMin;
      yAxisConfig.max = finalMax;
    } else {
      const range = manualRange(sig, set, { min: finalMin, max: finalMax });
      yAxisConfig.min = range.min;
      yAxisConfig.max = range.max;
    }

    if (isYBreakActive && activeBreak) {
      yAxisConfig.breaks = [{ start: activeBreak.start, end: activeBreak.end }];

      yAxisConfig.breakArea = { show: true, zigzagZ: 100 };
    }

    option.yAxis.push(yAxisConfig);

    if (customStateData.length > 0) {
      option.series.push({
        name: sig + "_state_label",
        type: "custom",
        xAxisIndex: gridIdx,
        yAxisIndex: realYAxisIdx,
        zlevel: 0,
        z: 11,
        encode: { x: -1, y: -1 },
        renderItem: function (
          params: echarts.CustomSeriesRenderItemParams,
          api: echarts.CustomSeriesRenderItemAPI,
        ) {
          var startIdx = api.value(0);
          var endIdx = api.value(1);
          var startCoord = api.coord([startIdx, 0]);
          var endCoord = api.coord([endIdx, 0]);
          var rect = params.coordSys as unknown as {
            x: number;
            y: number;
            width: number;
            height: number;
          };
          var visibleStart = Math.max(startCoord[0], rect.x);
          var visibleEnd = Math.min(
            endCoord[0],
            rect.x + Math.max(0, rect.width),
          );

          if (visibleStart >= visibleEnd) return;
          var centerX = (visibleStart + visibleEnd) / 2;

          var textStr = String(api.value(2));
          var paddingH = 8; // left 4 + right 4
          var paddingV = 4; // top 2 + bottom 2
          var maxWidth = visibleEnd - visibleStart - paddingH;
          var maxHeight = rect.height - paddingV;

          if (maxWidth < 5 || maxHeight < 10) return; // Too small

          const fitted = fitLabel(textStr, maxWidth, maxHeight);
          if (!fitted) return;
          const bestLines = fitted.text.split("\n");
          const bestFontSize = fitted.fontSize;
          const bestLineHeight = fitted.lineHeight;

          var baseColor = String(api.value(3));
          var textColor = theme === "dark" ? baseColor : getDarkerColor(baseColor, 0.6); // 60% darker for legibility

          return {
            type: "text",
            style: {
              text: bestLines.join("\n"),
              x: centerX,
              y: rect.y + rect.height / 2,
              fill: textColor,
              align: "center",
              verticalAlign: "middle",
              fontSize: bestFontSize,
              fontWeight: 600, // Make it slightly bolder for readability
              fontFamily: FONT_MONO,
              stroke: theme === "dark" ? "transparent" : "#ffffff",
              lineWidth: 2,
              backgroundColor: "transparent",
              padding: [2, 4],
              lineHeight: bestLineHeight,
            },
          };
        },
        data: customStateData,
        clip: true,
      });
    }

    if (markAreaData.length > 0) {
      option.series.push({
        name: sig + "_highlight",
        type: "line",
        xAxisIndex: gridIdx,
        yAxisIndex: realYAxisIdx,
        z: 12, // Higher z so markArea is above line series (z:10)
        data: [],
        markArea: {
          z: 12, // Also set inside markArea just in case
          itemStyle: { opacity: theme === "dark" ? 0.3 : 0.15 },
          data: markAreaData,
        },
        tooltip: { show: false },
      });
    }

    const baseWidth = set.lineWidth ?? 1.5;
    const baseType = set.lineType ?? (isExp ? "dashed" : "solid");

    if (hiddenRanges(sig).length) {
      option.visualMap.push({
        type: "piecewise",
        show: false,
        dimension: 0,
        seriesIndex: option.series.length,
        pieces: hiddenRanges(sig).map(([s, e]) => ({
          gte: s,
          lt: e,
        })),
        inRange: { colorAlpha: 0.3 },
        outOfRange: { colorAlpha: 1 },
      });
    }
    option.series.push({
      name: sig,
      type: "line",
      xAxisIndex: gridIdx,
      yAxisIndex: realYAxisIdx,
      z: 10,
      data: displayData,
      symbol: "none",
      showSymbol: false,
      step: "end",
      sampling: "none",
      itemStyle: { color: color },
      lineStyle: { width: baseWidth, type: baseType },
      emphasis: { focus: "none", lineStyle: { width: baseWidth } },
    });

    option.series.push({
      name: sig + "_mouse_tracker",
      type: "line",
      xAxisIndex: gridIdx,
      yAxisIndex: realYAxisIdx,
      z: 30,
      data: displayData,
      symbol: "none",
      showSymbol: false,
      step: "end",
      sampling: "none",
      itemStyle: { color: "transparent" },
      lineStyle: { width: 0, color: "transparent" },
      emphasis: {
        focus: "none",
        lineStyle: { width: 0 },
        label: {
          show: false, // Changed from true to false to let DOM handle it
          position: "right",
          distance: 2,
          offset: [0, -7],
          backgroundColor: theme === "dark" ? "rgba(30, 41, 59, 0.9)" : "rgba(255, 255, 255, 0.9)",
          padding: [2, 4],
          fontSize: 11,
          fontFamily: FONT_MONO,
          color: theme === "dark" ? color : getDarkerColor(color),
          formatter: (params: { value: any; dataIndex: number }) => {
            const inactiveText =
              inactive && inactiveLabel(inactive, sig, params.dataIndex);
            if (inactiveText) return `— (${inactiveText})`;
            let physiVal = params.value;
            let dVal = formatPhysical(physiVal, conv);
            let str = dVal + (unit ? " " + unit : "");
            let rawVal = rawDataZOH[params.dataIndex];
            if (
              showCursorLabels === "full" &&
              modeMapping[sig] &&
              rawVal !== null &&
              modeMapping[sig][rawVal] !== undefined
            )
              str += ` : ${modeMapping[sig][rawVal]}`;
            return str;
          },
        },
      },
    });

    if (scatterData.length > 0) {
      option.series.push({
        name: sig + "_cursor_values",
        type: "scatter",
        xAxisIndex: gridIdx,
        yAxisIndex: realYAxisIdx,
        z: 20,
        data: scatterData,
        animation: false,
        tooltip: { show: false },
      });
    }
    realYAxisIdx++;
  });

// cursorMarkLines moved to overlay

  const dummyGridIndex = gridGroups.length;
  option.grid.push({
    left: leftMargin,
    right: globalRightMargin,
    top: 15,
    bottom: Math.max(0, 100 - maxBottom) + "%",
    tooltip: { show: false },
  });
  option.xAxis.push({
    gridIndex: dummyGridIndex,
    type: "category",
    data: xDataText,
    boundaryGap: false,
    axisLine: { show: false },
    axisTick: { show: false },
    axisLabel: { show: false },
    splitLine: { show: false },
    axisPointer: {
      show: true,
      type: "line",
      z: 15,
      lineStyle: { width: 1.5, color: theme === "dark" ? "#cbd5e1" : "#888" },
      label: {
        show: true,
        fontSize: 10,
        fontFamily: FONT_MONO,
        backgroundColor: theme === "dark" ? "#475569" : "#555",
        formatter: (p: { value: any }) => p.value + " ms",
      },
    },
  });
  option.yAxis.push({
    gridIndex: dummyGridIndex,
    type: "value",
    show: false,
  });
  option.series.push({
    name: "dummy_for_pointer",
    type: "line",
    xAxisIndex: dummyGridIndex,
    yAxisIndex: option.yAxis.length - 1,
    z: 10,
    data: chartData.steps.map(() => 0),
    showSymbol: false,
    lineStyle: { width: 0, opacity: 0 },
    itemStyle: { opacity: 0 },
  });

  if (inactive?.show && gridGroups.length) {
    const gridIndex = option.grid.length;
    const xIndex = option.xAxis.length;
    const yIndex = option.yAxis.length;
    option.grid.push({
      left: leftMargin,
      right: globalRightMargin,
      top: fixedTopMarginPx,
      height: containerHeight * maxBottom / 100 - fixedTopMarginPx,
    });
    option.xAxis.push({
      gridIndex,
      type: "category",
      data: xDataText,
      boundaryGap: false,
      show: false,
      axisPointer: { show: false },
    });
    option.yAxis.push({
      gridIndex,
      type: "value",
      min: 0,
      max: 1,
      show: false,
    });
    inactive.perRule.forEach(({ rule, ranges }) => {
      const color = rule.color || INACTIVE_COLOR;
      option.series.push({
        name: "_inactive_" + rule.label,
        type: "custom",
        xAxisIndex: xIndex,
        yAxisIndex: yIndex,
        silent: true,
        clip: false,
        z: 15,
        encode: { x: -1, y: -1 },
        data: ranges.map(([start, end]) => [start, end]),
        renderItem: (params, api) => {
          const rect = params.coordSys as unknown as {
            x: number;
            y: number;
            width: number;
            height: number;
          };
          const start = api.coord([Number(api.value(0)), 0])[0];
          const endIdx = chartData.sourceSampleCount === undefined ? Number(api.value(1))
            : Math.min(Number(api.value(1)), chartData.sourceSampleCount - 1);
          // Category coordinates are indices; extrapolate the exclusive last boundary.
          const end =
            endIdx < xDataText.length
              ? api.coord([endIdx, 0])[0]
              : api.coord([endIdx - 1, 0])[0] +
                (api.size!([1, 0]) as number[])[0];
          const left = Math.max(rect.x, start);
          const right = Math.min(rect.x + rect.width, end);
          if (right <= left) return;
          const fitted = fitLabel(rule.label, right - left - 12, 18, false);
          const children: any[] = [
            {
              type: "rect",
              shape: {
                x: left,
                y: rect.y,
                width: right - left,
                height: rect.height,
              },
              style: { fill: color, opacity: 0.14 },
            },
            {
              type: "rect",
              shape: {
                x: left,
                y: rect.y - 18,
                width: right - left,
                height: 18,
                r: [3, 3, 0, 0],
              },
              style: { fill: color },
            },
          ];
          for (const x of [start, end]) {
            if (x >= rect.x && x <= rect.x + rect.width) {
              children.push({
                type: "line",
                shape: {
                  x1: x,
                  x2: x,
                  y1: rect.y,
                  y2: rect.y + rect.height,
                },
                style: { stroke: color, lineDash: [4, 3] },
              });
            }
          }
          if (fitted) {
            children.push({
              type: "text",
              style: {
                x: (left + right) / 2,
                y: rect.y - 9,
                text: fitted.text,
                fontSize: fitted.fontSize,
                fontFamily: FONT_MONO,
                fill: "#fff",
                align: "center",
                verticalAlign: "middle",
              },
            });
          }
          return { type: "group", children };
        },
      });
    });
  }

  if (comparison) {
    addCompareRendering(option, chartData, comparison, activeSignals, signalSettings,
      modeMapping, displayConversion, showMode, theme, containerHeight, taskPeriod);
  }

  // Cursor positions in the slider's data area below its 7px move handle (the
  // same rows as the diff bands). The slider always shows the whole recording;
  // z 5 keeps the lines above it.
  const cursorAxisSeries = option.series.find((s: any) => s.type === "line") as any;
  if (showMarkers && cursors.length > 0 && cursorAxisSeries && chartData.steps.length > 1) {
    const last = chartData.steps.length - 1;
    option.series.push({
      name: "_zoom_cursor_marks",
      type: "custom",
      xAxisIndex: cursorAxisSeries.xAxisIndex,
      yAxisIndex: cursorAxisSeries.yAxisIndex,
      silent: true,
      animation: false,
      clip: false,
      z: 5,
      encode: { x: -1, y: -1 },
      data: [0],
      renderItem: (params) => {
        const rect = params.coordSys as unknown as { x: number; width: number };
        return {
          type: "group",
          $mergeChildren: false,
          children: cursors.map((c) => {
            const x = Math.round(rect.x + Math.max(0, Math.min(1, c.index / last)) * rect.width) + 0.5;
            return {
              type: "line",
              shape: { x1: x, y1: containerHeight - 14, x2: x, y2: containerHeight },
              style: { stroke: theme === "dark" ? "#e2e8f0" : "#475569", lineWidth: 1.5 },
            };
          }),
        } as any;
      },
    });
  }

  const allZoomIndices = option.xAxis.map((_: unknown, idx: number) => idx);
  option.dataZoom = [
    {
      type: "slider",
      bottom: 5,
      xAxisIndex: allZoomIndices,
      height: 16,
      filterMode: "none",
      start: currentZoom.start,
      end: currentZoom.end,
      startValue: currentZoom.startValue,
      endValue: currentZoom.endValue,
    },
    {
      type: "inside",
      xAxisIndex: allZoomIndices,
      filterMode: "none",
      start: currentZoom.start,
      end: currentZoom.end,
      startValue: currentZoom.startValue,
      endValue: currentZoom.endValue,
    },
  ];

  return option;
}
