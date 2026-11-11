import { decodeWaveformCSV } from "./waveformCSV";

/** Compare exact text prefixes, allowing the last physical line to be unfinished. */
export function classifyReload(previousText: string, nextText: string): "append" | "restart" | "incomplete" {
  try {
    const next = decodeWaveformCSV(nextText);
    if (next.headerIdx < 0 || next.errors.length || !next.chartData.steps.length) return "incomplete";
    const previous = decodeWaveformCSV(previousText);
    // Ignore a trailing newline when locating the last (possibly unfinished) line.
    const end = previousText.replace(/(?:\r?\n)$/, "");
    const lastNewline = end.lastIndexOf("\n");
    const prefix = lastNewline < 0 ? "" : end.slice(0, lastNewline + 1);
    return nextText.startsWith(prefix) && next.chartData.steps.length >= previous.chartData.steps.length
      ? "append" : "restart";
  } catch {
    return "incomplete";
  }
}
