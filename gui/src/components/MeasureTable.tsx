import { displaySignalName } from "../utils/displaySignalName";
import { useCompareView } from "../hooks/useCompareView";
import { compareSample } from "../utils/compareView";
import React, { useRef, useState, useLayoutEffect, useMemo } from "react";
import { useWaveformStore } from "../store/useWaveformStore";
import { useShallow } from "zustand/react/shallow";
import { getDisplayConversion, getDisplayUnit, toPhysical, formatPhysical, getDarkerColor, cn } from "../utils/helpers";
import { X } from "lucide-react";
import { computeInactive, inactiveLabel } from "../utils/inactiveRanges";

export function MeasureTable({ id = "measure-table", forceTransposed }: { id?: string, forceTransposed?: boolean }) {
  const {
      
    theme,
    showTable,
    isTableTransposed: isTableTransposedState,
    cursors,
    chartData,
    signalSettings: storedSignalSettings,
    unitMapping,
    valueDisplayMode,
    lsbMapping,
    offsetMapping,
    modeMapping,
    showMode,
    showAlias,
    removeCursor,
    clearAllCursors,
    toggleTableTranspose,
    hoverCursor,
    setTableContentWidth,
  
    } = useWaveformStore(useShallow((state) => ({
      theme: state.theme,
      showTable: state.showTable,
      isTableTransposed: state.isTableTransposed,
      cursors: state.cursors,
      chartData: state.chartData,
      signalSettings: state.signalSettings,
      unitMapping: state.unitMapping,
      valueDisplayMode: state.valueDisplayMode,
      lsbMapping: state.lsbMapping,
      offsetMapping: state.offsetMapping,
      modeMapping: state.modeMapping,
      showMode: state.showMode,
      showAlias: state.showAlias,
      removeCursor: state.removeCursor,
      clearAllCursors: state.clearAllCursors,
      toggleTableTranspose: state.toggleTableTranspose,
      hoverCursor: state.hoverCursor,
      setTableContentWidth: state.setTableContentWidth
    })));

  const view = useCompareView();
  const signalSettings = view.enabled ? view.settings : storedSignalSettings;
  const compareData = useWaveformStore(s => s.compareChartData);
  const inactive = useWaveformStore((s) => s.inactive);
  const inactiveResult = useMemo(
    () => computeInactive(chartData, inactive),
    [chartData, inactive],
  );

  const compareInactive = useMemo(() => computeInactive(compareData, inactive), [compareData, inactive]);
  const isTableTransposed = forceTransposed !== undefined ? forceTransposed : isTableTransposedState;

  const activeSignals = view.signals.filter(
    (sig) => signalSettings[sig]?.checked && view.visible(sig),
  );

  const tableColumns = activeSignals.flatMap(sig => {
    const presence = view.summary[sig]?.presence;
    const sides = !view.enabled ? [""]
      : presence === "aOnly" ? ["A"]
      : presence === "bOnly" ? ["B"]
      : Object.values(modeMapping[sig] ?? {}).some(name => name.trim()) ? ["A", "B"] : ["A", "B", "Δ"];
    return sides.map(side => ({ sig, side }));
  });
  const hasCompareColumns = tableColumns.some(({ side }) => !!side);
  const columnGroups = tableColumns.reduce<{ sig: string; count: number }[]>((groups, { sig }) => {
    const previous = groups[groups.length - 1];
    if (previous?.sig === sig) previous.count += 1;
    else groups.push({ sig, count: 1 });
    return groups;
  }, []);

  // --- 幅の動的計測のためのRefとState ---
  const col1Ref = useRef<HTMLTableCellElement>(null);
  const col2Ref = useRef<HTMLTableCellElement>(null);
  const signalNameRef = useRef<HTMLTableCellElement>(null);
  const [colWidths, setColWidths] = useState({ w1: 0, w2: 0 });
  const tableRef = useRef<HTMLTableElement>(null);

  useLayoutEffect(() => {
    if (isTableTransposed || cursors.length === 0 && !hoverCursor) {
      setTableContentWidth(null);
      return;
    }

    let animFrame: number;
    const observer = new ResizeObserver((entries) => {
      animFrame = requestAnimationFrame(() => {
        const entry = entries[0];
        if (entry) {
          const rect = entry.target.getBoundingClientRect();
          setTableContentWidth(rect.width);
        }
      });
    });

    if (tableRef.current) observer.observe(tableRef.current);

    return () => {
      observer.disconnect();
      cancelAnimationFrame(animFrame);
    };
  }, [isTableTransposed, cursors.length, hoverCursor, setTableContentWidth]);

  useLayoutEffect(() => {
    if (!isTableTransposed || hasCompareColumns) {
      let animFrame: number;
      const getWidths = () => {
        const firstColumn = isTableTransposed ? signalNameRef.current : col1Ref.current;
        const w1 = firstColumn?.getBoundingClientRect().width || 0;
        const w2 = col2Ref.current?.getBoundingClientRect().width || 0;
        setColWidths((prev) => {
          // cssのずれを防ぐために少数の場合は ceil などしないと隙間が空く可能性があるが、そのままで試す
          if (prev.w1 === w1 && prev.w2 === w2) return prev;
          return { w1, w2 };
        });
      };

      const observer = new ResizeObserver(() => {
        animFrame = requestAnimationFrame(getWidths);
      });

      const firstColumn = isTableTransposed ? signalNameRef.current : col1Ref.current;
      if (firstColumn) observer.observe(firstColumn);
      if (!isTableTransposed && col2Ref.current) observer.observe(col2Ref.current);

      animFrame = requestAnimationFrame(getWidths);

      return () => {
        observer.disconnect();
        cancelAnimationFrame(animFrame);
      };
    }
  }, [isTableTransposed, cursors, hoverCursor, showTable, hasCompareColumns, signalSettings]);

  if (!showTable) return null;

  // Combine markers and hover cursor
  const displayRows: any[] = cursors.map((c, idx) => ({
    ...c,
    isHover: false,
    originalIndex: idx,
  }));
  const activeHover = hoverCursor || { time: 0, index: 0 };
  displayRows.unshift({
    ...activeHover,
    isHover: true,
    originalIndex: -1,
    isEmptyHover: !hoverCursor,
  });

  const renderFormattedValue = (
    sig: string,
    index: number,
    isEmptyHover?: boolean,
    side = "",
  ) => {
    if (isEmptyHover) return "-";
    if (!chartData) return "-";
    if (view.enabled) {
      const step = view.chartData?.steps[index];
      if (step === undefined) return "—";
      const a = compareSample(compareData, sig, step), b = compareSample(chartData, sig, step);
      const aLabel = a.index >= 0 ? inactiveLabel(compareInactive, sig, a.index) : null;
      const bLabel = b.index >= 0 ? inactiveLabel(inactiveResult, sig, b.index) : null;
      const different = !aLabel && !bLabel && a.raw !== null && b.raw !== null && a.raw !== b.raw;
      const conv = getDisplayConversion(sig, lsbMapping, offsetMapping, valueDisplayMode);
      let text: string;
      const sample = side === "A" ? a : b, label = side === "A" ? aLabel : bLabel;
      if (side === "Δ") {
        if (a.raw === null || b.raw === null || aLabel || bLabel) return "—";
        const delta = toPhysical(b.raw, conv) - toPhysical(a.raw, conv);
        text = `${delta > 0 ? "+" : ""}${formatPhysical(delta, conv)}`;
      } else {
        if (label) return <span className="text-gray-400">— ({label})</span>;
        if (sample.raw === null) return index >= chartData.steps.length ? "値なし" : "—";
        text = String(formatPhysical(toPhysical(sample.raw, conv), conv));
        if (modeMapping[sig]?.[sample.raw] !== undefined) text += ` (${modeMapping[sig][sample.raw]})`;
      }
      return <span className={different ? "text-red-600 dark:text-red-400 font-bold" : ""}>{text}</span>;
    }
    const label = inactiveLabel(inactiveResult, sig, index);
    if (label) {
      return <span className="text-gray-400 dark:text-gray-400">— ({label})</span>;
    }
    let rawVal = chartData.seriesMap[sig][index];
    if (rawVal === null || rawVal === undefined) return "-";

    const conv = getDisplayConversion(sig, lsbMapping, offsetMapping, valueDisplayMode);
    let physVal = toPhysical(rawVal, conv);
    let displayVal = formatPhysical(physVal, conv);

    let str = String(displayVal);
    if (
      modeMapping[sig] &&
      modeMapping[sig][rawVal] !== undefined
    ) {
      str += ` (${modeMapping[sig][rawVal]})`;
    }
    return str;
  };

  return (
    <div id={id} className="flex flex-col h-full bg-white dark:bg-slate-800 w-full border-t border-gray-200 dark:border-gray-700 transition-colors">
      <div className="flex flex-wrap items-center gap-2 px-2.5 py-1.5 bg-gray-50 dark:bg-slate-900 border-b border-gray-200 dark:border-slate-700 flex-shrink-0 shadow-sm z-10 w-full min-h-[32px] transition-colors">
        <button
          className="bg-green-500 hover:bg-green-600 dark:bg-green-600 dark:hover:bg-green-700 text-white px-2.5 py-1 text-[11px] rounded transition-colors whitespace-nowrap"
          onClick={toggleTableTranspose}
        >
          縦横入替
        </button>
        <button
          className="bg-white dark:bg-slate-800 border border-gray-300 dark:border-slate-600 text-gray-700 dark:text-gray-300 hover:bg-gray-100 dark:hover:bg-slate-700 px-2.5 py-1 text-[11px] rounded transition-colors whitespace-nowrap"
          onClick={clearAllCursors}
        >
          全カーソル削除
        </button>
        <span className="text-[10px] text-gray-500 dark:text-gray-400 ml-1 hidden sm:inline">
          ※グラフ上の任意の場所をダブルクリックするとカーソルが追加されます。
        </span>
      </div>

      <div className="flex-1 overflow-auto bg-white dark:bg-slate-800 min-h-0 w-full relative transition-colors">
        {displayRows.length === 0 ? (
          <div className="p-4 text-center text-gray-500 text-sm h-full flex items-center justify-center">
            カーソルがありません。波形上をダブルクリックするか、波形上にマウスを合わせてください。
          </div>
        ) : (
          <table
            ref={tableRef}
            className={cn("border-collapse text-[11px] dark:text-gray-200", isTableTransposed && "w-full min-w-full")}
          >
            {!isTableTransposed ? (
              // === Normal Horizontal Layout ===
              <>
                <thead className="bg-[#e8e8e8] dark:bg-slate-700 sticky top-0 z-20 transition-colors">
                  <tr>
                    <th
                      ref={col1Ref}
                      rowSpan={hasCompareColumns ? 2 : undefined}
                      className="sticky z-30 bg-gray-100 dark:bg-slate-700 border-b border-r border-gray-300 dark:border-slate-600 py-[3px] px-1.5 whitespace-nowrap box-border w-[1%]"
                      style={{ left: 0 }}
                    >
                      No.
                    </th>
                    <th
                      ref={col2Ref}
                      rowSpan={hasCompareColumns ? 2 : undefined}
                      className="sticky z-30 bg-[#e8e8e8] dark:bg-slate-700 border-b border-gray-300 dark:border-slate-600 py-[3px] px-1.5 text-right whitespace-nowrap box-border w-[1%]"
                      style={{ left: colWidths.w1 }}
                    >
                      時間(ms)
                    </th>
                    <th
                      rowSpan={hasCompareColumns ? 2 : undefined}
                      className="sticky z-30 bg-gray-100 dark:bg-slate-700 border-b border-r border-gray-300 dark:border-slate-600 py-[3px] px-1.5 text-right text-red-600 dark:text-red-400 shadow-[1px_0_0_#ccc] dark:shadow-none whitespace-nowrap box-border w-[1%]"
                      style={{ left: colWidths.w1 + colWidths.w2 }}
                    >
                      Δt
                    </th>
                    {columnGroups.map(({ sig, count }) => {
                      const displayName = (showAlias && signalSettings[sig].alias) || displaySignalName(sig, activeSignals);
                      // Raw mode drops the unit here; the toolbar already says values are raw.
                      const unitText = valueDisplayMode === "raw" ? "" : getDisplayUnit(sig, unitMapping, modeMapping, valueDisplayMode);
                      const unit = unitText ? ` [${unitText}]` : "";
                      return (
                        <th
                          key={sig}
                          colSpan={hasCompareColumns ? count : undefined}
                          title={sig}
                          className={cn("py-[3px] px-1.5 border-b border-gray-300 dark:border-slate-600 border-r border-gray-200 dark:border-slate-600 bg-gray-50 dark:bg-slate-700 whitespace-normal break-words leading-tight align-middle", hasCompareColumns ? "text-center" : "text-left")}
                          style={{ color: signalSettings[sig].color }}
                        >
                          {displayName}
                          {unit}
                        </th>
                      );
                    })}
                  </tr>
                  {hasCompareColumns && (
                    <tr>
                      {tableColumns.map(({ sig, side }) => (
                        <th
                          key={`${sig}-${side}`}
                          className="py-[3px] px-1.5 border-b border-gray-300 dark:border-slate-600 border-r border-gray-200 dark:border-slate-600 bg-gray-50 dark:bg-slate-700 text-center whitespace-nowrap"
                          style={{ color: signalSettings[sig].color }}
                        >
                          {side}
                        </th>
                      ))}
                    </tr>
                  )}
                </thead>
                <tbody>
                  {displayRows.map((c) => {
                    const dt =
                      !c.isHover && c.originalIndex > 0
                        ? Number(
                            (
                              c.time - cursors[c.originalIndex - 1].time
                            ).toPrecision(7),
                          )
                        : "-";
                    return (
                      <tr
                        key={c.isHover ? "hover" : c.originalIndex}
                        className={cn(
                          "hover:bg-blue-50/50 dark:hover:bg-slate-600/50",
                          c.isHover && "bg-blue-50/20 dark:bg-slate-700/50 export-ignore-hover",
                        )}
                      >
                        <td
                          className="sticky z-10 bg-gray-50 dark:bg-slate-800 border-b border-r border-gray-200 dark:border-slate-600 py-[3px] px-1.5 text-center whitespace-nowrap box-border"
                          style={{ left: 0 }}
                        >
                          <div className="flex items-center justify-center gap-1">
                            {!c.isHover && (
                              <button
                                onClick={() => removeCursor(c.originalIndex)}
                                className="export-ignore-action flex items-center justify-center w-[14px] h-[14px] rounded bg-red-100 dark:bg-red-900 text-red-500 dark:text-red-300 hover:text-white hover:bg-red-500 transition-colors flex-shrink-0"
                                aria-label="カーソルを削除"
                              >
                                <X className="w-2.5 h-2.5" />
                              </button>
                            )}
                            <span>{c.isHover ? "Hov" : `C${c.originalIndex + 1}`}</span>
                          </div>
                        </td>
                        <td
                          className="sticky z-10 bg-white dark:bg-slate-800 border-b border-gray-200 dark:border-slate-600 py-[3px] px-1.5 text-right whitespace-nowrap box-border"
                          style={{ left: colWidths.w1 }}
                        >
                          {(c as any).isEmptyHover ? "-" : c.time}
                        </td>
                        <td
                          className="sticky z-10 bg-gray-50 dark:bg-slate-800 border-b border-r border-gray-200 dark:border-slate-600 py-[3px] px-1.5 text-right text-red-600 dark:text-red-400 shadow-[1px_0_0_#ccc] dark:shadow-none whitespace-nowrap box-border"
                          style={{ left: colWidths.w1 + colWidths.w2 }}
                        >
                          {dt}
                        </td>
                        {tableColumns.map(({ sig, side }) => (
                      <td
                        key={`${sig}-${side}`}
                        className="py-[3px] px-1.5 text-right border-b border-gray-200 dark:border-slate-600 border-r border-gray-100 dark:border-slate-700 whitespace-nowrap"
                        style={{
                          color: theme === 'dark' ? signalSettings[sig].color : getDarkerColor(signalSettings[sig].color),
                        }}
                      >
                            {renderFormattedValue(
                              sig,
                              c.index,
                              (c as any).isEmptyHover,
                              side,
                            )}
                          </td>
                        ))}
                      </tr>
                    );
                  })}
                </tbody>
              </>
            ) : (
              // === Transposed Vertical Layout ===
              <>
                <thead className="bg-[#e8e8e8] dark:bg-slate-700 sticky top-0 z-30 transition-colors">
                  <tr>
                    <th colSpan={hasCompareColumns ? 2 : undefined} className="sticky top-0 left-0 z-40 bg-[#e8e8e8] dark:bg-slate-700 border-r border-b border-gray-300 dark:border-slate-600 min-w-[120px] max-w-[250px] py-[3px] px-1.5 whitespace-nowrap box-border overflow-hidden text-ellipsis">
                      信号
                    </th>
                    {displayRows.map((c) => (
                      <th
                        key={c.isHover ? "hover" : c.originalIndex}
                        className={cn("sticky top-0 z-30 bg-gray-100 dark:bg-slate-700 border-r border-b border-gray-200 dark:border-slate-600 min-w-[70px] py-[3px] px-1.5 whitespace-nowrap box-border", c.isHover && "export-ignore-hover")}
                      >
                        <div className="flex justify-between items-center w-full min-w-[50px] pl-1 relative group">
                          <span>
                            {c.isHover ? "Hov" : `C${c.originalIndex + 1}`}
                          </span>
                          {!c.isHover && (
                            <button
                              onClick={() => removeCursor(c.originalIndex)}
                              className="flex items-center justify-center w-4 h-4 rounded bg-red-100 dark:bg-red-900 text-red-500 dark:text-red-300 hover:text-white hover:bg-red-500 transition-colors"
                              aria-label="カーソルを削除"
                            >
                              <X className="w-3 h-3" />
                            </button>
                          )}
                        </div>
                      </th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  <tr className="hover:bg-gray-50 dark:hover:bg-slate-600/50">
                    <td colSpan={hasCompareColumns ? 2 : undefined} className="sticky left-0 z-20 bg-gray-50 dark:bg-slate-700 border-r border-b border-gray-300 dark:border-slate-600 min-w-[120px] max-w-[250px] py-[3px] px-1.5 whitespace-nowrap box-border shadow-[1px_0_0_#eee] dark:shadow-none overflow-hidden text-ellipsis">
                      時間(ms)
                    </td>
                    {displayRows.map((c) => (
                      <td
                        key={c.isHover ? "hover" : c.originalIndex}
                        className={cn("text-right bg-gray-50 dark:bg-slate-800 border-b border-r border-gray-200 dark:border-slate-600 min-w-[70px] py-[3px] px-1.5 whitespace-nowrap box-border break-all", c.isHover && "export-ignore-hover")}
                      >
                        {(c as any).isEmptyHover ? "-" : c.time}
                      </td>
                    ))}
                  </tr>
                  <tr className="hover:bg-gray-50 dark:hover:bg-slate-600/50">
                    <td colSpan={hasCompareColumns ? 2 : undefined} className="sticky left-0 z-20 bg-gray-50 dark:bg-slate-700 border-r border-b border-gray-300 dark:border-slate-600 min-w-[120px] max-w-[250px] py-[3px] px-1.5 whitespace-nowrap box-border shadow-[1px_0_0_#eee] dark:shadow-none overflow-hidden text-ellipsis">
                      Δt(ms)
                    </td>
                    {displayRows.map((c) => {
                      const dt =
                        !c.isHover && c.originalIndex > 0
                          ? Number(
                              (
                                c.time - cursors[c.originalIndex - 1].time
                              ).toPrecision(7),
                            )
                          : "-";
                      return (
                        <td
                          key={c.isHover ? "hover" : c.originalIndex}
                          className={cn("text-right bg-gray-50 dark:bg-slate-800 text-red-600 dark:text-red-400 border-b border-r border-gray-200 dark:border-slate-600 min-w-[70px] py-[3px] px-1.5 whitespace-nowrap box-border", c.isHover && "export-ignore-hover")}
                        >
                          {dt}
                        </td>
                      );
                    })}
                  </tr>
                  {tableColumns.map(({ sig, side }, columnIndex) => {
                    const isGroupStart = columnIndex === 0 || tableColumns[columnIndex - 1].sig !== sig;
                    const displayName = (showAlias && signalSettings[sig].alias) || displaySignalName(sig, activeSignals);
                    // Raw mode drops the unit here; the toolbar already says values are raw.
                    const unitText = valueDisplayMode === "raw" ? "" : getDisplayUnit(sig, unitMapping, modeMapping, valueDisplayMode);
                    const unit = unitText ? ` [${unitText}]` : "";
                    return (
                      <tr key={`${sig}-${side}`} className="hover:bg-blue-50/30 dark:hover:bg-slate-600/20">
                        {(!hasCompareColumns || isGroupStart) && (
                          <td
                            ref={columnIndex === 0 ? signalNameRef : undefined}
                            rowSpan={hasCompareColumns ? columnGroups.find(group => group.sig === sig)?.count : undefined}
                            className="sticky left-0 z-20 bg-white dark:bg-slate-700 border-r border-b border-gray-200 dark:border-slate-600 min-w-[120px] max-w-[250px] py-[3px] px-1.5 whitespace-nowrap box-border shadow-[1px_0_0_#eee] dark:shadow-none align-middle overflow-hidden text-ellipsis"
                            style={{
                              color: signalSettings[sig].color,
                              borderLeft: `3px solid ${signalSettings[sig].color}`,
                            }}
                            title={sig}
                          >
                            {displayName}
                            {unit}
                          </td>
                        )}
                        {hasCompareColumns && (
                          <td
                            className="sticky z-20 bg-white dark:bg-slate-700 border-r border-b border-gray-200 dark:border-slate-600 w-[1%] py-[3px] px-1.5 text-center whitespace-nowrap box-border shadow-[1px_0_0_#eee] dark:shadow-none"
                            style={{ left: colWidths.w1, color: signalSettings[sig].color }}
                            title={`${sig} ${side}`}
                          >
                            {side}
                          </td>
                        )}
                        {displayRows.map((c) => (
                          <td
                            key={c.isHover ? "hover" : c.originalIndex}
                            className={cn("text-right border-b border-r border-gray-100 dark:border-slate-600 min-w-[70px] py-[3px] px-1.5 whitespace-nowrap box-border", c.isHover && "export-ignore-hover")}
                            style={{
                              color: theme === 'dark' ? signalSettings[sig].color : getDarkerColor(signalSettings[sig].color),
                            }}
                          >
                            {renderFormattedValue(
                              sig,
                              c.index,
                              (c as any).isEmptyHover,
                              side,
                            )}
                          </td>
                        ))}
                      </tr>
                    );
                  })}
                </tbody>
              </>
            )}
          </table>
        )}
      </div>
    </div>
  );
}
