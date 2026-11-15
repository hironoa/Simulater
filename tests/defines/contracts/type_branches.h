#if SELECT_TYPES
struct Branch { unsigned char value; };
typedef unsigned char Branch;
typedef unsigned char Qualifier;
#else
struct Branch { unsigned short value; };
typedef unsigned short Branch;
typedef const unsigned char Qualifier;
#endif
#define BRANCH_TAG (*(struct Branch*)0xF930)
#define BRANCH_ALIAS (*(Branch*)0xF932)
#define BRANCH_QUAL (*(Qualifier*)0xF934)
