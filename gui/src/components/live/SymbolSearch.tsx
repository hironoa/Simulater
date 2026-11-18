import React, { useEffect, useRef, useState } from "react";
import { ChevronRight, ChevronDown, Search } from "lucide-react";
import { liveController } from "../../live/controller";
import { memberPath, type SymbolEntry } from "../../live/api";
import { displaySignalName } from "../../utils/displaySignalName";
import { cn } from "../../utils/helpers";

export interface SymbolAction {
  label: string;
  title?: string;
  enabled?: (s: SymbolEntry) => boolean;
  onPick: (s: SymbolEntry) => void;
}

const isAggregate = (s: SymbolEntry) => s.kind === "struct" || s.kind === "array" || (s.members?.length ?? 0) > 0;

export type ResolvedSymbol = SymbolEntry & { resolutionError?: boolean };

/** 完全一致が得られない子は、失敗を表示してすべての選択操作を無効にする。 */
export async function resolveMembers(parent: SymbolEntry, api = liveController().api): Promise<ResolvedSymbol[]> {
  return Promise.all((parent.members ?? []).map(async (m) => {
    const name = memberPath(parent.name, m);
    try {
      const hits = await api.symbols(name, 20);
      const exact = hits.find((h) => h.name === name);
      if (exact) return exact;
    } catch { /* 解決失敗を行に表示する */ }
    return { name, type: "", size: 0, kind: "int", writable: false, resolutionError: true };
  }));
}

export function SymbolRow({ entry, depth, actions, names }: { entry: ResolvedSymbol; depth: number; actions: SymbolAction[]; names?: readonly string[]; key?: React.Key }) {
  const [open, setOpen] = useState(false);
  const [children, setChildren] = useState<ResolvedSymbol[] | null>(null);
  const aggregate = isAggregate(entry);
  const childNames = children?.map((child) => child.name);
  const toggle = async () => {
    if (!aggregate) return;
    if (!open && !children) setChildren(await resolveMembers(entry));
    setOpen(!open);
  };
  return (
    <>
      <div className="flex items-center gap-1 px-1 py-0.5 hover:bg-gray-100 dark:hover:bg-slate-700" style={{ paddingLeft: 4 + depth * 12 }}>
        <button className={cn("w-4 h-4 flex items-center justify-center text-gray-500", !aggregate && "invisible")} onClick={toggle}
          title={open ? "閉じる" : "メンバを展開"}>
          {open ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
        </button>
        <span className="font-mono text-xs truncate flex-1 cursor-default" title={entry.resolutionError ? `${entry.name}: 解決できません` : `${entry.name}  ${entry.type}  (${entry.kind}, ${entry.size} バイト)${entry.writable ? "" : "  書き込み不可"}`}
          onDoubleClick={toggle}>
          {displaySignalName(entry.name, names)}
        </span>
        <span className={cn("text-[10px]", entry.resolutionError ? "text-red-500" : "text-gray-400 truncate max-w-[70px]")}>{entry.resolutionError ? "解決できません" : entry.type || entry.kind}</span>
        {(!aggregate || entry.kind === "ptr") && actions.map((a) => {
          const ok = !entry.resolutionError && (a.enabled ? a.enabled(entry) : true);
          return (
            <button key={a.label} disabled={!ok} title={a.title}
              className="text-[10px] px-1.5 py-0.5 rounded bg-blue-500 text-white hover:bg-blue-600 disabled:bg-gray-300 dark:disabled:bg-slate-600 disabled:cursor-not-allowed"
              onClick={() => a.onPick(entry)}>
              {a.label}
            </button>
          );
        })}
      </div>
      {open && children?.map((c) => <SymbolRow key={c.name} entry={c} depth={depth + 1} actions={actions} names={childNames} />)}
    </>
  );
}

export function SymbolSearch({ actions, placeholder }: { actions: SymbolAction[]; placeholder?: string }) {
  const [q, setQ] = useState("");
  const [hits, setHits] = useState<SymbolEntry[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const reqId = useRef(0);
  const names = hits.map((hit) => hit.name);

  useEffect(() => {
    const query = q.trim();
    if (!query) { setHits([]); setError(null); return; }
    const id = ++reqId.current;
    const timer = setTimeout(async () => {
      setBusy(true);
      try {
        const res = await liveController().api.symbols(query, 200);
        if (id === reqId.current) { setHits(Array.isArray(res) ? res : []); setError(null); }
      } catch (e) {
        if (id === reqId.current) setError((e as Error).message);
      } finally {
        if (id === reqId.current) setBusy(false);
      }
    }, 200);
    return () => clearTimeout(timer);
  }, [q]);

  return (
    <div className="flex flex-col gap-1">
      <div className="relative">
        <Search size={12} className="absolute left-1.5 top-1/2 -translate-y-1/2 text-gray-400" />
        <input value={q} onChange={(e) => setQ(e.target.value)} placeholder={placeholder ?? "変数名で検索"}
          className="w-full pl-6 pr-2 py-1 text-xs rounded border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-900" />
      </div>
      {(hits.length > 0 || busy || error) && (
        <div className="max-h-48 overflow-auto rounded border border-gray-200 dark:border-slate-700 bg-white dark:bg-slate-900">
          {busy && hits.length === 0 && <div className="px-2 py-1 text-xs text-gray-400">検索中…</div>}
          {error && <div className="px-2 py-1 text-xs text-red-500">検索できません: {error}</div>}
          {hits.map((h) => <SymbolRow key={h.name} entry={h} depth={0} actions={actions} names={names} />)}
        </div>
      )}
      {!busy && !error && q.trim() && hits.length === 0 && (
        <div className="text-[11px] text-gray-400 px-1">見つかりません</div>
      )}
    </div>
  );
}
