# CA78K0R / `.lmf` 調査メモ（実装担当向け）

調査日: 2026-11-14。比較の基準: `ccrl_abi.md`（同じ research ディレクトリ）。

凡例:
- 【確認】= 一次資料（ルネサス/NEC/ガイオの公式文書）に明記
- 【観察】= 公開 GitHub 上の実ファイルの先頭バイト等を自分で読んだ結果（公式資料ではない。ダウンロードはせず、raw URL の Range 取得で標準出力に出しただけ）
- 【推測】= 上から導いた推測
- 【不明】= 確認できず

主な資料（以降は略称で引用）:
- **[BUILD]** CubeSuite+ V1.03.00 ユーザーズマニュアル RL78,78K0R ビルド編 R20UT2143JJ0100 Rev.1.00 2012.09 — https://www.renesas.com/ja/document/mat/cubesuite-v10300-integrated-development-environment-users-manual-rl78-78k0r-build （PDF 6.2MB、全文テキスト化して読んだ）
- **[CODING]** CubeSuite+ V1.03.00 ユーザーズマニュアル RL78,78K0R コーディング編 R20UT2140JJ0100 Rev.1.00 2012.09（931 ページ、C 言語仕様・アセンブラ・スタートアップを含む）— https://www.renesas.com/ja/document/mat/cubesuite-v10300-integrated-development-environment-users-manual-rl78-78k0r-coding
- **[ID78R]** ID78K0R-QB Ver.3.60 統合デバッガ操作編 U19613EJ1V0UM — https://www.renesas.com/us/en/document/mat/id78k0r-qb-ver360-integrated-debugger-operation
- **[ID78]** ID78K0-QB Ver.2.94 統合デバッガ操作編 — https://www.renesas.com/en/document/mah/id78k0-qb-ver294-integrated-debugger-operation?language=en
- **[MIG-C]** RL78 開発環境移行ガイド（コンパイラ編：コーディング）CA78K0R→CC-RL R20UT3416JJ0102 — https://renesas.com/ja/document/gde/rl78-development-environment-migration-guide-migration-between-rl78-family-compiler-ed-coding
- **[MIG-L]** 同（リンカオプション）R20UT3417JJ0101 — https://renesas.com/ja/document/gde/rl78-development-environment-migration-guide-migration-between-rl78-family-compiler-ed-linker-option
- **[GAIO-TUT]** カバレッジマスター winAMS チュートリアル V6.0.0（CS+ 78K0 編 p.250〜、CS+ RL78 編 p.266〜）— https://www.gaio.co.jp/support/user/pdf/CoverageMaster_Tutorial_V600.pdf
- **[GAIO-MPU]** ガイオ 開発ツール サポート MPU 一覧 — https://www.support.gaio.co.jp/mpu/
- CS+ オンラインヘルプに `Compiler-CA78K0R.chm` 相当は見つからなかった（`.../csp/latest/CS+.chm/Compiler-CA78K0R.chm/Output/` は 404。CA78K0R の一次資料は上の PDF が実質すべて）。

---
## 1. lmf のファイル形式（最重要）

### 結論
**lmf は ELF ではない。COFF 系（NEC/ルネサス拡張）のバイナリ。デバッグ情報（C の変数・型・行番号）は COFF のシンボル表・行番号表に入っており、DWARF ではない。**

根拠の強さ:
| 主張 | 強さ | 出典 |
|---|---|---|
| lmf は COFF フォーマット | **一次資料（ガイオ）＋実ファイル観察** | [GAIO-TUT] p.271「COFF フォーマット(*.lmf)には、ソースファイルのパス参照情報がないため、ソース検索パスを指定する必要があります」。実ファイルの先頭が COFF ヘッダ構造と整合（下記【観察】） |
| ルネサス純正デバッガは lmf を「XCOFF」と呼ぶ | 一次資料 | [ID78R] 表 5-2「Load module (XCOFF(.lnk, .lmf))」、ロードモジュール一覧の Format 欄に「XCOFF」、エラー Fc008「Specified load module file (COFF) is old version」。[ID78]（78K0 版）も同じ表記 |
| ELF/DWARF の記述は CS+ のビルド編・コーディング編に存在しない | 一次資料（全文検索で 0 件） | [BUILD][CODING]。lmf を「リンク結果の全情報を持つバイナリ・イメージのファイル」「リンクの結果のオブジェクト・コードのバイナリ・イメージ・ファイル」と説明するのみ（[BUILD] B.3.1 表 B-8、B.5.1 表 B-15） |
| 内部レイアウト（ヘッダ・セクション・シンボル表）の仕様書 | **不明（公開仕様なし）** | 下記【観察】のみが根拠 |
| lmf を ELF/DWARF に変換する公式ツール | **不明（見つからず）** | CC-RL の `.abs`（ELF）は別物。CS+ のリンカ／オブジェクトコンバータ（oc78k0r）に ELF 出力オプションは無い（[BUILD] B.5。出力は HEX 系と sym のみ）。e2 studio / GDB / IAR の変換機能も検索で確認できず |

デバッグ情報の付与:
- 【確認】コンパイラ `-g`（既定 `-g2`、オブジェクトとアセンブラ出力の両方）、リンカ `-g`（既定 on。「ロード・モジュール・ファイル中にデバッグ情報（ローカル・シンボル情報）を付加」）。リンカ `-ng` を付けると付かない。リンクする全モジュールを `-g` でコンパイルしないとソースデバッグ不可（[BUILD] B.1 -g/-ng、B.3 -g/-ng）。
- 【確認】アセンブラの `$DEBUG`（ローカルシンボル情報）/ `$DEBUGA`（アセンブラソースデバッグ情報）。`$TOL_INF` / `$DGS` / `$DGL` は「C コンパイラなどの上位プログラムが出す特別な制御命令」で書式は非公開（[CODING] 4.3.10）。

### 【観察】実ファイルの先頭構造（公開リポジトリの lmf 4 本で確認、すべて同じ）
対象: 下の項目 8 の一覧にある lmf（RL78 `dr5f100lg.dvf` 向け、RL78 `dr5f10bbg.dvf` 向け、78K0R `df182848.78k` 向け）。先頭 256 バイトと、シンボル表の先頭約 900 バイトだけを読んだ。

- 先頭 2 バイトは **`48 11`（リトルエンディアンで 0x1148、f_magic）**。`7F 'E' 'L' 'F'` ではない。
- 先頭 20 バイトは標準 COFF の `f_magic(2) f_nscns(2) f_timdat(4) f_symptr(4) f_nsyms(4) f_opthdr(2) f_flags(2)` と一致:
  - `f_nscns`: Sample_LED_100LG は 0x20 = 32。**リンクリストの「32 output segment(s)」と一致**
  - `f_nsyms`: 同 0x124 = 292。**リンクリストの「292 symbol(s) defined」と一致**
  - `f_symptr` = 0xB3C(2876)、シンボル 18 バイト × 292 = 5256 → 2876 + 5256 = 8132、ファイルサイズ 9008 との差 876 は文字列表と推測
  - `f_timdat` はビルド日時（Unix 時刻として妥当: 0x5DC15E9D = 2019-11-05、map の `Date: 5 Nov 2019` と一致）
  - `f_opthdr` = 0x44（68 バイト）、`f_flags` = 0x0003
- オプションヘッダ内（オフセット 0x2C 付近）に **デバイスファイル名**: `dr5f100lg.dvf`（RL78）、`dr5f10bbg.dvf`（RL78）、`df182848.78k`（78K0R）。
- セクションヘッダは 20+68=0x58 から始まり、先頭セクション名は `@@VECT00`。2 番目の `@@CNSTL` まで 0x34 = **52 バイト刻み**（標準 COFF の 40 バイトではない。拡張あり）。セクション名は CA78K0R のセグメント名そのもの（`@@VECT00`、`@@CNSTL`、`@@CODE`、`@@CODEL`、`@@R_INIT` ほか）。
- シンボル表（symptr 位置）に `.file`（sclass 0x67 = C_FILE、scnum 0xFFFE = N_DEBUG、補助エントリにソース名 `cstart.asm`、`r_main.c`）、`.bf` / `.bb` / `.eb`（C_FCN/C_BLOCK 相当の 0x65 / 0x64）、`_main`（値 0x22A2）などが並ぶ。→ **C のソースレベルデバッグ情報が COFF 流儀で入っている**。8 バイト名はインライン、長い名前は文字列表（と推測）。
- コンパイラ出力 `.asm` の `$DGS FIL_NAM, .file, 03BH, 0FFFEH, 03FH, 067H, 01H, 00H`（[BUILD] 3.1.1）の `0FFFEH`（scnum）と `067H`（sclass）が、lmf の `.file` エントリの `feff` と `0x67` に一致。**【推測】`$DGS` は COFF シンボルエントリ（名前、値、scnum、型、sclass、補助数…）をアセンブラ経由でオブジェクトに渡す疑似命令で、`$DGL` は行番号エントリ**。
- 【不明】型の符号化（n_type）の詳細、構造体メンバ・列挙・配列の補助エントリの形式、行番号表の形式、52 バイトセクションヘッダの追加フィールド、文字列表の位置と形式。実装するなら、上記ファイルを「ダウンロードして」解析する工程が必要（本調査ではダウンロードしていない）。一般の COFF（binutils の `coff/internal.h`、TI の COFF 仕様 SPRAAO8、Microsoft PE/COFF 仕様）は構造の参考になるが、**そのまま読めるツールは見つからなかった**（binutils/GDB/Ghidra/IDA/radare2/pyelftools に 78K0R の COFF ローダがあるという記述は検索で未発見）。

### winAMS との関係（詳細は項目 7）
winAMS は lmf を直接読まず、内蔵の「OMF 変換」で ガイオ独自の SAUF 形式 `*.xlo` に変換して使う（変換器名「RL78/78K0R RENESAS OMF Converter」、出力名 `target.lmf.xlo` または `target.xlo`）。SAUF/xlo の仕様は【不明】（ガイオ独自、非公開と推測）。

---
## 2. lmf 以外の出力と書式

### 生成物一覧【確認】（[BUILD] 2.16、B.3、B.5）
| 拡張子 | 内容 | 生成元 |
|---|---|---|
| `.lmf` | ロード・モジュール（COFF 系、デバッグ情報付き） | リンカ lk78k0r（`-o`） |
| `.map` | リンク・リスト（ヘッダ、`*** Memory map ***`、`-kp` でパブリックシンボル、`-kl` でローカルシンボル） | lk78k0r |
| `.elk` `.erp` `.eoc` | エラーリスト（リンカ/コンパイラ/オブジェクトコンバータ） | |
| `.hex` `.hxb` `.hxf` | HEX（既定 Intel 拡張 `-kie`。`-ki` Intel 標準、`-km` Motorola S（24bit）、`-kme` Motorola S（32bit）、`-kt` 拡張 Tektronix。拡張空間は `.H1〜.H15`） | oc78k0r（`-o`、`-k`） |
| `.sym` | シンボル・テーブル（デバッガ入力。拡張空間は `.S1〜.S15`） | oc78k0r（`-s`） |
| `.prn` | アセンブル・リスト（`-p`）。シンボルリスト（`-ks`）、クロスリファレンス（`-kx`）付き | ra78k0r |
| `.asm` | コンパイラ出力のアセンブラソース（`-sa` で C ソースをコメントに、`-g2` で `$DGS`/`$DGL`） | cc78k0r |
| `.rel` | オブジェクトモジュール（バイナリ、再配置情報・シンボル情報を含む） | |
| `.vfi` | 変数／関数情報ファイル（配置指定用の入力） | |
| `.cer` / `.pre` / `.cre` | コンパイラのエラー／プリプロセス／クロスリファレンスリスト | |

### `.sym` の書式【確認】[BUILD] B.5.2(4) ＋【観察】実ファイル
```
#05
;FF     PUBLIC
02FCFBA_i            ← 属性 2 桁 + 値 5 桁(16 進、20bit アドレス) + 名前（C の識別子は先頭に _ が付く）
0100187_main
00FFEE0_@STBEG
;FF     r_main       ← モジュール名ごとにローカルシンボルが続く
<0100185_r_wdt_interrupt   ← '<' で始まる行がそのモジュールのローカル（file static など）
=                    ← 終端
```
- 属性: 00=EQU 定数、01=コード・セグメント内ラベル、02=データ・セグメント内ラベル、03=ビット・シンボル、FF=モジュール名。ビットは「0FE00H からの相対 13 ビット + ビット位置 3 ビット」。
- **型・サイズ・構造体メンバのオフセットは `.sym` に無い**（アドレスと名前のみ）。関数かデータかはラベル属性（01/02）で区別できる。関数のサイズも無い。
- 関数内 static 変数: 【不明】（`.sym` のローカル部に出るかは実例で未確認。lmf の COFF シンボルには出ると推測）。ファイル内 static は `<` 行で出る（【観察】関数 `r_wdt_interrupt` が実例）。

### `.map` の書式【確認】[BUILD] 3.3 ＋【観察】
- ヘッダ: `78K0R Linker W1.70  Date: ...`、`Command:`（リンカへ渡したオプション全文）、`Out-file:`、`Map-file:`、`Direc-file:`、`Directive:`。続いて `*** Link information ***`（`32 output segment(s)` / `1CDH byte(s) real data` / `292 symbol(s) defined`）。
- `*** Memory map ***`: `SPACE=REGULAR` → `MEMORY=ROM`（`BASE ADDRESS=00000H SIZE=10000H`）→ 出力セグメント行（`@@VECT00  00000H 00002H  CSEG AT`）と入力モジュール行（`@@CODE  r_main  000C4H 0003CH`）の表。`* gap *` で空き。
- `*** Public symbol list ***`（`-kp`）: `MODULE / ATTR / VALUE / NAME`。ATTR は ADDR/NUM/CSEG/DSEG/…。`*** Local symbol list ***`（`-kl`）も同形式。
- 実例のリンク行: `-mi0 -s -gi... -go84h,1FE00h,512 -gb0FFFFE9H ... s0rll.rel ... -bcl0ram.lib -bcl0rm.lib -bcl0rmf.lib`（CA78K0R V1.60、リンカ W1.70）。
- map に型情報は無い。ただしセグメント配置（`@@DATA`、`@@INIT` など）は分かる。

### コンパイラ出力 `.asm`【確認】[BUILD] 3.1.1、[CODING] 3.5.4
ヘッダコメント → `$PROCESSOR(F1166A0)` → `$DEBUG` → `$TOL_INF ...` → `$DGS FIL_NAM, .file, ...`（シンボル情報）→ `PUBLIC _sym` 宣言 → セグメントごとのデータ定義（`@@INIT DSEG BASEP` / `_l_init : DS (4)`）→ 関数（`_main :` に続く命令。各行に `;[INF] size, clock`）→ 末尾の `*** Code Information ***`（`$FILE`、`$FUNC name(行)`、`void=(char c:x)` のような**引数・戻り値のレジスタ/スタック位置**、`CODE SIZE= .. bytes, CLOCK_SIZE= .., STACK_SIZE= ..`、`$CALL`）。
- 変数・関数の名前とセグメントは `.asm` で完全に取れる（`PUBLIC` と `_name : DS (n)`）。**サイズは `DS(n)` で分かる**が、型名や構造体メンバ構成は `$DGS` の中（書式【不明】）と C ソース（ヘッダ）頼み。
- 関数内 static / ファイル内 static はローカルラベルとして `@@DATA`/`@@INIT` に出る（【推測】名前は `PUBLIC` なし、重複回避のため修飾あり。実例未確認）。

### lmf が読めない場合の代替案（現実的な順）
1. COFF として自前解析（項目 1。型表現の解読が必要、作業量大）。
2. `.sym` ＋ `.map` ＋ `.asm`（`-sa`）で「名前→アドレス/サイズ」を得て、型は C ヘッダ（構造体定義）から別途解決。関数の引数・戻り値レジスタは `.asm` の `*** Code Information ***` で確認できる。
3. 実行用のコードは HEX（`.hex`）でも読める（Intel 拡張/Motorola/Tektronix）。

---
## 3. 関数呼び出し規約（CA78K0R。CC-RL と異なる）
出典: [CODING] 3.3、3.4、9 章、2.8、4.6、割り込み関数（#pragma vect/interrupt）、[MIG-C] p.26。以下は特記なき限り【確認】。

### 戻り値（[CODING] 表 3-17）
| 型 | 場所 |
|---|---|
| 1 ビット（`__boolean` 等） | CY |
| 1 バイト・2 バイト整数、near ポインタ | BC |
| 4 バイト整数、far ポインタ、浮動小数点 | BC（下位ワード）、DE（上位ワード） |
| 構造体 | 返却する構造体を関数固有の領域にコピーし、**そのアドレスを BC、DE に格納**（隠し引数ではない）。**【実測訂正】near データモデルでは BC に 16 ビットのアドレスだけが入り、DE は設定されない（末尾「probe による機械語の確認」(c)）** |
- 【推測→実測で確定】1 バイト戻りは C（BC の下位）。関数ポインタ戻りの明記は無く、near/far ポインタと同様と推測。構造体アドレスの BC=下位／DE=上位は 4 バイト整数と同じと推測。
- CC-RL との違い: CC-RL は A / AX / BC-AX（BC が上位）で、5 バイト以上は隠し引数。CA78K0R は **BC（DE:BC）**。

### 引数（表 3-18）
- **第 1 引数のみレジスタ、第 2 引数以降はすべてスタック**。第 1 引数が 1〜2 バイト整数・near データポインタ → AX（1 バイトは X）、3〜4 バイト（構造体・共用体・ポインタ含む）・関数ポインタ・far ポインタ・浮動小数点 → AX と BC、それ以外（5 バイト以上の構造体など）→ スタック。
- 【確定】3〜4 バイトのときは **AX が下位ワード、BC が上位ワード**（末尾「probe による機械語の確認」(a)。呼び出し側・呼ばれた側の命令列と実行で確認）。3 バイトデータの詰め方は未確認【不明】。
- スタック引数は右から左へ push（`func(a, b)` は b を先に push し、a は AX）。caller が `pop ax` / `add sp` で解放（**caller cleanup**）。1 バイト引数もスタック上は 2 バイトを占め、値は下位バイト。long は小アドレス側が下位ワード。
- 関数入口の SP: `[SP+0..3]` = 戻り番地（4 バイト）、`[SP+4]` = 最初のスタック引数（実例 [CODING] 9.3.2: `push hl; push ax; movw hl,sp; ... [HL+8]` が第 2 引数 long の下位）。
- プロトタイプなし/可変個引数部は int 拡張（char は int へ）。プロトタイプありの char はそのまま（拡張しない）。
- `register` 引数や `-qv`: 関数の先頭で引数を HL（スタックフレームがない場合のみ）や saddr `_@KREG12〜15`（`-qr` 時）に載せ替える。呼び出し側の規約は変わらない。
- `__pascal` / `norec` / `__leaf` / `noauto` / `__mxcall` / `callf` / `__callf` は 78K0 互換用の予約語で、CA78K0R では**ワーニングを出して無視**する（[CODING] 3.2.2）。`callf !addr11` は自動的に `call !addr16` に変換される（4.6.1）。

### 保存レジスタ
- 【確認】AX、BC、DE は作業用で、呼び出し先は復帰しない（caller-save）。呼び出し側が必要なら退避する（[CODING] 9.4.1）。
- 【確認】HL はベースポインタ。使う関数が入口で push し出口で pop する（callee-save）。
- 【確認】レジスタ変数用 saddr `_@KREG00〜15`（`-qr` 時）: コンパイラは呼び出し前に退避しない。**呼ばれる関数が変更するなら先に退避して復帰**する（callee-save）。
- 【確認】`_@RTARG0〜7`（ランタイムライブラリ引数）、`_@SEGAX/_@SEGBC/_@SEGDE/_@SEGHL`（セグメント情報）、`_@NRARG0〜3`/`_@NRAT00〜07`（コンパイラ予約）は割り込み関数が退避対象とする領域。通常の呼び出し間での扱いは明記なし（【推測】ランタイム/コンパイラ作業用で保存対象ではない）。
- ES、CS: 通常関数での保存規約は明記なし（【推測】caller-save。小モデルの関数ポインタ呼び出しは `mov CS,#00H; call ax`、far データアクセスは `mov ES,#highw(_x)`）。
- 割り込み関数は上記（使用レジスタ、ES/CS、saddr）を退避。`RBn` 指定時は `sel RBn` で切替（退避しない）。`NOBANK` は何もしない。

### 呼び出し命令・メモリモデル・近遠
- 【確認】`-ms` スモール（コード 64KB、データ 64KB、near のみ）、`-mm` ミディアム（コード 1MB: 関数は既定 far、データ near）、`-ml` ラージ（コード・データとも far）。`__near` / `__far` で個別指定（`-zf` 時のみ割り込み関数に `__far` 可）。
- 【確認】小モデルの呼び出し: `call !_func`（CALL !addr16）。ミディアム/ラージ: `call !!_func`（CALL !!addr20）。関数ポインタ（小）: `movw ax,!_fp; mov CS,#00H; call ax`。（中）: 4 バイトポインタ（下位ワード＋ページ）を `mov a,!_fp+2; mov CS,a; movw ax,!_fp; call ax`。
- 【確認】CALL/CALLT のスタック消費は 4 バイト、`RET` が 4 バイト復帰、割り込みと BRK も 4 バイト（PSW が 00H の上の 1 バイトに入る）、`PUSH rp` と `PUSH PSW` は 2 バイト。near/far を問わず戻り番地は 4 バイト（CC-RL と同じ）。
- 【確認】`__callt`（`callt [?func]`、テーブルは 0x80〜0xBF の `@@CALT`、最大 32/30 個）。`callf` は消えた。
- 【確認】割り込み: `#pragma vect|interrupt 割り込み要求名 関数名 [SP=配列+オフセット] [STACK] [NOBANK] [RB0〜RB3]`、`__interrupt` / `__interrupt_brk` 修飾も可。ベクタは `@@VECTnn CSEG AT 00nnH` に `_@vectnn : DW _func`。割り込み関数は `@@BASE`（near）で `reti`。引数・戻り値なし（void）。割り込み要求名は大文字。（CC-RL の `vect=addr` 形式と異なる）
- 【確認】`DI()`/`EI()`/`HALT()`/`STOP()`/`NOP()`/`BRK()` は組み込み関数（コンパイラが命令に展開）。

### 実例（[CODING] 3.3.2）
`func0(0x1234, 0x5678)` → `movw ax,#5678H; push ax; movw ax,#1234H; call !!_func0; pop ax`。

---
## 4. シンボル名・スタートアップ・セグメント
- 【確認】C の識別子には `_` が前置される（関数 `main` は `_main`、変数 `i` は `_i`）。コンパイラ生成ラベルは `?`（`?f_clt` = callt 用）や `_@` 系（`_@STBEG`）。アセンブラ側も同名で `_FUNC` を `PUBLIC`。
- 【確認】スタートアップ: ソース `cstart.asm`（標準ライブラリ処理あり）/ `cstartn.asm`（なし）。オブジェクト名は `s0r*.rel`（`m` = スモール/ミディアム、`l` = ラージ。`l` が付くのは far データ、`b` = ブート領域用、`e` = フラッシュ領域用。実例 `s0rll.rel`）。入口は `@@VECT00 CSEG AT 0: DW _@cstart`（リセットベクタ）。流れは `SEL RB0` → ミラー設定（`MOV1 MAA,CY`、`_@MAA`）→ `MOVW SP,#LOWW _@STBEG` → `CALL !!_hdwinit`（ユーザ定義、無ければ空）→ ROM 化（初期値コピー）→ 0 クリア → `CALL !!_main` → `CALL !!_exit`（`_@cend`）。
- 【確認】リンカ生成シンボル: `_@STBEG`（スタック先頭＝最上位+1。実例 0xFFEE0）、`_@STEND`、`_@MAA`、`_@SADBEG`、`_@SADSIZ`（[BUILD] 表 B-9）。
- 【確認】セグメント（[CODING] 3.5）: コード `@@CODE`（near、BASE）/ `@@CODEL`（far）/ `@@LCODE` `@@LCODEL`（ライブラリ）/ `@@BASE`（割り込み・callt 関数）/ `@@CALT` / `@@VECTnn`。ROM データ `@@CNST`（near、`MIRRORP`）/ `@@CNSTL`（far、`PAGE64KP`）。初期値つきデータの ROM 側 `@@R_INIT`（near）/ `@@RLINIT`（far）/ `@@R_INIS`（sreg）。RAM 側 `@@INIT` / `@@INITL` / `@@INIS`。初期値なし `@@DATA` / `@@DATAL` / `@@DATS`（sreg、saddr）。`@@BITS`（bit/`__boolean`）。`@@CNSTR` `@@CODER` は RAM 配置版。
- 【確認】ROM→RAM コピー: `_@R_INIT`〜`_?R_INIT` を `_@INIT`〜 へバイトコピー（far 時は `ES:`）。初期値なしの外部変数と sreg は 0 クリア。ラベルは各セグメント `_@seg`（先頭）と `_?seg`（末尾）。`@rom`（`rom.asm`、ライブラリ内）が最終アドレスを定義。**シミュレータが `_@cstart` から走らせれば初期化は自動で行われる**。
- 【確認】ミラー: 64KB のフラッシュ（`MAA=0` なら 0x00000〜0x0FFFF、`MAA=1` なら 0x10000〜0x1FFFF）を 0xF0000〜0xFFFFF に読み出し専用でミラー（SFR、拡張 SFR、RAM、使用不可領域は除く。命令フェッチ不可）。`@@CNST`（`MIRRORP`）はこのミラー側のアドレスに割り当てられうる。
- 【観察】`-go84h,1FE00h,512` などオンチップ・デバッグ領域と `-gb` オプションバイトが lmf に入る（0xC0〜0xC3、0xCE〜0xD7 に特別値。[BUILD] B.3）。

メモリ配置（[CODING] 図 3-2）: 0x80〜0xBF callt、0xFFE20〜0xFFEB3 sreg/boolean、0xFFEB4〜0xFFEC3 レジスタ変数 `_@KREG00〜15`、0xFFEC4〜0xFFED3 コンパイラ予約、0xFFED4〜0xFFED7 セグメント情報、0xFFED8〜0xFFEDF `_@RTARG0〜7`、0xFFEE0〜0xFFEFF 汎用レジスタ（RB3〜RB0）、0xFFF00〜0xFFFFF SFR、0xF0000〜0xF07FF 拡張 SFR。

---
## 5. 型のサイズと配置（CC-RL との違い）
出典: [CODING] 3.1.1、3.1.2、2.2.x、[MIG-C] p.5〜7。【確認】。
| 項目 | CA78K0R | CC-RL |
|---|---|---|
| char | 1B。**無修飾 char は signed**（`-qu` で unsigned、マクロ `__CHAR_UNSIGNED__`） | 無修飾 char は unsigned（`-signed_char` で signed） |
| short/int | 2B | 同じ |
| long | 4B | 同じ |
| float/double/long double | すべて **4B IEEE754 単精度**（little-endian） | double 既定 4B（`-dbl_size=8` で 8B） |
| long long | 非サポート | 8B |
| ポインタ | near 2B、far 4B（20bit アドレス + ページ） | 同じ |
| size_t / ptrdiff_t | unsigned int / int（2B） | |
| enum | 全列挙定数を表せる最初の型: signed char → unsigned char → signed int | char/signed char/unsigned char/signed short |
| 構造体メンバ整列 | char 系は 1B 境界、それ以外（ポインタ含む）は 2B 境界。**構造体自体もワード境界に置かれる**（[MIG-C] p.7 の図で、`struct{char a,b;}` を含むと 2B 境界から始まる）。最後は偶数に詰める【推測】 | 最大メンバの整列条件に合わせる（`_Bool` も 1B） |
| ビットフィールド | 型指定子なしの int は unsigned。割り付けは**下位ビットから**（`-rb` で上位から）。char 系は 1 バイト境界、short/int 系は 1 ワード境界をまたがない（`-rc` で緩和）。`unsigned char`/`signed char`/`short` 型も指定可 | 下位から。`-signed_bitfield` で符号 |
- エンディアン little。符号付き整数は 2 の補数。
- 【確認】浮動小数点: 符号 1・指数 8（バイアス 0x7F）・仮数 23 ビット、最近偶数丸め。除算・オーバーフロー時は `matherr` を呼ぶ。
- 【確認】`__sreg` / `sreg` 変数（saddr 領域）と `__boolean` / `bit`（`@@BITS`、saddr/sfr ビット）は CA78K0R 固有。

---
## 6. 命令セット: 78K0R と RL78
- 【確認】[CODING] 4.6.5「RL78 ファミリ，78K0R マイクロコントローラ製品の命令は、**すべて共通**です」。メモリ空間 1MB、`CALL` 時スタック 4 バイト、ES/CS レジスタ、20 ビット PC も共通（4.6.2〜）。
- 【確認】拡張命令（`MULHU`、`MULH`、`DIVHU`、`DIVWU`、`MACHU`、`MACH`）は「製品ごとに搭載/非搭載が異なる」（[CODING] 4.6.5 乗除積和算命令の注）。`MULU X` は全製品。クロック: MULU 1、MULHU/MULH 2、DIVHU 9、DIVWU 17、MACHU/MACH 3（表 4-35）。
- 【確認】CA78K0R のマクロ: `__K0R__`（共通）、`__RL78__`（RL78）、`__RL78_1__`（拡張命令なし、レジスタバンクあり）、`__RL78_2__`（拡張命令あり）、`__RL78_3__`（拡張命令なし、バンクなし = S1 コア）、`__CA78K0R__`、`__K0R_SMALL__` `__K0R_MEDIUM__` `__K0R_LARGE__`。
- 【確認】ライブラリに「乗算器」「乗除算器」「積和演算器」を使う版があり、割り込み禁止区間がある（[CODING] 6.14〜6.15、`repmul.bat` / `repmuldiv.bat` / `repmac.bat`）。【推測】78K0R の乗除算器は周辺機能（SFR 経由）。シミュレータがそれらのライブラリ版を使った lmf を実行するなら、該当 SFR のエミュレーションが要る。使うかどうかは `-bcl0rm.lib` 等のリンク指定、および lmf 内のシンボルで判別できる。
- 【確認】RL78 の S1 コアは `SEL RBn` とレジスタバンクなし（`ccrl_abi.md` と [CODING] 割り込み関数の制限「RL78 8 ビット CPU はレジスタ・バンクが 1 つ」）。
- 【確認】クロック数は 78K0 に比べパイプライン化で短い。RL78 と 78K0R の命令ごとのクロック差は、この資料（コーディング編の命令表）では**区別されていない**。【不明】
- 【不明】78K0R にだけある/ないレジスタ・割り込み・周辺の差（デバイス依存。各品種のユーザーズマニュアルに依存）。
- デバイスファイル指定: 【観察】lmf のオプションヘッダに `df182848.78k`（78K0R）/ `dr5f100lg.dvf`（RL78）。`-c` コンパイラオプションで `-cf1166a0` のような品種名を指定。

---
## 7. ガイオ winAMS（カバレッジマスター）の扱い
- 【確認】[GAIO-TUT] p.253/269/271: **winAMS がテストに使える形式は `*.xlo`（ガイオ独自 SAUF 形式）だけ**。他社コンパイラの実行形式は、winAMS 内蔵の「OMF 変換」で `*.xlo` にする。CA78K0R の場合: 新規プロジェクトで MPU=「78K0R」または「RL78」、コンパイラ=「RENESAS(CA78K0R)」を選ぶと OMF 変換が有効になり、コンバータ名「**RL78/78K0R RENESAS OMF Converter**」に `target.lmf` を指定、`OMF 自動変換` を ON にすると `target.lmf.xlo`（78K0 編の記述では `target.xlo`）ができる。変換は「デバッグ情報を変換するだけで、命令コードには影響しない」。
- 【確認】winAMS が必要とする情報: 変数シンボル名、ソースファイルパス、ソース行と実行コードの対応（ライン情報）。つまり **lmf（COFF）の型付きシンボル表と行番号表を読んで SAUF に変換している**。lmf の COFF にはソースファイルのパス情報が無いため、`ソース検索パス` の指定が必要（p.271）。
- 【確認】ビルド設定: 最適化なし（`-nq`）、デバッグ情報を生成（`-g`）、リンク `-g`。
- 【確認】実行エンジン: ガイオの System-G シミュレータ。78K/0R = `sxgue7ss V1.09`（**販売終了**）、RL78 = `sxgus4 V1.10.0`（販売中、IAR/RENESAS コンパイラ対応）。出典 [GAIO-MPU]（2026/11/06 更新と表示）。
- 結論: **winAMS は lmf を直接は読まず、xlo に変換して読む**。利用者の手元に `*.lmf.xlo`（または `*.xlo`）があれば、それはデバッグ情報込みの SAUF 形式。ただし SAUF の仕様は【不明】（公開資料に見当たらない）。xlo から変数名/型/アドレスを抜く道は、仕様が得られない限り使えない。lmf が手に入らず xlo だけがある場合に備え、ガイオにフォーマット仕様の提供可否を問い合わせる価値はある（推測）。
- 【不明】winAMS が lmf を ELF に変換するか、COFF を直接解釈するかの内部処理。

---
## 8. CA78K0R でビルドされた `.lmf` が公開されているリポジトリ
`gh api search/code` と `gh api repos/*/git/trees/HEAD?recursive=1` で確認（ダウンロード/クローンはしていない。マジックバイトは raw URL の Range で読んだのみ）。検索はレート制限（403）に当たって途中で終えたため、**網羅ではない**。lmf はバイナリなのでコード検索には出ず、`.mtpj`/`.map`/`lk78k0r` 等のヒットから辿った。

| リポジトリ | ライセンス | lmf（サイズ） | 対象品種 | 同梱物 |
|---|---|---|---|---|
| https://github.com/rongkunxue/XJTU-Automation-share  パス: `（实验）嵌入式/2019计算机原理与嵌入式系统设计专题实验/瑞萨实验板/例程/<Sample名>/<Sample名>/DefaultBuild/<Sample名>.lmf` | **BSD-3-Clause**（リポジトリ全体。中身は大学実習資料とルネサス配布のサンプル。ライセンス適用の範囲は不確か） | Sample_LED_100LG 9,008B / Sample_Buzzle_100LG 12,512B / Sample_AD_100LG 13,852B / Sample_I2C_100LG 15,380B / Sample_CSI_100LG 15,468B / Sample_IR_100LG 15,500B / Sample_LCD_100LG 16,616B（別ディレクトリに 19,920B）/ Sample_Key&LED_100LG 16,328B / Sample_Timer & LED_100LG 10,984B / Sample_UART_100LG 13,248B / RL78_FunctionDemoCode 22,460B ほか。RZH 配下にも複製あり | RL78/G13 `R5F100LG`（lmf 内 `dr5f100lg.dvf`）。CA78K0R V1.60、リンカ W1.70、2019-11 ビルド【観察】 | **最適**。同じ `DefaultBuild/` に `.map`（Sample_LED で 15,632B）、`.sym`（898B）、`.hex`（360,789B）、`.rel`、`.mtpj`、C ソース（`r_main.c`、`r_cg_*.c`）、`function.html`。lmf の先頭 `48 11 20 00` を確認 |
| https://github.com/laixianghong/D53_AVM  パス: `SOFTWARE/MCU/D53_AVM/DefaultBuild/` | なし（権利者に全権利。再配布不可の扱い） | `D53_AVM.lmf` 360,264B、`N3_AVM.lmf` 325,320B | 78K0R `uPD78F1828`（`df182848.78k`）【観察】 | `.sym`（23,371B / 21,158B）、`.prn`、`D53_AVM.mtpj`、`QualityReport(...,DefaultBuild).txt`。`.map` は未確認。C ソースは別ディレクトリにある可能性【未確認】。lmf 先頭 `48 11 41 00` を確認 |
| https://github.com/chuoli/MS11-2023071802  パス: `MS11-2023071201-0XED7A/Project/DefaultBuild/` | なし | `JF_CarRefrigerator.lmf` 96,184B | RL78 `R5F10BBG`（`dr5f10bbg.dvf`）、2023-07 ビルド【観察】 | `.sym`（7,722B）、`.mtpj`（309,889B）、`QualityReport(...)`。ソース/`.map` は未確認。先頭 `48 11 36 00` を確認 |
| https://github.com/userdevil/rl78Bootlader  パス: `DefaultBuild/bootloader.lmf` | なし | 38,928B | RL78 系（品種未確認） | `.map`（26,091B）、`.sym`（4,609B）、`.hex`、`.rel` 一式、`.dr`、C ソース（`Command.c` ほか）。マジック未確認 |
| https://github.com/SreedharanNandu/EmbeddedProjects  パス: `BootProjects/uD78F1827_*/DefaultBuild/*.lmf` | なし | `Test_Project.lmf` 18,464B、`UDS_App.lmf` 166,856B | 78K0R `uPD78F1827`【推測: パス名から】 | C ソース多数。`.map`/`.sym` は未確認。マジック未確認 |
| https://github.com/Project-HOS/hos-v4  `sample/78k0r/sample.prj`、`lib/78K0R/CC78K0R/` | NOASSERTION | lmf は無し（makefile/prj のみ） | 78K0R | ビルド手順の参考（`lk78k0r`/`oc78k0r` の呼び出し例） |
- 用途の提案: **rongkunxue の Sample_LED_100LG が、lmf（9KB）・`.map`・`.sym`・`.hex`・ソースが揃った最小のテストデータ**。実装担当は、これを自分でダウンロードしてパーサのテストに使うか（BSD-3-Clause 表記だがサードパーティ・サンプル混在のため、リポジトリに取り込むのではなくローカルでの検証用に留める方が安全）、利用者の実 lmf（機密）に対する自前の最小 C ソースを CA78K0R でビルドして作るのが筋。ダウンロードやインストールは本調査では行っていない。
- 他に参考: `https://github.com/shoui520/psp2`（`tools/ca78k0r.py`、`lk78k0r.py`: ビルド呼び出し）は lmf を含まない。

---
## 実装担当への要点（まとめ）
1. lmf は **COFF 系**（magic 0x1148）。ELF/DWARF 用の既存パーサは使えない。構造は標準 COFF + 52 バイトのセクションヘッダ + 68 バイトのオプションヘッダ。型情報の符号化は仕様が無く、実ファイルの解析が必要。
2. 名前・アドレスだけで足りるなら `.sym`（アドレス+名前、先頭に `_`）と `.map` が最短。ただし型/サイズ/メンバ・オフセットは無い。
3. 呼び出し規約は CC-RL と大きく違う: 第 1 引数のみ AX（（4 バイトは AX+BC））、残りはスタック（右から左）、caller が解放、戻りは BC（4 バイトは DE:BC）、HL と `_@KREG` は callee-save、AX/BC/DE は破壊される。戻り番地は 4 バイト。小モデルは `call !addr16`、中/大は `call !!addr20`。
4. 型: int=2、long/float/double=4（double も 4）、char は signed、構造体は 2 バイト整列で入れ子も 2 バイト境界。enum は 1B になりうる。
5. winAMS は `RL78/78K0R RENESAS OMF Converter` で lmf → xlo（SAUF）に変換して使う。SAUF の仕様は不明。

## 第1段階実装での機械語確認（2026-11-14）

`tests/tools/check_lmf.c` が CPU コアの `rl78_disasm_buf` で実物を逆アセンブルした。
サンプル自体とその機械語はリポジトリへ取り込んでいない。

- **1B の第2引数は2B枠【観察】**: RL78_FunctionDemoCode の `lcd_init`:
  `0x00582 CLRW AX; 0x00583 PUSH AX; 0x00584 MOV X,#38H; 0x00586 CALL !!00503H; 0x0058A POP AX`。
  呼び先 `lcd_write(uint8_t data,uint8_t op)` は `0x00503 PUSH HL; 0x00504 PUSH AX; 0x00505 PUSH AX;
  0x00506 MOVW HL,!0FFF8H`（SP を HL へ）、`0x00514 MOV A,[HL+0AH]` で op を読む。
  入口からの退避6Bと戻り番地4Bにより、入口SP+4が第2引数の下位バイト。caller の POP AX も2B枠を裏付ける。
  追加例: Sample_CSI の main `0x00233 ONEW AX; 0x00234 PUSH AX; 0x00235 MOV X,#06H;
  0x00237 CALL !!003C0H; 0x0023B POP AX; 0x0023C MOV A,C` は
  `R_CSI10_CmdSend(WREN,1)` の1B引数と C の1B戻り値を確認できる。
- **4B 第1引数の AX/BC 順は未確認（→ 第2段階の probe で確定。末尾参照）**: 5件のデバッグ情報の全関数を調べたが、該当するC関数も呼び出し対もない。
  `long` ローカルと演算ランタイムは存在するが、ランタイム固有の規約からC ABIを推測しない。
  現実装は「AX/BC の順序が未確認のため対応外」として拒否する。3B引数も対応外。
- near/far CALL ともシミュレータ側の入口PCは20bitのままで、戻り番地を4B積む既存の処理を共有する。
  LMFの関数ポインタ型・far型の符号化は未確認なので復元しない。nearデータポインタだけ0xF0000を補う。
  （→ 関数ポインタ・far ポインタの符号化も probe で確定。`docs/LMF_FORMAT.md` §5.6。）

### 実物5件の確認結果

`LMF_SAMPLES=/path/to/lmf_samples tests/rl78/check_lmf_samples.sh`。
Cローダの定義バイトは `.hex` と全件一致。`.sym` の名前とアドレスの集合も一致
（公開/ローカルを含む。KeyandLED のbit 1件は位置の符号化が未確認のため除外）。

| サンプル | 定義バイト | 比較シンボル | 無設定時の発振待ちPC | スタブ後 main | 呼び出しCSV |
|---|---:|---:|---|---|---:|
| LED | 461 | 40 | 0x001EB | 0x00187 | 1/1 |
| AD | 1201 | 76 | 0x00317 | 0x002CE | 1/1 |
| CSI | 1086 | 87 | 0x002C5 | 0x0021C | 1/1 |
| KeyandLED | 1346 | 75（bit除外） | 0x002B6 | 0x001F6 | 4/4 |
| FunctionDemoCode | 2789 | 106 | 0x00485 | 0x00966 | 1/1 |

全件 `R_CGC_Create` の OSTC 待ち。確認用設定は次のとおり。
```
startup_max_insns = 1000000
stub = R_CGC_Create:R_CGC_Get_ResetSource
```
これで周辺クロック設定だけを読み取りのみの既存void関数に置換し、残りのスタートアップ（データコピー・ゼロ初期化）は実行する。
`init_end = R_CGC_Create` でも待ちの前に止められるが、RAM初期化前なので今回の呼び出し検証には使っていない。

期待値の根拠（Cソースを読んで手計算）:
LED の `r_main.c/R_MAIN_UserInit` は EI のみなので入力した i=123を保持。
AD の `r_cg_adc.c/delay(3)`、FunctionDemoCode の `r_cg_AD.c/AD_delay(3)` は引数を減らすループだけで、
それぞれ mun[0]=123 / time_lenth=123を保持。
CSI の `r_cg_serial_user.c/r_csi10_callback_sendend` は end_txif=1。
KeyandLED の `r_cg_digtalLED.c/Getcode(0,1,9,22)` の各呼び出しは63,6,103,0を返す。
実物で未対応警告となったのは `bit_control`: 型0x034C / 記憶クラス0x27のみ
（`.sym` のbit属性0x03も照合対象外として警告）。CPUコアの不足・バグはこの確認では見つからなかった。

---
## probe による機械語の確認（第2段階、2026-11-14）

材料: 試験用ソース `tests/ca78k0r_probe/probe.c` を CA78K0R V1.60（ミディアムモデル、RL78/F14 R5F10PGH、最適化なし相当）でビルドした
`DefaultBuild/ca78k0r_probe.lmf`。ROM は `.mot`（lmf のイメージと全バイト一致）。以下の命令列は 2 通りで確認した。
(1) lmf のイメージを `rl78_disasm_buf()`（RL78 CPU コアの逆アセンブラ。表記は CA78K0R 形式）と GNU `rl78-elf-objdump` の両方で逆アセンブルし、両者の命令が一致することを確認。
(2) **CPU コア（`rl78_exec.c`、S3 コア、ROM 192KB、ミラー 0xF3000–0xFBEFF→0x3000）に lmf のイメージを載せてリセットから実行**し、
`main` の各 `CALL` の直前・直後と各関数の入口にブレークポイントを置いてレジスタとスタックを採取した（下の「実行」欄はその値）。
実行結果は `g_uc=0xC5`、`g_ul=0x02030A15`（f_mix の戻り値）、`g_l=0x10101010`（f_long2）など、C の意味から手計算した値と一致した。

### (a) 4 バイトの第 1 引数: AX が下位ワード、BC が上位ワード【確定】
呼び出し側（`main`。`g_ul = f_u32(g_ul)` と `f_long2(0x11121314L, 0x01020304L)`）:
```
0065D AF78C0    MOVW AX, !0C078H     ; g_ul の下位ワード（g_ul = 0x20212223 なので 0x2223）
00660 DB7AC0    MOVW BC, !0C07AH     ; g_ul の上位ワード（0x2021）
00663 FCB00400  CALL !!004B0H        ; f_u32
...
00696 301413    MOVW AX, #1314H      ; 0x11121314 の下位ワード
00699 321211    MOVW BC, #1112H      ; 上位ワード
0069C FC020500  CALL !!00502H        ; f_long2（第 2 引数は先にスタックへ。(b)）
```
呼ばれた側（`f_u32` の入口）:
```
004B0 C7        PUSH HL
004B1 C3        PUSH BC              ; 先に BC（上位）
004B2 C1        PUSH AX              ; 次に AX（下位）= 低アドレス
004B3 FBF8FF    MOVW HL, !0FFF8H     ; HL = SP
004B6 AB        MOVW AX, [HL]        ; [HL+0..1] = 引数の下位ワード
004B7 BDD8      MOVW 0FED8H, AX      ; _@RTARG0（ランタイムの第 1 オペランド下位）
004B9 AC02      MOVW AX, [HL+02H]    ; [HL+2..3] = 上位ワード
004BB BDDA      MOVW 0FEDAH, AX      ; _@RTARG2（上位）
```
つまり引数は「AX+BC を push して `[SP+0..3]` にリトルエンディアンの 32 ビット値として置く」形になる。
実行（`f_u32` 入口）: `AX=2223 BC=2021`（g_ul=0x20212223）。`f_long2` 入口: `AX=1314 BC=1112`。`f_float` 入口（g_f=1.5f=0x3FC00000）: `AX=0000 BC=3FC0`。
`f_farptr(unsigned char __far *p)` 入口（p=&g_uc=0xFC06A）: `AX=C06A BC=000F`。far ポインタは「AX=下位 16 ビット（オフセット）、BC=上位（C=ページ=ES の値、B=0）」で、32 ビット整数と同じ並び。
呼ばれた側は `MOVW DE,AX`（オフセット）、`MOV A,[HL+02H]; MOV ES,A`（ページ）、`MOV A,ES:[DE]` で読む（`f_farptr` 0056C–0057F）。

### (b) スタックに積まれる引数の順と枠【確定】
`g_ul = f_mix(0x01, 0x02030405UL, 0x0607, 0x08)`（`unsigned char a, unsigned long b, unsigned int c, unsigned char d`）:
```
0066F 300800    MOVW AX, #0008H      ; d
00672 C1        PUSH AX              ; 最後の引数から先に push（右から左）
00673 300706    MOVW AX, #0607H      ; c
00676 C1        PUSH AX
00677 300302    MOVW AX, #0203H      ; b の上位ワード
0067A C1        PUSH AX              ; 4 バイトの引数は、上位ワードを先に push → 下位ワードが低アドレス
0067B 300504    MOVW AX, #0405H      ; b の下位ワード
0067E C1        PUSH AX
0067F E6        ONEW AX              ; a = 1 → 第 1 引数は X（AX=0001）
00680 FCCF0400  CALL !!004CFH
00684 1008      ADDW SP, #08H        ; 呼び出し側が 8 バイト解放（(d)）
```
実行（`f_mix` 入口、SP=0xFEC0）のスタック `[SP+0..13]` = `84 06 00 00 | 05 04 03 02 | 07 06 | 08 00 | …`:
- `[SP+0..3]` 戻り番地（4 バイト。`CALL !!` でも near `CALL !` でも 4 バイト）。0x00684。
- `[SP+4..7]` b = 0x02030405（**下位ワードが低アドレス**）。
- `[SP+8..9]` c = 0x0607。
- `[SP+10..11]` d: **1 バイトの引数も 2 バイトの枠**（値は下位バイト、上位バイトは 0。呼ぶ側は `MOVW AX,#0008H; PUSH AX` で積む）。
枠の大きさ: 1 バイト=2、2 バイト=2、4 バイト=4。第 1 引数（a）はスタックに載らず X（4 バイトなら AX+BC）。
呼ばれた側は `PUSH HL; PUSH AX; MOVW HL,SP` の後、第 2 引数以降を `[HL+8]`（b 下位）、`[HL+10]`（b 上位）、`[HL+12]`（c）、`[HL+14]`（d）で読む（004CF–0050A）。
`f_long2(a, b)` の b（第 2 引数、4 バイト）は `[HL+10]`/`[HL+12]`（PUSH HL/BC/AX の 6 バイト＋戻り番地 4 バイト）。lmf の `_b` の value（0x0A）と一致（`docs/LMF_FORMAT.md` §6.2）。

### (c) 戻り値のレジスタ【確定。ただし未観測の型あり】
| 型 | 戻り値 | 根拠（実行値は 0x20212223 等を渡した結果） |
|---|---|---|
| 1 バイト（`unsigned char`、1 バイトの enum） | **C**（B の値は当てにならない。実際は 0） | `f_u8`: 呼び出し側 `00649 62 MOV A,C`。実行 `BC=0014`（0x13+1）。`f_enum`、`f_farptr` も `MOV A,C` |
| 2 バイト（`unsigned int`、near ポインタ） | **BC** | `add2` 実行 `BC=1B1D`（0x1A1B+0x0102）、呼び出し側 `00659 13 MOVW AX,BC` |
| 4 バイト（`long`、`unsigned long`） | **BC=下位ワード、DE=上位ワード** | `f_u32`（0x20212223+0x01020304）実行 `BC=2527 DE=2123`。`f_mix` 実行 `BC=0A15 DE=0203`（0x02030A15）。`f_long2` 実行 `BC=1010 DE=1010`。呼び出し側 `00667 13 MOVW AX,BC; 00668 BF78C0 MOVW !0C078H,AX; 0066B 15 MOVW AX,DE; 0066C BF7AC0 MOVW !0C07AH,AX` |
| float（double も同じ 4 バイト） | **BC=下位ワード、DE=上位ワード** | `f_float`（1.5f×2.0f=3.0f=0x40400000）実行 `BC=0000 DE=4040`。戻る直前の `0051A DAD8 MOVW BC,0FED8H`（`_@RTARG0`）と `00518 EADA MOVW DE,0FEDAH`（`_@RTARG2`）が同じ組み |
| enum（probe では 1 バイトと 2 バイトのみ） | 1 バイトなら C、2 バイトなら BC | `f_enum`: `005CC E6 ONEW AX; 005D0 12 MOVW BC,AX`、呼び出し側 `00724 62 MOV A,C` |
| 構造体（6 バイトの `struct inner`） | **関数の中の static な一時領域（`@@DATA` の `L0027`、0xFC05E）へコピーして、その near アドレスを BC に入れる。DE は設定しない** | `f_ret_struct`: 下記。実行 `BC=C05E`、`DE=C088`（呼ぶ前から入っていた無関係の値のまま） |
`f_ret_struct`（0059A–005C5）と呼び出し側（006FC–0071A）:
```
005AF 9C04      MOV [HL+04H], A      ; ローカル r に値を作る
005B1 C7        PUSH HL
005B2 17        MOVW AX, HL
005B3 16        MOVW HL, AX
005B4 5206      MOV C, #06H
005B6 92        DEC C                ; 6 バイトを r → 0xC05E のバッファへコピー
005B7 61E9      MOV A, [HL+C]
005B9 285EC0    MOV 0C05EH[C], A
005BC DFF8      BNZ $005B6H
005BE C6        POP HL
005BF 325EC0    MOVW BC, #0C05EH     ; BC = バッファの near アドレス（0xFC05E の下位 16 ビット）
...
006FC FC9A0500  CALL !!0059AH
00700 17        MOVW AX, HL          ; 呼び出し側は BC の指す 6 バイトを自分のローカル t へコピー
00701 14        MOVW DE, AX
00702 5006      MOV X, #06H
00704 490000    MOV A, 0000H[BC]
00707 A3        INCW BC
00708 99        MOV [DE], A
```
**リエントラントではない**（グローバルの一時領域）。構造体（6 バイト）の中身は、パディングまで含めてバイトコピーされる（実行で `t` のパディング（5 バイト目、0 起算）に、コピー元の古い値 0xC0 が入っていた）。
4 バイト以下の構造体、far ポインタ、`__far` データモデルでの構造体、浮動小数点以外の 8 バイト型は probe に無く、**返し方は未確認【不明】**（far ポインタの戻り値は、データを返す関数の戻りが 4 バイトなら BC/DE と推測【推測】）。

### (d) 呼び出し後のスタックの解放: 呼び出し側【確定】
スタック引数を積んだ呼び出し側が、戻った直後に `POP AX`（2 バイト）または `ADDW SP,#n`（それ以上）で解放する。
```
00650 C1 PUSH AX → 00654 FCA30400 CALL !!004A3H(add2) → 00658 C0 POP AX
0066F…0067E PUSH×4 → 00680 CALL !!004CFH(f_mix)          → 00684 1008 ADDW SP,#08H
00691/00695 PUSH×2 → 0069C CALL !!00502H(f_long2)        → 006A0 1004 ADDW SP,#04H
```
実行（SP）: f_mix は呼ぶ前 `FEC4`（0xFECC−8）、戻った直後 `FEC4`（RET が戻り番地 4 バイトを取るだけ）、`ADDW SP,#8` の後 `FECC`。引数が第 1 引数だけ（スタック引数なし）の呼び出しは何もしない。
呼ばれた側は、入口で保存した `PUSH HL` と第 1 引数の退避（`PUSH AX`/`PUSH BC`）、ローカル（`SUBW SP,#n`）を、`ADDW SP,#n`/`POP AX; POP HL` で自分で戻してから `RET` する（スタック引数は触らない）。
**HL は呼ばれた側が保存・復帰する**（実行で `main` の HL=0xFECC が全呼び出しで不変）。AX/BC/DE は壊れる（実行で確認）。

### (e) 関数ポインタ経由の呼び出し（ミディアム）【確定】
`g_ui = g_fn(1, 2)`（`g_fn` は 4 バイト `{オフセット, CS ページ, 0}` = `A3 04 00 00`）:
```
0074E E6        ONEW AX
0074F A1        INCW AX              ; AX = 2
00750 C1        PUSH AX              ; 第 2 引数（スタックへ）
00751 EB04C1    MOVW DE, !0C104H     ; DE = g_fn のオフセット（0x04A3）
00754 8F06C1    MOV A, !0C106H       ; A = ページ（0）
00757 9EFC      MOV CS, A            ; CS = ページ
00759 E6        ONEW AX              ; 第 1 引数 = 1
0075A 61EA      CALL DE              ; PC = CS:DE。戻り番地は 4 バイト
0075C C0        POP AX               ; スタック引数の解放
0075D 13        MOVW AX, BC          ; 戻り値は BC
```
関数ポインタの代入 `g_fn_tbl[0] = f_void` は `30 96 04 MOVW AX,#0496H; BF56C0 MOVW !0C056H,AX; CF58C0 00 MOV !0C058H,#00H`（オフセットとページの 3 バイトだけ書く。4 バイト目は使われない）。
実行: `CALL DE` 時 `AX=0001 DE=04A3 CS=00`、`add2` 入口のスタック `[SP+0..3]=5C 07 00 00`（戻り番地 0x075C）`[SP+4..5]=02 00`。AX/BC/DE の役割は直接呼び出しと同じ。
直接呼び出しは、`main`（`@@CODEL`）→ 関数は `CALL !!addr20`（`FC lo hi pg`）、ランタイムライブラリ（`@@LCODE`、64KB 以内）は `CALL !addr16`（`FD lo hi`。例 004C4 `FD7B01 CALL !017BH`）。どちらも戻り番地 4 バイト。

### (f) スタートアップ `_@cstart`（0x0CE）から `_main`（0x637）まで【確定】
```
000CE 61CF      SEL RB0
000D0 5100      MOV A,#00H           ; ミラー設定 MAA=0（PMC.0）
000D2 718C      MOV1 CY,A.0
000D4 7109FE    MOV1 PMC.0,CY
000D7 CBF8D8FE  MOVW SP,#0FED8H      ; SP = _@STBEG（0xFFED8 の下位 16 ビット）
000DB FC3E0800  CALL !!0083EH        ; _hdwinit
000DF 3614C1    MOVW HL,#0C114H      ; _@STEND
000E2 FC3F0800  CALL !!0083FH        ; _stkinit: HL..SP のスタック領域を 0 クリア
000E6 53C0 …    saddr（0xFFE20〜）を 0 クリア
000F0 4100 …    ES=0 で ROM の @@R_INIT（0x0002〜0x00AD、172 B）を @@INIT（0xFC068）へコピー（ループ 000FA–00103）
00105 / 0010D   @@R_INIS・@@RLINIT 等のコピー（サイズ 0 なので空ループ）
0011D 36 00 BF  @@DATA（0xFBF00〜0xFC067）を 0 クリア（ループ 00125–0012A）
00162 …         ランタイムの変数の初期化（_@SEED=1、_@BRKADR=_@MEMTOP など）
00170 FC370600  CALL !!00637H        ; _main
00174 F6        CLRW AX
00175 FC830800  CALL !!00883H        ; _exit
00179 EFFE      BR $00179H
```
`_hdwinit`（0x083E）は **`D7 RET` だけの空関数**（利用者が `hdwinit` を書かなければライブラリの空実装が入る）。**スタートアップ自身には周辺機能の待ちループは無い**。
公開サンプルで見つかった発振待ち（`R_CGC_Create` の OSTC 待ち）は、利用者の `hdwinit` → `R_Systeminit` 側にあったもの。
実行: リセットから `main` の入口まで **14458 命令（18553 クロック）**、待ちなし（`_stkinit` と RAM クリアで大半を使う）。入口の `SP=0xFED4`（0xFED8 − 戻り番地 4）、`ES=0`、`CS=0`。`main` 終了後 `_exit` から `BR $` の自己ループ。

### 確定した点と、なお未確認の点
確定（probe の lmf で、機械語と実行の両方から）: (a) 4 バイト第 1 引数は AX=下位/BC=上位（far ポインタも同じ並び、C=ページ）、(b) スタック引数は右から左、枠は 1B→2、2B→2、4B→4（下位ワードが低アドレス、戻り番地 4 バイトの次から）、(c) 戻り値: 1B→C、2B→BC、4B・float→BC(下位)/DE(上位)、6 バイト構造体→static 領域のアドレスを BC、(d) 呼び出し側がスタック引数を解放、HL は呼ばれた側が保存、(e) 関数ポインタはミディアムで 4 バイト `{offset, CS ページ, 0}`、`MOV CS,A; CALL DE`、(f) スタートアップに待ちなし（`hdwinit` は空）。
未確認（probe に例が無い）: 3 バイトの引数、4 バイト以下の構造体・共用体の引数と戻り値、far ポインタ・関数ポインタを**戻り値**にした場合、`double` を使う場合（4 バイトなので float と同じと推測）、スモール/ラージモデルのコード・呼び出し、可変個引数、`-qr` のレジスタ変数（`_@KREG`）、割り込み関数の入口・出口、`__callt`、`ES` の保存の規約、乗除算ランタイム（`@@lmul` など）が使う `_@RTARGn` の受け渡し（probe は加算・減算・float 乗算のみ）。
