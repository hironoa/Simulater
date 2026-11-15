/* cond_types.h - 条件の中にある typedef。品種 2 だけ 16 ビット */
#if !defined(COND_TYPES_H)
#define COND_TYPES_H

#if PRODUCT == 2
typedef unsigned short cond_reg_t;
typedef struct {
    unsigned char lo;
    unsigned char hi;
} cond_pair_t;
#else
typedef unsigned char cond_reg_t;
typedef struct {
    unsigned char lo;
} cond_pair_t;
#endif

#endif
