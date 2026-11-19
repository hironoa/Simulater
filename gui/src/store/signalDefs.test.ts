import { describe, expect, it } from "vitest";
import yaml from "js-yaml";
import { dumpSignals, effectiveDef, readDefs, readSignalDocument } from "./signalDefs";

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
