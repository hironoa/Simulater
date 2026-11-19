import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { afterEach, describe, expect, it } from "vitest";
import { initialLiveState, useLiveStore } from "../../live/liveStore";
import { useVariantsStore, variantWarning } from "../../live/variantsStore";
import { LiveBar } from "./LiveBar";

const initial = useLiveStore.getInitialState();
const originalSim = initial.sim;
const variantsInitial = useVariantsStore.getInitialState();
const originalVariants = { ...variantsInitial };
afterEach(() => { initial.sim = originalSim; Object.assign(variantsInitial, originalVariants); });

function render(func: string, needReset = false) {
  // サーバ描画は Zustand の初期スナップショットを読む。
  initial.sim = { ...initialLiveState.sim, func, needReset };
  return renderToStaticMarkup(createElement(LiveBar));
}

function button(html: string, title: string) {
  const buttons = html.match(/<button\b[^>]*>/g) ?? [];
  return buttons.find((b) => b.includes(`title="${title}"`));
}

describe("LiveBar", () => {
  it("対象関数が未設定なら実行とステップを無効にし選択を促す", () => {
    const html = render("");
    expect(button(html, "実行")).toContain('disabled=""');
    expect(button(html, "N 周期だけ進めて止める")).toContain('disabled=""');
    expect(html).toContain("対象関数を選択してください");
    expect(html).toContain('<option value="" selected="">');
  });

  it("対象関数を設定すると実行でき、要リセット中は引き続き無効", () => {
    const html = render("TEST_cycle");
    expect(button(html, "実行")).not.toContain('disabled=""');
    expect(button(html, "N 周期だけ進めて止める")).not.toContain('disabled=""');
    expect(html).not.toContain("対象関数を選択してください");
    const blocked = render("TEST_cycle", true);
    expect(button(blocked, "実行")).toContain('disabled=""');
    expect(button(blocked, "N 周期だけ進めて止める")).toContain('disabled=""');
  });

  it("仕向を選んでいなければ警告を出す（読み込み前・ファイルを読めないときは出さない）", () => {
    const doc = { version: 1 as const, selected: "", variants: [{ name: "国内", values: [] }] };
    Object.assign(variantsInitial, { doc, loaded: true, error: "" });
    expect(render("TEST_cycle")).toContain("仕向が選ばれていません");
    Object.assign(variantsInitial, { doc: { ...doc, selected: "国内" } });
    expect(render("TEST_cycle")).not.toContain("仕向が選ばれていません");
    expect(variantWarning({ doc, loaded: false, error: "" })).toBe("");
    expect(variantWarning({ doc, loaded: true, error: "壊れています" })).toBe("");
  });
});
