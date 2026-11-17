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
| ビルド元 | `5b97bb3`（PC 用 DLL の方式 `exec = native` で、変数と関数をデバッグ情報から取る変更（段階 2）と、`long int` と `__asm` のあるソースをコンパイルできるようにする修正（プリプロセス後に型と `__asm` を書き直す。GNU の修飾子の別名、`__asm` のあとの行の情報、型の語の途中にある属性なども扱う）と、どの `.c` にも定義がない関数と変数（アセンブラの中のものなど）の代わりを自動で作る修正（float / double を返す関数の代わりは 0.0 を返す）を含む） |
| ビルドした日 | 2026-11-17 |
| ビルド方法 | Windows 11 の上で `tools\build_dist.bat`（MinGW-w64 の gcc 16.2.0、x86_64、UCRT。`build.bat` の `CC=gcc` と同じ設定） |
| 依存する DLL | Windows 標準のものだけ（KERNEL32、SHELL32、WS2_32、UCRT）。MinGW の DLL は要らない |
| 確認 | ビルドした Windows の PC で実行した。ビルド元のソースは `tests\run_tests.bat` 786 件、`rl78\tests\run_tests.bat` 4,903,782 件（`31554b7` で実行。戻り値の修正のあとは実行していない）が成功。このフォルダの `sim.exe` そのものでは、`exec = native` の試験用の CSV（`native_basic.csv` 17 行、変数を名前で読み書きする `native_vars.csv` 11 行、`long int`・`unsigned` だけの宣言・`__asm` のあるソースを試す `native_legacy.csv` 3 行、アセンブラの中にある想定の関数と変数を使う `native_asmcall.csv` 5 行）が全行 OK になることを確かめた |

普段の配布物は CI（GitHub Actions の `windows` ジョブの成果物 `sim-windows-x64`）が作ります。
このフォルダは、CI を使えなかったときの代わりに置いたものです。ソースを変えても自動では作り直されないので、
上の「ビルド元」より新しい変更は入っていません。中のファイルは直接編集せず、作り直すときは
Windows の上で `tools\build_dist.bat` を実行します（フォルダごと置き換わる）。macOS / Linux の上で作るときは
`tools/build_windows_cross.sh`（作った `sim.exe` をその機械では実行できない）。
