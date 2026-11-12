# Simulater

RL78 向け組み込みソフトを、CS+ / CC-RL でビルドしたロードモジュール（`.abs`）のまま PC 上で実行するシミュレータ。

- WinAMS 用のテスト CSV を読み込み、同じ書式の結果 CSV を出力する（バッチ実行）
- ブラウザの画面で入力信号を変えながら、結果を波形でリアルタイムに見る（GUI 実行）
- 機械語をそのまま解釈実行するので、`int` 16 ビット、ポインタ幅、SFR のアドレス、アセンブラで書かれた処理は実機と同じに動く

仕様の詳細は [docs/DESIGN.md](docs/DESIGN.md)。

## 用意するもの

- **WinAMS に読み込ませている `.abs`**（テストドライバとスタブを含めて CS+ でビルドしたもの。デバッグ情報つき）
- WinAMS 用のテスト CSV
- `sim.exe`。次のどちらかで入手する
  - GitHub Actions の成果物 `sim-windows-x64`（`sim.exe`、`sim_gui.html`、`sim.cfg` の見本）
  - 自分でビルドする: `build.bat`（LLVM の clang が要る。`build\sim\sim.exe` ができる）

macOS / Linux では `./build.sh`（`build/sim/sim` ができる）。CMake、Python、Node は使わない。

## 最初に試す手順

```
sim info --abs DefaultBuild\project.abs -v     # 読み込めるか、main まで走るか、警告
sim list --abs DefaultBuild\project.abs g_     # 変数と関数の一覧（名前の一部で絞り込み）
sim run  --abs DefaultBuild\project.abs test.csv -o result
```

`sim run` は結果 CSV を `result` フォルダに入力と同じ名前で作り、最後に件数（OK / NG / NO Check）と
実行した命令数・所要時間・命令/秒を表示する。NG が 1 つでもあれば終了コード 1、実行できないエラーは 2。
結果の出力先が入力の CSV そのもの（`./` や相対パスなど書き方が違っても同じファイル）になるときは、上書きせずにエラー 2。

スタートアップが周辺機能の待ち（発振安定待ちなど）で止まる場合は、エラーに PC と逆アセンブル結果が出る。
`sim.cfg` の `stub =` でその関数を置き換えるか、`init_end =` で止める場所を変える。

## テスト CSV と結果 CSV

WinAMS の書式をそのまま使う。

```
mod,func1,func1単体テスト,3,2
#COMMENT,@enable,@mode,@input,gb_result.data,gb_result.ret_code
;,自由なコメント
,1,1,10,100,1
```

- 1 行が対象関数 1 回の呼び出し。変数は行をまたいで保持される
- 変数名: グローバル、`file.c/name`（ファイル内 static）、`file.c/func@name`（関数内 static）、`@引数`、`func@@`（戻り値）、
  `a.b` `a[2]` `p[0].m`、`$ptr`（実体の確保）、`&arr` / `*ptr`（`'文字列'` や `1|2|3`）、`0xFFF01:2`（アドレス直接指定）
- 結果 CSV: 出力セルに実測値、期待値と違えば `実測?(期待)`。右に判定（`OK` / `NG` / `NO Check`）と実行時間
- 入力の空セルは「設定しない」、出力の空セルは期待値なし。空文字列 `''` は書く（終端の 0）／比べる（空文字列か）
- 期待値は入力と同じ規則で解釈し、対象のビット幅に収まらない期待値は `NG` にする
- 文字コード（Shift_JIS / UTF-8）と改行コードは入力のまま保つ
- 結果 CSV を入力として読み直せる。**WinAMS の結果 CSV を入力にすると、WinAMS の実測値を期待値として
  このシミュレータの結果と突き合わせられる**（`--expect-actual` を付ける）

## GUI

```
sim gui --abs DefaultBuild\project.abs --func <周期関数> --period-ms 5
```

既定のブラウザ（Chrome / Edge）で画面が開く。1 周期ごとに指定した関数を 1 回呼ぶ。

- 実行 / 一時停止 / ステップ実行 / リセット、速度の倍率
- 入力パネル: 変数を検索して追加し、数値・スライダ・トグル・押している間だけ 1 のボタンで値を変える。固定（毎周期書き続ける）もできる
- 変数モニタ: 現在値を 10 進 / 16 進で表示。構造体や配列は展開して選べる
- 波形: 実行中は直近の点を流し、一時停止すると全区間をズームやカーソルで調べられる
- 記録を WinAMS 形式の CSV で保存、テスト CSV の読み込みと再生。保存した CSV は `sim run` でそのまま再実行できる
  （入力は書いた値の表記のまま残す）。1 周期の入力は保存する CSV の入力列の順に書くので、同じメモリを指す固定と
  単発書き込みを併用しても再実行と合う。周期関数を切り替えるとリセットして記録を始め直す。
  切り替えた先の関数にない引数（`@rpm` など）や前の関数の戻り値の固定・監視は外す

画面は [SignalViewer2](https://github.com/hironoa/SignalViewer2) を取り込んで作っている。
`sim_gui.html` を直接開けば、従来どおり CSV を読む Viewer として使える。

## 設定ファイル sim.cfg

`.abs` と同じフォルダに置くと自動で読む。見本は [example/sim.cfg](example/sim.cfg)。主な項目:

| キー | 内容 |
|---|---|
| `stub = 元の関数:スタブ関数` | 元の関数が呼ばれたらスタブへ飛ばす（WinAMS の「置換」）。複数書ける |
| `stub_auto = on` | `AMSTB_<関数名>` がある関数をすべて置換する |
| `core = auto \| S1 \| S2 \| S3` | CPU コア種別。品種が分からなければ `auto`（既定） |
| `cpu_hz` | CPU クロック。結果 CSV の実行時間の換算に使う |
| `init_end` | 初期設定終了アドレス（既定 `main`） |
| `func` | GUI の既定の周期関数 |
| `halt` / `stop` | HALT / STOP 命令を読み飛ばすか、エラーにするか |
| `max_insns` | 1 回の呼び出しの命令数の上限（暴走の検出） |

## 制限

- 周辺機能（タイマ、シリアル、A/D など）は模擬しない。SFR は読み書きできるメモリとして扱う。
  割り込みは、テストドライバがハンドラ関数を直接呼ぶ前提
- 対象関数の引数は整数 / ポインタ 8 個まで。浮動小数や構造体の値渡しの引数は使えない
- 結果 CSV の実行時間は、実行したクロック数からの換算値。WinAMS の値と一致するとは限らない
- CPU コアは、ルネサスのソフトウェアマニュアルと GNU の RL78 シミュレータとの差分テストで検証している。
  実機や WinAMS との突き合わせはしていないので、最初は WinAMS の結果 CSV と比べて確かめること。
  マニュアルの解釈が分かれる点は [rl78/README.md](rl78/README.md) にまとめてある

## 構成

```
rl78/      RL78 CPU コア（単体でテストできる）
runtime/   CSV の実行、変数名の解決、GUI サーバ、.abs（ELF / DWARF）の読み込み
gui/       GUI のソース（ビルド済みの gui/dist/sim_gui.html を同梱）
tests/     自動テスト
docs/      設計書
```

## テスト

`tests/run_tests.sh`（Windows は `tests\run_tests.bat`）と `rl78/tests/run_tests.sh`。
ルネサス公式サンプル（BSD-3-Clause）の CC-RL 製 `.abs` と、`tests/tools/mkabs.c` が作るテスト用 `.abs` で確かめる。
GUI は `gui` フォルダで `npm test`。
