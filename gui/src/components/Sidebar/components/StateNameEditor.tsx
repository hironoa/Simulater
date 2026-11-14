import React, { useEffect, useMemo, useRef } from "react";
import { ListPlus, Plus, Trash2, X } from "lucide-react";
import { useWaveformStore } from "../../../store/useWaveformStore";
import { cn } from "../../../utils/helpers";
import { StateMapping, StateRow, getStateColor, stateRowErrors } from "../../../utils/stateNames";

const MAX_FILL_VALUES = 32;

function StateChip({ value, name }: { value: number; name?: string; key?: React.Key }) {
  const theme = useWaveformStore((state) => state.theme);
  const color = getStateColor(value);
  // 帯と同じ薄さで塗り、小さくても見分けられるよう枠だけ元の色にする。
  return (
    <span
      title={name === undefined ? undefined : `${value}: ${name}`}
      className="inline-block w-2 h-2 rounded-sm shrink-0 border"
      style={{ borderColor: color, backgroundColor: color + (theme === "dark" ? "4d" : "26") }}
    />
  );
}

export function StateSummary({ mapping, max = 8 }: { mapping: StateMapping | undefined; max?: number }) {
  const entries = Object.entries(mapping ?? {}).sort((a, b) => Number(a[0]) - Number(b[0]));
  if (entries.length === 0) return <span className="text-gray-400">なし</span>;
  return (
    <span className="inline-flex items-center gap-0.5 min-w-0">
      {entries.slice(0, max).map(([value, name]) => <StateChip key={value} value={Number(value)} name={name} />)}
      {entries.length > max && <span className="text-gray-400">…</span>}
      <span className="ml-1 whitespace-nowrap">{entries.length}件</span>
    </span>
  );
}

export function StateNameEditor({ rows, rawData, onChange, onEscape }: {
  rows: StateRow[];
  rawData: (number | null)[] | undefined;
  // commit: 入力の確定（フォーカスが外れた・行の追加削除など）
  onChange: (rows: StateRow[], commit: boolean) => void;
  onEscape?: () => void;
}) {
  const errors = stateRowErrors(rows);
  const present = useMemo(() => new Set((rawData ?? []).filter((v): v is number => v !== null && Number.isFinite(v))), [rawData]);
  const missing = useMemo(() => {
    const named = new Set(rows.filter(row => row.value.trim() !== "").map(row => Number(row.value)));
    return [...present].filter(v => !named.has(v)).sort((a, b) => a - b);
  }, [present, rows]);
  const listRef = useRef<HTMLDivElement>(null);
  const focusLast = useRef(false);
  useEffect(() => {
    if (!focusLast.current) return;
    focusLast.current = false;
    listRef.current?.querySelector<HTMLInputElement>("[data-row]:last-child input")?.focus();
  }, [rows.length]);

  const edit = (index: number, patch: Partial<StateRow>) => onChange(rows.map((row, i) => i === index ? { ...row, ...patch } : row), false);
  const keyDown = (event: React.KeyboardEvent<HTMLInputElement>) => {
    if (event.nativeEvent.isComposing) return;
    if (event.key === "Enter") {
      event.preventDefault();
      event.currentTarget.blur();
    } else if (event.key === "Escape" && onEscape) {
      event.preventDefault();
      event.stopPropagation();
      onEscape();
    }
  };
  const inputClass = "w-full px-1.5 py-1 text-[11px] border bg-white dark:bg-slate-800 rounded outline-none focus:border-blue-500";
  const buttonClass = "whitespace-nowrap px-1.5 py-0.5 text-[10px] bg-white dark:bg-slate-700 border border-gray-300 dark:border-slate-600 rounded hover:bg-gray-100 dark:hover:bg-slate-600 flex items-center gap-0.5 disabled:opacity-40 disabled:cursor-not-allowed";
  const grid = "grid grid-cols-[56px_1fr_10px_16px] gap-1 items-center";

  return (
    <div>
      <div className={cn(grid, "text-[10px] text-gray-500 dark:text-gray-400 font-bold mb-1")}>
        <span className="text-right pr-1">値（生値）</span><span>状態名</span>
      </div>
      <div ref={listRef} className="flex flex-col gap-1">
        {rows.length === 0 && <div className="text-[10px] text-gray-400 py-1">状態名は設定されていません</div>}
        {rows.map((row, i) => {
          const value = row.value.trim();
          const numeric = value !== "" && Number.isFinite(Number(value));
          return (
            <div key={i} data-row className={grid}>
              <input aria-label="値" value={row.value} title={errors[i]} aria-invalid={!!errors[i]}
                onChange={(e) => edit(i, { value: e.target.value })} onBlur={() => onChange(rows, true)} onKeyDown={keyDown}
                className={cn(inputClass, "font-mono text-right", errors[i] ? "border-red-500" : "border-gray-300 dark:border-slate-600")} />
              <input aria-label="状態名" value={row.name} placeholder="状態名" title={row.name}
                onChange={(e) => edit(i, { name: e.target.value })} onBlur={() => onChange(rows, true)} onKeyDown={keyDown}
                className={cn(inputClass, "border-gray-300 dark:border-slate-600")} />
              {numeric ? <StateChip value={Number(value)} /> : <span />}
              <button type="button" aria-label="削除" title="削除" className="text-gray-400 hover:text-red-500"
                onClick={() => onChange(rows.filter((_, j) => j !== i), true)}><X size={12} /></button>
              {errors[i] && <div className="col-span-4 -mt-0.5 text-[9px] text-red-500">{errors[i]}</div>}
              {!errors[i] && numeric && rawData && !present.has(Number(value)) && (
                <div className="col-start-2 col-span-3 -mt-0.5 text-[9px] text-gray-400">この値はデータに出てきません</div>
              )}
            </div>
          );
        })}
      </div>
      <div className="flex items-center gap-1 mt-1.5">
        <button type="button" className={buttonClass} onClick={() => { focusLast.current = true; onChange([...rows, { value: "", name: "" }], true); }}>
          <Plus size={11} />行を追加
        </button>
        <button type="button" className={buttonClass} disabled={missing.length === 0 || missing.length > MAX_FILL_VALUES}
          title={missing.length > MAX_FILL_VALUES ? `値の種類が多すぎます（${MAX_FILL_VALUES} 種類まで）` : "データに出てくる値のうち、名前のないものを空行で追加"}
          onClick={() => onChange([...rows.filter(row => row.value.trim() !== "" || row.name.trim() !== ""), ...missing.map(v => ({ value: String(v), name: "" }))]
            .sort((a, b) => Number(a.value) - Number(b.value)), true)}>
          <ListPlus size={11} />データの値を追加{missing.length > 0 && ` (${missing.length})`}
        </button>
        {rows.length > 0 && (
          <button type="button" aria-label="すべて削除" title="すべて削除" className="ml-auto p-0.5 text-gray-400 hover:text-red-500" onClick={() => onChange([], true)}>
            <Trash2 size={12} />
          </button>
        )}
      </div>
      <div className="text-[10px] text-gray-400 leading-snug mt-1.5">値は LSB 変換前の生値。状態名が空の行は保存しない。色は値から自動で決まる。</div>
    </div>
  );
}
