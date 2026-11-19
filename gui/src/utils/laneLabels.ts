import type { AppState, SignalSetting } from "../types";
import { displaySignalName } from "./displaySignalName";
import { getDisplayUnit } from "./helpers";

export const FONT_MONO = '"BIZ UDGothic", "JetBrains Mono", monospace';
export const RIGHT_AXIS_WIDTH = 35;

type TitleSettings = Pick<AppState, "showAlias" | "unitMapping" | "modeMapping" | "valueDisplayMode">;

export function signalTitle(sig: string, active: readonly string[], set: Pick<SignalSetting, "alias">,
  settings: TitleSettings): string {
  const name = (settings.showAlias && set.alias) || displaySignalName(sig, active);
  const unit = getDisplayUnit(sig, settings.unitMapping, settings.modeMapping, settings.valueDisplayMode);
  return unit ? `${name} [${unit}]` : name;
}

/** ECharts のタイトル幅と同じ見積もり。Canvas の measureText は使わない。 */
export function getTextWidth(text: string, fontSize = 10): number {
  let width = 0;
  for (let i = 0; i < text.length; i++) width += text.charCodeAt(i) > 255 ? fontSize * 1.5 : fontSize * 0.6;
  return width;
}

export function maxTitleWidth(containerWidth: number): number {
  return Math.max(40, containerWidth * 0.3);
}

/** 幅の見積もりだけで省略する。構成・幅の変更時に使い、描画中のmeasureTextを避ける。 */
export function truncateTitle(text: string, width: number): string {
  if (getTextWidth(text) <= width) return text;
  const ellipsis = "…";
  let used = getTextWidth(ellipsis);
  if (used > width) return "";
  let prefix = "";
  for (const char of text) {
    const next = getTextWidth(char);
    if (used + next > width) break;
    prefix += char;
    used += next;
  }
  return prefix + ellipsis;
}

export function laneLabelTop(top: number, height: number, count: number, index: number): number {
  return top + height / 2 - (count * 14 + (count - 1) * 4) / 2 + index * 18;
}

export function leftMargin(showNames: boolean, titleWidth: number, tickCharacters: number): number {
  const axisWidth = tickCharacters * 6 + 4;
  return showNames ? 5 + titleWidth + 6 + axisWidth : Math.max(50, axisWidth + 5);
}

export function rightAxisWidth(tickCharacters = 0): number {
  return Math.max(RIGHT_AXIS_WIDTH, tickCharacters * 6 + 4 + 4);
}

/** 右余白を領域幅の3割以内に収める。幅未取得時は従来の本数。 */
export function visibleRightAxes(maxRightAxes: number, axisWidth: number, containerWidth: number): number {
  if (!(containerWidth > 0)) return maxRightAxes;
  return Math.min(maxRightAxes, Math.max(0, Math.floor((containerWidth * 0.3 - 15) / axisWidth)));
}

export function rightMargin(maxRightAxes: number, tickCharacters = 0): number {
  return 15 + maxRightAxes * rightAxisWidth(tickCharacters);
}

export function laneSplitNumber(height: number): number {
  return height < 30 ? 1 : height < 60 ? 2 : height < 100 ? 3 : 5;
}

export function signalLineType(sig: string, type?: SignalSetting["lineType"]): NonNullable<SignalSetting["lineType"]> {
  return type ?? (/exp/i.test(sig) || sig.includes("期待値") ? "dashed" : "solid");
}
