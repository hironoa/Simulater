typedef unsigned char Bytes[4];
typedef unsigned long long U64;
#define BYTES (*(Bytes*)0xF910)
#define WIDE (*(U64*)0xF900)
typedef struct { Bytes bytes; unsigned char tail; } ArrayRecord;
#define ARRAY_RECORD (*(ArrayRecord*)0xF920)
typedef unsigned char Matrix[2][3];
#define MATRIX (*(Matrix*)0xF930)
typedef signed long long S64;
#define SIGNEDWIDE (*(S64*)0xF940)
