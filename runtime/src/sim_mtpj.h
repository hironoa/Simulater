/*
 * sim_mtpj.h - CS+ のプロジェクトファイル（.mtpj、XML）からコンパイル・オプションの定義マクロを読む
 *
 * 実例（tests/ca78k0r_probe/ca78k0r_probe.mtpj、CS+ 5.02 / CA78K0R V1.60）で確認したこと:
 *   - 文字コードは UTF-8（BOM なし）。
 *   - ビルド・モードは <BuildModeCount>、<BuildMode0>…（名前を UTF-16LE にして Base64 にしたもの）、
 *     <CurrentBuildMode>（そのままの文字列）にある。
 *   - コンパイラのオプションを持つ <Instance> に <MacroDefinitions-N>（定義マクロ）と
 *     <MacroUndefinitions-N>（定義解除マクロ）がある。N はビルド・モードの番号（0 始まり）。
 *     アセンブラのオプションを持つ <Instance> にも <MacroDefinitions-N> があるが、
 *     <MacroUndefinitions-N> はない。
 *   - 複数行の項目（StandardLibraryFileNames など）は、要素の文字列に改行区切りでそのまま入る。
 * 推定（実例では定義マクロが空 <MacroDefinitions-0 /> で、中身の入った例を見ていない）:
 *   - 定義マクロは 1 行に 1 個、「名前」または「名前=値」の形で、改行区切り・XML のエスケープで入る。
 * ファイルごとの個別コンパイル・オプションは読まない（あれば件数だけ数える）。
 *
 * 登録ファイルとインクルード・パス（defines_scope = mtpj が使う）。同じ実例で確認したこと:
 *   - プロジェクト・ツリーのファイルは <Instance Guid="…"> の中に <Type>File</Type> と
 *     <RelativePath>（mtpj のフォルダからの相対パス、区切りは \）を持つ。
 *   - ビルド・ツールの Instance に <SourceItemGuidN>（上の Guid）と <SourceItemTypeN> があり、
 *     別の Instance に <SourceItemN-BuildingTarget-M>（M はビルド・モードの番号、True / False）がある。
 *   - コンパイラのオプションを持つ Instance に <AdditionalIncludePaths-M> がある。
 * 推定（実例はファイルが 1 個で、インクルード・パスは空 <AdditionalIncludePaths-0 />）:
 *   - <SourceItemN-BuildingTarget-M> が False のファイルは、そのモードではビルドしない。
 *   - インクルード・パスは 1 行に 1 個、改行区切りで、mtpj のフォルダからの相対パスか絶対パス。
 *     %BuildModeName% と %ProjectName% は置き換える（ほかの項目での使われ方から）。
 */
#ifndef SIM_MTPJ_H
#define SIM_MTPJ_H

#include <stddef.h>

typedef struct {
    char **modes;           /* ビルド・モードの名前（UTF-8） */
    int nmodes;
    int mode;               /* 使ったビルド・モードの番号 */
    char *current;          /* <CurrentBuildMode>（なければ NULL） */
    char **defs;            /* 「名前」または「名前=値」 */
    int ndefs;
    char **undefs;
    int nundefs;
    char **bad;             /* 名前として読めなかった行 */
    int nbad;
    char *def_element;      /* 読んだ要素名（MacroDefinitions-0 など）。要素がなければ NULL */
    char *undef_element;
    int compiler_instance;  /* MacroUndefinitions を併せ持つ Instance（コンパイラ）を特定できたら 1 */
    int individual;         /* ファイルごとの個別オプションらしい要素（…MacroDefinitions…）の数 */
    char **files;           /* 登録ファイルのうち、選んだモードでビルド対象のもの（<RelativePath>、区切りは '/'） */
    int nfiles;
    int files_total;        /* 登録ファイルの総数（<Type>File</Type>） */
    int files_off;          /* ビルド対象外（BuildingTarget が False）で外した数 */
    char **incs;            /* インクルード・パス（区切りは '/'、%…% は置き換え済み） */
    int nincs;
    char *inc_element;      /* 読んだ要素名（AdditionalIncludePaths-0 など）。要素がなければ NULL */
} sim_mtpj;

/* mode = NULL なら <CurrentBuildMode>、それもなければ最初のモード。失敗なら -1 と err */
int sim_mtpj_load(sim_mtpj *m, const char *path, const char *mode, char *err, size_t errlen);
void sim_mtpj_free(sim_mtpj *m);

#endif
