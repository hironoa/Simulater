/* defines_scope = mtpj のテスト: どの品種でもビルドするファイル */
#include "product.h"        /* インクルード・パスから（ビルド・モードで prodA / prodB が変わる） */
#include <reg.h>            /* プロジェクトに登録していないヘッダ（インクルード・パスの inc） */
#ifdef USE_OLD
#include "../unused/old.h"  /* 無効な枝。たどって見つけても読まない */
#endif
#define SCOPE_APP (*(volatile unsigned char *)0xF9A0)
/* 構造体のメンバ。配置指定（#pragma pack）のあるファイルを読むと解決をやめる。old.h が無効な枝にある間は解決できる */
typedef struct { unsigned char a; unsigned char b; } scope_pk_t;
#define SCOPE_PK (((volatile scope_pk_t *)0xF9A6)->b)
