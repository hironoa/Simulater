import { useEffect, useRef, useState } from "react";
import { useShallow } from "zustand/react/shallow";
import { Play, Pause, StepForward, RotateCcw, Download, Upload, AlertTriangle, PlugZap, Unplug } from "lucide-react";
import { useLiveStore } from "../../live/liveStore";
import { liveController } from "../../live/controller";
import { cn } from "../../utils/helpers";

const SPEEDS: { value: number; label: string }[] = [
  { value: 0.1, label: "×0.1" }, { value: 0.25, label: "×0.25" }, { value: 0.5, label: "×0.5" },
  { value: 1, label: "×1（実時間）" }, { value: 2, label: "×2" }, { value: 5, label: "×5" },
  { value: 10, label: "×10" }, { value: 100, label: "×100" }, { value: 0, label: "最速" },
];

const btn = "flex items-center gap-1 px-2 py-1 text-xs rounded border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800 hover:bg-gray-100 dark:hover:bg-slate-700 disabled:opacity-40 disabled:cursor-not-allowed";
const field = "px-1.5 py-0.5 text-xs rounded border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800";

/** 接続バー: 実行/一時停止/ステップ/リセット、速度、周期、現在ステップ、オーバーラン、対象関数、CSV。 */
export function LiveBar() {
  const live = liveController();
  const { sim, connected, functions, windowSize, maxSamples } = useLiveStore(useShallow((s) => ({
    sim: s.sim, connected: s.connected, functions: s.info?.functions ?? [], windowSize: s.windowSize, maxSamples: s.maxSamples,
  })));
  const [steps, setSteps] = useState(1);
  const [period, setPeriod] = useState(String(sim.periodMs));
  const [win, setWin] = useState(String(windowSize));
  const [max, setMax] = useState(String(maxSamples));
  const fileRef = useRef<HTMLInputElement>(null);

  useEffect(() => setPeriod(String(sim.periodMs)), [sim.periodMs]);
  useEffect(() => setWin(String(windowSize)), [windowSize]);
  useEffect(() => setMax(String(maxSamples)), [maxSamples]);

  const commitPeriod = () => {
    const v = Number(period);
    if (Number.isFinite(v) && v > 0 && v !== sim.periodMs) void live.control({ periodMs: v });
    else setPeriod(String(sim.periodMs));
  };
  const commitWin = () => {
    const v = Number(win);
    if (Number.isFinite(v) && v >= 50) live.setWindowSize(v); else setWin(String(windowSize));
  };
  const commitMax = () => {
    const v = Number(max);
    if (Number.isFinite(v) && v >= 1000) live.setMaxSamples(v); else setMax(String(maxSamples));
  };
  const funcs = functions.includes(sim.func) || !sim.func ? functions : [sim.func, ...functions];

  return (
    <div className="flex flex-wrap items-center gap-x-3 gap-y-1.5 px-2.5 py-1.5 bg-slate-100 dark:bg-slate-800 border-b border-gray-300 dark:border-slate-700 text-xs text-gray-700 dark:text-gray-200">
      <span className={cn("flex items-center gap-1 font-bold", connected ? "text-emerald-600 dark:text-emerald-400" : "text-red-600 dark:text-red-400")}
        title={connected ? "sim に接続しています" : "ストリームが切れています（自動で再接続します）"}>
        {connected ? <PlugZap size={14} /> : <Unplug size={14} />}
        {connected ? "接続中" : "未接続"}
      </span>

      <div className="flex items-center gap-1">
        {sim.running ? (
          <button className={cn(btn, "text-amber-700 dark:text-amber-300")} onClick={() => live.control({ cmd: "pause" })} title="一時停止（全区間をズーム・計測できます）">
            <Pause size={14} /> 一時停止
          </button>
        ) : (
          <button className={cn(btn, "text-emerald-700 dark:text-emerald-300")} onClick={() => live.control({ cmd: "run" })} title="実行">
            <Play size={14} /> 実行
          </button>
        )}
        <button className={btn} disabled={sim.running} onClick={() => live.control({ cmd: "step", steps })} title="N 周期だけ進めて止める">
          <StepForward size={14} /> ステップ
        </button>
        <input type="number" min={1} value={steps} aria-label="ステップ実行の周期数"
          onChange={(e) => setSteps(Math.max(1, Math.floor(Number(e.target.value) || 1)))}
          className={cn(field, "w-16")} title="ステップ実行する周期数" />
        <button className={btn} onClick={() => { if (confirm("変数を起動時の状態に戻し、記録を消します。よろしいですか？")) void live.control({ cmd: "reset" }); }} title="リセット">
          <RotateCcw size={14} /> リセット
        </button>
      </div>

      <label className="flex items-center gap-1">速度
        <select className={field} value={String(sim.speed)} onChange={(e) => live.control({ speed: Number(e.target.value) })}>
          {!SPEEDS.some((s) => s.value === sim.speed) && <option value={String(sim.speed)}>×{sim.speed}</option>}
          {SPEEDS.map((s) => <option key={s.value} value={String(s.value)}>{s.label}</option>)}
        </select>
      </label>

      <label className="flex items-center gap-1">周期
        <input type="number" min={0.1} step={0.1} className={cn(field, "w-16")} value={period}
          onChange={(e) => setPeriod(e.target.value)} onBlur={commitPeriod}
          onKeyDown={(e) => { if (e.key === "Enter") commitPeriod(); }} />ms
      </label>

      <label className="flex items-center gap-1">対象関数
        <select className={cn(field, "max-w-[180px]")} value={sim.func} onChange={(e) => live.control({ func: e.target.value })}>
          {funcs.length === 0 && <option value="">（なし）</option>}
          {funcs.map((f) => <option key={f} value={f}>{f}</option>)}
        </select>
      </label>

      <span className="font-mono" title="現在のステップ（周期の回数）と、その時刻">
        ステップ <b>{sim.step.toLocaleString()}</b>
        <span className="text-gray-500 dark:text-gray-400"> （{Number((sim.step * sim.periodMs / 1000).toFixed(3))} s）</span>
      </span>

      {sim.overrun && (
        <span className="flex items-center gap-1 px-1.5 py-0.5 rounded bg-red-100 dark:bg-red-900/50 text-red-700 dark:text-red-300 font-bold"
          title="実時間に追いついていません（周期を詰めずに続行しています）">
          <AlertTriangle size={13} /> オーバーラン
        </span>
      )}

      <div className="flex items-center gap-1 ml-auto">
        <label className="flex items-center gap-1" title="実行中に表示する直近の点数">表示
          <input type="number" min={50} step={100} className={cn(field, "w-20")} value={win}
            onChange={(e) => setWin(e.target.value)} onBlur={commitWin} onKeyDown={(e) => { if (e.key === "Enter") commitWin(); }} />点
        </label>
        <label className="flex items-center gap-1" title="画面に保持する記録の上限（信号あたりの点数）。超えたら古い側から捨てます。大きくすると一時停止したときの表示が重くなります。全記録は CSV 保存で取れます">上限
          <input type="number" min={1000} step={10000} className={cn(field, "w-24")} value={max}
            onChange={(e) => setMax(e.target.value)} onBlur={commitMax} onKeyDown={(e) => { if (e.key === "Enter") commitMax(); }} />点
        </label>
        <button className={btn} onClick={() => live.exportCsv()} title="開始からの記録を WinAMS 形式の CSV で保存">
          <Download size={14} /> 記録を CSV 保存
        </button>
        <button className={btn} onClick={() => fileRef.current?.click()} title="テスト CSV を読み込んで先頭から再生">
          <Upload size={14} /> テスト CSV 再生
        </button>
        <input ref={fileRef} type="file" accept=".csv,text/csv" className="hidden"
          onChange={(e) => { const f = e.target.files?.[0]; if (f) void live.loadCsv(f); e.target.value = ""; }} />
      </div>
    </div>
  );
}
