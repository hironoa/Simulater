# Simulater

RL78 向け組み込みソフトを、CS+ / CC-RL でビルドしたロードモジュール（`.abs`）のまま PC 上で実行するシミュレータ。
仕様は `docs/DESIGN.md`。

## ビルド

- macOS / Linux: `./build.sh`（`build/sim/sim` ができる）
- Windows: `build.bat`（LLVM の clang が要る。`build\sim\sim.exe` ができる）
- `mock` を付けると共通部分のテスト用のモック版（`build/mock`）

## 使い方（最初に試す手順）

```
sim info --abs DefaultBuild\project.abs        # 読み込めるか、main まで走るか、警告の件数
sim list --abs DefaultBuild\project.abs g_     # 変数と関数の一覧（名前の一部で絞り込み）
sim run  --abs DefaultBuild\project.abs test.csv -o result
```

設定は `.abs` と同じフォルダの `sim.cfg`（見本は `example/sim.cfg`）。GUI は `sim gui --abs ...`。

## テスト

`tests/run_tests.sh`（Windows は `tests\run_tests.bat`）。モックで共通部分を、本物のバックエンドでルネサス公式サンプルの
`.abs` と `tests/tools/mkabs.c` が作るテスト用 `.abs` を確かめる。
