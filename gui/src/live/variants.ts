import Papa from "papaparse";

export interface InitValue { name: string; value: string; mode: "once" | "every" }
export interface Variant { name: string; values: InitValue[] }
export interface VariantsDoc { version: 1; selected: string; variants: Variant[] }
export interface VariantsResult { path: string; exists: boolean; doc: unknown; error: string }
const object = (v: unknown): v is Record<string, unknown> => !!v && typeof v === "object" && !Array.isArray(v);
export function sanitizeVariants(raw: unknown): VariantsDoc {
  const doc: VariantsDoc = { version: 1, selected: "", variants: [] };
  if (!object(raw) || !Array.isArray(raw.variants)) return doc;
  const names = new Set<string>();
  for (const v of raw.variants) {
    if (!object(v) || typeof v.name !== "string" || !v.name.trim() || names.has(v.name) || !Array.isArray(v.values)) continue;
    names.add(v.name);
    const seen = new Set<string>();
    const values: InitValue[] = [];
    for (const row of v.values) {
      if (!object(row) || typeof row.name !== "string" || !row.name.trim() || seen.has(row.name) || typeof row.value !== "string" || (row.mode !== "once" && row.mode !== "every")) continue;
      seen.add(row.name);
      values.push({ name: row.name, value: row.value, mode: row.mode });
    }
    doc.variants.push({ name: v.name, values });
  }
  if (typeof raw.selected === "string" && names.has(raw.selected)) doc.selected = raw.selected;
  return doc;
}
export function decodeVariantsCsv(bytes: ArrayBuffer): string {
  try {
    return new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch {
    return new TextDecoder("shift_jis").decode(bytes);
  }
}

export function parseInitFromCsv(text: string): InitValue[] {
  const rows = Papa.parse<string[]>(text.replace(/^\uFEFF/, "")).data;
  const values = new Map<string, InitValue>();
  for (let i = 0; i + 1 < rows.length; i++) {
    if (!/^#init/i.test(rows[i][0]?.trim() ?? "")) continue;
    rows[i].slice(1).forEach((name, col) => {
      const value = rows[i + 1][col + 1];
      if (name.trim() && value !== undefined && value.trim() && !values.has(name.trim())) values.set(name.trim(), { name: name.trim(), value, mode: "once" });
    });
    i++;
  }
  return [...values.values()];
}
