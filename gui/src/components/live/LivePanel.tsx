import React, { useState } from "react";
import { ChevronDown, ChevronRight, PanelRightClose, PanelRightOpen } from "lucide-react";
import { InputPanel } from "./InputPanel";
import { VariableMonitor } from "./VariableMonitor";
import { cn } from "../../utils/helpers";

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  const [open, setOpen] = useState(true);
  return (
    <div className="border-b border-gray-200 dark:border-slate-700">
      <button className="w-full flex items-center gap-1 px-2 py-1.5 text-xs font-bold text-gray-700 dark:text-gray-200 bg-gray-100 dark:bg-slate-700/60 hover:bg-gray-200 dark:hover:bg-slate-700"
        onClick={() => setOpen(!open)}>
        {open ? <ChevronDown size={13} /> : <ChevronRight size={13} />}{title}
      </button>
      {open && <div className="p-2">{children}</div>}
    </div>
  );
}

/** ライブモードの右パネル（入力パネルと変数モニタ）。 */
export function LivePanel() {
  const [open, setOpen] = useState(true);
  return (
    <div className={cn("flex flex-col flex-shrink-0 bg-white dark:bg-slate-800 border-l border-gray-300 dark:border-gray-700 overflow-hidden transition-[width]",
      open ? "w-[360px]" : "w-7")}>
      <button className="flex items-center justify-center h-7 text-gray-500 hover:text-blue-500 border-b border-gray-200 dark:border-slate-700"
        title={open ? "パネルを閉じる" : "入力・監視パネルを開く"}
        onClick={() => { setOpen(!open); setTimeout(() => window.dispatchEvent(new Event("resize")), 0); }}>
        {open ? <span className="flex items-center gap-1 text-xs"><PanelRightClose size={14} /> 入力・監視</span> : <PanelRightOpen size={14} />}
      </button>
      {open && (
        <div className="flex-1 overflow-auto">
          <Section title="入力パネル"><InputPanel /></Section>
          <Section title="変数モニタ"><VariableMonitor /></Section>
        </div>
      )}
    </div>
  );
}
