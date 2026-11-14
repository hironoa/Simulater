import { clsx, type ClassValue } from "clsx";
import { twMerge } from "tailwind-merge";

export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}

export const colorPalette = [
  "#5470c6",
  "#91cc75",
  "#fac858",
  "#ee6666",
  "#73c0de",
  "#3ba272",
  "#fc8452",
  "#9a60b4",
  "#ea7ccc",
];

export function getDarkerColor(hex: string, factor = 0.25): string {
  if (!hex || hex.length < 7) return "#333";
  let r = parseInt(hex.slice(1, 3), 16);
  let g = parseInt(hex.slice(3, 5), 16);
  let b = parseInt(hex.slice(5, 7), 16);
  r = Math.floor(r * (1 - factor));
  g = Math.floor(g * (1 - factor));
  b = Math.floor(b * (1 - factor));
  return `rgb(${r}, ${g}, ${b})`;
}

export function evaluateExpression(expr: string): number | null {
  if (expr.trim() === "") return null;
  const expression = expr.trim().toLowerCase().replace(/\^/g, "**");
  try {
    const value = new Function("return (" + expression + ")")();
    return typeof value === "number" && Number.isFinite(value) ? value : null;
  } catch {
    return null;
  }
}

export function parseLSB(expr: string | number | undefined | null): number {
  if (expr === undefined || expr === null || expr === "") return 1;
  let s = String(expr).trim().toLowerCase();
  s = s.replace(/\^/g, "**");
  try {
    const val = new Function("return (" + s + ")")();
    return isNaN(val) || val === 0 ? 1 : val;
  } catch (e) {
    console.warn("LSB解析エラー:", expr);
    return 1;
  }
}

export function parseOffset(expr: string | number | undefined | null): number {
  if (expr === undefined || expr === null || String(expr).trim() === "") return 0;
  const s = String(expr).trim().toLowerCase().replace(/\^/g, "**");
  try {
    const val = new Function("return (" + s + ")")();
    return typeof val === "number" && Number.isFinite(val) ? val : 0;
  } catch (e) {
    console.warn("オフセット解析エラー:", expr);
    return 0;
  }
}

export function getConversion(
  sig: string,
  lsbMapping: Record<string, string>,
  offsetMapping: Record<string, string>,
): { lsb: number; offset: number } {
  return { lsb: parseLSB(lsbMapping[sig]), offset: parseOffset(offsetMapping[sig]) };
}

// Display-only conversion; stored settings and Sidebar keep physical units.
export function getDisplayConversion(
  sig: string,
  lsbMapping: Record<string, string>,
  offsetMapping: Record<string, string>,
  valueDisplayMode: "physical" | "raw",
): { lsb: number; offset: number } {
  return valueDisplayMode === "raw"
    ? { lsb: 1, offset: 0 }
    : getConversion(sig, lsbMapping, offsetMapping);
}

// State signals carry no unit in raw mode: "[raw]" is noise there, and a physical unit would be wrong.
export function getDisplayUnit(
  sig: string,
  unitMapping: Record<string, string>,
  modeMapping: Record<string, Record<number, string>>,
  valueDisplayMode: "physical" | "raw",
): string {
  if (valueDisplayMode === "physical") return unitMapping[sig] || "";
  const hasStates = !!modeMapping[sig] && Object.keys(modeMapping[sig]).length > 0;
  return hasStates ? "" : "raw";
}

export function toPhysical(raw: number, conv: { lsb: number; offset: number }): number {
  const value = raw * conv.lsb;
  return conv.offset === 0 ? value : value + conv.offset;
}

export function formatPhysical(value: number, conv: { lsb: number; offset: number }): number {
  return conv.lsb !== 1 || conv.offset !== 0 ? Number(value.toPrecision(7)) : value;
}

export function calculateAutoYRange(
  sig: string,
  data: (number | null)[],
  lsbExpr: string | undefined,
  offsetExpr: string | undefined,
  undefinedVals: number[] = [],
  isYBreakActive: boolean = true,
): { min: number; max: number; breakStart?: number; breakEnd?: number } {
  const conv = getConversion(sig, { [sig]: lsbExpr }, { [sig]: offsetExpr });
  const { lsb } = conv;
  if (!data || data.length === 0) return { min: 0, max: 1 };

  const normalData = data.filter(
    (v) => v !== null && !isNaN(v) && !undefinedVals.includes(v),
  ) as number[];

  const undefinedData = data.filter(
    (v) => v !== null && !isNaN(v) && undefinedVals.includes(v),
  ) as number[];

  const useData =
    normalData.length > 0
      ? normalData
      : (data.filter((v) => v !== null && !isNaN(v)) as number[]);
  if (useData.length === 0) return { min: 0, max: 1 };

  let min = Infinity;
  let max = -Infinity;
  for (const rawVal of useData) {
    const physVal = toPhysical(rawVal, conv);
    if (physVal < min) min = physVal;
    if (physVal > max) max = physVal;
  }
  if (min === Infinity) return { min: 0, max: 1 };

  let diff = max - min;
  if (diff === 0) diff = lsb !== 1 ? lsb : 1;

  let normalMin = min - diff * 0.05;
  let normalMax = max + diff * 0.05;

  if (min >= 0 && normalMin < 0) normalMin = 0;
  if (max <= 0 && normalMax > 0) normalMax = 0;

  if (isYBreakActive && undefinedData.length > 0) {
    let outMin = Infinity;
    let outMax = -Infinity;
    const undefPhys = undefinedData.map((v) => toPhysical(v, conv));
    for (const physVal of undefPhys) {
      if (physVal < outMin) outMin = physVal;
      if (physVal > outMax) outMax = physVal;
    }

    let finalMin = normalMin;
    let finalMax = normalMax;
    let b = computeAutoBreak(normalMin, normalMax, undefPhys);

    if (outMax > normalMax) {
      finalMax = outMax + diff * 0.05;
    }
    if (outMin < normalMin) {
      finalMin = outMin - diff * 0.05;
    }

    return {
      min: finalMin,
      max: finalMax,
      breakStart: b ? b.start : undefined,
      breakEnd: b ? b.end : undefined,
    };
  } else {
    // If Break is disabled, we just show normal range.
    return {
      min: normalMin,
      max: normalMax,
    };
  }
}

import { YBreak } from "../types";

export function computeAutoBreak(
  normalMin: number,
  normalMax: number,
  undefinedVals: number[],
): YBreak | null {
  if (!undefinedVals || undefinedVals.length === 0) return null;
  const diff = Math.max(normalMax - normalMin, 1);
  const margin = diff * 0.1;
  const pad = diff * 0.05;

  const outOfUpper = undefinedVals.filter((v) => v > normalMax + margin);
  const outOfLower = undefinedVals.filter((v) => v < normalMin - margin);

  if (outOfUpper.length > 0) {
    const minOutOfUpper = Math.min(...outOfUpper);
    const maxUV = Math.max(...undefinedVals);
    return {
      start: normalMax + pad,
      end: minOutOfUpper - pad,
    };
  } else if (outOfLower.length > 0) {
    const maxOutOfLower = Math.max(...outOfLower);
    const minUV = Math.min(...undefinedVals);
    return {
      start: maxOutOfLower + pad,
      end: normalMin - pad,
    };
  }
  return null;
}

export function detectUndefinedValue(
  data: (number | null)[],
  explicitUndefinedVals?: number[],
): number[] | null {
  if (!data || data.length === 0) return null;

  if (explicitUndefinedVals && explicitUndefinedVals.length > 0) {
    const presentVals = explicitUndefinedVals.filter((u) => data.includes(u));
    return presentVals.length > 0 ? presentVals : null;
  }

  const KNOWN_UNDEFINED = new Set([
    255, 32767, 65535, 2147483647, 4294967295, -1, -32768, -2147483648,
  ]);
  const validData = data.filter((v) => v !== null && !isNaN(v)) as number[];
  if (validData.length === 0) return null;

  // Loops instead of Math.max(...data): spreading 100k+ samples overflows the call stack.
  let max = -Infinity;
  let min = Infinity;
  for (const v of validData) {
    if (v > max) max = v;
    if (v < min) min = v;
  }
  if (!KNOWN_UNDEFINED.has(max)) return null;

  const sorted = [...new Set(validData)].sort((a, b) => b - a);
  if (sorted.length < 2) return null;

  const secondMax = sorted[1];
  const normalRange = secondMax - min;
  if (max - secondMax > Math.max(normalRange * 10, 1000)) {
    return [max];
  }
  return null;
}
