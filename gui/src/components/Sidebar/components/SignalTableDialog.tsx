import React, { useEffect, useMemo, useState } from "react";
import { createPortal } from "react-dom";
import { AlertCircle, ClipboardPaste, Copy, Pencil, Search, Table2, X } from "lucide-react";
import toast from "react-hot-toast";
import { useWaveformStore } from "../../../store/useWaveformStore";
import { SignalSetting } from "../../../types";
import { displaySignalName } from "../../../utils/displaySignalName";
import { cn, evaluateExpression, parseLSB, parseOffset } from "../../../utils/helpers";
import { StateRow, mappingToRows, parseStates, rowsToMapping, serializeStates, stateRowErrors } from "../../../utils/stateNames";
import { StateNameEditor, StateSummary } from "./StateNameEditor";

type Draft = {
  name: string;
  group: string;
  alias: string;
  color: string;
  lineWidth: string;
  lineType: string;
  weight: string;
  lsb: string;
  offset: string;
  unit: string;
  undefinedValues: string;
  states: string;
  yAuto: boolean;
  yMin: string;
  yMax: string;
  yBreak: boolean;
};
type Field = Exclude<keyof Draft, "name">;
type ColumnSet = "disp" | "conv" | "yaxis";
type Column = { key: Field; label: string; width?: string; mono?: boolean; right?: boolean };
const displayColumns: Column[] = [
  { key: "alias", label: "別名", width: "w-36" },
  { key: "color", label: "色", width: "w-8" },
  { key: "lineWidth", label: "太さ", width: "w-12", mono: true, right: true },
  { key: "lineType", label: "線種", width: "w-16" },
  { key: "weight", label: "高さ", width: "w-12", mono: true, right: true },
];
const conversionColumns: Column[] = [
  { key: "lsb", label: "LSB", width: "w-[88px]", mono: true },
  { key: "offset", label: "オフセット", width: "w-16", mono: true, right: true },
  { key: "unit", label: "単位", width: "w-[52px]" },
  { key: "undefinedValues", label: "除外値", width: "w-20", mono: true },
  { key: "states", label: "状態名", width: "w-[120px]" },
];
const axisColumns: Column[] = [
  { key: "yAuto", label: "Auto" },
  { key: "yMin", label: "Min", width: "w-16", mono: true, right: true },
  { key: "yMax", label: "Max", width: "w-16", mono: true, right: true },
  { key: "yBreak", label: "Break" },
];
const sections = [
  { key: "disp", label: "表示", columns: displayColumns },
  { key: "conv", label: "物理値変換", columns: conversionColumns },
  { key: "yaxis", label: "Y軸", columns: axisColumns },
] as const;
const allColumns: Column[] = [{ key: "group", label: "グループ", width: "w-28" }, ...displayColumns, ...conversionColumns, ...axisColumns];
const bulkColumns = [...conversionColumns.filter(column => column.key !== "states"), allColumns[0], displayColumns[2], displayColumns[3], displayColumns[4]];
const lineTypes = { solid: "実線", dashed: "破線", dotted: "点線" };
const inputClass = "px-1.5 py-0.5 text-[11px] border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800 rounded outline-none focus:border-blue-500";
const cellClass = "px-1.5 py-1 border-b border-gray-100 dark:border-slate-700";
const dividerClass = "border-l border-l-gray-300 dark:border-l-slate-600";
const headerClass = "px-2 py-1 border-b border-gray-300 dark:border-slate-600 font-bold";
const toolButtonClass = "px-2 py-1 text-[11px] border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-700 rounded hover:bg-gray-50 dark:hover:bg-slate-600 flex items-center gap-1";

function numeric(value: string) {
  return value.trim() !== "" && Number.isFinite(Number(value));
}
function errorsFor(row: Draft, groups: { id: string }[], original: Draft): Partial<Record<Field, string>> {
  const errors: Partial<Record<Field, string>> = {};
  const changed = (key: Field) => row[key] !== original[key];
  for (const key of ["lsb", "offset"] as const) {
    if (!changed(key)) continue;
    const value = row[key].trim();
    const evaluated = evaluateExpression(value);
    if (value && (evaluated === null || !Number.isFinite(evaluated) || (key === "lsb" && evaluated === 0))) {
      errors[key] = key === "lsb" ? "0 以外の有限値になる式を入力してください" : "有限値になる式を入力してください";
    }
  }
  if (changed("undefinedValues") && row.undefinedValues.trim() && !row.undefinedValues.split(",").every(numeric)) errors.undefinedValues = "数値をカンマ区切りで入力してください";
  if (changed("states") && parseStates(row.states) === null) errors.states = "「値=状態名」を 1 行に 1 つずつ、値が重複しないように入力してください";
  if (changed("lineWidth") && (!numeric(row.lineWidth) || Number(row.lineWidth) < 0.5 || Number(row.lineWidth) > 10)) errors.lineWidth = "太さは 0.5〜10 で入力してください";
  if (changed("weight") && (!numeric(row.weight) || Number(row.weight) < 0.5)) errors.weight = "高さは 0.5 以上で入力してください";
  if (!row.yAuto && (changed("yAuto") || changed("yMin") || changed("yMax"))) {
    if (!numeric(row.yMin)) errors.yMin = "Min に有限の数値を入力してください";
    if (!numeric(row.yMax)) errors.yMax = "Max に有限の数値を入力してください";
    if (numeric(row.yMin) && numeric(row.yMax) && Number(row.yMin) >= Number(row.yMax)) {
      errors.yMin = errors.yMax = "Min は Max より小さくしてください";
    }
  }
  if (changed("group") && row.group !== "unassigned" && !groups.some(g => g.id === row.group)) errors.group = "存在するグループを選択してください";
  if (changed("lineType") && !(row.lineType in lineTypes)) errors.lineType = "実線・破線・点線を選択してください";
  if (changed("color") && !/^#[\da-f]{6}$/i.test(row.color)) errors.color = "色は #RRGGBB 形式で入力してください";
  return errors;
}

// Excel の引用符付きセル（タブ・改行・二重引用符を含む）も往復できるようにする。
function parseTsv(text: string): string[][] {
  const rows: string[][] = [];
  let row: string[] = [], cell = "", quoted = false;
  text = text.replace(/^\uFEFF/, "");
  for (let i = 0; i < text.length; i++) {
    const char = text[i];
    if (char === '"' && (quoted || cell === "")) {
      if (quoted && text[i + 1] === '"') { cell += '"'; i++; }
      else quoted = !quoted;
    } else if (!quoted && (char === "\t" || char === "\r" || char === "\n")) {
      row.push(cell); cell = "";
      if (char !== "\t") {
        rows.push(row); row = [];
        if (char === "\r" && text[i + 1] === "\n") i++;
      }
    } else cell += char;
  }
  if (cell !== "" || row.length > 0) { row.push(cell); rows.push(row); }
  return rows;
}
const tsvCell = (value: string) => /[\t\r\n"]/.test(value) ? `"${value.replace(/"/g, '""')}"` : value;

export function SignalTableDialog({ onClose }: { onClose: () => void }) {
  const [snapshot] = useState(() => {
    const state = useWaveformStore.getState();
    const groups = state.signalGroups.map(group => ({ ...group, signals: [...group.signals] }));
    const order = [...groups.flatMap(group => group.signals), ...state.unassignedSignals];
    const groupBySignal = new Map(groups.flatMap(group => group.signals.map(name => [name, group.id] as const)));
    const rows: Draft[] = order.filter(name => state.signalSettings[name]).map(name => {
      const setting = state.signalSettings[name];
      return {
        name, group: groupBySignal.get(name) ?? "unassigned", alias: setting.alias ?? "", color: setting.color,
        lineWidth: String(setting.lineWidth ?? 1.5), lineType: setting.lineType ?? "solid", weight: String(setting.weight ?? 1),
        lsb: state.lsbMapping[name] ?? "", offset: state.offsetMapping[name] ?? "", unit: state.unitMapping[name] ?? "",
        undefinedValues: (state.undefinedMapping[name] ?? []).join(","), states: serializeStates(state.modeMapping[name]), yAuto: setting.yAuto,
        yMin: setting.yMin == null ? "" : String(setting.yMin), yMax: setting.yMax == null ? "" : String(setting.yMax), yBreak: setting.yBreak !== false,
      };
    });
    return { groups, rows, unassignedSyncRange: state.unassignedGroupConfig.syncRange, originals: new Map(rows.map(row => [row.name, row])) };
  });
  const [rows, setRows] = useState(snapshot.rows);
  const [columnSet, setColumnSet] = useState<ColumnSet | "all">("conv");
  const [filter, setFilter] = useState("");
  const [groupFilter, setGroupFilter] = useState("");
  const [selected, setSelected] = useState<Set<string>>(() => new Set());
  const [bulkField, setBulkField] = useState<Field>("lsb");
  const [bulkValue, setBulkValue] = useState("");
  const [statePopover, setStatePopover] = useState<{ name: string; rows: StateRow[]; top?: number; bottom?: number; left: number } | null>(null);
  useEffect(() => {
    const keyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !event.isComposing) {
        event.preventDefault();
        if (statePopover) setStatePopover(null);
        else onClose();
      }
    };
    document.addEventListener("keydown", keyDown);
    return () => document.removeEventListener("keydown", keyDown);
  }, [onClose, statePopover]);
  const visibleRows = useMemo(() => {
    const query = filter.toLocaleLowerCase();
    return rows.filter(row => (!groupFilter || row.group === groupFilter) &&
      (row.name.toLocaleLowerCase().includes(query) || row.alias.toLocaleLowerCase().includes(query)));
  }, [rows, filter, groupFilter]);
  const errors = useMemo(() => new Map(rows.map(row => [row.name, errorsFor(row, snapshot.groups, snapshot.originals.get(row.name)!)])), [rows, snapshot]);
  const changed = (row: Draft, field: Field) => row[field] !== snapshot.originals.get(row.name)![field];
  const changedCount = rows.reduce((sum, row) => sum + allColumns.filter(column => changed(row, column.key)).length, 0);
  const errorCount = [...errors.values()].reduce((sum, error) => sum + Object.keys(error).length, 0);
  const allVisibleSelected = visibleRows.length > 0 && visibleRows.every(row => selected.has(row.name));
  const isSyncGroup = (id: string) => id === "unassigned" ? snapshot.unassignedSyncRange === true : snapshot.groups.some(group => group.id === id && group.syncRange === true);
  const patchRows = (current: Draft[], name: string, patch: Partial<Draft>) => {
    const target = current.find(row => row.name === name);
    if (!target) return current;
    const group = patch.group ?? target.group;
    if (group !== target.group && isSyncGroup(group)) {
      const leader = current.find(row => row.name !== name && row.group === group);
      if (leader) return current.map(row => row.name === name ? { ...row, ...patch, yAuto: leader.yAuto, yMin: leader.yMin, yMax: leader.yMax, yBreak: leader.yBreak } : row);
    }
    const axisPatch: Partial<Draft> = {};
    if (isSyncGroup(group)) {
      for (const key of ["yAuto", "yMin", "yMax", "yBreak"] as const) {
        if (patch[key] !== undefined) Object.assign(axisPatch, { [key]: patch[key] });
      }
    }
    return current.map(row => row.name === name ? { ...row, ...patch } : row.group === group ? { ...row, ...axisPatch } : row);
  };
  const update = (name: string, field: Field, value: string | boolean) => {
    setRows(current => patchRows(current, name, {
      [field]: value, ...((field === "yMin" || field === "yMax") ? { yAuto: false } : {}),
    }));
  };
  const groupOptions = <>{snapshot.groups.map(group => <option key={group.id} value={group.id}>{group.name}</option>)}<option value="unassigned">未分類</option></>;
  const typeOptions = <>{Object.entries(lineTypes).map(([value, label]) => <option key={value} value={value}>{label}</option>)}</>;
  const groupName = (id: string) => snapshot.groups.find(group => group.id === id)?.name ?? "未分類";

  const copyTable = async () => {
    const data = [["信号名", ...allColumns.map(column => column.label)], ...visibleRows.map(row => [row.name, ...allColumns.map(({ key }) => {
      if (key === "group") return groupName(row.group);
      if (key === "lineType") return lineTypes[row.lineType] ?? row.lineType;
      const value = row[key];
      return typeof value === "boolean" ? (value ? "TRUE" : "FALSE") : value;
    })])];
    try {
      await navigator.clipboard.writeText(data.map(row => row.map(tsvCell).join("\t")).join("\r\n"));
      toast.success(`${visibleRows.length} 信号の表をコピーしました`);
    } catch { toast.error("表をコピーできませんでした"); }
  };
  const pasteTable = async () => {
    try {
      const [headers, ...data] = parseTsv(await navigator.clipboard.readText());
      const nameIndex = headers?.indexOf("信号名") ?? -1;
      if (nameIndex < 0) { toast.error("「信号名」列が見つかりません"); return; }
      const byName = new Map(rows.map(row => [row.name, row]));
      const patches: [string, Partial<Draft>][] = [];
      const missing = new Set<string>();
      for (const cells of data) {
        if (cells.every(cell => cell === "")) continue;
        const name = cells[nameIndex] ?? "";
        if (!byName.has(name)) { missing.add(name || "（空欄）"); continue; }
        const patch: Partial<Draft> = {};
        headers.forEach((header, index) => {
          const column = allColumns.find(column => column.label === header);
          if (!column || index >= cells.length) return;
          const value = cells[index];
          const key = column.key;
          if (key === "yAuto" || key === "yBreak") {
            if (!/^(true|false)$/i.test(value.trim())) throw new Error(`${header} は TRUE/FALSE で入力してください`);
            patch[key] = value.trim().toUpperCase() === "TRUE";
          } else if (key === "group") patch.group = value === "未分類" ? "unassigned" : snapshot.groups.find(group => group.name === value)?.id ?? value;
          else if (key === "lineType") patch.lineType = Object.entries(lineTypes).find(([, label]) => label === value)?.[0] ?? value;
          else patch[key] = value;
        });
        const original = byName.get(name)!;
        for (const key of Object.keys(patch) as Field[]) {
          if (patch[key] === original[key]) delete patch[key];
        }
        // Auto が変わらず範囲だけ変更された場合は、手動指定として扱う。
        if (patch.yAuto === undefined && (patch.yMin !== undefined || patch.yMax !== undefined)) patch.yAuto = false;
        patches.push([name, patch]);
      }
      setRows(current => patches.reduce((draft, [name, patch]) => patchRows(draft, name, patch), current));
      toast.success(`${new Set(patches.map(([name]) => name)).size} 信号の設定を下書きに貼り付けました`);
      if (missing.size) toast.error(`見つからない信号名: ${[...missing].join("、")}`);
    } catch (error) { toast.error(error instanceof Error && error.message.includes("TRUE/FALSE") ? error.message : "クリップボードを読み取れませんでした"); }
  };

  const apply = () => {
    if (errorCount) return;
    const state = useWaveformStore.getState();
    const changedRows = rows.filter(row => allColumns.some(column => changed(row, column.key)));
    const conversionChangedRows = rows.filter(row => {
      const original = snapshot.originals.get(row.name)!;
      return parseLSB(original.lsb) !== parseLSB(row.lsb) || parseOffset(original.offset) !== parseOffset(row.offset);
    });
    const conversionChangedNames = new Set(conversionChangedRows.map(row => row.name));
    const conversionChangedGroups = new Set(conversionChangedRows.filter(row => isSyncGroup(row.group)).map(row => row.group));
    const moved = rows.filter(row => changed(row, "group"));
    if (moved.length) {
      const movedNames = new Set(moved.map(row => row.name));
      const groups = state.signalGroups.map(group => ({ ...group, signals: group.signals.filter(name => !movedNames.has(name)) }));
      const unassigned = state.unassignedSignals.filter(name => !movedNames.has(name));
      for (const row of moved) {
        if (row.group === "unassigned") unassigned.push(row.name);
        else groups.find(group => group.id === row.group)!.signals.push(row.name);
      }
      state.setGroups(groups, unassigned);
    }
    const conversions: Parameters<typeof state.setSignalConversions>[0] = {};
    for (const row of changedRows) {
      const patch: (typeof conversions)[string] = {};
      for (const key of ["lsb", "offset", "unit"] as const) if (changed(row, key)) patch[key] = key === "unit" ? row[key] : row[key].trim();
      if (changed(row, "states")) patch.states = parseStates(row.states)!;
      if (changed(row, "undefinedValues")) patch.undefinedValues = row.undefinedValues.trim() ? row.undefinedValues.split(",").map(Number) : [];
      if (Object.keys(patch).length) conversions[row.name] = patch;
    }
    if (Object.keys(conversions).length) state.setSignalConversions(conversions);
    for (const row of changedRows) {
      const updates: Partial<SignalSetting> = {};
      for (const key of ["alias", "color"] as const) if (changed(row, key)) updates[key] = row[key];
      for (const key of ["lineWidth", "weight"] as const) if (changed(row, key)) updates[key] = Number(row[key]);
      if (changed(row, "lineType")) updates.lineType = row.lineType as SignalSetting["lineType"];
      for (const key of ["yAuto", "yBreak"] as const) if (changed(row, key)) updates[key] = row[key];
      if (!row.yAuto) {
        for (const key of ["yMin", "yMax"] as const) if (changed(row, key)) updates[key] = Number(row[key]);
        // 自身または同期グループ内の変換の評価値が変わり、Y軸も明示的に変更した行の手動範囲をまとめて復元する。
        if ((conversionChangedNames.has(row.name) || conversionChangedGroups.has(row.group)) &&
          (["yAuto", "yMin", "yMax"] as const).some(key => changed(row, key))) {
          updates.yAuto = false;
          updates.yMin = Number(row.yMin);
          updates.yMax = Number(row.yMax);
        }
      }
      if (changed(row, "yAuto") && row.yAuto) {
        updates.yMin = updates.yMax = updates.yBreakStart = updates.yBreakEnd = null;
      }
      if (Object.keys(updates).length) state.updateSignalSetting(row.name, updates);
    }
    onClose();
    toast.success(`${changedRows.length} 信号の設定を更新しました`);
  };

  const openStatePopover = (row: Draft, anchor: HTMLElement) => {
    const rect = anchor.getBoundingClientRect();
    const mapping = parseStates(row.states) ?? parseStates(snapshot.originals.get(row.name)!.states) ?? {};
    const below = window.innerHeight - rect.bottom > 360;
    setStatePopover({
      name: row.name, rows: mappingToRows(mapping), left: Math.max(8, Math.min(rect.left, window.innerWidth - 348)),
      ...(below ? { top: rect.bottom + 4 } : { bottom: window.innerHeight - rect.top + 4 }),
    });
  };
  const popoverErrors = statePopover ? stateRowErrors(statePopover.rows) : [];
  const copySources = statePopover ? rows.filter(row => row.name !== statePopover.name && Object.keys(parseStates(row.states) ?? {}).length > 0) : [];
  const visibleNames = visibleRows.map(row => row.name);
  const copySourceNames = copySources.map(row => row.name);

  const renderCell = (row: Draft, column: Column, first = false) => {
    const { key } = column;
    const error = errors.get(row.name)?.[key];
    const dirty = changed(row, key);
    const autoRange = (key === "yMin" || key === "yMax") && row.yAuto;
    const className = cn(inputClass, column.width, column.mono && "font-mono", column.right && "text-right",
      autoRange && "!bg-gray-100 dark:!bg-slate-900 text-gray-400", dirty && "!border-amber-400", error && "!border-red-500");
    const label = `${row.name} ${column.label}`;
    let control: React.ReactNode;
    if (key === "group" || key === "lineType") {
      control = <select aria-label={label} title={error} className={cn(className, "h-[22px]")} value={row[key]} onChange={event => update(row.name, key, event.target.value)}>{key === "group" ? groupOptions : typeOptions}</select>;
    } else if (key === "states") {
      const mapping = parseStates(row.states);
      control = <button type="button" aria-label={label} title={error} onClick={event => openStatePopover(row, event.currentTarget)}
        className={cn(className, "flex items-center gap-1 text-left text-[10px] text-gray-600 dark:text-gray-300 hover:border-blue-500")}>
        <span className="flex items-center min-w-0 truncate">{mapping ? <StateSummary mapping={mapping} max={6} /> : <span className="text-red-500">形式エラー</span>}</span>
        <Pencil size={10} className="ml-auto text-gray-400 shrink-0" />
      </button>;
    } else if (key === "yAuto" || key === "yBreak") {
      control = <input aria-label={label} type="checkbox" checked={row[key]} className={cn("accent-blue-500", dirty && "!border-amber-400")} onChange={event => update(row.name, key, event.target.checked)} />;
    } else {
      control = <input aria-label={label} title={error} aria-invalid={!!error} type={key === "color" ? "color" : "text"} value={autoRange ? "" : key === "color" && !/^#[\da-f]{6}$/i.test(row.color) ? "#000000" : row[key]}
        placeholder={autoRange ? "auto" : undefined} onChange={event => update(row.name, key, event.target.value)}
        className={cn(className, key === "color" && "h-[20px] p-0 cursor-pointer")} />;
    }
    return <td key={key} className={cn(cellClass, first && dividerClass, dirty && "bg-amber-50 dark:bg-amber-900/30", (key === "color" || key === "yAuto" || key === "yBreak") && "text-center")}>{control}</td>;
  };

  return createPortal(
    <div className="fixed inset-0 bg-black/50 z-[9999] flex justify-center items-center">
      <div role="dialog" aria-modal="true" aria-labelledby="signal-table-title" className="bg-white dark:bg-slate-800 dark:text-gray-200 rounded w-full max-w-[1080px] max-h-[90vh] shadow-lg flex flex-col">
        <div className="px-5 pt-4 pb-3 border-b border-gray-200 dark:border-slate-700 shrink-0">
          <div className="flex items-center">
            <h3 id="signal-table-title" className="m-0 text-sm font-bold flex items-center gap-1.5"><Table2 size={16} />信号設定の一覧編集</h3>
            <span className="ml-2 text-[11px] text-gray-400">{rows.length} 信号</span>
            <button aria-label="閉じる" onClick={onClose} className="ml-auto text-gray-400 hover:text-gray-600 dark:hover:text-gray-200"><X size={16} /></button>
          </div>
          <div className="mt-3 flex items-center gap-2 flex-wrap">
            <div className="flex shadow-sm rounded">
              {[...sections.map(({ key, label }) => ({ key, label })), { key: "all" as const, label: "すべて" }].map(({ key, label }, index) => (
                <button key={key} aria-pressed={columnSet === key} onClick={() => setColumnSet(key)} className={cn(
                  "px-2.5 py-1 text-[11px] border transition-colors", index === 0 ? "rounded-l" : "-ml-px", key === "all" && "rounded-r",
                  columnSet === key ? "bg-blue-500 text-white border-blue-500 z-10" : "bg-white dark:bg-slate-700 text-gray-600 dark:text-gray-300 border-gray-300 dark:border-slate-600 hover:bg-gray-50 dark:hover:bg-slate-600",
                )}>{label}</button>
              ))}
            </div>
            <div className="relative">
              <Search size={12} className="absolute left-1.5 top-1/2 -translate-y-1/2 text-gray-400" />
              <input aria-label="信号名・別名で絞り込み" placeholder="信号名・別名で絞り込み" value={filter} onChange={event => setFilter(event.target.value)} className="pl-6 pr-2 py-1 w-48 text-[11px] border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-700 rounded outline-none focus:border-blue-500" />
            </div>
            <select aria-label="グループで絞り込み" value={groupFilter} onChange={event => setGroupFilter(event.target.value)} className="px-1 h-[26px] text-[11px] border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-700 rounded outline-none"><option value="">全グループ</option>{groupOptions}</select>
            <div className="ml-auto flex items-center gap-1.5">
              <button onClick={pasteTable} className={toolButtonClass}><ClipboardPaste size={12} />Excelから貼り付け</button>
              <button onClick={copyTable} className={toolButtonClass}><Copy size={12} />表をコピー</button>
            </div>
          </div>
        </div>
        {selected.size > 0 && (
          <div className="px-5 py-1.5 bg-blue-50 dark:bg-slate-900 border-b border-blue-100 dark:border-slate-700 text-[11px] flex items-center gap-2 shrink-0 flex-wrap">
            <span className="font-bold text-blue-600 dark:text-blue-300">{selected.size} 行選択中</span>
            <span className="text-gray-500 dark:text-gray-400">まとめて設定:</span>
            <select aria-label="一括設定の項目" className={cn(inputClass, "h-[22px]")} value={bulkField} onChange={event => { const field = event.target.value as Field; setBulkField(field); setBulkValue(field === "group" ? "unassigned" : field === "lineType" ? "solid" : ""); }}>
              {bulkColumns.map(column => <option key={column.key} value={column.key}>{column.key === "lineWidth" ? "線の太さ" : column.label}</option>)}
            </select>
            {bulkField === "group" || bulkField === "lineType" ? (
              <select aria-label="一括設定の値" className={cn(inputClass, "w-28 h-[22px]")} value={bulkValue} onChange={event => setBulkValue(event.target.value)}>{bulkField === "group" ? groupOptions : typeOptions}</select>
            ) : <input aria-label="一括設定の値" value={bulkValue} onChange={event => setBulkValue(event.target.value)} className={cn(inputClass, "w-20 font-mono")} />}
            <button className="px-2 py-0.5 text-[11px] text-white bg-blue-500 rounded hover:bg-blue-600" onClick={() => setRows(current => current.reduce((draft, row) => selected.has(row.name) ? patchRows(draft, row.name, { [bulkField]: bulkValue }) : draft, current))}>適用</button>
            <button className="ml-auto text-gray-400 hover:text-blue-500" onClick={() => setSelected(new Set())}>選択解除</button>
          </div>
        )}
        <div className="overflow-auto min-h-0 max-h-[420px]">
          <table className="w-max border-collapse text-[11px] whitespace-nowrap">
            <thead className="sticky top-0 z-10">
              <tr className="bg-gray-100 dark:bg-slate-700 text-[10px] text-gray-500 dark:text-gray-300">
                <th rowSpan={2} className={cn(headerClass, "w-6")}>
                  <input aria-label="表示中の行をすべて選択" type="checkbox" checked={allVisibleSelected}
                    ref={element => { if (element) element.indeterminate = !allVisibleSelected && visibleRows.some(row => selected.has(row.name)); }}
                    onChange={() => setSelected(current => { const next = new Set(current); visibleRows.forEach(row => allVisibleSelected ? next.delete(row.name) : next.add(row.name)); return next; })} />
                </th>
                <th rowSpan={2} className={cn(headerClass, "text-left")}>信号名</th>
                <th rowSpan={2} className={cn(headerClass, "text-left")}>グループ</th>
                {sections.filter(section => columnSet === "all" || columnSet === section.key).map(section => <th key={section.key} colSpan={section.columns.length} className={cn("px-2 py-0.5 border-l border-gray-300 dark:border-slate-600 font-bold", section.key === "conv" && "text-blue-600 dark:text-blue-300")}>{section.label}</th>)}
              </tr>
              <tr className="bg-gray-100 dark:bg-slate-700 text-[10px] text-gray-500 dark:text-gray-300">
                {sections.filter(section => columnSet === "all" || columnSet === section.key).flatMap(section => section.columns.map((column, index) => <th key={column.key} className={cn(headerClass, index === 0 && dividerClass)}>{column.label}</th>))}
              </tr>
            </thead>
            <tbody>
              {visibleRows.map(row => <tr key={row.name} className={cn(selected.has(row.name) ? "bg-blue-50/60 dark:bg-slate-700/60" : "bg-white dark:bg-slate-800", "hover:bg-blue-50 dark:hover:bg-slate-700")}>
                <td className={cn(cellClass, "px-2 text-center")}><input type="checkbox" aria-label={`${row.name} を選択`} checked={selected.has(row.name)} onChange={() => setSelected(current => { const next = new Set(current); if (next.has(row.name)) next.delete(row.name); else next.add(row.name); return next; })} /></td>
                <td className={cn(cellClass, "px-2 font-semibold max-w-[160px] truncate")} style={{ color: row.color }} title={row.name}>{displaySignalName(row.name, visibleNames)}</td>
                {renderCell(row, allColumns[0])}
                {sections.filter(section => columnSet === "all" || columnSet === section.key).flatMap(section => section.columns.map((column, index) => renderCell(row, column, index === 0)))}
              </tr>)}
            </tbody>
          </table>
        </div>
        <div className="px-5 py-3 border-t border-gray-200 dark:border-slate-700 flex items-center shrink-0">
          <span className="text-[11px] text-amber-600 dark:text-amber-400 flex items-center gap-1"><Pencil size={12} />{changedCount} 項目を変更</span>
          {errorCount > 0 && <span className="ml-3 text-[11px] text-red-500 flex items-center gap-1"><AlertCircle size={12} />{errorCount} 件の入力エラー</span>}
          <div className="ml-auto">
            <button onClick={onClose} className="px-3 py-1.5 mr-2 text-sm text-blue-500 bg-transparent border border-blue-500 rounded hover:bg-blue-50 dark:hover:bg-slate-700">キャンセル</button>
            <button onClick={apply} disabled={errorCount > 0} title={errorCount > 0 ? "入力エラーを直してください" : undefined} className="px-3 py-1.5 text-sm text-white bg-blue-500 rounded hover:bg-blue-600 disabled:opacity-50 disabled:cursor-not-allowed">適用</button>
          </div>
        </div>
      </div>
      {statePopover && <>
        <div className="fixed inset-0" onClick={() => setStatePopover(null)} />
        <div role="dialog" aria-label={`${statePopover.name} の状態名`} className="fixed w-[340px] max-h-[80vh] overflow-y-auto bg-white dark:bg-slate-800 border border-gray-300 dark:border-slate-600 rounded shadow-xl text-left"
          style={{ top: statePopover.top, bottom: statePopover.bottom, left: statePopover.left }}>
          <div className="flex items-center px-2.5 py-1.5 border-b border-gray-200 dark:border-slate-700">
            <span className="text-[11px] font-bold truncate" title={statePopover.name}>{displaySignalName(statePopover.name, visibleNames)} の状態名</span>
            <button aria-label="閉じる" onClick={() => setStatePopover(null)} className="ml-auto text-gray-400 hover:text-gray-600 dark:hover:text-gray-200"><X size={12} /></button>
          </div>
          <div className="px-2.5 pt-2 flex items-center gap-1 text-[10px] text-gray-500 dark:text-gray-400">
            他の信号からコピー:
            <select aria-label="コピー元の信号" value="" disabled={copySources.length === 0} className={cn(inputClass, "flex-1 h-[22px] text-[10px]")}
              onChange={event => {
                const source = rows.find(row => row.name === event.target.value);
                if (source) setStatePopover({ ...statePopover, rows: mappingToRows(parseStates(source.states) ?? {}) });
              }}>
              <option value="">{copySources.length ? "選択…" : "コピーできる信号がありません"}</option>
              {copySources.map(row => <option key={row.name} value={row.name} title={row.name}>{displaySignalName(row.name, copySourceNames)}</option>)}
            </select>
          </div>
          <div className="px-2.5 py-2">
            <StateNameEditor rows={statePopover.rows} rawData={useWaveformStore.getState().chartData?.seriesMap[statePopover.name]}
              onChange={next => setStatePopover({ ...statePopover, rows: next })} />
          </div>
          <div className="px-2.5 py-1.5 border-t border-gray-200 dark:border-slate-700 flex justify-end gap-1.5">
            <button onClick={() => setStatePopover(null)} className="px-2.5 py-0.5 text-[11px] text-blue-500 border border-blue-500 rounded hover:bg-blue-50 dark:hover:bg-slate-700">キャンセル</button>
            <button disabled={popoverErrors.some(Boolean)} onClick={() => { update(statePopover.name, "states", serializeStates(rowsToMapping(statePopover.rows))); setStatePopover(null); }}
              className="px-2.5 py-0.5 text-[11px] text-white bg-blue-500 rounded hover:bg-blue-600 disabled:opacity-50 disabled:cursor-not-allowed">OK</button>
          </div>
        </div>
      </>}
    </div>, document.body
  );
}
