/* cond_switch.h - 別のヘッダで定義するスイッチ（cond_product.h が #include して参照する） */
#ifndef COND_SWITCH_H
#define COND_SWITCH_H

#define COND_BOARD 3
#define COND_FUNC(x) (x ## 0)

#ifdef SIM
#define COND_HAS_SIM 1
#endif

#endif /* COND_SWITCH_H */
