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
| ビルド元 | `9e46de2`（GUI でテスト CSV を読み込むとき、`#InitWheneverCall` などの `#Init` 行にだけ出てくる変数を監視（波形）に加えない修正と、信号設定・信号の一覧表・非アクティブ条件の選択肢・シンボル検索で「ファイル名/信号名」を信号名だけで表示する修正を、`2e2fd1e` までの内容に足したもの） |
| ビルドした日 | 2026-11-18 |
| ビルド方法 | Windows 11 の上で `tools\build_dist.bat`（MinGW-w64 の gcc 16.2.0、x86_64、UCRT。`build.bat` の `CC=gcc` と同じ設定） |
| 依存する DLL | Windows 標準のものだけ（KERNEL32、SHELL32、WS2_32、UCRT）。MinGW の DLL は要らない |
| 確認 | ビルドした Windows の PC で実行した。ビルド元のソースは `tests\run_tests.bat` 893 件（`9e46de2` の内容で実行。前回の 874 件から、GUI の CSV 読み込みの試験 2 件などが増えた）と、GUI の `npm test` 165 件が成功。コア（`rl78`）は変えていないので、コア単体の試験は今回は実行していない。このフォルダの `sim.exe` そのものでは、起動して版を表示できることだけを確かめた |

普段の配布物は CI（GitHub Actions の `windows` ジョブの成果物 `sim-windows-x64`）が作ります。
このフォルダは、CI を使えなかったときの代わりに置いたものです。ソースを変えても自動では作り直されないので、
上の「ビルド元」より新しい変更は入っていません。中のファイルは直接編集せず、作り直すときは
Windows の上で `tools\build_dist.bat` を実行します（フォルダごと置き換わる）。macOS / Linux の上で作るときは
`tools/build_windows_cross.sh`（作った `sim.exe` をその機械では実行できない）。
