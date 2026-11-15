// ƒ\
#define SJIS_SLASH (*(unsigned char*)0xF934)
/* •\*/
#define SJIS_BLOCK (*(unsigned char*)0xF935)
const char *s="”\";
#define SJIS_STRING (*(unsigned char*)0xF936)
int c='ƒ\';
#define SJIS_CHAR (*(unsigned char*)0xF937)
#if 1 // ƒ\
#define SJIS_IF (*(unsigned char*)0xF938)
#endif
typedef struct { /* •\*/ unsigned char a; } SjisType;
#define SJIS_TYPE (*(SjisType*)0xF939)
// ‚
#define SJIS_INVALID_NL (*(unsigned char*)0xF93A)
/* ‚*/
#define SJIS_INVALID_STAR (*(unsigned char*)0xF93B)
// ‚/
#define SJIS_INVALID_SLASH (*(unsigned char*)0xF93C)
const char *q="‚";
#define SJIS_INVALID_QUOTE (*(unsigned char*)0xF93D)
// ±
#define SJIS_KANA (*(unsigned char*)0xF93E)
// ‚