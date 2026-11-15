import { readFileSync } from "node:fs";
import { describe, expect, it, vi } from "vitest";
import { classifyReload } from "./classifyReload";
import { decodeWaveformCSV } from "./waveformCSV";

vi.mock("react-hot-toast", () => ({ default: Object.assign(vi.fn(), { error: vi.fn(), success: vi.fn() }) }));
import toast from "react-hot-toast";
import { useWaveformStore } from "../store/useWaveformStore";

const multiBlock = 'mod,eng_judge_gear,test,1,1\n#COMMENT,@rpm,eng_judge_gear@@\n,900,3\n;mod,ignored,comment,0,0\n "MoD" ,calc_duty,test,1,2\n#COMMENT,@temp,calc_duty@@,name\n,90,0.25,1\n';

const sample = (name: string) =>
  readFileSync(new URL(`../../docs/samples/compare/${name}`, import.meta.url), "utf8");

/** すべての系列が steps と同じ長さか */
function expectAligned(decoded: ReturnType<typeof decodeWaveformCSV>) {
  for (const h of decoded.headers) expect(decoded.chartData.seriesMap[h]).toHaveLength(decoded.chartData.steps.length);
  expect(Object.keys(decoded.chartData.seriesMap)).toEqual(decoded.headers);
}

describe("decodeWaveformCSV", () => {
  it("comment_col_only の空行を除き、コメント欄だけの周期も数える", () => {
    const d = decodeWaveformCSV("mod,TEST_cycle,t,0,1\n#COMMENT,call_cnt\n,,\nnote,\n,,\n,2\n");
    expect(d.chartData).toEqual({ steps: [1, 2], seriesMap: { call_cnt: [null, 2] } });
  });

  it("空白・引用つきのコメントと空行を除き、step N の1セル行も数える", () => {
    const d = decodeWaveformCSV('mod,TEST_cycle,t,0,1\n　 " #comment " ,A\n,,\n ,　 , \n　 " ; note " ,99\n " #skip " ,99\nstep 1,\nstep 2\n,3\n');
    expect(d.chartData).toEqual({ steps: [1, 2, 3], seriesMap: { A: [null, null, 3] } });
    expect(d.errors).toEqual([]);
    expectAligned(d);
  });

  it("実行側と同じく引用された空セルだけの行もデータ行とする", () => {
    const d = decodeWaveformCSV('mod,TEST_cycle,t,0,1\n#COMMENT,A\n"",\n');
    expect(d.chartData).toEqual({ steps: [1], seriesMap: { A: [null] } });
  });

  it("従来形式でも空行と # 行だけを除き、数値の1列目を保つ", () => {
    const d = decodeWaveformCSV("#COMMENT,A\n0,7\n,,\n#skip,99\n10,8\n");
    expect(d.chartData).toEqual({ steps: [1, 2], seriesMap: { A: [7, 8] } });
  });

  it("自動再読込と A/B 比較も実行するデータ行だけを数える", () => {
    const head = "mod,TEST_cycle,t,0,1\n#COMMENT,A\n";
    const skipped = ",,\n#skip,99\n";
    expect(classifyReload(head, head + skipped)).toBe("incomplete");
    const first = head + "note,\n";
    const next = first + skipped + ",2\n";
    expect(classifyReload(first, next)).toBe("append");
    expect(useWaveformStore.getState().parseWaveformCSV(next, "A.csv")).toBe(true);
    expect(useWaveformStore.getState().loadCompareCSV(first + ",2\n", "B.csv")).toBe(true);
    const state = useWaveformStore.getState();
    expect(state.chartData).toEqual(state.compareChartData);
    expect(state.chartData.steps).toEqual([1, 2]);
  });

  it("複数 mod ブロックは先頭だけを読み、総数を errors とは別に警告する", () => {
    const d = decodeWaveformCSV(multiBlock);
    expect(d.chartData).toEqual({ steps: [1], seriesMap: { "@rpm": [900], "eng_judge_gear@@": [3] } });
    expect(d.errors).toEqual([]);
    expect(d.warnings).toHaveLength(1);
    expect(d.warnings[0]).toContain("2 個");
    expect(d.warnings[0]).toContain("先頭のブロックだけを表示");
  });

  it("mod がない複数 #COMMENT は2つ目から除外し、総数を警告する", () => {
    const d = decodeWaveformCSV("#COMMENT,A\n0,7\n#COMMENT,B\n0,99\n#COMMENT,C\n0,100\n");
    expect(d.chartData).toEqual({ steps: [1], seriesMap: { A: [7] } });
    expect(d.errors).toEqual([]);
    expect(d.warnings[0]).toContain("3 個");
  });

  it.each(["parseWaveformCSV", "loadCompareCSV"] as const)("%s は読込を成功させ、警告を表示する", (method) => {
    vi.mocked(toast).mockClear();
    expect(useWaveformStore.getState()[method](multiBlock, "multi.csv")).toBe(true);
    expect(toast).toHaveBeenCalledWith(expect.stringContaining("2 個"), expect.any(Object));
    const state = useWaveformStore.getState();
    const data = method === "parseWaveformCSV" ? state.chartData : state.compareChartData;
    expect(data?.seriesMap["eng_judge_gear@@"]).toEqual([3]);
  });

  it("同じ変数の入力列と出力列は、mod 行から出力側の列を採る", () => {
    const d = decodeWaveformCSV("mod,main,GUI記録,2,2\r\n#COMMENT,P1,P2,P2,P1\r\n,7,1,2,9\r\n,,3,4,8\r\n");
    expect(d.headers).toEqual(["P1", "P2"]);
    expect(d.chartData.steps).toEqual([1, 2]);
    expect(d.chartData.seriesMap).toEqual({ P1: [9, 8], P2: [2, 4] });
    expectAligned(d);
  });

  it("サーバのエクスポート（P1 を入力と監視の両方に使った 1 ステップ）は 1 点になる", () => {
    const d = decodeWaveformCSV("mod,main,GUI記録,1,1\r\n#COMMENT,P1,P1\r\n,7,7\r\n");
    expect(d.chartData.seriesMap).toEqual({ P1: [7] });
    expect(d.chartData.steps).toEqual([1]);
  });

  it("mod 行がなければ同名の列は最後の列を採る", () => {
    const d = decodeWaveformCSV("#COMMENT,A,B,A\n0,1,10,2\n1,3,11,4\n");
    expect(d.warnings).toEqual([]);
    expect(d.headers).toEqual(["A", "B"]);
    expect(d.chartData.seriesMap).toEqual({ A: [2, 4], B: [10, 11] });
    expectAligned(d);
  });

  it("mod 行の入力数・出力数が読めなければ最後の列を採る", () => {
    const d = decodeWaveformCSV("mod,main,題名,x,1\n#COMMENT,A,A\n,1,2\n");
    expect(d.chartData.seriesMap).toEqual({ A: [2] });
  });

  it("出力に同名がなければ入力どうしの同名は最後の列を採る", () => {
    const d = decodeWaveformCSV("mod,main,題名,2,1\n#COMMENT,A,A,B\n,1,2,3\n");
    expect(d.chartData.seriesMap).toEqual({ A: [2], B: [3] });
  });

  it("パスを除いた名前が衝突する列だけパスつきの最短の名前にする", () => {
    const d = decodeWaveformCSV("#COMMENT,src/engine.c/s_cnt,src/sensor.c/s_cnt,Sample/Speed\n0,1,2,3\n1,4,5,6\n");
    expect(d.headers).toEqual(["engine.c/s_cnt", "sensor.c/s_cnt", "Speed"]);
    expect(d.chartData.seriesMap).toEqual({ "engine.c/s_cnt": [1, 4], "sensor.c/s_cnt": [2, 5], Speed: [3, 6] });
    expectAligned(d);
  });

  it("末尾が同じパスでも区別がつくところまで遡る", () => {
    const d = decodeWaveformCSV("#COMMENT,a/x/v,b/x/v,v\n0,1,2,3\n");
    expect(d.headers).toEqual(["a/x/v", "b/x/v", "v"]);
    expectAligned(d);
  });

  it("結果 CSV の実測?(期待) は実測値を採り、判定・時間の列は読まない", () => {
    const text = [
      "mod,func1,func1単体テスト,2,2",
      "#COMMENT,@enable,@mode,gb_result.data,gb_result.ret_code",
      ";,自由なコメント",
      ",1,1,100,1,OK,0.0003ms",
      ",1,2,\"5?(7)\",0x10?(0x11),NG,0.0004ms",
      ",1,3,'abc',,NO Check,0.0002ms",
    ].join("\r\n");
    const d = decodeWaveformCSV(text);
    expect(d.headers).toEqual(["@enable", "@mode", "gb_result.data", "gb_result.ret_code"]);
    expect(d.chartData.seriesMap).toEqual({
      "@enable": [1, 1, 1],
      "@mode": [1, 2, 3],
      "gb_result.data": [100, 5, null],
      "gb_result.ret_code": [1, 16, null],
    });
    expectAligned(d);
  });

  it("SignalViewer2 形式のサンプルは従来どおり読める", () => {
    for (const name of ["engine_test_A.csv", "engine_test_B.csv"]) {
      const d = decodeWaveformCSV(sample(name));
      expect(d.headerIdx).toBe(0);
      expect(d.errors).toEqual([]);
      expect(d.warnings).toEqual([]);
      expect(d.headers).toContain("EngineSpeed");
      expect(d.headers.every((h) => !h.includes("/"))).toBe(true);
      const lines = sample(name).trim().split(/\r?\n/).filter((line) => !line.startsWith(";"));
      const legacyRows = lines.slice(1).map((line) => line.split(","));
      expect(d.chartData.steps).toEqual(legacyRows.map((_, i) => i + 1));
      d.headers.forEach((h, col) => {
        expect(d.chartData.seriesMap[h]).toEqual(legacyRows.map((row) => row[col + 1].trim() === "" ? null : Number(row[col + 1])));
      });
      expectAligned(d);
    }
    const a = decodeWaveformCSV(sample("engine_test_A.csv"));
    expect(a.headers).toEqual(["PIG", "EngineSpeed", "WaterTemp", "JudgeFlag", "CurrentGear", "DriveMode", "OldSignal"]);
    expect(a.chartData.seriesMap.EngineSpeed.slice(0, 3)).toEqual([8000, 8040, 8080]);
  });
});

describe("classifyReload と同名の列", () => {
  it.each([
    ["mod,main,t,0,1\n#COMMENT,A\n,7\n", "mod,sub,t,0,1\n#COMMENT,B\n,99\n"],
    ["#COMMENT,A\n0,7\n", "#COMMENT,B\n0,99\n"],
  ])("後続ブロックの追加を先頭ブロックのデータ増加と扱わない", (first, second) => {
    expect(classifyReload(first, first + second)).toBe("restart");
    expect(decodeWaveformCSV(first + second).chartData).toEqual(decodeWaveformCSV(first).chartData);
  });

  it("同名の列がある記録に行を足すと追記と判定する", () => {
    const head = "mod,main,GUI記録,1,1\r\n#COMMENT,P1,P1\r\n,7,7\r\n";
    expect(classifyReload(head, head + ",8,8\r\n")).toBe("append");
    expect(decodeWaveformCSV(head + ",8,8\r\n").chartData.seriesMap.P1).toEqual([7, 8]);
  });
});

describe("modi initialization metadata", () => {
  it("skips initialization after an early COMMENT and recognizes the next modi block", () => {
    const d = decodeWaveformCSV("modi,f,t,1,1\n#COMMENT,A,B\n#InitWheneverCall,A\n0,999\n,1,2\nmodi,g,t,0,1\n#COMMENT,C\n,4\n");
    expect(d.chartData).toEqual({ steps: [1], seriesMap: { A: [1], B: [2] } });
    expect(d.warnings).toHaveLength(1);
  });
});
