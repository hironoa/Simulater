import React, {
  useEffect,
  useMemo,
  useRef,
  useState,
  useLayoutEffect,
} from "react";
import { Settings } from "lucide-react";
import { useWaveformStore } from "../../../store/useWaveformStore";
import { InactiveCondition, InactiveRule } from "../../../types";
import { computeInactive, INACTIVE_COLOR } from "../../../utils/inactiveRanges";
import { displaySignalName } from "../../../utils/displaySignalName";

const fieldClass =
  "min-w-0 rounded border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800 px-1 py-0.5";

function DraftInput({
  value,
  commit,
  validate = () => true,
  placeholder,
}: {
  value: string;
  commit: (v: string) => void;
  validate?: (v: string) => boolean;
  placeholder?: string;
}) {
  const [draft, setDraft] = useState(value);
  useEffect(() => setDraft(value), [value]);
  const valid = validate(draft);

  return (
    <input
      aria-label={placeholder}
      placeholder={placeholder}
      className={`${fieldClass} w-full ${valid ? "" : "border-red-500 outline-red-500"}`}
      value={draft}
      onChange={(e) => setDraft(e.target.value)}
      onBlur={() => {
        if (valid && draft !== value) commit(draft);
      }}
      onKeyDown={(e) => {
        if (e.key === "Enter") e.currentTarget.blur();
      }}
    />
  );
}

const numberOK = (s: string) => s.trim() !== "" && Number.isFinite(Number(s));

function RangeInput({
  condition,
  commit,
  remove,
}: {
  condition: Exclude<InactiveCondition, number>;
  commit: (c: Exclude<InactiveCondition, number>) => void;
  remove: () => void;
}) {
  const [min, setMin] = useState(condition.min?.toString() ?? "");
  const [max, setMax] = useState(condition.max?.toString() ?? "");
  useEffect(() => {
    setMin(condition.min?.toString() ?? "");
    setMax(condition.max?.toString() ?? "");
  }, [condition]);
  const valid =
    (!min.trim() || numberOK(min)) &&
    (!max.trim() || numberOK(max)) &&
    (!!min.trim() || !!max.trim()) &&
    (!min.trim() || !max.trim() || Number(min) <= Number(max));
  const save = () => {
    if (valid) {
      commit({
        ...(min.trim() ? { min: Number(min) } : {}),
        ...(max.trim() ? { max: Number(max) } : {}),
      });
    }
  };

  return (
    <div
      className="flex gap-1 items-center mt-1"
      onKeyDown={(e) => {
        if (e.key === "Enter") (e.target as HTMLInputElement).blur();
      }}
    >
      <input
        aria-label="範囲の下限"
        placeholder="下限なし"
        className={`${fieldClass} w-1/2 ${valid ? "" : "border-red-500"}`}
        value={min}
        onChange={(e) => setMin(e.target.value)}
        onBlur={save}
      />
      〜
      <input
        aria-label="範囲の上限"
        placeholder="上限なし"
        className={`${fieldClass} w-1/2 ${valid ? "" : "border-red-500"}`}
        value={max}
        onChange={(e) => setMax(e.target.value)}
        onBlur={save}
      />
      <button aria-label="範囲を削除" onClick={remove}>
        ×
      </button>
    </div>
  );
}

function RuleCard({
  rule,
  update,
  remove,
  count,
  seconds,
}: {
  rule: InactiveRule;
  update: (patch: Partial<InactiveRule>) => void;
  remove: () => void;
  count: number;
  seconds: number;
}) {
  const chartData = useWaveformStore((s) => s.chartData);
  const states = useWaveformStore((s) => s.modeMapping[rule.signal]);
  const settings = useWaveformStore((s) => s.signalSettings);
  const showAlias = useWaveformStore((s) => s.showAlias);
  const names = Object.keys(chartData?.seriesMap ?? {});
  const missing = rule.signal && !chartData?.seriesMap[rule.signal];
  if (missing) names.unshift(rule.signal);
  const [newValue, setNewValue] = useState("");
  const addValue = () => {
    if (numberOK(newValue)) {
      const v = Number(newValue);
      if (!rule.when.includes(v)) update({ when: [...rule.when, v] });
      setNewValue("");
    }
  };

  return (
    <div className="border border-gray-300 dark:border-slate-600 rounded p-2 space-y-1.5">
      <div className="flex gap-1 items-center">
        <select
          aria-label="条件信号"
          className={`${fieldClass} flex-1 w-0`}
          value={rule.signal}
          onChange={(e) => update({ signal: e.target.value })}
        >
          <option value="">条件信号を選択</option>
          {names.map((sig) => (
            <option key={sig} value={sig} title={sig}>
              {(!(missing && sig === rule.signal) && showAlias && settings[sig]?.alias) || displaySignalName(sig, names)}
              {missing && sig === rule.signal ? "（未読込）" : ""}
            </option>
          ))}
        </select>
        <input
          aria-label="帯の色"
          type="color"
          className="w-6 h-6"
          value={rule.color || INACTIVE_COLOR}
          onChange={(e) => update({ color: e.target.value })}
        />
        <button aria-label="ルールを削除" onClick={remove}>
          ×
        </button>
      </div>
      <div>無効な値</div>
      <div className="flex flex-wrap gap-1">
        {Object.entries(states ?? {}).map(([value, label]) => {
          const selected = rule.when.includes(Number(value));
          return (
            <button
              key={value}
              className={`rounded border px-1 ${selected ? "bg-blue-500 text-white border-blue-500" : "border-gray-300 dark:border-slate-500"}`}
              onClick={() =>
                update({
                  when: selected
                    ? rule.when.filter((c) => c !== Number(value))
                    : [...rule.when, Number(value)],
                })
              }
            >
              {value} : {label}
            </button>
          );
        })}
        {rule.when.map((c, i) =>
          typeof c === "number" && (!states || states[c] === undefined) ? (
            <button
              key={i}
              className="border rounded px-1"
              onClick={() =>
                update({ when: rule.when.filter((_, j) => j !== i) })
              }
            >
              {c} ×
            </button>
          ) : null,
        )}
      </div>
      {!Object.keys(states ?? {}).length && (
        <div className="flex gap-1 items-center">
          <input
            aria-label="追加する生値"
            placeholder="生値を入力"
            className={`${fieldClass} flex-1 min-w-0 h-6 ${newValue && !numberOK(newValue) ? "border-red-500" : ""}`}
            value={newValue}
            onChange={(e) => setNewValue(e.target.value)}
            onBlur={addValue}
            onKeyDown={(e) => {
              if (e.key === "Enter") addValue();
            }}
          />
          <button
            className="shrink-0 whitespace-nowrap rounded border px-1 border-gray-300 dark:border-slate-500 hover:bg-gray-100 dark:hover:bg-slate-700"
            onClick={addValue}
          >
            追加
          </button>
        </div>
      )}
      {rule.when.map((c, i) =>
        typeof c !== "number" ? (
          <div key={i}>
            <RangeInput
              condition={c}
              commit={(condition) =>
                update({
                  when: rule.when.map((v, j) => (i === j ? condition : v)),
                })
              }
              remove={() =>
                update({ when: rule.when.filter((_, j) => j !== i) })
              }
            />
          </div>
        ) : null,
      )}
      <button
        className="text-blue-600 dark:text-blue-300"
        onClick={() => update({ when: [...rule.when, {}] })}
      >
        ＋ 範囲
      </button>
      <label className="block">
        見出し
        <DraftInput value={rule.label} commit={(label) => update({ label })} />
      </label>
      <div className="text-gray-500 dark:text-gray-400">
        該当 {count} 区間・{seconds.toFixed(2)} s
        {count === 0 ? "（該当なし）" : ""}
      </div>
    </div>
  );
}

export function InactiveRulesEditor() {
  const config = useWaveformStore((s) => s.inactive);
  const setConfig = useWaveformStore((s) => s.setInactive);
  const open = useWaveformStore((s) => s.inactiveEditorOpen);
  const setOpen = useWaveformStore((s) => s.openInactiveEditor);
  const data = useWaveformStore((s) => s.chartData);
  const period = useWaveformStore((s) => s.taskPeriod);
  const result = useMemo(() => computeInactive(data, config), [data, config]);
  const root = useRef<HTMLDivElement>(null);
  const chipBox = useRef<HTMLDivElement>(null);
  const [visible, setVisible] = useState(0);

  useLayoutEffect(() => {
    const box = chipBox.current;
    if (!box) return;
    const measure = () => {
      let width = 0;
      let count = 0;
      const chips = box.querySelectorAll<HTMLElement>("[data-measure-chip]");
      chips.forEach((chip) => {
        width += chip.offsetWidth + 4;
        if (width <= box.clientWidth - 28) count++;
      });
      setVisible(count);
    };
    const observer = new ResizeObserver(measure);
    observer.observe(box);
    measure();
    return () => observer.disconnect();
  }, [config.rules]);

  useEffect(() => {
    if (!open) return;
    const outside = (e: PointerEvent) => {
      if (!root.current?.contains(e.target as Node)) setOpen(false);
    };
    const key = (e: KeyboardEvent) => {
      if (e.key === "Escape") setOpen(false);
    };
    document.addEventListener("pointerdown", outside);
    document.addEventListener("keydown", key);
    return () => {
      document.removeEventListener("pointerdown", outside);
      document.removeEventListener("keydown", key);
    };
  }, [open, setOpen]);

  const chip = (rule: InactiveRule, i: number, measure = false) => (
    <span
      key={i}
      data-measure-chip={measure || undefined}
      className="inline-flex items-center gap-1 shrink-0 whitespace-nowrap"
    >
      <span
        className="w-2 h-2 inline-block"
        style={{ backgroundColor: rule.color || INACTIVE_COLOR }}
      />
      {rule.label}
    </span>
  );

  return (
    <div ref={root} className="relative mt-2 text-[11px] font-normal">
      <div className="flex items-center gap-1 min-w-0">
        <span className="shrink-0">無効区間:</span>
        {!config.rules.length ? (
          <span className="flex-1">なし</span>
        ) : (
          <>
            <div
              ref={chipBox}
              className="flex-1 min-w-0 relative overflow-hidden"
            >
              <div className="absolute invisible flex gap-1">
                {config.rules.map((r, i) => chip(r, i, true))}
              </div>
              <div className="flex gap-1">
                {config.rules.slice(0, visible).map((r, i) => chip(r, i))}
                {visible < config.rules.length && (
                  <button
                    title={config.rules
                      .slice(visible)
                      .map((r) => r.label)
                      .join(", ")}
                    onClick={() => setOpen(true)}
                  >
                    +{config.rules.length - visible}
                  </button>
                )}
              </div>
            </div>
            <div className="flex border border-gray-300 dark:border-slate-600 rounded overflow-hidden">
              {[true, false].map((show) => (
                <button
                  key={String(show)}
                  className={`px-1 ${config.show === show ? "bg-blue-500 text-white" : "bg-white dark:bg-slate-800"}`}
                  onClick={() => setConfig({ ...config, show })}
                >
                  {show ? "ON" : "OFF"}
                </button>
              ))}
            </div>
          </>
        )}
        <button
          aria-label="無効区間の設定を開く"
          className="px-2 py-0 text-[11px] bg-white dark:bg-slate-700 text-blue-500 dark:text-blue-300 border border-blue-500 rounded hover:bg-blue-50 dark:hover:bg-slate-600 flex items-center gap-1 shrink-0 whitespace-nowrap"
          onClick={() => setOpen(!open)}
        >
          <Settings size={12} />設定
        </button>
      </div>
      {open && (
        <div
          role="dialog"
          aria-label="無効区間の設定"
          className="absolute top-full left-0 right-0 mt-1 z-50 bg-white dark:bg-slate-800 text-gray-800 dark:text-gray-200 border border-gray-300 dark:border-slate-600 rounded shadow-xl p-2 max-h-[65vh] overflow-y-auto space-y-2"
        >
          <div className="flex justify-between font-bold">
            無効区間の設定
            <button aria-label="閉じる" onClick={() => setOpen(false)}>
              ×
            </button>
          </div>
          <p>どれかに当たれば無効（生値で判定）</p>
          {config.rules.map((rule, i) => (
            <div key={i}>
              <RuleCard
                rule={rule}
                update={(patch) =>
                  setConfig({
                    ...config,
                    rules: config.rules.map((r, j) =>
                      i === j ? { ...r, ...patch } : r,
                    ),
                  })
                }
                remove={() =>
                  setConfig({
                    ...config,
                    rules: config.rules.filter((_, j) => j !== i),
                  })
                }
                count={result.perRule[i].ranges.length}
                seconds={(result.perRule[i].steps * period) / 1000}
              />
            </div>
          ))}
          <button
            className="text-blue-600 dark:text-blue-300"
            onClick={() =>
              setConfig({
                ...config,
                rules: [
                  ...config.rules,
                  { signal: "", when: [], label: "無効" },
                ],
              })
            }
          >
            ＋ ルールを追加
          </button>
        </div>
      )}
    </div>
  );
}
