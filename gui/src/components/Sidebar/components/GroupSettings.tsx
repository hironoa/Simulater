import React from "react";
import { useWaveformStore } from "../../../store/useWaveformStore";
import { useShallow } from "zustand/react/shallow";
import { Layers, Rows, Link } from "lucide-react";
import { cn } from "../../../utils/helpers";

export function GroupSettings({ group }: any) {
  const {
       setGroupLayout, toggleGroupSync 
    } = useWaveformStore(useShallow((state) => ({
      setGroupLayout: state.setGroupLayout,
      toggleGroupSync: state.toggleGroupSync
    })));
  return (
    <div
      className="flex gap-1.5 items-center"
      onClick={(e) => e.stopPropagation()}
      onPointerDown={(e) => e.stopPropagation()}
    >
      <div className="flex shadow-sm rounded">
        <button
          className={cn(
            "p-1 text-[10px] flex items-center justify-center border rounded-l transition-colors",
            group.layout === "overlap"
              ? "bg-blue-500 text-white border-blue-500 z-10"
              : "bg-white dark:bg-slate-700 text-gray-600 dark:text-gray-300 border-gray-300 dark:border-slate-600 hover:bg-gray-50 dark:hover:bg-slate-600 z-0",
          )}
          onClick={() => setGroupLayout(group.id, "overlap")}
          title="重ね表示"
        >
          <Layers size={14} />
        </button>
        <button
          className={cn(
            "p-1 text-[10px] flex items-center justify-center border -ml-px rounded-r transition-colors",
            group.layout === "lane"
              ? "bg-blue-500 text-white border-blue-500 z-10"
              : "bg-white dark:bg-slate-700 text-gray-600 dark:text-gray-300 border-gray-300 dark:border-slate-600 hover:bg-gray-50 dark:hover:bg-slate-600 z-0",
          )}
          onClick={() => setGroupLayout(group.id, "lane")}
          title="レーン表示"
        >
          <Rows size={14} />
        </button>
      </div>
      <button
        className={cn(
          "p-1 bg-white dark:bg-slate-700 border border-gray-300 dark:border-slate-600 rounded flex items-center justify-center text-gray-600 dark:text-gray-300 shadow-sm transition-colors hover:bg-gray-50 dark:hover:bg-slate-600",
          group.syncRange &&
            "bg-blue-500 text-white border-blue-500 hover:bg-blue-600",
        )}
        onClick={() => toggleGroupSync(group.id)}
        title="同一レンジ（Y軸同期）"
      >
        <Link size={14} />
      </button>
    </div>
  );
}
