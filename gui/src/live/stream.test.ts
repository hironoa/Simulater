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
    expect(h.state).toHaveBeenCalledWith({ running: true, step: 3, speed: 0, holds: [{ name: "x", value: 1 }], overrun: false, dropped: [] });
  });

  it("state の dropped を読む（無ければ空、不正な項目は捨て、配列でなければ無視）", () => {
    const h = handlers();
    dispatchStreamEvent("state", JSON.stringify({
      func: "mul16",
      dropped: [
        { name: "@rpm", kind: "hold", message: "引数がありません" },
        { name: "@rpm", kind: "watch" },
        { name: "x", kind: "other", message: "m" },
        { kind: "hold", message: "名前なし" },
        "bad",
      ],
    }), h);
    expect(h.state).toHaveBeenLastCalledWith({
      func: "mul16",
      dropped: [
        { name: "@rpm", kind: "hold", message: "引数がありません" },
        { name: "@rpm", kind: "watch", message: "" },
      ],
    });
    dispatchStreamEvent("state", '{"step":1,"dropped":"x"}', h);
    expect(h.state).toHaveBeenLastCalledWith({ step: 1 });
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
  /** /api/control の応答（state と同じ形） */
  let controlResponse: unknown;
  let notify: { error: ReturnType<typeof vi.fn>; success: ReturnType<typeof vi.fn>; warn: ReturnType<typeof vi.fn> };

  beforeEach(() => {
    vi.useFakeTimers();
    useLiveStore.setState({ ...initialLiveState });
    useWaveformStore.setState({ chartData: null, signalSettings: {}, unassignedSignals: [], signalGroups: [], cursors: [], fileName: null });
    posted = [];
    record = new LiveRecord();
    controlResponse = {};
    notify = { error: vi.fn(), success: vi.fn(), warn: vi.fn() };
    const mem = new Map<string, string>();
    vi.stubGlobal("localStorage", {
      getItem: (k: string) => mem.get(k) ?? null,
      setItem: (k: string, v: string) => { mem.set(k, v); },
      removeItem: (k: string) => { mem.delete(k); },
      clear: () => mem.clear(),
    });
  });
  afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });

  function makeController() {
    const fetchImpl = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = String(input);
      if (init?.method === "POST") posted.push({ url, body: JSON.parse(String(init.body)) });
      if (url.endsWith("/api/info")) return new Response(JSON.stringify(info));
      if (url.endsWith("/api/watch")) {
        const names = (JSON.parse(String(init?.body)) as { names: string[] }).names;
        const errors = names.filter((n) => n === "zz" || n.startsWith("@")).map((name) => ({ name, message: "見つかりません" }));
        return new Response(JSON.stringify({ errors }));
      }
      if (url.endsWith("/api/control")) return new Response(JSON.stringify(controlResponse));
      return new Response("{}");
    });
    return createLiveController({
      api: createApi("", fetchImpl as unknown as typeof fetch),
      record,
      protocol: "http:",
      createEventSource: (u) => new FakeEventSource(u),
      notify,
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

  const droppedRpm = [
    { name: "@rpm", kind: "hold", message: "引数 '@rpm' は関数 mul16 にありません" },
    { name: "@rpm", kind: "watch", message: "引数 '@rpm' は関数 mul16 にありません" },
    { name: "@gear", kind: "watch", message: "引数 '@gear' は関数 mul16 にありません" },
  ];

  /** eng_judge_gear で @rpm を入力（固定）・@gear を監視にしてから始める */
  async function startWithArgs() {
    info.func = "eng_judge_gear";
    info.state = { running: false, step: 0, speed: 1, holds: [{ name: "@rpm", value: 3000 }] as never[], overrun: false };
    localStorage.setItem("sim-gui:layout:v1", JSON.stringify({
      inputs: [{ name: "@rpm", widget: "number", min: 0, max: 100, step: 1, hold: true, value: 3000 }],
      watch: [{ name: "@gear", plot: true, radix: "dec" }, { name: "g_out", plot: true, radix: "dec" }],
    }));
    const live = makeController();
    await live.start();
    return live;
  }
  afterEach(() => {
    info.func = "main";
    info.state = { running: true, step: 0, speed: 1, holds: [], overrun: false };
  });

  it("関数を変えた応答の dropped で固定を OFF にし、監視から外し、保存した構成にも反映する", async () => {
    const live = await startWithArgs();
    expect(useLiveStore.getState().inputs[0]).toMatchObject({ name: "@rpm", hold: true });
    controlResponse = { running: false, step: 0, func: "mul16", holds: [], dropped: droppedRpm };
    await live.control({ func: "mul16" });
    const s = useLiveStore.getState();
    expect(s.sim.func).toBe("mul16");
    expect(s.inputs[0]).toMatchObject({ name: "@rpm", hold: false, monitor: false });
    expect(s.watch.map((w) => w.name)).toEqual(["g_out"]);
    const saved = JSON.parse(localStorage.getItem("sim-gui:layout:v1")!);
    expect(saved.inputs[0]).toMatchObject({ name: "@rpm", hold: false, monitor: false });
    expect(saved.watch.map((w: { name: string }) => w.name)).toEqual(["g_out"]);
    // 送り直しても外した名前は送らない（errors にならない）
    await live.syncWatch();
    expect(posted.at(-1)).toEqual({ url: "/api/watch", body: { names: ["g_out"] } });
    expect(useLiveStore.getState().watchErrors).toEqual({});
    // 入力に追加し直すと監視に戻る
    live.addInput("@rpm");
    await vi.runOnlyPendingTimersAsync();
    expect(useLiveStore.getState().inputs[0].monitor).toBeUndefined();
    expect(posted.at(-1)).toEqual({ url: "/api/watch", body: { names: ["g_out", "@rpm"] } });
    live.stop();
  });

  it("同じ dropped を応答と SSE の両方で受けても通知は 1 回だけ", async () => {
    const live = await startWithArgs();
    const es = FakeEventSource.last!;
    controlResponse = { func: "mul16", holds: [], dropped: droppedRpm };
    await live.control({ func: "mul16" });
    es.emit("state", { func: "mul16", holds: [], dropped: droppedRpm });
    es.emit("state", { func: "mul16", holds: [], dropped: droppedRpm });
    expect(notify.warn).toHaveBeenCalledTimes(1);
    const text = notify.warn.mock.calls[0][0] as string;
    expect(text).toContain("@rpm（固定）: 引数 '@rpm' は関数 mul16 にありません");
    expect(text).toContain("@gear（監視）");
    // 外したあとに利用者が監視へ戻しても、1 秒ごとに届く同じ dropped では外し直さない
    live.addWatch("g_new");
    es.emit("state", { func: "mul16", holds: [], dropped: droppedRpm });
    expect(useLiveStore.getState().watch.map((w) => w.name)).toEqual(["g_out", "g_new"]);
    // 次の操作で空に戻り、また同じものを外したら改めて知らせる
    controlResponse = { func: "eng_judge_gear", holds: [], dropped: [] };
    await live.control({ func: "eng_judge_gear" });
    es.emit("state", { func: "eng_judge_gear", holds: [], dropped: [] });
    live.addWatch("@gear");
    controlResponse = { func: "mul16", holds: [], dropped: [droppedRpm[2]] };
    await live.control({ func: "mul16" });
    expect(notify.warn).toHaveBeenCalledTimes(2);
    live.stop();
  });

  it("SSE だけで届いた dropped（テスト CSV の読み込みなど）も反映して 1 回知らせる", async () => {
    const live = await startWithArgs();
    const es = FakeEventSource.last!;
    es.emit("state", { func: "mul16", holds: [], dropped: droppedRpm });
    es.emit("state", { func: "mul16", holds: [], dropped: droppedRpm });
    expect(notify.warn).toHaveBeenCalledTimes(1);
    expect(useLiveStore.getState().inputs[0]).toMatchObject({ hold: false, monitor: false });
    expect(useLiveStore.getState().watch.map((w) => w.name)).toEqual(["g_out"]);
    live.stop();
  });

  it("固定の表示はサーバの state.holds に合わせる（押している間だけのボタンは除く）", async () => {
    const live = await startWithArgs();
    const es = FakeEventSource.last!;
    // サーバの固定が無くなった
    es.emit("state", { holds: [] });
    expect(useLiveStore.getState().inputs[0].hold).toBe(false);
    // ボタンで一時的に固定しても固定 ON の表示にはしない
    await live.write("@rpm", 1, true);
    es.emit("state", { holds: [{ name: "@rpm", value: 1 }] });
    expect(useLiveStore.getState().inputs[0].hold).toBe(false);
    await live.releaseButton("@rpm");
    es.emit("state", { holds: [] });
    expect(useLiveStore.getState().inputs[0].hold).toBe(false);
    // ほかから固定されたら ON にする
    es.emit("state", { holds: [{ name: "@rpm", value: 5 }] });
    expect(useLiveStore.getState().inputs[0].hold).toBe(true);
    live.stop();
  });

  it("起動時に固定がサーバに無ければ OFF で表示する", async () => {
    info.func = "eng_judge_gear";
    localStorage.setItem("sim-gui:layout:v1", JSON.stringify({ inputs: [{ name: "@rpm", widget: "number", hold: true }] }));
    const live = makeController();
    await live.start();
    expect(useLiveStore.getState().inputs[0].hold).toBe(false);
    live.stop();
  });

  it("停止中に関数だけ変えても、手元の記録とライブ波形を消す（1 回だけ）", async () => {
    const live = await startWithArgs();
    const es = FakeEventSource.last!;
    es.emit("samples", { step0: 0, names: ["g_out"], rows: [[1], [2]] });
    live.commit();
    expect(useWaveformStore.getState().chartData).not.toBeNull();
    controlResponse = { running: false, step: 0, func: "mul16", holds: [], dropped: [] };
    await live.control({ func: "mul16" });
    expect(posted.at(-1)).toEqual({ url: "/api/control", body: { func: "mul16" } });
    expect(record.length).toBe(0);
    expect(useWaveformStore.getState().chartData).toBeNull();
    // 新しい関数の記録は、同じ関数の state を受けても消さない
    es.emit("samples", { step0: 0, names: ["g_out"], rows: [[7]] });
    es.emit("state", { func: "mul16", running: false });
    expect(record.length).toBe(1);
    // 関数が変わらない応答では消さない
    controlResponse = { func: "mul16" };
    await live.control({ speed: 2 });
    expect(record.length).toBe(1);
    live.stop();
  });

  it("SSE で先に関数の変更を知っても記録を消す", async () => {
    const live = await startWithArgs();
    const es = FakeEventSource.last!;
    es.emit("samples", { step0: 0, names: ["g_out"], rows: [[1]] });
    es.emit("state", { func: "mul16" });
    expect(record.length).toBe(0);
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
