/* プロジェクトに登録されておらず、どこからもインクルードされないファイル（フォルダの下すべてを読む方式でだけ読まれる） */
#define SCOPE_MODE (*(volatile unsigned char *)0xF9C2)
#define SCOPE_STRAY (*(volatile unsigned char *)0xF9C3)
