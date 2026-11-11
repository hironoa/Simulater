import React, { useEffect, useRef, useState } from "react";
import { useShallow } from "zustand/react/shallow";
import { X, Lock, Unlock } from "lucide-react";
import { useLiveStore } from "../../live/liveStore";
import { liveController } from "../../live/controller";
import type { InputItem, InputWidget } from "../../live/persist";
import { SymbolSearch } from "./SymbolSearch";
import { formatValue } from "../../live/format";
import { cn } from "../../utils/helpers";

const WIDGETS: { value: InputWidget; label: string }[] = [
  { value: "number", label: "数値" },
  { value: "slider", label: "スライダ" },
  { value: "toggle", label: "トグル" },
  { value: "button", label: "ボタン" },
];

const field = "px-1.5 py-0.5 text-xs rounded border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-900";

function parseNumber(text: string): number | null {
  const t = text.trim();
  if (!t) return null;
  const v = /^-?0x[0-9a-f]+$/i.test(t) ? (t.startsWith("-") ? -parseInt(t.slice(1), 16) : parseInt(t, 16)) : Number(t);
  return Number.isFinite(v) ? v : null;
}

function InputCard({ item, current }: { item: InputItem; current: number | null | undefined; key?: React.Key }) {
  const live = liveController();
  const [text, setText] = useState(item.value !== undefined ? String(item.value) : "");
  const [slider, setSlider] = useState(item.value ?? current ?? item.min);
  const [pressed, setPressed] = useState(false);
  // まだ操作していないスライダは、届いた現在値に合わせておく
  const touched = useRef(item.value !== undefined);
  useEffect(() => {
    if (!touched.current && current !== null && current !== undefined) setSlider(current);
  }, [current]);
  const lastSent = useRef(0);
  const trailing = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => { if (trailing.current) clearTimeout(trailing.current); }, []);

  const writeNumber = () => {
    const v = parseNumber(text);
    if (v === null) return;
    void live.write(item.name, v);
  };

  // スライダはドラッグ中に書き込みが詰まらないよう 50ms に 1 回まで送り、最後の値は必ず送る
  const writeSlider = (v: number) => {
    touched.current = true;
    setSlider(v);
    const now = performance.now();
    if (trailing.current) clearTimeout(trailing.current);
    if (now - lastSent.current >= 50) {
      lastSent.current = now;
      void live.write(item.name, v);
    } else {
      trailing.current = setTimeout(() => { lastSent.current = performance.now(); void live.write(item.name, v); }, 50);
    }
  };

  const pressedAt = useRef(0);
  const press = async (down: boolean) => {
    if (down === pressed) return;
    setPressed(down);
    if (down) {
      pressedAt.current = performance.now();
      await live.write(item.name, 1, true);
    } else {
      // 一瞬のクリックでも対象が 1 を読めるよう、最低 100ms（かつ 3 周期）は 1 のままにする
      const { periodMs, speed } = useLiveStore.getState().sim;
      const minMs = Math.max(100, speed > 0 ? (3 * periodMs) / speed : 0);
      const wait = minMs - (performance.now() - pressedAt.current);
      if (wait > 0) await new Promise((r) => setTimeout(r, Math.min(wait, 2000)));
      // 押している間は固定で 1 を書き続け、離したら 0 に戻す（固定 ON の項目は 0 で固定し直す）
      await live.api.release(item.name).catch(() => undefined);
      await live.write(item.name, 0, item.hold);
    }
  };

  const on = (current ?? 0) !== 0;

  return (
    <div className="rounded border border-gray-200 dark:border-slate-700 p-1.5 flex flex-col gap-1 bg-white dark:bg-slate-800">
      <div className="flex items-center gap-1">
        <span className="font-mono text-xs font-bold truncate flex-1" title={item.name}>{item.name}</span>
        <span className="font-mono text-xs text-gray-500 dark:text-gray-400" title="現在値">= {formatValue(current, "dec")}</span>
        <select className={cn(field, "text-[11px]")} value={item.widget} onChange={(e) => live.updateInput(item.name, { widget: e.target.value as InputWidget })}>
          {WIDGETS.map((w) => <option key={w.value} value={w.value}>{w.label}</option>)}
        </select>
        <button
          className={cn("flex items-center gap-0.5 text-[11px] px-1 py-0.5 rounded border",
            item.hold ? "bg-amber-100 border-amber-400 text-amber-800 dark:bg-amber-900/50 dark:text-amber-200 dark:border-amber-600"
              : "border-gray-300 dark:border-slate-600 text-gray-500 dark:text-gray-400")}
          title={item.hold ? "固定中（毎周期書き込み続けます）。押すと解除" : "固定する（毎周期この値を書き込み続けます）"}
          onClick={() => {
            const v = item.widget === "slider" ? slider : item.widget === "number" ? parseNumber(text) ?? current ?? 0 : current ?? 0;
            void live.setHold(item.name, !item.hold, v);
          }}>
          {item.hold ? <Lock size={11} /> : <Unlock size={11} />}固定
        </button>
        <button className="text-gray-400 hover:text-red-500" title="入力から外す" onClick={() => live.removeInput(item.name)}><X size={13} /></button>
      </div>

      {item.widget === "number" && (
        <div className="flex items-center gap-1">
          <input className={cn(field, "flex-1 font-mono")} value={text} placeholder="値（10 進 / 0x16 進）"
            onChange={(e) => setText(e.target.value)} onKeyDown={(e) => { if (e.key === "Enter") writeNumber(); }} />
          <button className="text-xs px-2 py-0.5 rounded bg-blue-500 text-white hover:bg-blue-600" onClick={writeNumber}>書込</button>
        </div>
      )}

      {item.widget === "slider" && (
        <div className="flex items-center gap-1">
          <input type="number" className={cn(field, "w-14")} value={item.min} title="最小"
            onChange={(e) => live.updateInput(item.name, { min: Number(e.target.value) })} />
          <input type="range" className="flex-1" min={item.min} max={item.max} step={item.step} value={slider}
            onChange={(e) => writeSlider(Number(e.target.value))} />
          <input type="number" className={cn(field, "w-14")} value={item.max} title="最大"
            onChange={(e) => live.updateInput(item.name, { max: Number(e.target.value) })} />
          <input type="number" className={cn(field, "w-12")} value={item.step} title="刻み" min={0}
            onChange={(e) => { const v = Number(e.target.value); if (v > 0) live.updateInput(item.name, { step: v }); }} />
          <span className="font-mono text-xs w-12 text-right">{slider}</span>
        </div>
      )}

      {item.widget === "toggle" && (
        <div className="flex items-center gap-2">
          <button role="switch" aria-checked={on}
            className={cn("relative w-10 h-5 rounded-full transition-colors", on ? "bg-emerald-500" : "bg-gray-300 dark:bg-slate-600")}
            onClick={() => void live.write(item.name, on ? 0 : 1)}>
            <span className={cn("absolute top-0.5 w-4 h-4 rounded-full bg-white shadow transition-all", on ? "left-5" : "left-0.5")} />
          </button>
          <span className="text-xs">{on ? "1（ON）" : "0（OFF）"}</span>
        </div>
      )}

      {item.widget === "button" && (
        <button
          className={cn("text-xs py-1.5 rounded border select-none touch-none",
            pressed ? "bg-blue-600 text-white border-blue-700" : "bg-gray-100 dark:bg-slate-700 border-gray-300 dark:border-slate-600")}
          onPointerDown={(e) => { (e.target as HTMLElement).setPointerCapture?.(e.pointerId); void press(true); }}
          onPointerUp={() => void press(false)}
          onPointerCancel={() => void press(false)}
          onLostPointerCapture={() => void press(false)}>
          押している間だけ 1
        </button>
      )}
    </div>
  );
}

export function InputPanel() {
  const live = liveController();
  const { inputs, values } = useLiveStore(useShallow((s) => ({ inputs: s.inputs, values: s.values })));
  return (
    <div className="flex flex-col gap-1.5">
      <SymbolSearch placeholder="入力にする変数を検索"
        actions={[{ label: "入力に追加", title: "入力パネルに追加", enabled: (s) => s.writable, onPick: (s) => live.addInput(s.name) }]} />
      {inputs.length === 0 && <div className="text-[11px] text-gray-400 px-1">変数を検索して入力に追加してください。</div>}
      {inputs.map((item) => <InputCard key={item.name} item={item} current={values[item.name]} />)}
    </div>
  );
}
