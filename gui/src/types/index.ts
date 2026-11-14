export type InactiveCondition = number | { min?: number; max?: number };
export interface InactiveRule { signal: string; when: InactiveCondition[]; label: string; color?: string }
export interface InactiveConfig { show: boolean; rules: InactiveRule[] }

export interface YBreak {
  start: number;
  end: number;
}

export interface SignalSetting {
  id: string;
  color: string;
  lineWidth?: number;
  lineType?: "solid" | "dashed" | "dotted";
  weight?: number;
  manualWeight?: number;
  top?: number;
  height?: number;
  expanded: boolean;
  yAuto: boolean;
  yMin: number | null;
  yMax: number | null;
  checked: boolean;
  overlapGroupTitle?: string | null;
  syncRange?: { min: number; max: number; yBreak?: YBreak | null } | null;
  alias?: string;
  yBreak?: boolean;
  yBreakStart?: number | null;
  yBreakEnd?: number | null;
}

export interface SignalGroup {
  id: string;
  name: string;
  expanded: boolean;
  visible: boolean;
  layout: "lane" | "overlap";
  syncRange: boolean;
  signals: string[];
}

export interface ChartData {
  seriesMap: Record<string, (number | null)[]>;
  steps: number[];
  /** Presentation-only boundary: appended comparison times have no B samples. */
  sourceSampleCount?: number;
}

/** Times are milliseconds; index coordinates interpolate between B samples.
 * Intervals are [start, end); zero-duration differences are omitted.
 */
export type WaveformDiff = {
  signal: string;
  start: number;
  end: number;
  startIndex: number;
  endIndex: number;
} & ({ type: "value" } | { type: "timing"; tA: number; tB: number; shiftMs: number });

export interface SignalDiffSummary {
  presence: "both" | "aOnly" | "bOnly";
  valueCount: number;
  timingCount: number;
}

export interface CompareOptions {
  timingToleranceMs?: number;
}

export interface CompareResult {
  diffs: WaveformDiff[];
  summary: Record<string, SignalDiffSummary>;
  /** Sum across signals, so simultaneous differences count separately. */
  totalValueDurationMs: number;
}

export interface Cursor {
  time: number;
  index: number;
}

export interface SignalSettingsSections {
  display: boolean;
  conversion: boolean;
  states: boolean;
  yScale: boolean;
  yBreak: boolean;
}

export interface AppState {
  inactive: InactiveConfig;
  chartData: ChartData | null;
  compareChartData: ChartData | null;
  compareFileName: string | null;
  compareRawCSVText: string | null;
  compareEnabled: boolean;
  diffs: WaveformDiff[];
  diffSummary: Record<string, SignalDiffSummary>;
  totalValueDurationMs: number;
  diffFilter: "all" | "value";
  showOnlyDiffSignals: boolean;
  focusedDiffIndex: number;
  signalSettings: Record<string, SignalSetting>;
  signalSettingsSections: SignalSettingsSections;
  signalGroups: SignalGroup[];
  unassignedSignals: string[];
  unassignedGroupConfig: Omit<SignalGroup, "signals">;
  cursors: Cursor[];

  // Mapping
  modeMapping: Record<string, Record<number, string>>;
  lsbMapping: Record<string, string>;
  offsetMapping: Record<string, string>;
  unitMapping: Record<string, string>;
  undefinedMapping: Record<string, number[]>;

  // View Settings
  valueDisplayMode: "physical" | "raw";
  fileName: string | null;
  taskPeriod: number;
  showMode: "off" | "bg-only" | "full";
  showSignalNames: boolean;
  showAlias: boolean;
  showMarkers: boolean;
  showCursorLabels: "off" | "value-only" | "full";
  showTable: boolean;
  tablePosition: "bottom" | "side";
  isTableTransposed: boolean;
  activeSidebarTab: "sig" | "cursor";
  isSidebarOpen: boolean;
  hoveredSignal: string | null;
  hoverCursor: { time: number; index: number } | null;
  autoAdjustTrigger: number;
  layoutMode: "uniform" | "optimized" | "manual";
  theme: "light" | "dark";
  tableContentWidth: number | null;
  rawCSVText: string | null;
}
