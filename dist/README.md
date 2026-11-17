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
| `native_ram_clear.c` | PC 用 DLL の方式で、マイコンの番地の範囲の RAM をクリアする関数の代わりのひな形（`native_src` に置いて書き換える） |
| `README.md` | ビルドしたときのルートの `README.md` の写し |

| 項目 | 内容 |
| --- | --- |
| ビルド元 | `b3a8a0e`（PC 用 DLL の方式 `exec = native` で、マイコンの番地の範囲にある変数をクリアできるようにする変更（sim.cfg の `native_layout` で製品のロードモジュールから変数の番地を読み、製品のソースの変数と名前で対応づける。DLL に入れる部品 `sim_native_clear_range` / `sim_native_clear_range_incl` から、範囲の中の製品の変数と番地を直接書いたデータを 0 にする。`native_src` の変数はクリアしない。ひな形 `native_ram_clear.c` を入れた）と、整数からポインタへの変換を near の番地として読み替える変更（値が 1〜0xFFFF なら 0xF0000 を足す。0 と 0x10000 以上はそのまま。コンパイルを LLVM IR 経由の 4 段にし、IR の inttoptr を書き換える。`native_near = off` でやめられる）と、関数の置換（sim.cfg の `stub`・`stub_auto`・`skip` と CSV の `%` 行。今の方式と同じ書き方。元の関数の入口を、スタブへの分岐か戻り値を返す命令に書き換える）を使えるようにする変更（段階 3）と、例外や時間切れで止まった行に、止まったソースのファイル名と行番号を表示する変更（LLVM の `llvm-symbolizer` を起動する。1 回の実行で 20 件まで。一時ファイルは、同時に動くほかの sim と重ならない名前にする）と、番地を書いた `#define` をコンパイルするソース（mtpj のビルド対象と `native_src`）とそのヘッダーから自動で読む変更（`defines_auto = off` または `defines_scope` の明示で止まる）と、変数と関数をデバッグ情報から取る変更（段階 2）と、関数の中で例外（確保していない番地の読み書き、0 での割り算など）が起きた行だけをエラーにして次の行へ進み、戻らない関数を `native_timeout_ms`（既定 10000 ミリ秒）で止める変更（段階 2 の後半。スタックの使い切りは終了コード 2 で終了）と、`long int` と `__asm` のあるソースをコンパイルできるようにする修正（プリプロセス後に型と `__asm` を書き直す。GNU の修飾子の別名、`__asm` のあとの行の情報、型の語の途中にある属性なども扱う）と、どの `.c` にも定義がない関数と変数（アセンブラの中のものなど）の代わりを自動で作る修正（float / double を返す関数の代わりは 0.0 を返す。変数の代わりは宣言の整列、最小 16 バイトで置く）を含む） |
| ビルドした日 | 2026-11-18 |
| ビルド方法 | Windows 11 の上で `tools\build_dist.bat`（MinGW-w64 の gcc 16.2.0、x86_64、UCRT。`build.bat` の `CC=gcc` と同じ設定） |
| 依存する DLL | Windows 標準のものだけ（KERNEL32、SHELL32、WS2_32、UCRT）。MinGW の DLL は要らない |
| 確認 | ビルドした Windows の PC で実行した。ビルド元のソースは `tests\run_tests.bat` 874 件（`b3a8a0e` の内容で実行。前回の 854 件に、番地の対応づけの単体テスト 3 件、probe の番地の突き合わせ 1 件、RAM のクリアの試験 16 件を足した）と、コア単体の `rl78\tests\run_tests.bat` 4,903,782 件（同じく `b3a8a0e` の内容で実行）が成功。このフォルダの `sim.exe` そのものでは、`exec = native`（`native_timeout_ms = 300`）の試験用の CSV（`native_basic.csv` 18 行、`native_vars.csv` 11 行、`native_legacy.csv` 3 行、`native_asmcall.csv` 6 行、`native_near.csv` 11 行、`native_low.csv` 2 行）が全行 OK、`native_fault.csv` が総数 8・OK 6・NG 2（0 での割り算と時間切れの行）になることを確かめた。RAM の範囲のクリアでは、CA78K0R でビルドした `tests/ca78k0r_probe` の `.lmf` を `native_layout` に指定した `tests/native/probe_clear` で、`native_ram_clear.csv` が総数 11・OK 11（範囲の中の製品の変数と番地を直接書いたデータが 0 になり、範囲の外と `native_src` の変数が残る。終わりを含む版と含まない版、配列の途中から始まる範囲、16 ビットの番地を含む）、`native_opt = 2` の `native_ram_clear_opt2.csv` が総数 10・OK 10、`native_layout` を外すと OK 4・NG 7（C の変数がクリアされない）になり、`sim info` が `native layout vars=97 matched=56 unmatched=41 size_diff=20 dll_only=0 skipped_src=0` を出すこと、ひな形 `native_ram_clear.c` をそのまま `native_src` に置いてコンパイルでき、範囲の中の変数をクリアすることを確かめた |

普段の配布物は CI（GitHub Actions の `windows` ジョブの成果物 `sim-windows-x64`）が作ります。
このフォルダは、CI を使えなかったときの代わりに置いたものです。ソースを変えても自動では作り直されないので、
上の「ビルド元」より新しい変更は入っていません。中のファイルは直接編集せず、作り直すときは
Windows の上で `tools\build_dist.bat` を実行します（フォルダごと置き換わる）。macOS / Linux の上で作るときは
`tools/build_windows_cross.sh`（作った `sim.exe` をその機械では実行できない）。
