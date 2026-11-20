import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { dispatchStreamEvent, type EventSourceLike, type StreamHandlers } from "./stream";
import { ApiError, createApi, type SimApi } from "./api";
import { createLiveController } from "./controller";
import { saveVariants as saveVariantFile, useVariantsStore } from "./variantsStore";
import { DEFAULT_LAYOUT, loadLayout, saveLayout, type InputItem, type WatchItem } from "./persist";
import type { VariantsDoc } from "./variants";
import { liveRecord, LiveRecord } from "./liveRecord";
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
    expect(h.state).toHaveBeenCalledWith({ needReset: false, running: true, step: 3, speed: 0, holds: [{ name: "x", value: 1 }], overrun: false, dropped: [], pending: [] });
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
      needReset: false,
      func: "mul16",
      dropped: [
        { name: "@rpm", kind: "hold", message: "引数がありません" },
        { name: "@rpm", kind: "watch", message: "" },
      ],
      pending: [],
    });
    dispatchStreamEvent("state", '{"step":1,"dropped":"x"}', h);
    expect(h.state).toHaveBeenLastCalledWith({ needReset: false, step: 1, pending: [] });
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
  let responseGate: Promise<void> | null;
  /** /api/load-csv の応答（status と本文） */
  let loadCsvResponse: { status: number; body: unknown };
  /** /api/write の応答（null なら 200 の {}） */
  let writeResponse: { status: number; body: unknown } | null;
  let notify: { error: ReturnType<typeof vi.fn>; success: ReturnType<typeof vi.fn>; warn: ReturnType<typeof vi.fn> };

  beforeEach(() => {
    vi.useFakeTimers();
    useLiveStore.setState({ ...initialLiveState });
    useVariantsStore.setState(useVariantsStore.getInitialState());
    useWaveformStore.setState({ chartData: null, signalSettings: {}, unassignedSignals: [], signalGroups: [], cursors: [], fileName: null });
    posted = [];
    record = new LiveRecord();
    controlResponse = {};
    responseGate = null;
    loadCsvResponse = { status: 200, body: { rows: 0, names: [] } };
    writeResponse = null;
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

  function makeController(apiOverrides: Partial<SimApi> = {}, targetRecord = record) {
    const fetchImpl = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = String(input);
      if (init?.method === "POST") posted.push({ url, body: url.endsWith("/api/load-csv") ? init.body : JSON.parse(String(init.body)) });
      if (url.endsWith("/api/info")) return new Response(JSON.stringify(info));
      if (url.endsWith("/api/watch")) {
        const names = (JSON.parse(String(init?.body)) as { names: string[] }).names;
        const errors = names.filter((n) => n === "zz" || n.startsWith("@")).map((name) => ({ name, message: "見つかりません" }));
        return new Response(JSON.stringify({ errors }));
      }
      if (url.endsWith("/api/control") || url.endsWith("/api/load-csv")) await responseGate;
      if (url.endsWith("/api/control")) return new Response(JSON.stringify(controlResponse));
      if (url.endsWith("/api/load-csv")) return new Response(JSON.stringify(loadCsvResponse.body), { status: loadCsvResponse.status });
      if (url.endsWith("/api/write") && writeResponse) return new Response(JSON.stringify(writeResponse.body), { status: writeResponse.status });
      return new Response("{}");
    });
    return createLiveController({
      api: { ...createApi("", fetchImpl as unknown as typeof fetch), ...apiOverrides },
      record: targetRecord,
      protocol: "http:",
      createEventSource: (u) => new FakeEventSource(u),
      notify,
    });
  }

  describe("ライブの系列名", () => {
    it.each(["波形側", "変数モニタ側"])("%sで短い系列名の波形を非表示にすると監視設定に保存する", (source) => {
      const live = makeController();
      const name = "engine.c/s_cnt";
      live.addWatch(name);
      expect(useWaveformStore.getState().signalSettings.s_cnt.checked).toBe(true);
      expect(loadLayout().watch).toMatchObject([{ name, plot: true }]);

      if (source === "波形側") useWaveformStore.getState().toggleSignalCheck("s_cnt");
      else live.updateWatch(name, { plot: false });

      expect(useWaveformStore.getState().signalSettings.s_cnt.checked).toBe(false);
      expect(useLiveStore.getState().watch).toMatchObject([{ name, plot: false }]);
      expect(loadLayout().watch).toMatchObject([{ name, plot: false }]);
    });

    it.each(["個別", "共有"])("%sの記録では完全名をサーバに送り、設定と記録に短い系列名を使う", async (kind) => {
      const target = kind === "共有" ? liveRecord : record;
      target.clear();
      const live = makeController({}, target);
      const state = useWaveformStore.getState();
      const ensure = vi.spyOn(state, "ensureLiveSignals");
      const name = "engine.c/s_cnt";
      try {
        state.parseMappingData("version: 2\nsignals: {s_cnt: {unit: rpm}}\nview: {signals: {s_cnt: {weight: 3}}}\n", "yaml", false);
        live.addWatch(name);
        await live.syncWatch();
        live.handlers.hello({ names: [name] });
        live.handlers.samples({ step0: 1, names: [name], rows: [[123]] });
        live.flushValues();
        expect(posted.filter(p => p.url === "/api/watch").at(-1)?.body).toEqual({ names: [name] });
        expect(ensure).toHaveBeenCalledWith(["s_cnt"], new Set(["s_cnt"]));
        expect(useWaveformStore.getState().signalSettings.s_cnt.weight).toBe(3);
        expect(useWaveformStore.getState().unitMapping[live.seriesName(name)]).toBe("rpm");
        expect(target.names).toEqual(["s_cnt"]);
        expect(target.latest("s_cnt")).toBe(123);
        expect(useLiveStore.getState().values[name]).toBe(123);
      } finally {
        target.clear();
        ensure.mockRestore();
        useWaveformStore.setState(state);
      }
    });

    it("監視の追加と削除で系列名が変わっても、各変数の記録を引き継ぐ", () => {
      const live = makeController();
      live.addWatch("a.c/x");
      live.handlers.samples({ step0: 1, names: ["a.c/x"], rows: [[11]] });
      live.commit();
      live.addInput("b.c/x");
      expect(live.seriesName("a.c/x")).toBe("a.c/x");
      expect(live.seriesName("b.c/x")).toBe("b.c/x");
      expect(record.names).toEqual(["a.c/x"]);
      expect(useWaveformStore.getState().chartData?.seriesMap).toEqual({ "a.c/x": [11] });

      live.handlers.samples({ step0: 2, names: ["a.c/x", "b.c/x"], rows: [[12, 22]] });
      expect(record.toChartData().seriesMap).toEqual({ "a.c/x": [11, 12], "b.c/x": [null, 22] });

      live.removeWatch("a.c/x");
      live.handlers.samples({ step0: 3, names: ["a.c/x", "b.c/x"], rows: [[13, 23]] });
      expect(record.toChartData().seriesMap).toEqual({ "a.c/x": [11, 12, 13], x: [null, 22, 23] });
    });
  });

  describe("入力パネルの並び・新規固定・物理値・状態", () => {
    it("並べ替えた inputs の順を保存し、読み戻せる", async () => {
      const live = makeController();
      await live.start();
      live.addInput("a"); live.addInput("b"); live.addInput("c");
      live.moveInput(0, 2);
      expect(useLiveStore.getState().inputs.map(i => i.name)).toEqual(["b", "c", "a"]);
      expect(JSON.parse(localStorage.getItem("sim-gui:layout:v1")!).inputs.map(i => i.name)).toEqual(["b", "c", "a"]);
      live.moveInput(-1, 0); live.moveInput(0, 8);
      live.stop();
      const restored = makeController();
      await restored.start();
      expect(useLiveStore.getState().inputs.map(i => i.name)).toEqual(["b", "c", "a"]);
      restored.stop();
    });

    it("新規入力は固定 ON で待ち、再読み込みや空の holds でも維持し、最初に固定として書く", async () => {
      const live = makeController();
      await live.start();
      live.addInput("new");
      live.addInput("switch", "toggle"); live.addInput("slide", "slider"); live.addInput("push", "button");
      expect(useLiveStore.getState().inputs.map(i => i.hold)).toEqual([true, true, true, false]);
      live.handlers.state({ holds: [] });
      expect(useLiveStore.getState().inputs[0]).toMatchObject({ hold: true, holdPending: true });
      live.stop();
      const restored = makeController();
      await restored.start();
      restored.handlers.state({ holds: [] });
      expect(useLiveStore.getState().inputs[0].hold).toBe(true);
      await restored.write("new", "7");
      expect(posted.filter(p => p.url === "/api/write").at(-1)?.body).toEqual({ name: "new", value: "7", hold: true });
      expect(useLiveStore.getState().inputs[0]).toMatchObject({ hold: true, value: "7", holdPending: undefined });
      restored.stop();
    });

    it("まだ書いていない新規入力をボタンに変えると、既定の固定を外す", async () => {
      const live = makeController();
      await live.start();
      live.addInput("push");
      live.updateInput("push", { widget: "button" });
      expect(useLiveStore.getState().inputs[0]).toMatchObject({ widget: "button", hold: false });
      expect(useLiveStore.getState().inputs[0].holdPending).toBeUndefined();
      live.addInput("kept"); await live.write("kept", "3");
      live.updateInput("kept", { widget: "button" });
      expect(useLiveStore.getState().inputs[1].hold).toBe(true); // 書いたあとの固定は利用者の設定なので残す
      live.stop();
    });

    it("物理値25.06は整数型でraw651を固定として送り、生の値を保存する", async () => {
      const symbols = vi.fn().mockResolvedValue([{ name: "temperature", kind: "int" }]);
      const live = makeController({ symbols });
      await live.start();
      useWaveformStore.setState({ lsbMapping: { temperature: "0.1" }, offsetMapping: { temperature: "-40" } });
      live.addInput("temperature");
      expect(await live.writePhysical("temperature", "25.06")).toBe(651);
      expect(posted.filter(p => p.url === "/api/write").at(-1)?.body).toEqual({ name: "temperature", value: 651, hold: true });
      expect(JSON.parse(localStorage.getItem("sim-gui:layout:v1")!).inputs[0].value).toBe(651);
      await live.writePhysical("temperature", "25.06");
      expect(symbols).toHaveBeenCalledTimes(1);
      live.stop();
    });

    it("型を調べている間に対象関数が変わったら、物理値を書かない", async () => {
      let resolve!: (v: { name: string; kind: string }[]) => void;
      const symbols = vi.fn().mockReturnValue(new Promise(r => { resolve = r; }));
      const live = makeController({ symbols });
      await live.start();
      useWaveformStore.setState({ lsbMapping: { temperature: "0.1" }, offsetMapping: { temperature: "-40" } });
      live.addInput("temperature");
      const pending = live.writePhysical("temperature", "25.06");
      useLiveStore.setState({ sim: { ...useLiveStore.getState().sim, func: "other_func" } });
      resolve([{ name: "temperature", kind: "int" }]);
      expect(await pending).toBeNull();
      expect(posted.filter(p => p.url === "/api/write")).toEqual([]);
      expect(notify.error).toHaveBeenCalledWith(expect.stringContaining("対象関数が変わった"));
      live.stop();
    });

    it("型を調べている間に換算の設定が変わっても、入力したときの換算で送る", async () => {
      let resolve!: (v: { name: string; kind: string }[]) => void;
      const symbols = vi.fn().mockReturnValue(new Promise(r => { resolve = r; }));
      const live = makeController({ symbols });
      await live.start();
      useWaveformStore.setState({ lsbMapping: { temperature: "0.1" }, offsetMapping: { temperature: "-40" } });
      live.addInput("temperature");
      const pending = live.writePhysical("temperature", "25.06");
      useWaveformStore.setState({ offsetMapping: { temperature: "0" } });
      resolve([{ name: "temperature", kind: "int" }]);
      expect(await pending).toBe(651);
      expect(posted.filter(p => p.url === "/api/write").at(-1)?.body).toMatchObject({ name: "temperature", value: 651 });
      live.stop();
    });

    it("型を調べている間にカードを外したら、物理値を書かない", async () => {
      let resolve!: (v: { name: string; kind: string }[]) => void;
      const symbols = vi.fn().mockReturnValue(new Promise(r => { resolve = r; }));
      const live = makeController({ symbols });
      await live.start();
      useWaveformStore.setState({ lsbMapping: { temperature: "0.1" }, offsetMapping: { temperature: "-40" } });
      live.addInput("temperature");
      const pending = live.writePhysical("temperature", "25.06", true);
      live.removeInput("temperature");
      live.addInput("temperature"); // 足し直しても、前のカードの操作は書かない
      resolve([{ name: "temperature", kind: "int" }]);
      expect(await pending).toBeNull();
      // 外したときの固定の解除は送ってよいが、換算した値 651 は書かない
      expect(posted.filter(p => p.url === "/api/write").map(p => (p.body as { value?: unknown }).value)).not.toContain(651);
      live.stop();
    });

    it("型が取得できない物理値は送らず、誤りを通知する", async () => {
      const live = makeController({ symbols: vi.fn().mockResolvedValue([]) });
      await live.start(); live.addInput("unknown");
      expect(await live.writePhysical("unknown", "25")).toBeNull();
      expect(posted.filter(p => p.url === "/api/write")).toEqual([]);
      expect(notify.error).toHaveBeenCalledWith(expect.stringContaining("型を取得できません"));
      live.stop();
    });

    it("状態名がある変数は状態が既定で、選んだ生の値を固定として書く", async () => {
      const live = makeController();
      await live.start();
      useWaveformStore.setState({ modeMapping: { mode: { 0: "停止", 1: "運転" } } });
      live.addInput("mode");
      expect(useLiveStore.getState().inputs[0]).toMatchObject({ widget: "state", hold: true });
      await live.write("mode", "1");
      expect(posted.filter(p => p.url === "/api/write").at(-1)?.body).toEqual({ name: "mode", value: "1", hold: true });
      live.stop();
    });
  });

  describe("起動時の仕向の同期", () => {
    const values = [{ name: "a", value: "0x20", mode: "every" as const }];
    it.each([
      { name: "選択なし", exists: true, doc: { selected: "", variants: [{ name: "国内", values }] }, error: "", expected: [] },
      { name: "選択あり", exists: true, doc: { selected: "国内", variants: [{ name: "国内", values }] }, error: "", expected: values },
      { name: "ファイルなし", exists: false, doc: null, error: "", expected: [] },
      { name: "壊れたファイル", exists: true, doc: null, error: "JSON を読めません", expected: [] },
    ])("$name のとき画面の選択に合わせて setInit を送る", async ({ exists, doc, error, expected }) => {
      const setInit = vi.fn().mockResolvedValue({ ok: true });
      const live = makeController({ getVariants: vi.fn().mockResolvedValue({ path: "variants.json", exists, doc, error }), setInit });
      try {
        expect(await live.start()).toBe(true);
        expect(setInit).toHaveBeenCalledExactlyOnceWith(expected);
      } finally { live.stop(); }
    });

    it.each([
      { status: 400, expected: { "": "初期化できません", a: "変数がありません" } },
      { status: 404, expected: {} },
    ])("起動時の $status を反映エラーとして判定する", async ({ status, expected }) => {
      useVariantsStore.setState({ errors: { "": "前回の失敗" } });
      const live = makeController({
        getVariants: vi.fn().mockResolvedValue({ path: "variants.json", exists: true, doc: null, error: "ファイル破損" }),
        setInit: vi.fn().mockRejectedValue(new ApiError("初期化できません", status, [{ name: "a", message: "変数がありません" }])),
      });
      try {
        expect(await live.start()).toBe(true);
        expect(useVariantsStore.getState().errors).toEqual(expected);
        expect(useVariantsStore.getState().error).toBe("ファイル破損");
      } finally { live.stop(); }
    });

    it("API 以外の反映エラーも保持する", async () => {
      const live = makeController({ setInit: vi.fn().mockRejectedValue(new Error("通信失敗")) });
      try {
        expect(await live.start()).toBe(true);
        expect(useVariantsStore.getState().errors).toEqual({ "": "通信失敗" });
      } finally { live.stop(); }
    });

    it("仕向の取得が例外で失敗しても setInit([]) を送る", async () => {
      const setInit = vi.fn().mockResolvedValue({ ok: true });
      const live = makeController({ getVariants: vi.fn().mockRejectedValue(new Error("接続失敗")), setInit });
      try {
        expect(await live.start()).toBe(true);
        expect(setInit).toHaveBeenCalledExactlyOnceWith([]);
      } finally { live.stop(); }
    });

    it("仕向の取得と setInit が両方失敗してもライブモードを開始する", async () => {
      const setInit = vi.fn().mockRejectedValue(new Error("404"));
      const live = makeController({ getVariants: vi.fn().mockRejectedValue(new Error("接続失敗")), setInit });
      try {
        expect(await live.start()).toBe(true);
        expect(setInit).toHaveBeenCalledExactlyOnceWith([]);
        expect(useLiveStore.getState().mode).toBe("live");
      } finally { live.stop(); }
    });
  });

  describe("仕向ごとの入力パネルと監視一覧", () => {
    const input = (name: string, hold = true): InputItem => ({ name, widget: "number", min: 0, max: 100, step: 1, hold, value: "7" });
    const watch = (name: string, plot = true): WatchItem => ({ name, plot, radix: "hex" });
    const layoutDoc: VariantsDoc = { version: 1, selected: "A", variants: [
      { name: "A", values: [], inputs: [input("a")], watch: [watch("shared")] },
      { name: "B", values: [], inputs: [input("b")], watch: [watch("shared", false), watch("w")] },
      { name: "C", values: [] },
    ] };
    async function setup(error = "", saveVariants = vi.fn().mockResolvedValue({ ok: true })) {
      saveLayout({ ...DEFAULT_LAYOUT, inputs: [input("local", false)], watch: [watch("local-watch")], windowSize: 300, maxSamples: 4000 });
      const live = makeController({
        getVariants: vi.fn().mockResolvedValue({ path: "variants.json", exists: true, doc: layoutDoc, error }),
        saveVariants,
      });
      await live.start();
      return { live, saveVariants };
    }
    function select(live: ReturnType<typeof makeController>, selected: string) {
      return live.selectVariant({ ...useVariantsStore.getState().doc, selected });
    }
    it("起動時は選択中の仕向の構成とブラウザの表示・記録上限を使う", async () => {
      const { live, saveVariants } = await setup();
      expect(useLiveStore.getState()).toMatchObject({
        inputs: [input("a")], watch: [watch("shared")], windowSize: 300, maxSamples: 4000,
      });
      expect(posted).toContainEqual({ url: "/api/write", body: { name: "a", value: "7", hold: true } });
      await vi.advanceTimersByTimeAsync(500);
      expect(saveVariants).not.toHaveBeenCalled();
      live.stop();
    });
    it("A の現在の構成を残し、固定と監視を B に切り替える", async () => {
      const { live } = await setup();
      live.updateInput("a", { widget: "slider" });
      live.addInput("push", "button");
      await live.write("push", 1, true);
      const revision = useLiveStore.getState().layoutRevision;
      await select(live, "B");
      await live.releaseButton("push", 0, revision);
      expect(useLiveStore.getState()).toMatchObject({ inputs: [input("b")], watch: [watch("shared", false), watch("w")] });
      expect(useVariantsStore.getState().doc.variants[0]).toMatchObject({
        inputs: [{ name: "a", widget: "slider" }, { name: "push", hold: false }], watch: [watch("shared")],
      });
      expect(posted.filter(p => p.url === "/api/write").slice(-3).map(p => p.body)).toEqual([
        { name: "a", release: true }, { name: "push", release: true }, { name: "b", hold: true, value: "7" },
      ]);
      expect(posted.filter(p => p.url === "/api/watch").at(-1)?.body).toEqual({ names: ["shared", "w", "b"] });
      live.stop();
    });
    it("値が未設定の固定入力に切り替えると前の固定を解除する", async () => {
      const doc: VariantsDoc = { version: 1, selected: "A", variants: [
        { name: "A", values: [], inputs: [input("x")] },
        { name: "B", values: [], inputs: [{ ...input("x"), value: undefined }] },
      ] };
      const live = makeController({
        getVariants: vi.fn().mockResolvedValue({ path: "variants.json", exists: true, doc, error: "" }),
      });
      try {
        expect(await live.start()).toBe(true);
        expect(posted).toContainEqual({ url: "/api/write", body: { name: "x", value: "7", hold: true } });
        const beforeSwitch = posted.length;

        await select(live, "B");

        expect(posted.slice(beforeSwitch).filter(p => p.url === "/api/write").map(p => p.body)).toEqual([
          { name: "x", release: true },
        ]);
        expect(useLiveStore.getState().inputs[0]).toMatchObject({ name: "x", hold: true, holdPending: true });
      } finally { live.stop(); }
    });
    it("前の固定の解除を待つ間に外した入力は書き込まない", async () => {
      const doc: VariantsDoc = { version: 1, selected: "A", variants: [
        { name: "A", values: [], inputs: [input("x")] },
        { name: "B", values: [], inputs: [input("y")] },
      ] };
      const live = makeController({
        getVariants: vi.fn().mockResolvedValue({ path: "variants.json", exists: true, doc, error: "" }),
      });
      let finishRelease!: () => void;
      const releasePending = new Promise<void>(resolve => { finishRelease = resolve; });
      try {
        expect(await live.start()).toBe(true);
        const release = live.api.release;
        vi.spyOn(live.api, "release").mockImplementationOnce(async name => {
          const result = await release(name);
          await releasePending;
          return result;
        });
        const beforeSwitch = posted.length;
        const switching = select(live, "B");
        expect(posted.slice(beforeSwitch)).toContainEqual({ url: "/api/write", body: { name: "x", release: true } });
        expect(useLiveStore.getState().inputs).toEqual([input("y")]);
        live.removeInput("y");
        expect(posted.slice(beforeSwitch)).toContainEqual({ url: "/api/write", body: { name: "y", release: true } });
        finishRelease();
        await switching;
        expect(posted.slice(beforeSwitch)).not.toContainEqual({
          url: "/api/write", body: expect.objectContaining({ name: "y", hold: true }),
        });
      } finally {
        finishRelease();
        live.stop();
      }
    });
    it("初めて選ぶ仕向は現在の構成を引き継ぎ、選択なしはブラウザの構成に戻す", async () => {
      const { live } = await setup();
      await select(live, "C");
      expect(useVariantsStore.getState().doc.variants[2]).toMatchObject({ inputs: [input("a")], watch: [watch("shared")] });
      expect(useLiveStore.getState().inputs).toEqual([input("a")]);
      await select(live, "");
      expect(useLiveStore.getState()).toMatchObject({ inputs: [input("local", false)], watch: [watch("local-watch")] });
      live.addWatch("extra");
      expect(loadLayout().watch.map(w => w.name)).toEqual(["local-watch", "extra"]);
      live.stop();
    });
    it("連続変更をまとめ、送信時の初期値と入力・監視を一度だけ保存する", async () => {
      const { live, saveVariants } = await setup();
      live.addInput("new");
      live.updateInput("a", { value: "8" });
      await vi.advanceTimersByTimeAsync(300);
      live.updateInput("a", { value: "9" });
      live.addWatch("extra", false);
      const { doc } = useVariantsStore.getState();
      const values = [{ name: "init", value: "3", mode: "once" as const }];
      useVariantsStore.setState({ doc: { ...doc, variants: doc.variants.map(v => v.name === "A" ? { ...v, values } : v) } });
      await vi.advanceTimersByTimeAsync(499);
      expect(saveVariants).not.toHaveBeenCalled();
      await vi.advanceTimersByTimeAsync(1);
      const saved = useVariantsStore.getState();
      expect(saveVariants).toHaveBeenCalledExactlyOnceWith(saved.doc);
      expect(saved.doc.variants[0]).toMatchObject({ values, inputs: [{ name: "a", value: "9" }, { name: "new", hold: true }],
        watch: [{ name: "shared" }, { name: "extra", plot: false }] });
      expect(saved.doc.variants[0].inputs?.[1]).not.toHaveProperty("holdPending");
      expect(saved.saved).toBe(JSON.stringify(saved.doc));
      live.stop();
    });
    it("仕向の読み込みエラー時はブラウザの構成で起動し、自動保存を止める", async () => {
      const { live, saveVariants } = await setup("読めません");
      expect(useLiveStore.getState().inputs).toEqual([input("local", false)]);
      live.addInput("extra");
      await vi.advanceTimersByTimeAsync(500);
      expect(saveVariants).not.toHaveBeenCalled();
      live.stop();
    });
    it("自動保存に失敗するとエラーを出し、次の変更で再試行する", async () => {
      const { live, saveVariants } = await setup("", vi.fn().mockRejectedValueOnce(new Error("書き込み失敗")).mockResolvedValue({ ok: true }));
      live.updateInput("a", { value: "8" });
      await vi.advanceTimersByTimeAsync(500);
      expect(useVariantsStore.getState().errors[""]).toBe("入力パネルを仕向ファイルに保存できませんでした: 書き込み失敗");
      live.updateInput("a", { value: "9" });
      await vi.advanceTimersByTimeAsync(500);
      expect(saveVariants).toHaveBeenCalledTimes(2);
      expect(useVariantsStore.getState().errors[""]).toBeUndefined();
      live.stop();
    });
    it("自動保存が応答待ちなら、手動保存は最新の内容で後から送る", async () => {
      let release!: () => void;
      const first = new Promise<void>(resolve => { release = resolve; });
      const { live, saveVariants } = await setup("", vi.fn().mockReturnValueOnce(first).mockResolvedValue({ ok: true }));
      live.updateInput("a", { value: "8" });
      await vi.advanceTimersByTimeAsync(500);
      live.updateInput("a", { value: "9" });
      const manual = saveVariantFile(live.api, "manual");
      expect(saveVariants).toHaveBeenCalledTimes(1);
      release();
      await manual;
      await vi.advanceTimersByTimeAsync(500);
      expect(saveVariants).toHaveBeenCalledTimes(2);
      const { doc, saved } = useVariantsStore.getState();
      expect(doc.variants[0].inputs?.[0].value).toBe("9");
      expect(saveVariants).toHaveBeenLastCalledWith(doc);
      expect(saved).toBe(JSON.stringify(doc));
      live.stop();
    });
  });

  it("再読み込み時の initStale と SSE のリセットを反映する", async () => {
    const live = makeController({ info: vi.fn().mockResolvedValue({ ...info, state: { ...info.state, initStale: true } }) });
    try {
      expect(await live.start()).toBe(true);
      expect(useLiveStore.getState().sim.initStale).toBe(true);
      FakeEventSource.last!.emit("state", { step: 0, initStale: false });
      expect(useLiveStore.getState().sim.initStale).toBe(false);
    } finally { live.stop(); }
  });

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

  describe.each(["CSV", "reset", "func"] as const)("%s の記録切替", (operation) => {
    it.each(["応答が先", "新 samples が先", "応答待ちに旧 samples"])("%sでも新しい先頭からだけを残す", async (order) => {
      const live = makeController();
      await live.start();
      const es = FakeEventSource.last!;
      es.emit("samples", { step0: 1, names: ["a"], rows: [[90], [91], [92]] });
      live.commit();
      let release!: () => void;
      responseGate = new Promise<void>((resolve) => { release = resolve; });
      controlResponse = { func: operation === "func" ? "sub" : "main", running: false, step: 0 };
      loadCsvResponse = { status: 200, body: { rows: 2, names: ["a"] } };
      const pending = operation === "CSV" ? live.loadCsv(new Blob(["csv"]))
        : live.control(operation === "reset" ? { cmd: "reset" } : { func: "sub", cmd: "step", steps: 2 });
      if (order === "応答待ちに旧 samples") {
        es.emit("samples", { step0: 4, names: ["a"], rows: [[93], [94]] });
      }
      const receiveNew = () => {
        es.emit("samples", { step0: 1, names: ["a"], rows: [[10]] });
        // samples より後で関数変更を知っても、新しい記録を消さない。
        if (operation === "func") es.emit("state", { func: "sub" });
      };
      if (order !== "応答が先") receiveNew();
      release();
      await pending;
      if (order === "応答が先") {
        expect(record.length).toBe(0);
        receiveNew();
      } else {
        expect(record.toChartData().seriesMap.a).toEqual([10]);
      }
      es.emit("samples", { step0: 2, names: ["a"], rows: [[11]] });
      expect([...record.steps()]).toEqual([1, 2]);
      expect(record.toChartData().seriesMap).toEqual({ a: [10, 11] });
      live.commit();
      expect(useWaveformStore.getState().chartData?.seriesMap.a).toEqual([10, 11]);
      live.stop();
    });
  });

  it("旧記録が空でも、応答前に完了した短い CSV の先頭サンプルを残す", async () => {
    const live = makeController();
    await live.start();
    let release!: () => void;
    responseGate = new Promise<void>((resolve) => { release = resolve; });
    const pending = live.loadCsv(new Blob(["csv"]));
    FakeEventSource.last!.emit("samples", { step0: 1, names: ["a"], rows: [[7]] });
    release();
    await pending;
    expect(record.toChartData().seriesMap.a).toEqual([7]);
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

  describe("反映待ちの書き込み（停止中はステップまでメモリに反映されない）", () => {
    /** 入力パネルのトグルを押したときと同じ: 元にする値を反転して書く */
    const toggle = (live: ReturnType<typeof makeController>, name: string) =>
      live.write(name, (live.inputBase(name) ?? 0) !== 0 ? 0 : 1);
    const writes = () => posted.filter((p) => p.url === "/api/write").map((p) => p.body);

    async function startPaused() {
      info.state = { running: false, step: 3, speed: 1, holds: [], overrun: false };
      const live = makeController();
      useLiveStore.setState({ watch: [] });
      await live.start();
      live.addInput("g_mode", "toggle");
      live.updateInput("g_mode", { hold: false, holdPending: undefined }); // 単発入力の既存動作を試す
      await vi.runOnlyPendingTimersAsync();
      const es = FakeEventSource.last!;
      es.emit("values", { step: 3, names: ["g_mode"], values: [0] });
      live.flushValues();
      return { live, es };
    }

    it("SSE の state の pending を取り込み、ステップで空になれば消す", async () => {
      const { live, es } = await startPaused();
      es.emit("state", { running: false, step: 3, pending: [{ name: "g_mode", value: "1" }] });
      expect(useLiveStore.getState().sim.pending).toEqual([{ name: "g_mode", value: "1" }]);
      expect(live.inputBase("g_mode")).toBe(1);
      // 現在値（メモリ）は変わらない
      expect(useLiveStore.getState().values.g_mode).toBe(0);
      // 1 ステップ進むと反映されて pending が空になる
      es.emit("samples", { step0: 3, names: ["g_mode"], rows: [[1]] });
      es.emit("state", { running: false, step: 4, pending: [] });
      live.flushValues();
      expect(useLiveStore.getState().sim.pending).toEqual([]);
      expect(useLiveStore.getState().values.g_mode).toBe(1);
      expect(live.inputBase("g_mode")).toBe(1);
      live.stop();
    });

    it("停止中にトグルを続けて押すと、反映待ちの値を元に反転する", async () => {
      const { live } = await startPaused();
      await toggle(live, "g_mode");
      expect(useLiveStore.getState().sim.pending).toEqual([{ name: "g_mode", value: "1" }]);
      expect(useLiveStore.getState().values.g_mode).toBe(0);
      await toggle(live, "g_mode");
      expect(writes()).toEqual([
        { name: "g_mode", value: 1, hold: false },
        { name: "g_mode", value: 0, hold: false },
      ]);
      expect(useLiveStore.getState().sim.pending).toEqual([{ name: "g_mode", value: "0" }]);
      live.stop();
    });

    it("固定中の入力に書くと hold:true で送り、固定の値を元に反転する。固定を外すと反映待ちも消す", async () => {
      const { live, es } = await startPaused();
      await live.setHold("g_mode", true, 0);
      es.emit("state", { running: false, holds: [{ name: "g_mode", value: 0 }], pending: [] });
      await toggle(live, "g_mode");
      expect(writes().at(-1)).toEqual({ name: "g_mode", value: 1, hold: true });
      expect(useLiveStore.getState().sim.holds).toEqual([{ name: "g_mode", value: 1 }]);
      expect(live.inputBase("g_mode")).toBe(1);
      await toggle(live, "g_mode");
      expect(writes().at(-1)).toEqual({ name: "g_mode", value: 0, hold: true });

      // 固定 OFF: その名前の反映待ちもサーバで捨てられる
      useLiveStore.setState({ sim: { ...useLiveStore.getState().sim, pending: [{ name: "g_mode", value: "1" }, { name: "x", value: "2" }] } });
      await live.setHold("g_mode", false);
      expect(posted.at(-1)).toEqual({ url: "/api/write", body: { name: "g_mode", release: true } });
      expect(useLiveStore.getState().sim.pending).toEqual([{ name: "x", value: "2" }]);
      expect(useLiveStore.getState().sim.holds).toEqual([]);
      expect(live.inputBase("g_mode")).toBe(0);
      live.stop();
    });

    it.each(["リセットが必要です", "対象関数が未設定です"])("ボタンの固定が拒否されたら失敗を返し一時固定の内部状態を残さない: %s", async (message) => {
      const { live, es } = await startPaused();
      live.addInput("g_mode", "button");
      writeResponse = { status: 400, body: { error: message } };
      expect(await live.write("g_mode", 1, true)).toBe(false);
      expect(useLiveStore.getState().sim.holds).toEqual([]);
      expect(notify.error).toHaveBeenCalledTimes(1);
      // 拒否した一時固定が残っていると、その後のサーバの固定状態と同期できない。
      es.emit("state", { holds: [{ name: "g_mode", value: 4 }] });
      expect(useLiveStore.getState().inputs.find((i) => i.name === "g_mode")?.hold).toBe(true);
      live.stop();
    });

    it("押している間だけ 1 のボタンは、離すと押す前の値（反映待ち）に戻す", async () => {
      const { live } = await startPaused();
      await live.write("g_mode", 1);
      const before = live.inputBase("g_mode");
      await live.write("g_mode", 1, true);
      expect(useLiveStore.getState().sim.pending).toEqual([]);
      await live.releaseButton("g_mode", before ?? 0);
      expect(writes().slice(-3)).toEqual([
        { name: "g_mode", value: 1, hold: true },
        { name: "g_mode", release: true },
        { name: "g_mode", value: 1, hold: false },
      ]);
      expect(useLiveStore.getState().sim.holds).toEqual([]);
      expect(useLiveStore.getState().sim.pending).toEqual([{ name: "g_mode", value: "1" }]);
      live.stop();
    });

    it("state.error が出たら 1 回だけ知らせ、表示用に残す", async () => {
      const { live, es } = await startPaused();
      es.emit("state", { running: false, error: "ポインタ g_p の先が NULL です" });
      es.emit("state", { running: false, error: "ポインタ g_p の先が NULL です" });
      expect(useLiveStore.getState().sim.error).toBe("ポインタ g_p の先が NULL です");
      expect(notify.error).toHaveBeenCalledTimes(1);
      expect(notify.error.mock.calls[0][0]).toContain("ポインタ g_p の先が NULL です");
      es.emit("state", { running: false, error: "" });
      expect(useLiveStore.getState().sim.error).toBe("");
      live.stop();
    });
  });

  describe("2^53 を超える整数（書いた表記のまま送り、丸めた値を送り返さない）", () => {
    const BIG = "9007199254740993";
    const BIG_HEX = "0x20000000000001";
    const writes = () => posted.filter((p) => p.url === "/api/write").map((p) => p.body);
    const saved = () => JSON.parse(localStorage.getItem("sim-gui:layout:v1")!) as { inputs: { name: string; value?: unknown; hold: boolean }[] };

    async function startWith(name: string) {
      info.state = { running: false, step: 0, speed: 1, holds: [], overrun: false };
      const live = makeController();
      useLiveStore.setState({ watch: [] });
      await live.start();
      live.addInput(name, "number");
      live.updateInput(name, { hold: false, holdPending: undefined }); // 単発の文字列を試す
      await vi.runOnlyPendingTimersAsync();
      return { live, es: FakeEventSource.last! };
    }

    it("数値入力の文字列はその表記のまま送り、反映待ちと構成にも同じ文字列で残す", async () => {
      const { live } = await startWith("g_u64");
      await live.write("g_u64", BIG);
      await live.write("g_u64", BIG_HEX);
      expect(writes()).toEqual([
        { name: "g_u64", value: BIG, hold: false },
        { name: "g_u64", value: BIG_HEX, hold: false },
      ]);
      // JSON の本文にも数値ではなく文字列として載る
      const raw = posted.filter((p) => p.url === "/api/write").map((p) => JSON.stringify(p.body));
      expect(raw[0]).toContain(`"value":"${BIG}"`);
      expect(useLiveStore.getState().sim.pending).toEqual([{ name: "g_u64", value: BIG_HEX }]);
      expect(useLiveStore.getState().inputs[0].value).toBe(BIG_HEX);
      expect(saved().inputs[0].value).toBe(BIG_HEX);
      live.stop();
    });

    it("400 ならサーバのメッセージをそのまま知らせ、反映待ちにも構成にも残さない", async () => {
      const { live } = await startWith("g_u64");
      writeResponse = { status: 400, body: { error: "g_u64: 値 '12abc' を読めません" } };
      await live.write("g_u64", "12abc");
      expect(writes()).toEqual([{ name: "g_u64", value: "12abc", hold: false }]);
      expect(notify.error).toHaveBeenCalledWith("g_u64 に書き込めませんでした: g_u64: 値 '12abc' を読めません");
      expect(useLiveStore.getState().sim.pending).toEqual([]);
      expect(useLiveStore.getState().inputs[0].value).toBeUndefined();
      live.stop();
    });

    it("固定と固定中の値の更新でも同じ文字列を送り、丸まって届く state.holds を書いた表記に戻す", async () => {
      const { live, es } = await startWith("g_u64");
      await live.setHold("g_u64", true, BIG_HEX);
      expect(writes().at(-1)).toEqual({ name: "g_u64", value: BIG_HEX, hold: true });
      // C 側は数値にできる固定値を JSON の数値で返すので、2^53 を超える整数は丸まって届く
      es.emit("state", { running: false, holds: [{ name: "g_u64", value: 9007199254740992 }], pending: [] });
      expect(useLiveStore.getState().sim.holds).toEqual([{ name: "g_u64", value: BIG_HEX }]);
      expect(live.inputBaseValue("g_u64")).toMatchObject({ source: "hold", send: BIG_HEX });
      // 固定中に書くと hold:true で同じ表記のまま送る
      await live.write("g_u64", BIG);
      expect(writes().at(-1)).toEqual({ name: "g_u64", value: BIG, hold: true });
      es.emit("state", { running: false, holds: [{ name: "g_u64", value: 9007199254740992 }], pending: [] });
      expect(useLiveStore.getState().sim.holds).toEqual([{ name: "g_u64", value: BIG }]);
      expect(saved().inputs[0]).toMatchObject({ value: BIG, hold: true });
      // 固定を外して付け直すと、最後に書いた表記で固定する
      await live.setHold("g_u64", false);
      await live.setHold("g_u64", true);
      expect(writes().slice(-2)).toEqual([{ name: "g_u64", release: true }, { name: "g_u64", value: BIG, hold: true }]);
      live.stop();
    });

    it("構成の保存と復元で文字列を保ち、再読み込み後も丸まった state.holds を書いた表記に戻す", async () => {
      const first = await startWith("g_u64");
      await first.live.setHold("g_u64", true, BIG_HEX);
      first.live.stop();
      expect(saved().inputs[0]).toMatchObject({ value: BIG_HEX, hold: true });
      // 再読み込み: サーバは固定を持ったまま、値は丸まった数値で返す
      info.state = { running: false, step: 0, speed: 1, holds: [{ name: "g_u64", value: 9007199254740992 }] as never[], overrun: false };
      useLiveStore.setState({ ...initialLiveState });
    useVariantsStore.setState({ errors: {} });
      const live = makeController();
      await live.start();
      expect(useLiveStore.getState().inputs[0]).toMatchObject({ name: "g_u64", value: BIG_HEX, hold: true });
      expect(useLiveStore.getState().sim.holds).toEqual([{ name: "g_u64", value: BIG_HEX }]);
      expect(live.inputBaseValue("g_u64").send).toBe(BIG_HEX);
      live.stop();
    });

    it("丸められている可能性のある現在値は、固定・ボタンの復帰で送り返さない", async () => {
      const { live, es } = await startWith("g_u64");
      es.emit("values", { step: 0, names: ["g_u64"], values: [9007199254740992] });
      live.flushValues();
      expect(live.inputBaseValue("g_u64")).toEqual({ value: 9007199254740992, source: "current", send: null, inexact: true });
      const before = writes().length;
      await live.setHold("g_u64", true, null);
      await live.setHold("g_u64", true);
      expect(writes().length).toBe(before);
      expect(useLiveStore.getState().inputs[0].hold).toBe(false);
      expect(notify.error).toHaveBeenCalledTimes(2);
      // ボタン: 押す前の値が分からなければ、離したときに固定を解くだけで値は書かない
      await live.write("g_u64", 1, true);
      await live.releaseButton("g_u64", null);
      expect(writes().slice(before)).toEqual([{ name: "g_u64", value: 1, hold: true }, { name: "g_u64", release: true }]);
      expect(notify.error).toHaveBeenCalledTimes(3);
      live.stop();
    });
  });

  describe("テスト CSV の読み込み", () => {
    it("mod ブロックが複数ある応答の warning は、成功とは別に警告で知らせる", async () => {
      const live = makeController();
      useLiveStore.setState({ watch: [] });
      await live.start();
      loadCsvResponse = { status: 200, body: {
        rows: 2, names: ["@a", "@b", "mul16@@"], blocks: 2,
        warning: "CSV に mod ブロックが 2 個あります。GUI では先頭のブロック（mul16）だけを再生し、2 つ目以降は再生しません",
      } };
      await live.loadCsv(new Blob(["mod,mul16,t,2,1\n"]));
      expect(posted.at(-1)?.url).toBe("/api/load-csv");
      expect(notify.success).toHaveBeenCalledTimes(1);
      expect(notify.success.mock.calls[0][0]).toContain("2 行、3 変数");
      expect(notify.warn).toHaveBeenCalledTimes(1);
      expect(notify.warn.mock.calls[0][0]).toContain("先頭のブロック（mul16）だけを再生し");
      expect(notify.error).not.toHaveBeenCalled();
      expect(useLiveStore.getState().watch.map((w) => w.name)).toEqual(["@a", "@b", "mul16@@"]);
      live.stop();
    });

    it("warning が無い（ブロックが 1 つ）・不正な形なら警告しない", async () => {
      const live = makeController();
      await live.start();
      loadCsvResponse = { status: 200, body: { rows: 1, names: ["a"] } };
      await live.loadCsv(new Blob(["x"]));
      loadCsvResponse = { status: 200, body: { rows: 1, names: ["a"], blocks: "2", warning: 3 } };
      await live.loadCsv(new Blob(["x"]));
      expect(notify.success).toHaveBeenCalledTimes(2);
      expect(notify.warn).not.toHaveBeenCalled();
      live.stop();
    });

    it("400 ならサーバのメッセージをそのまま知らせ、記録・波形・入力パネル・監視を変えない", async () => {
      const live = await startWithArgs();
      const es = FakeEventSource.last!;
      es.emit("samples", { step0: 0, names: ["g_out"], rows: [[1], [2]] });
      live.commit();
      const before = {
        inputs: useLiveStore.getState().inputs,
        watch: useLiveStore.getState().watch,
        sim: useLiveStore.getState().sim,
        chartData: useWaveformStore.getState().chartData,
        saved: localStorage.getItem("sim-gui:layout:v1"),
      };
      expect(before.chartData).not.toBeNull();
      const message = "入力 2 列目と 4 列目が同じ名前 'P1' です。GUI では同じ名前の入力列が複数ある CSV は読み込めません"
        + "（sim run では実行できます。左から順に書きます）";
      loadCsvResponse = { status: 400, body: { error: message } };
      await live.loadCsv(new Blob(["mod,eng_judge_gear,t,3,0\n#COMMENT,P1,P1_bit.no0,P1\n,0,1,2\n"]));
      expect(notify.error).toHaveBeenCalledTimes(1);
      expect(notify.error.mock.calls[0][0]).toBe(`テスト CSV を読み込めませんでした: ${message}`);
      expect(notify.success).not.toHaveBeenCalled();
      expect(notify.warn).not.toHaveBeenCalled();
      const s = useLiveStore.getState();
      expect(s.inputs).toBe(before.inputs);
      expect(s.watch).toBe(before.watch);
      expect(s.sim).toBe(before.sim);
      expect(record.length).toBe(2);
      expect(useWaveformStore.getState().chartData).toBe(before.chartData);
      expect(localStorage.getItem("sim-gui:layout:v1")).toBe(before.saved);
      live.stop();
    });
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


describe("needReset のストリーム", () => {
  it("SSE は要リセットと解除を読み、旧サーバの未指定は false にする", () => {
    const h = handlers();
    for (const state of [{ needReset: true, error: "NULL" }, { needReset: false, error: "" }, {}]) {
      dispatchStreamEvent("state", JSON.stringify(state), h);
      expect(h.state).toHaveBeenLastCalledWith(expect.objectContaining({ needReset: "needReset" in state && state.needReset === true }));
    }
  });
});
