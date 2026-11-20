import { describe, expect, it } from "vitest";
import yaml from "js-yaml";
import { dumpSignals, effectiveDef, readDefs, readSignalDocument } from "./signalDefs";

describe("信号名の正規化", () => {
  it("末尾名が一意なら順序と設定を保持し、曖昧な名前は保持する", () => {
    const definition = { unit: "V", variants: { EU: { unit: "mV" } } };
    const view = { group: "path/group" };
    const doc = readSignalDocument(yaml.dump({
      version: 2,
      signals: { first: {}, "path/file/y": definition, "a.c/x": {}, "b.c/x": {} },
      view: { signals: { first: {}, "path/file/y": view } },
    }));
    expect(Object.keys(doc.signals)).toEqual(["first", "y", "a.c/x", "b.c/x"]);
    expect(doc.signals.y).toEqual(definition);
    expect(Object.keys(doc.view.signals)).toEqual(["first", "y"]);
    expect(doc.view.signals.y).toEqual(view);
  });

  const rule = { signal: "path/x", when: [0], label: "path/x" };
  const normalized = { ...rule, signal: "x" };
  it.each([
    ["単独", rule, normalized],
    ["配列", [rule], [normalized]],
    ["rules 付き", { show: false, rules: [rule] }, { show: false, rules: [normalized] }],
  ])("inactive の信号名を正規化する（%s）", (_shape, inactive, expected) => {
    expect(readSignalDocument(yaml.dump({ inactive })).inactive).toEqual(expected);
  });
});

describe("dumpSignals", () => {
  it("数値に見える名前を保持し、状態名のキーだけ引用符を外す", () => {
    const doc = {
      version: 2,
      variants: ["01"],
      signals: {
        "123": {
          unit: "base",
          states: { 0: "OFF", 1: "01" },
          variants: { "01": { unit: "override", states: { 0: "ON" } } },
        },
      },
      view: { signals: { "123": { group: "7" } }, groups: { "7": { name: "7" } } },
    };
    const text = dumpSignals(doc);
    const loaded = readSignalDocument(text);
    const defs = readDefs(loaded);
    expect(loaded).toEqual(doc);
    expect(defs.variantNames).toEqual(["01"]);
    expect(Object.keys(defs.base)).toEqual(["123"]);
    expect(Object.keys(loaded.view.groups)).toEqual(["7"]);
    expect(effectiveDef(defs, "123", "01")).toEqual({ unit: "override", states: { 0: "ON", 1: "01" } });
    expect(text).toContain("      0: 'OFF'\n");
    expect(text).toContain("          0: 'ON'\n");
    expect(text).toContain("      1: '01'\n");
  });

  it("states の深い階層とブロック外の数値キーには触れない", () => {
    const doc = { states: { 0: { "01": "nested" }, 1: "OFF" }, after: { "01": "outside" } };
    const text = dumpSignals(doc);
    expect(text).toContain("  0:\n    '01': nested\n  1: 'OFF'\n");
    expect(text).toContain("after:\n  '01': outside\n");
    expect(yaml.load(text)).toEqual(doc);
  });
});
