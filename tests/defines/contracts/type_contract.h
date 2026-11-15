typedef unsigned char Reg;
struct Reg { unsigned short value; };
#define REG_B (*(struct Reg*)0xF910)
#define REG_VALUE (((struct Reg*)0xF910)->value)
#define REG_BYTE (*(Reg*)0xF912)
typedef struct Same { unsigned short value; } Same;
#define SAME (*(Same*)0xF914)
typedef struct Forward Forward;
struct Forward { unsigned short value; };
#define FORWARD (*(Forward*)0xF916)
typedef struct { unsigned short value; } Anonymous;
#define ANONYMOUS (*(Anonymous*)0xF918)
struct Incomplete;
#define INCOMPLETE (*(struct Incomplete*)0xF91A)
union OnlyUnion { unsigned short value; };
#define WRONG_KIND (*(struct OnlyUnion*)0xF91C)
#define RIGHT_UNION (*(union OnlyUnion*)0xF91C)
struct Clash { unsigned char value; };
union Clash { unsigned short value; };
#define CLASH_STRUCT (*(struct Clash*)0xF91E)
#define CLASH_UNION (*(union Clash*)0xF91E)
#define DEBUG_TAG (*(struct eng_flag*)0xF920)
#define DEBUG_WRONG (*(union eng_flag*)0xF920)
typedef const unsigned char ConstByte;
typedef ConstByte ConstArray[4];
typedef struct { ConstByte field; unsigned char mutable; unsigned char data[2]; } Mixed;
#define CONST_ARRAY (*(ConstArray*)0xF900)
#define MIXED (*(Mixed*)0xF904)
#define DIRECT_FIELD (((Mixed*)0xF904)->field)
#define CONST_STRUCT (*(const Mixed*)0xF908)
#define DIRECT_NEST (((const Mixed*)0xF908)->data[0])
#define DEBUG_UNQUAL (*(struct DebugConst*)0xF950)
