import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createMomentaryButton } from "./momentaryButton";

function setup(minimum = 100) {
  let value: number | string | null = 7;
  const write = vi.fn(async () => { value = 1; return true; });
  const release = vi.fn(async (restore: number | string | null) => { value = restore; });
  const changed = vi.fn();
  const button = createMomentaryButton({
    restoreValue: () => value, write, release, changed,
    minimumMs: () => minimum, now: () => Date.now(),
  });
  return { button, write, release, changed, value: () => value };
}

describe("押している間だけ 1 の遅延解除", () => {
  beforeEach(() => { vi.useFakeTimers(); });
  afterEach(() => { vi.useRealTimers(); });

  it("0ms のクリック後、30ms に再押下して保持しても古い100msの解除は実行しない", async () => {
    const { button, write, release, value } = setup();
    await button.press();
    await button.up();
    await vi.advanceTimersByTimeAsync(30);
    await button.press();
    await vi.advanceTimersByTimeAsync(100);
    expect(write).toHaveBeenCalledTimes(2);
    expect(release).not.toHaveBeenCalled();
    expect(value()).toBe(1);
    await button.up();
    expect(release).toHaveBeenCalledExactlyOnceWith(7);
    await vi.advanceTimersByTimeAsync(500);
    expect(release).toHaveBeenCalledTimes(1);
  });

  it("再押下をすぐ離すと最後の押下から100msまで待ち、最初の値を復帰する", async () => {
    const { button, release, value } = setup();
    await button.press();
    await button.up();
    await vi.advanceTimersByTimeAsync(30);
    await button.press();
    await button.up();
    await vi.advanceTimersByTimeAsync(99);
    expect(release).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1);
    expect(release).toHaveBeenCalledExactlyOnceWith(7);
    expect(value()).toBe(7);
  });

  it("通常の短いクリックは100msまで固定する", async () => {
    const { button, release } = setup();
    await button.press();
    await vi.advanceTimersByTimeAsync(10);
    await button.up();
    await vi.advanceTimersByTimeAsync(89);
    expect(release).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1);
    expect(release).toHaveBeenCalledExactlyOnceWith(7);
  });

  it("長押しは時間だけでは解除せず、離したら1回だけ解除する", async () => {
    const { button, write, release } = setup();
    await button.press();
    await button.press();
    await vi.advanceTimersByTimeAsync(1000);
    expect(write).toHaveBeenCalledTimes(1);
    expect(release).not.toHaveBeenCalled();
    await button.up();
    await button.up();
    expect(release).toHaveBeenCalledTimes(1);
  });

  it("3周期の方が長いときはその時間まで待つ", async () => {
    const { button, release } = setup(3000);
    await button.press();
    await button.up();
    await vi.advanceTimersByTimeAsync(2999);
    expect(release).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1);
    expect(release).toHaveBeenCalledTimes(1);
  });

  it.each(["取り外し", "pointercancel", "フォーカス喪失"])("%s は押下中でも即座に解除する", async () => {
    const { button, release } = setup();
    await button.press();
    await button.cancel();
    await button.cancel();
    await vi.advanceTimersByTimeAsync(200);
    expect(release).toHaveBeenCalledExactlyOnceWith(7);
  });

  it("遅延解除待ちで取り外しても1回だけ解除する", async () => {
    const { button, release } = setup();
    await button.press();
    await button.up();
    await button.cancel();
    await vi.advanceTimersByTimeAsync(200);
    expect(release).toHaveBeenCalledTimes(1);
  });

  it("書き込み応答より先に取り外しても応答後に解除する", async () => {
    const { button, write, release } = setup();
    let finish!: (ok: boolean) => void;
    write.mockImplementationOnce(() => new Promise<boolean>((resolve) => { finish = resolve; }));
    const down = button.press();
    await Promise.resolve();
    const removed = button.cancel();
    expect(release).not.toHaveBeenCalled();
    finish(true);
    await down;
    await removed;
    expect(release).toHaveBeenCalledExactlyOnceWith(7);
  });

  it("拒否された押下は解除を予約せず、次の押下で新しい復帰値を採る", async () => {
    const { button, write, release, changed } = setup();
    write.mockResolvedValueOnce(false);
    await button.press();
    expect(changed).toHaveBeenLastCalledWith(false);
    await button.up();
    await vi.advanceTimersByTimeAsync(200);
    expect(release).not.toHaveBeenCalled();
    await button.press();
    await button.up();
    await vi.advanceTimersByTimeAsync(100);
    expect(release).toHaveBeenCalledExactlyOnceWith(7);
  });

  it("解除通信中の再押下も直列化し、最初の復帰値を保持する", async () => {
    const { button, write, release } = setup();
    let finish!: () => void;
    release.mockImplementationOnce(() => new Promise<void>((resolve) => { finish = resolve; }));
    await button.press();
    await vi.advanceTimersByTimeAsync(100);
    const up = button.up();
    await Promise.resolve();
    const down = button.press();
    finish();
    await up;
    await down;
    expect(write).toHaveBeenCalledTimes(2);
    await button.up();
    await vi.advanceTimersByTimeAsync(100);
    expect(release.mock.calls).toEqual([[7], [7]]);
  });
});
