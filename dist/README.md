# Windows 用のビルド済み sim

`sim-windows-x64/` フォルダは、Windows 用にビルドした `sim.exe` と、一緒に置くファイル一式です。
フォルダごと取り出して使います（リポジトリを clone するか、GitHub の「Code」→「Download ZIP」）。
`sim.exe` と `xlate` フォルダ、`native` フォルダは同じ場所に置いたままにします（C への変換方式が `xlate` の中の
ファイルを、PC 用 DLL の方式が `native` の中のファイルを使う）。
使い方はルートの `README.md` です。

| ファイル | 内容 |
| --- | --- |
| `sim.exe` | シミュレータ本体 |
| `xlate/` | C への変換方式（`exec = xlate`）が使うソース。`sim.exe` と同じビルド元のもの |
| `native/` | PC 用 DLL の方式（`exec = native`）が使うファイル。`sim.exe` と同じビルド元のもの |
| `sim_gui.html` | GUI（`sim.exe` と同じ場所に置く） |
| `sim.cfg.example` | 設定ファイルの例 |
| `README.md` | ビルドしたときのルートの `README.md` の写し |

| 項目 | 内容 |
| --- | --- |
| ビルド元 | `5da58be`（PC 用 DLL の方式 `exec = native` で、関数の置換（sim.cfg の `stub`・`stub_auto`・`skip` と CSV の `%` 行。今の方式と同じ書き方。元の関数の入口を、スタブへの分岐か戻り値を返す命令に書き換える）を使えるようにする変更（段階 3）と、例外や時間切れで止まった行に、止まったソースのファイル名と行番号を表示する変更（LLVM の `llvm-symbolizer` を起動する。1 回の実行で 20 件まで。一時ファイルは、同時に動くほかの sim と重ならない名前にする）と、番地を書いた `#define` をコンパイルするソース（mtpj のビルド対象と `native_src`）とそのヘッダーから自動で読む変更（`defines_auto = off` または `defines_scope` の明示で止まる）と、変数と関数をデバッグ情報から取る変更（段階 2）と、関数の中で例外（確保していない番地の読み書き、0 での割り算など）が起きた行だけをエラーにして次の行へ進み、戻らない関数を `native_timeout_ms`（既定 10000 ミリ秒）で止める変更（段階 2 の後半。スタックの使い切りは終了コード 2 で終了）と、`long int` と `__asm` のあるソースをコンパイルできるようにする修正（プリプロセス後に型と `__asm` を書き直す。GNU の修飾子の別名、`__asm` のあとの行の情報、型の語の途中にある属性なども扱う）と、どの `.c` にも定義がない関数と変数（アセンブラの中のものなど）の代わりを自動で作る修正（float / double を返す関数の代わりは 0.0 を返す。変数の代わりは宣言の整列、最小 16 バイトで置く）を含む） |
| ビルドした日 | 2026-11-17 |
| ビルド方法 | Windows 11 の上で `tools\build_dist.bat`（MinGW-w64 の gcc 16.2.0、x86_64、UCRT。`build.bat` の `CC=gcc` と同じ設定） |
| 依存する DLL | Windows 標準のものだけ（KERNEL32、SHELL32、WS2_32、UCRT）。MinGW の DLL は要らない |
| 確認 | ビルドした Windows の PC で実行した。ビルド元のソースは `tests\run_tests.bat` 838 件（`5da58be` の内容で実行。前回の 809 件に native の置換の試験 29 件を足した）と、コア単体の `rl78\tests\run_tests.bat` 4,903,782 件（同じく `5da58be` の内容で実行）が成功。このフォルダの `sim.exe` そのものでは、`exec = native`（`native_timeout_ms = 300`）の試験用の CSV（`defines =` を書かずにヘッダーの名前を使う `native_basic.csv` 18 行、変数を名前で読み書きする `native_vars.csv` 11 行、`long int`・`unsigned` だけの宣言・`__asm` のあるソースを試す `native_legacy.csv` 3 行、アセンブラの中にある想定の関数と変数を使う `native_asmcall.csv` 6 行）が全行 OK、例外・0 での割り算・時間切れの行を含む `native_fault.csv` が総数 8・OK 5・NG 3（終了コード 1。エラーの行のあとの正常な行は OK。NG の 3 行には止まったソースの `io.c` と行番号が出る）になることを確かめた。`native_opt = 2` でも同じ結果（ただし `native_vars.csv` の static 関数 `vars_static_add` の 1 行は、最適化で関数がなくなり「見つかりません」のエラーになる。前のビルド `3f178c3` でも同じ）。関数の置換では、置換なしの `native_stub_cfg.csv` が総数 6・OK 4・NG 2（周辺機能を待つ関数の時間切れと、範囲外の番地の読み出し）、`stub =` を足すと全 6 行 OK、`stub_auto = on` の `native_stub_auto.csv` と `skip =` の `native_stub_skip.csv` が全 6 行 OK、CSV の `%` 行の `native_stub_csv.csv` が OK 7・エラー 1（見つからない関数を書いたブロックだけ。終了コード 2）、sim.cfg と CSV の両方を使う `native_stub_priority.csv` が全 3 行 OK になることを確かめた（置換の CSV は `native_opt = 0` で確かめた。`native_opt = 2` では同じファイルの中の関数が埋め込まれて置換が効かない） |

普段の配布物は CI（GitHub Actions の `windows` ジョブの成果物 `sim-windows-x64`）が作ります。
このフォルダは、CI を使えなかったときの代わりに置いたものです。ソースを変えても自動では作り直されないので、
上の「ビルド元」より新しい変更は入っていません。中のファイルは直接編集せず、作り直すときは
Windows の上で `tools\build_dist.bat` を実行します（フォルダごと置き換わる）。macOS / Linux の上で作るときは
`tools/build_windows_cross.sh`（作った `sim.exe` をその機械では実行できない）。
