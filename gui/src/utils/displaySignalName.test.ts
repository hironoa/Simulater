import { describe, expect, it } from "vitest";
import { displaySignalName } from "./displaySignalName";

describe("displaySignalName", () => {
  it.each([
    ["engine.c/s_cnt", "s_cnt"],
    ["src/foo.c/bar", "bar"],
    ["src\\foo.c/bar", "bar"],
    ["x", "x"],
    ["src\\foo.c", "src\\foo.c"],
    ["a.c/", "a.c/"],
    ["/", "/"],
    ["", ""],
  ])("%s の表示名は %s", (name, expected) => {
    expect(displaySignalName(name)).toBe(expected);
  });

  it.each([
    ["a.c/x", ["a.c/x", "b.c/x"]],
    ["b.c/x", ["a.c/x", "b.c/x"]],
    ["a.c/x", ["x", "a.c/x"]],
    ["x", ["x", "a.c/x"]],
  ])("短縮名が重なる %s は完全名を維持する", (name, all) => {
    expect(displaySignalName(name, all)).toBe(name);
  });

  it("同じ完全名の繰り返しは別信号との衝突にしない", () => {
    expect(displaySignalName("a.c/x", ["a.c/x", "a.c/x"])).toBe("x");
  });

  it("異なる末尾名と空の一覧では短縮し、一覧を変更しない", () => {
    const all = Object.freeze(["a.c/x", "b.c/y"]);
    expect(displaySignalName("a.c/x", all)).toBe("x");
    expect(all).toEqual(["a.c/x", "b.c/y"]);
    expect(displaySignalName("a.c/x", [])).toBe("x");
  });
});
