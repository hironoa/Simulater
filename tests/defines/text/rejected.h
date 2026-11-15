#define SELF SELF
#define PING PONG
#define PONG PING
#define FN(x) x
#define BROKEN (1+2
#define EXTRA 1 2
typedef unsigned char Self[SELF];
typedef unsigned char Cycle[PING];
typedef unsigned char Func[FN(2)];
typedef unsigned char Broken[BROKEN];
typedef unsigned char Extra[EXTRA];
#define BAD_SELF (*(Self*)0xF900)
#define BAD_CYCLE (*(Cycle*)0xF900)
#define BAD_FUNC (*(Func*)0xF900)
#define BAD_BROKEN (*(Broken*)0xF900)
#define BAD_EXTRA (*(Extra*)0xF900)
#define BAD_ADDR (*(unsigned char*)EXTRA)
#define BAD_PAREN (*(unsigned char*)0xF900))
