import { seriesNames } from "../utils/waveformCSV";
import type { LiveLayout } from "./persist";

export type WatchLayout = Pick<LiveLayout, "watch" | "inputs">;

export function watchedNames({ watch, inputs }: WatchLayout): string[] {
  const names = [...watch.map(w => w.name), ...inputs.filter(i => i.monitor !== false).map(i => i.name)];
  return [...new Set(names.filter(Boolean))];
}

export function watchedSeriesNames(layout: WatchLayout): Map<string, string> {
  const names = watchedNames(layout);
  const series = seriesNames(names);
  return new Map(names.map((name, i) => [name, series[i]]));
}

export function shortSeriesName(name: string): string {
  return name.slice(name.lastIndexOf("/") + 1) || name;
}
