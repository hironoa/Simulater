/* defines_scope = mtpj のテスト: どの品種でもビルドするファイル */
#include "product.h"        /* インクルード・パスから（ビルド・モードで prodA / prodB が変わる） */
#include <reg.h>            /* プロジェクトに登録していないヘッダ（インクルード・パスの inc） */
#ifdef USE_OLD
#include "../unused/old.h"  /* 無効な枝。たどって見つけても読まない */
#endif
#define SCOPE_APP (*(volatile unsigned char *)0xF9A0)
