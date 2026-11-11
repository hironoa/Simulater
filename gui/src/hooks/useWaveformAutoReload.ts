import { useCallback, useEffect, useRef } from "react";
import toast from "react-hot-toast";
import { useWaveformStore, type WaveformFileHandle } from "../store/useWaveformStore";
import { classifyReload } from "../utils/classifyReload";

type FileVersion = { lastModified: number; size: number };

const sameVersion = (a: FileVersion | null, b: FileVersion) =>
  a?.lastModified === b.lastModified && a?.size === b.size;

export function useWaveformAutoReload() {
  const handle = useWaveformStore((state) => state.waveformHandle);
  const autoReload = useWaveformStore((state) => state.autoReload);
  const busy = useRef(false);
  const mounted = useRef(false);
  const observed = useRef<{
    handle: WaveformFileHandle;
    sourceVersion: FileVersion | null;
    last: FileVersion | null;
    pending: FileVersion | null;
  } | null>(null);

  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  const read = useCallback(async (manual: boolean) => {
    const state = useWaveformStore.getState();
    // Only the current B handle is watched.
    const currentHandle = state.waveformHandle;
    if (!currentHandle || busy.current || (!manual && (!state.autoReload || document.hidden))) return;

    const isCurrent = () => {
      const latest = useWaveformStore.getState();
      return mounted.current && latest.waveformHandle === currentHandle &&
        latest.waveformFileVersion === state.waveformFileVersion &&
        (manual || (latest.autoReload && !document.hidden));
    };

    busy.current = true;
    try {
      let permission = await currentHandle.queryPermission({ mode: "read" });
      if (!isCurrent()) return;
      if (permission !== "granted" && manual) {
        permission = await currentHandle.requestPermission({ mode: "read" });
      }
      if (!isCurrent()) return;
      if (permission !== "granted") {
        state.setAutoReload(false);
        toast.error("読み取り権限がありません。再読み込みボタンで権限を許可してください。", { id: "waveform-reload" });
        return;
      }

      const file = await currentHandle.getFile();
      if (!isCurrent()) return;
      if (observed.current?.handle !== currentHandle || observed.current.sourceVersion !== state.waveformFileVersion) {
        observed.current = {
          handle: currentHandle,
          sourceVersion: state.waveformFileVersion,
          last: state.waveformFileVersion,
          pending: null,
        };
      }
      const observation = observed.current;
      const version = { lastModified: file.lastModified, size: file.size };
      if (!manual) {
        if (sameVersion(observation.last, version)) {
          observation.pending = null;
          return;
        }
        if (!sameVersion(observation.pending, version)) {
          observation.pending = version;
          return;
        }
      }

      const text = await file.text();
      if (!isCurrent()) return;
      const latest = useWaveformStore.getState();
      const kind = classifyReload(latest.rawCSVText ?? "", text);
      if (kind === "incomplete") return;
      if (latest.keepPreviousAsA && kind === "restart" && latest.promoteBToA()) {
        toast.success("前回の結果を A にしました");
      }
      if (latest.parseWaveformCSV(text, file.name, { preserveView: true })) {
        useWaveformStore.setState({ waveformFileVersion: version });
        observation.sourceVersion = version;
      }
      // 読み込んだ版を記録し、次の変更を待つ
      observation.last = version;
      observation.pending = null;
    } catch (error) {
      if (!isCurrent()) return;
      state.setAutoReload(false);
      toast.error("波形CSVを読み込めませんでした。ファイルの場所と読み取り権限を確認してください。", { id: "waveform-reload" });
    } finally {
      busy.current = false;
    }
  }, []);

  useEffect(() => {
    if (!handle || !autoReload) return;
    let timer: ReturnType<typeof setInterval> | undefined;
    const updateTimer = () => {
      if (timer !== undefined) clearInterval(timer);
      timer = undefined;
      if (observed.current) observed.current.pending = null;
      if (!document.hidden) timer = setInterval(() => void read(false), 1500);
    };
    updateTimer();
    document.addEventListener("visibilitychange", updateTimer);
    return () => {
      if (timer !== undefined) clearInterval(timer);
      document.removeEventListener("visibilitychange", updateTimer);
      if (observed.current) observed.current.pending = null;
    };
  }, [handle, autoReload, read]);

  const reload = useCallback(() => read(true), [read]);
  return { reload };
}
