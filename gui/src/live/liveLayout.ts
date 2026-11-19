import type { AppState } from "../types";
import { activeDisplaySignals, buildDisplayBlocks, layoutBlocks } from "../utils/displayBlocks";
import { getConversion, getDisplayConversion } from "../utils/helpers";
import { signalTitle, signalLineType, getTextWidth, maxTitleWidth, truncateTitle } from "../utils/laneLabels";
import type { LiveCanvasLane, LiveCanvasSeries } from "./liveCanvas";

type LiveLayoutSettings = Pick<AppState, "signalSettings" | "signalGroups" | "unassignedSignals" |
  "unassignedGroupConfig" | "layoutMode" | "showSignalNames" | "unitMapping" | "showAlias" | "lsbMapping" | "offsetMapping" | "valueDisplayMode" | "modeMapping">;

export function buildLiveLanes(state: LiveLayoutSettings, names: readonly string[], height: number,
  visible?: (sig: string) => boolean): LiveCanvasLane[] {
  const active = activeDisplaySignals(names, state.signalSettings, visible);
  const blocks = buildDisplayBlocks(active, state.signalGroups, state.unassignedSignals, state.unassignedGroupConfig);
  const h = Math.max(0, height);
  // 横軸の目盛りとmsの行に最低30pxを確保する。十分な高さでは停止時と同じ8%。
  const bottomMargin = h > 0 ? Math.max(8, 30 / h * 100) : 8;
  const positions = layoutBlocks(blocks, state.signalSettings, state.layoutMode, h, 35, bottomMargin);
  const toSeries = (sig: string): LiveCanvasSeries => {
    const set = state.signalSettings[sig];
    const conv = getConversion(sig, state.lsbMapping, state.offsetMapping);
    const raw = state.valueDisplayMode === "raw";
    const rangeValue = (value: number | null) => value === null ? null : raw ? (value - conv.offset) / conv.lsb : value;
    const min = rangeValue(set.yMin), max = rangeValue(set.yMax);
    return {
      // yaml の信号は状態名がなくても空の表を持つので、空なら状態名なしとする（重ね表示で状態の系列を選び違えないため）
      modeMapping: Object.keys(state.modeMapping[sig] ?? {}).length ? state.modeMapping[sig] : undefined,
      sig, color: set.color, label: state.showSignalNames ? signalTitle(sig, active, set, state) : "",
      conv: getDisplayConversion(sig, state.lsbMapping, state.offsetMapping, state.valueDisplayMode),
      yMin: set.yAuto ? null : raw && conv.lsb < 0 ? max : min,
      yMax: set.yAuto ? null : raw && conv.lsb < 0 ? min : max,
      lineWidth: set.lineWidth ?? 1.5,
      lineType: signalLineType(sig, set.lineType),
    };
  };
  // 縦軸の同期は止まっているときの表示（chartOptionsBuilder）と同じ: 重ね表示は syncRange、並べる表示は syncGroup
  return blocks.map((block, i) => ({
    top: positions[i].top * h / 100,
    height: Math.max(0, positions[i].height) * h / 100,
    ...(block.type === "single"
      ? { series: [toSeries(block.sig)], syncKey: block.syncGroup?.join(",") ?? null,
          ...(block.syncGroup ? { rangeSeries: block.syncGroup.map(toSeries) } : {}) }
      : { series: block.signals.map(toSeries), syncRange: block.syncRange }),
  }));
}

/** 名前の幅と省略結果はレーン構成・キャンバス幅が変わるときだけ求める。 */
export function fitLiveLabels(lanes: LiveCanvasLane[], containerWidth: number) {
  const limit = containerWidth > 0 ? maxTitleWidth(containerWidth) : Infinity;
  const titleWidth = Math.min(limit, Math.max(0,
    ...lanes.flatMap(lane => lane.series.map(series => getTextWidth(series.label)))));
  const layout = lanes.map(lane => ({ ...lane,
    series: lane.series.map(series => ({ ...series, label: truncateTitle(series.label, limit) })),
  }));
  return { layout, titleWidth };
}
