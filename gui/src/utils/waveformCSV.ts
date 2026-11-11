import Papa from "papaparse";

export function decodeWaveformCSV(text: string) {
  const cleanText = text
    .split(/\r?\n/)
    .filter((line) => !line.trim().startsWith(";"))
    .join("\n");
  const parsed = Papa.parse<string[]>(cleanText, { skipEmptyLines: true });
  const headerIdx = parsed.data.findIndex((row) => row[0] === "#COMMENT");
  const rawHeaders = parsed.data[headerIdx] ?? [];
  const dataRows = parsed.data.slice(headerIdx + 1);
  const headers: string[] = [];
  const colIndices: number[] = [];
  for (let i = 1; i < rawHeaders.length; i++) {
    if (rawHeaders[i] && rawHeaders[i].trim() !== "") {
      let h = rawHeaders[i].trim();
      h = h.includes("/") ? h.split("/").pop()! : h;
      headers.push(h);
      colIndices.push(i);
    }
  }
  const seriesMap: Record<string, (number | null)[]> = {};
  headers.forEach((h) => (seriesMap[h] = []));
  const steps: number[] = [];

  dataRows.forEach((row, idx) => {
    if (row.length < 2) return;
    steps.push(idx + 1);
    colIndices.forEach((colIdx, i) => {
      let textVal = row[colIdx];
      if (!textVal) {
        seriesMap[headers[i]].push(null);
        return;
      }
      textVal = textVal.trim();
      if (textVal === "") {
        seriesMap[headers[i]].push(null);
        return;
      }
      let val = Number(textVal);
      seriesMap[headers[i]].push(isNaN(val) ? null : val);
    });
  });

  return { chartData: { seriesMap, steps }, headers, headerIdx, errors: parsed.errors };
}

