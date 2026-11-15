import { displaySignalName } from "../utils/displaySignalName";
import React, { useRef, useState, useEffect } from "react";
import { useWaveformStore } from "../store/useWaveformStore";
import { useShallow } from "zustand/react/shallow";
import {
  FolderOpen,
  Save,
  ChevronDown,
  Archive,
  FileText,
  Table,
  Sun,
  Moon,
  FileSpreadsheet,
  Menu,
  RefreshCw,
  ArrowLeftRight,
  Pin,
} from "lucide-react";
import { cn } from "../utils/helpers";
import JSZip from "jszip";
import { getCompareCSVPath } from "../utils/wcp";
import { useWaveformAutoReload } from "../hooks/useWaveformAutoReload";
import { loadDroppedFiles } from "../utils/loadDroppedFiles";
import { compareDisplaySteps } from "../utils/compareView";
import type { WaveformFileHandle } from "../store/useWaveformStore";

export function Toolbar() {
  const { reload } = useWaveformAutoReload();
  const compareInputRef = useRef<HTMLInputElement>(null);
  const [isOpenMenuOpen, setIsOpenMenuOpen] = useState(false);
  const comparison = useWaveformStore(useShallow(s => ({
    data: s.compareChartData, name: s.compareFileName, enabled: s.compareEnabled,
    diffs: s.diffs, filter: s.diffFilter, onlyDiff: s.showOnlyDiffSignals, focus: s.focusedDiffIndex,
    swap: s.swapCompare, keepPrevious: s.keepPreviousAsA, setKeepPrevious: s.setKeepPreviousAsA,
    load: s.loadCompareCSV, close: s.closeCompare, toggle: s.setCompareEnabled,
    setFilter: s.setDiffFilter, setOnlyDiff: s.setShowOnlyDiffSignals,
    setFocus: s.setFocusedDiffIndex, setZoom: s.setZoomRange, promote: s.promoteBToA,
  })));
  const openCompareFile = async () => {
    if (!("showOpenFilePicker" in window)) {
      compareInputRef.current?.click();
      return;
    }
    try {
      const [handle] = await (window as any).showOpenFilePicker({
        types: [{ description: "比較する CSV", accept: { "text/csv": [".csv"], "text/plain": [".txt"] } }],
      });
      const file = await handle.getFile();
      if (comparison.load(await file.text(), file.name)) useWaveformStore.getState().setCompareHandle(handle, file);
    } catch (error) {
      if ((error as DOMException).name !== "AbortError") alert("比較するCSVの読み込みに失敗しました。");
    }
  };
  const visibleDiffs = comparison.diffs.filter(d => comparison.filter === "all" || d.type === "value");
  const goDiff = (direction: number) => {
    if (!visibleDiffs.length) return;
    const index = comparison.focus < 0 ? (direction > 0 ? 0 : visibleDiffs.length - 1)
      : (comparison.focus + direction + visibleDiffs.length) % visibleDiffs.length;
    const diff = visibleDiffs[index];
    const { chartData, taskPeriod } = useWaveformStore.getState();
    if (!chartData?.steps.length) return;
    const steps = comparison.enabled && comparison.data ? compareDisplaySteps(chartData, comparison.data) : chartData.steps;
    if (steps.length > chartData.steps.length) {
      const span = steps.length - 1;
      const width = Math.min(span, Math.max(250 / taskPeriod, (diff.endIndex - diff.startIndex) * 1.5));
      const left = Math.max(0, Math.min(span - width, (diff.startIndex + diff.endIndex - width) / 2));
      comparison.setZoom({ start: left / span * 100, end: (left + width) / span * 100,
        startValue: undefined, endValue: undefined });
      comparison.setFocus(index);
      return;
    }
    const first = chartData.steps[0] * taskPeriod;
    const last = chartData.steps[chartData.steps.length - 1] * taskPeriod;
    const span = last - first;
    const width = Math.min(span, Math.max(250, (diff.end - diff.start) * 1.5));
    const left = Math.max(first, Math.min(last - width, (diff.start + diff.end - width) / 2));
    comparison.setZoom({ start: span ? (left - first) / span * 100 : 0,
      end: span ? (left + width - first) / span * 100 : 100,
      startValue: undefined, endValue: undefined });
    comparison.setFocus(index);
  };
  const fileInputRef = useRef<HTMLInputElement>(null);
  const [isSaveMenuOpen, setIsSaveMenuOpen] = useState(false);

  useEffect(() => {
    const handleClickOutside = (e: MouseEvent) => {
      if (!(e.target as Element).closest('.open-dropdown-area')) setIsOpenMenuOpen(false);
      if (isSaveMenuOpen && !(e.target as Element).closest('.save-dropdown-area')) {
        setIsSaveMenuOpen(false);
      }
    };
    document.addEventListener("mousedown", handleClickOutside);
    return () => document.removeEventListener("mousedown", handleClickOutside);
  }, [isSaveMenuOpen]);

  const {
      
    waveformHandle,
    autoReload,
    setAutoReload,
    setWaveformHandle,
    fileName,
    showMode,
    showSignalNames,
    showAlias,
    showMarkers,
    showCursorLabels,
    valueDisplayMode,
    showTable,
    tablePosition,
    theme,
    toggleTheme,
    toggleShowMode,
    cycleSignalDisplay,
    toggleShowMarkers,
    toggleShowCursorLabels,
    toggleValueDisplayMode,
    cycleLayoutMode,
    toggleTableDisplay,
    setTablePosition,
    parseWaveformCSV,
    parseMappingData,
    getConfigYAML,
    configHandle,
    setConfigHandle,
    layoutMode,
    rawCSVText,
    hoveredSignal,
    isSidebarOpen,
    setIsSidebarOpen,
  
    } = useWaveformStore(useShallow((state) => ({
      waveformHandle: state.waveformHandle,
      autoReload: state.autoReload,
      setAutoReload: state.setAutoReload,
      setWaveformHandle: state.setWaveformHandle,
      fileName: state.fileName,
      showMode: state.showMode,
      showSignalNames: state.showSignalNames,
      showAlias: state.showAlias,
      showMarkers: state.showMarkers,
      showCursorLabels: state.showCursorLabels,
      valueDisplayMode: state.valueDisplayMode,
      showTable: state.showTable,
      tablePosition: state.tablePosition,
      theme: state.theme,
      toggleTheme: state.toggleTheme,
      toggleShowMode: state.toggleShowMode,
      cycleSignalDisplay: state.cycleSignalDisplay,
      toggleShowMarkers: state.toggleShowMarkers,
      toggleShowCursorLabels: state.toggleShowCursorLabels,
      toggleValueDisplayMode: state.toggleValueDisplayMode,
      cycleLayoutMode: state.cycleLayoutMode,
      toggleTableDisplay: state.toggleTableDisplay,
      setTablePosition: state.setTablePosition,
      parseWaveformCSV: state.parseWaveformCSV,
      parseMappingData: state.parseMappingData,
      getConfigYAML: state.getConfigYAML,
      configHandle: state.configHandle,
      setConfigHandle: state.setConfigHandle,
      layoutMode: state.layoutMode,
      rawCSVText: state.rawCSVText,
      hoveredSignal: state.hoveredSignal,
      isSidebarOpen: state.isSidebarOpen,
      setIsSidebarOpen: state.setIsSidebarOpen,
    })));

  const handleFileChange = async (e: React.ChangeEvent<HTMLInputElement>, handle?: WaveformFileHandle) => {
    const files = Array.from(e.target.files || []) as File[];
    if (files.length === 0) return;

    try {
      const wcpFile = files.find(f => f.name.toLowerCase().endsWith('.wcp'));
      if (wcpFile) {
        await useWaveformStore.getState().loadWcpProject(wcpFile);
        if (handle) setConfigHandle(handle);
        e.target.value = "";
        return;
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

      // 1. Waveform CSV priority
      for (const res of fileResults) {
        const name = res.file.name.toLowerCase();
        if (
          (name.endsWith(".csv") || name.endsWith(".txt")) &&
          res.content.includes("#COMMENT")
        ) {
          if (parseWaveformCSV(res.content, res.file.name)) {
            setWaveformHandle(handle ?? null, handle ? res.file : undefined);
          }
          break;
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
             if (handle) setConfigHandle(handle);
          }, 0);
        } else if (
          (name.endsWith(".csv") || name.endsWith(".txt")) &&
          !res.content.includes("#COMMENT")
        ) {
          setTimeout(() => {
             parseMappingData(res.content, "csv", definitionApplied);
             definitionApplied = true;
             if (handle) setConfigHandle(handle);
          }, 0);
        }
      });
    } catch (err: any) {
      alert("ファイルの読み込み中にエラーが発生しました: " + err.message);
    }
    
    e.target.value = "";
  };

  const handleLoadSettings = async () => {
    if ("showOpenFilePicker" in window) {
      try {
        const [fileHandle] = await (window as any).showOpenFilePicker({
          types: [
            {
              description: "波形・設定・プロジェクト",
              accept: {
                "text/csv": [".csv"],
                "text/plain": [".txt"],
                "text/yaml": [".yaml", ".yml"],
                "application/zip": [".wcp"]
              },
            },
          ],
        });
        const file = await fileHandle.getFile();
        
        // Mock a React.ChangeEvent to reuse existing parsing logic
        const fakeEvent = {
          target: { files: [file], value: "" }
        } as unknown as React.ChangeEvent<HTMLInputElement>;
        
        await handleFileChange(fakeEvent, fileHandle);
        return;
      } catch (e: any) {
        if (e.name === "AbortError") return;
        console.warn("File System Access API failed, falling back:", e);
      }
    }
    fileInputRef.current?.click();
  };

  const exportSettings = async () => {
    setIsSaveMenuOpen(false);
    const yamlStr = getConfigYAML(false);
    const downloadName =
      (fileName ? fileName.split(".")[0] : "waveform") + "_config.yaml";

    const blob = new Blob([yamlStr], { type: "text/yaml" });

    if ("showSaveFilePicker" in window) {
      try {
        const options: any = {
          suggestedName: downloadName,
          types: [{ description: "YAML Configuration", accept: { "text/yaml": [".yaml"] } }],
        };
        // 読み込んだ YAML / .wcp のフォルダ、なければ波形 CSV のフォルダから始める
        const startIn = configHandle ?? waveformHandle;
        if (startIn) options.startIn = startIn;

        const handle = await (window as any).showSaveFilePicker(options);
        const writable = await handle.createWritable();
        await writable.write(blob);
        await writable.close();
        return;
      } catch (e: any) {
        if (e.name === "AbortError") return;
        console.warn("Save API failed", e);
      }
    }

    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = downloadName;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  };

  const exportProject = async () => {
    setIsSaveMenuOpen(false);
    if (!rawCSVText || !fileName) {
      alert("保存する波形データが読み込まれていません。");
      return;
    }

    try {
      const zip = new JSZip();
      
      // 1. Add CSV
      zip.file(fileName, rawCSVText);
      const { compareChartData, compareRawCSVText } = useWaveformStore.getState();
      if (compareChartData && compareRawCSVText !== null) {
        zip.file(getCompareCSVPath(fileName), compareRawCSVText);
      }

      // 2. Add Config YAML
      const yamlStr = getConfigYAML(true);
      const yamlName = fileName.split(".")[0] + "_config.yaml";
      zip.file(yamlName, yamlStr);

      const blob = await zip.generateAsync({ type: "blob", compression: "DEFLATE", compressionOptions: { level: 6 } });
      const downloadName = (fileName ? fileName.split(".")[0] : "project") + ".wcp";

      if ("showSaveFilePicker" in window) {
        try {
          const options: any = {
            suggestedName: downloadName,
            types: [{ description: "Waveform Project", accept: { "application/zip": [".wcp"] } }],
          };
          const startIn = configHandle ?? waveformHandle;
          if (startIn) options.startIn = startIn;
          
          const handle = await (window as any).showSaveFilePicker(options);
          const writable = await handle.createWritable();
          await writable.write(blob);
          await writable.close();
          return;
        } catch (e: any) {
          if (e.name === "AbortError") return;
          console.warn("Save API failed", e);
        }
      }

      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = downloadName;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
    } catch (err: any) {
      alert("プロジェクトの保存に失敗しました: " + err.message);
    }
  };

  const reloadUnavailableTitle = "この開き方/ブラウザでは自動再読み込みを使えません（Chrome/Edge でツールバーから開くかドラッグ＆ドロップしてください）";

  return (
    <div className={cn("bg-slate-800 text-white px-2.5 py-2.5 flex items-center shadow-md relative z-[100]", (fileName || comparison.data) && "flex-wrap gap-y-2")}>
      <button 
        onClick={() => setIsSidebarOpen(!isSidebarOpen)}
        className="w-8 h-8 rounded hover:bg-slate-700 flex items-center justify-center mr-2.5 transition-colors flex-shrink-0 text-gray-300 hover:text-white"
        title="サイドバーの表示/非表示"
      >
        <Menu size={20} />
      </button>

      <div className="flex items-center bg-white/10 rounded border border-white/20 flex-shrink-0">
        <div className="relative open-dropdown-area">
          <button onClick={() => setIsOpenMenuOpen(!isOpenMenuOpen)} className="flex items-center gap-1.5 px-2.5 py-1 text-xs text-gray-200 hover:bg-slate-700">
            <FolderOpen size={14} />開く<ChevronDown size={12} />
          </button>
          {isOpenMenuOpen && <div className="absolute top-full left-0 mt-2 w-64 bg-slate-800 border border-slate-600 rounded shadow-lg z-50 py-1 text-xs">
            <button className="block w-full text-left px-3 py-2 hover:bg-slate-700" onClick={() => { setIsOpenMenuOpen(false); void handleLoadSettings(); }}>波形・設定・プロジェクトを開く</button>
            <button disabled={!fileName} className="block w-full text-left px-3 py-2 hover:bg-slate-700 disabled:opacity-40" onClick={() => { setIsOpenMenuOpen(false); void openCompareFile(); }}>比較する CSV（A）を開く</button>
          </div>}
          <input ref={compareInputRef} type="file" accept=".csv,.txt" className="hidden" onChange={async e => {
            const input = e.currentTarget, file = input.files?.[0];
            try { if (file) comparison.load(await file.text(), file.name); }
            catch { alert("比較するCSVの読み込みに失敗しました。"); }
            finally { input.value = ""; }
          }} />
        </div>
        <input
          type="file"
          ref={fileInputRef}
          accept=".csv,.txt,.yaml,.yml,.wcp"
          multiple
          className="hidden"
          onChange={(e) => handleFileChange(e)}
        />
        
        <div className="h-4 w-px bg-white/20 mx-1"></div>
        
        <div className="relative group save-dropdown-area">
          <button
            onClick={() => setIsSaveMenuOpen(!isSaveMenuOpen)}
            className="flex items-center gap-1.5 px-2.5 py-1 text-xs bg-transparent text-gray-200 hover:bg-slate-700 hover:text-white transition-colors"
          >
            <Save size={14} />
            保存
            <ChevronDown size={12} className="ml-0.5" />
          </button>
          
          {isSaveMenuOpen && (
            <div className="absolute top-full left-0 mt-2 w-64 bg-slate-800 border border-slate-600 rounded shadow-lg z-50 py-1 text-sm">
              <div 
                className="px-3 py-2 border-b border-slate-700 hover:bg-slate-700 cursor-pointer flex flex-col group/item transition-colors"
                onClick={exportProject}
              >
                <div className="flex items-center justify-between text-blue-300">
                  <span className="font-bold">プロジェクトとして保存 (.wcp)</span>
                  <Archive size={14} />
                </div>
                <span className="text-[10px] text-gray-400 mt-1 leading-tight">波形データ、表示色などの設定、Zoom・マーカー位置を含めて保存</span>
              </div>
              <div 
                className="px-3 py-2 hover:bg-slate-700 cursor-pointer flex flex-col group/item transition-colors"
                onClick={exportSettings}
              >
                <div className="flex items-center justify-between text-gray-200 group-hover/item:text-white">
                  <span className="font-medium">設定ファイルのみ保存 (.yaml)</span>
                  <FileText size={14} className="text-gray-400" />
                </div>
                <span className="text-[10px] text-gray-400 mt-1 leading-tight">表示色やグループ分けなどの設定のみを保存（Zoom・マーカー位置は含まず）</span>
              </div>
            </div>
          )}
        </div>
      </div>

      <div className="flex items-center bg-white/10 rounded border border-white/20 ml-2 flex-shrink-0">
        <button
          onClick={() => setAutoReload(!autoReload)}
          disabled={!waveformHandle}
          aria-pressed={autoReload}
          title={waveformHandle ? (comparison.keepPrevious ? "B のCSVの変更を確認し、テスト再実行時は前回を A に残します" : comparison.data ? "B のCSVの変更を約1.5秒ごとに確認します（A は保持します）" : "波形CSVの変更を約1.5秒ごとに確認します") : reloadUnavailableTitle}
          className={cn(
            "flex items-center gap-1.5 px-2.5 py-1 text-xs transition-colors border-r border-white/20 disabled:opacity-40 disabled:cursor-not-allowed",
            autoReload ? "bg-blue-500 text-white font-bold" : "text-gray-200 hover:bg-slate-700 hover:text-white",
          )}
        >
          <RefreshCw size={14} />
          自動再読み込み
        </button>
        <button
          onClick={() => comparison.setKeepPrevious(!comparison.keepPrevious)}
          disabled={!waveformHandle}
          aria-pressed={comparison.keepPrevious}
          title="テストを流し直したとき、前回の結果を A にして比べます"
          className={cn("flex items-center gap-1.5 px-2.5 py-1 text-xs border-r border-white/20 disabled:opacity-40 disabled:cursor-not-allowed", comparison.keepPrevious ? "bg-blue-500 text-white" : "text-gray-200 hover:bg-slate-700")}
        ><Pin size={14} />前回を A に残す</button>
        <button
          onClick={() => void reload()}
          disabled={!waveformHandle}
          title={waveformHandle ? (comparison.keepPrevious ? "B のCSVを再読み込みし、テスト再実行時は前回を A に残します" : comparison.data ? "B のCSVだけを再読み込みします（A は保持します）" : "波形CSVを再読み込みします") : reloadUnavailableTitle}
          className="flex items-center gap-1.5 px-2.5 py-1 text-xs text-gray-200 hover:bg-slate-700 hover:text-white transition-colors disabled:opacity-40 disabled:cursor-not-allowed"
        >
          <RefreshCw size={14} />
          再読み込み
        </button>
      </div>

      {fileName ? (
        <div className={cn("flex items-center mx-2 px-3 py-1 bg-slate-900/60 rounded-full border border-slate-700 flex-1 overflow-hidden", comparison.data ? "min-w-[480px]" : "min-w-[240px]")}>
          {!comparison.data && <button
            onClick={() => void openCompareFile()}
            title="クリックで選ぶか、ここに CSV をドロップ"
            onDragEnter={e => { e.preventDefault(); e.stopPropagation(); }}
            onDragLeave={e => { e.preventDefault(); e.stopPropagation(); }}
            onDragOver={e => { e.preventDefault(); e.stopPropagation(); e.dataTransfer.dropEffect = "copy"; }}
            onDrop={e => { e.preventDefault(); e.stopPropagation(); void loadDroppedFiles(e.dataTransfer, true); }}
            className="border border-dashed border-slate-500 rounded px-2 py-1 mr-2 text-xs text-slate-300 shrink-0 hover:bg-slate-700"
          >＋ A を開く</button>}
          {comparison.data && <span className={cn("flex items-center gap-1 mr-2 min-w-[100px] flex-1 text-xs", !comparison.enabled && "opacity-40")}>
            <span className="shrink-0">A</span><span className="truncate min-w-[6ch] max-w-48 flex-1" title={comparison.name || ""}>{comparison.name}</span>
            <button aria-label="A を閉じる" onClick={comparison.close} className="px-1 hover:bg-slate-600 rounded shrink-0">✕</button><button aria-label="A と B を入れ替える" title="A と B を入れ替える" onClick={comparison.swap} className="ml-2 p-0.5 hover:bg-slate-600 rounded shrink-0 text-white"><ArrowLeftRight size={16} strokeWidth={2.2} /></button>
          </span>}
          <FileSpreadsheet size={14} className="text-green-400 mr-2 flex-shrink-0" />
          <span className="text-xs text-slate-300 truncate mr-3 min-w-[8ch] flex-1" title={fileName}>
            {comparison.data ? `B ${fileName}` : fileName}
          </span>
          <div className="w-px h-3 bg-slate-600 mr-3 flex-shrink-0"></div>
          <span title={hoveredSignal || undefined} className="text-xs font-mono text-cyan-400 truncate flex-1 text-left opacity-90 transition-opacity min-h-[16px] min-w-0">
            {hoveredSignal ? displaySignalName(hoveredSignal, Object.keys(useWaveformStore.getState().signalSettings)) : ""}
          </span>
        </div>
      ) : (
        <div className="mx-2 flex-1"></div>
      )}

      {comparison.data && <div className="flex flex-wrap items-center gap-2 text-xs order-last">
        <button aria-pressed={comparison.enabled} className={cn("px-2 py-1 rounded", comparison.enabled ? "bg-blue-500 font-bold" : "bg-slate-700")} onClick={() => comparison.toggle(!comparison.enabled)}>比較</button>
        {comparison.enabled && <>
          <button aria-label="前の差分" disabled={!visibleDiffs.length} onClick={() => goDiff(-1)} className="px-2 py-1 hover:bg-slate-700 disabled:opacity-40">◀</button>
          <span className="tabular-nums">差分 {comparison.focus >= 0 && comparison.focus < visibleDiffs.length ? comparison.focus + 1 : "–"} / {visibleDiffs.length}</span>
          <button aria-label="次の差分" disabled={!visibleDiffs.length} onClick={() => goDiff(1)} className="px-2 py-1 hover:bg-slate-700 disabled:opacity-40">▶</button>
          <button className="px-2 py-1 rounded border border-slate-500 hover:bg-slate-700" onClick={() => { comparison.setFocus(-1); comparison.setZoom({ start: 0, end: 100, startValue: undefined, endValue: undefined }); }}>全体</button>
          <label>差分: <select className="bg-slate-700 rounded px-2 py-1" value={comparison.filter} onChange={e => comparison.setFilter(e.target.value as "all" | "value")}><option value="all">すべて</option><option value="value">値の違いだけ</option></select></label>
          <label className="flex items-center gap-1"><input type="checkbox" checked={comparison.onlyDiff} onChange={e => comparison.setOnlyDiff(e.target.checked)} />差分のある信号だけ</label>
        </>}
          <button
            onClick={() => { comparison.promote(); }}
            title="変更を繰り返すときに、今の B の結果を次の比較の A にします"
            className="text-xs px-2 py-1 mr-3 rounded border border-slate-500 hover:bg-slate-700 whitespace-nowrap"
          >B を A にする</button>
      </div>}

      <div className={cn("flex gap-2 items-center flex-shrink-0", comparison.data && "mr-2")}>
        <div className="flex bg-white/10 rounded overflow-hidden border border-white/20">
          <button
            className={cn(
              "px-2.5 py-1 text-xs transition-colors border-r border-white/20 whitespace-nowrap",
              showMode !== "off"
                ? "bg-blue-500 text-white font-bold"
                : "text-gray-300 hover:bg-white/20 hover:text-white",
            )}
            onClick={toggleShowMode}
            title="状態表示の切り替え"
          >
            状態: {showMode === "off" ? "OFF" : showMode === "bg-only" ? "背景" : "ON"}
          </button>
          <button
            className={cn(
              "px-2.5 py-1 text-xs transition-colors border-r border-white/20 whitespace-nowrap",
              showSignalNames
                ? "bg-blue-500 text-white font-bold"
                : "text-gray-300 hover:bg-white/20 hover:text-white",
            )}
            onClick={cycleSignalDisplay}
            title="信号名・別名表示の切り替え"
          >
            信号名: {!showSignalNames ? "OFF" : showAlias ? "別名" : "ON"}
          </button>
          <button
            className={cn(
              "px-2.5 py-1 text-xs transition-colors border-r border-white/20",
              showMarkers
                ? "bg-blue-500 text-white font-bold"
                : "text-gray-300 hover:bg-white/20 hover:text-white",
            )}
            onClick={toggleShowMarkers}
            title="マーカーのON/OFF"
          >
            マーカー
          </button>
          <button
            className={cn(
              "px-2.5 py-1 text-xs transition-colors border-r border-white/20 whitespace-nowrap",
              showCursorLabels !== "off"
                ? "bg-blue-500 text-white font-bold"
                : "text-gray-300 hover:bg-white/20 hover:text-white",
            )}
            onClick={toggleShowCursorLabels}
            title="カーソルのラベル表示の切り替え"
          >
            ラベル: {showCursorLabels === "off" ? "OFF" : showCursorLabels === "value-only" ? "値のみ" : "ON"}
          </button>
          <button
            className={cn(
              "px-2.5 py-1 text-xs transition-colors border-r border-white/20 whitespace-nowrap",
              valueDisplayMode === "raw"
                ? "bg-amber-500 text-white font-bold"
                : "bg-blue-500 text-white font-bold",
            )}
            onClick={toggleValueDisplayMode}
            title="表示値を物理値と生値で切り替えます"
          >
            値: {valueDisplayMode === "physical" ? "物理" : "生値"}
          </button>
          <button
            className={cn(
              "px-2.5 py-1 text-xs transition-colors",
              layoutMode !== "uniform"
                ? "bg-amber-500 text-white font-bold"
                : "text-gray-300 hover:bg-white/20 hover:text-white",
            )}
            onClick={cycleLayoutMode}
            title="レーン幅の調整モードを切り替えます"
          >
            自動調整 ({layoutMode === "uniform" ? "均一" : layoutMode === "optimized" ? "最適化" : "手動"})
          </button>
        </div>

        <div className="w-px h-5 bg-gray-600"></div>

        <button
          onClick={toggleTableDisplay}
          className={cn(
            "flex items-center gap-1.5 px-3 py-1 text-sm rounded font-bold border transition-colors",
            showTable
              ? "bg-blue-500 text-white border-blue-500"
              : "bg-white text-blue-500 border-blue-500 hover:bg-blue-50",
          )}
        >
          <Table size={16} />
          計測表
        </button>

        <div className="flex items-center gap-1.5 bg-white/10 px-2 py-1 rounded">
          <span className="text-[11px] text-gray-300">位置:</span>
          <label className="text-[11px] cursor-pointer text-white flex items-center m-0">
            <input
              type="radio"
              name="tablePos"
              value="bottom"
              checked={tablePosition === "bottom"}
              onChange={() => setTablePosition("bottom")}
              className="mr-1"
            />
            下
          </label>
          <label className="text-[11px] cursor-pointer text-white flex items-center m-0">
            <input
              type="radio"
              name="tablePos"
              value="side"
              checked={tablePosition === "side"}
              onChange={() => setTablePosition("side")}
              className="mr-1"
            />
            左
          </label>
        </div>

        <div className="w-px h-5 bg-gray-600"></div>

        <button
          onClick={toggleTheme}
          title="テーマ切り替え"
          className="flex items-center justify-center py-1.5 px-1 text-gray-400 hover:text-white rounded hover:bg-white/20 transition-colors"
        >
          {theme === "dark" ? <Sun size={16} /> : <Moon size={16} />}
        </button>
      </div>
    </div>
  );
}
