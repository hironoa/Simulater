import { create } from "zustand";
import type { VariantsDoc } from "./variants";
export const useVariantsStore = create<{
  doc: VariantsDoc; saved: string; path: string; error: string; loaded: boolean;
}>(() => ({ doc: { version: 1, selected: "", variants: [] }, saved: "", path: "", error: "", loaded: false }));
