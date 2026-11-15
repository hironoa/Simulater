/** 表示だけを短縮する。識別・保存・通信には元の名前を使う。 */
export function displaySignalName(name: string, all?: readonly string[]): string {
  const shortName = (value: string) => value.slice(value.lastIndexOf("/") + 1) || value;
  const short = shortName(name);
  return all?.some(other => other !== name && shortName(other) === short) ? name : short;
}
