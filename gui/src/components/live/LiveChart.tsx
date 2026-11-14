import { useEffect, useMemo, useRef, useState } from "react";
import * as echarts from "echarts";
import { useShallow } from "zustand/react/shallow";
import { useWaveformStore } from "../../store/useWaveformStore";
import { useLiveStore } from "../../live/liveStore";
import { liveRecord } from "../../live/liveRecord";
import { buildLiveSeriesData } from "../../live/liveWindow";
import { getDisplayConversion } from "../../utils/helpers";

/** 描画の上限（fps）。samples は約 50ms ごとに届くが、描画はこれ以下に間引く。 */
const MAX_FPS = 12;

export interface LiveDrawStats { fps: number; lastDrawMs: number; points: number }

/**
 * 実行中の軽い描画経路。レーンの構成（信号・色・テーマ・大きさ）が変わったときだけ option を作り直し、
 * それ以外は requestAnimationFrame で記録の変化を見て、直近 N 点の series.data と x 軸範囲だけを差し替える。
 */
export function LiveChart() {
  const ref = useRef<HTMLDivElement>(null);
  const chart = useRef<echarts.ECharts | null>(null);
  const lastRev = useRef(-1);
  const [stats, setStats] = useState<LiveDrawStats>({ fps: 0, lastDrawMs: 0, points: 0 });
  const { watch, windowSize, periodMs } = useLiveStore(useShallow((s) => ({
    watch: s.watch, windowSize: s.windowSize, periodMs: s.sim.periodMs,
  })));
  const { settings, theme, lsb, offset, mode, showAlias } = useWaveformStore(useShallow((s) => ({
    settings: s.signalSettings, theme: s.theme, lsb: s.lsbMapping, offset: s.offsetMapping,
    mode: s.valueDisplayMode, showAlias: s.showAlias,
  })));
  const plotted = useMemo(() => watch.filter((w) => w.plot).map((w) => w.name), [watch]);
  const [size, setSize] = useState({ w: 0, h: 0 });

  useEffect(() => {
    if (!ref.current) return;
    const ro = new ResizeObserver(() => {
      if (ref.current) setSize({ w: ref.current.clientWidth, h: ref.current.clientHeight });
    });
    ro.observe(ref.current);
    return () => ro.disconnect();
  }, []);

  // レーン構成
  const layout = useMemo(() => {
    const n = Math.max(1, plotted.length);
    const top = 10, bottom = 28, gap = 14;
    const h = Math.max(1, size.h - top - bottom);
    const lane = Math.max(20, (h - gap * (n - 1)) / n);
    return plotted.map((sig, i) => ({
      sig,
      top: top + i * (lane + gap),
      height: lane,
      color: settings[sig]?.color ?? "#3b82f6",
      label: (showAlias && settings[sig]?.alias) || sig,
      conv: getDisplayConversion(sig, lsb, offset, mode),
      yMin: settings[sig] && !settings[sig].yAuto ? settings[sig].yMin : null,
      yMax: settings[sig] && !settings[sig].yAuto ? settings[sig].yMax : null,
      lineWidth: settings[sig]?.lineWidth ?? 1.5,
    }));
  }, [plotted, size.h, settings, showAlias, lsb, offset, mode]);

  useEffect(() => {
    if (!ref.current) return;
    chart.current?.dispose();
    const c = echarts.init(ref.current, theme === "dark" ? "dark" : undefined, { renderer: "canvas" });
    chart.current = c;
    return () => { c.dispose(); if (chart.current === c) chart.current = null; };
  }, [theme]);

  useEffect(() => {
    const c = chart.current;
    if (!c) return;
    c.resize();
    const dark = theme === "dark";
    const axisColor = dark ? "#94a3b8" : "#64748b";
    const splitColor = dark ? "#334155" : "#e5e7eb";
    const left = 150, right = 20;
    c.setOption({
      backgroundColor: "transparent",
      animation: false,
      grid: layout.map((l) => ({ left, right, top: l.top, height: l.height, containLabel: false })),
      xAxis: layout.map((_, i) => ({
        type: "value", gridIndex: i, scale: true,
        axisLabel: { show: i === layout.length - 1, color: axisColor, fontSize: 10, formatter: (v: number) => `${Number(v.toPrecision(10))}` },
        axisTick: { show: i === layout.length - 1 },
        axisLine: { lineStyle: { color: splitColor } },
        splitLine: { show: true, lineStyle: { color: splitColor } },
        name: i === layout.length - 1 ? "ms" : undefined, nameGap: 4, nameTextStyle: { color: axisColor, fontSize: 10 },
      })),
      yAxis: layout.map((l, i) => ({
        type: "value", gridIndex: i, scale: true,
        min: l.yMin ?? undefined, max: l.yMax ?? undefined,
        name: l.label, nameLocation: "middle", nameRotate: 0, nameGap: 60,
        nameTextStyle: { color: l.color, fontSize: 11, fontWeight: "bold", width: 80, overflow: "truncate", align: "right" },
        axisLabel: { color: axisColor, fontSize: 10 },
        splitNumber: 2,
        splitLine: { lineStyle: { color: splitColor } },
      })),
      series: layout.map((l, i) => ({
        id: l.sig, name: l.sig, type: "line", xAxisIndex: i, yAxisIndex: i,
        step: "end", showSymbol: false, symbol: "none", sampling: "lttb", silent: true,
        lineStyle: { color: l.color, width: l.lineWidth }, itemStyle: { color: l.color },
        data: [],
      })),
      tooltip: { show: false },
    }, true);
    lastRev.current = -1; // 構成を変えたら必ず描き直す
  }, [layout, theme, size.w]);

  const layoutRef = useRef(layout);
  layoutRef.current = layout;
  const paramsRef = useRef({ windowSize, periodMs });
  paramsRef.current = { windowSize, periodMs };

  useEffect(() => {
    let raf = 0;
    let lastDraw = 0;
    let frames = 0;
    let fpsSince = performance.now();
    let drawMs = 0;
    let points = 0;
    const tick = (now: number) => {
      raf = requestAnimationFrame(tick);
      const c = chart.current;
      if (!c) return;
      if (now - lastDraw < 1000 / MAX_FPS - 1) return;
      // 非表示から表示に戻った直後など、ResizeObserver より先に描くときは大きさを合わせ直す
      const el = ref.current;
      if (el && el.clientWidth > 0 && (el.clientWidth !== c.getWidth() || el.clientHeight !== c.getHeight())) {
        c.resize();
        setSize((prev) => (prev.w === el.clientWidth && prev.h === el.clientHeight ? prev : { w: el.clientWidth, h: el.clientHeight }));
      }
      if (now - fpsSince >= 1000) {
        setStats({ fps: Math.round((frames * 1000) / (now - fpsSince)), lastDrawMs: Number(drawMs.toFixed(1)), points });
        frames = 0;
        fpsSince = now;
      }
      if (liveRecord.revision === lastRev.current) return;
      lastRev.current = liveRecord.revision;
      lastDraw = now;
      const t0 = performance.now();
      const { windowSize: n, periodMs: p } = paramsRef.current;
      const lanes = layoutRef.current;
      const data = buildLiveSeriesData(liveRecord, lanes, n, p);
      c.setOption({
        xAxis: lanes.map(() => ({ min: data.xMin, max: data.xMax })),
        series: lanes.map((l, i) => ({ id: l.sig, data: data.series[i] })),
      }, { lazyUpdate: false, silent: true });
      drawMs = performance.now() - t0;
      points = data.points;
      frames++;
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, []);

  useEffect(() => {
    (window as unknown as { __simLiveStats?: LiveDrawStats }).__simLiveStats = stats;
  }, [stats]);

  return (
    <div className="relative w-full h-full">
      <div ref={ref} className="w-full h-full rounded bg-white dark:bg-slate-900" />
      {plotted.length === 0 && (
        <div className="absolute inset-0 flex items-center justify-center text-sm text-gray-400 pointer-events-none">
          変数モニタで「波形」に印を付けた変数がここに流れます
        </div>
      )}
      <div className="absolute top-1 right-2 text-[10px] font-mono text-gray-400 pointer-events-none" title="ライブ描画の実測（1 秒ごと）">
        ライブ表示 直近 {windowSize} 点 / {stats.fps} fps / {stats.lastDrawMs} ms
      </div>
    </div>
  );
}
