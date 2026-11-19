import { useEffect, useMemo, useRef, useState, useSyncExternalStore } from "react";
import { useShallow } from "zustand/react/shallow";
import { useWaveformStore } from "../../store/useWaveformStore";
import { useLiveStore } from "../../live/liveStore";
import { liveRecord } from "../../live/liveRecord";
import { drawLive } from "../../live/liveCanvas";
import { leftMargin, rightMargin, rightAxisWidth } from "../../utils/laneLabels";
import { buildLiveLanes, fitLiveLabels } from "../../live/liveLayout";
import { useCompareView } from "../../hooks/useCompareView";

/** 描画の上限（fps）。samples は約 50ms ごとに届く。描画は記録が変わったときだけ行う。 */
const MAX_FPS = 30;

export interface LiveDrawStats { fps: number; lastDrawMs: number; points: number }

/**
 * 実行中の軽い描画経路。requestAnimationFrame で記録の変化を見て、直近 N 点を Canvas 2D に直接描く
 * （描き方と、ECharts を使わない理由は live/liveCanvas.ts）。
 */
export function LiveChart() {
  const ref = useRef<HTMLCanvasElement>(null);
  const lastRev = useRef(-1);
  const [stats, setStats] = useState<LiveDrawStats>({ fps: 0, lastDrawMs: 0, points: 0 });
  const { windowSize, periodMs } = useLiveStore(useShallow((s) => ({
    windowSize: s.windowSize, periodMs: s.sim.periodMs,
  })));
  const state = useWaveformStore(useShallow(s => ({
    modeMapping: s.modeMapping, showMode: s.showMode, unitMapping: s.unitMapping, showSignalNames: s.showSignalNames,
    signalSettings: s.signalSettings, theme: s.theme, lsbMapping: s.lsbMapping, offsetMapping: s.offsetMapping,
    valueDisplayMode: s.valueDisplayMode, showAlias: s.showAlias, signalGroups: s.signalGroups,
    unassignedSignals: s.unassignedSignals, unassignedGroupConfig: s.unassignedGroupConfig, layoutMode: s.layoutMode,
  })));
  const { theme, showMode } = state;
  const view = useCompareView();
  const names = useSyncExternalStore(liveRecord.subscribeNames, liveRecord.getNames);
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
  const rawLayout = useMemo(() => buildLiveLanes(state, names, size.h, view.visible), [state, names, size.h, view]);
  const { layout, titleWidth } = useMemo(() => fitLiveLabels(rawLayout, size.w), [rawLayout, size.w]);

  const margins = useMemo(() => ({
    titleWidth,
    rightAxes: Math.max(0, ...layout.map(lane => lane.syncRange === false ? lane.series.length - 1 : 0)),
  }), [layout, titleWidth]);
  const tickCharacters = useRef(0);
  const rightTickCharacters = useRef(0);
  const layoutRef = useRef(layout);
  layoutRef.current = layout;
  const paramsRef = useRef({ windowSize, periodMs, theme, size, showMode, margins, showSignalNames: state.showSignalNames });
  paramsRef.current = { windowSize, periodMs, theme, size, showMode, margins, showSignalNames: state.showSignalNames };

  // 構成（レーン・テーマ・大きさ）が変わったら必ず描き直す
  useEffect(() => { lastRev.current = -1; tickCharacters.current = 0; rightTickCharacters.current = 0; }, [layout, theme, showMode, size.w, size.h]);

  useEffect(() => {
    let raf = 0;
    let lastDraw = 0;
    let frames = 0;
    let fpsSince = performance.now();
    let drawMs = 0;
    let points = 0;
    const tick = (now: number) => {
      raf = requestAnimationFrame(tick);
      const cv = ref.current;
      if (!cv) return;
      if (now - fpsSince >= 1000) {
        setStats({ fps: Math.round((frames * 1000) / (now - fpsSince)), lastDrawMs: Number(drawMs.toFixed(1)), points });
        frames = 0;
        fpsSince = now;
      }
      if (now - lastDraw < 1000 / MAX_FPS - 1) return;
      // 非表示から表示に戻った直後など、ResizeObserver より先に描くときは大きさを合わせ直す
      const w = cv.clientWidth, h = cv.clientHeight;
      if (w <= 0 || h <= 0) return;
      const dpr = window.devicePixelRatio || 1;
      const bw = Math.round(w * dpr), bh = Math.round(h * dpr);
      let resized = false;
      if (cv.width !== bw || cv.height !== bh) {
        cv.width = bw;
        cv.height = bh;
        resized = true;
        setSize((prev) => (prev.w === w && prev.h === h ? prev : { w, h }));
      }
      if (!resized && liveRecord.revision === lastRev.current) return;
      const ctx = cv.getContext("2d");
      if (!ctx) return;
      lastRev.current = liveRecord.revision;
      lastDraw = now;
      const t0 = performance.now();
      const { windowSize: n, periodMs: p, theme: th, showMode: mode, margins: m, showSignalNames } = paramsRef.current;
      const dark = th === "dark";
      const res = drawLive(ctx, w, h, dpr, liveRecord, layoutRef.current, n, p, {
        dark,
        stateOpacity: dark ? 0.3 : 0.15,
        axisColor: dark ? "#94a3b8" : "#64748b",
        splitColor: dark ? "#334155" : "#e5e7eb",
        left: leftMargin(showSignalNames, m.titleWidth, tickCharacters.current),
        right: rightMargin(m.rightAxes, rightTickCharacters.current),
        rightAxisWidth: rightAxisWidth(rightTickCharacters.current),
      }, mode);
      if (res.maxTickCharacters > tickCharacters.current) {
        tickCharacters.current = res.maxTickCharacters;
        lastRev.current = -1;
      }
      if (res.maxRightTickCharacters > rightTickCharacters.current) {
        rightTickCharacters.current = res.maxRightTickCharacters;
        lastRev.current = -1;
      }
      drawMs = performance.now() - t0;
      points = res.points;
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
      <canvas ref={ref} className="block w-full h-full rounded bg-white dark:bg-slate-900" />
      {layout.length === 0 && (
        <div className="absolute inset-0 flex items-center justify-center text-sm text-gray-400 pointer-events-none">
          左の信号設定でチェックした信号が、実行中ここに流れます
        </div>
      )}
      <div className="absolute top-1 right-2 text-[10px] font-mono text-gray-400 pointer-events-none" title="ライブ描画の実測（1 秒ごと）">
        ライブ表示 直近 {windowSize} 点 / {stats.fps} fps / {stats.lastDrawMs} ms
      </div>
    </div>
  );
}
