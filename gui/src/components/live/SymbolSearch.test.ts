import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, it, vi } from "vitest";
import { createApi, type SymbolEntry } from "../../live/api";
import { resolveMembers, SymbolRow } from "./SymbolSearch";

const parent: SymbolEntry = { name: "g_cal", type: "cal_t", size: 4, kind: "struct", writable: false, members: ["g_cal.lo"] };
const child: SymbolEntry = { name: "g_cal.lo", type: "uint8", size: 1, kind: "uint", writable: true, members: [] };
const api = () => ({ ...createApi(), symbols: vi.fn() });

describe("SymbolSearch members", () => {
  it("完全一致の子の型と writable を保持する", async () => {
    const a = api();
    a.symbols.mockResolvedValue([{ ...child, name: "g_cal.lower" }, child]);
    expect(await resolveMembers(parent, a)).toEqual([child]);
    expect(a.symbols).toHaveBeenCalledWith("g_cal.lo", 20);
  });

  it("展開できるポインタ自体も入力と監視に追加できる", () => {
    const html = renderToStaticMarkup(createElement(SymbolRow, {
      entry: { ...child, name: "gp_cal", kind: "ptr", members: ["gp_cal->lo"] }, depth: 0,
      actions: [{ label: "入力", enabled: (s) => s.writable, onPick: vi.fn() }],
    }));
    expect(html).toContain("メンバを展開");
    expect(html).toContain("入力");
    expect(html).not.toContain('disabled=""');
  });

  it.each(["missing", "network"])("%s: 解決できない子を表示して全操作を無効にする", async (failure) => {
    const a = api();
    if (failure === "network") a.symbols.mockRejectedValue(new Error("offline"));
    else a.symbols.mockResolvedValue([{ ...child, name: "g_cal.lower" }]);
    for (const writable of [false, true]) {
      const [entry] = await resolveMembers({ ...parent, writable }, a);
      expect(entry.resolutionError).toBe(true);
      expect(entry.writable).toBe(false);
      const html = renderToStaticMarkup(createElement(SymbolRow, { entry, depth: 1, actions: [
        { label: "入力", enabled: () => true, onPick: vi.fn() },
        { label: "監視", onPick: vi.fn() },
      ] }));
      expect(html).toContain("解決できません");
      expect(html.match(/disabled=""/g)).toHaveLength(2);
    }
  });
});
