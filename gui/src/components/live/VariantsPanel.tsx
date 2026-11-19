import { useWaveformStore } from "../../store/useWaveformStore";
import { unique } from "../../store/signalDefs";
import { useRef, useState } from "react";
import { liveController } from "../../live/controller";
import { useLiveStore } from "../../live/liveStore";
import { decodeVariantsCsv, parseInitFromCsv, type VariantsDoc, type InitValue } from "../../live/variants";
import { setVariantsApplyError, useVariantsStore, variantWarning } from "../../live/variantsStore";
import { SymbolSearch } from "./SymbolSearch";

const field = "px-1.5 py-0.5 text-xs rounded border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-900";
const button = "text-xs px-2 py-0.5 rounded border border-gray-300 dark:border-slate-600 hover:bg-gray-100 dark:hover:bg-slate-700 disabled:opacity-50";
const primaryButton = "text-xs px-2 py-0.5 rounded bg-blue-500 text-white hover:bg-blue-600 disabled:opacity-50";

export function VariantsPanel() {
  const { doc, saved, path, error, loaded, errors } = useVariantsStore();
  const signalDefs = useWaveformStore(s => s.signalDefs);
  const names = unique([...doc.variants.map(v => v.name), ...signalDefs.variantNames, ...Object.keys(signalDefs.variants)]);
  const resetNeeded = useLiveStore(s => s.sim.initStale);
  const setErrors = (errors: Record<string, string>) => useVariantsStore.setState({ errors });
  const [busy, setBusy] = useState(false);
  const file = useRef<HTMLInputElement>(null);
  const current = doc.variants.find(v => v.name === doc.selected);
  const api = liveController().api;
  async function apply(next: VariantsDoc) {
    useVariantsStore.setState({ doc: next });
    setBusy(true);
    try { await api.setInit(next.variants.find(v => v.name === next.selected)?.values ?? []); setErrors({}); }
    catch (e) { setVariantsApplyError(e); }
    finally { setBusy(false); }
  }
  /**
   * 仕向を選んだら、反映して、選択をファイルにも保存する（保存しないと、次の起動で選んでいない状態に戻る）。
   * 仕向ファイルを読めなかったときは、壊れたファイルを上書きしないよう保存しない。
   */
  async function select(name: string) {
    const next = { ...doc, selected: name, variants: name && !doc.variants.some(v => v.name === name) ? [...doc.variants, { name, values: [] }] : doc.variants };
    await apply(next);
    if (error || useVariantsStore.getState().errors[""]) return;
    try { await api.saveVariants(next); useVariantsStore.setState({ saved: JSON.stringify(next) }); }
    catch (e) { setErrors({ "": `選択をファイルに保存できませんでした: ${(e as Error).message}` }); }
  }
  const warning = variantWarning({ doc, loaded, error });
  const update = (values: InitValue[]) => apply({ ...doc, variants: doc.variants.map(v => v.name === doc.selected ? { ...v, values } : v) });
  const rename = (adding: boolean) => {
    const name = window.prompt(adding ? "仕向の名前" : "新しい名前", adding ? "" : doc.selected)?.trim();
    if (!name || doc.variants.some(v => v.name === name)) return;
    void apply({ ...doc, selected: name, variants: adding ? [...doc.variants, { name, values: [] }] : doc.variants.map(v => v.name === doc.selected ? { ...v, name } : v) });
  };
  return <div className="text-xs flex flex-col gap-2">
    {error && <p role="alert" className="text-red-500">{error}（自動保存しません）</p>}
    {warning && <p role="alert" className="font-bold text-amber-800 dark:text-amber-300">{warning}。入力が効かないときは、仕向を選んでください。</p>}
    <select className={field} aria-label="仕向" value={doc.selected} disabled={!loaded || busy} onChange={e => void select(e.target.value)}>
      <option value="">選択なし</option>{names.map(name => <option key={name}>{name}</option>)}
    </select>
    <div className="flex items-center gap-1 whitespace-nowrap"><button className={button} disabled={busy} onClick={() => rename(true)}>追加</button><button className={button} disabled={!current || busy} onClick={() => rename(false)}>名前変更</button><button className={button} disabled={!current || busy} onClick={() => { if (window.confirm("この仕向を削除しますか？")) void apply({ ...doc, selected: "", variants: doc.variants.filter(v => v !== current) }); }}>削除</button><button className={button} disabled={busy} onClick={() => file.current?.click()}>CSV から取り込み</button></div>
    <div className="flex flex-col gap-1">
    <button className={`${primaryButton} self-start`} disabled={!loaded || busy} onClick={async () => {
      if (error && !window.confirm("読めなかった仕向ファイルを上書きしますか？")) return;
      setBusy(true);
      try { await api.saveVariants(doc); useVariantsStore.setState({ saved: JSON.stringify(doc), error: "" }); } catch (e) { setErrors({ "": (e as Error).message }); } finally { setBusy(false); }
    }}>ファイルに保存{JSON.stringify(doc) !== saved ? " *" : ""}</button>
    <small className="break-all text-gray-500">{path}</small>
    </div>
    {current && <>
      <SymbolSearch actions={[{ label: "行を追加", enabled: s => s.writable, onPick: s => { if (!current.values.some(v => v.name === s.name)) void update([...current.values, { name: s.name, value: "0", mode: "once" }]); } }]} />
      <table className="w-full text-left"><thead><tr className="text-[10px] text-gray-500 dark:text-gray-400 border-b border-gray-200 dark:border-slate-700"><th className="text-left font-normal py-0.5">変数</th><th className="text-left font-normal py-0.5">値</th><th className="text-left font-normal py-0.5">書き方</th><th /></tr></thead><tbody>{current.values.map((row, i) => <tr key={`${doc.selected}:${row.name}:${row.value}`} className="border-b border-gray-100 dark:border-slate-800 hover:bg-gray-50 dark:hover:bg-slate-800/60">
        <td title={row.name} className="max-w-28 truncate font-mono py-0.5">{row.name}{errors[row.name] && <p role="alert" className="text-red-500 whitespace-normal">{errors[row.name]}</p>}</td>
        <td><input aria-label={`${row.name} の値`} className={`${field} w-20 font-mono`} defaultValue={row.value} disabled={busy} onBlur={e => { if (row.value !== e.target.value) void update(current.values.map((v, j) => j === i ? { ...v, value: e.target.value } : v)); }} onKeyDown={e => { if (e.key === "Enter") e.currentTarget.blur(); }} /></td>
        <td><select className={field} aria-label={`${row.name} の書き方`} value={row.mode} disabled={busy} onChange={e => void update(current.values.map((v, j) => j === i ? { ...v, mode: e.target.value as InitValue["mode"] } : v))}><option value="once">最初だけ</option><option value="every">毎回</option></select></td>
        <td><button className={button} disabled={busy} onClick={() => void update(current.values.filter((_, j) => i !== j))}>削除</button></td>
      </tr>)}</tbody></table>
    </>}
    {errors[""] && <p role="alert" className="text-red-500">{errors[""]}</p>}
    {resetNeeded && <button className={primaryButton} disabled={busy || Object.keys(errors).length > 0} onClick={async () => { try { await liveController().control({ cmd: "reset" }); } catch (e) { setErrors({ "": (e as Error).message }); } }}>リセットして反映</button>}
    <input ref={file} type="file" accept=".csv" hidden onChange={async e => {
      const picked = e.target.files?.[0]; e.target.value = ""; if (!picked) return;
      try {
        const name = picked.name.replace(/\.[^.]+$/, "");
        const values = parseInitFromCsv(decodeVariantsCsv(await picked.arrayBuffer()));
        if (!values.length) { setErrors({ "": "#Init の初期値が見つかりません" }); return; }
        if (doc.variants.some(v => v.name === name) && !window.confirm(`${name} を上書きしますか？`)) return;
        await apply({ ...doc, selected: name, variants: [...doc.variants.filter(v => v.name !== name), { name, values }] });
      } catch (err) { setErrors({ "": (err as Error).message }); }
    }} />
  </div>;
}
