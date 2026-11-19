import { create } from "zustand";
import type { SimApi } from "../live/api";
import { useVariantsStore } from "../live/variantsStore";
import { useWaveformStore } from "./useWaveformStore";
import { readSignalDocument } from "./signalDefs";

const KEY = "sim-gui:signals:v1";
const VARIANT_KEY = "sim-gui:signals-variant:v1";
export const useSignalsPersistStore = create<{ error: string }>(() => ({ error: "" }));
function storage(): Storage | null {
  try { return typeof localStorage === "undefined" ? null : localStorage; } catch { return null; }
}
const watched = (s: ReturnType<typeof useWaveformStore.getState>) => [s.signalDefs, s.signalView,
  s.modeMapping, s.lsbMapping, s.offsetMapping, s.unitMapping, s.undefinedMapping, s.inactive,
  s.signalGroups, s.unassignedSignals, s.signalSettings, s.layoutMode];

/** One lifecycle per controller; no saves before initialization completes. */
export function createSignalsPersistence(api: Pick<SimApi, "getSignals" | "saveSignals">) {
  let ready = false, stopped = false, live = false, path = "", lastText: string | null = null;
  let timer: ReturnType<typeof setTimeout> | undefined;
  let unsubscribe: (() => void) | undefined, unsubscribeVariants: (() => void) | undefined;
  let saving = false, pending = false;
  const error = (message: string) => useSignalsPersistStore.setState({ error: message });
  async function save() {
    if (!ready || stopped) return;
    if (saving) { pending = true; return; }
    const text = useWaveformStore.getState().getConfigYAML();
    if (text === lastText) return;
    saving = true;
    try {
      if (live) {
        const result = await api.saveSignals(text);
        if (!result.ok) throw new Error("保存できませんでした");
        path = result.path;
      } else {
        const target = storage();
        if (!target) return;
        target.setItem(KEY, text);
      }
      lastText = text;
      if (!stopped) error("");
    } catch (e) {
      if (live && !stopped) error(`信号情報ファイルを保存できません: ${String(e instanceof Error ? e.message : e)}（${path}）`);
    } finally {
      saving = false;
      if (pending && !stopped) { pending = false; schedule(); }
    }
  }
  function schedule() {
    if (!ready || stopped) return;
    clearTimeout(timer);
    timer = setTimeout(() => { void save(); }, 1000);
  }
  function followVariants() {
    useWaveformStore.getState().setSelectedVariant(useVariantsStore.getState().doc.selected);
  }
  async function start(mode: "live" | "offline") {
    ready = false;
    live = mode === "live";
    error("");
    let text: string | null = null;
    try {
      if (live) {
        followVariants();
        unsubscribeVariants = useVariantsStore.subscribe((next, prev) => {
          if (next.doc !== prev.doc) { followVariants(); schedule(); }
        });
        const result = await api.getSignals();
        if (stopped) return;
        path = result.path;
        if (result.error) throw new Error(result.error);
        if (result.exists) {
          text = result.text;
        }
      } else {
        try { text = storage()?.getItem(KEY) ?? null; } catch { /* unavailable */ }
        try { useWaveformStore.getState().setSelectedVariant(storage()?.getItem(VARIANT_KEY) ?? ""); } catch { /* unavailable */ }
      }
      if (stopped) return;
      if (text !== null && !text.split(/\r?\n/).every(line => !line.trim() || line.trimStart().startsWith("#"))) {
        readSignalDocument(text);
        useWaveformStore.getState().parseMappingData(text, "yaml", false);
        lastText = text;
      }
      if (live) followVariants();
      ready = true;
    } catch (e) {
      if (live) error(`信号情報ファイルを読めません: ${String(e instanceof Error ? e.message : e)}（${path}）`);
      return;
    }
    unsubscribe = useWaveformStore.subscribe((next, prev) => {
      const a = watched(next), b = watched(prev);
      if (a.some((value, i) => value !== b[i])) schedule();
    });
  }
  function stop() {
    stopped = true;
    ready = false;
    clearTimeout(timer);
    unsubscribe?.();
    unsubscribeVariants?.();
  }
  return { start, stop };
}
