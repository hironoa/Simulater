import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { afterEach, describe, expect, it } from "vitest";
import { initialLiveState, useLiveStore } from "../../live/liveStore";
import { useWaveformStore } from "../../store/useWaveformStore";
import type { InputItem } from "../../live/persist";
import { InputPanel } from "./InputPanel";

const initial = useLiveStore.getInitialState();
const original = { ...initial };
const wave = useWaveformStore.getInitialState();
const originalWave = { ...wave };
afterEach(() => { Object.assign(initial, original); Object.assign(wave, originalWave); });
const item = (patch: Partial<InputItem> = {}): InputItem => ({ name: "temperature", widget: "number", min: 0, max: 100, step: 0.1, hold: true, ...patch });
function render(input: InputItem, current = 651, pending: string | undefined = undefined, held: string | undefined = undefined) {
  Object.assign(initial, { inputs: [input], values: { [input.name]: current }, sim: {
    ...initialLiveState.sim, pending: pending === undefined ? [] : [{ name: input.name, value: pending }],
    holds: held === undefined ? [] : [{ name: input.name, value: held }],
  } });
  return renderToStaticMarkup(createElement(InputPanel));
}

describe("入力カードの表示", () => {
  it("保存したraw651を物理値25.1で表示し、単位と生の値の案内を付ける", () => {
    Object.assign(wave, { lsbMapping: { temperature: "0.1" }, offsetMapping: { temperature: "-40" }, unitMapping: { temperature: "℃" } });
    const html = render(item({ value: 651 }), 650, "652", "651");
    expect(html).toContain('value="25.2"'); // 欄は保存値より反映待ちの値を優先する
    expect(html).toContain("物理値"); expect(html).toContain("℃");
    expect(html).toContain("反映待ち: 25.2"); expect(html).toContain("固定値: 25.1");
    expect(html).toContain("生の値: 現在 650 / 反映待ち 652 / 固定 651");
    expect(html).toContain("ドラッグで並べ替え");
  });
  it("保存値よりサーバの固定値を欄に出す（別のタブで固定値を変えたあと）", () => {
    Object.assign(wave, { lsbMapping: { temperature: "0.1" }, offsetMapping: { temperature: "-40" } });
    expect(render(item({ value: 651 }), 650, undefined, "652")).toContain('value="25.2"');
  });
  it("まだ書いていない状態のカードは『選んでください』を選び、今の値と同じ状態も選べる", () => {
    Object.assign(wave, { modeMapping: { temperature: { 0: "停止", 1: "運転" } } });
    const html = render(item({ widget: "state", holdPending: true }), 0);
    expect(html).toContain('<option value="" selected="">（選んでください。今は 0: 停止）</option>');
    expect(html).toContain('<option value="0">0: 停止</option>');
  });
  it("まだ書いていない固定には案内を表示する", () => {
    expect(render(item({ holdPending: true }))).toContain("値を入れると固定します");
  });
  it("状態を数値順に並べ、現在の値が一覧になければ値を表示する", () => {
    Object.assign(wave, { modeMapping: { temperature: { 2: "運転", 0: "停止" } } });
    const html = render(item({ widget: "state" }), 7);
    expect(html).toContain("（値 7）");
    expect(html.indexOf("0: 停止")).toBeLessThan(html.indexOf("2: 運転"));
    expect(html).toContain('<option value="state" selected="">状態</option>');
  });
  it("換算のない数値欄は文字列表記を保ち、トグルは換算しない", () => {
    expect(render(item({ value: "0x20000000000001" }))).toContain('value="0x20000000000001"');
    Object.assign(wave, { lsbMapping: { temperature: "0.1" }, offsetMapping: { temperature: "-40" } });
    const html = render(item({ widget: "toggle" }), 1);
    expect(html).not.toContain('>物理値<');
    expect(html).toContain("1（ON）");
  });
});
