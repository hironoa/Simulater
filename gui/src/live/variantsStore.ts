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
