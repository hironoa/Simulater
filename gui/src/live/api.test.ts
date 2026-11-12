import { describe, expect, it, vi } from "vitest";
import { ApiError, createApi, detectServer, memberPath, parseControlResponse, parseDropped, parseLoadCsvResult, parsePending, parseState } from "./api";

function fakeFetch(handler: (url: string, init?: RequestInit) => { status?: number; body: unknown }) {
  return vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
    const { status = 200, body } = handler(String(input), init);
    return new Response(typeof body === "string" ? body : JSON.stringify(body), { status });
  });
}

describe("createApi", () => {
  it("各エンドポイントに設計書どおりのメソッド・本文で送る", async () => {
    const f = fakeFetch(() => ({ body: {} }));
    const api = createApi("http://h", f as unknown as typeof fetch);
    await api.control({ cmd: "step", steps: 10 });
    await api.watch(["a", "b.c"]);
    await api.write("x", 3, true);
    await api.release("x");
    const calls = f.mock.calls.map(([u, i]) => [String(u), i?.method, i?.body]);
    expect(calls).toEqual([
      ["http://h/api/control", "POST", JSON.stringify({ cmd: "step", steps: 10 })],
      ["http://h/api/watch", "POST", JSON.stringify({ names: ["a", "b.c"] })],
      ["http://h/api/write", "POST", JSON.stringify({ name: "x", value: 3, hold: true })],
      ["http://h/api/write", "POST", JSON.stringify({ name: "x", release: true })],
    ]);
  });

  it("symbols は検索文字列を URL エンコードする", async () => {
    const f = fakeFetch(() => ({ body: [{ name: "drv.c/s_tick", type: "uint32_t", size: 4, kind: "uint", writable: false }] }));
    const api = createApi("", f as unknown as typeof fetch);
    const hits = await api.symbols("drv.c/s t&", 50);
    expect(String(f.mock.calls[0][0])).toBe("/api/symbols?q=drv.c%2Fs%20t%26&limit=50");
    expect(hits[0].name).toBe("drv.c/s_tick");
  });

  it("load-csv はバイト列をそのまま送り、結果を返す", async () => {
    const f = fakeFetch(() => ({ body: { rows: 3, names: ["@a", "b"] } }));
    const api = createApi("", f as unknown as typeof fetch);
    const bytes = new Uint8Array([0x82, 0xa0, 0x2c]).buffer; // Shift_JIS の「あ,」
    const res = await api.loadCsv(bytes);
    expect(res).toEqual({ rows: 3, names: ["@a", "b"] });
    expect(f.mock.calls[0][1]?.body).toBe(bytes);
  });

  it("load-csv の blocks / warning を読み、無ければ付けず、不正な形は無視する", async () => {
    const f = fakeFetch(() => ({ body: { rows: 1, names: ["a"], blocks: 2, warning: "先頭のブロックだけを再生します" } }));
    expect(await createApi("", f as unknown as typeof fetch).loadCsv("x"))
      .toEqual({ rows: 1, names: ["a"], blocks: 2, warning: "先頭のブロックだけを再生します" });
    expect(parseLoadCsvResult({ rows: 1, names: ["a"] })).toEqual({ rows: 1, names: ["a"] });
    expect(parseLoadCsvResult({ rows: 1, names: ["a", 3], blocks: "2", warning: 5 })).toEqual({ rows: 1, names: ["a"] });
    expect(parseLoadCsvResult({ rows: 1, names: [], blocks: 1.5, warning: "  " })).toEqual({ rows: 1, names: [] });
    expect(parseLoadCsvResult(null)).toEqual({ rows: 0, names: [] });
  });

  it("load-csv の 400 はサーバのメッセージをそのまま ApiError にする", async () => {
    const message = "入力 2 列目と 4 列目が同じ名前 'P1' です。";
    const f = fakeFetch(() => ({ status: 400, body: { error: message } }));
    await expect(createApi("", f as unknown as typeof fetch).loadCsv("x")).rejects.toMatchObject({ status: 400, message });
  });

  it("エラー応答は本文の error を載せた ApiError にする", async () => {
    const f = fakeFetch(() => ({ status: 404, body: { error: "変数 zz が見つかりません" } }));
    const api = createApi("", f as unknown as typeof fetch);
    await expect(api.write("zz", 1)).rejects.toMatchObject({ name: "ApiError", status: 404, message: "変数 zz が見つかりません" });
    const g = fakeFetch(() => ({ status: 500, body: "boom" }));
    await expect(createApi("", g as unknown as typeof fetch).info()).rejects.toBeInstanceOf(ApiError);
  });
});

describe("detectServer", () => {
  const info = { version: "1", periodMs: 5, func: "main", functions: ["main"], state: { running: false } };

  it("file:// ではサーバを探さない", async () => {
    const f = fakeFetch(() => ({ body: info }));
    expect(await detectServer(createApi("", f as unknown as typeof fetch), "file:")).toBeNull();
    expect(f).not.toHaveBeenCalled();
  });

  it("/api/info が答えればライブモード", async () => {
    const f = fakeFetch(() => ({ body: info }));
    const got = await detectServer(createApi("", f as unknown as typeof fetch), "http:");
    expect(got).toMatchObject({ periodMs: 5, func: "main", functions: ["main"] });
  });

  it("失敗・形の違う応答は従来の Viewer として扱う", async () => {
    const bad = fakeFetch(() => ({ status: 404, body: "not found" }));
    expect(await detectServer(createApi("", bad as unknown as typeof fetch), "http:")).toBeNull();
    const html = fakeFetch(() => ({ body: "<html>" }));
    expect(await detectServer(createApi("", html as unknown as typeof fetch), "http:")).toBeNull();
    const other = fakeFetch(() => ({ body: { hello: 1 } }));
    expect(await detectServer(createApi("", other as unknown as typeof fetch), "https:")).toBeNull();
  });
});

describe("memberPath", () => {
  it("完全名・メンバ名・添字のどれで返されても子の完全名にする", () => {
    expect(memberPath("g_cmd", "g_cmd.mode")).toBe("g_cmd.mode");
    expect(memberPath("g_cmd", "mode")).toBe("g_cmd.mode");
    expect(memberPath("g_buf", "[2]")).toBe("g_buf[2]");
    expect(memberPath("g_buf", "2")).toBe("g_buf[2]");
    expect(memberPath("g_buf", "g_buf[2]")).toBe("g_buf[2]");
    expect(memberPath("p", "->next")).toBe("p->next");
    expect(memberPath("g_cmd", "g_cmd2")).toBe("g_cmd.g_cmd2");
  });
});

describe("dropped と control の応答", () => {
  it("parseDropped は無ければ空、配列でなければ undefined、不正な項目は捨てる", () => {
    expect(parseDropped(undefined)).toEqual([]);
    expect(parseDropped({})).toBeUndefined();
    expect(parseDropped([{ name: "@a", kind: "hold", message: "m" }, { name: "@b", kind: "x" }, { name: 1, kind: "watch" }, null]))
      .toEqual([{ name: "@a", kind: "hold", message: "m" }]);
  });

  it("control は応答の state を読む（state そのもの・{state} のどちらでも）", async () => {
    const state = { running: false, step: 0, func: "mul16", dropped: [{ name: "@rpm", kind: "watch", message: "m" }] };
    for (const body of [state, { ok: true, state }]) {
      const api = createApi("", fakeFetch(() => ({ body })) as unknown as typeof fetch);
      expect(await api.control({ func: "mul16" })).toEqual({ ...state, pending: [] });
    }
    expect(parseControlResponse({ ok: true })).toBeNull();
  });

  it("detectServer は info の state の dropped も読む", async () => {
    const f = fakeFetch(() => ({ body: { version: "v", periodMs: 5, func: "f", functions: [], state: { holds: [], dropped: [{ name: "@a", kind: "hold", message: "m" }, 3] } } }));
    const info = await detectServer(createApi("", f as unknown as typeof fetch), "http:");
    expect(info?.state).toEqual({ holds: [], dropped: [{ name: "@a", kind: "hold", message: "m" }], pending: [] });
  });
});

describe("pending（反映待ちの単発書き込み）と error", () => {
  it("parsePending は無ければ空、配列でなければ undefined、不正な項目は捨て、同じ名前は後のものを採る", () => {
    expect(parsePending(undefined)).toEqual([]);
    expect(parsePending(null)).toEqual([]);
    expect(parsePending({ name: "a", value: "1" })).toBeUndefined();
    expect(parsePending([
      { name: "P1", value: "0x78" }, { name: "", value: "1" }, { name: "g_c", value: "'A'" }, { name: "x" }, 7,
      { name: "n", value: 3 }, { name: "P1", value: "5" },
    ])).toEqual([{ name: "g_c", value: "'A'" }, { name: "n", value: "3" }, { name: "P1", value: "5" }]);
  });

  it("parseState は pending と error を読み、不正な pending は無視する", () => {
    expect(parseState({ running: false, pending: [{ name: "g_mode", value: "1" }], error: "" }))
      .toEqual({ running: false, pending: [{ name: "g_mode", value: "1" }], error: "", dropped: [] });
    expect(parseState({ pending: "x", error: 3 })).toEqual({ dropped: [] });
    expect(parseState({ step: 1 })?.pending).toEqual([]);
  });

  it("detectServer は info の state の pending と error も読む", async () => {
    const f = fakeFetch(() => ({ body: { version: "v", periodMs: 5, func: "f", functions: [],
      state: { pending: [{ name: "P1", value: "0x10" }], error: "ポインタ g_p が NULL です" } } }));
    const info = await detectServer(createApi("", f as unknown as typeof fetch), "http:");
    expect(info?.state).toMatchObject({ pending: [{ name: "P1", value: "0x10" }], error: "ポインタ g_p が NULL です" });
  });
});
