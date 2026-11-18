/** 表示だけを短縮する。識別・保存・通信には元の名前を使う。 */
export function displaySignalName(name: string, all?: readonly string[]): string {
  const shortName = (value: string) => value.slice(value.lastIndexOf("/") + 1) || value;
  const short = shortName(name);
  return all?.some(other => other !== name && shortName(other) === short) ? name : short;
}

/**
 * 左の信号設定に並んでいる信号（グループ → 未分類の順）。表示名の重なりはこの一覧と比べる。
 * signalSettings には、前に開いたファイルや yaml の信号の設定も残るので、そちらと比べると、
 * 今は無い「ファイル名/信号名」と重なって、信号名だけで出せるものまで元の名前で出てしまう。
 */
export function listedSignals(state: { signalGroups: readonly { signals: readonly string[] }[]; unassignedSignals: readonly string[] }): string[] {
  return [...new Set([...state.signalGroups.flatMap(g => g.signals), ...state.unassignedSignals])];
}
