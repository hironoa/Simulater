import { create } from "zustand";
import { ApiError } from "./api";
import type { VariantsDoc } from "./variants";
export const useVariantsStore = create<{
  errors: Record<string, string>;
  doc: VariantsDoc; saved: string; path: string; error: string; loaded: boolean;
}>(() => ({ errors: {}, doc: { version: 1, selected: "", variants: [] }, saved: "", path: "", error: "", loaded: false }));

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
