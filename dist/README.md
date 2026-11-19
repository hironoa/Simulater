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
| ビルド元 | `33ecdcc`（信号情報ファイル。sim gui の `/api/signals`（`sim.cfg` の `gui_signals`）と、GUI の形式 v2・仕向ごとの差分・起動時の自動の読み込みと保存、およびそのレビューの修正を、PR #63 までの main に足したもの。PR #64） |
| ビルドした日 | 2026-11-19 |
| ビルド方法 | GitHub Actions の `windows` ジョブ（run 44516756670、windows-2025、MinGW-Builds の gcc 15.2.0、x86_64、posix-seh、UCRT。`build.bat` の `CC=gcc`）の成果物 `sim-windows-x64` から `sim.exe` を取った。テキストのファイル（`sim_gui.html`、`README.md`、`sim.cfg.example`、`native_ram_clear.c`、`xlate/`、`native/`）は、成果物と改行コード以外が同じであることを確かめて、リポジトリのソースの写し（LF）を置いた |
| 依存する DLL | Windows 標準のものだけ（KERNEL32、SHELL32、WS2_32、UCRT）。MinGW の DLL は要らない |
| 確認 | CI の同じ run で、windows ジョブの `tests\run_tests.bat` 927 件と、コア単体の試験（`rl78\tests\run_tests.bat`）、clang でのビルドが成功し、macos ジョブの `tests/run_tests.sh` 1186 件がすべて成功。Mac の上では GUI の `npx vitest run` 380 件と `npm run lint` が成功。`sim.exe` そのものは Windows で実行していない |

普段の配布物は CI（GitHub Actions の `windows` ジョブの成果物 `sim-windows-x64`）が作ります。
このフォルダは、CI を使えなかったときの代わりに置いたものです。ソースを変えても自動では作り直されないので、
上の「ビルド元」より新しい変更は入っていません。中のファイルは直接編集せず、作り直すときは
Windows の上で `tools\build_dist.bat` を実行します（フォルダごと置き換わる）。macOS / Linux の上で作るときは
`tools/build_windows_cross.sh`（作った `sim.exe` をその機械では実行できない）。
