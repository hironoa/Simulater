import React, { useEffect, useRef, useState } from "react";
import { useShallow } from "zustand/react/shallow";
import { X, Lock, Unlock } from "lucide-react";
import { useLiveStore } from "../../live/liveStore";
import { liveController } from "../../live/controller";
import type { InputItem, InputWidget } from "../../live/persist";
import { SymbolSearch } from "./SymbolSearch";
import { formatValue, ROUNDED_MARK, ROUNDED_TITLE } from "../../live/format";
import {
  buttonRestoreValue, cellNumber, holdInputValue, inputValue, mayBeRounded, numberInputValue, type InputValue,
} from "../../live/inputValue";
import { cn } from "../../utils/helpers";

const WIDGETS: { value: InputWidget; label: string }[] = [
  { value: "number", label: "数値" },
  { value: "slider", label: "スライダ" },
  { value: "toggle", label: "トグル" },
  { value: "button", label: "ボタン" },
];

const field = "px-1.5 py-0.5 text-xs rounded border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-900";

interface InputCardProps {
  item: InputItem;
  /** メモリの現在値 */
  current: number | null | undefined;
  /** 反映待ちの値（書いたセルの文字列）。無ければ undefined */
  pending: string | undefined;
  /** 固定の値（届いたまま文字列にしたもの）。固定していなければ undefined */
  held: string | undefined;
  /** 入力の元にする値（反映待ち → 固定の値 → 現在値） */
  base: InputValue;
  key?: React.Key;
}

function InputCard({ item, current, pending, held, base }: InputCardProps) {
  const live = liveController();
  // 数値入力は書いた文字列のまま持つ（Number に通すと 2^53 を超える整数が丸まる）
  const [text, setText] = useState(item.value !== undefined ? String(item.value) : "");
  const [slider, setSlider] = useState(cellNumber(item.value) ?? base.value ?? item.min);
  const [pressed, setPressed] = useState(false);
  const textRef = useRef<HTMLInputElement>(null);
  // スライダは、利用者が最後に設定した値（反映待ち・固定）に合わせる。まだ操作していなければ現在値にも合わせる。
  // 書き込みはステップまでメモリに反映されないので、現在値だけを見ると書いた直後に古い値へ戻って見える
  const touched = useRef(item.value !== undefined);
  const lastSlide = useRef(0);
  useEffect(() => {
    if (base.value === null) return;
    if (base.source === "current" && touched.current) return;
    if (performance.now() - lastSlide.current < 500) return; // ドラッグ中は届いた値で戻さない
    setSlider(base.value);
  }, [base.value, base.source]);
  // 数値入力も、ほかから設定された反映待ち・固定の値に合わせる（入力中は触らない）。数値に直さず書いた表記のまま出す
  const shown = pending ?? held;
  useEffect(() => {
    if (shown === undefined) return;
    if (textRef.current && document.activeElement === textRef.current) return;
    setText(shown);
  }, [shown]);
  const lastSent = useRef(0);
  const trailing = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => { if (trailing.current) clearTimeout(trailing.current); }, []);

  // 書式の検証はサーバに任せ、400 ならそのメッセージを出す（'文字列' や 1|2|3 も書ける）。空欄は送らない
  const writeNumber = () => {
    const v = numberInputValue(text);
    if (v === null) return;
    void live.write(item.name, v);
  };

  // スライダはドラッグ中に書き込みが詰まらないよう 50ms に 1 回まで送り、最後の値は必ず送る
  const writeSlider = (v: number) => {
    touched.current = true;
    lastSlide.current = performance.now();
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
  /** 押す前の値（離したらこれに戻す）。丸められた現在値しか無ければ null（戻さずに知らせる） */
  const restoreTo = useRef<number | string | null>(0);
  const press = async (down: boolean) => {
    if (down === pressed) return;
    setPressed(down);
    if (down) {
      pressedAt.current = performance.now();
      restoreTo.current = buttonRestoreValue(base);
      await live.write(item.name, 1, true);
    } else {
      // 一瞬のクリックでも対象が 1 を読めるよう、最低 100ms（かつ 3 周期）は 1 のままにする
      const { periodMs, speed } = useLiveStore.getState().sim;
      const minMs = Math.max(100, speed > 0 ? (3 * periodMs) / speed : 0);
      const wait = minMs - (performance.now() - pressedAt.current);
      if (wait > 0) await new Promise((r) => setTimeout(r, Math.min(wait, 2000)));
      // 押している間は固定で 1 を書き続け、離したら押す前の値に戻す（固定 ON の項目はその値で固定し直す）
      await live.releaseButton(item.name, restoreTo.current);
    }
  };

  // トグルは反映待ち → 固定 → 現在値 を元に反転する（停止中に続けて押しても、書いた値を元に反転する）
  const on = (base.value ?? 0) !== 0;
  const unmonitored = item.monitor === false;

  return (
    <div className="rounded border border-gray-200 dark:border-slate-700 p-1.5 flex flex-col gap-1 bg-white dark:bg-slate-800">
      <div className="flex items-center gap-1">
        <span className="font-mono text-xs font-bold truncate flex-1" title={item.name}>{item.name}</span>
        <span className="font-mono text-xs text-gray-500 dark:text-gray-400"
          title={unmonitored ? "今の対象関数では解決できないため監視から外れています。書き込めると監視に戻ります" : "現在値"}>
          = {unmonitored ? "—" : formatValue(current, "dec")}
          {!unmonitored && mayBeRounded(current) && <span title={ROUNDED_TITLE}>{ROUNDED_MARK}</span>}
        </span>
        {pending !== undefined && (
          <span className="font-mono text-[11px] px-1 rounded bg-sky-100 text-sky-800 dark:bg-sky-900/50 dark:text-sky-200 truncate max-w-[9rem]"
            title="次のステップの開始時にメモリへ書き込みます（それまで現在値は変わりません）">
            反映待ち: {pending}
          </span>
        )}
        <select className={cn(field, "text-[11px]")} value={item.widget} onChange={(e) => live.updateInput(item.name, { widget: e.target.value as InputWidget })}>
          {WIDGETS.map((w) => <option key={w.value} value={w.value}>{w.label}</option>)}
        </select>
        <button
          className={cn("flex items-center gap-0.5 text-[11px] px-1 py-0.5 rounded border",
            item.hold ? "bg-amber-100 border-amber-400 text-amber-800 dark:bg-amber-900/50 dark:text-amber-200 dark:border-amber-600"
              : "border-gray-300 dark:border-slate-600 text-gray-500 dark:text-gray-400")}
          title={item.hold ? "固定中（毎周期書き込み続けます）。押すと解除" : "固定する（毎周期この値を書き込み続けます）"}
          onClick={() => {
            // 送る値は丸めない（数値入力は欄の文字列のまま、元にする値は届いた表記のまま）
            void live.setHold(item.name, !item.hold, item.hold ? undefined : holdInputValue(item.widget, text, slider, base));
          }}>
          {item.hold ? <Lock size={11} /> : <Unlock size={11} />}固定
        </button>
        <button className="text-gray-400 hover:text-red-500" title="入力から外す" onClick={() => live.removeInput(item.name)}><X size={13} /></button>
      </div>

      {item.widget === "number" && (
        <div className="flex items-center gap-1">
          <input ref={textRef} className={cn(field, "flex-1 font-mono")} value={text} placeholder="値（テスト CSV のセルと同じ書式）"
            title="テスト CSV のセルと同じ書式で書けます（10 進、0x 16 進、負数、浮動小数、'文字列'、1|2|3）。書いた表記のまま送ります"
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

/** 固定の値を表示用の文字列にする。届いたまま文字列にし、丸められている可能性のある数値は出さない（欄に入れると送り返してしまう）。 */
function heldValue(holds: { name: string; value: number | string }[], name: string): string | undefined {
  const h = holds.find((x) => x.name === name);
  if (!h || (typeof h.value === "number" && mayBeRounded(h.value))) return undefined;
  return String(h.value);
}

export function InputPanel() {
  const live = liveController();
  const { inputs, values, pending, holds, needReset } = useLiveStore(useShallow((s) => ({
    inputs: s.inputs, values: s.values, pending: s.sim.pending, holds: s.sim.holds, needReset: s.sim.needReset,
  })));
  return (
    <fieldset disabled={needReset} className="flex flex-col gap-1.5 min-w-0" title={needReset ? "リセットが必要です" : undefined}>
      <SymbolSearch placeholder="入力にする変数を検索"
        actions={[{ label: "入力に追加", title: "入力パネルに追加", enabled: (s) => s.writable, onPick: (s) => live.addInput(s.name) }]} />
      {inputs.length === 0 && <div className="text-[11px] text-gray-400 px-1">変数を検索して入力に追加してください。</div>}
      {inputs.map((item) => (
        <InputCard key={item.name} item={item} current={values[item.name]}
          pending={pending.find((p) => p.name === item.name)?.value}
          held={heldValue(holds, item.name)}
          base={inputValue(item.name, { pending, holds }, values[item.name])} />
      ))}
    </fieldset>
  );
}
