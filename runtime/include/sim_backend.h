/*
 * sim_backend.h - ターゲット・バックエンドのインターフェース
 *
 * CSV 実行、GUI サーバ、周期実行ループなどの共通部分は、このインターフェースだけを通して
 * ターゲット（対象ソフトの実行環境）に触る。ホストのポインタでターゲットのメモリを直接触らない。
 *
 * バックエンドが提供するもの:
 *   (1) シンボル表: 変数（名前、ファイル、関数内 static なら関数名、ターゲット上のアドレス、型）と
 *       関数（名前、アドレス、引数と戻り値の型）。名前の書式（file.c/func@var など）の解決は
 *       共通部分（sim_expr.c）がこの表を使って行う。lookup_var を実装すれば置き換えられる。
 *   (2) ターゲットのアドレス空間に対するバイト列の読み書き（read / write）。
 *       ポインタ幅とエンディアンは ptr_size / big_endian で申告する。
 *   (3) 関数の呼び出し（call）: 整数引数列を渡し、戻り値の生のビット列を受け取る。
 *   (4) ターゲット上のメモリ確保（alloc）: `$ptr` 用。reset で解放される。
 *   (5) 初期状態へのリセット（reset）。
 *   (6) シンボルの列挙（var_count/var_at、func_count/func_at）。検索は共通部分が行う。
 *
 * 型記述（sim_type）はバックエンドが所有し、バックエンドを閉じるまで有効であること。
 */
#ifndef SIM_BACKEND_H
#define SIM_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIM_T_VOID = 0,
    SIM_T_INT,      /* 符号付き整数（enum もここ） */
    SIM_T_UINT,     /* 符号なし整数 */
    SIM_T_BOOL,
    SIM_T_FLOAT,    /* size 4 = float、8 = double */
    SIM_T_PTR,
    SIM_T_ARRAY,
    SIM_T_STRUCT,
    SIM_T_UNION,
    SIM_T_FUNC,
    SIM_T_UNKNOWN
} sim_kind;

typedef struct sim_type sim_type;

typedef struct {
    const char *name;        /* 匿名の構造体/共用体メンバは "" */
    uint32_t offset;         /* 構造体先頭からのバイト位置 */
    int32_t bit_offset;      /* ビットフィールドなら構造体先頭からのビット位置、違えば -1 */
    uint32_t bit_size;       /* ビットフィールドの幅 */
    const sim_type *type;
} sim_member;

struct sim_type {
    sim_kind kind;
    const char *name;        /* 表示名（"unsigned char"、"struct eng_status" など） */
    uint32_t size;           /* バイト数 */
    const sim_type *target;  /* PTR の指す先（NULL = void）、ARRAY の要素 */
    uint32_t count;          /* ARRAY の要素数 */
    const sim_member *members;
    int nmembers;
};

typedef struct {
    const char *name;
    const char *file;        /* 定義しているソースファイル（パス可）。不明なら "" */
    const char *func;        /* 関数内 static なら関数名、違えば NULL */
    uint64_t addr;           /* ターゲット上のアドレス */
    const sim_type *type;
    int is_external;         /* グローバル（extern リンケージ）なら 1、ファイル内 static なら 0 */
    int is_const;            /* 書き込み不可 */
} sim_var;

typedef struct {
    const char *name;
    const char *file;
    uint64_t addr;
    const sim_type *ret;     /* NULL = void */
    int nparams;
    const char *const *param_names;
    const sim_type *const *param_types;
    int variadic;
    int is_external;
} sim_func;

typedef struct sim_backend sim_backend;

/* 実行の統計（exec_stats）。単位はターゲットの命令数とクロック数 */
typedef struct {
    uint64_t insns;          /* これまでに実行した命令数（スタートアップを含む） */
    uint64_t clocks;         /* これまでに実行したクロック数 */
    uint64_t calls;          /* call の回数 */
    uint64_t startup_insns;  /* スタートアップ（リセットから初期設定終了まで）の命令数 */
} sim_exec_stats;

/* バックエンドを開くときの指定（sim_backend_open） */
typedef struct {
    const char *exe_dir;     /* 実行ファイルのあるフォルダ */
    const char *abs_path;    /* ロードモジュール（.abs）。NULL なら設定ファイルの abs= */
    const char *cfg_path;    /* 設定ファイル。NULL なら .abs と同じフォルダの sim.cfg（あれば） */
    const char **defines;
    int ndefines;
    const char **cdefs;      /* --cdef 名前[=値]（defines の条件付きコンパイル用の定義マクロ） */
    int ncdefs;
    const char *mtpj_path;   /* --mtpj: CS+ のプロジェクトファイル。NULL なら設定ファイルの mtpj= */
    const char *csv_init;    /* CLI override: auto / once / every */
    int lmf_names;
    int trace_startup;       /* スタートアップを単命令で診断 */
    unsigned trace_head, trace_tail; /* 0 は既定（50 / 100） */
    int verbose;             /* 1 なら読み込み中の警告や詳細を標準エラーに出す */
} sim_open_opts;

struct sim_backend {
    const char *name;        /* "mock" など */
    int ptr_size;            /* ターゲットのポインタのバイト数 */
    int big_endian;          /* 0 = リトルエンディアン */
    const char *default_func;/* GUI の既定の周期関数（NULL 可） */

    /* (6) シンボルの列挙 */
    size_t (*var_count)(sim_backend *b);
    const sim_var *(*var_at)(sim_backend *b, size_t i);
    size_t (*func_count)(sim_backend *b);
    const sim_func *(*func_at)(sim_backend *b, size_t i);

    /* (2) メモリの読み書き。範囲外なら -1 を返し、err に理由を書く */
    int (*read)(sim_backend *b, uint64_t addr, void *buf, size_t n, char *err, size_t errlen);
    int (*write)(sim_backend *b, uint64_t addr, const void *buf, size_t n, char *err, size_t errlen);

    /* (3) 関数の呼び出し。args は引数の型に合わせて符号/ゼロ拡張済みの値。
     *     ret_raw には戻り値の生のビット列（下位 ret->size バイト。float は IEEE のビット列）。
     *     失敗なら -1 と err。 */
    int (*call)(sim_backend *b, const sim_func *f, const int64_t *args, int nargs,
                uint64_t *ret_raw, char *err, size_t errlen);

    /* (4) ターゲット上のメモリ確保（0 で初期化）。reset で解放される。 */
    int (*alloc)(sim_backend *b, size_t size, uint64_t *addr, char *err, size_t errlen);

    /* (5) 変数を初期状態に戻し、alloc した領域を解放する */
    int (*reset)(sim_backend *b, char *err, size_t errlen);

    /* (1) 省略可: 名前の解決を置き換える。file/func は NULL 可。見つからなければ NULL。
     *     NULL のままなら共通部分が var_at を使って解決する。 */
    const sim_var *(*lookup_var)(sim_backend *b, const char *file, const char *func, const char *name);

    void (*close)(sim_backend *b);

    /* 省略可: ポインタ型の値をターゲットのアドレスに換算する（RL78 の near ポインタは 0xF0000 を足す、
     *         far ポインタは下位 20 ビット）。NULL なら値そのものをアドレスとする */
    uint64_t (*ptr_to_addr)(sim_backend *b, const sim_type *ptr_type, uint64_t value);

    /* 省略可: 実行の統計。NULL なら命令数やクロック数は分からない */
    void (*exec_stats)(sim_backend *b, sim_exec_stats *st);
    /* CPU のクロック周波数（Hz）。0 より大きければ、結果 CSV の実行時間を call のクロック数から換算する。
     * 0 なら実時間を測る */
    double cpu_hz;

    /* 省略可: `sim info` 用の概要を out に書く */
    void (*print_info)(sim_backend *b, FILE *out);

    int csv_init;            /* 0 auto, 1 once, 2 every */
    void *impl;              /* バックエンド固有のデータ */
};

/* リンクされたバックエンドを開く。失敗なら NULL と err。 */
sim_backend *sim_backend_open(const sim_open_opts *o, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
