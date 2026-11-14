import { useCompareView } from "../../../hooks/useCompareView";
import { CompareBadge } from "./CompareStatus";
import React, { useMemo } from "react";
import { useWaveformStore } from "../../../store/useWaveformStore";
import { useShallow } from "zustand/react/shallow";
import { calculateAutoYRange, detectUndefinedValue, getConversion, toPhysical, computeAutoBreak, cn } from "../../../utils/helpers";
import { Settings, ChevronDown, ChevronRight, Layers, Rows, Link } from "lucide-react";
import { useSortable } from "@dnd-kit/sortable";
import { useDroppable } from "@dnd-kit/core";
import { SortableContext, verticalListSortingStrategy } from "@dnd-kit/sortable";
import { CSS } from "@dnd-kit/utilities";
import { GroupSettings } from "./GroupSettings";
import { SignalSettings } from "./SignalSettings";

export function SignalItemOverlay({ sigName }: { sigName: string }) {
  const {
       signalSettings, showAlias 
    } = useWaveformStore(useShallow((state) => ({
      signalSettings: state.signalSettings,
      showAlias: state.showAlias
    })));
  const setting = signalSettings[sigName];
  if (!setting) return null;
  const displayName = showAlias ? setting.alias || sigName : sigName;

  return (
    <div className="border border-gray-200 dark:border-slate-700 bg-white dark:bg-slate-800 shadow-xl opacity-90 ring-2 ring-blue-500 transition-colors" style={{ listStyle: "none" }}>
      <div className="flex items-center pr-1 pl-1.5 py-0.5 text-xs border-l-4 border-transparent cursor-grabbing">
        <span className="w-3 mr-1 shrink-0"></span>
        <input type="checkbox" checked={setting.checked} readOnly className="mr-1 cursor-pointer" />
        <span className="flex-1 whitespace-nowrap overflow-hidden text-ellipsis font-semibold px-1 rounded" style={{ color: setting.color }}>
          {displayName}
        </span>
        <button className="text-gray-400 ml-auto p-1 pointer-events-none"><Settings size={14} /></button>
      </div>
    </div>
  );
}

export function GroupItemOverlay({ id }: { id: string }) {
  const {
       signalGroups 
    } = useWaveformStore(useShallow((state) => ({
      signalGroups: state.signalGroups
    })));
  const group = signalGroups.find((g) => g.id === id);
  if (!group) return null;

  return (
    <div className="border border-gray-300 dark:border-slate-600 bg-[#e8ecef] dark:bg-slate-700 shadow-xl opacity-90 ring-2 ring-blue-500 transition-colors" style={{ listStyle: "none" }}>
      <div className="flex items-center px-2.5 py-1.5 text-xs font-bold border-b border-gray-300 dark:border-slate-600 cursor-grabbing dark:text-gray-200">
        <span className="mr-1 w-3 text-center text-[10px]">
          {group.expanded ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
        </span>
        <input type="checkbox" checked={group.visible} readOnly className="mr-1 pointer-events-none" />
        <span className="flex-1 ml-1 truncate">{group.name}</span>
        <div className="flex gap-1.5 items-center pointer-events-none">
          <div className="flex shadow-sm rounded">
            <button className={cn("p-1 text-[10px] flex items-center justify-center border rounded-l transition-colors", group.layout === "overlap" ? "bg-blue-500 text-white border-blue-500 z-10" : "bg-white dark:bg-slate-800 text-gray-500 dark:text-gray-400 border-gray-300 dark:border-slate-600 z-0")} disabled><Layers size={14} /></button>
            <button className={cn("p-1 text-[10px] flex items-center justify-center border -ml-px rounded-r transition-colors", group.layout === "lane" ? "bg-blue-500 text-white border-blue-500 z-10" : "bg-white dark:bg-slate-800 text-gray-500 dark:text-gray-400 border-gray-300 dark:border-slate-600 z-0")} disabled><Rows size={14} /></button>
          </div>
          <button className={cn("p-1 bg-white dark:bg-slate-800 border border-gray-300 dark:border-slate-600 rounded flex items-center justify-center text-gray-600 dark:text-gray-400 shadow-sm transition-colors", group.syncRange && "bg-blue-500 text-white border-blue-500")} disabled title="同一レンジ（Y軸同期）"><Link size={14} /></button>
        </div>
      </div>
    </div>
  );
}

export function SortableSignalItem({ id, sigName }: { id: string; sigName: string; key?: React.Key; }) {
  const { attributes, listeners, setNodeRef, transform, transition, isDragging } = useSortable({ id, data: { type: "Signal" } });
  const style = { transform: CSS.Transform.toString(transform), transition, opacity: isDragging ? 0.3 : 1 };

  const { signalSettings, updateSignalSetting, toggleSignalCheck, hoveredSignal, setHoveredSignal, chartData, lsbMapping, offsetMapping, undefinedMapping, signalGroups, unassignedSignals, unassignedGroupConfig, showAlias } = useWaveformStore(useShallow((state) => ({
      signalSettings: state.signalSettings,
      updateSignalSetting: state.updateSignalSetting,
      toggleSignalCheck: state.toggleSignalCheck,
      hoveredSignal: state.hoveredSignal,
      setHoveredSignal: state.setHoveredSignal,
      chartData: state.chartData,
      lsbMapping: state.lsbMapping,
      offsetMapping: state.offsetMapping,
      undefinedMapping: state.undefinedMapping,
      signalGroups: state.signalGroups,
      unassignedSignals: state.unassignedSignals,
      unassignedGroupConfig: state.unassignedGroupConfig,
      showAlias: state.showAlias
    })));
  const setting = signalSettings[sigName];
  if (!setting) return null;

  const isHovered = hoveredSignal === sigName;
  const displayName = showAlias ? setting.alias || sigName : sigName;

  const autoRange = useMemo(() => {
    if (!chartData || !chartData.seriesMap[sigName]) return { min: 0, max: 1 };
    const undefinedVals = detectUndefinedValue(chartData.seriesMap[sigName], undefinedMapping[sigName]);
    return calculateAutoYRange(sigName, chartData.seriesMap[sigName], lsbMapping[sigName], offsetMapping[sigName], undefinedVals || [], setting.yBreak !== false);
  }, [chartData, sigName, lsbMapping, offsetMapping, undefinedMapping, setting.yBreak]);

  const groupAutoRange = useMemo(() => {
    let targetSignals: string[] = [];
    const group = signalGroups.find((g) => g.signals.includes(sigName));

    if (group && group.syncRange) {
      targetSignals = group.signals.filter((s) => signalSettings[s]?.checked);
    } else if (unassignedSignals.includes(sigName) && unassignedGroupConfig.syncRange) {
      targetSignals = unassignedSignals.filter((s) => signalSettings[s]?.checked);
    } else {
      return autoRange;
    }

    if (targetSignals.length === 0) return autoRange;

    let gMin = Infinity;
    let gMax = -Infinity;
    let allUndefinedPhys: number[] = [];

    targetSignals.forEach((s) => {
      if (!chartData || !chartData.seriesMap[s]) return;
      const undefinedVals = detectUndefinedValue(chartData.seriesMap[s], undefinedMapping[s]);
      const conv = getConversion(s, lsbMapping, offsetMapping);
      const r = calculateAutoYRange(s, chartData.seriesMap[s], lsbMapping[s], offsetMapping[s], undefinedVals || [], false);
      if (r.min < gMin) gMin = r.min;
      if (r.max > gMax) gMax = r.max;
      if (undefinedVals) allUndefinedPhys.push(...undefinedVals.map((v) => toPhysical(v, conv)));
    });

    if (gMin === Infinity) return autoRange;

    const leaderSetting = signalSettings[targetSignals[0]] || setting;
    const isYBreakActive = leaderSetting.yBreak !== false;
    let breakStart: number | undefined = undefined;
    let breakEnd: number | undefined = undefined;
    let finalMin = gMin;
    let finalMax = gMax;

    if (allUndefinedPhys.length > 0 && isYBreakActive) {
      const b = computeAutoBreak(gMin, gMax, allUndefinedPhys);
      if (b) {
        breakStart = b.start;
        breakEnd = b.end;
        let undefMin = Math.min(...allUndefinedPhys);
        let undefMax = Math.max(...allUndefinedPhys);
        const diffNormal = Math.max(gMax - gMin, 1);
        const pad = diffNormal * 0.05;
        if (undefMax > gMax) finalMax = undefMax + pad;
        if (undefMin < gMin) finalMin = undefMin - pad;
      }
    }

    return { min: finalMin, max: finalMax, breakStart, breakEnd };
  }, [autoRange, chartData, lsbMapping, offsetMapping, undefinedMapping, signalGroups, unassignedSignals, unassignedGroupConfig, sigName, signalSettings, setting]);

  return (
    <li ref={setNodeRef} style={style} className={cn("border-b border-gray-100 dark:border-slate-700 bg-white dark:bg-slate-800 transition-colors", isHovered ? "bg-blue-50/80 dark:bg-slate-700" : "")} onMouseEnter={() => setHoveredSignal(sigName)} onMouseLeave={() => setHoveredSignal(null)}>
      <div {...attributes} {...listeners} className={cn("flex items-center pr-1 pl-1.5 py-0.5 text-xs border-l-4 transition-colors cursor-grab active:cursor-grabbing touch-none", "border-transparent hover:border-blue-500", isHovered ? "border-blue-500" : "")}>
        <span className="w-3 mr-1 shrink-0"></span>
        <input type="checkbox" checked={setting.checked} onChange={() => toggleSignalCheck(sigName)} onPointerDown={(e) => e.stopPropagation()} className="mr-1 cursor-pointer" />
        <span className="flex-1 whitespace-nowrap overflow-hidden text-ellipsis cursor-pointer font-semibold px-1 rounded transition-opacity hover:opacity-70 hover:underline" style={{ color: setting.color }} title={displayName} onClick={() => toggleSignalCheck(sigName)}>
          {displayName}
        </span>
        <CompareBadge signal={sigName} />
        <button className="text-gray-400 hover:text-blue-500 transition-colors ml-auto p-1" onClick={(e) => { e.stopPropagation(); updateSignalSetting(sigName, { expanded: !setting.expanded }); }} onPointerDown={(e) => e.stopPropagation()} title="設定">
          <Settings size={14} className="pointer-events-none" />
        </button>
      </div>

      {setting.expanded && (
        <SignalSettings sigName={sigName} setting={setting} groupAutoRange={groupAutoRange} />
      )}
    </li>
  );
}

export function GroupDroppableList({ id, signals }: { id: string; signals: string[]; }) {
  const view = useCompareView();
  const aOnly = new Set(view.aOnly);
  const containerId = `${id}-container`;
  const {
       setNodeRef } = useDroppable({ id: containerId, data: { type: "Container", groupId: id } });
  return (
    <ul ref={setNodeRef} className="list-none p-0 m-0 min-h-[20px] bg-gray-50 dark:bg-slate-900 pb-2 transition-colors">
      <SortableContext items={signals.filter(sig => !aOnly.has(sig))} strategy={verticalListSortingStrategy}>
        {signals.map((sig) => (
          aOnly.has(sig) ? <li key={sig} className="flex items-center gap-2 px-2.5 py-1 text-xs">
            <span className="truncate" style={{ color: view.settings[sig].color }}>{sig}</span>
            <CompareBadge signal={sig} />
          </li> : <SortableSignalItem key={sig} id={sig} sigName={sig} />
        ))}
      </SortableContext>
    </ul>
  );
}

export const SortableGroupItem: React.FC<{ group: any; onRename: (id: string, name: string) => void; }> = ({ group, onRename }) => {
  const { attributes, listeners, setNodeRef, transform, transition, isDragging } = useSortable({ id: group.id, data: { type: "Group" } });
  const { toggleGroupExpand, toggleGroupVisibility } = useWaveformStore(useShallow((state) => ({
      toggleGroupExpand: state.toggleGroupExpand,
      toggleGroupVisibility: state.toggleGroupVisibility
  })));
  const style = { transform: CSS.Transform.toString(transform), transition, opacity: isDragging ? 0.3 : 1, zIndex: isDragging ? 999 : 1, position: "relative" as const };

  return (
    <div className="mb-2" ref={setNodeRef} style={style}>
      <div {...attributes} {...listeners} className="flex items-center bg-[#e8ecef] dark:bg-slate-700 px-2.5 py-1.5 text-xs font-bold border-b border-gray-300 dark:border-slate-600 dark:text-gray-200 hover:bg-[#dfe4e8] dark:hover:bg-slate-600 select-none cursor-grab active:cursor-grabbing touch-none transition-colors" onClick={() => toggleGroupVisibility(group.id, !group.visible)}>
        <span className="mr-1 w-3 text-center text-[10px] cursor-pointer" onClick={(e) => { e.stopPropagation(); toggleGroupExpand(group.id); }} onPointerDown={(e) => e.stopPropagation()}>
          {group.expanded ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
        </span>
        <input type="checkbox" checked={group.visible} onChange={(e) => toggleGroupVisibility(group.id, !group.visible)} onClick={(e) => e.stopPropagation()} onPointerDown={(e) => e.stopPropagation()} className="mr-1 cursor-pointer" />
        <span className="flex-1 ml-1 truncate" onDoubleClick={(e) => { e.stopPropagation(); onRename(group.id, group.name); }}>{group.name}</span>
        <GroupSettings group={group} />
      </div>
      {group.expanded && <GroupDroppableList id={group.id} signals={group.signals} />}
    </div>
  );
};
