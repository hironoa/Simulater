/** 監視値の表示。16 進は整数だけ（負数は 32 ビットの 2 の補数で表す）。浮動小数は 10 進で出す。 */
export function formatValue(v: number | null | undefined, radix: "dec" | "hex"): string {
  if (v === null || v === undefined || Number.isNaN(v)) return "—";
  if (!Number.isInteger(v)) return String(Number(v.toPrecision(7)));
  if (radix === "dec") return String(v);
  const n = v < 0 ? (v >>> 0) : v;
  let hex = n.toString(16).toUpperCase();
  if (hex.length % 2) hex = "0" + hex;
  return "0x" + hex;
}

/** 丸められている可能性のある値（2^53 を超える整数）に付ける印と、その説明。 */
export const ROUNDED_MARK = "≈";
export const ROUNDED_TITLE =
  "2^53 を超える整数は JSON の数値として届く時点で丸められている可能性があります（下位の桁が正確とは限りません）";
