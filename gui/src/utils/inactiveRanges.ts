import { ChartData, InactiveConfig, InactiveRule } from "../types";

export type IndexRange = [number, number];
export const INACTIVE_COLOR = "#6b7280";

export function parseInactive(input: unknown): InactiveConfig {
  const object =
    input && typeof input === "object"
      ? (input as Record<string, unknown>)
      : {};
  const entries = Array.isArray(input)
    ? input
    : Array.isArray(object.rules)
      ? object.rules
      : [object];
  const rules: InactiveRule[] = [];

  for (const entry of entries) {
    if (
      !entry ||
      typeof entry !== "object" ||
      typeof entry.signal !== "string"
    ) {
      continue;
    }
    const when = (
      Array.isArray(entry.when) ? entry.when : [entry.when]
    ).filter((v: unknown) => {
      if (typeof v === "number") return Number.isFinite(v);
      if (!v || typeof v !== "object") return false;
      const { min, max } = v as { min?: unknown; max?: unknown };
      return (
        (min !== undefined || max !== undefined) &&
        (min === undefined ||
          (typeof min === "number" && Number.isFinite(min))) &&
        (max === undefined ||
          (typeof max === "number" && Number.isFinite(max))) &&
        !(typeof min === "number" && typeof max === "number" && min > max)
      );
    });
    rules.push({
      signal: entry.signal,
      when,
      label:
        typeof entry.label === "string" ? entry.label : `${entry.signal} 無効`,
      ...(typeof entry.color === "string" ? { color: entry.color } : {}),
    });
  }
  return { show: object.show !== false, rules };
}

export function computeInactive(data: ChartData | null, config: InactiveConfig) {
  const perRule = config.rules.map((rule) => {
    const ranges: IndexRange[] = [];
    const values = data?.seriesMap[rule.signal];
    let start = -1;
    let v: number | null = null;

    const sampleCount = data?.sourceSampleCount ?? data?.steps.length ?? 0;
    for (let i = 0; i <= sampleCount; i++) {
      if (values?.[i] != null) v = values[i];
      const hit =
        i < sampleCount &&
        v != null &&
        rule.when.some((c) =>
          typeof c === "number"
            ? v === c
            : (c.min !== undefined || c.max !== undefined) &&
              (c.min === undefined || v >= c.min) &&
              (c.max === undefined || v <= c.max),
        );
      if (hit && start < 0) start = i;
      if (!hit && start >= 0) {
        ranges.push([start, i]);
        start = -1;
      }
    }
    return {
      rule,
      ranges,
      steps: ranges.reduce((n, [s, e]) => n + e - s, 0),
    };
  });
  const union: IndexRange[] = [];
  for (const [s, e] of perRule
    .flatMap((r) => r.ranges)
    .sort((a, b) => a[0] - b[0])) {
    const last = union[union.length - 1];
    if (last && s <= last[1]) last[1] = Math.max(last[1], e);
    else union.push([s, e]);
  }
  return {
    perRule,
    union,
    conditions: new Set(config.rules.map((r) => r.signal)),
    show: config.show && config.rules.length > 0,
  };
}

export type InactiveResult = ReturnType<typeof computeInactive>;

export function inactiveLabel(result: InactiveResult, sig: string, index: number) {
  if (!result.show || result.conditions.has(sig)) return "";
  return result.perRule
    .filter((r) => r.ranges.some(([s, e]) => index >= s && index < e))
    .map((r) => r.rule.label)
    .join(", ");
}

export function subtractInactive(
  start: number,
  end: number,
  ranges: IndexRange[],
): IndexRange[] {
  const result: IndexRange[] = [];
  let pos = start;
  for (const [s, e] of ranges) {
    if (e <= pos) continue;
    if (s >= end) break;
    if (s > pos) result.push([pos, s]);
    pos = Math.max(pos, e);
  }
  if (pos < end) result.push([pos, end]);
  return result;
}
