import yaml from "js-yaml";
import type { AppState } from "../types";

export interface Def {
  lsb?: string | number;
  offset?: string | number;
  unit?: string;
  undefined?: number[];
  states?: Record<number, string>;
}
export interface DefOverride extends Omit<Def, "states"> { states?: Record<number, string | null> }
export interface SignalDefs {
  base: Record<string, Def>;
  variants: Record<string, Record<string, DefOverride>>;
  variantNames: string[];
  order: string[];
}
export interface SignalView {
  signals: Record<string, Record<string, unknown>>;
  groups: Record<string, Record<string, unknown>>;
}
export const emptyDefs = (): SignalDefs => ({ base: {}, variants: {}, variantNames: [], order: [] });
export const emptyView = (): SignalView => ({ signals: {}, groups: {} });
export const unique = (names: string[]) => [...new Set(names)];
const fields = ["lsb", "offset", "unit", "undefined", "states"] as const;
export function object(value: unknown): value is Record<string, any> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

/** Validate before touching the store, especially for automatically loaded files. */
export function readSignalDocument(text: string): Record<string, any> {
  const doc = yaml.load(text);
  if (!object(doc)) throw new Error("YAML の最上位は対応表にしてください");
  if (doc.version === 2) {
    if (!object(doc.signals)) throw new Error("signals は対応表にしてください");
    if (doc.variants !== undefined && (!Array.isArray(doc.variants) || !doc.variants.every((v: unknown) => typeof v === "string"))) throw new Error("variants は仕向名の配列にしてください");
    if (doc.view !== undefined && !object(doc.view)) throw new Error("view は対応表にしてください");
    for (const key of ["signals", "groups"]) if (doc.view?.[key] !== undefined && !object(doc.view[key])) throw new Error(`view.${key} は対応表にしてください`);
    for (const [sig, def] of Object.entries(doc.signals)) {
      if (!object(def)) throw new Error(`${sig} の定義は対応表にしてください`);
      validateDef(def, false);
      if (def.variants !== undefined) {
        if (!object(def.variants)) throw new Error(`${sig}.variants は対応表にしてください`);
        for (const override of Object.values(def.variants)) {
          if (!object(override)) throw new Error(`${sig} の上書きは対応表にしてください`);
          validateDef(override, true);
        }
      }
    }
    for (const value of Object.values(doc.view?.signals ?? {})) if (!object(value)) throw new Error("view.signals の設定は対応表にしてください");
  } else {
    for (const [name, value] of Object.entries(doc)) {
      if (!["layoutMode", "inactive", "viewState"].includes(name) && !object(value)) throw new Error(`${name} は対応表にしてください`);
    }
  }
  return normalizeSignalNames(doc);
}

function normalizeSignalNames(doc: Record<string, any>): Record<string, any> {
  const v2 = doc.version === 2;
  const metadata = new Set(["groups", "inactive", "viewState", "layoutMode", "version"]);
  const inactive = doc.inactive;
  const rules = Array.isArray(inactive) ? inactive
    : Array.isArray(inactive?.rules) ? inactive.rules : [inactive];
  const names = new Set(v2
    ? [...Object.keys(doc.signals), ...Object.keys(doc.view?.signals ?? {})]
    : Object.keys(doc).filter(name => !metadata.has(name)));
  for (const rule of rules) {
    if (object(rule) && typeof rule.signal === "string") names.add(rule.signal);
  }

  const shortName = (name: string) => name.slice(name.lastIndexOf("/") + 1);
  const qualifiedCounts = new Map<string, number>();
  for (const name of names) {
    if (!name.includes("/")) continue;
    const short = shortName(name);
    qualifiedCounts.set(short, (qualifiedCounts.get(short) ?? 0) + 1);
  }
  const canonical = (name: string) => {
    if (!name.includes("/")) return name;
    const short = shortName(name);
    if (short === "" || metadata.has(short)) return name;
    return qualifiedCounts.get(short) === 1 ? short : name;
  };

  const renameKeys = (table: Record<string, unknown>, reserved?: Set<string>) => {
    const entries = new Map<string, unknown>();
    for (const [name, value] of Object.entries(table)) {
      const key = reserved?.has(name) ? name : canonical(name);
      if (key !== name && (Object.prototype.hasOwnProperty.call(table, key) || entries.has(key))) continue;
      entries.set(key, value);
    }
    return Object.fromEntries(entries);
  };
  const renameRule = (rule: unknown) =>
    object(rule) && typeof rule.signal === "string" ? { ...rule, signal: canonical(rule.signal) } : rule;

  const normalized: Record<string, any> = v2
    ? { ...doc, signals: renameKeys(doc.signals) } : renameKeys(doc, metadata);
  if (v2 && doc.view?.signals !== undefined) {
    normalized.view = { ...doc.view, signals: renameKeys(doc.view.signals) };
  }
  if (Object.prototype.hasOwnProperty.call(doc, "inactive")) {
    normalized.inactive = Array.isArray(inactive) ? rules.map(renameRule)
      : Array.isArray(inactive?.rules) ? { ...inactive, rules: rules.map(renameRule) } : renameRule(inactive);
  }
  return normalized;
}

function validateDef(def: Record<string, any>, override: boolean) {
  for (const key of ["lsb", "offset", "unit"]) if (def[key] !== undefined && typeof def[key] !== "string" && typeof def[key] !== "number") throw new Error(`${key} の形式が不正です`);
  if (def.undefined !== undefined && !(typeof def.undefined === "number" || (Array.isArray(def.undefined) && def.undefined.every((n: unknown) => typeof n === "number" && Number.isFinite(n))))) throw new Error("undefined は数値の配列にしてください");
  if (def.states !== undefined && (!object(def.states) || !Object.entries(def.states).every(([k, v]) => k.trim() !== "" && Number.isFinite(Number(k)) && (typeof v === "string" || (override && v === null))))) throw new Error("states は数値キーと状態名の対応表にしてください");
}
function pickDef(value: Record<string, any>): DefOverride {
  const def: Record<string, unknown> = {};
  for (const field of fields) if (value[field] !== undefined) {
    def[field] = field === "undefined" && !Array.isArray(value[field]) ? [value[field]] : value[field];
  }
  return def;
}
export function readDefs(doc: Record<string, any>): SignalDefs {
  const defs = emptyDefs();
  defs.variantNames = [...(doc.variants ?? [])];
  defs.order = Object.keys(doc.signals);
  for (const [sig, value] of Object.entries(doc.signals) as [string, Record<string, any>][]) {
    defs.base[sig] = pickDef(value) as Def;
    for (const [variant, override] of Object.entries(value.variants ?? {})) {
      (defs.variants[variant] ??= {})[sig] = pickDef(override as Record<string, any>);
    }
  }
  defs.variantNames = unique([...defs.variantNames, ...Object.keys(defs.variants)]);
  return defs;
}
export function effectiveDef(defs: SignalDefs, sig: string, variant: string): Def {
  const base = defs.base[sig] ?? {};
  const override = defs.variants[variant]?.[sig];
  if (!override) return base;
  const states = { ...base.states };
  for (const [key, value] of Object.entries(override.states ?? {})) {
    if (value === null) delete states[Number(key)]; else states[Number(key)] = value;
  }
  return { ...base, ...override, states };
}
export function effectiveMappings(defs: SignalDefs, variant: string) {
  const mappings: Pick<AppState, "modeMapping" | "lsbMapping" | "offsetMapping" | "unitMapping" | "undefinedMapping"> = {
    modeMapping: {}, lsbMapping: {}, offsetMapping: {}, unitMapping: {}, undefinedMapping: {},
  };
  for (const sig of unique([...defs.order, ...Object.keys(defs.base), ...Object.keys(defs.variants[variant] ?? {})])) {
    const def = effectiveDef(defs, sig, variant);
    if (def.states && Object.keys(def.states).length) mappings.modeMapping[sig] = { ...def.states };
    if (def.lsb !== undefined && def.lsb !== "") mappings.lsbMapping[sig] = String(def.lsb);
    if (def.offset !== undefined && def.offset !== "") mappings.offsetMapping[sig] = String(def.offset);
    if (def.unit !== undefined && def.unit !== "") mappings.unitMapping[sig] = def.unit;
    if (def.undefined?.length) mappings.undefinedMapping[sig] = [...def.undefined];
  }
  return mappings;
}
export function defFromMappings(state: Pick<AppState, "modeMapping" | "lsbMapping" | "offsetMapping" | "unitMapping" | "undefinedMapping">, sig: string): Def {
  const def: Def = {};
  if (state.lsbMapping[sig] !== undefined) def.lsb = numeric(state.lsbMapping[sig]);
  if (state.offsetMapping[sig] !== undefined) def.offset = numeric(state.offsetMapping[sig]);
  if (state.unitMapping[sig] !== undefined) def.unit = state.unitMapping[sig];
  if (state.undefinedMapping[sig] !== undefined) def.undefined = state.undefinedMapping[sig];
  if (state.modeMapping[sig] && Object.keys(state.modeMapping[sig]).length) def.states = state.modeMapping[sig];
  return def;
}
function numeric(value: string | number): string | number {
  return typeof value === "string" && value.trim() !== "" && Number.isFinite(Number(value)) && String(Number(value)) === value.trim() ? Number(value) : value;
}
const equal = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
export function writeEffective(defs: SignalDefs, state: AppState, variant: string, names: string[]): SignalDefs {
  const base = { ...defs.base };
  const variants = { ...defs.variants };
  for (const sig of names) {
    const effective = defFromMappings(state, sig);
    if (variant && variants[variant]?.[sig] !== undefined) {
      const common = base[sig] ?? {};
      const override: DefOverride = {};
      for (const key of ["lsb", "offset", "unit", "undefined"] as const) {
        const value = effective[key] ?? (key === "undefined" ? [] : "");
        const baseValue = common[key] ?? (key === "undefined" ? [] : "");
        if (!equal(key === "lsb" || key === "offset" ? numeric(value as string | number) : value, key === "lsb" || key === "offset" ? numeric(baseValue as string | number) : baseValue)) Object.assign(override, { [key]: value });
      }
      const states: Record<number, string | null> = {};
      for (const k of unique([...Object.keys(common.states ?? {}), ...Object.keys(effective.states ?? {})])) {
        if (common.states?.[Number(k)] !== effective.states?.[Number(k)]) states[Number(k)] = effective.states?.[Number(k)] ?? null;
      }
      if (Object.keys(states).length) override.states = states;
      variants[variant] = { ...variants[variant], [sig]: override };
    } else base[sig] = effective;
  }
  return { ...defs, base, variants, order: unique([...defs.order, ...names]) };
}

/** Adapt v2 to the established v1 view parser; definitions remain separate. */
export function legacyView(doc: Record<string, any>, defs: SignalDefs, variant: string) {
  const flat: Record<string, unknown> = {};
  if (doc.viewState) flat.viewState = doc.viewState;
  if (doc.inactive) flat.inactive = doc.inactive;
  flat.layoutMode = doc.view?.layoutMode ?? "uniform";
  flat.groups = doc.view?.groups ?? {};
  for (const sig of unique([...Object.keys(doc.view?.signals ?? {}), ...defs.order])) {
    const { states, ...def } = effectiveDef(defs, sig, variant);
    flat[sig] = { ...doc.view?.signals?.[sig], ...def, ...states };
  }
  return flat;
}
export function dumpSignals(doc: unknown): string {
  // Only direct children of states use numeric keys; other names stay quoted.
  let statesIndent: number | undefined;
  return yaml.dump(doc, { noRefs: true }).split("\n").map(line => {
    const indent = line.match(/^ */)![0].length;
    if (line.trim() && statesIndent !== undefined && indent <= statesIndent) statesIndent = undefined;
    if (statesIndent !== undefined && indent === statesIndent + 2) {
      line = line.replace(/^( *)'(-?\d+(?:\.\d+)?(?:e[+-]?\d+)?)':/, "$1$2:");
    }
    if (/^ *states:$/.test(line)) statesIndent = indent;
    return line;
  }).join("\n");
}
