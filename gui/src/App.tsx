import React, { useEffect, useRef, useState } from "react";
import { Toaster } from "react-hot-toast";
import { Toolbar } from "./components/Toolbar";
import { Sidebar } from "./components/Sidebar/Sidebar";
import { Chart } from "./components/Chart";
import { MeasureTable } from "./components/MeasureTable";
import { useWaveformStore } from "./store/useWaveformStore";
import { useShallow } from "zustand/react/shallow";
import { loadDroppedFiles } from "./utils/loadDroppedFiles";
import { cn } from "./utils/helpers";
import { useLiveStore } from "./live/liveStore";
import { liveController } from "./live/controller";
import { LiveBar } from "./components/live/LiveBar";
import { LiveChart } from "./components/live/LiveChart";
import { LivePanel } from "./components/live/LivePanel";

let liveStarted = false;

export default function App() {
  const {
      
    tablePosition,
    showTable,
    activeSidebarTab,
    setActiveSidebarTab,
    tableContentWidth,
    isSidebarOpen,
  
    } = useWaveformStore(useShallow((state) => ({
      tablePosition: state.tablePosition,
      showTable: state.showTable,
      activeSidebarTab: state.activeSidebarTab,
      setActiveSidebarTab: state.setActiveSidebarTab,
      tableContentWidth: state.tableContentWidth,
      isSidebarOpen: state.isSidebarOpen,
    })));

  // 同一オリジンに sim gui がいればライブモード、いなければ従来の Viewer のまま
  const liveMode = useLiveStore((s) => s.mode);
  const liveRunning = useLiveStore((s) => s.sim.running);
  useEffect(() => {
    if (liveStarted) return;
    liveStarted = true;
    void liveController().start();
  }, []);
  const isLive = liveMode === "live";
  const showLiveChart = isLive && liveRunning;

  const sidebarRef = useRef<HTMLDivElement>(null);
  const tableRef = useRef<HTMLDivElement>(null);
  const wrapperRef = useRef<HTMLDivElement>(null);
  const vResizerRef = useRef<HTMLDivElement>(null);
  const hResizerRef = useRef<HTMLDivElement>(null);

  const isSidebarOpenRef = useRef(isSidebarOpen);
  const lastSidebarWidth = useRef(340);

  useEffect(() => {
    isSidebarOpenRef.current = isSidebarOpen;
    if (sidebarRef.current) {
      if (!isSidebarOpen) {
        lastSidebarWidth.current = sidebarRef.current.getBoundingClientRect().width || 340;
        sidebarRef.current.style.width = '0px';
      } else {
        sidebarRef.current.style.width = `${lastSidebarWidth.current}px`;
      }
    }
    
    window.dispatchEvent(new Event("resize"));
  }, [isSidebarOpen]);

  const hasWaveform = useWaveformStore(state => !!state.chartData);
  const [dropSide, setDropSide] = useState<"a" | "b">("b");
  const [isDragOver, setIsDragOver] = useState(false);
  const dragCounter = useRef(0);

  // Drag and Drop Logic
  const handleDragEnter = (e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    dragCounter.current += 1;
    setIsDragOver(true);
  };

  const handleDragLeave = (e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    dragCounter.current -= 1;
    if (dragCounter.current === 0) {
      setIsDragOver(false);
    }
  };

  const handleDragOver = (e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    const rect = e.currentTarget.getBoundingClientRect();
    setDropSide(e.clientX >= rect.left + rect.width / 2 ? "a" : "b");
  };

  const handleDrop = (e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    dragCounter.current = 0;
    setIsDragOver(false);
    const rect = e.currentTarget.getBoundingClientRect();
    void loadDroppedFiles(e.dataTransfer, hasWaveform && e.clientX >= rect.left + rect.width / 2);
  };

  // Resize Logic
  const isResizingV = useRef(false);
  const isResizingH = useRef(false);

  const onMouseDownV = (e: React.MouseEvent) => {
    if (!isSidebarOpenRef.current) return;
    isResizingV.current = true;
    document.body.style.cursor = "col-resize";
    vResizerRef.current?.classList.add("bg-blue-500");
    e.preventDefault();
  };

  const onMouseDownH = (e: React.MouseEvent) => {
    isResizingH.current = true;
    document.body.style.cursor = "ns-resize";
    hResizerRef.current?.classList.add("bg-blue-500");
    e.preventDefault();
  };

  useEffect(() => {
    const onMouseMove = (e: MouseEvent) => {
      if (isResizingV.current && sidebarRef.current && isSidebarOpenRef.current) {
        let newWidth = e.clientX;
        if (newWidth < 150) newWidth = 150;
        if (newWidth > window.innerWidth - 200)
          newWidth = window.innerWidth - 200;
        sidebarRef.current.style.width = `${newWidth}px`;
        // Trigger resize event for echarts
        window.dispatchEvent(new Event("resize"));
      }

      if (isResizingH.current && tableRef.current && wrapperRef.current) {
        const wrapperRect = wrapperRef.current.getBoundingClientRect();
        let newTableHeight = wrapperRect.bottom - e.clientY;
        if (newTableHeight < 50) newTableHeight = 50;
        if (newTableHeight > wrapperRect.height - 100)
          newTableHeight = wrapperRect.height - 100;
        tableRef.current.style.height = `${newTableHeight}px`;
        window.dispatchEvent(new Event("resize"));
      }
    };

    const onMouseUp = () => {
      if (isResizingV.current) {
        isResizingV.current = false;
        document.body.style.cursor = "default";
        vResizerRef.current?.classList.remove("bg-blue-500");
      }
      if (isResizingH.current) {
        isResizingH.current = false;
        document.body.style.cursor = "default";
        hResizerRef.current?.classList.remove("bg-blue-500");
      }
    };

    document.addEventListener("mousemove", onMouseMove);
    document.addEventListener("mouseup", onMouseUp);

    return () => {
      document.removeEventListener("mousemove", onMouseMove);
      document.removeEventListener("mouseup", onMouseUp);
    };
  }, []);

  return (
    <div
      className="flex flex-col h-screen w-screen bg-slate-50 dark:bg-slate-900 dark:text-gray-200 font-sans overflow-hidden transition-colors"
      onDragEnter={handleDragEnter}
      onDragLeave={handleDragLeave}
      onDragOver={handleDragOver}
      onDropCapture={() => { dragCounter.current = 0; setIsDragOver(false); }}
      onDrop={handleDrop}
    >
      <Toaster position="bottom-right" />
      {isDragOver && (
        <div className="fixed inset-0 bg-blue-500/85 text-white flex items-center justify-center text-3xl font-bold z-[9999] pointer-events-none">
          {hasWaveform ? <div className="flex w-full h-full gap-6 p-10 items-stretch">
            {(["b", "a"] as const).map(side => <div key={side} className={cn("flex-1 flex items-center justify-center border-4 border-dashed rounded-2xl text-center", dropSide === side ? "border-white bg-white/20" : "border-white/40 bg-black/20")}>
              {side === "b" ? "B として開く（今までどおり）" : "A（比較元）として開く"}
            </div>)}
          </div> : <div className="border-4 border-dashed border-white p-10 md:p-16 rounded-2xl bg-black/20 text-center">
            📁 波形CSVまたは信号定義ファイル(CSV/YAML)をドロップ
          </div>}
        </div>
      )}

      <div id="export-measure-table-container-transposed" className="fixed top-[-9999px] left-[-9999px] w-fit h-[1000px] bg-white dark:bg-slate-800 z-[-1] overflow-visible">
        <MeasureTable id="export-measure-table-transposed" forceTransposed={true} />
      </div>
      <div id="export-measure-table-container-normal" className="fixed top-[-9999px] left-[-9999px] w-fit h-[1000px] bg-white dark:bg-slate-800 z-[-1] overflow-visible">
        <MeasureTable id="export-measure-table-normal" forceTransposed={false} />
      </div>

      <Toolbar />
      {isLive && <LiveBar />}

      <div className="flex flex-1 overflow-hidden" ref={wrapperRef}>
        <div
          ref={sidebarRef}
          className={cn(
            "flex flex-col bg-white dark:bg-slate-800 border-gray-300 dark:border-gray-700 z-10 flex-shrink-0 overflow-hidden",
            isSidebarOpen ? "border-r" : "border-r-0 pointer-events-none"
          )}
          style={{ width: "340px" }}
        >
          {tablePosition === "side" && (
            <div className="flex bg-gray-200 dark:bg-slate-700 border-b border-gray-300 dark:border-gray-600 transition-colors">
              <button
                className={cn(
                  "flex-1 py-2 text-xs font-bold transition-colors",
                  activeSidebarTab === "sig"
                    ? "bg-white dark:bg-slate-800 text-blue-500 border-b-2 border-blue-500"
                    : "text-gray-500 dark:text-gray-400 hover:bg-gray-300 dark:hover:bg-slate-600",
                )}
                onClick={() => setActiveSidebarTab("sig")}
              >
                信号設定
              </button>
              <button
                className={cn(
                  "flex-1 py-2 text-xs font-bold transition-colors",
                  activeSidebarTab === "cursor"
                    ? "bg-white dark:bg-slate-800 text-blue-500 border-b-2 border-blue-500"
                    : "text-gray-500 dark:text-gray-400 hover:bg-gray-300 dark:hover:bg-slate-600",
                )}
                onClick={() => setActiveSidebarTab("cursor")}
              >
                計測表
              </button>
            </div>
          )}

          {tablePosition === "side" ? (
            <div className="flex-1 overflow-hidden flex flex-col relative w-full h-full">
              <div
                className={cn(
                  "absolute inset-0 w-full h-full",
                  activeSidebarTab === "sig" ? "block" : "hidden",
                )}
              >
                <Sidebar />
              </div>
              <div
                className={cn(
                  "absolute inset-0 w-full h-full",
                  activeSidebarTab === "cursor" ? "block" : "hidden",
                )}
              >
                {showTable && <MeasureTable />}
              </div>
            </div>
          ) : (
            <Sidebar />
          )}
        </div>

        <div
          ref={vResizerRef}
          onMouseDown={onMouseDownV}
          className={cn(
            "relative bg-[#eee] dark:bg-gray-700 z-50 transition-colors shrink-0",
            isSidebarOpen ? "hover:bg-blue-500 cursor-col-resize w-1" : "hidden"
          )}
        />

        <div className="flex flex-col flex-1 overflow-hidden bg-white dark:bg-slate-900 relative transition-colors">
          <div className="flex-1 min-h-[100px] relative p-2.5 pb-0 flex flex-col">
            {/* 実行中は軽いライブ描画、一時停止中は全区間を従来の Chart で操作する。どちらも作り直さずに表示だけ切り替える */}
            <div className={cn("flex-1 min-h-0 relative flex flex-col", showLiveChart && "hidden")}>
              <Chart />
            </div>
            {isLive && (
              <div className={cn("flex-1 min-h-0 relative", !showLiveChart && "hidden")}>
                <LiveChart />
              </div>
            )}
          </div>

          {tablePosition === "bottom" && showTable && (
            <>
              <div
                ref={hResizerRef}
                onMouseDown={onMouseDownH}
                className="h-1.5 bg-[#e0e0e0] dark:bg-gray-700 hover:bg-blue-500 cursor-row-resize z-50 transition-colors shrink-0"
              />
              <div
                ref={tableRef}
                className="h-[250px] flex flex-col flex-shrink-0 min-h-[50px] relative dark:bg-slate-900 transition-colors"
              >
                <MeasureTable />
              </div>
            </>
          )}
        </div>
        {isLive && <LivePanel />}
      </div>
    </div>
  );
}
