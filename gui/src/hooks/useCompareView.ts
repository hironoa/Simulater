import { useMemo } from "react";
import { useShallow } from "zustand/react/shallow";
import { useWaveformStore } from "../store/useWaveformStore";
import { colorPalette } from "../utils/helpers";
import { hasSignalDiff, compareSignalLayout, compareDisplaySteps } from "../utils/compareView";
import type { SignalSetting } from "../types";

/** Temporary presentation settings for A-only signals; never modify B's settings. */
export function useCompareView() {
  const state = useWaveformStore(useShallow(s => ({
    chartData: s.chartData, compareChartData: s.compareChartData, compareEnabled: s.compareEnabled,
    signalSettings: s.signalSettings, signalGroups: s.signalGroups, unassignedSignals: s.unassignedSignals,
    showOnlyDiffSignals: s.showOnlyDiffSignals, diffSummary: s.diffSummary, diffFilter: s.diffFilter,
  })));
  return useMemo(() => {
    const enabled = state.compareEnabled && !!state.compareChartData;
    const aOnly = enabled ? Object.keys(state.compareChartData!.seriesMap)
      .filter(sig => !state.chartData?.seriesMap[sig]) : [];
    const settings = { ...state.signalSettings };
    for (const [i, sig] of aOnly.entries()) settings[sig] ??= {
      id: sig, color: colorPalette[(Object.keys(state.signalSettings).length + i) % colorPalette.length],
      checked: true, expanded: false, yAuto: true, yMin: null, yMax: null,
    } satisfies SignalSetting;
    let chartData = state.chartData;
    if (enabled && chartData) {
      const steps = compareDisplaySteps(chartData, state.compareChartData!);
      const extra = steps.length - chartData.steps.length;
      if (extra || aOnly.length) chartData = {
        ...chartData, steps,
        ...(extra ? { sourceSampleCount: chartData.steps.length } : {}),
        seriesMap: {
          ...(extra ? Object.fromEntries(Object.entries(chartData.seriesMap)
            .map(([sig, values]) => [sig, [...values, ...Array<number | null>(extra).fill(null)]])) : chartData.seriesMap),
          ...Object.fromEntries(aOnly.map(sig => [sig, steps.map(() => null)])),
        },
      };
    }
    // Keep stored membership intact; the non-comparison view can only render B's signals.
    const hasBSignal = (sig: string) => Object.hasOwn(state.chartData?.seriesMap ?? {}, sig);
    const groups = enabled ? state.signalGroups : state.signalGroups.map(group => ({
      ...group, signals: group.signals.filter(hasBSignal),
    }));
    const unassigned = enabled ? state.unassignedSignals : state.unassignedSignals.filter(hasBSignal);
    const layout = compareSignalLayout(groups, unassigned, aOnly);
    return { groups: layout.signalGroups, signals: layout.signals, enabled, aOnly, settings: enabled ? settings : state.signalSettings, chartData, summary: state.diffSummary,
      unassigned: layout.unassignedSignals,
      visible: (sig: string) => !enabled || !state.showOnlyDiffSignals || hasSignalDiff(state.diffSummary[sig], state.diffFilter),
    };
  }, [state]);
}
