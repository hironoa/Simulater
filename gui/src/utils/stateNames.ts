import { colorPalette } from "./helpers";

export type StateRow = { value: string; name: string };
export type StateMapping = Record<number, string>;

// 状態の背景色は値から決まる（チャートの帯と同じ規則）。
export function getStateColor(value: number) {
  return colorPalette[Number.isFinite(value) ? Math.abs(Math.floor(value)) % colorPalette.length : 0];
}

export function mappingToRows(mapping: StateMapping | undefined): StateRow[] {
  return Object.entries(mapping ?? {})
    .map(([value, name]) => ({ value, name }))
    .sort((a, b) => Number(a.value) - Number(b.value));
}

// 値・状態名ともに空の行は入力途中として無視する。状態名だけ空の行は保存しない。
const isBlank = (row: StateRow) => row.value.trim() === "" && row.name.trim() === "";

export function rowsToMapping(rows: StateRow[]): StateMapping {
  const mapping: StateMapping = {};
  for (const row of rows) {
    if (isBlank(row) || row.name.trim() === "") continue;
    mapping[Number(row.value)] = row.name;
  }
  return mapping;
}

export function stateRowErrors(rows: StateRow[]): (string | undefined)[] {
  const errors: (string | undefined)[] = rows.map(() => undefined);
  const seen = new Map<number, number>();
  rows.forEach((row, i) => {
    if (isBlank(row)) return;
    const value = row.value.trim();
    if (value === "" || !Number.isFinite(Number(value))) { errors[i] = "数値を入力してください"; return; }
    const prev = seen.get(Number(value));
    if (prev !== undefined) errors[i] = errors[prev] = "値が重複しています";
    else seen.set(Number(value), i);
  });
  return errors;
}

export function sameMapping(a: StateMapping | undefined, b: StateMapping | undefined) {
  return serializeStates(a) === serializeStates(b);
}

// 一覧編集のセル・TSV 用。1 行に「値=状態名」を書く（Excel では 1 セル内の改行）。
// 状態名中の改行は区切りと区別するため \n とエスケープする。
const escapeName = (name: string) => name.replace(/\\/g, "\\\\").replace(/\n/g, "\\n").replace(/\r/g, "\\r");
const unescapeName = (name: string) => name.replace(/\\([\\nr])/g, (_, c) => c === "n" ? "\n" : c === "r" ? "\r" : "\\");

export function serializeStates(mapping: StateMapping | undefined) {
  return mappingToRows(mapping).map(row => `${row.value}=${escapeName(row.name)}`).join("\n");
}

export function parseStates(text: string): StateMapping | null {
  const rows: StateRow[] = [];
  for (const line of text.split(/\r?\n/)) {
    if (line.trim() === "") continue;
    const eq = line.indexOf("=");
    if (eq < 0) return null;
    rows.push({ value: line.slice(0, eq).trim(), name: unescapeName(line.slice(eq + 1).trim()) });
  }
  if (rows.some(row => row.value === "") || stateRowErrors(rows).some(Boolean)) return null;
  return rowsToMapping(rows);
}
