import toast from "react-hot-toast";
import { useWaveformStore } from "../store/useWaveformStore";

export async function loadDroppedFiles(dataTransfer: DataTransfer, asCompare: boolean) {
  const { parseWaveformCSV, parseMappingData, setConfigHandle } = useWaveformStore.getState();
  const files = Array.from(dataTransfer.files) as File[];
  if (files.length === 0) return;

  try {
    // await の前に全 item のハンドル取得を開始する
    const items = Array.from(dataTransfer.items || []) as DataTransferItem[];
    let fileIndex = 0;
    const handlePromises = items.map((item) => {
      const file = item.kind === "file" ? files[fileIndex++] : null;
      if (item.kind !== "file" || typeof (item as any).getAsFileSystemHandle !== "function") {
        return Promise.resolve(null);
      }
      try {
        return (item as any).getAsFileSystemHandle()
          .then((handle: any) => file && handle?.kind === "file" ? [file, handle] as const : null)
          .catch(() => null);
      } catch {
        return Promise.resolve(null);
      }
    });
    const fileHandles = new Map<File, any>();
    for (const entry of await Promise.all(handlePromises)) {
      if (entry) fileHandles.set(entry[0], entry[1]);
    }

    // Check for .wcp first
    const wcpFile = files.find(f => f.name.toLowerCase().endsWith('.wcp'));
    if (wcpFile) {
      try {
        await useWaveformStore.getState().loadWcpProject(wcpFile);
        const handle = fileHandles.get(wcpFile);
        if (handle) setConfigHandle(handle);
      } catch (err: any) {
        alert("プロジェクトファイル(.wcp)の展開に失敗しました: " + err.message);
      }
      return; // Skip normal file processing if we loaded a project
    }

    const fileResults = await Promise.all(
      files.map((f: File) => {
        return new Promise<{ file: File; content: string }>(
          (resolve, reject) => {
            const reader = new FileReader();
            reader.onload = (ev) =>
              resolve({ file: f, content: ev.target?.result as string });
            reader.onerror = (err) => reject(err);
            reader.readAsText(f);
          },
        );
      }),
    );

    const waveforms = fileResults.filter(res => /\.(csv|txt)$/i.test(res.file.name) && res.content.includes("#COMMENT"));
    const store = useWaveformStore.getState();
    const openA = (res: typeof waveforms[number]) => {
      if (store.loadCompareCSV(res.content, res.file.name)) {
        const handle = fileHandles.get(res.file) ?? null;
        store.setCompareHandle(handle, handle ? res.file : undefined);
        return true;
      }
      return false;
    };
    if (asCompare && waveforms.length === 1) {
      if (waveforms[0]) openA(waveforms[0]);
    } else if (waveforms.length) {
      const sorted = [...waveforms].sort((a, b) => a.file.lastModified - b.file.lastModified);
      const res = waveforms.length > 1 ? sorted[sorted.length - 1] : waveforms[0];
      if (parseWaveformCSV(res.content, res.file.name)) {
        const handle = fileHandles.get(res.file) ?? null;
        store.setWaveformHandle(handle, handle ? res.file : undefined);
        if (waveforms.length > 1) {
          if (openA(sorted[0])) toast.success("古い方を A、新しい方を B にしました（⇆ で入れ替えられます）");
        }
      }
    }

    // 2. Map files
    let definitionApplied = false;
    fileResults.forEach((res) => {
      const name = res.file.name.toLowerCase();
      if (name.endsWith(".yaml") || name.endsWith(".yml")) {
        setTimeout(() => {
           parseMappingData(res.content, "yaml", definitionApplied);
           definitionApplied = true;
           const handle = fileHandles.get(res.file);
           if (handle) setConfigHandle(handle);
        }, 0);
      } else if (
        (name.endsWith(".csv") || name.endsWith(".txt")) &&
        !res.content.includes("#COMMENT")
      ) {
        setTimeout(() => {
           parseMappingData(res.content, "csv", definitionApplied);
           definitionApplied = true;
           const handle = fileHandles.get(res.file);
           if (handle) setConfigHandle(handle);
        }, 0);
      }
    });
  } catch (err: any) {
    alert("ファイルの読み込み中にエラーが発生しました: " + err.message);
  }
}
