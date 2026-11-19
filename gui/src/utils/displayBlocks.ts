import type { SignalGroup, SignalSetting } from "../types";

export type DisplayBlock =
    | {
        type: "group_overlap";
        name: string;
        signals: string[];
        syncRange: boolean;
        sig?: never;
        syncGroup?: never;
      }
    | {
        type: "single";
        sig: string;
        syncGroup: string[] | null;
        signals?: never;
        name?: never;
        syncRange?: never;
      };


export function buildDisplayBlocks(
  activeSignals: readonly string[], signalGroups: readonly Pick<SignalGroup, "name" | "signals" | "layout" | "syncRange">[],
  unassignedSignals: readonly string[], unassignedGroupConfig: Pick<SignalGroup, "layout" | "syncRange">,
): DisplayBlock[] {
  let displayBlocks: DisplayBlock[] = [];
  signalGroups.forEach((group) => {
    const groupActiveSigs = group.signals.filter((s) =>
      activeSignals.includes(s),
    );
    if (groupActiveSigs.length === 0) return;
    if (group.layout === "overlap") {
      displayBlocks.push({
        type: "group_overlap",
        name: group.name,
        signals: groupActiveSigs,
        syncRange: group.syncRange,
      });
    } else {
      groupActiveSigs.forEach((sig) => {
        displayBlocks.push({
          type: "single",
          sig: sig,
          syncGroup: group.syncRange ? groupActiveSigs : null,
        });
      });
    }
  });

  const unassignedActiveSigs = unassignedSignals.filter((s) =>
    activeSignals.includes(s),
  );
  if (unassignedActiveSigs.length > 0) {
    if (unassignedGroupConfig.layout === "overlap") {
      displayBlocks.push({
        type: "group_overlap",
        name: "未分類",
        signals: unassignedActiveSigs,
        syncRange: unassignedGroupConfig.syncRange,
      });
    } else {
      unassignedActiveSigs.forEach((sig) => {
        displayBlocks.push({
          type: "single",
          sig: sig,
          syncGroup: unassignedGroupConfig.syncRange
            ? unassignedActiveSigs
            : null,
        });
      });
    }
  }

  return displayBlocks;
}

/** 割合で配置する。停止時とライブで同じ余白・間隔を使う。 */
export function layoutBlocks(
  displayBlocks: readonly DisplayBlock[], signalSettings: Record<string, Pick<SignalSetting, "weight">>,
  layoutMode: "uniform" | "optimized" | "manual", containerHeight: number,
  topPx = 35, bottomMargin = 8, maxSpacing = 2,
): { top: number; height: number }[] {
  const N = displayBlocks.length;
  const topMargin =
    containerHeight > 0 ? (topPx / containerHeight) * 100 : 4;
  const availableHeight = 100 - topMargin - bottomMargin;
  const spacing =
    N > 1 ? Math.min(maxSpacing, (availableHeight * 0.25) / (N - 1)) : 0;

  let totalWeight = 0;
  const blockWeights = displayBlocks.map((block) => {
    if (layoutMode === "uniform") {
      totalWeight += 1;
      return 1;
    }

    let userWeight = 1;
    if (block.type === "group_overlap") {
      userWeight =
        block.signals.reduce(
          (sum: number, s: string) => sum + (signalSettings[s]?.weight ?? 1),
          0,
        ) / block.signals.length || 1;
    } else {
      userWeight = signalSettings[block.sig]?.weight ?? 1;
    }
    totalWeight += userWeight;
    return userWeight;
  });

  let currentTop = topMargin;
  return displayBlocks.map((_, idx) => {
    const height = totalWeight > 0
      ? (availableHeight - spacing * (N - 1)) * (blockWeights[idx] / totalWeight) : 0;
    const result = { top: currentTop, height };
    currentTop += height + spacing;
    return result;
  });
}

/** 通常表示とライブで共通のチェック・表示フィルター。 */
export function activeDisplaySignals(
  signals: readonly string[], settings: Record<string, Pick<SignalSetting, "checked">>,
  visible: (sig: string) => boolean = () => true,
): string[] {
  return signals.filter(sig => settings[sig]?.checked && visible(sig));
}
