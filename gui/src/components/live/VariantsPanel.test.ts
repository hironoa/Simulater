import type { StateCreator } from "zustand";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { isValidElement, type ReactElement, type ReactNode } from "react";
import { VariantsPanel } from "./VariantsPanel";
import { useWaveformStore } from "../../store/useWaveformStore";
import { useVariantsStore } from "../../live/variantsStore";
import { useLiveStore } from "../../live/liveStore";

const { setInit } = vi.hoisted(() => ({ setInit: vi.fn() }));
vi.mock("../../live/controller", () => ({ liveController: () => ({ api: { setInit } }) }));
vi.mock("zustand", async importOriginal => {
  const actual = await importOriginal<typeof import("zustand")>();
  return { ...actual, create: (initializer: StateCreator<unknown>) => {
    const store = actual.create(initializer);
    return Object.assign((selector?: (state: unknown) => unknown) => selector ? selector(store.getState()) : store.getState(), store);
  } };
});
vi.mock("react", async importOriginal => ({
  ...await importOriginal<typeof import("react")>(),
  useState: (value: unknown) => [value, vi.fn()],
  useRef: () => ({ current: null }),
}));
function button(node: ReactNode, label: string): ReactElement<{ onClick: () => void }> | undefined {
  if (!isValidElement<{ children?: ReactNode; onClick: () => void }>(node)) return;
  if (node.type === "button" && node.props.children === label) return node;
  for (const child of [node.props.children].flat(Infinity)) {
    const found = button(child, label);
    if (found) return found;
  }
}
beforeEach(() => {
  vi.clearAllMocks();
  vi.stubGlobal("window", { prompt: vi.fn(() => "EU2"), confirm: vi.fn(() => true) });
  useLiveStore.setState({ mode: "live" });
  useWaveformStore.setState(useWaveformStore.getInitialState());
  useWaveformStore.getState().parseMappingData("version: 2\nvariants: [EU]\nsignals:\n  X: {lsb: 1, variants: {EU: {lsb: 0.1}}}\n", "yaml", false);
  useWaveformStore.getState().setSelectedVariant("EU");
  useVariantsStore.setState({ ...useVariantsStore.getInitialState(), loaded: true,
    doc: { version: 1, selected: "EU", variants: [{ name: "EU", values: [] }] } });
  setInit.mockResolvedValue({ ok: true });
});
async function click(label: string) {
  const target = button(VariantsPanel(), label);
  expect(target).toBeDefined();
  target!.props.onClick();
  await Promise.resolve();
}
describe("仕向パネルの名前変更", () => {
  it("反映成功時に仕向と信号情報の名前・選択を同時に移す", async () => {
    await click("名前変更");
    expect(useVariantsStore.getState().doc.selected).toBe("EU2");
    expect(useWaveformStore.getState().selectedVariant).toBe("EU2");
    expect(useWaveformStore.getState().lsbMapping.X).toBe("0.1");
    expect(useWaveformStore.getState().signalDefs.variants).toEqual({ EU2: { X: { lsb: 0.1 } } });
  });
  it("反映失敗時は両方の旧名を保つ", async () => {
    setInit.mockRejectedValue(new Error("failed"));
    await click("名前変更");
    expect(useVariantsStore.getState().doc.selected).toBe("EU");
    expect(useWaveformStore.getState().selectedVariant).toBe("EU");
    expect(useWaveformStore.getState().signalDefs.variants.EU.X).toEqual({ lsb: 0.1 });
    expect(useVariantsStore.getState().errors[""]).toBe("failed");
  });
  it("信号情報にだけある仕向名への変更では両方の上書きを保つ", async () => {
    useWaveformStore.getState().parseMappingData("version: 2\nvariants: [EU, US]\nsignals:\n  X: {lsb: 1, variants: {EU: {lsb: 0.1}, US: {lsb: 0.2}}}\n", "yaml", false);
    vi.mocked(window.prompt).mockReturnValue("US");
    const beforeDoc = useVariantsStore.getState().doc;
    const beforeDefs = useWaveformStore.getState().signalDefs;
    await click("名前変更");
    expect(setInit).not.toHaveBeenCalled();
    expect(useVariantsStore.getState().doc).toBe(beforeDoc);
    expect(useWaveformStore.getState().selectedVariant).toBe("EU");
    expect(useWaveformStore.getState().signalDefs).toBe(beforeDefs);
    expect(useWaveformStore.getState().signalDefs.variants).toEqual({ EU: { X: { lsb: 0.1 } }, US: { X: { lsb: 0.2 } } });
  });
  it.each([null, "", "EU"])("取り消し・空名・既存名 %s では変更しない", async name => {
    vi.mocked(window.prompt).mockReturnValue(name);
    const before = useWaveformStore.getState().signalDefs;
    await click("名前変更");
    expect(setInit).not.toHaveBeenCalled();
    expect(useWaveformStore.getState().signalDefs).toBe(before);
  });
  it("仕向の削除では信号情報の上書きを残す", async () => {
    const before = useWaveformStore.getState().signalDefs;
    await click("削除");
    expect(useVariantsStore.getState().doc.variants).toEqual([]);
    expect(useWaveformStore.getState().signalDefs).toBe(before);
  });
});
