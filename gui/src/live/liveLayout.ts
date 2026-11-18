import type { AppState } from "../types";
import { activeDisplaySignals, buildDisplayBlocks, layoutBlocks } from "../utils/displayBlocks";
import { getConversion, getDisplayConversion } from "../utils/helpers";
import { displaySignalName } from "../utils/displaySignalName";
import type { LiveCanvasLane, LiveCanvasSeries } from "./liveCanvas";

type LiveLayoutSettings = Pick<AppState, "signalSettings" | "signalGroups" | "unassignedSignals" |
  "unassignedGroupConfig" | "layoutMode" | "showAlias" | "lsbMapping" | "offsetMapping" | "valueDisplayMode">;

export function buildLiveLanes(state: LiveLayoutSettings, names: readonly string[], height: number,
  visible?: (sig: string) => boolean): LiveCanvasLane[] {
  const active = activeDisplaySignals(names, state.signalSettings, visible);
  const blocks = buildDisplayBlocks(active, state.signalGroups, state.unassignedSignals, state.unassignedGroupConfig);
  // 上下の余白は従来の 10px / 28px。極小のキャンバスでも領域をはみ出させない。
  const h = Math.max(0, height);
  const top = Math.min(10, h), bottom = Math.min(28, h - top);
  const positions = layoutBlocks(blocks, state.signalSettings, state.layoutMode, h || 1,
    top, bottom / (h || 1) * 100, 14 / (h || 1) * 100);
  const toSeries = (sig: string): LiveCanvasSeries => {
    const set = state.signalSettings[sig];
    const conv = getConversion(sig, state.lsbMapping, state.offsetMapping);
    const raw = state.valueDisplayMode === "raw";
    const rangeValue = (value: number | null) => value === null ? null : raw ? (value - conv.offset) / conv.lsb : value;
    const min = rangeValue(set.yMin), max = rangeValue(set.yMax);
    return {
      sig, color: set.color, label: (state.showAlias && set.alias) || displaySignalName(sig, active),
      conv: getDisplayConversion(sig, state.lsbMapping, state.offsetMapping, state.valueDisplayMode),
      yMin: set.yAuto ? null : raw && conv.lsb < 0 ? max : min,
      yMax: set.yAuto ? null : raw && conv.lsb < 0 ? min : max,
      lineWidth: set.lineWidth ?? 1.5,
    };
  };
  // 縦軸の同期は止まっているときの表示（chartOptionsBuilder）と同じ: 重ね表示は syncRange、並べる表示は syncGroup
  return blocks.map((block, i) => ({
    top: positions[i].top * h / 100,
    height: positions[i].height * h / 100,
    ...(block.type === "single"
      ? { series: [toSeries(block.sig)], syncKey: block.syncGroup?.join(",") ?? null,
          ...(block.syncGroup ? { rangeSeries: block.syncGroup.map(toSeries) } : {}) }
      : { series: block.signals.map(toSeries), syncRange: block.syncRange }),
  }));
}
