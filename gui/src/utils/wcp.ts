import JSZip from "jszip";
import yaml from "js-yaml";

// Keep B's existing archive name, even if it happens to use our baseline path.
export function getCompareCSVPath(fileName: string | null): string {
  return fileName === "_compare/A.csv" ? "_compare/A_1.csv" : "_compare/A.csv";
}

export async function readWcpProject(file: File) {
  const zip = await JSZip.loadAsync(file);
  let csvContent = "", csvName = "", yamlContent = "";
  for (const [filename, entry] of Object.entries(zip.files)) {
    if (!entry.dir && /\.ya?ml$/i.test(filename)) yamlContent = await entry.async("text");
  }
  let doc: { viewState?: { compareCSVPath?: unknown; compareFileName?: unknown } } | null = null;
  try {
    doc = yamlContent ? yaml.load(yamlContent) as typeof doc : null;
  } catch {
    // Keep legacy behavior: load B, then let parseMappingData report invalid YAML.
  }
  const viewState = doc?.viewState;
  const comparePath = typeof viewState?.compareCSVPath === "string" ? viewState.compareCSVPath : null;
  for (const [filename, entry] of Object.entries(zip.files)) {
    if (!entry.dir && filename !== comparePath && /\.(csv|txt)$/i.test(filename)) {
      csvContent = await entry.async("text");
      csvName = filename;
    }
  }
  const compareEntry = comparePath ? zip.file(comparePath) : null;
  if (comparePath && !compareEntry) throw new Error("比較する CSV（A）がプロジェクト内に見つかりません。");
  return { csvContent, csvName, yamlContent,
    compareContent: compareEntry ? await compareEntry.async("text") : null,
    compareName: typeof viewState?.compareFileName === "string" ? viewState.compareFileName : "A.csv" };
}
