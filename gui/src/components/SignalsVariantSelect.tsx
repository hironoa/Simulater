import { useLiveStore } from "../live/liveStore";
import { useWaveformStore } from "../store/useWaveformStore";
import { unique } from "../store/signalDefs";

export function SignalsVariantSelect() {
  const mode = useLiveStore(s => s.mode);
  const defs = useWaveformStore(s => s.signalDefs);
  const selected = useWaveformStore(s => s.selectedVariant);
  const select = useWaveformStore(s => s.setSelectedVariant);
  const names = unique([...defs.variantNames, ...Object.keys(defs.variants)]);
  if (mode === "live" || !names.length) return null;
  return <label className="flex items-center gap-1 text-xs whitespace-nowrap shrink-0">仕向
    <select aria-label="信号情報の仕向" className="bg-slate-700 rounded px-2 py-1" value={selected} onChange={e => select(e.target.value)}>
      <option value="">（共通）</option>
      {unique([...names, ...(selected ? [selected] : [])]).map(name => <option key={name} value={name}>{name}</option>)}
    </select>
  </label>;
}
