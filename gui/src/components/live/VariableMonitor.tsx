import { useShallow } from "zustand/react/shallow";
import { X, AlertCircle } from "lucide-react";
import { useLiveStore } from "../../live/liveStore";
import { liveController } from "../../live/controller";
import { useWaveformStore } from "../../store/useWaveformStore";
import { formatValue, ROUNDED_MARK, ROUNDED_TITLE } from "../../live/format";
import { mayBeRounded } from "../../live/inputValue";
import { SymbolSearch } from "./SymbolSearch";
import { cn } from "../../utils/helpers";

/** 変数モニタ: 監視変数の現在値（10 進/16 進）、追加・削除、波形に出すかの切り替え。 */
export function VariableMonitor() {
  const live = liveController();
  const { watch, values, errors, serverWatch } = useLiveStore(useShallow((s) => ({
    watch: s.watch, values: s.values, errors: s.watchErrors, serverWatch: s.serverWatch,
  })));
  const colors = useWaveformStore(useShallow((s) => Object.fromEntries(watch.map((w) => [w.name, s.signalSettings[w.name]?.color ?? "#999"]))));
  const watched = new Set(serverWatch);
  const allPlot = watch.length > 0 && watch.every((w) => w.plot);

  return (
    <div className="flex flex-col gap-1.5">
      <SymbolSearch placeholder="監視する変数を検索"
        actions={[{ label: "監視", title: "監視一覧に追加", onPick: (s) => live.addWatch(s.name) }]} />
      {watch.length === 0 ? (
        <div className="text-[11px] text-gray-400 px-1">変数を検索して監視に追加してください。</div>
      ) : (
        <table className="w-full text-xs">
          <thead>
            <tr className="text-[10px] text-gray-500 dark:text-gray-400 border-b border-gray-200 dark:border-slate-700">
              <th className="text-left font-normal py-0.5" title="波形に出す">
                <input type="checkbox" checked={allPlot} onChange={(e) => watch.forEach((w) => live.updateWatch(w.name, { plot: e.target.checked }))} aria-label="すべて波形に出す" /> 波形
              </th>
              <th className="text-left font-normal">変数</th>
              <th className="text-right font-normal">現在値</th>
              <th className="font-normal">表記</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {watch.map((w) => {
              const err = errors[w.name];
              return (
                <tr key={w.name} className="border-b border-gray-100 dark:border-slate-800 hover:bg-gray-50 dark:hover:bg-slate-800/60">
                  <td className="py-0.5 w-10">
                    <label className="flex items-center gap-1">
                      <input type="checkbox" checked={w.plot} onChange={(e) => live.updateWatch(w.name, { plot: e.target.checked })} />
                      <span className="inline-block w-2.5 h-2.5 rounded-sm" style={{ background: colors[w.name] }} />
                    </label>
                  </td>
                  <td className={cn("font-mono truncate max-w-[120px]", err && "text-red-500")} title={err ? `${w.name}: ${err}` : w.name}>
                    {err && <AlertCircle size={11} className="inline mr-0.5 -mt-0.5" />}
                    {w.name}
                    {!err && serverWatch.length > 0 && !watched.has(w.name) && <span className="text-[10px] text-gray-400">（同期待ち）</span>}
                  </td>
                  <td className="font-mono text-right tabular-nums" title={!err && mayBeRounded(values[w.name]) ? ROUNDED_TITLE : undefined}>
                    {err ? "—" : formatValue(values[w.name], w.radix)}
                    {!err && mayBeRounded(values[w.name]) && <span className="text-amber-600 dark:text-amber-400">{ROUNDED_MARK}</span>}
                  </td>
                  <td className="text-center">
                    <button className="text-[10px] px-1 rounded border border-gray-300 dark:border-slate-600 hover:bg-gray-100 dark:hover:bg-slate-700"
                      title="10 進 / 16 進を切り替え" onClick={() => live.updateWatch(w.name, { radix: w.radix === "dec" ? "hex" : "dec" })}>
                      {w.radix === "dec" ? "10進" : "16進"}
                    </button>
                  </td>
                  <td className="w-4">
                    <button className="text-gray-400 hover:text-red-500" title="監視から外す" onClick={() => live.removeWatch(w.name)}><X size={12} /></button>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
    </div>
  );
}
