import Papa from "papaparse";

// sim_csv.c の first_key と同じく、前後の空白（全角を含む）と外側の引用を除く。
const firstKey = (row: string[]) => (row[0] ?? "").trim().replace(/^"(.*)"$/s, "$1").trim();
const rowKind = (row: string[]) => firstKey(row).toLowerCase();
const isHeader = (row: string[]) => rowKind(row).startsWith("#comment");

/** `mod,関数名,題名,入力数,出力数` の入力数・出力数。読めなければ null。 */
function readModCounts(row: string[] | undefined): { inputs: number; outputs: number } | null {
  if (!row || rowKind(row) !== "mod") return null;
  const count = (cell: string | undefined) => {
    const text = cell?.trim() ?? "";
    return /^\d+$/.test(text) ? Number(text) : null;
  };
  const inputs = count(row[3]);
  const outputs = count(row[4]);
  return inputs === null || outputs === null ? null : { inputs, outputs };
}

/**
 * 同じ見出しが複数ある列から 1 列だけ採る。
 * `mod` 行で出力側の範囲が分かれば出力側の列（実測値）を、分からなければ最後の列を採る。
 */
function pickColumns(
  columns: { name: string; colIdx: number }[],
  isOutput: (colIdx: number) => boolean,
) {
  const byName = new Map<string, number[]>();
  for (const { name, colIdx } of columns) {
    const list = byName.get(name);
    if (list) list.push(colIdx);
    else byName.set(name, [colIdx]);
  }
  return [...byName].map(([name, indices]) => {
    const outputs = indices.filter(isOutput);
    const pool = outputs.length ? outputs : indices;
    return { name, colIdx: pool[pool.length - 1] };
  });
}

/**
 * パスを除いた名前（最後の `/` 以降）を系列名にする。それが他の列と衝突する列だけ、
 * 区別がつく最短の末尾（`engine.c/s_cnt` など）を系列名にする。
 */
function seriesNames(fullNames: string[]): string[] {
  const segments = fullNames.map((name) => name.split("/"));
  const suffix = (segs: string[], k: number) => segs.slice(Math.max(0, segs.length - k)).join("/");
  const shortCount = new Map<string, number>();
  for (const segs of segments) {
    const s = suffix(segs, 1);
    shortCount.set(s, (shortCount.get(s) ?? 0) + 1);
  }
  return segments.map((segs, i) => {
    const short = suffix(segs, 1);
    if (shortCount.get(short) === 1) return short;
    const rivals = segments.filter((other, j) => j !== i && suffix(other, 1) === short);
    for (let k = 2; k <= segs.length; k++) {
      const mine = suffix(segs, k);
      if (rivals.every((other) => suffix(other, k) !== mine)) return mine;
    }
    return fullNames[i];
  });
}

/** セルを数値にする。結果 CSV の `実測?(期待)` は実測側を採る。読めなければ null。 */
function cellValue(cell: string | undefined): number | null {
  let text = cell?.trim() ?? "";
  const mismatch = /^(.*?)\?\(.*\)$/s.exec(text);
  if (mismatch) text = mismatch[1].trim();
  if (text === "") return null;
  const val = Number(text);
  return Number.isNaN(val) ? null : val;
}

/**
 * 波形 CSV を読む。各系列にはデータ行ごとに値を 1 個だけ入れる（どの系列も steps と同じ長さ）。
 * 見出しが完全に同じ列は 1 列だけ採る（`pickColumns`）。判定・時間の列は見出しがないので読まない。
 */
export function decodeWaveformCSV(text: string) {
  const parsed: { data: string[][]; errors: Papa.ParseError[] } = { data: [], errors: [] };
  let cursor = 0;
  Papa.parse<string[]>(text, {
    skipEmptyLines: true,
    step(result) {
      const raw = text.slice(cursor, result.meta.cursor).replace(/\r?\n$/, "");
      cursor = result.meta.cursor;
      parsed.errors.push(...result.errors);
      // sim_csv.c の all_empty は生のセルを見る。引用された空文字 ("") は空行ではない。
      if (/^[ \t\u3000,\r\n]*$/.test(raw) || firstKey(result.data).startsWith(";")) return;
      parsed.data.push(result.data);
    },
  });
  const modIndices = parsed.data.flatMap((row, i) => rowKind(row) === "mod" ? [i] : []);
  const commentIndices = parsed.data.flatMap((row, i) => isHeader(row) ? [i] : []);
  const blockCount = modIndices.length || commentIndices.length;
  const end = (modIndices.length ? modIndices[1] : commentIndices[1]) ?? parsed.data.length;
  const start = modIndices[0] ?? 0;
  const headerIdx = parsed.data.findIndex((row, i) => i >= start && i < end && isHeader(row));
  const rawHeaders = parsed.data[headerIdx] ?? [];
  const dataRows = headerIdx < 0 ? [] : parsed.data.slice(headerIdx + 1, end);
  const warnings = blockCount > 1
    ? [`CSV にブロックが ${blockCount} 個あります。先頭のブロックだけを表示し、2 つ目以降は表示しません。`]
    : [];

  const columns: { name: string; colIdx: number }[] = [];
  for (let i = 1; i < rawHeaders.length; i++) {
    const name = rawHeaders[i]?.trim();
    if (name) columns.push({ name, colIdx: i });
  }
  const mod = headerIdx > 0 ? readModCounts(parsed.data[headerIdx - 1]) : null;
  const isOutput = (colIdx: number) =>
    !!mod && colIdx > mod.inputs && colIdx <= mod.inputs + mod.outputs;
  const picked = pickColumns(columns, isOutput);
  const headers = seriesNames(picked.map((c) => c.name));

  const seriesMap: Record<string, (number | null)[]> = {};
  headers.forEach((h) => (seriesMap[h] = []));
  const steps: number[] = [];

  dataRows.forEach((row) => {
    const key = firstKey(row);
    if (key.startsWith(";") || key.startsWith("#")) return;
    steps.push(steps.length + 1);
    picked.forEach(({ colIdx }, i) => seriesMap[headers[i]].push(cellValue(row[colIdx])));
  });

  return { chartData: { seriesMap, steps }, headers, headerIdx, warnings, errors: parsed.errors };
}
