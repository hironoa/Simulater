import { persistLiveLayout, useVariantsStore } from "../live/variantsStore";
import { emptyDefs, emptyView, readSignalDocument, readDefs, legacyView, effectiveMappings, writeEffective, defFromMappings, dumpSignals, unique, type DefOverride, type SignalDefs, type SignalView } from "./signalDefs";
import { useLiveStore } from "../live/liveStore";
import { listedSignals } from "../utils/displaySignalName";
import { create } from "zustand";
import {
  AppState,
  SignalSetting,
  SignalGroup,
  Cursor,
  ChartData,
} from "../types";
import { colorPalette, calculateAutoYRange, detectUndefinedValue, getConversion, parseOffset } from "../utils/helpers";
import Papa from "papaparse";
import yaml from "js-yaml";
import toast from "react-hot-toast";
import { getCompareCSVPath, readWcpProject } from "../utils/wcp";
import { compareWaveforms } from "../utils/compare";
import { compareDisplaySteps } from "../utils/compareView";
import { computeInactive, parseInactive } from "../utils/inactiveRanges";
import { InactiveConfig } from "../types";
import { decodeWaveformCSV } from "../utils/waveformCSV";
import { liveRecord, type AppendResult, type LiveRecord } from "../live/liveRecord";

/** ライブ記録を Viewer に渡したときのファイル名表示。 */
export const LIVE_FILE_NAME = "ライブ記録";

// Swaps restore the view explicitly after both CSVs have been parsed.
let swappingCompare = false;

function mergeDefinitions<T extends DefOverride>(previous: Record<string, T>, incoming: Record<string, T>): Record<string, T> {
  const merged = { ...previous };
  for (const [sig, def] of Object.entries(incoming)) {
    merged[sig] = { ...previous[sig], ...def,
      ...(def.states !== undefined ? { states: { ...previous[sig]?.states, ...def.states } } : {}),
    };
  }
  return merged;
}

function mergeViewEntries(previous: SignalView["signals"], incoming: SignalView["signals"]): SignalView["signals"] {
  const merged = { ...previous };
  for (const [name, fields] of Object.entries(incoming)) merged[name] = { ...previous[name], ...fields };
  return merged;
}

export interface WaveformFileHandle extends FileSystemFileHandle {
  queryPermission: (options: { mode: "read" }) => Promise<PermissionState>;
  requestPermission: (options: { mode: "read" }) => Promise<PermissionState>;
}

type SignalConversionPatch = {
  lsb?: string;
  offset?: string;
  unit?: string;
  undefinedValues?: number[];
  states?: Record<number, string>;
};

interface WaveformStore extends AppState {
  signalDefs: SignalDefs;
  signalView: SignalView;
  selectedVariant: string;
  setSelectedVariant: (variant: string) => void;
  renameVariant: (oldName: string, newName: string) => void;
  setVariantOverride: (sig: string, enabled: boolean) => void;
  setInactive: (config: InactiveConfig) => void;
  inactiveEditorOpen: boolean;
  openInactiveEditor: (open: boolean) => void;
  setTaskPeriod: (period: number) => void;
  toggleValueDisplayMode: () => void;
  toggleShowMode: () => void;
  cycleSignalDisplay: () => void;
  toggleShowMarkers: () => void;
  toggleShowCursorLabels: () => void;
  zoomRange: {
    start: number;
    end: number;
    startValue?: string | number;
    endValue?: string | number;
  } | null;
  setZoomRange: (zoom: {
    start: number;
    end: number;
    startValue?: string | number;
    endValue?: string | number;
  } | null) => void;
  toggleTableDisplay: () => void;
  setTablePosition: (pos: "bottom" | "side") => void;
  toggleTableTranspose: () => void;
  setActiveSidebarTab: (tab: "sig" | "cursor") => void;
  setIsSidebarOpen: (isOpen: boolean) => void;

  // Comparison (A); the existing chartData/rawCSVText remain B.
  loadCompareCSV: (text: string, filename: string) => boolean;
  compareHandle: WaveformFileHandle | null;
  compareFileVersion: { lastModified: number; size: number } | null;
  setCompareHandle: (handle: WaveformFileHandle | null, file?: File) => void;
  swapCompare: () => boolean;
  closeCompare: () => void;
  setCompareEnabled: (enabled: boolean) => void;
  setDiffFilter: (filter: AppState["diffFilter"]) => void;
  setShowOnlyDiffSignals: (enabled: boolean) => void;
  setFocusedDiffIndex: (index: number) => void;
  promoteBToA: () => boolean;

  // Live (sim gui)
  /** `samples` を記録に追記する。描画の更新は記録の revision を見て間引いて行う（ここでは状態を変えない）。 */
  appendSamples: (step0: number, names: readonly string[], rows: readonly (readonly unknown[])[]) => AppendResult;
  /** 記録の全区間を chartData に載せ、従来の Viewer で操作できるようにする。 */
  commitLiveRecord: (options?: { windowSize?: number; record?: LiveRecord }) => void;
  /** 監視変数の信号設定を用意する。既存の checked は保持する。 */
  ensureLiveSignals: (names: readonly string[], plotted: ReadonlySet<string>) => void;

  // Data Handling
  parseWaveformCSV: (text: string, filename: string, options?: { preserveView?: boolean }) => boolean;
  waveformHandle: WaveformFileHandle | null;
  waveformFileVersion: { lastModified: number; size: number } | null;
  setWaveformHandle: (handle: WaveformFileHandle | null, file?: File) => void;
  keepPreviousAsA: boolean;
  setKeepPreviousAsA: (enabled: boolean) => void;
  autoReload: boolean;
  setAutoReload: (enabled: boolean) => void;
  parseMappingData: (
    text: string,
    format: "csv" | "yaml",
    keepExisting?: boolean,
    options?: { reapply?: boolean; keepListed?: boolean },
  ) => void;
  getConfigYAML: (includeViewState?: boolean) => string;
  loadWcpProject: (file: File) => Promise<void>;
  configHandle: unknown | null;
  setConfigHandle: (handle: unknown | null) => void;

  // Signal Settings
  toggleSignalSettingsSection: (key: keyof AppState["signalSettingsSections"]) => void;
  setSignalConversion: (sig: string, patch: SignalConversionPatch) => void;
  setSignalConversions: (patches: Record<string, SignalConversionPatch>) => void;
  updateSignalSetting: (sig: string, updates: Partial<SignalSetting>) => void;
  toggleSignalCheck: (sig: string) => void;
  toggleAllSignals: (check: boolean) => void;
  cycleLayoutMode: () => void;

  // Group Management
  addGroup: (name: string) => void;
  renameGroup: (id: string, name: string) => void;
  toggleGroupExpand: (id: string | "unassigned") => void;
  toggleGroupVisibility: (id: string | "unassigned", visible: boolean) => void;
  setGroupLayout: (
    id: string | "unassigned",
    layout: "lane" | "overlap",
  ) => void;
  toggleGroupSync: (id: string | "unassigned") => void;
  setGroups: (groups: SignalGroup[], unassigned: string[]) => void;

  // Cursors
  addCursor: (time: number, index: number) => void;
  removeCursor: (index: number) => void;
  clearAllCursors: () => void;

  setHoveredSignal: (sig: string | null) => void;
  setHoverCursor: (cursor: { time: number; index: number } | null) => void;
  toggleTheme: () => void;
  setTableContentWidth: (width: number | null) => void;
}

function serializeConversionExpression(expr: string): string | number {
  const value = Number(expr);
  return Number.isFinite(value) && String(value) === expr.trim() ? value : expr;
}

function recalculateGroupSyncRange(
  state: AppState,
  targetSignals: string[],
  newSettings: Record<string, SignalSetting>,
): void {
  let gMin = Infinity;
  let gMax = -Infinity;
  targetSignals.forEach((sig) => {
    let r = { min: 0, max: 1 };
    if (state.chartData?.seriesMap[sig]) {
      const undefinedVals = detectUndefinedValue(state.chartData.seriesMap[sig], state.undefinedMapping[sig]);
      r = calculateAutoYRange(
        sig,
        state.chartData.seriesMap[sig],
        state.lsbMapping[sig],
        state.offsetMapping[sig],
        undefinedVals || [],
        false,
      );
    }
    const set = newSettings[sig];
    if (set && !set.yAuto) {
      if (set.yMin !== null) r.min = set.yMin;
      if (set.yMax !== null) r.max = set.yMax;
    }
    if (r.min < gMin) gMin = r.min;
    if (r.max > gMax) gMax = r.max;
  });

  if (gMin !== Infinity && targetSignals.length > 0) {
    const fMin = Number(gMin.toPrecision(4));
    const fMax = Number(gMax.toPrecision(4));
    targetSignals.forEach((sig) => {
      if (newSettings[sig]) {
        newSettings[sig] = {
          ...newSettings[sig],
          yMin: fMin,
          yMax: fMax,
        };
      }
    });
  }
}

const initialState: AppState = {
  inactive: { show: true, rules: [] },
  chartData: null,
  compareChartData: null,
  compareFileName: null,
  compareRawCSVText: null,
  compareEnabled: false,
  diffs: [],
  diffSummary: {},
  totalValueDurationMs: 0,
  diffFilter: "all",
  showOnlyDiffSignals: false,
  focusedDiffIndex: -1,
  signalSettings: {},
  signalSettingsSections: { display: true, conversion: true, states: false, yScale: false, yBreak: false },
  signalGroups: [],
  unassignedSignals: [],
  unassignedGroupConfig: {
    id: "unassigned",
    name: "未分類",
    expanded: true,
    visible: true,
    layout: "lane",
    syncRange: false,
  },
  cursors: [],
  modeMapping: {},
  lsbMapping: {},
  offsetMapping: {},
  unitMapping: {},
  undefinedMapping: {},
  fileName: null,
  taskPeriod: 5,
  valueDisplayMode: "physical",
  showMode: "full",
  showSignalNames: true,
  showAlias: false,
  showMarkers: true,
  showCursorLabels: "full",
  showTable: true,
  tablePosition: "side",
  isTableTransposed: true,
  activeSidebarTab: "cursor",
  isSidebarOpen: true,
  hoveredSignal: null,
  hoverCursor: null,
  autoAdjustTrigger: 0,
  layoutMode: "uniform",
  theme: "light",
  tableContentWidth: null,
  rawCSVText: null,
};

export const useWaveformStore = create<WaveformStore>((set, get) => ({
  ...initialState,
  signalDefs: emptyDefs(),
  signalView: emptyView(),
  selectedVariant: "",
  setSelectedVariant: (selectedVariant) => {
    const state = get();
    if (selectedVariant === state.selectedVariant) return;
    set({ selectedVariant, ...effectiveMappings(state.signalDefs, selectedVariant), autoAdjustTrigger: state.autoAdjustTrigger + 1 });
    if (useLiveStore.getState().mode === "offline") {
      try { localStorage.setItem("sim-gui:signals-variant:v1", selectedVariant); } catch { /* unavailable */ }
    }
  },
  renameVariant: (oldName, newName) => {
    if (!oldName || !newName || oldName === newName) return;
    const state = get();
    const variants = { ...state.signalDefs.variants };
    if (Object.hasOwn(variants, oldName)) {
      variants[newName] = { ...variants[newName], ...variants[oldName] };
      delete variants[oldName];
    }
    const signalDefs = { ...state.signalDefs, variants,
      variantNames: unique(state.signalDefs.variantNames.map(name => name === oldName ? newName : name)) };
    const selectedVariant = state.selectedVariant === oldName ? newName : state.selectedVariant;
    set({ signalDefs, selectedVariant, ...effectiveMappings(signalDefs, selectedVariant), autoAdjustTrigger: state.autoAdjustTrigger + 1 });
    if (useLiveStore.getState().mode === "offline") {
      try {
        if (localStorage.getItem("sim-gui:signals-variant:v1") === oldName) localStorage.setItem("sim-gui:signals-variant:v1", newName);
      } catch { /* unavailable */ }
    }
  },
  setVariantOverride: (sig, enabled) => {
    const { selectedVariant: variant, signalDefs } = get();
    if (!variant) return;
    const overrides = { ...signalDefs.variants[variant] };
    if (enabled) {
      if (overrides[sig] !== undefined) return;
      overrides[sig] = {};
    } else delete overrides[sig];
    const next = { ...signalDefs, variants: { ...signalDefs.variants, [variant]: overrides }, variantNames: unique([...signalDefs.variantNames, variant]) };
    set({ signalDefs: next, ...effectiveMappings(next, variant) });
  },
  loadWcpProject: async (file) => {
    const { csvContent, csvName, yamlContent, compareContent, compareName } = await readWcpProject(file);
    if (!csvContent) {
      alert(".wcpファイルの中に有効な波形データ(CSV)が見つかりませんでした。");
      return;
    }
    if (!get().parseWaveformCSV(csvContent, csvName)) return;
    get().closeCompare();
    if (compareContent !== null) get().loadCompareCSV(compareContent, compareName);
    if (yamlContent) setTimeout(() => get().parseMappingData(yamlContent, "yaml", false), 0);
  },
  loadCompareCSV: (text, filename) => {
    const { chartData, headerIdx, errors, warnings } = decodeWaveformCSV(text);
    if (headerIdx < 0 || errors.length > 0) {
      toast.error("比較するCSVを開けません: CSVの形式または#COMMENT行を確認してください。");
      return false;
    }
    warnings.forEach((warning) => toast(warning, { duration: 10_000 }));
    set({ compareChartData: chartData, compareFileName: filename,
      compareRawCSVText: text, compareHandle: null, compareFileVersion: null, compareEnabled: true, focusedDiffIndex: -1 });
    return true;
  },
  compareHandle: null,
  compareFileVersion: null,
  setCompareHandle: (handle, file) => set({ compareHandle: handle,
    compareFileVersion: file ? { lastModified: file.lastModified, size: file.size } : null }),
  swapCompare: () => {
    const old = get();
    if (!old.compareChartData || old.compareRawCSVText === null || old.rawCSVText === null || !old.chartData) return false;
    const decoded = decodeWaveformCSV(old.compareRawCSVText);
    const previousB = decodeWaveformCSV(old.rawCSVText);
    if (decoded.headerIdx < 0 || decoded.errors.length || previousB.headerIdx < 0 || previousB.errors.length) return false;
    const steps = old.compareEnabled ? compareDisplaySteps(old.chartData, old.compareChartData) : old.chartData.steps;
    const timeAtPercent = (percent: number) => {
      const index = percent / 100 * Math.max(0, steps.length - 1);
      const lo = Math.floor(index), hi = Math.ceil(index);
      return ((steps[lo] ?? 1) + ((steps[hi] ?? steps[lo] ?? 1) - (steps[lo] ?? 1)) * (index - lo) - 1) * old.taskPeriod;
    };
    const boundTime = (value: number | string | undefined, percent: number) =>
      typeof value === "string" && Number.isFinite(Number(value)) ? Number(value)
        : typeof value === "number" ? ((steps[value] ?? 1) - 1) * old.taskPeriod : timeAtPercent(percent);
    const startTime = boundTime(old.zoomRange?.startValue, old.zoomRange?.start ?? 0);
    const endTime = boundTime(old.zoomRange?.endValue, old.zoomRange?.end ?? 100);
    swappingCompare = true;
    try {
      set({ compareChartData: previousB.chartData, compareRawCSVText: old.rawCSVText,
        compareFileName: old.fileName, compareHandle: old.waveformHandle,
        compareFileVersion: old.waveformFileVersion });
      get().parseWaveformCSV(old.compareRawCSVText, old.compareFileName ?? "", { preserveView: true });
      const next = get();
      const nextSteps = next.compareEnabled ? compareDisplaySteps(next.chartData!, next.compareChartData!) : next.chartData!.steps;
      const first = ((nextSteps[0] ?? 1) - 1) * old.taskPeriod;
      const last = ((nextSteps[nextSteps.length - 1] ?? 1) - 1) * old.taskPeriod;
      const percent = (time: number) => last > first ? Math.max(0, Math.min(100, (time - first) / (last - first) * 100)) : 0;
      set({ waveformHandle: old.compareHandle, waveformFileVersion: old.compareFileVersion,
        signalSettings: { ...old.signalSettings, ...next.signalSettings },
        autoReload: old.autoReload && !!old.compareHandle, focusedDiffIndex: -1, hoverCursor: null,
        zoomRange: { start: percent(startTime), end: last > first ? percent(endTime) : 100,
          startValue: undefined, endValue: undefined },
        cursors: old.cursors.flatMap(cursor => {
          const index = nextSteps.findIndex(step => Math.abs((step - 1) * old.taskPeriod - cursor.time) < 1e-8);
          return index < 0 ? [] : [{ ...cursor, index }];
        }) });
    } finally { swappingCompare = false; }
    return true;
  },
  closeCompare: () => set({ compareHandle: null, compareFileVersion: null, compareChartData: null, compareFileName: null,
    compareRawCSVText: null, compareEnabled: false, diffs: [], diffSummary: {},
    totalValueDurationMs: 0, diffFilter: "all", showOnlyDiffSignals: false, focusedDiffIndex: -1 }),
  setCompareEnabled: (enabled) => set({ compareEnabled: enabled && !!get().compareChartData,
    focusedDiffIndex: -1 }),
  setDiffFilter: (diffFilter) => set({ diffFilter, focusedDiffIndex: -1 }),
  setShowOnlyDiffSignals: (showOnlyDiffSignals) => set({ showOnlyDiffSignals }),
  setFocusedDiffIndex: (index) => {
    const { diffs, diffFilter } = get();
    const count = diffs.filter((d) => diffFilter === "all" || d.type === "value").length;
    set({ focusedDiffIndex: Number.isInteger(index) && index >= 0 && index < count ? index : -1 });
  },
  promoteBToA: () => {
    const { rawCSVText, fileName, waveformHandle, waveformFileVersion } = get();
    if (rawCSVText === null || !get().loadCompareCSV(rawCSVText, fileName ?? "")) return false;
    set({ compareHandle: waveformHandle, compareFileVersion: waveformFileVersion });
    return true;
  },
  inactiveEditorOpen: false,
  setInactive: (inactive) => set({ inactive }),
  openInactiveEditor: (open) =>
    set(
      open
        ? { inactiveEditorOpen: true, isSidebarOpen: true, activeSidebarTab: "sig" }
        : { inactiveEditorOpen: false },
    ),

  setTableContentWidth: (width) => set({ tableContentWidth: width }),

  setTaskPeriod: (period) => set({ taskPeriod: period }),
  toggleValueDisplayMode: () => set((state) => ({
    valueDisplayMode: state.valueDisplayMode === "physical" ? "raw" : "physical",
  })),
  toggleShowMode: () => set((state) => ({ 
    showMode: state.showMode === "off" ? "bg-only" : state.showMode === "bg-only" ? "full" : "off" 
  })),
  cycleSignalDisplay: () =>
    set((state) => {
      if (state.showSignalNames && !state.showAlias) {
        return { showSignalNames: true, showAlias: true };
      } else if (state.showSignalNames && state.showAlias) {
        return { showSignalNames: false, showAlias: false };
      } else {
        return { showSignalNames: true, showAlias: false };
      }
    }),
  toggleShowMarkers: () =>
    set((state) => ({ showMarkers: !state.showMarkers })),
  toggleShowCursorLabels: () =>
    set((state) => ({
      showCursorLabels:
        state.showCursorLabels === "off"
          ? "value-only"
          : state.showCursorLabels === "value-only"
            ? "full"
            : "off",
    })),
  zoomRange: null,
  setZoomRange: (zoom) => set({ zoomRange: zoom }),
  toggleTableDisplay: () => {
    const { tablePosition, showTable } = get();
    if (tablePosition === "side") {
      set({
        tablePosition: "bottom",
        showTable: true,
        activeSidebarTab: "sig",
        isTableTransposed: false,
      });
    } else {
      set({ showTable: !showTable });
    }
  },
  setTablePosition: (pos) => {
    if (pos === "side") {
      set({
        tablePosition: pos,
        showTable: true,
        activeSidebarTab: "cursor",
        isTableTransposed: true,
      });
    } else {
      set({
        tablePosition: pos,
        activeSidebarTab: "sig",
        isTableTransposed: false,
      });
    }
  },
  toggleTableTranspose: () =>
    set((state) => ({ isTableTransposed: !state.isTableTransposed })),
  setActiveSidebarTab: (tab) => set({ activeSidebarTab: tab }),
  setIsSidebarOpen: (isOpen) => set({ isSidebarOpen: isOpen }),

  waveformHandle: null,
  waveformFileVersion: null,
  setWaveformHandle: (handle, file) => set({
    waveformHandle: handle,
    waveformFileVersion: file ? { lastModified: file.lastModified, size: file.size } : null,
    autoReload: false,
  }),
  keepPreviousAsA: false,
  setKeepPreviousAsA: (keepPreviousAsA) => set({ keepPreviousAsA }),
  autoReload: false,
  setAutoReload: (enabled) => set({ autoReload: enabled && !!get().waveformHandle }),

  appendSamples: (step0, names, rows) => liveRecord.append(step0, names, rows),

  commitLiveRecord: (options) => {
    const state = get();
    const record = options?.record ?? liveRecord;
    const names = record.names;
    const chartData = record.toChartData(names);
    const len = chartData.steps.length;
    const win = options?.windowSize ? Math.max(2, options.windowSize) : 0;
    const zoomRange = win && len > 1 && win < len
      ? { start: ((len - win) / (len - 1)) * 100, end: 100, startValue: undefined, endValue: undefined }
      : { start: 0, end: 100, startValue: undefined, endValue: undefined };
    get().ensureLiveSignals(names, new Set(names.filter((n) => state.signalSettings[n]?.checked ?? true)));
    set({
      chartData,
      fileName: LIVE_FILE_NAME,
      rawCSVText: null,
      waveformHandle: null,
      waveformFileVersion: null,
      autoReload: false,
      // 古い側を捨てると添字がずれるので、カーソルは時刻（= ステップ）で引き直す
      cursors: state.fileName === LIVE_FILE_NAME ? state.cursors.flatMap((c) => {
        const step = Math.round(c.time / state.taskPeriod) + 1;
        let lo = 0, hi = len - 1;
        while (lo <= hi) {
          const mid = (lo + hi) >> 1;
          if (chartData.steps[mid] === step) return [{ ...c, index: mid }];
          if (chartData.steps[mid] < step) lo = mid + 1; else hi = mid - 1;
        }
        return [];
      }) : [],
      hoverCursor: null,
      zoomRange,
    });
  },

  ensureLiveSignals: (names, plotted) => {
    const state = get();
    const settings = { ...state.signalSettings };
    let changed = false;
    let count = Object.keys(settings).length;
    for (const name of names) {
      const checked = plotted.has(name);
      if (!settings[name]) {
        settings[name] = {
          id: "sig_" + count++,
          color: colorPalette[count % colorPalette.length],
          weight: 1,
          expanded: false,
          yAuto: true,
          yMin: null,
          yMax: null,
          checked,
          yBreak: true,
          yBreakStart: null,
          yBreakEnd: null,
        };
        changed = true;
      }
    }
    const assigned = new Set(state.signalGroups.flatMap((g) => g.signals));
    const known = new Set(state.unassignedSignals);
    const additions = names.filter((n) => !assigned.has(n) && !known.has(n));
    if (!changed && additions.length === 0) return;
    const config = get().getConfigYAML();
    set({
      signalSettings: settings,
      ...(additions.length ? { unassignedSignals: [...state.unassignedSignals, ...additions] } : {}),
    });
    if (additions.some(name => state.signalView.signals[name])) get().parseMappingData(config, "yaml", false, { reapply: true, keepListed: true });
  },

  parseWaveformCSV: (text, filename, options) => {
    const { chartData, headers, headerIdx, errors, warnings } = decodeWaveformCSV(text);
    const { steps } = chartData;
    if (options?.preserveView && errors.length > 0) {
      toast.error("再読み込みを見送りました: CSVの形式が不正です。", { id: "waveform-reload-parse" });
      return false;
    }

    if (headerIdx === -1) {
      if (options?.preserveView) {
        toast.error("再読み込みを見送りました: #COMMENT行が見つかりません。", { id: "waveform-reload-parse" });
      } else {
        alert("エラー: #COMMENT行が見つかりません。");
      }
      return false;
    }

    warnings.forEach((warning) => toast(warning, { duration: 10_000 }));

    // Capture all settings before narrowing the current CSV's list.
    const savedConfig = !options?.preserveView ? get().getConfigYAML() : null;
    // Existing settings
    const existingSettings = get().signalSettings;
    const newSettings: Record<string, SignalSetting> = {};
    let sigCount = Object.keys(existingSettings).length;

    for (const h of headers) {
      if (existingSettings[h]) {
        newSettings[h] = existingSettings[h];
      } else {
        newSettings[h] = {
          id: "sig_" + sigCount++,
          color: colorPalette[sigCount % colorPalette.length],
          weight: 1,
          expanded: false,
          yAuto: true,
          yMin: null,
          yMax: null,
          checked: true,
          yBreak: true,
          yBreakStart: null,
          yBreakEnd: null,
        };
      }
    }

    const { signalGroups } = get();
    const assignedSignals = new Set<string>();
    signalGroups.forEach((g) =>
      g.signals.forEach((s) => assignedSignals.add(s)),
    );

    const unassignedSignals = headers.filter((h) => !assignedSignals.has(h));
    const { compareEnabled, compareChartData } = get();
    const displaySteps = compareEnabled && compareChartData
      ? compareDisplaySteps(chartData, compareChartData) : steps;

    set({
      chartData,
      signalSettings: newSettings,
      fileName: filename,
      unassignedSignals,
      cursors: options?.preserveView ? get().cursors.filter((cursor) => cursor.index < displaySteps.length) : [],
      rawCSVText: text,
      ...(!options?.preserveView ? { waveformHandle: null, waveformFileVersion: null, autoReload: false } : {}),
    });
    if (savedConfig) get().parseMappingData(savedConfig, "yaml", false, { reapply: true });
    return true;
  },

  parseMappingData: (text, format, keepExisting = false, options) => {
    let importedDefs: SignalDefs | null = null;
    let importedView: SignalView | null = null;
    let legacyDefinitions = format !== "yaml";
    let selectedVariant = useLiveStore.getState().mode !== "offline" ? useVariantsStore.getState().doc.selected : get().selectedVariant;
    if (format === "yaml") {
      try {
        const original = readSignalDocument(text);
        if (original.version === 2) {
          importedDefs = readDefs(original);
          importedView = { signals: original.view?.signals ?? {}, groups: original.view?.groups ?? {} };
          if (!options?.reapply && useLiveStore.getState().mode === "offline" && typeof original.viewState?.variant === "string") selectedVariant = original.viewState.variant;
          text = yaml.dump(legacyView(original, importedDefs, selectedVariant));
        } else {
          importedView = { signals: Object.fromEntries(Object.entries(original).filter(([k]) => !["groups", "inactive", "viewState", "layoutMode"].includes(k))), groups: original.groups ?? {} };
          // v1 has no variants; it becomes a common definition.
          legacyDefinitions = true;
          importedDefs = emptyDefs();
          importedDefs.order = Object.keys(importedView.signals);
        }
      } catch (e) {
        toast.error(`YAML を読めません: ${e instanceof Error ? e.message : String(e)}`);
        return;
      }
    }
    // Parse only this file; existing effective values may contain variant overrides.
    let modeMapping: AppState["modeMapping"] = {};
    let lsbMapping: AppState["lsbMapping"] = {};
    let offsetMapping: AppState["offsetMapping"] = {};
    let unitMapping: AppState["unitMapping"] = {};
    let undefinedMapping: AppState["undefinedMapping"] = {};
    let inactive = keepExisting ? get().inactive : { show: true, rules: [] };
    let signalGroups = keepExisting ? [...get().signalGroups] : [];
    let signalSettings = { ...get().signalSettings };
    let viewStateConfig: any = null;

    let tempGroupMap: Record<string, string[]> = {};
    let groupConfigs: Record<string, Record<string, unknown>> = {};

    if (format === "yaml") {
      try {
        const doc = yaml.load(text) as Record<string, Record<string, unknown>>;
        if (!doc) return;

        if (!options?.reapply && doc.viewState && typeof doc.viewState === "object") {
          viewStateConfig = doc.viewState;
        }

        if (doc.groups && typeof doc.groups === "object") {
          for (let gName in doc.groups) {
            groupConfigs[gName] = doc.groups[gName] as Record<string, unknown>;
          }
        }

        if (doc.inactive !== undefined) {
          inactive = parseInactive(doc.inactive);
        }

        for (let sig in doc) {
          // viewState は表示の状態で、信号ではない（信号の設定を作ると、左の一覧や名前の重なりに混ざる）
          if (sig === "groups" || sig === "inactive" || sig === "viewState") continue;
          if (sig === "layoutMode") {
            const val = String(doc[sig]);
            if (["uniform", "optimized", "manual"].includes(val)) {
              set({ layoutMode: val as "uniform" | "optimized" | "manual" });
            }
            continue;
          }
          if (!modeMapping[sig]) modeMapping[sig] = {};
          if (!signalSettings[sig]) {
            signalSettings[sig] = {
              id: "sig_" + Object.keys(signalSettings).length,
              color: colorPalette[Object.keys(signalSettings).length % colorPalette.length],
              weight: 1,
              manualWeight: 1,
              expanded: false,
              yAuto: true,
              yMin: null,
              yMax: null,
              checked: true,
            };
          }
          const sigDoc = doc[sig];

          for (let key in sigDoc) {
            let lowerKey = String(key).toLowerCase();
            if (lowerKey === "lsb") {
              lsbMapping[sig] = String(sigDoc[key]);
            } else if (lowerKey === "offset") {
              offsetMapping[sig] = String(sigDoc[key]);
            } else if (lowerKey === "unit") {
              unitMapping[sig] = String(sigDoc[key]);
            } else if (lowerKey === "group") {
              const gName = String(sigDoc[key]);
              if (!tempGroupMap[gName]) tempGroupMap[gName] = [];
              tempGroupMap[gName].push(sig);
            } else if (lowerKey === "undefined") {
              const uval = sigDoc[key];
              undefinedMapping[sig] = Array.isArray(uval)
                ? uval.map(Number)
                : [Number(uval)];
            } else if (lowerKey === "color") {
              if (signalSettings[sig])
                signalSettings[sig].color = String(sigDoc[key]);
            } else if (lowerKey === "linewidth" || lowerKey === "width") {
              if (signalSettings[sig])
                signalSettings[sig].lineWidth = Number(sigDoc[key]);
            } else if (lowerKey === "linetype" || lowerKey === "type") {
              if (signalSettings[sig]) {
                const type = String(sigDoc[key]).toLowerCase();
                if (["solid", "dashed", "dotted"].includes(type)) {
                  signalSettings[sig].lineType = type as "solid" | "dashed" | "dotted";
                }
              }
            } else if (lowerKey === "weight") {
              if (signalSettings[sig]) {
                signalSettings[sig].weight = Number(sigDoc[key]);
                signalSettings[sig].manualWeight = Number(sigDoc[key]);
              }
            } else if (lowerKey === "alias") {
              if (signalSettings[sig]) {
                signalSettings[sig].alias = String(sigDoc[key]);
              }
            } else if (lowerKey === "ymin") {
              if (signalSettings[sig]) {
                signalSettings[sig].yMin = Number(sigDoc[key]);
                signalSettings[sig].yAuto = false;
              }
            } else if (lowerKey === "ymax") {
              if (signalSettings[sig]) {
                signalSettings[sig].yMax = Number(sigDoc[key]);
                signalSettings[sig].yAuto = false;
              }
            } else if (lowerKey === "yauto") {
              if (signalSettings[sig]) {
                signalSettings[sig].yAuto =
                  Boolean(sigDoc[key]) && sigDoc[key] !== "false";
              }
            } else if (lowerKey === "checked") {
              if (signalSettings[sig]) {
                signalSettings[sig].checked =
                  Boolean(sigDoc[key]) && sigDoc[key] !== "false";
              }
            } else {
              let numKey = parseFloat(key);
              if (!isNaN(numKey)) {
                modeMapping[sig][numKey] = String(sigDoc[key]);
              }
            }
          }
        }
      } catch (e) {
        console.error("YAML Parse Error", e);
      }
    } else {
      const parsed = Papa.parse<string[]>(text, { skipEmptyLines: true });
      parsed.data.forEach((row) => {
        if (row.length >= 3) {
          const sig = row[0].trim();
          const key = row[1].trim();
          const val = row[2].trim();
          let lowerKey = key.toLowerCase();

          if (lowerKey === "lsb") {
            lsbMapping[sig] = val;
          } else if (lowerKey === "offset") {
            offsetMapping[sig] = val;
          } else if (lowerKey === "unit") {
            unitMapping[sig] = val;
          } else if (lowerKey === "undefined") {
            undefinedMapping[sig] = val.split(",").map((v) => Number(v.trim()));
          } else {
            if (!modeMapping[sig]) modeMapping[sig] = {};
            let numKey = parseFloat(key);
            if (!isNaN(numKey)) {
              modeMapping[sig][numKey] = val;
            }
          }
        }
      });
    }

    // ライブ追加時は停止済み記録にまだない信号も残す。CSV 切替時は新しいデータだけを対象にする。
    const state = get();
    const dataSignals = state.chartData ? Object.keys(state.chartData.seriesMap) : null;
    let unassignedSignals = dataSignals
      ? options?.keepListed ? [...new Set([...dataSignals, ...listedSignals(state)])] : dataSignals
      : listedSignals(state);

    // Preserve yaml order for unassigned signals
    let yamlSignalOrder: string[] = [];
    if (format === "yaml") {
      try {
        const doc = yaml.load(text) as Record<string, unknown>;
        if (doc) {
          for (let sig in doc) {
            if (sig !== "groups" && sig !== "layoutMode" && sig !== "inactive" && sig !== "viewState") {
              yamlSignalOrder.push(sig);
            }
          }
        }
      } catch {}
    }
    if (yamlSignalOrder.length > 0) {
      const explicitSignals = yamlSignalOrder.filter((s) =>
        unassignedSignals.includes(s),
      );
      const remainingSignals = unassignedSignals.filter(
        (s) => !yamlSignalOrder.includes(s),
      );
      unassignedSignals = [...explicitSignals, ...remainingSignals];
    }

    if (!keepExisting) {
      let groupCounter = 1;
      const allGroupNames = Array.from(
        new Set([...Object.keys(groupConfigs), ...Object.keys(tempGroupMap)]),
      );

      for (let gName of allGroupNames) {
        const gId = "group_" + groupCounter++;
        const config = groupConfigs[gName] || {};
        const group: SignalGroup = {
          id: gId,
          name: gName,
          expanded: config.expanded !== undefined ? Boolean(config.expanded) : true,
          visible: config.visible !== undefined ? Boolean(config.visible) : true,
          layout:
            config.layout === "overlap" || config.layout === "lane"
              ? config.layout
              : "lane",
          syncRange: !!config.syncRange,
          signals: [],
        };

        if (tempGroupMap[gName]) {
          // Push signals according to yaml flow within that group
          tempGroupMap[gName].forEach((sig) => {
            if (unassignedSignals.includes(sig)) {
              group.signals.push(sig);
              unassignedSignals = unassignedSignals.filter((s) => s !== sig);
            }
          });
        }

        if (groupConfigs[gName] || group.signals.length > 0) {
          signalGroups.push(group);
        }
      }
    }

    const nextState: Partial<WaveformStore> = {
      ...(!keepExisting && format === "yaml" ? { layoutMode: (yaml.load(text) as Record<string, any>).layoutMode ?? "uniform" } : {}),
      modeMapping,
      lsbMapping,
      offsetMapping,
      unitMapping,
      undefinedMapping,
      inactive,
      signalSettings,
      ...(!options?.reapply ? { showMode: viewStateConfig?.showMode ?? "full" } : {}),
    };

    if (viewStateConfig) {
      // Restore the view together with compareEnabled so it uses the saved axis.
      const { zoom, markers } = viewStateConfig;
      if (zoom) nextState.zoomRange = { start: zoom.start, end: zoom.end, startValue: zoom.startValue, endValue: zoom.endValue };
      if (Array.isArray(markers)) nextState.cursors = markers;
      if (viewStateConfig.showSignalNames !== undefined) nextState.showSignalNames = viewStateConfig.showSignalNames;
      if (viewStateConfig.showAlias !== undefined) nextState.showAlias = viewStateConfig.showAlias;
      if (viewStateConfig.showMarkers !== undefined) nextState.showMarkers = viewStateConfig.showMarkers;
      if (viewStateConfig.showCursorLabels !== undefined) nextState.showCursorLabels = viewStateConfig.showCursorLabels;
      if (viewStateConfig.isSidebarOpen !== undefined) nextState.isSidebarOpen = viewStateConfig.isSidebarOpen;
      if (viewStateConfig.activeSidebarTab !== undefined) nextState.activeSidebarTab = viewStateConfig.activeSidebarTab;
      if (viewStateConfig.showTable !== undefined) nextState.showTable = viewStateConfig.showTable;
      if (viewStateConfig.tablePosition !== undefined) nextState.tablePosition = viewStateConfig.tablePosition;
      if (viewStateConfig.isTableTransposed !== undefined) nextState.isTableTransposed = viewStateConfig.isTableTransposed;
      if (viewStateConfig.theme !== undefined) nextState.theme = viewStateConfig.theme;
      // A standalone YAML cannot supply a baseline; restore only for the loaded A.
      if (get().compareChartData && viewStateConfig.compareFileName === get().compareFileName) {
        if (typeof viewStateConfig.compareEnabled === "boolean") nextState.compareEnabled = viewStateConfig.compareEnabled;
        if (viewStateConfig.diffFilter === "all" || viewStateConfig.diffFilter === "value") nextState.diffFilter = viewStateConfig.diffFilter;
        if (typeof viewStateConfig.showOnlyDiffSignals === "boolean") nextState.showOnlyDiffSignals = viewStateConfig.showOnlyDiffSignals;
        nextState.focusedDiffIndex = -1;
      }
    }

    if (legacyDefinitions) {
      if (!importedDefs) {
        importedDefs = emptyDefs();
        importedDefs.order = unique([...Object.keys(modeMapping), ...Object.keys(lsbMapping), ...Object.keys(offsetMapping), ...Object.keys(unitMapping), ...Object.keys(undefinedMapping)]);
        importedView = get().signalView;
      }
      for (const sig of importedDefs.order) importedDefs.base[sig] = defFromMappings(nextState as AppState, sig);
    }
    if (importedDefs) {
      if (keepExisting) {
        const previous = get().signalDefs;
        const variants = { ...previous.variants };
        for (const [name, overrides] of Object.entries(importedDefs.variants)) variants[name] = mergeDefinitions(variants[name] ?? {}, overrides);
        importedDefs = { base: mergeDefinitions(previous.base, importedDefs.base), variants,
          variantNames: unique([...previous.variantNames, ...importedDefs.variantNames]), order: unique([...previous.order, ...importedDefs.order]) };
        importedView = { signals: mergeViewEntries(get().signalView.signals, importedView!.signals), groups: mergeViewEntries(get().signalView.groups, importedView!.groups) };
      }
      nextState.signalDefs = importedDefs;
      nextState.signalView = importedView!;
      nextState.selectedVariant = selectedVariant;
      Object.assign(nextState, effectiveMappings(importedDefs, selectedVariant));
    }
    set(nextState);
    if (useLiveStore.getState().mode === "offline" && typeof viewStateConfig?.variant === "string") {
      try { localStorage.setItem("sim-gui:signals-variant:v1", selectedVariant); } catch { /* unavailable */ }
    }

    // Apply theme to DOM
    const currentTheme = get().theme;
    if (typeof document !== "undefined" && currentTheme === "dark") {
      document.documentElement.classList.add("dark");
    } else if (typeof document !== "undefined") {
      document.documentElement.classList.remove("dark");
    }

    if (!keepExisting) {
      set({ signalGroups, unassignedSignals });
    }
  },

  configHandle: null,
  setConfigHandle: (handle) => set({ configHandle: handle }),

  getConfigYAML: (includeViewState = false) => {
    const state = get();
    const doc: Record<string, any> = {};
    const groupsOutput: Record<string, unknown> = {};

    // Save View State
    if (includeViewState) {
      const viewState: Record<string, unknown> = {};
      if (state.zoomRange) {
        viewState.zoom = { ...state.zoomRange };
      }
      if (state.cursors && state.cursors.length > 0) {
        viewState.markers = state.cursors.map(c => ({ time: c.time, index: c.index }));
      }
      viewState.variant = state.selectedVariant;
      viewState.showMode = state.showMode;
      viewState.showSignalNames = state.showSignalNames;
      viewState.showAlias = state.showAlias;
      viewState.showMarkers = state.showMarkers;
      viewState.showCursorLabels = state.showCursorLabels;
      viewState.isSidebarOpen = state.isSidebarOpen;
      viewState.activeSidebarTab = state.activeSidebarTab;
      viewState.showTable = state.showTable;
      viewState.tablePosition = state.tablePosition;
      viewState.isTableTransposed = state.isTableTransposed;
      viewState.theme = state.theme;
      if (state.compareChartData && state.compareRawCSVText !== null) {
        viewState.compareCSVPath = getCompareCSVPath(state.fileName);
        viewState.compareFileName = state.compareFileName;
        viewState.compareEnabled = state.compareEnabled;
        viewState.diffFilter = state.diffFilter;
        viewState.showOnlyDiffSignals = state.showOnlyDiffSignals;
      }

      if (Object.keys(viewState).length > 0) {
        doc.viewState = viewState;
      }
    }

    state.signalGroups.forEach((g) => {
      groupsOutput[g.name] = {
        expanded: g.expanded,
        visible: g.visible,
        layout: g.layout,
        syncRange: g.syncRange,
      };
    });
    for (const [name, config] of Object.entries(state.signalView.groups)) {
      if (!Object.prototype.hasOwnProperty.call(groupsOutput, name)) groupsOutput[name] = config;
    }
    if (Object.keys(groupsOutput).length > 0) {
      doc.groups = groupsOutput;
    }

    const inactiveRules = state.inactive.rules
      .filter((rule) => rule.signal !== "")
      .map((rule) => ({
        ...rule,
        when: rule.when.filter(
          (condition) =>
            typeof condition === "number" ||
            condition.min !== undefined ||
            condition.max !== undefined,
        ),
      }));
    if (inactiveRules.length > 0) {
      doc.inactive = state.inactive.show
        ? inactiveRules
        : { show: false, rules: inactiveRules };
    }

    if (state.layoutMode !== "uniform") {
      doc.layoutMode = state.layoutMode;
    }

    const orderedSignals = [
      ...state.signalGroups.flatMap((g) => g.signals),
      ...state.unassignedSignals,
    ];

    orderedSignals.forEach((sig) => {
      if (!state.signalSettings[sig]) return;
      const g = state.signalGroups.find((g) => g.signals.includes(sig));
      const gName = g ? g.name : undefined;
      const sigData: Record<string, unknown> = {};

      if (gName) sigData.group = gName;
      if (state.lsbMapping[sig] !== undefined)
        sigData.lsb = serializeConversionExpression(state.lsbMapping[sig]);
      if (state.offsetMapping[sig]?.trim() && parseOffset(state.offsetMapping[sig]) !== 0)
        sigData.offset = serializeConversionExpression(state.offsetMapping[sig]);
      if (state.unitMapping[sig] !== undefined)
        sigData.unit = state.unitMapping[sig];
      if (
        state.undefinedMapping[sig] &&
        state.undefinedMapping[sig].length > 0
      ) {
        sigData.undefined =
          state.undefinedMapping[sig].length === 1
            ? state.undefinedMapping[sig][0]
            : state.undefinedMapping[sig];
      }

      const setting = state.signalSettings[sig];
      if (setting) {
        if (setting.color) sigData.color = setting.color;
        if (setting.lineWidth !== undefined)
          sigData.lineWidth = setting.lineWidth;
        if (setting.lineType) sigData.lineType = setting.lineType;
        if (setting.manualWeight !== undefined && setting.manualWeight !== 1)
          sigData.weight = setting.manualWeight;
        if (setting.alias) sigData.alias = setting.alias;

        // Settings related to Y-axis
        if (!setting.yAuto) {
          if (setting.yMin !== null) sigData.yMin = setting.yMin;
          if (setting.yMax !== null) sigData.yMax = setting.yMax;
        }

        // Save visibility (checked)
        if (setting.checked !== undefined) {
          sigData.checked = setting.checked;
        }
      }

      const modeMap = state.modeMapping[sig];
      if (modeMap) {
        for (const [k, v] of Object.entries(modeMap)) {
          sigData[k] = v;
        }
      }

      if (Object.keys(sigData).length > 0) {
        doc[sig] = sigData;
      }
    });

    const current = new Set(orderedSignals);
    const viewSignals: Record<string, unknown> = {};
    const meanings: Record<string, unknown> = {};
    const defs = state.signalDefs;
    const order = unique([...orderedSignals, ...Object.keys(state.signalView.signals), ...defs.order, ...Object.keys(defs.base)]);
    for (const sig of order) {
      const flat = current.has(sig) ? doc[sig] ?? {} : state.signalView.signals[sig];
      if (flat) viewSignals[sig] = Object.fromEntries(Object.entries(flat).filter(([key]) => !["lsb", "offset", "unit", "undefined"].includes(key) && !Number.isFinite(Number(key))));
      const variants = Object.fromEntries(Object.entries(defs.variants).filter(([, signals]) => signals[sig] !== undefined).map(([name, signals]) => [name, signals[sig]]));
      const def = { ...(defs.base[sig] ?? (Object.keys(variants).length ? {} : defFromMappings(state, sig))) };
      if (Object.keys(def).length || Object.keys(variants).length) {
        meanings[sig] = { ...def, ...(Object.keys(variants).length ? { variants } : {}) };
      }
    }
    const variantNames = unique([...defs.variantNames, ...Object.keys(defs.variants), ...(useLiveStore.getState().mode !== "offline" ? useVariantsStore.getState().doc.variants.map(v => v.name) : [])]);
    return dumpSignals({ version: 2, variants: variantNames, signals: meanings,
      ...(doc.inactive ? { inactive: doc.inactive } : {}),
      view: { ...(doc.layoutMode ? { layoutMode: doc.layoutMode } : {}), groups: groupsOutput, signals: viewSignals },
      ...(doc.viewState ? { viewState: doc.viewState } : {}),
    });
  },

  toggleSignalSettingsSection: (key) =>
    set((state) => ({
      signalSettingsSections: {
        ...state.signalSettingsSections,
        [key]: !state.signalSettingsSections[key],
      },
    })),

  setSignalConversion: (sig, patch) => get().setSignalConversions({ [sig]: patch }),

  setSignalConversions: (patches) =>
    set((state) => {
      const lsbMapping = { ...state.lsbMapping };
      const offsetMapping = { ...state.offsetMapping };
      const unitMapping = { ...state.unitMapping };
      const undefinedMapping = { ...state.undefinedMapping };
      const modeMapping = { ...state.modeMapping };
      const signalSettings = { ...state.signalSettings };
      const conversionChanged = new Set<string>();
      const rangeChanged = new Set<string>();

      for (const [sig, patch] of Object.entries(patches)) {
        const before = getConversion(sig, state.lsbMapping, state.offsetMapping);
        for (const [key, mapping] of [
          ["lsb", lsbMapping],
          ["offset", offsetMapping],
          ["unit", unitMapping],
        ] as const) {
          const value = patch[key];
          if (value === undefined) continue;
          if (value === "") delete mapping[sig];
          else mapping[sig] = value;
        }
        if (patch.undefinedValues !== undefined) {
          if (patch.undefinedValues.length === 0) delete undefinedMapping[sig];
          else undefinedMapping[sig] = [...patch.undefinedValues];
          rangeChanged.add(sig);
        }
        if (patch.states !== undefined) {
          if (Object.keys(patch.states).length === 0) delete modeMapping[sig];
          else modeMapping[sig] = { ...patch.states };
        }
        const after = getConversion(sig, lsbMapping, offsetMapping);
        if (before.lsb !== after.lsb || before.offset !== after.offset) {
          const group = state.signalGroups.find((g) => g.signals.includes(sig));
          const resetSignals = group?.syncRange
            ? group.signals
            : !group && state.unassignedGroupConfig.syncRange && state.unassignedSignals.includes(sig)
              ? state.unassignedSignals
              : [sig];
          for (const target of resetSignals) {
            conversionChanged.add(target);
            rangeChanged.add(target);
            if (signalSettings[target]) {
              signalSettings[target] = {
                ...signalSettings[target],
                yAuto: true,
                yMin: null,
                yMax: null,
                yBreakStart: null,
                yBreakEnd: null,
              };
            }
          }
        }
      }

      const nextState = { ...state, lsbMapping, offsetMapping, unitMapping, undefinedMapping, signalSettings };
      for (const group of state.signalGroups) {
        if (group.syncRange && group.signals.some((sig) => rangeChanged.has(sig))) {
          recalculateGroupSyncRange(nextState, group.signals, signalSettings);
        }
      }
      if (state.unassignedGroupConfig.syncRange && state.unassignedSignals.some((sig) => rangeChanged.has(sig))) {
        recalculateGroupSyncRange(nextState, state.unassignedSignals, signalSettings);
      }
      // Keep reset signals in Auto, including their unset manual bounds.
      for (const sig of conversionChanged) {
        if (signalSettings[sig]) {
          signalSettings[sig] = { ...signalSettings[sig], yMin: null, yMax: null };
        }
      }
      const effective = { ...state, lsbMapping, offsetMapping, unitMapping, undefinedMapping, modeMapping };
      const signalDefs = writeEffective(state.signalDefs, effective, state.selectedVariant, Object.keys(patches));
      return { lsbMapping, offsetMapping, unitMapping, undefinedMapping, modeMapping, signalSettings, signalDefs };
    }),

  updateSignalSetting: (sig, updates) =>
    set((state) => {
      const actualUpdates = { ...updates };
      if ("weight" in actualUpdates) {
        actualUpdates.manualWeight = actualUpdates.weight;
      }
      let newSettings = {
        ...state.signalSettings,
        [sig]: { ...state.signalSettings[sig], ...actualUpdates },
      };

      const hasSyncTargetUpdate =
        "yAuto" in updates ||
        "yMin" in updates ||
        "yMax" in updates ||
        "weight" in updates ||
        "yBreak" in updates ||
        "yBreakStart" in updates ||
        "yBreakEnd" in updates;

      if (hasSyncTargetUpdate) {
        let targetGroup: SignalGroup | Omit<SignalGroup, "signals"> | null =
          null;
        let groupSignals: string[] = [];

        const group = state.signalGroups.find((g) => g.signals.includes(sig));
        if (group) {
          targetGroup = group;
          groupSignals = group.signals;
        } else if (state.unassignedSignals.includes(sig)) {
          targetGroup = state.unassignedGroupConfig;
          groupSignals = state.unassignedSignals;
        }

        const isSyncRange = targetGroup && targetGroup.syncRange;
        const isOverlap = targetGroup && targetGroup.layout === "overlap";

        if (isSyncRange || isOverlap) {
          groupSignals.forEach((s) => {
            if (s !== sig && newSettings[s]) {
              newSettings[s] = { ...newSettings[s] };
              if (isSyncRange) {
                if ("yAuto" in updates) newSettings[s].yAuto = updates.yAuto;
                if ("yMin" in updates) newSettings[s].yMin = updates.yMin;
                if ("yMax" in updates) newSettings[s].yMax = updates.yMax;
                if ("yBreak" in updates) newSettings[s].yBreak = updates.yBreak;
                if ("yBreakStart" in updates) newSettings[s].yBreakStart = updates.yBreakStart;
                if ("yBreakEnd" in updates) newSettings[s].yBreakEnd = updates.yBreakEnd;
              }
              if (isOverlap) {
                if ("weight" in actualUpdates) {
                  newSettings[s].weight = actualUpdates.weight;
                  newSettings[s].manualWeight = actualUpdates.weight;
                }
              }
            }
          });
        }
      }

        // If user manually changes weight, switch out of auto layout modes
        if ("weight" in actualUpdates && get().layoutMode !== "manual") {
          return { signalSettings: newSettings, layoutMode: "manual" };
        }

      return { signalSettings: newSettings };
    }),

  toggleSignalCheck: (sig) =>
    set((state) => {
      const current = state.signalSettings[sig]?.checked;
      const newSettings = {
        ...state.signalSettings,
        [sig]: { ...state.signalSettings[sig], checked: !current },
      };

      const newGroups = state.signalGroups.map((g) => {
        if (g.signals.includes(sig)) {
          return {
            ...g,
            visible: g.signals.some((s) => newSettings[s]?.checked),
          };
        }
        return g;
      });

      let newUnassignedConfig = state.unassignedGroupConfig;
      if (state.unassignedSignals.includes(sig)) {
        newUnassignedConfig = {
          ...newUnassignedConfig,
          visible: state.unassignedSignals.some((s) => newSettings[s]?.checked),
        };
      }

      return {
        signalSettings: newSettings,
        signalGroups: newGroups,
        unassignedGroupConfig: newUnassignedConfig,
      };
    }),

  toggleAllSignals: (check) =>
    set((state) => {
      const newSettings = { ...state.signalSettings };
      for (let sig in newSettings) {
        newSettings[sig] = { ...newSettings[sig], checked: check };
      }
      const newGroups = state.signalGroups.map((g) => ({
        ...g,
        visible: check,
      }));
      const newUnassigned = { ...state.unassignedGroupConfig, visible: check };
      return {
        signalSettings: newSettings,
        signalGroups: newGroups,
        unassignedGroupConfig: newUnassigned,
      };
    }),

  cycleLayoutMode: () =>
    set((state) => {
      const modeSequence: ("uniform" | "optimized" | "manual")[] = [
        "uniform",
        "optimized",
        "manual",
      ];
      const nextIdx =
        (modeSequence.indexOf(state.layoutMode) + 1) % modeSequence.length;
      const nextMode = modeSequence[nextIdx];

      const newSettings = { ...state.signalSettings };

      const getOptimizedWeight = (sig: string) => {
        if (state.modeMapping[sig] && Object.keys(state.modeMapping[sig]).length > 0) return 1.0;
        const data = state.chartData?.seriesMap[sig];
        if (!data) return 0.5;

        let uniques = new Set();
        let nonNullCount = 0;
        const step = Math.max(1, Math.floor(data.length / 2000));
        for (let i = 0; i < data.length; i += step) {
          if (data[i] !== null && data[i] !== undefined) {
            uniques.add(data[i]);
            nonNullCount++;
          }
        }
        
        if (nonNullCount === 0) return 0.5;
        if (uniques.size === 1) return 0.8;

        const undefinedVals = detectUndefinedValue(
          data,
          state.undefinedMapping[sig],
        );

        const autoRange = calculateAutoYRange(
          sig,
          data,
          state.lsbMapping[sig],
          state.offsetMapping[sig],
          undefinedVals || [],
          newSettings[sig]?.yBreak !== false,
        );
        const diff = autoRange.max - autoRange.min;
        
        if (uniques.size <= 3) return 1.0;

        let weight = 3.0;

        if (uniques.size > 100) weight += 1.0;
        else if (uniques.size > 20) weight += 0.5;

        if (diff > 100) weight += 1.0;
        else if (diff > 10) weight += 0.5;

        return weight;
      };

      for (let sig in newSettings) {
        let weight = 1;
        if (nextMode === "optimized") {
           weight = getOptimizedWeight(sig);
        } else if (nextMode === "uniform") {
           weight = 1;
        } else {
           weight = newSettings[sig].manualWeight ?? 1;
        }

        newSettings[sig] = {
          ...newSettings[sig],
          weight,
        };
      }

      return {
        signalSettings: newSettings,
        layoutMode: nextMode,
      };
    }),

  addGroup: (name) =>
    set((state) => ({
      signalGroups: [
        ...state.signalGroups,
        {
          id: "g_" + Date.now(),
          name,
          expanded: true,
          visible: true,
          layout: "overlap",
          syncRange: true,
          signals: [],
        },
      ],
    })),

  renameGroup: (id, name) =>
    set((state) => {
      const group = state.signalGroups.find(g => g.id === id);
      if (!group || group.name === name) return state;
      const groups = { ...state.signalView.groups };
      delete groups[group.name];
      groups[name] = { expanded: group.expanded, visible: group.visible,
        layout: group.layout, syncRange: group.syncRange };
      const signals = Object.fromEntries(Object.entries(state.signalView.signals).map(([sig, config]) =>
        [sig, config.group === group.name ? { ...config, group: name } : config]));
      return {
        signalGroups: state.signalGroups.map(g => g.id === id ? { ...g, name } : g),
        signalView: { ...state.signalView, groups, signals },
      };
    }),

  toggleGroupExpand: (id) =>
    set((state) => {
      if (id === "unassigned") {
        return {
          unassignedGroupConfig: {
            ...state.unassignedGroupConfig,
            expanded: !state.unassignedGroupConfig.expanded,
          },
        };
      }
      return {
        signalGroups: state.signalGroups.map((g) =>
          g.id === id ? { ...g, expanded: !g.expanded } : g,
        ),
      };
    }),

  toggleGroupVisibility: (id, visible) =>
    set((state) => {
      if (id === "unassigned") {
        const newSettings = { ...state.signalSettings };
        state.unassignedSignals.forEach((sig) => {
          newSettings[sig] = { ...newSettings[sig], checked: visible };
        });
        return {
          unassignedGroupConfig: { ...state.unassignedGroupConfig, visible },
          signalSettings: newSettings,
        };
      }
      const newSettings = { ...state.signalSettings };
      const group = state.signalGroups.find((g) => g.id === id);
      if (group) {
        group.signals.forEach((sig) => {
          newSettings[sig] = { ...newSettings[sig], checked: visible };
        });
      }
      return {
        signalGroups: state.signalGroups.map((g) =>
          g.id === id ? { ...g, visible } : g,
        ),
        signalSettings: newSettings,
      };
    }),

  setGroupLayout: (id, layout) =>
    set((state) => {
      if (id === "unassigned") {
        return {
          unassignedGroupConfig: { ...state.unassignedGroupConfig, layout },
        };
      }
      return {
        signalGroups: state.signalGroups.map((g) =>
          g.id === id ? { ...g, layout } : g,
        ),
      };
    }),

  toggleGroupSync: (id) =>
    set((state) => {
      let isSyncNow = false;
      let targetSignals: string[] = [];
      let newUnassignedConfig = state.unassignedGroupConfig;
      let newGroups = state.signalGroups;

      if (id === "unassigned") {
        isSyncNow = !state.unassignedGroupConfig.syncRange;
        newUnassignedConfig = {
          ...state.unassignedGroupConfig,
          syncRange: isSyncNow,
        };
        targetSignals = state.unassignedSignals;
      } else {
        newGroups = state.signalGroups.map((g) => {
          if (g.id === id) {
            isSyncNow = !g.syncRange;
            targetSignals = g.signals;
            return { ...g, syncRange: isSyncNow };
          }
          return g;
        });
      }

      let newSettings = { ...state.signalSettings };

      if (isSyncNow) {
        recalculateGroupSyncRange(state, targetSignals, newSettings);
      } else {
        targetSignals.forEach((sig) => {
          const set = newSettings[sig];
          if (set && set.yAuto) {
            let r = { min: 0, max: 1 };
            if (state.chartData?.seriesMap[sig]) {
              const undefinedVals = detectUndefinedValue(state.chartData.seriesMap[sig], state.undefinedMapping[sig]);
              r = calculateAutoYRange(
                sig,
                state.chartData.seriesMap[sig],
                state.lsbMapping[sig],
                state.offsetMapping[sig],
                undefinedVals || [],
              );
            }
            newSettings[sig] = {
              ...set,
              yMin: Number(r.min.toPrecision(4)),
              yMax: Number(r.max.toPrecision(4)),
            };
          }
        });
      }

      return {
        unassignedGroupConfig: newUnassignedConfig,
        signalGroups: newGroups,
        signalSettings: newSettings,
      };
    }),

  setGroups: (groups, unassigned) =>
    set((state) => {
      let newSettings = { ...state.signalSettings };

      const applyLeaderSettings = (signals: string[]) => {
        if (signals.length > 0) {
          const leader = newSettings[signals[0]];
          if (leader) {
            signals.forEach((s) => {
              if (s !== signals[0] && newSettings[s]) {
                newSettings[s] = {
                  ...newSettings[s],
                  yAuto: leader.yAuto,
                  yMin: leader.yMin,
                  yMax: leader.yMax,
                  yBreak: leader.yBreak,
                  yBreakStart: leader.yBreakStart,
                  yBreakEnd: leader.yBreakEnd,
                };
              }
            });
          }
        }
      };

      groups.forEach((g) => {
        if (g.syncRange) applyLeaderSettings(g.signals);
      });
      if (state.unassignedGroupConfig.syncRange) {
        applyLeaderSettings(unassigned);
      }

      const removedNames = new Set(state.signalGroups.filter(g => !groups.some(next => next.name === g.name)).map(g => g.name));
      let signalView = state.signalView;
      if (removedNames.size) {
        const retainedGroups = { ...signalView.groups };
        for (const name of removedNames) delete retainedGroups[name];
        const signals = Object.fromEntries(Object.entries(signalView.signals).map(([sig, config]) => {
          if (typeof config.group !== "string" || !removedNames.has(config.group)) return [sig, config];
          const remaining = { ...config };
          delete remaining.group;
          return [sig, remaining];
        }));
        signalView = { ...signalView, groups: retainedGroups, signals };
      }
      return { signalGroups: groups, unassignedSignals: unassigned, signalSettings: newSettings, signalView };
    }),

  addCursor: (time, index) =>
    set((state) => {
      if (state.cursors.find((c) => c.time === time)) return state;
      const newCursors = [...state.cursors, { time, index }].sort(
        (a, b) => a.time - b.time,
      );
      return { cursors: newCursors };
    }),

  removeCursor: (index) =>
    set((state) => ({ cursors: state.cursors.filter((_, i) => i !== index) })),

  clearAllCursors: () => set({ cursors: [] }),

  setHoveredSignal: (sig) => set({ hoveredSignal: sig }),
  setHoverCursor: (cursor) => set({ hoverCursor: cursor }),
  toggleTheme: () =>
    set((state) => {
      const newTheme = state.theme === "light" ? "dark" : "light";
      if (newTheme === "dark") {
        document.documentElement.classList.add("dark");
      } else {
        document.documentElement.classList.remove("dark");
      }
      return { theme: newTheme };
    }),
}));

// Subscribe to source references so every update path (including mapping loads and
// direct setState) refreshes derived comparison data. B's source data stays intact.
useWaveformStore.subscribe((state, previous) => {
  if (state.compareChartData === previous.compareChartData &&
      state.chartData === previous.chartData && state.modeMapping === previous.modeMapping &&
      state.inactive === previous.inactive && state.taskPeriod === previous.taskPeriod &&
      state.compareEnabled === previous.compareEnabled) return;
  const steps = state.compareEnabled && state.chartData && state.compareChartData
    ? compareDisplaySteps(state.chartData, state.compareChartData) : state.chartData?.steps;
  const previousSteps = previous.compareEnabled && previous.chartData && previous.compareChartData
    ? compareDisplaySteps(previous.chartData, previous.compareChartData) : previous.chartData?.steps;
  const extended = steps && steps.length > (state.chartData?.steps.length ?? 0);
  const wasExtended = previousSteps && previousSteps.length > (previous.chartData?.steps.length ?? 0);
  const sameAxis = steps === previousSteps || (!!steps && !!previousSteps &&
    steps.length === previousSteps.length && steps.every((step, index) => step === previousSteps[index]));
  if (!swappingCompare && (extended || wasExtended) && !sameAxis) {
    // Explicit view updates (e.g. project restore) take precedence over resets.
    const cursorsChanged = state.cursors !== previous.cursors;
    const limit = !cursorsChanged && state.chartData !== previous.chartData && wasExtended
      ? Math.min(steps?.length ?? 0, previous.chartData!.steps.length) : steps?.length ?? 0;
    useWaveformStore.setState({
      cursors: state.cursors.filter(cursor => cursor.index >= 0 && cursor.index < limit),
      hoverCursor: null,
      ...(state.zoomRange === previous.zoomRange
        ? { zoomRange: { start: 0, end: 100, startValue: undefined, endValue: undefined } }
        : {}),
    });
  }
  const result = state.compareEnabled && state.compareChartData && state.chartData
    ? compareWaveforms(state.compareChartData, state.chartData, state.taskPeriod,
        state.modeMapping, computeInactive(state.compareChartData, state.inactive),
        computeInactive(state.chartData, state.inactive))
    : { diffs: [], summary: {}, totalValueDurationMs: 0 };
  useWaveformStore.setState({ diffs: result.diffs, diffSummary: result.summary,
    totalValueDurationMs: result.totalValueDurationMs, focusedDiffIndex: -1 });
});

// 左のチェック（個別・一括・グループ・yaml）を保存用の監視設定にも反映する。
useWaveformStore.subscribe((state, prev) => {
  if (state.signalSettings === prev.signalSettings) return;
  const live = useLiveStore.getState();
  const watch = live.watch.map(w => state.signalSettings[w.name] && w.plot !== state.signalSettings[w.name].checked
    ? { ...w, plot: state.signalSettings[w.name].checked } : w);
  if (!watch.some((w, i) => w !== live.watch[i])) return;
  useLiveStore.setState({ watch });
  persistLiveLayout({ inputs: live.inputs, watch, windowSize: live.windowSize, maxSamples: live.maxSamples });
});
