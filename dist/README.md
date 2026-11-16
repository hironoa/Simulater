# Windows 用のビルド済み sim

`sim-windows-x64/` フォルダは、Windows 用にビルドした `sim.exe` と、一緒に置くファイル一式です。
フォルダごと取り出して使います（リポジトリを clone するか、GitHub の「Code」→「Download ZIP」）。
`sim.exe` と `xlate` フォルダは同じ場所に置いたままにします（C への変換方式が `xlate` の中のファイルを使う）。
使い方はルートの `README.md` です。

| ファイル | 内容 |
| --- | --- |
| `sim.exe` | シミュレータ本体 |
| `xlate/` | C への変換方式（`exec = xlate`）が使うソース。`sim.exe` と同じビルド元のもの |
| `sim_gui.html` | GUI（`sim.exe` と同じ場所に置く） |
| `sim.cfg.example` | 設定ファイルの例 |
| `README.md` | ビルドしたときのルートの `README.md` の写し |

| 項目 | 内容 |
| --- | --- |
| ビルド元 | `ebc1d98`（C への変換方式、実行したフォルダの `sim.cfg` を読む変更、`defines_scope = mtpj` を含む） |
| ビルドした日 | 2026-11-16 |
| ビルド方法 | macOS の上で `tools/build_windows_cross.sh`（MinGW-w64 の gcc 16.2.0、`build.bat` の `CC=gcc` と同じ設定） |
| 依存する DLL | Windows 標準のものだけ（KERNEL32、SHELL32、WS2_32、UCRT）。MinGW の DLL は要らない |
| 確認 | **Windows では実行していない**（ビルドした機械では実行できないため）。ビルド元のソースは macOS でテスト 1,140 件が成功 |

普段の配布物は CI（GitHub Actions の `windows` ジョブの成果物 `sim-windows-x64`）が作ります。
このフォルダは、CI を使えなかったときの代わりに置いたものです。ソースを変えても自動では作り直されないので、
上の「ビルド元」より新しい変更は入っていません。中のファイルは直接編集せず、作り直すときは
`tools/build_windows_cross.sh` を実行します（フォルダごと置き換わる）。
