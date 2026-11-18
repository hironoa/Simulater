# AGENTS.md

このリポジトリで作業するエージェント（Claude Code、Codex など）向けの決まりです。利用者とのやり取りで決まったことをまとめています。
コードの構成や設計は `README.md` と `docs/DESIGN.md`、変更の経緯は git の履歴を見てください。

## 1. 前提（このプロジェクトの事情）

- RL78 の命令シミュレーターです。WinAMS の環境（テストドライバ、スタブ、テストの CSV）をそのまま使い、同じ形式の結果の CSV を出します。いちばん大事なのは速さです。
- **製品の制御ソフトのソースは、この PC にもエージェントにも渡せません。** 利用者に頼めるのは、処理名・時間・件数など、中身の出ない情報だけです。製品での実行は利用者が別の PC で行い、結果をイシューに貼ります。
- 製品でしか起きない差は、手元の試験用のプロジェクトで再現してから直します。再現できないときは、まず理由を表示する診断を足して、利用者に試してもらいます。

## 2. 応答と文章

- 利用者への応答、コミットメッセージ、PR・イシューの本文は日本語で書きます。英語の通知やツールの出力に続けて答えるときも同じです。コード、識別子、コマンド、エラーメッセージは原文のまま引用してかまいません。
- コミットメッセージには、Conventional Commits 風の英語のプレフィックス（`feat:` / `fix:` / `docs:` / `chore:` / `test:` / `refactor:` など）を付けます。1 つのコミットは 1 つの目的にします。
- 末尾の署名:
  - コミット: `Co-Authored-By: <モデル名> <noreply@anthropic.com>`
  - PR の本文: `🤖 Generated with [Claude Code](https://claude.com/claude-code)`
  - イシューへのコメント: `_🤖 Posted by [Claude Code](https://claude.com/claude-code)_`
  - レビューのスレッドへの返信: `_🤖 Addressed by [Claude Code](https://claude.com/claude-code)_`
- バックスラッシュを含む本文（コミットメッセージ、PR やイシューの本文）は、ファイルに書いてから渡します（`git commit -F`、`gh … --body-file`）。printf や heredoc では `\b` などが消えることがあります。

## 3. 着手前の確認と、作業の振り分け

- 計画が承認されても、実装に着手してよいという承認ではありません。担当や進め方の指定がなければ、着手の前に次の 5 つを添えて利用者に確認します。
  - 変更量（触るファイル数と、おおよその追加・削除行数）
  - 難易度（機械的な置き換えか、設計判断や調査を伴うか）
  - 影響範囲（波及する箇所、壊れうる機能）
  - 可逆性とリスク（戻しやすいか、破壊的な操作を含むか）
  - 推奨（だれが実装するのがよいか、理由を一言）
- 振り分け:
  - 最上位のモデル（Claude Fable）: 本当に複雑な作業だけ（設計判断、原因の分からない不具合の切り分けなど）。
  - Codex（`codex:codex-rescue`）: 仕様のはっきりした実装と修正。小さな修正でも、まず Codex に出します。
  - Opus: 中くらいの判断が要る作業とレビュー。
  - Sonnet: 調査、試験の実行、コミット、dist の作り直しなどの定型の作業。
- サブエージェントの「できました」は鵜呑みにしません。差分と試験の結果（件数を含む）を、自分で確かめます。

## 4. ブランチ・PR・マージ

- main で直接編集しません。作業ブランチ（できれば git worktree）を切ってから編集します。現在地は `git status` だけでなく、`git worktree list` と `git branch -vv` でも確かめます。
- 変更が一区切りついて検証も済んだら、言われなくてもコミット、プッシュ、PR 作成まで行います。PR はドラフトにしません。そのブランチに開いている PR があれば、新しく作らずに更新し、本文がずれたら本文も直します。
- 機能を追加・変更・削除したら、README などの利用者向けの文書も、同じ PR で更新します。
- レビュー（Codex を含む）の指摘は、検討して妥当なら直し、コミットしてプッシュします。これは確認なしで進め、指摘がなくなるまで繰り返します。
  - 直した指摘には、修正のコミットを添えて返信し、スレッドを解決済みにします（`gh api graphql` の `resolveReviewThread`）。
  - 直さない指摘には、理由を返信して解決済みにし、利用者にも報告します。
- **Codex のレビューは、指摘なしで終わると通知が来ません**（👍 が付くだけ）。PR を出したり修正をプッシュしたりしたら、PR のコメントの `codex-review-state:v1` の `commit_sha` が最新のコミットと一致し、`status` が `completed` になるまで、自分で待ちます（60 秒おきに見る裏の処理など）。
- **CI（GitHub Actions、`.github/workflows/ci.yml`）は動きます。** PR とプッシュのたびに、2 つのジョブが走ります。
  - `windows`: MinGW-Builds の gcc で `build.bat`（`CC=gcc`）を実行し、`tests\run_tests.bat` と `rl78\tests\run_tests.bat` を走らせ、clang でもビルドします。成果物 `sim-windows-x64` に、ビルドした `sim.exe` と同梱のファイルを置きます。
  - `macos`: `tests/run_tests.sh` を走らせます。
  - 失敗の通知が来たら、ジョブのログで失敗した確認の名前と件数（`checks run: N` / `実行 N 件`）を見て、その PR の変更が原因なら直します。main でも同じく落ちるものは、別の PR で直し、PR の本文にそう書きます。
- **マージは PR ごとに利用者に確認してから**行います。確認するときは、レビューの状態（`has_unresolved_findings`、未解決のスレッド）、CI の結果、試験の件数を添えます。Codex のレビューが利用上限などで動かなかったときは、そのことも書きます。
- マージは merge commit で行います（リポジトリの履歴に合わせる）。マージ API が 502 を返すことがあります。そのときは `gh api -X PUT repos/hironoa/Simulater/pulls/<番号>/merge -f merge_method=merge -f sha=<完全な sha>` を 60 秒おきに再試行し、`git merge-base --is-ancestor <sha> origin/main` で main に入ったことを確かめます。
- マージしたら、確認を取らずに次を行います。
  1. PR のブランチを消し、`git ls-remote --heads origin <ブランチ>` で消えたことを確かめる。消すのはマージした PR のブランチだけ。
  2. メインのチェックアウトで `git status -sb`（既定のブランチにいて、未コミットの変更がないこと）、`git fetch origin main`、`git merge --ff-only origin/main` を行い、behind が消えたことを確かめる。
  3. 早送りできないときは、リセットや強制更新をせずに、状況を報告して指示を待つ。

## 5. dist（ビルド済みの sim）

- ソースを変える PR では、`dist/sim-windows-x64/`（`sim.exe` と同梱のファイル）を作り直します。
  - 作り方は 2 つあります。Mac のクロスビルドは置きません。
    - CI の `windows` ジョブの成果物 `sim-windows-x64` から作る（`gh run download <run> -n sim-windows-x64`）。`sim.exe` は成果物の一番上のものを使います（成果物には、リポジトリにある古い `dist/sim-windows-x64/` が入れ子で入っているので、そちらは使わない）。テキストのファイル（`sim_gui.html`、`README.md`、`sim.cfg.example`、`native_ram_clear.c`、`xlate/`、`native/`）は、成果物と改行コード以外が同じであることを確かめて、リポジトリのソースの写し（LF）を置きます。
    - この Windows PC で `tools\build_dist.bat` を実行して作る。
  - `dist/README.md` の「ビルド元」を、もとにしたコミットと変更の内容に合わせて更新します。
  - 本体の変更とは別のコミットにします（`chore: Windows 用のビルド済み sim を作り直す（…）`）。本体をコミットしてから作り直します。

## 6. 検証

- 「直った」「通った」と言う前に、実際にコマンドを走らせて出力を確かめます。
- 全体の試験は `tests\run_tests.bat`（CC=gcc を付ける。下の 8）と、Mac / Linux の `tests/run_tests.sh` です。最後の `checks run: N, passed: N, failed: 0`（`.sh` は `実行 N 件、成功 N 件、失敗 0 件`）で件数を確かめます。0 件の成功は、失敗として扱います。手元で走らせられない側は、CI の同じジョブのログで件数を確かめます。
- 不具合を直したときは、直す前の sim で試験が失敗する（再現する）ことも、できるだけ確かめます。
- 確かめていない部分（とくに製品のソースでの結果）は、成功と言い切らずに、そう明記します。

## 7. イシュー

- イシューのコメントは `gh api "repos/hironoa/Simulater/issues/<番号>/comments?per_page=100" --paginate` で読みます。既定では 30 件までしか返らず、31 件目からを取りこぼします。
- 対応を main に入れて、証拠（試験、数値）があるイシューは、自分でクローズします。コメントに、対応した PR と確かめられた範囲を書きます。
- 利用者の PC での結果（製品のソースでの実行）を待っているものは、コメントを付けて開いたままにします。

## 8. 環境（この Windows PC の場合）

- ツールの場所:
  - gcc: WinLibs の MinGW-w64（UCRT、16.2.0）。winget で入れたもの。
  - clang / LLVM 18.1.8: インストールせずに展開したもの（本番の環境と同じ形）。`%USERPROFILE%\Documents\Projects\clang+llvm-18.1.8-x86_64-pc-windows-msvc`
  - Python 3.13: `%LOCALAPPDATA%\Programs\Python\Python313\python.exe`。PATH にある `python` は Microsoft Store の入口なので使いません。
  - gh: `C:\Program Files\GitHub CLI`
  - MSVC と Windows SDK は入れていません。
- **試験とビルド（`tests\run_tests.bat`、`tools\build_dist.bat`）では `CC=gcc` を付けます。** 付けないと `build.bat` が clang と MSVC を探し、`stdio.h` が見つからずに全滅します。
- Codex の CLI（npm で入れたもの）を使うときは、シェルの PATH に `C:\Program Files\nodejs` と `%APPDATA%\npm` を足します。足さないと「Codex CLI is not installed」になります。長い依頼文は、コマンドに直接書かずにファイルで渡します。
- `.bat` は ASCII と CRLF のままにします。LF にすると cmd が誤動作します（ラベルへの `call` が再帰して落ちる）。`sed -i` で書き換えず、書き換えたら `file` で CRLF を確かめます。
- 日本語の Windows では、`findstr` と PowerShell の `Get-Content` が UTF-8 を読み違えます。試験の中で文字列を探すときは `has_text.exe`（`tests/tools` からビルドするもの）を使い、PowerShell では `-Encoding UTF8` を付けます。
- ウイルス対策（ESET）が、作ったばかりの小さな exe を消すことがあります。exe が見つからないときは、これを疑います。
