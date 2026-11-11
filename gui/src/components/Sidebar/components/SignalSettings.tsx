import React, { useState, useEffect } from "react";
import { useWaveformStore } from "../../../store/useWaveformStore";
import { ChevronDown, ChevronRight, RotateCcw } from "lucide-react";
import { evaluateExpression, formatPhysical, toPhysical } from "../../../utils/helpers";
import { SignalSetting, SignalSettingsSections } from "../../../types";
import { useShallow } from "zustand/react/shallow";
import { StateNameEditor, StateSummary } from "./StateNameEditor";
import { StateRow, mappingToRows, rowsToMapping, sameMapping, stateRowErrors } from "../../../utils/stateNames";

function Section({ sectionKey, title, summary, children }: {
  sectionKey: keyof SignalSettingsSections;
  title: string;
  summary: React.ReactNode;
  children: React.ReactNode;
}) {
  const open = useWaveformStore((state) => state.signalSettingsSections[sectionKey]);
  const toggle = useWaveformStore((state) => state.toggleSignalSettingsSection);
  return (
    <div className={`sec ${open ? "" : "closed"} ${sectionKey === "yBreak" ? "" : "border-b border-gray-200 dark:border-slate-700"}`}>
      <button type="button" aria-expanded={open} onClick={() => toggle(sectionKey)} className="sec-head w-[calc(100%+16px)] -ml-4 flex items-center gap-1 py-1.5 text-left">
        {open ? <ChevronDown size={12} className="text-gray-400 shrink-0" /> : <ChevronRight size={12} className="text-gray-400 shrink-0" />}
        <span className="text-[10px] text-gray-500 dark:text-gray-400 font-bold whitespace-nowrap">{title}</span>
        {!open && <span className="sec-summary ml-auto text-[10px] text-gray-400 truncate min-w-0">{summary}</span>}
      </button>
      {open && children}
    </div>
  );
}

export function SignalSettings({ sigName, setting, groupAutoRange }: {
  sigName: string;
  setting: SignalSetting;
  groupAutoRange: { min: number; max: number; breakStart?: number; breakEnd?: number };
}) {
  const { updateSignalSetting, setSignalConversion, lsb, offset, unit, states, undefinedText, rawData } = useWaveformStore(useShallow((state) => ({
    updateSignalSetting: state.updateSignalSetting,
    setSignalConversion: state.setSignalConversion,
    lsb: state.lsbMapping[sigName] ?? "",
    offset: state.offsetMapping[sigName] ?? "",
    unit: state.unitMapping[sigName] ?? "",
    states: state.modeMapping[sigName],
    undefinedText: (state.undefinedMapping[sigName] ?? []).join(","),
    rawData: state.chartData?.seriesMap[sigName],
  })));
  const [localLsb, setLocalLsb] = useState(lsb);
  const [localOffset, setLocalOffset] = useState(offset);
  const [localUnit, setLocalUnit] = useState(unit);
  const [raw, setRaw] = useState(() => String(rawData?.find((value) => value !== null) ?? 0));
  useEffect(() => { setLocalLsb(lsb); }, [sigName, lsb]);
  useEffect(() => { setLocalOffset(offset); }, [sigName, offset]);
  useEffect(() => { setLocalUnit(unit); }, [sigName, unit]);
  const [localUndefined, setLocalUndefined] = useState(undefinedText);
  useEffect(() => { setLocalUndefined(undefinedText); }, [sigName, undefinedText]);
  // 一覧編集の「除外値」列と同じく、カンマ区切りの数値（空欄で解除）
  const invalidUndefined = localUndefined.trim() !== ""
    && !localUndefined.split(",").every((value) => value.trim() !== "" && Number.isFinite(Number(value)));
  const commitUndefined = () => {
    if (invalidUndefined) return;
    const values = localUndefined.trim() ? localUndefined.split(",").map(Number) : [];
    if (values.join(",") !== undefinedText) setSignalConversion(sigName, { undefinedValues: values });
  };
  const undefinedKeyDown = (event: React.KeyboardEvent<HTMLInputElement>) => {
    if (event.nativeEvent.isComposing) return;
    if (event.key === "Enter") {
      event.preventDefault();
      event.currentTarget.blur();
    } else if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      setLocalUndefined(undefinedText);
    }
  };
  const [stateRows, setStateRows] = useState<StateRow[]>(() => mappingToRows(states));
  // 外部（一覧編集・設定ファイル読み込み）の変更を反映する。入力途中の空行は残す。
  useEffect(() => {
    setStateRows((rows) => sameMapping(rowsToMapping(rows), states) ? rows : mappingToRows(states));
  }, [sigName, states]);
  const changeStateRows = (rows: StateRow[], commit: boolean) => {
    setStateRows(rows);
    if (!commit || stateRowErrors(rows).some(Boolean)) return;
    const mapping = rowsToMapping(rows);
    if (!sameMapping(mapping, states)) setSignalConversion(sigName, { states: mapping });
  };
  useEffect(() => {
    setRaw(String(rawData?.find((value) => value !== null) ?? 0));
  }, [sigName, rawData]);

  const evaluatedLsb = localLsb.trim() === "" ? 1 : evaluateExpression(localLsb);
  const evaluatedOffset = localOffset.trim() === "" ? 0 : evaluateExpression(localOffset);
  const invalidLsb = evaluatedLsb === null || evaluatedLsb === 0;
  const invalidOffset = evaluatedOffset === null;
  const previewConversion = { lsb: evaluatedLsb ?? 1, offset: evaluatedOffset ?? 0 };
  const physical = toPhysical(Number(raw), previewConversion);
  const preview = invalidLsb || invalidOffset || raw.trim() === "" || !Number.isFinite(physical)
    ? "—" : formatPhysical(physical, previewConversion);
  const commitConversion = (key: "lsb" | "offset" | "unit") => {
    if (key === "lsb" && !invalidLsb) setSignalConversion(sigName, { lsb: localLsb.trim() });
    if (key === "offset" && !invalidOffset) setSignalConversion(sigName, { offset: localOffset.trim() });
    if (key === "unit") setSignalConversion(sigName, { unit: localUnit });
  };
  const conversionKeyDown = (key: "lsb" | "offset" | "unit") => (event: React.KeyboardEvent<HTMLInputElement>) => {
    if (event.nativeEvent.isComposing) return;
    if (event.key === "Enter") {
      event.preventDefault();
      event.currentTarget.blur();
    } else if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      if (key === "lsb") setLocalLsb(lsb);
      if (key === "offset") setLocalOffset(offset);
      if (key === "unit") setLocalUnit(unit);
    }
  };
  const conversionInputClass = "w-full px-1.5 py-1 text-[11px] border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800 rounded outline-none focus:border-blue-500";

  // 色選択時のパフォーマンス改善のためのローカルステート
  const [localColor, setLocalColor] = useState(setting.color || "#000000");

  // 外部からの更新（別シグナルの選択や初期ロード時）をローカルステートに反映
  useEffect(() => {
    setLocalColor(setting.color || "#000000");
  }, [setting.color]);

  // ローカルステートの変更をDebounceしてグローバルステートに反映
  useEffect(() => {
    if (localColor !== (setting.color || "#000000")) {
      const handler = setTimeout(() => {
        updateSignalSetting(sigName, { color: localColor });
      }, 200); // ドラッグ中の再描画負荷を軽減するため200ms待機
      return () => clearTimeout(handler);
    }
  }, [localColor, setting.color, sigName, updateSignalSetting]);
  
  const displayMin =
    !setting.yAuto && setting.yMin !== null
      ? setting.yMin
      : setting.yAuto
      ? Number(groupAutoRange.min.toPrecision(4))
      : "";
  const displayMax =
    !setting.yAuto && setting.yMax !== null
      ? setting.yMax
      : setting.yAuto
      ? Number(groupAutoRange.max.toPrecision(4))
      : "";

  const displayBreakStart =
    setting.yBreakStart !== null && setting.yBreakStart !== undefined
      ? setting.yBreakStart
      : setting.yAuto && groupAutoRange.breakStart !== undefined
      ? Number(groupAutoRange.breakStart.toPrecision(4))
      : "";

  const displayBreakEnd =
    setting.yBreakEnd !== null && setting.yBreakEnd !== undefined
      ? setting.yBreakEnd
      : setting.yAuto && groupAutoRange.breakEnd !== undefined
      ? Number(groupAutoRange.breakEnd.toPrecision(4))
      : "";

  return (
    <div className="bg-gray-50 dark:bg-slate-900 border-t border-dashed border-gray-200 dark:border-slate-700 pl-8 pr-3 py-1.5 transition-colors">
      <Section sectionKey="display" title="表示" summary={
        <span className="flex items-center gap-1 max-w-[170px]">
          <span className="inline-block w-2 h-2 rounded-sm shrink-0" style={{ background: localColor }} />
          <span className="truncate">{setting.alias || sigName} / {setting.lineWidth ?? 1.5} / {({ solid: "実線", dashed: "破線", dotted: "点線" })[setting.lineType || "solid"]}</span>
        </span>
      }>
      <div className="sec-body grid grid-cols-2 gap-x-3 gap-y-2 pb-2.5">
      <div className="col-span-2 flex flex-col">
        <label className="text-[10px] text-gray-500 dark:text-gray-400 mb-1 font-bold">
          表示名 (別名)
        </label>
        <input
          type="text"
          placeholder="別名を入力..."
          value={setting.alias || ""}
          onChange={(e) => updateSignalSetting(sigName, { alias: e.target.value })}
          className="w-full box-border px-1.5 py-1 text-[11px] border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800 rounded outline-none transition-colors"
        />
      </div>

      <div className="flex flex-col">
        <label className="text-[10px] text-gray-500 dark:text-gray-400 mb-1 font-bold">
          高さ割合
        </label>
        <div className="flex items-center gap-1">
          <button
            className="px-2 py-0.5 bg-white dark:bg-slate-700 border border-gray-300 dark:border-slate-600 hover:bg-gray-100 dark:hover:bg-slate-600 rounded text-xs text-gray-600 dark:text-gray-300 transition-colors"
            onClick={(e) => {
              e.stopPropagation();
              updateSignalSetting(sigName, {
                weight: Math.max(0.5, (setting.weight ?? 1) - 0.5),
              });
            }}
          >
            -
          </button>
          <span className="text-[11px] font-mono w-6 text-center dark:text-gray-200">
            {setting.weight ?? 1}
          </span>
          <button
            className="px-2 py-0.5 bg-white dark:bg-slate-700 border border-gray-300 dark:border-slate-600 hover:bg-gray-100 dark:hover:bg-slate-600 rounded text-xs text-gray-600 dark:text-gray-300 transition-colors"
            onClick={(e) => {
              e.stopPropagation();
              updateSignalSetting(sigName, {
                weight: (setting.weight ?? 1) + 0.5,
              });
            }}
          >
            +
          </button>
        </div>
      </div>

      <div className="flex flex-col">
        <label className="text-[10px] text-gray-500 dark:text-gray-400 mb-1 font-bold">
          線の色
        </label>
        <input
          type="color"
          value={localColor}
          onChange={(e) => setLocalColor(e.target.value)}
          className="w-full h-[26px] box-border p-0 border border-gray-300 dark:border-slate-600 rounded cursor-pointer"
        />
      </div>
      <div className="flex flex-col">
        <label className="text-[10px] text-gray-500 dark:text-gray-400 mb-1 font-bold">
          線の太さ
        </label>
        <input
          type="number"
          value={setting.lineWidth ?? 1.5}
          step="0.5"
          min="0.5"
          max="10"
          onChange={(e) => updateSignalSetting(sigName, { lineWidth: parseFloat(e.target.value) || 1.5 })}
          className="w-full box-border px-1.5 py-1 text-[11px] border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800 rounded outline-none transition-colors"
        />
      </div>
      <div className="flex flex-col">
        <label className="text-[10px] text-gray-500 dark:text-gray-400 mb-1 font-bold">
          線の種類
        </label>
        <select
          value={setting.lineType || "solid"}
          onChange={(e) => updateSignalSetting(sigName, { lineType: e.target.value as any })}
          className="w-full box-border px-1 border border-gray-300 dark:border-slate-600 rounded text-[11px] h-[26px] bg-white dark:bg-slate-800 outline-none transition-colors"
        >
          <option value="solid">実線</option>
          <option value="dashed">破線</option>
          <option value="dotted">点線</option>
        </select>
      </div>

      </div>
      </Section>
      <Section sectionKey="conversion" title="物理値変換" summary={<span className="font-mono max-w-[150px]">×{lsb || "1"} {offset.trim().startsWith("-") ? "" : "+"}{offset || "0"}{unit ? ` ${unit}` : ""}</span>}>
        <div className="sec-body grid grid-cols-[1fr_1fr_64px] gap-x-2 gap-y-1 pb-2.5">
          <div className="flex flex-col min-w-0">
            <label className="text-[10px] text-gray-500 dark:text-gray-400 mb-1 font-bold">LSB</label>
            <input type="text" aria-label="LSB" aria-invalid={invalidLsb} value={localLsb} onChange={(e) => setLocalLsb(e.target.value)} onBlur={() => commitConversion("lsb")} onKeyDown={conversionKeyDown("lsb")} className={`${conversionInputClass} font-mono ${invalidLsb ? "!border-red-500" : ""}`} />
          </div>
          <div className="flex flex-col min-w-0">
            <label className="text-[10px] text-gray-500 dark:text-gray-400 mb-1 font-bold">オフセット</label>
            <input type="text" aria-label="オフセット" aria-invalid={invalidOffset} value={localOffset} onChange={(e) => setLocalOffset(e.target.value)} onBlur={() => commitConversion("offset")} onKeyDown={conversionKeyDown("offset")} className={`${conversionInputClass} font-mono ${invalidOffset ? "!border-red-500" : ""}`} />
          </div>
          <div className="flex flex-col min-w-0">
            <label className="text-[10px] text-gray-500 dark:text-gray-400 mb-1 font-bold">単位</label>
            <input type="text" aria-label="単位" value={localUnit} onChange={(e) => setLocalUnit(e.target.value)} onBlur={() => commitConversion("unit")} onKeyDown={conversionKeyDown("unit")} className={conversionInputClass} />
          </div>
          <div className={`text-[10px] font-mono min-h-[14px] ${invalidLsb || invalidOffset ? "text-red-500" : "text-gray-400"}`}>
            {invalidLsb || invalidOffset ? "式を解釈できません" : `= ${Number(evaluatedLsb!.toPrecision(7))}`}
          </div>
          <div className="col-span-2" />
          <div className="col-span-3 mt-1 flex items-center gap-1.5 text-[10px] text-gray-500 dark:text-gray-400 bg-white dark:bg-slate-800 border border-gray-200 dark:border-slate-700 rounded px-1.5 py-1">
            <span className="font-bold">確認</span>
            <span className="font-mono">生値</span>
            <input type="number" aria-label="確認用の生値" value={raw} onChange={(e) => setRaw(e.target.value)} className="w-16 px-1 py-0.5 text-[10px] font-mono border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800 rounded outline-none" />
            <span>→</span>
            <span className="font-mono font-bold text-gray-700 dark:text-gray-200">{preview} {localUnit}</span>
            <button type="button" className="ml-auto text-[10px] text-gray-400 hover:text-blue-500 dark:hover:text-blue-400 flex items-center gap-0.5" title="LSB=1, オフセット=0 に戻す" onClick={() => {
              setLocalLsb("");
              setLocalOffset("");
              setSignalConversion(sigName, { lsb: "", offset: "" });
            }}><RotateCcw size={11} />初期化</button>
          </div>
          <div className="col-span-3 flex flex-col min-w-0 mt-1">
            <label className="text-[10px] text-gray-500 dark:text-gray-400 mb-1 font-bold">除外値（生値、カンマ区切り）</label>
            <input type="text" aria-label="除外値" aria-invalid={invalidUndefined} title={invalidUndefined ? "数値をカンマ区切りで入力してください" : undefined} placeholder="例: 32767" value={localUndefined} onChange={(e) => setLocalUndefined(e.target.value)} onBlur={commitUndefined} onKeyDown={undefinedKeyDown} className={`${conversionInputClass} font-mono ${invalidUndefined ? "!border-red-500" : ""}`} />
          </div>
          <div className="col-span-3 text-[10px] text-gray-400 leading-snug">物理値 = 生値 × LSB + オフセット　／　除外値は生値で判定</div>
        </div>
      </Section>
      <Section sectionKey="states" title="状態名" summary={<StateSummary mapping={states} />}>
        <div className="sec-body pb-2.5">
          <StateNameEditor rows={stateRows} rawData={rawData} onChange={changeStateRows} onEscape={() => setStateRows(mappingToRows(states))} />
        </div>
      </Section>
      <Section sectionKey="yScale" title="Y軸 スケール設定" summary={<span className="font-mono">{setting.yAuto ? `Auto (${Number(groupAutoRange.min.toPrecision(4))} ~ ${Number(groupAutoRange.max.toPrecision(4))})` : `${displayMin} ~ ${displayMax}`}</span>}>
      <div className="sec-body grid pb-2.5">
         <div className="flex items-center justify-end mb-1.5">
           <label className="flex items-center cursor-pointer text-[10px] bg-white dark:bg-slate-700 border border-gray-300 dark:border-slate-600 px-1.5 py-0.5 rounded shadow-sm hover:bg-gray-50 dark:hover:bg-slate-600 transition-colors">
             <input
               type="checkbox"
               checked={setting.yAuto}
               onChange={(e) => {
                 const isAuto = e.target.checked;
                 if (!isAuto) {
                   updateSignalSetting(sigName, {
                     yAuto: false,
                     yMin: Number(groupAutoRange.min.toPrecision(4)),
                     yMax: Number(groupAutoRange.max.toPrecision(4)),
                     yBreakStart:
                       groupAutoRange.breakStart !== undefined
                         ? Number(groupAutoRange.breakStart.toPrecision(4))
                         : null,
                     yBreakEnd:
                       groupAutoRange.breakEnd !== undefined
                         ? Number(groupAutoRange.breakEnd.toPrecision(4))
                         : null,
                   });
                 } else {
                   updateSignalSetting(sigName, {
                     yAuto: true,
                     yMin: null,
                     yMax: null,
                     yBreakStart: null,
                     yBreakEnd: null,
                   });
                 }
               }}
               className="mr-1.5 accent-blue-500"
             />
             Auto
           </label>
         </div>
         <div className="flex items-center gap-1.5">
           <input
             type="number"
             value={displayMin}
             placeholder="Min"
             onChange={(e) =>
               updateSignalSetting(sigName, {
                 yMin: e.target.value ? parseFloat(e.target.value) : null,
                 yAuto: false,
               })
             }
             className={`w-full px-1.5 py-1 text-[11px] border border-gray-300 dark:border-slate-600 rounded outline-none transition-colors ${
               setting.yAuto ? "bg-gray-100 dark:bg-slate-800 text-gray-400 dark:text-gray-500" : "bg-white dark:bg-slate-700"
             }`}
           />
           <span className="text-gray-400 text-xs">~</span>
           <input
             type="number"
             value={displayMax}
             placeholder="Max"
             onChange={(e) =>
               updateSignalSetting(sigName, {
                 yMax: e.target.value ? parseFloat(e.target.value) : null,
                 yAuto: false,
               })
             }
             className={`w-full px-1.5 py-1 text-[11px] border border-gray-300 dark:border-slate-600 rounded outline-none transition-colors ${
               setting.yAuto ? "bg-gray-100 dark:bg-slate-800 text-gray-400 dark:text-gray-500" : "bg-white dark:bg-slate-700"
             }`}
           />
         </div>
       </div>

      </Section>
      <Section sectionKey="yBreak" title="Y軸 省略 (Break) 設定" summary={setting.yBreak !== false ? "有効" : "無効"}>
       <div className="sec-body grid pb-2.5">
         <div className="flex items-center justify-end mb-1.5">
           <label className="flex items-center cursor-pointer text-[10px] bg-white dark:bg-slate-700 border border-gray-300 dark:border-slate-600 px-1.5 py-0.5 rounded shadow-sm hover:bg-gray-50 dark:hover:bg-slate-600 transition-colors">
             <input
               type="checkbox"
               checked={setting.yBreak !== false}
               onChange={(e) =>
                 updateSignalSetting(sigName, { yBreak: e.target.checked })
               }
               className="mr-1.5 accent-blue-500"
             />
             有効
           </label>
         </div>
         {setting.yBreak !== false && (
           <div className="flex items-center gap-1">
             <input
               type="number"
               value={displayBreakStart}
               placeholder="Start"
               onChange={(e) =>
                 updateSignalSetting(sigName, {
                   yBreakStart: e.target.value
                     ? parseFloat(e.target.value)
                     : null,
                   yAuto: false,
                 })
               }
               className={`w-full px-1.5 py-1 text-[11px] border border-gray-300 dark:border-slate-600 rounded outline-none transition-colors ${setting.yAuto && setting.yBreakStart == null ? "bg-gray-100 dark:bg-slate-800 text-gray-400 dark:text-gray-500" : "bg-white dark:bg-slate-700"}`}
             />
             <span className="text-gray-400 text-xs">~</span>
             <input
               type="number"
               value={displayBreakEnd}
               placeholder="End"
               onChange={(e) =>
                 updateSignalSetting(sigName, {
                   yBreakEnd: e.target.value
                     ? parseFloat(e.target.value)
                     : null,
                   yAuto: false,
                 })
               }
               className={`w-full px-1.5 py-1 text-[11px] border border-gray-300 dark:border-slate-600 rounded outline-none transition-colors ${setting.yAuto && setting.yBreakEnd == null ? "bg-gray-100 dark:bg-slate-800 text-gray-400 dark:text-gray-500" : "bg-white dark:bg-slate-700"}`}
             />
           </div>
         )}
       </div>
      </Section>
    </div>
  );
}
