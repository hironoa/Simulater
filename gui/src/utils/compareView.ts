import type { ChartData, SignalDiffSummary, InactiveConfig, SignalGroup } from "../types";

import { computeInactive } from "./inactiveRanges";

const extendedSteps = new WeakMap<ChartData, WeakMap<ChartData, number[]>>();

/** Append A's tail only; B's existing category indices must never move. */
export function compareDisplaySteps(b: ChartData, a: ChartData): number[] {
  if (!b.steps.length || !a.steps.length || a.steps[a.steps.length - 1] <= b.steps[b.steps.length - 1]) return b.steps;
  let cache = extendedSteps.get(b);
  if (!cache) { cache = new WeakMap(); extendedSteps.set(b, cache); }
  let steps = cache.get(a);
  if (!steps) {
    steps = [...b.steps, ...a.steps.filter(step => step > b.steps[b.steps.length - 1])];
    cache.set(a, steps);
  }
  return steps;
}

const inactiveCache = new WeakMap<ChartData, { config: InactiveConfig; result: ReturnType<typeof computeInactive> }>();
export function compareInactive(data: ChartData, config: InactiveConfig) {
  const cached = inactiveCache.get(data);
  if (cached?.config === config) return cached.result;
  const result = computeInactive(data, config);
  inactiveCache.set(data, { config, result });
  return result;
}

const heldValues = new WeakMap<ChartData, Map<string, (number | null)[]>>();

/** Match actual sample times; hold null values only within an existing CSV. */
export function compareSample(data: ChartData | null, signal: string, step: number) {
  if (!data?.seriesMap[signal] || !Number.isFinite(step)) return { index: -1, raw: null };
  let lo = 0, hi = data.steps.length - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >>> 1;
    if (data.steps[mid] < step) lo = mid + 1;
    else if (data.steps[mid] > step) hi = mid - 1;
    else {
      if (mid >= (data.sourceSampleCount ?? data.steps.length)) return { index: -1, raw: null };
      let cache = heldValues.get(data);
      if (!cache) { cache = new Map(); heldValues.set(data, cache); }
      let values = cache.get(signal);
      if (!values) {
        let last: number | null = null;
        values = data.seriesMap[signal].map(value => value == null ? last : (last = value));
        cache.set(signal, values);
      }
      return { index: mid, raw: values[mid] ?? null };
    }
  }
  return { index: -1, raw: null };
}

export function hasSignalDiff(summary: SignalDiffSummary | undefined, filter: "all" | "value") {
  return !!summary && (summary.presence !== "both" || summary.valueCount > 0 ||
    (filter === "all" && summary.timingCount > 0));
}

/** Resolve presentation membership without changing the stored groups. */
export function compareSignalLayout(groups: SignalGroup[], unassigned: string[], aOnly: string[]) {
  const seen = new Set<string>();
  const unique = (signals: string[]) => signals.filter(signal => {
    if (seen.has(signal)) return false;
    seen.add(signal);
    return true;
  });
  const signalGroups = groups.map(group => ({ ...group, signals: unique(group.signals) }));
  const unassignedSignals = unique([...unassigned, ...aOnly]);
  return { signalGroups, unassignedSignals, signals: [...seen] };
}
