import type { ChartData } from "../types";

/**
 * ライブ記録。`samples` で届く行を列ごとの Float64Array に追記して持つ（読めない値は NaN）。
 *
 * - React の状態には載せない。描画側は `revision` の変化を requestAnimationFrame で見て間引いて描く。
 * - 上限（既定 50,000 点/信号。周期 5ms で約 4 分、1ms で 50 秒）を超えたら、古い側から上限の 10% をまとめて捨てる。
 *   一時停止したときの Viewer は見えている範囲だけを絞って描く（components/Chart.tsx）が、x 軸の目盛りなど
 *   点数に比例する処理が残る（実測: 15 信号でカーソル移動 1 回が 60,000 点で約 0.2〜0.5 秒、200,000 点で約 0.5〜0.9 秒。
 *   200,000 点で JS のヒープ約 375MB）。
 *   記録そのものを間引き（点を飛ばす）にしないのは、Viewer がステップを等間隔として時刻を計算するため。
 *   全記録はサーバ側にあり `/api/export.csv` で取れる。
 * - ステップ番号が巻き戻ったら（reset など）記録を作り直す。
 */
export const DEFAULT_MAX_SAMPLES = 50_000;

export interface AppendResult {
  appended: number;
  restarted: boolean;
  dropped: number;
}

export class LiveRecord {
  private cols = new Map<string, Float64Array>();
  private order: string[] = [];
  private stepBuf: Float64Array;
  private len = 0;
  private cap: number;
  /** 追記・消去のたびに増える。描画側が変化を検出するのに使う。 */
  revision = 0;
  /** 上限超過で捨てた点の累計（今の記録の先頭より前にあった点の数）。 */
  droppedTotal = 0;

  constructor(public maxSamples = DEFAULT_MAX_SAMPLES, initialCapacity = 4096) {
    this.cap = Math.max(16, initialCapacity);
    this.stepBuf = new Float64Array(this.cap);
  }

  private nameListeners = new Set<() => void>();
  subscribeNames = (listener: () => void) => {
    this.nameListeners.add(listener);
    return () => { this.nameListeners.delete(listener); };
  };
  getNames = () => this.names;

  get length() { return this.len; }
  get names(): readonly string[] { return this.order; }
  get firstStep(): number | null { return this.len ? this.stepBuf[0] : null; }
  get lastStep(): number | null { return this.len ? this.stepBuf[this.len - 1] : null; }

  clear() {
    this.cols.clear();
    this.order = [];
    this.len = 0;
    this.nameListeners.forEach(listener => listener());
    this.droppedTotal = 0;
    this.revision++;
  }

  setMaxSamples(max: number) {
    this.maxSamples = Math.max(1000, Math.floor(max));
    const dropped = this.trim();
    if (dropped) this.revision++;
  }

  /** 列だけ先に作る（監視一覧が変わったときなど）。過去の分は NaN で埋める。 */
  ensureColumn(name: string): Float64Array {
    let col = this.cols.get(name);
    if (!col) {
      col = new Float64Array(this.cap);
      col.fill(NaN, 0, this.len);
      this.cols.set(name, col);
      this.order = [...this.order, name];
      this.nameListeners.forEach(listener => listener());
    }
    return col;
  }

  /** 列バッファを移す。同じ列への改名では、値があるセルをまとめる。 */
  renameColumns(renames: ReadonlyMap<string, string>) {
    if (!this.order.some(name => renames.has(name))) return false;
    const cols = new Map<string, Float64Array>();
    for (const name of this.order) {
      const target = renames.get(name) ?? name;
      const source = this.cols.get(name)!;
      const existing = cols.get(target);
      if (existing) {
        for (let i = 0; i < this.len; i++) {
          if (!Number.isNaN(source[i])) existing[i] = source[i];
        }
      } else {
        cols.set(target, source);
      }
    }
    this.cols = cols;
    this.order = [...cols.keys()];
    this.revision++;
    this.nameListeners.forEach(listener => listener());
    return true;
  }

  private grow(need: number) {
    if (need <= this.cap) return;
    let cap = this.cap;
    while (cap < need) cap *= 2;
    const resize = (src: Float64Array) => {
      const dst = new Float64Array(cap);
      dst.set(src.subarray(0, this.len));
      return dst;
    };
    this.stepBuf = resize(this.stepBuf);
    for (const [name, col] of this.cols) this.cols.set(name, resize(col));
    this.cap = cap;
  }

  private trim(): number {
    if (this.len <= this.maxSamples) return 0;
    const chunk = Math.max(1, Math.floor(this.maxSamples * 0.1));
    const drop = this.len - this.maxSamples + chunk;
    this.stepBuf.copyWithin(0, drop, this.len);
    for (const col of this.cols.values()) col.copyWithin(0, drop, this.len);
    this.len -= drop;
    this.droppedTotal += drop;
    return drop;
  }

  append(step0: number, names: readonly string[], rows: readonly (readonly unknown[])[]): AppendResult {
    if (!rows.length) return { appended: 0, restarted: false, dropped: 0 };
    let restarted = false;
    const last = this.lastStep;
    if (last !== null && step0 <= last) {
      this.clear();
      restarted = true;
    }
    this.grow(this.len + rows.length);
    const targets = names.map((n) => this.ensureColumn(n));
    const present = new Set(names);
    const absent = this.order.filter((n) => !present.has(n)).map((n) => this.cols.get(n)!);
    const base = this.len;
    for (let r = 0; r < rows.length; r++) {
      const row = rows[r];
      const at = base + r;
      this.stepBuf[at] = step0 + r;
      for (let c = 0; c < targets.length; c++) {
        const v = row[c];
        targets[c][at] = typeof v === "number" && Number.isFinite(v) ? v : NaN;
      }
      for (const col of absent) col[at] = NaN;
    }
    this.len += rows.length;
    const dropped = this.trim();
    this.revision++;
    return { appended: rows.length, restarted, dropped };
  }

  stepAt(i: number) { return this.stepBuf[i]; }

  valueAt(name: string, i: number): number | null {
    const col = this.cols.get(name);
    if (!col || i < 0 || i >= this.len) return null;
    const v = col[i];
    return Number.isNaN(v) ? null : v;
  }

  latest(name: string): number | null {
    return this.valueAt(name, this.len - 1);
  }

  /** 直近 n 点の範囲 [start, end)。 */
  window(n: number): { start: number; end: number } {
    const end = this.len;
    return { start: Math.max(0, end - Math.max(1, Math.floor(n))), end };
  }

  column(name: string): Float64Array | undefined {
    const col = this.cols.get(name);
    return col ? col.subarray(0, this.len) : undefined;
  }

  steps(): Float64Array {
    return this.stepBuf.subarray(0, this.len);
  }

  /**
   * Viewer に渡す ChartData に変換する。Viewer は時刻を (step - 1) * 周期 で出すので、
   * シミュレータのステップ番号 s を s + 1 として渡し、時刻 = s * 周期 にする。
   */
  toChartData(names: readonly string[] = this.order): ChartData {
    const steps = new Array<number>(this.len);
    for (let i = 0; i < this.len; i++) steps[i] = this.stepBuf[i] + 1;
    const seriesMap: Record<string, (number | null)[]> = {};
    for (const name of names) {
      const col = this.cols.get(name);
      const out = new Array<number | null>(this.len);
      for (let i = 0; i < this.len; i++) {
        const v = col ? col[i] : NaN;
        out[i] = Number.isNaN(v) ? null : v;
      }
      seriesMap[name] = out;
    }
    return { seriesMap, steps };
  }
}

/** アプリ全体で 1 つのライブ記録。 */
export const liveRecord = new LiveRecord();
