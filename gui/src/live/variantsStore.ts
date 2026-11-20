import { create } from "zustand";
import { ApiError, createApi, type SimApi } from "./api";
import { loadLayout, sanitizeInputs, sanitizeWatch, saveLayout, type LiveLayout } from "./persist";
import type { VariantsDoc } from "./variants";
export const useVariantsStore = create<{
  errors: Record<string, string>;
  doc: VariantsDoc; saved: string; path: string; error: string; loaded: boolean;
}>(() => ({ errors: {}, doc: { version: 1, selected: "", variants: [] }, saved: "", path: "", error: "", loaded: false }));

let saveTimer: ReturnType<typeof setTimeout> | undefined;
let saveTail = Promise.resolve();
const layoutSaveError = "入力パネルを仕向ファイルに保存できませんでした: ";

/** 手動保存・選択・自動保存を直列化し、送信直前の文書を保存する。 */
export function saveVariants(api: SimApi, kind: "manual" | "selection" | "layout") {
  clearTimeout(saveTimer);
  saveTimer = undefined;
  const request = saveTail.then(async () => {
    const { doc, saved, error } = useVariantsStore.getState();
    if (kind !== "manual" && (error || JSON.stringify(doc) === saved)) return;
    try {
      await api.saveVariants(doc);
      useVariantsStore.setState(state => {
        const errors = { ...state.errors };
        if (errors[""]?.startsWith(layoutSaveError)) delete errors[""];
        return { saved: JSON.stringify(doc), error: "", errors };
      });
    } catch (e) {
      const prefix = kind === "layout" ? layoutSaveError : kind === "selection" ? "選択をファイルに保存できませんでした: " : "";
      useVariantsStore.setState(state => ({ errors: { ...state.errors, "": prefix + (e instanceof Error ? e.message : String(e)) } }));
    }
  });
  saveTail = request;
  return request;
}

export function variantLayout(layout: Pick<LiveLayout, "inputs" | "watch">) {
  return { inputs: sanitizeInputs(layout.inputs), watch: sanitizeWatch(layout.watch) };
}

export function persistLiveLayout(layout: LiveLayout, api?: SimApi) {
  const { doc, error, loaded } = useVariantsStore.getState();
  const current = loaded && doc.variants.find(v => v.name === doc.selected);
  if (!current) {
    saveLayout(layout);
    return;
  }
  if (error) return;
  const next = variantLayout(layout);
  if (JSON.stringify(next.inputs) === JSON.stringify(current.inputs) &&
      JSON.stringify(next.watch) === JSON.stringify(current.watch)) return;
  useVariantsStore.setState({ doc: { ...doc, variants: doc.variants.map(v => v === current ? { ...v, ...next } : v) } });
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => { void saveVariants(api ?? createApi(""), "layout"); }, 500);
}

export function persistLayoutLimits(layout: LiveLayout) {
  const { doc } = useVariantsStore.getState();
  const fallback = doc.selected ? loadLayout() : layout;
  saveLayout({ ...fallback, windowSize: layout.windowSize, maxSamples: layout.maxSamples });
}

/** 反映の全体エラーと名前ごとのエラー。ファイル読み込みエラーとは分ける。 */
export function setVariantsApplyError(error: unknown) {
  useVariantsStore.setState({ errors: {
    "": error instanceof Error ? error.message : String(error),
    ...(error instanceof ApiError ? Object.fromEntries(error.errors.map(row => [row.name, row.message])) : {}),
  } });
}

/**
 * 仕向を選んでいないときの警告。選ばないと初期化の値（#InitWheneverCall など）を書かないので、
 * 対象プログラムの初期化の処理が動いて入力が効かないことがある。仕向ファイルを読めなかったときは、そちらのエラーを出す。
 */
export function variantWarning(state: { doc: VariantsDoc; loaded: boolean; error: string }): string {
  if (!state.loaded || state.error || state.doc.selected) return "";
  return "仕向が選ばれていません（初期化の値を書きません）";
}
