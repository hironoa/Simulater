# Project Specific Agent Instructions

このディレクトリは SignalViewer2 を取り込んだもの（取り込み元は `UPSTREAM.md`）。取り込み元の運用ルールを引き継ぐ。

1.  **Strict Adherence to Requests**: When modifying code, ensure you ONLY make changes related to the requested task. Do not make unrelated changes.

3.  **Do Not Map Break Values**: NEVER use manual data mapping (`applyBreakValue` / `revertBreakValue`) for Axis breaks, nor map physical data just to achieve UI visualization constraints. ECharts handles `breaks: [{ start, end }]` inherently without data mapping. You must respect raw data and avoid transforming the `series` values.

2.  **Deployment File Update**: When you modify code in this project, you MUST always rebuild the application with `npm run build` so that `dist/sim_gui.html` is kept up-to-date. `dist/sim_gui.html` is committed (the Windows PCs that run `sim` have no Node).

# Build Process

- `npm run build` is configured to build the SPA.
- The `postbuild` script (`scripts/postbuild.mjs`) renames `dist/index.html` to `dist/sim_gui.html`.
- Therefore, simply running `npm run build` after changes is sufficient to update the deployment file.
- `npm run lint`（tsc --noEmit）と `npm test`（vitest）を通す。

# ライブモード（sim gui）の開発

- `npm run mock` で `dev/mock-server.mjs`（`docs/DESIGN.md`「5. GUI モード」の API と SSE の模擬）を 127.0.0.1:8765 に立てる。
  ビルド済みの `dist/sim_gui.html` を `/` で配るので、`http://127.0.0.1:8765/` を開けばライブモードで確かめられる。
- `npm run dev` の開発サーバは `/api` をモックサーバ（`SIM_GUI_API` で変更可）に中継する。
- HTTP API は C 側の `sim gui` と共有の仕様。GUI の都合で勝手に変えない。

# Review Language

- GitHub のコードレビュー（Codex などの自動レビューを含む）では、レビュー本文、指摘のタイトルと本文、レビュー結果の要約を日本語で書く。
- コード、識別子、ファイル名、コマンド、エラーメッセージなどは、必要に応じて原文のまま引用する。
