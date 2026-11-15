# Windows 用のビルド済み sim

`sim-windows-x64.zip` は、Windows 用にビルドした `sim.exe` と、一緒に置くファイル一式です。
展開して、`sim.exe` と `xlate` フォルダを同じ場所に置いて使います（使い方はルートの `README.md`）。

| 項目 | 内容 |
| --- | --- |
| ビルド元 | main の `933bdef`（C への変換方式を含む） |
| ビルドした日 | 2026-11-15 |
| ビルド方法 | macOS の上で `tools/build_windows_cross.sh`（MinGW-w64 の gcc 16.2.0、`build.bat` の `CC=gcc` と同じ設定） |
| 依存する DLL | Windows 標準のものだけ（KERNEL32、SHELL32、WS2_32、UCRT）。MinGW の DLL は要らない |
| 確認 | **Windows では実行していない**（ビルドした機械では実行できないため）。ビルド元のソースは macOS でテスト 1,050 件が成功 |

普段の配布物は CI（GitHub Actions の `windows` ジョブの成果物 `sim-windows-x64`）が作ります。
この zip は、CI を使えなかったときの代わりに置いたものです。ソースを変えても自動では作り直されないので、
上の「ビルド元」より新しい変更は入っていません。作り直すときは `tools/build_windows_cross.sh` を実行します。
