# 取り込み元

このディレクトリは利用者の波形ビューア SignalViewer2 を取り込み、`sim gui` のライブ機能を足したもの。

| 項目 | 値 |
|---|---|
| リポジトリ | https://github.com/hironoa/SignalViewer2 |
| コミット | `f3dc245f9412f294897f687fa7aab2964eadf0cd`（2026-11-10、Merge pull request #22 from hironoa/claude/save-start-folder） |

持ち込まなかったもの: `.git`、`node_modules`、ビルド済みの `winams_csv_viewer.html`、`dist`、`.issue-watch/`（取り込み元リポジトリの Issue 自動対応の設定）。

## 取り込み後に変えたところ

- `src/live/`、`src/components/live/`（新規）: ライブモード一式。
- `src/App.tsx`: 起動時のサーバ検出、接続バー・ライブ描画・入力/監視パネルの配置。
- `src/store/useWaveformStore.ts`: `appendSamples`、`commitLiveRecord`、`ensureLiveSignals` を追加。
- `src/components/Chart.tsx`: ホバー時刻を、添字ではなく `steps` から求めるようにした（CSV では `steps` が 1..n なので結果は同じ）。
- `src/utils/helpers.ts`: `detectUndefinedValue` の `Math.max(...data)` をループにした（十万点を超えるとスタックが溢れていた）。
- ビルド: `postbuild` で `dist/sim_gui.html` を作る（`winams_csv_viewer.html` は作らない）。`dist/sim_gui.html` はコミットする。
- `vitest` を追加（`npm test`）。`dev/mock-server.mjs` を追加（`npm run mock`）。

取り込み元の更新を取り込むときは、上の変更点と衝突しないかを確かめてから差分を当てる。
