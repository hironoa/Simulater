// 入力パネルが「いまの値」として使う値を決める。
// 書き込みは停止中も次のステップの開始時まで反映されないので、メモリの現在値だけを見ると、
// 書いた直後のトグルの反転やスライダの表示が古い値に戻って見える。
//
// サーバへ送り返す値は丸めない。`/api/write` は文字列の値をテスト CSV のセルと同じ書式として書いた表記のまま読むので、
// 利用者が書いた文字列（`"9007199254740993"`、`"0x20000000000001"`）は Number に通さずにそのまま送る。
// SSE の samples / values は JSON の数値なので、2^53 を超える整数は届いた時点で丸まっている。そうした現在値は送り返さない。

import type { PendingWrite, SimState, SymbolKind } from "./api";

/** セルの文字列（`"7"`、`"-3"`、`"0x78"`、`"1.5"`、`"'A'"`）を数値にする。数値にできなければ null。表示・判定用で、送る値には使わない。 */
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

/**
 * 数値が JSON の数値として届いたままで正確か。2^53 を超える整数は丸められている可能性がある
 * （`9007199254740993` は `9007199254740992` として届く）。
 */
export function isExactNumber(v: number): boolean {
  return Number.isFinite(v) && (!Number.isInteger(v) || Number.isSafeInteger(v));
}

/** 表示する数値が丸められている可能性があるか（安全整数の範囲を超えた整数）。 */
export function mayBeRounded(v: number | null | undefined): boolean {
  return typeof v === "number" && Number.isFinite(v) && Number.isInteger(v) && !Number.isSafeInteger(v);
}

export type InputValueSource = "pending" | "hold" | "current";

export interface InputValue {
  /** 数値にした値（表示・トグルの反転用）。2^53 を超える整数は丸まっていることがある */
  value: number | null;
  source: InputValueSource;
  /**
   * サーバへ送り返してよい値。反映待ち・固定は届いた値そのもの（文字列なら書いた表記のまま）、
   * 現在値は正確な数値のときだけ。送れる値が無ければ null。
   */
  send: number | string | null;
  /** 送れる値が無いのは、現在値が丸められている可能性があるため（2^53 を超える整数） */
  inexact: boolean;
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
  if (p && pv !== null) return { value: pv, source: "pending", send: p.value, inexact: false };
  const h = sim.holds?.find((x) => x.name === name);
  const hv = h ? cellNumber(h.value) : null;
  if (h && hv !== null) {
    const exact = typeof h.value === "string" || isExactNumber(h.value);
    return { value: hv, source: "hold", send: exact ? h.value : null, inexact: !exact };
  }
  const c = current ?? null;
  const exact = c !== null && isExactNumber(c);
  return { value: c, source: "current", send: exact ? c : null, inexact: c !== null && !exact };
}

/**
 * 数値入力欄の文字列から送る値を作る。前後の空白だけを除き、そのほかは書いたまま（16 進は 16 進のまま）。
 * 書式の検証はサーバに任せる（`'文字列'` や `1|2|3` も送る）。空欄は送らないので null。
 */
export function numberInputValue(text: string): string | null {
  const t = text.trim();
  return t ? t : null;
}

/**
 * 「固定」を押したときに送る値。undefined は「呼び出し側（controller）の既定に任せる」、
 * null は「正確な値が分からないので送らない」。
 * - 数値入力: 欄の文字列 → 元にする値。
 * - スライダ: スライダの値。ただし元にする値から合わせただけ（同じ数値）なら、元にする値の表記のまま送る。
 * - トグル・ボタン: 元にする値（無ければ 0）。
 */
export function holdInputValue(
  widget: "number" | "slider" | "toggle" | "button" | "state",
  text: string,
  slider: number,
  base: InputValue,
): number | string | null | undefined {
  if (widget === "number") return numberInputValue(text) ?? base.send ?? (base.inexact ? null : undefined);
  if (widget === "slider") return slider === base.value && base.send !== null ? base.send : slider;
  return base.send ?? (base.inexact ? null : 0);
}

/** 押している間だけ 1 のボタンを離したときに戻す値（押す前の元にする値。正確な値が分からなければ null、何も無ければ 0）。 */
export function buttonRestoreValue(base: InputValue): number | string | null {
  return base.send ?? (base.inexact ? null : 0);
}

/**
 * サーバの state.holds は数値にできる固定値を JSON の数値で返すので、2^53 を超える整数は丸まって届く。
 * 手元で固定に書いた文字列（`known`）があり、その数値と届いた数値が同じなら、書いた表記に戻す。
 */
export function restoreHoldText(
  holds: { name: string; value: number | string }[],
  known: ReadonlyMap<string, string>,
): { name: string; value: number | string }[] {
  return holds.map((h) => {
    const text = known.get(h.name);
    if (typeof h.value !== "number" || text === undefined) return h;
    return firstCellNumber(text) === h.value ? { ...h, value: text } : h;
  });
}

/** セルの先頭の要素の数値（`1|2|3` は 1、`'AB'` は 'A'）。サーバが固定値として返す数値と比べるのに使う。 */
function firstCellNumber(text: string): number | null {
  const t = text.trim();
  if (t.startsWith("''")) return 0;
  const str = /^'(.)/u.exec(t);
  if (str) return str[1].codePointAt(0) ?? null;
  return cellNumber(t.split("|")[0]);
}

/** 反映待ちの一覧のその名前を value に置き換える（無ければ足す）。 */
export function upsertPending(list: PendingWrite[], name: string, value: string): PendingWrite[] {
  return [...list.filter((p) => p.name !== name), { name, value }];
}

/** 反映待ちの一覧からその名前を除く。 */
export function dropPending(list: PendingWrite[], name: string): PendingWrite[] {
  return list.some((p) => p.name === name) ? list.filter((p) => p.name !== name) : list;
}

/** 物理値から書き込み用の生の値を作る。整数型だけを四捨五入する。 */
export function physicalInputValue(text: string, conversion: { lsb: number; offset: number }, kind: SymbolKind): number | null {
  if (!text.trim() || !["int", "uint", "ptr", "float"].includes(kind)) return null;
  const physical = Number(text);
  if (!Number.isFinite(physical) || !Number.isFinite(conversion.lsb) || conversion.lsb === 0 || !Number.isFinite(conversion.offset)) return null;
  const raw = (physical - conversion.offset) / conversion.lsb;
  // 十進小数の割り算の誤差（25.05 → 650.4999999999999）で四捨五入の結果が変わらないよう、15 桁に整えてから丸める。
  // 15 桁を超える大きさの値は整えると桁が落ちるので、そのまま丸める。四捨五入は 0 から遠い側へ（-650.5 → -651）
  const tidy = Math.abs(raw) < 1e15 ? Number(raw.toPrecision(15)) : raw;
  const value = kind === "float" ? raw : Math.sign(tidy) * Math.round(Math.abs(tidy));
  return Number.isFinite(value) && (kind === "float" || Number.isSafeInteger(value)) ? value : null;
}

/** 表示だけを換算する。十進小数の乗算誤差を欄に残さない。 */
export function physicalInputText(raw: number | string | null | undefined, conversion: { lsb: number; offset: number }): string {
  const value = cellNumber(raw);
  if (value === null) return raw == null ? "—" : String(raw);
  const physical = value * conversion.lsb + conversion.offset;
  // 安全整数はそのまま出す（15 桁に丸めると、欄の値を書き戻したときに別の値になる）
  if (Number.isSafeInteger(physical)) return String(physical);
  return String(Number(physical.toPrecision(15)));
}
