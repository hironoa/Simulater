import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { dispatchStreamEvent, type EventSourceLike, type StreamHandlers } from "./stream";
import { createApi } from "./api";
import { createLiveController } from "./controller";
import { LiveRecord } from "./liveRecord";
import { initialLiveState, useLiveStore } from "./liveStore";
import { useWaveformStore, LIVE_FILE_NAME } from "../store/useWaveformStore";

function handlers() {
  return { hello: vi.fn(), samples: vi.fn(), state: vi.fn(), values: vi.fn(), error: vi.fn() } satisfies StreamHandlers;
}

describe("dispatchStreamEvent", () => {
  it("hello / samples / state を検証して渡す", () => {
    const h = handlers();
    expect(dispatchStreamEvent("hello", '{"periodMs":5,"func":"f","names":["a"]}', h)).toBe(true);
    expect(h.hello).toHaveBeenCalledWith({ periodMs: 5, func: "f", names: ["a"] });
    dispatchStreamEvent("samples", '{"step0":10,"names":["a","b"],"rows":[[1,null],[2,"x"]]}', h);
    expect(h.samples).toHaveBeenCalledWith({ step0: 10, names: ["a", "b"], rows: [[1, null], [2, null]] });
    dispatchStreamEvent("state", '{"running":true,"step":3,"speed":0,"holds":[{"name":"x","value":1}],"overrun":false}', h);
    expect(h.state).toHaveBeenCalledWith({ running: true, step: 3, speed: 0, holds: [{ name: "x", value: 1 }], overrun: false });
  });

  it("values を検証して渡し、形式が違えば捨てる", () => {
    const h = handlers();
    expect(dispatchStreamEvent("values", '{"step":4,"names":["a","b"],"values":[0,"x"]}', h)).toBe(true);
    expect(h.values).toHaveBeenCalledWith({ step: 4, names: ["a", "b"], values: [0, null] });
    expect(dispatchStreamEvent("values", '{"names":["a"],"values":[1]}', h)).toBe(false);
    expect(dispatchStreamEvent("values", '{"step":1,"names":["a"],"values":[]}', h)).toBe(false);
    expect(dispatchStreamEvent("values", '{"step":1,"names":"a","values":[1]}', h)).toBe(false);
    expect(dispatchStreamEvent("values", "[1]", h)).toBe(false);
    expect(h.values).toHaveBeenCalledTimes(1);
  });

  it("壊れたメッセージや未知のイベントは捨てる", () => {
    const h = handlers();
    expect(dispatchStreamEvent("samples", "{", h)).toBe(false);
    expect(dispatchStreamEvent("samples", '{"names":[],"rows":[]}', h)).toBe(false);
    expect(dispatchStreamEvent("other", "{}", h)).toBe(false);
    expect(h.samples).not.toHaveBeenCalled();
    expect(h.error).toHaveBeenCalledTimes(2);
  });
});

class FakeEventSource implements EventSourceLike {
  static last: FakeEventSource | null = null;
  listeners = new Map<string, (ev: MessageEvent) => void>();
  onopen: ((ev: Event) => void) | null = null;
  onerror: ((ev: Event) => void) | null = null;
  closed = false;
  constructor(public url: string) { FakeEventSource.last = this; }
  addEventListener(type: string, l: (ev: MessageEvent) => void) { this.listeners.set(type, l); }
  close() { this.closed = true; }
  emit(type: string, data: unknown) {
    this.listeners.get(type)?.({ data: JSON.stringify(data) } as MessageEvent);
  }
}

describe("ライブ制御（SSE の取り込み）", () => {
  const info = { version: "t", periodMs: 2, func: "main", functions: ["main", "sub"], state: { running: true, step: 0, speed: 1, holds: [], overrun: false } };
  let posted: { url: string; body: unknown }[];
  let record: LiveRecord;

  beforeEach(() => {
    vi.useFakeTimers();
    useLiveStore.setState({ ...initialLiveState });
    useWaveformStore.setState({ chartData: null, signalSettings: {}, unassignedSignals: [], signalGroups: [], cursors: [], fileName: null });
    posted = [];
    record = new LiveRecord();
  });
  afterEach(() => vi.useRealTimers());

  function makeController() {
    const fetchImpl = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = String(input);
      if (init?.method === "POST") posted.push({ url, body: JSON.parse(String(init.body)) });
      if (url.endsWith("/api/info")) return new Response(JSON.stringify(info));
      if (url.endsWith("/api/watch")) return new Response(JSON.stringify({ errors: [{ name: "zz", message: "見つかりません" }] }));
      return new Response("{}");
    });
    return createLiveController({
      api: createApi("", fetchImpl as unknown as typeof fetch),
      record,
      protocol: "http:",
      createEventSource: (u) => new FakeEventSource(u),
      notify: { error: vi.fn(), success: vi.fn() },
    });
  }

  it("起動するとライブモードになり、監視一覧を送り、ストリームを張る", async () => {
    const live = makeController();
    useLiveStore.setState({ watch: [] });
    expect(await live.start()).toBe(true);
    expect(useLiveStore.getState().mode).toBe("live");
    expect(useLiveStore.getState().sim).toMatchObject({ running: true, periodMs: 2, func: "main" });
    expect(useWaveformStore.getState().taskPeriod).toBe(2);
    expect(FakeEventSource.last?.url).toBe("/api/stream");
    live.addWatch("a");
    live.addWatch("zz");
    await vi.runOnlyPendingTimersAsync();
    expect(posted.at(-1)).toEqual({ url: "/api/watch", body: { names: ["a", "zz"] } });
    expect(useLiveStore.getState().watchErrors).toEqual({ zz: "見つかりません" });
    expect(useWaveformStore.getState().signalSettings.a?.checked).toBe(true);
    live.stop();
    expect(FakeEventSource.last?.closed).toBe(true);
  });

  it("samples を記録に追記し、最新値をまとめて反映し、停止で Viewer に載せる", async () => {
    const live = makeController();
    await live.start();
    live.addWatch("a");
    live.addWatch("b", false);
    const es = FakeEventSource.last!;
    es.onopen?.(new Event("open"));
    expect(useLiveStore.getState().connected).toBe(true);
    es.emit("hello", { periodMs: 2, func: "main", names: ["a", "b"] });
    es.emit("samples", { step0: 0, names: ["a", "b"], rows: [[1, 10], [2, 20]] });
    es.emit("samples", { step0: 2, names: ["a", "b"], rows: [[3, null]] });
    expect(record.length).toBe(3);
    expect(useLiveStore.getState().sim.step).toBe(2);
    expect(useLiveStore.getState().values).toEqual({});
    live.flushValues();
    expect(useLiveStore.getState().values).toEqual({ a: 3, b: null });
    // 実行中は Viewer の chartData を触らない
    expect(useWaveformStore.getState().chartData).toBeNull();

    es.emit("state", { running: false, step: 2 });
    await vi.advanceTimersByTimeAsync(300);
    const wf = useWaveformStore.getState();
    expect(wf.fileName).toBe(LIVE_FILE_NAME);
    expect(wf.chartData?.steps).toEqual([1, 2, 3]);
    expect(wf.chartData?.seriesMap.a).toEqual([1, 2, 3]);
    expect(wf.chartData?.seriesMap.b).toEqual([10, 20, null]);
    expect(wf.signalSettings.b?.checked).toBe(false);
    live.stop();
  });

  it("停止中に届いた samples（ステップ実行）も少し待って Viewer に載せ直す", async () => {
    const live = makeController();
    await live.start();
    const es = FakeEventSource.last!;
    es.emit("state", { running: false });
    es.emit("samples", { step0: 0, names: ["a"], rows: [[5]] });
    await vi.advanceTimersByTimeAsync(300);
    expect(useWaveformStore.getState().chartData?.seriesMap.a).toEqual([5]);
    es.emit("samples", { step0: 1, names: ["a"], rows: [[6]] });
    await vi.advanceTimersByTimeAsync(300);
    expect(useWaveformStore.getState().chartData?.seriesMap.a).toEqual([5, 6]);
    live.stop();
  });

  it("values は現在値だけを更新し、記録・波形・ステップ番号は変えない", async () => {
    const live = makeController();
    await live.start();
    const es = FakeEventSource.last!;
    es.emit("state", { running: false, step: 7 });
    es.emit("samples", { step0: 0, names: ["a"], rows: [[1]] });
    live.flushValues();
    expect(useLiveStore.getState().values).toEqual({ a: 1 });
    const stepBefore = useLiveStore.getState().sim.step;
    es.emit("values", { step: 1, names: ["a", "b"], values: [0, 42] });
    es.emit("values", { step: 1, names: ["a"], values: ["bad"] }); // 読めない値は null
    es.emit("values", { names: ["a"], values: [9] }); // 形式違いは無視
    await vi.advanceTimersByTimeAsync(300);
    expect(useLiveStore.getState().values).toEqual({ a: null, b: 42 });
    expect(record.length).toBe(1);
    expect(useLiveStore.getState().sim.step).toBe(stepBefore);
    expect(useWaveformStore.getState().chartData?.seriesMap.a).toEqual([1]);
    live.stop();
  });

  it("リセットで記録を消す", async () => {
    const live = makeController();
    await live.start();
    FakeEventSource.last!.emit("samples", { step0: 0, names: ["a"], rows: [[5]] });
    live.commit();
    expect(useWaveformStore.getState().chartData).not.toBeNull();
    await live.control({ cmd: "reset" });
    expect(posted.at(-1)).toEqual({ url: "/api/control", body: { cmd: "reset" } });
    expect(record.length).toBe(0);
    expect(useWaveformStore.getState().chartData).toBeNull();
    live.stop();
  });
});

describe("useWaveformStore.appendSamples", () => {
  it("ストア経由で共有のライブ記録に追記する", async () => {
    const { liveRecord } = await import("./liveRecord");
    liveRecord.clear();
    const res = useWaveformStore.getState().appendSamples(0, ["s"], [[1], [2]]);
    expect(res.appended).toBe(2);
    expect(liveRecord.latest("s")).toBe(2);
    useWaveformStore.getState().appendSamples(2, ["s"], [[3], [4], [5]]);
    useWaveformStore.getState().commitLiveRecord({ windowSize: 3 });
    expect(useWaveformStore.getState().chartData?.seriesMap.s).toEqual([1, 2, 3, 4, 5]);
    // 一時停止した直後は、直前まで見ていた直近の窓（3 点）を表示する
    expect(useWaveformStore.getState().zoomRange).toMatchObject({ start: 50, end: 100 });
    liveRecord.clear();
  });

  it("載せ直しても、カーソルは同じ時刻を指す（古い側を捨てて添字がずれても）", async () => {
    const { LiveRecord } = await import("./liveRecord");
    const r = new LiveRecord(1000);
    r.append(0, ["s"], Array.from({ length: 1000 }, (_, i) => [i]));
    useWaveformStore.setState({ taskPeriod: 2, fileName: null, cursors: [] });
    useWaveformStore.getState().commitLiveRecord({ record: r });
    useWaveformStore.setState({ cursors: [{ time: 2 * 950, index: 950 }] });
    r.append(1000, ["s"], [[1000]]); // 上限を超えて先頭 101 点が捨てられる
    useWaveformStore.getState().commitLiveRecord({ record: r });
    const c = useWaveformStore.getState().cursors;
    expect(c).toEqual([{ time: 1900, index: 849 }]);
    expect(useWaveformStore.getState().chartData!.seriesMap.s[849]).toBe(950);
  });
});
