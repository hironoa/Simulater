import type { ChartData, CompareOptions, CompareResult, WaveformDiff } from "../types";
import type { InactiveResult } from "./inactiveRanges";

type Transition = { time: number; before: number | null; after: number | null };
type TimingPair = { tA: number; tB: number; start: number; end: number;
  before: number | null; after: number | null };

/** Hold CSV blanks exactly as in the waveform display, without changing raw data. */
function holdValues(values: (number | null)[]): (number | null)[] {
  let last: number | null = null;
  return values.map(value => value === null ? last : (last = value));
}

function inactiveMask(length: number, inactive: InactiveResult | null): boolean[] {
  const mask = new Array<boolean>(length).fill(false);
  // `show` controls visualization only; configured invalid samples stay excluded.
  for (const [start, end] of inactive?.union ?? []) {
    for (let i = Math.max(0, start); i < Math.min(length, end); i++) mask[i] = true;
  }
  return mask;
}

function transitions(times: number[], values: (number | null)[], invalid: boolean[]): Transition[] {
  const result: Transition[] = [];
  for (let i = 1; i < times.length; i++) {
    if (invalid[i - 1] || invalid[i] || values[i - 1] === undefined || values[i] === undefined) continue;
    if (values[i - 1] !== values[i]) {
      result.push({ time: times[i], before: values[i - 1], after: values[i] });
    }
  }
  return result;
}

function pairTransitions(a: Transition[], b: Transition[], tolerance: number): TimingPair[] {
  const key = (t: Transition) => JSON.stringify([t.before, t.after]);
  type Bucket = { edges: { edge: Transition; index: number }[]; next: number[]; previous: number[] };
  const buckets = new Map<string, Bucket>();
  b.forEach((edge, index) => {
    const k = key(edge);
    if (!buckets.has(k)) buckets.set(k, { edges: [], next: [], previous: [] });
    buckets.get(k)!.edges.push({ edge, index });
  });
  for (const bucket of buckets.values()) {
    bucket.next = Array.from({ length: bucket.edges.length + 1 }, (_, i) => i);
    bucket.previous = [...bucket.next];
  }
  // Disjoint successor/predecessor sets skip consumed B edges without rescanning.
  const find = (parents: number[], index: number): number => {
    let root = index;
    while (parents[root] !== root) root = parents[root];
    while (parents[index] !== index) {
      const next = parents[index];
      parents[index] = root;
      index = next;
    }
    return root;
  };
  // Fenwick prefix maxima / reversed prefix minima bound B by accepted A anchors.
  const maxB = new Array<number>(a.length + 1).fill(-1);
  const minB = new Array<number>(a.length + 1).fill(b.length);
  const nearest = (ai: number) => {
    const bucket = buckets.get(key(a[ai]));
    if (!bucket) return;
    let lower = -1, upper = b.length;
    for (let i = ai; i > 0; i -= i & -i) lower = Math.max(lower, maxB[i]);
    for (let i = a.length - ai - 1; i > 0; i -= i & -i) upper = Math.min(upper, minB[i]);
    const { edges, next, previous } = bucket;
    const lowerBound = (before: (item: typeof edges[number]) => boolean) => {
      let lo = 0, hi = edges.length;
      while (lo < hi) {
        const mid = (lo + hi) >>> 1;
        if (before(edges[mid])) lo = mid + 1; else hi = mid;
      }
      return lo;
    };
    const first = lowerBound(item => item.index <= lower);
    const end = lowerBound(item => item.index < upper);
    const pivot = Math.max(first, Math.min(end, lowerBound(item => item.edge.time < a[ai].time)));
    const left = find(previous, pivot) - 1;
    const right = find(next, pivot);
    let bi = -1, distance = Infinity;
    for (const index of [left, right]) {
      if (index < first || index >= end) continue;
      const gap = Math.abs(edges[index].edge.time - a[ai].time);
      if (gap <= tolerance && gap < distance) { bi = index; distance = gap; }
    }
    if (bi >= 0) return { ai, bi, bucket, distance };
  };
  type Candidate = NonNullable<ReturnType<typeof nearest>>;
  const heap: Candidate[] = [];
  const precedes = (x: Candidate, y: Candidate) =>
    x.distance < y.distance || (x.distance === y.distance && (x.ai < y.ai || (x.ai === y.ai && x.bi < y.bi)));
  const push = (candidate: Candidate) => {
    let i = heap.length;
    heap.push(candidate);
    while (i > 0) {
      const parent = (i - 1) >>> 1;
      if (!precedes(candidate, heap[parent])) break;
      heap[i] = heap[parent];
      i = parent;
    }
    heap[i] = candidate;
  };
  const pop = () => {
    const first = heap[0], last = heap.pop()!;
    if (heap.length) {
      let i = 0;
      while (2 * i + 1 < heap.length) {
        let child = 2 * i + 1;
        if (child + 1 < heap.length && precedes(heap[child + 1], heap[child])) child++;
        if (!precedes(heap[child], last)) break;
        heap[i] = heap[child];
        i = child;
      }
      heap[i] = last;
    }
    return first;
  };
  for (let ai = 0; ai < a.length; ai++) {
    const candidate = nearest(ai);
    if (candidate) push(candidate);
  }
  const pairs: TimingPair[] = [];
  while (heap.length) {
    const candidate = pop();
    // Bounds only shrink and B edges only disappear. Refresh stale heap entries
    // lazily; their distance cannot decrease, so global closest-first is preserved.
    const current = nearest(candidate.ai);
    if (!current) continue;
    if (current.bi !== candidate.bi) { push(current); continue; }
    const { ai, bi, bucket } = current;
    const bj = bucket.edges[bi].index;
    for (let i = ai + 1; i <= a.length; i += i & -i) maxB[i] = Math.max(maxB[i], bj);
    for (let i = a.length - ai; i <= a.length; i += i & -i) minB[i] = Math.min(minB[i], bj);
    bucket.next[bi] = find(bucket.next, bi + 1);
    bucket.previous[bi + 1] = find(bucket.previous, bi);
    const tA = a[ai].time, tB = bucket.edges[bi].edge.time;
    // Equal-time anchors still prevent later matches from crossing them.
    if (tA !== tB) pairs.push({ tA, tB, start: Math.min(tA, tB), end: Math.max(tA, tB),
      before: a[ai].before, after: a[ai].after });
  }
  return pairs.sort((x, y) => x.start - y.start || x.end - y.end);
}

/** Compare exact shared sample times in ms, without resampling or converting raw values.
 * Inputs use ascending step order, as produced by the CSV parser.
 * Missing timestamps and invalid samples break intervals. No duration is invented
 * after the last sample. B index coordinates are fractional between samples.
 */
export function compareWaveforms(
  a: ChartData,
  b: ChartData,
  taskPeriod: number,
  modeMapping: Record<string, Record<number, string>>,
  inactiveA: InactiveResult | null,
  inactiveB: InactiveResult | null,
  options: CompareOptions = {},
): CompareResult {
  const result: CompareResult = { diffs: [], summary: {}, totalValueDurationMs: 0 };
  const signals = [...new Set([...Object.keys(a.seriesMap), ...Object.keys(b.seriesMap)])];
  for (const signal of signals) {
    const hasA = Object.hasOwn(a.seriesMap, signal), hasB = Object.hasOwn(b.seriesMap, signal);
    result.summary[signal] = { presence: hasA && hasB ? "both" : hasA ? "aOnly" : "bOnly",
      valueCount: 0, timingCount: 0 };
  }
  if (!Number.isFinite(taskPeriod) || taskPeriod <= 0) return result;
  const requestedTolerance = options.timingToleranceMs ?? 500;
  const tolerance = Number.isFinite(requestedTolerance) ? Math.max(0, requestedTolerance) : 500;
  const timesA = a.steps.map((step) => step * taskPeriod);
  const timesB = b.steps.map((step) => step * taskPeriod);
  const indexA = new Map(timesA.map((time, index) => [time, index]));
  const indexB = new Map(timesB.map((time, index) => [time, index]));
  const times = [...new Set([...timesA, ...timesB])].sort((x, y) => x - y);
  const lastCommonTime = Math.min(timesA.at(-1) ?? -Infinity, timesB.at(-1) ?? -Infinity);
  const invalidA = inactiveMask(timesA.length, inactiveA);
  const invalidB = inactiveMask(timesB.length, inactiveB);
  // Coordinates are evaluated once, shared by all signals.
  let rightB = 0;
  const coordinates = times.map((time) => {
    while (rightB < timesB.length && timesB[rightB] < time) rightB++;
    if (rightB === 0) return 0;
    if (rightB === timesB.length) return Math.max(0, timesB.length - 1);
    return rightB - 1 + (time - timesB[rightB - 1]) / (timesB[rightB] - timesB[rightB - 1]);
  });

  for (const signal of signals) {
    if (result.summary[signal].presence !== "both") continue;
    const valuesA = holdValues(a.seriesMap[signal]), valuesB = holdValues(b.seriesMap[signal]);
    const pairs = Object.values(modeMapping[signal] ?? {}).some((name) => name.trim() !== "")
      ? pairTransitions(transitions(timesA, valuesA, invalidA), transitions(timesB, valuesB, invalidB), tolerance)
      : [];
    let pairIndex = 0;
    const active = new Set<TimingPair>();
    const countedPairs = new Set<TimingPair>();
    let current: WaveformDiff | null = null;
    let currentPair: TimingPair | undefined;
    for (let i = 0; i < times.length; i++) {
      const time = times[i];
      while (pairIndex < pairs.length && pairs[pairIndex].start <= time) active.add(pairs[pairIndex++]);
      for (const pair of active) if (pair.end <= time) active.delete(pair);
      const ia = indexA.get(time), ib = indexB.get(time);
      if (ia === undefined || ib === undefined || invalidA[ia] || invalidB[ib] ||
          valuesA[ia] === undefined || valuesB[ib] === undefined || valuesA[ia] === valuesB[ib]) {
        current = null;
        currentPair = undefined;
        continue;
      }
      let pair: TimingPair | undefined;
      for (const candidate of active) {
        const delayed = candidate.tA < candidate.tB;
        if (valuesA[ia] === (delayed ? candidate.after : candidate.before) &&
            valuesB[ib] === (delayed ? candidate.before : candidate.after)) {
          pair = candidate;
          break;
        }
      }
      const type = pair ? "timing" : "value";
      const end = Math.min(times[i + 1] ?? time, lastCommonTime);
      if (end <= time) {
        current = null;
        currentPair = undefined;
        continue;
      }
      const endIndex = coordinates[i + 1];
      if (current && current.end === time && current.type === type && currentPair === pair) {
        current.end = end;
        current.endIndex = endIndex;
      } else {
        const base = { signal, start: time, end, startIndex: coordinates[i], endIndex };
        current = pair
          ? { ...base, type: "timing", tA: pair.tA, tB: pair.tB, shiftMs: pair.tB - pair.tA }
          : { ...base, type: "value" };
        result.diffs.push(current);
        currentPair = pair;
        if (pair) {
          if (!countedPairs.has(pair)) {
            countedPairs.add(pair);
            result.summary[signal].timingCount++;
          }
        } else result.summary[signal].valueCount++;
      }
      if (type === "value") result.totalValueDurationMs += end - time;
    }
  }
  result.diffs.sort((x, y) => x.start - y.start || x.end - y.end || x.signal.localeCompare(y.signal));
  return result;
}
