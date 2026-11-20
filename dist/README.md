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
| ビルド元 | `ee77d47`（仕向ごとの入力パネルと監視一覧。仕向ファイルの各仕向に `inputs`・`watch` を持たせ、仕向の切り替えで入れ替え、変更を仕向ファイルへ自動保存する GUI の変更を、PR #67 までの main に足したもの。sim 本体の C のソースは PR #67 の main と同じ） |
| ビルドした日 | 2026-11-20 |
| ビルド方法 | Windows 11 の PC で `tools\build_dist.bat`（`CC=gcc`。WinLibs の MinGW-w64 gcc 16.2.0、x86_64、posix-seh、UCRT） |
| 依存する DLL | Windows 標準のものだけ（KERNEL32、SHELL32、WS2_32、UCRT）。MinGW の DLL は要らない |
| 確認 | 同じ PC で GUI の `npm test` 393 件と `npm run lint` が成功。模擬サーバ（`npm run mock`）で、仕向の切り替えによる入力・固定・監視の入れ替えと自動保存を確かめた。`tests\run_tests.bat` は 922 件中 799 件が成功し、123 件（clang で DLL を作る xlate と native の項目）が失敗したが、PR #67 の main でも同じ 123 件が同じく失敗するこの PC の環境の問題で、今回の変更によるものではない |

普段の配布物は CI（GitHub Actions の `windows` ジョブの成果物 `sim-windows-x64`）が作ります。
このフォルダは、CI を使えなかったときの代わりに置いたものです。ソースを変えても自動では作り直されないので、
上の「ビルド元」より新しい変更は入っていません。中のファイルは直接編集せず、作り直すときは
Windows の上で `tools\build_dist.bat` を実行します（フォルダごと置き換わる）。macOS / Linux の上で作るときは
`tools/build_windows_cross.sh`（作った `sim.exe` をその機械では実行できない）。
