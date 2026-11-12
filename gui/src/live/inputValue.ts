// 入力パネルが「いまの値」として使う値を決める。
// 書き込みは停止中も次のステップの開始時まで反映されないので、メモリの現在値だけを見ると、
// 書いた直後のトグルの反転やスライダの表示が古い値に戻って見える。

import type { PendingWrite, SimState } from "./api";

/** セルの文字列（`"7"`、`"-3"`、`"0x78"`、`"1.5"`、`"'A'"`）を数値にする。数値にできなければ null。 */
export function cellNumber(v: number | string | undefined | null): number | null {
  if (typeof v === "number") return Number.isFinite(v) ? v : null;
  if (typeof v !== "string") return null;
  const t = v.trim();
  if (/^[-+]?0x[0-9a-f]+$/i.test(t)) {
    const n = parseInt(t.replace(/^[-+]/, ""), 16);
    return t.startsWith("-") ? -n : n;
  }
  if (/^[-+]?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?$/i.test(t)) return Number(t);
  const ch = /^'(.)'$/u.exec(t);
  if (ch) return ch[1].codePointAt(0) ?? null;
  return null;
}

export type InputValueSource = "pending" | "hold" | "current";

export interface InputValue {
  value: number | null;
  source: InputValueSource;
}

/**
 * 入力の元にする値: その名前の反映待ち → 固定の値 → メモリの現在値 の順に、数値にできる最初のもの。
 * トグルの反転、押している間だけ 1 のボタンの復帰、スライダの表示に使う。
 */
export function inputValue(
  name: string,
  sim: Pick<SimState, "pending" | "holds">,
  current: number | null | undefined,
): InputValue {
  const p = sim.pending?.find((x) => x.name === name);
  const pv = p ? cellNumber(p.value) : null;
  if (pv !== null) return { value: pv, source: "pending" };
  const h = sim.holds?.find((x) => x.name === name);
  const hv = h ? cellNumber(h.value) : null;
  if (hv !== null) return { value: hv, source: "hold" };
  return { value: current ?? null, source: "current" };
}

/** 反映待ちの一覧のその名前を value に置き換える（無ければ足す）。 */
export function upsertPending(list: PendingWrite[], name: string, value: string): PendingWrite[] {
  return [...list.filter((p) => p.name !== name), { name, value }];
}

/** 反映待ちの一覧からその名前を除く。 */
export function dropPending(list: PendingWrite[], name: string): PendingWrite[] {
  return list.some((p) => p.name === name) ? list.filter((p) => p.name !== name) : list;
}
