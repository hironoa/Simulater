/** 押下状態と遅延解除を管理する。通信は直列化し、押下直後の取り外しも書き込み完了後に解除する。 */
export function createMomentaryButton(deps: {
  restoreValue: () => number | string | null;
  write: () => Promise<boolean>;
  release: (restore: number | string | null) => Promise<void>;
  minimumMs: () => number;
  changed: (pressed: boolean) => void;
  now?: () => number;
  schedule?: (fn: () => void, ms: number) => ReturnType<typeof setTimeout>;
  cancel?: (timer: ReturnType<typeof setTimeout>) => void;
}) {
  const now = deps.now ?? (() => performance.now());
  const schedule = deps.schedule ?? ((fn, ms) => setTimeout(fn, ms));
  const cancel = deps.cancel ?? clearTimeout;
  let generation = 0;
  let pressed = false;
  let pressedAt = 0;
  let timer: ReturnType<typeof setTimeout> | undefined;
  let session: { restore: number | string | null; held: boolean } | undefined;
  let tail = Promise.resolve();

  function clearTimer() {
    if (timer !== undefined) cancel(timer);
    timer = undefined;
  }

  function enqueue(fn: () => Promise<void>) {
    tail = tail.then(fn);
    return tail;
  }

  function release(current: NonNullable<typeof session>, token: number, force = false) {
    return enqueue(async () => {
      if (!force && (token !== generation || pressed)) return;
      if (current.held) {
        current.held = false;
        await deps.release(current.restore);
      }
      if (session === current && token === generation && !pressed) session = undefined;
    });
  }

  function press() {
    if (pressed) return tail;
    clearTimer();
    const token = ++generation;
    pressed = true;
    pressedAt = now();
    deps.changed(true);
    const current = session ??= { restore: deps.restoreValue(), held: false };
    return enqueue(async () => {
      const accepted = await deps.write();
      current.held ||= accepted;
      // 拒否された押下を残さない。先行する押下の固定があれば、それも解除する。
      if (!accepted && token === generation) {
        pressed = false;
        deps.changed(false);
        clearTimer();
        if (current.held) {
          current.held = false;
          await deps.release(current.restore);
        }
        if (session === current) session = undefined;
      }
    });
  }

  function up() {
    if (!pressed) return tail;
    pressed = false;
    deps.changed(false);
    const token = generation;
    const current = session!;
    const wait = Math.max(0, deps.minimumMs() - (now() - pressedAt));
    if (!wait) return release(current, token);
    timer = schedule(() => {
      timer = undefined;
      void release(current, token);
    }, wait);
    return tail;
  }

  /** 取り外し・操作中断時は最低時間を待たずに解除する。重複通知は何もしない。 */
  function cancelPress() {
    clearTimer();
    const token = ++generation;
    pressed = false;
    deps.changed(false);
    const current = session;
    session = undefined;
    return current ? release(current, token, true) : tail;
  }

  return { press, up, cancel: cancelPress };
}
