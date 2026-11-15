/*
 * mock_backend.c - 検証用のモック・バックエンド
 *
 * 64KB のメモリ配列と手書きのシンボル表で、RL78 らしいターゲットを模擬する。
 *   - int 16 ビット、ポインタ 16 ビット、リトルエンディアン
 *   - SFR は 0xFF00〜0xFFFF（P1、P1_bit、P2、P2_bit、ADCR）
 *   - $ptr 用のヒープは 0xE000〜0xEFFF
 * 対象関数は、このファイルの中の C 関数がターゲットのメモリを読み書きして模擬する
 * （架空のエンジン制御: 回転数と水温から状態・判定フラグ・ギア段を決める）。
 */
#define _CRT_SECURE_NO_WARNINGS
#include "sim_backend.h"
#include "sim_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE 0x10000u
#define HEAP_BEGIN 0xE000u
#define HEAP_END 0xF000u

typedef struct {
    uint8_t mem[MEM_SIZE];
    uint8_t init[MEM_SIZE];
    uint32_t heap;
} mock_t;

/* ---------------------------------------------------------------- 型 */

static const sim_type t_u8 = {SIM_T_UINT, "uint8", 1, NULL, 0, NULL, 0,0};
static const sim_type t_char = {SIM_T_UINT, "char", 1, NULL, 0, NULL, 0,0};
static const sim_type t_u16 = {SIM_T_UINT, "uint16", 2, NULL, 0, NULL, 0,0};
static const sim_type t_s16 = {SIM_T_INT, "sint16", 2, NULL, 0, NULL, 0,0};
static const sim_type t_int = {SIM_T_INT, "int", 2, NULL, 0, NULL, 0,0};
static const sim_type t_u32 = {SIM_T_UINT, "uint32", 4, NULL, 0, NULL, 0,0};
static const sim_type t_float = {SIM_T_FLOAT, "float", 4, NULL, 0, NULL, 0,0};
static const sim_type t_enum_state = {SIM_T_INT, "eng_state_t", 2, NULL, 0, NULL, 0,0};

static const sim_member m_bitf[] = {
    {"no0", 0, 0, 1, &t_u8}, {"no1", 0, 1, 1, &t_u8}, {"no2", 0, 2, 1, &t_u8}, {"no3", 0, 3, 1, &t_u8},
    {"no4", 0, 4, 1, &t_u8}, {"no5", 0, 5, 1, &t_u8}, {"no6", 0, 6, 1, &t_u8}, {"no7", 0, 7, 1, &t_u8},
};
static const sim_type t_bitf = {SIM_T_STRUCT, "__bitf_T", 1, NULL, 0, m_bitf, 8,0};

static const sim_member m_flag[] = {
    {"warm", 0, 0, 1, &t_u8}, {"over_rev", 0, 1, 1, &t_u8}, {"overheat", 0, 2, 1, &t_u8},
    {"fan_req", 0, 3, 1, &t_u8}, {"reserved", 0, 4, 4, &t_u8},
};
static const sim_type t_flag = {SIM_T_STRUCT, "struct eng_flag", 1, NULL, 0, m_flag, 5,0};

/* typedef struct { uint16 rpm; sint16 water_temp; eng_state_t state; uint8 gear; struct eng_flag flag; } eng_status_t; */
static const sim_member m_eng[] = {
    {"rpm", 0, -1, 0, &t_u16}, {"water_temp", 2, -1, 0, &t_s16}, {"state", 4, -1, 0, &t_enum_state},
    {"gear", 6, -1, 0, &t_u8}, {"flag", 7, -1, 0, &t_flag},
};
static const sim_type t_eng = {SIM_T_STRUCT, "eng_status_t", 8, NULL, 0, m_eng, 5,0};
static const sim_type t_eng_ptr = {SIM_T_PTR, "eng_status_t *", 2, &t_eng, 0, NULL, 0,0};
static const sim_type t_u16_ptr = {SIM_T_PTR, "uint16 *", 2, &t_u16, 0, NULL, 0,0};
static const sim_type t_char_ptr = {SIM_T_PTR, "char *", 2, &t_char, 0, NULL, 0,0};

static const sim_type t_u16x5 = {SIM_T_ARRAY, "uint16[5]", 10, &t_u16, 5, NULL, 0,0};
static const sim_type t_u16x4 = {SIM_T_ARRAY, "uint16[4]", 8, &t_u16, 4, NULL, 0,0};
static const sim_type t_u16x8 = {SIM_T_ARRAY, "uint16[8]", 16, &t_u16, 8, NULL, 0,0};
static const sim_type t_charx16 = {SIM_T_ARRAY, "char[16]", 16, &t_char, 16, NULL, 0,0};
static const sim_type t_u8x3 = {SIM_T_ARRAY, "uint8[3]", 3, &t_u8, 3, NULL, 0,0};
static const sim_type t_u8x2x3 = {SIM_T_ARRAY, "uint8[2][3]", 6, &t_u8x3, 2, NULL, 0,0};

/* 匿名共用体を含む構造体 */
static const sim_member m_bytes[] = {{"lo", 0, -1, 0, &t_u8}, {"hi", 1, -1, 0, &t_u8}};
static const sim_type t_bytes = {SIM_T_STRUCT, NULL, 2, NULL, 0, m_bytes, 2,0};
static const sim_member m_cal_u[] = {{"word", 0, -1, 0, &t_u16}, {"", 0, -1, 0, &t_bytes}};
static const sim_type t_cal_u = {SIM_T_UNION, NULL, 2, NULL, 0, m_cal_u, 2,0};
static const sim_member m_cal[] = {{"", 0, -1, 0, &t_cal_u}, {"offset", 2, -1, 0, &t_s16}};
static const sim_type t_cal = {SIM_T_STRUCT, "cal_t", 4, NULL, 0, m_cal, 2,0};

static const sim_member m_hist[] = {{"rpm", 0, -1, 0, &t_u16}, {"temp", 2, -1, 0, &t_s16}};
static const sim_type t_hist = {SIM_T_STRUCT, "hist_t", 4, NULL, 0, m_hist, 2,0};
static const sim_type t_histx3 = {SIM_T_ARRAY, "hist_t[3]", 12, &t_hist, 3, NULL, 0,0};

/* GUI 検索の境界: 匿名内の配列・ビット、構造体配列、深さと要素数の上限。 */
static const sim_type t_calx2 = {SIM_T_ARRAY, "cal_t[2]", 8, &t_cal, 2, NULL, 0,0};
static const sim_type t_cal_ptr = {SIM_T_PTR, "cal_t *", 2, &t_cal, 0, NULL, 0,0};
static const sim_member m_search_union[] = {{"bytes", 0, -1, 0, &t_u8x3}, {"", 0, -1, 0, &t_flag}};
static const sim_type t_search_union = {SIM_T_UNION, NULL, 3, NULL, 0, m_search_union, 2,0};
static const sim_member m_search[] = {{"", 0, -1, 0, &t_search_union}};
static const sim_type t_search = {SIM_T_STRUCT, "search_t", 3, NULL, 0, m_search, 1,0};
static const sim_type t_big = {SIM_T_ARRAY, "uint8[258]", 258, &t_u8, 258, NULL, 0,0};
#define DEEP(n, target) static const sim_type t_deep##n = {SIM_T_ARRAY, "deep" #n, 1, target, 1, NULL, 0,0}
DEEP(1, &t_u8);
DEEP(2, &t_deep1);
DEEP(3, &t_deep2);
DEEP(4, &t_deep3);
DEEP(5, &t_deep4);
DEEP(6, &t_deep5);
DEEP(7, &t_deep6);
DEEP(8, &t_deep7);
DEEP(9, &t_deep8);
DEEP(10, &t_deep9);
#undef DEEP

/* ---------------------------------------------------------------- 変数 */

enum {
    A_ENG = 0xFB00, A_GEAR_UP = 0xFB08, A_NAME = 0xFB12, A_GP_ENG = 0xFB22, A_GP_LOG = 0xFB24,
    A_DUTY = 0xFB26, A_MS_TICK = 0xFB2A, A_TIME_US = 0xFB2E, A_CYCLE_CNT = 0xFB32,
    A_ENG_S_CNT = 0xFB34, A_SEN_S_CNT = 0xFB35, A_CALL_CNT = 0xFB36, A_AMIN_ADC = 0xFB38,
    A_AMOUT_CH = 0xFB48, A_PSW_IE = 0xFB49, A_LOG_IDX = 0xFB4A, A_TM_SUB = 0xFB4B, A_LUT = 0xFB4C,
    A_MODE = 0xFB54, A_CAL = 0xFB56, A_HIST = 0xFB5A, A_MATRIX = 0xFB66, A_SEN_AVG = 0xFB6C,
    A_P1 = 0xFF01, A_P2 = 0xFF02, A_ADCR = 0xFF1E
};

static const sim_var g_vars[] = {
    {"g_eng", "src/engine.c", NULL, A_ENG, &t_eng, 1, 0},
    {"g_gear_up_rpm", "src/engine.c", NULL, A_GEAR_UP, &t_u16x5, 1, 0},
    {"g_eng_name", "src/engine.c", NULL, A_NAME, &t_charx16, 1, 0},
    {"gp_eng", "src/engine.c", NULL, A_GP_ENG, &t_eng_ptr, 1, 0},
    {"gp_log", "src/engine.c", NULL, A_GP_LOG, &t_u16_ptr, 1, 0},
    {"g_fan_duty", "src/engine.c", NULL, A_DUTY, &t_float, 1, 0},
    {"g_ms_tick", "src/timer.c", NULL, A_MS_TICK, &t_u32, 1, 0},
    {"TEST_time_us", "winams/TEST_drv.c", NULL, A_TIME_US, &t_u32, 1, 0},
    {"TEST_cycle_cnt", "winams/TEST_drv.c", NULL, A_CYCLE_CNT, &t_u16, 1, 0},
    {"s_cnt", "src/engine.c", NULL, A_ENG_S_CNT, &t_u8, 0, 0},
    {"s_cnt", "src/sensor.c", NULL, A_SEN_S_CNT, &t_u8, 0, 0},
    {"call_cnt", "src/engine.c", "eng_update", A_CALL_CNT, &t_u16, 0, 0},
    {"AMIN_adc", "winams/AMSTB_SrcFile.c", "AMSTB_adc_read", A_AMIN_ADC, &t_u16x8, 0, 0},
    {"AMOUT_ch", "winams/AMSTB_SrcFile.c", "AMSTB_adc_read", A_AMOUT_CH, &t_u8, 0, 0},
    {"sim_psw_ie", "runtime/sim_target_vars.c", NULL, A_PSW_IE, &t_u8, 1, 0},
    {"s_log_idx", "src/engine.c", NULL, A_LOG_IDX, &t_u8, 0, 0},
    {"s_sub", "src/timer.c", NULL, A_TM_SUB, &t_u8, 0, 0},
    {"g_lut", "src/engine.c", NULL, A_LUT, &t_u16x4, 1, 1},
    {"g_mode", "src/engine.c", NULL, A_MODE, &t_u8, 1, 0},
    {"g_cal", "src/sensor.c", NULL, A_CAL, &t_cal, 1, 0},
    {"g_cal_const", "src/sensor.c", NULL, A_CAL, &t_cal, 1, 1},
    {"g_cal_array", "test.c", NULL, 0xD200, &t_calx2, 1, 0},
    {"gp_cal", "test.c", NULL, 0xD300, &t_cal_ptr, 1, 0},
    {"g_search", "test.c", NULL, 0xD310, &t_search, 1, 0},
    {"g_big", "test.c", NULL, 0xD000, &t_big, 1, 0},
    {"g_deep", "test.c", NULL, 0xD320, &t_deep10, 1, 0},
    {"g_hist", "src/engine.c", NULL, A_HIST, &t_histx3, 1, 0},
    {"g_matrix", "src/engine.c", NULL, A_MATRIX, &t_u8x2x3, 1, 0},
    {"s_avg", "src/sensor.c", "sensor_filter", A_SEN_AVG, &t_s16, 0, 0},
    {"P1", "src/iodefine.h", NULL, A_P1, &t_u8, 1, 0},
    {"P1_bit", "src/iodefine.h", NULL, A_P1, &t_bitf, 1, 0},
    {"P2", "src/iodefine.h", NULL, A_P2, &t_u8, 1, 0},
    {"P2_bit", "src/iodefine.h", NULL, A_P2, &t_bitf, 1, 0},
    {"ADCR", "src/iodefine.h", NULL, A_ADCR, &t_u16, 1, 0},
};
#define NVARS (sizeof g_vars / sizeof g_vars[0])

/* ---------------------------------------------------------------- メモリ */

static uint8_t rd8(mock_t *m, uint32_t a) { return m->mem[a & 0xFFFF]; }
static void wr8(mock_t *m, uint32_t a, uint8_t v) { m->mem[a & 0xFFFF] = v; }
static uint16_t rd16(mock_t *m, uint32_t a) { return (uint16_t)(rd8(m, a) | (rd8(m, a + 1) << 8)); }
static void wr16(mock_t *m, uint32_t a, uint16_t v) { wr8(m, a, (uint8_t)v); wr8(m, a + 1, (uint8_t)(v >> 8)); }
static uint32_t rd32(mock_t *m, uint32_t a) { return (uint32_t)rd16(m, a) | ((uint32_t)rd16(m, a + 2) << 16); }
static void wr32(mock_t *m, uint32_t a, uint32_t v) { wr16(m, a, (uint16_t)v); wr16(m, a + 2, (uint16_t)(v >> 16)); }
static int rdbit(mock_t *m, uint32_t a, int bit) { return (rd8(m, a) >> bit) & 1; }
static void wrbit(mock_t *m, uint32_t a, int bit, int v)
{
    uint8_t x = rd8(m, a);
    x = (uint8_t)(v ? (x | (1u << bit)) : (x & ~(1u << bit)));
    wr8(m, a, x);
}
static void wrfloat(mock_t *m, uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(m, a, u); }

/* ---------------------------------------------------------------- 模擬する対象ソフト */

enum { ENG_STOP = 0, ENG_CRANK, ENG_IDLE, ENG_RUN, ENG_OVERHEAT };

/* AMSTB_SrcFile.c: uint16 AMSTB_adc_read(uint8 ch) { static uint16 AMIN_adc[8]; static uint8 AMOUT_ch; ... } */
static uint16_t f_adc_read(mock_t *m, uint8_t ch)
{
    wr8(m, A_AMOUT_CH, ch);
    return rd16(m, A_AMIN_ADC + 2u * (ch & 7u));
}

/* engine.c: uint8 eng_judge_gear(uint16 rpm, sint16 temp, uint8 cur_gear) */
static uint8_t f_judge_gear(mock_t *m, uint16_t rpm, int16_t temp, uint8_t cur)
{
    uint8_t g = cur;
    uint8_t maxg = temp < 40 ? 3 : 5;
    if (g == 0) g = 1;
    if (g < maxg && g < 5 && rpm >= rd16(m, A_GEAR_UP + 2u * g)) {
        g++;
    } else if (g > 1 && (uint16_t)(rpm + 500) < rd16(m, A_GEAR_UP + 2u * (g - 1u))) {
        /* int が 16 ビットなので rpm + 500 は 65536 で折り返す */
        g--;
    }
    if (g > maxg) g = maxg;
    return g;
}

/* engine.c: uint16 eng_sum(const uint16 *buf, uint8 n) */
static uint16_t f_sum(mock_t *m, uint16_t buf, uint8_t n)
{
    uint16_t s = 0;
    uint8_t i;
    for (i = 0; i < n; i++) s = (uint16_t)(s + rd16(m, (uint32_t)buf + 2u * i));
    return s;
}

/* engine.c: uint8 log_push(uint16 v) { if (gp_log) gp_log[s_log_idx++] = v; return s_log_idx; } */
static uint8_t f_log_push(mock_t *m, uint16_t v)
{
    uint16_t p = rd16(m, A_GP_LOG);
    uint8_t idx = rd8(m, A_LOG_IDX);
    if (p) {
        wr16(m, (uint32_t)p + 2u * idx, v);
        idx++;
        wr8(m, A_LOG_IDX, idx);
    }
    return idx;
}

/* engine.c: uint16 eng_name_len(const char *s) */
static uint16_t f_name_len(mock_t *m, uint16_t s)
{
    uint16_t n = 0;
    if (!s) return 0xFFFF;
    while (n < 0x1000 && rd8(m, (uint32_t)s + n)) n++;
    return n;
}

/* engine.c: float calc_duty(sint16 temp) */
static float f_calc_duty(mock_t *m, int16_t temp)
{
    float d = (float)(temp - 80) * 0.05f;
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    wrfloat(m, A_DUTY, d);
    return d;
}

/* engine.c: int mul16(int a, int b) -- int は 16 ビット */
static int16_t f_mul16(int16_t a, int16_t b) { return (int16_t)(uint16_t)((int32_t)a * (int32_t)b); }

/* sensor.c: sint16 sensor_filter(sint16 x) { static sint16 s_avg; s_avg = (s_avg * 3 + x) / 4; return s_avg; } */
static int16_t f_sensor_filter(mock_t *m, int16_t x)
{
    int16_t avg = (int16_t)rd16(m, A_SEN_AVG);
    avg = (int16_t)(((int32_t)avg * 3 + x) / 4);
    wr16(m, A_SEN_AVG, (uint16_t)avg);
    wr8(m, A_SEN_S_CNT, (uint8_t)(rd8(m, A_SEN_S_CNT) + 1));
    return avg;
}

/* timer.c: #pragma interrupt intr_tm00(vect=INTTM00) */
static void f_intr_tm00(mock_t *m)
{
    uint8_t sub;
    wr32(m, A_MS_TICK, rd32(m, A_MS_TICK) + 1);
    sub = (uint8_t)((rd8(m, A_TM_SUB) + 1) % 10);
    wr8(m, A_TM_SUB, sub);
    if (sub == 0) wrbit(m, A_P1, 7, !rdbit(m, A_P1, 7));   /* 10ms ごとに LED を反転 */
}

/* engine.c: void eng_update(void) -- 10ms 周期 */
static void f_eng_update(mock_t *m)
{
    static const uint32_t FLAG = A_ENG + 7;
    uint16_t rpm, cc;
    int16_t temp, state;
    uint8_t gear, cnt, fan;
    int ign;

    cc = (uint16_t)(rd16(m, A_CALL_CNT) + 1);
    wr16(m, A_CALL_CNT, cc);
    ign = rdbit(m, A_P2, 0);
    rpm = f_adc_read(m, 0);
    temp = (int16_t)(f_adc_read(m, 1) - 40);
    (void)f_sensor_filter(m, temp);

    wr8(m, A_PSW_IE, 0);                   /* __DI() */
    wr16(m, A_ENG + 0, rpm);
    wr16(m, A_ENG + 2, (uint16_t)temp);
    wr8(m, A_PSW_IE, 1);                   /* __EI() */

    state = (int16_t)rd16(m, A_ENG + 4);
    if (!ign) {
        state = ENG_STOP;
    } else {
        switch (state) {
        case ENG_STOP: if (rpm > 0) state = ENG_CRANK; break;
        case ENG_CRANK: if (rpm >= 500) state = ENG_IDLE; else if (rpm == 0) state = ENG_STOP; break;
        case ENG_IDLE: if (rpm >= 1200) state = ENG_RUN; else if (rpm == 0) state = ENG_STOP; break;
        case ENG_RUN: if (rpm < 1000) state = ENG_IDLE; break;
        case ENG_OVERHEAT: if (temp < 100) state = ENG_IDLE; break;
        default: state = ENG_STOP; break;
        }
    }
    cnt = rd8(m, A_ENG_S_CNT);
    if ((state == ENG_IDLE || state == ENG_RUN) && temp >= 110) {
        cnt++;
        if (cnt >= 3) { state = ENG_OVERHEAT; cnt = 0; }
    } else {
        cnt = 0;
    }
    wr8(m, A_ENG_S_CNT, cnt);
    wr16(m, A_ENG + 4, (uint16_t)state);

    wrbit(m, FLAG, 0, temp >= 60);
    wrbit(m, FLAG, 1, rpm >= 6500);
    wrbit(m, FLAG, 2, state == ENG_OVERHEAT);
    fan = (uint8_t)rdbit(m, FLAG, 3);
    if (temp >= 95) fan = 1;
    else if (temp < 90) fan = 0;
    wrbit(m, FLAG, 3, fan);

    gear = rd8(m, A_ENG + 6);
    gear = state == ENG_RUN ? f_judge_gear(m, rpm, temp, gear) : 0;
    wr8(m, A_ENG + 6, gear);

    wrbit(m, A_P1, 3, fan);
    wrbit(m, A_P1, 0, state == ENG_RUN);

    /* 履歴 g_hist[0..2] */
    {
        int i;
        for (i = 2; i > 0; i--) wr32(m, A_HIST + 4u * (uint32_t)i, rd32(m, A_HIST + 4u * (uint32_t)(i - 1)));
        wr16(m, A_HIST, rpm);
        wr16(m, A_HIST + 2, (uint16_t)temp);
    }
}

/* TEST_drv.c: 1 行 = 1 周期（10ms）。10us 刻みのインナーループから割り込みと周期処理を呼ぶ */
static void f_test_cycle(mock_t *m)
{
    int i;
    for (i = 0; i < 1000; i++) {
        wr32(m, A_TIME_US, rd32(m, A_TIME_US) + 10);
        if ((i % 100) == 99) f_intr_tm00(m);   /* 1ms タイマ割り込み */
        if (i == 999) f_eng_update(m);         /* 10ms 周期処理 */
    }
    wr16(m, A_CYCLE_CNT, (uint16_t)(rd16(m, A_CYCLE_CNT) + 1));
}

/* ---------------------------------------------------------------- 関数表 */

typedef uint64_t (*impl_fn)(mock_t *m, const int64_t *a);

static uint64_t i_judge(mock_t *m, const int64_t *a) { return f_judge_gear(m, (uint16_t)a[0], (int16_t)a[1], (uint8_t)a[2]); }
static uint64_t i_sum(mock_t *m, const int64_t *a) { return f_sum(m, (uint16_t)a[0], (uint8_t)a[1]); }
static uint64_t i_update(mock_t *m, const int64_t *a) { (void)a; f_eng_update(m); return 0; }
static uint64_t i_intr(mock_t *m, const int64_t *a) { (void)a; f_intr_tm00(m); return 0; }
static uint64_t i_cycle(mock_t *m, const int64_t *a) { (void)a; f_test_cycle(m); return 0; }
static uint64_t i_adc(mock_t *m, const int64_t *a) { return f_adc_read(m, (uint8_t)a[0]); }
static uint64_t i_log(mock_t *m, const int64_t *a) { return f_log_push(m, (uint16_t)a[0]); }
static uint64_t i_len(mock_t *m, const int64_t *a) { return f_name_len(m, (uint16_t)a[0]); }
static uint64_t i_duty(mock_t *m, const int64_t *a)
{
    float f = f_calc_duty(m, (int16_t)a[0]);
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static uint64_t i_mul(mock_t *m, const int64_t *a) { (void)m; return (uint16_t)f_mul16((int16_t)a[0], (int16_t)a[1]); }
static uint64_t i_filter(mock_t *m, const int64_t *a) { return (uint16_t)f_sensor_filter(m, (int16_t)a[0]); }
static uint64_t i_set_duty(mock_t *m, const int64_t *a) { (void)m; (void)a; return 0; }

static const char *const pn_judge[] = {"rpm", "temp", "cur_gear"};
static const sim_type *const pt_judge[] = {&t_u16, &t_s16, &t_u8};
static const char *const pn_sum[] = {"buf", "n"};
static const sim_type *const pt_sum[] = {&t_u16_ptr, &t_u8};
static const char *const pn_adc[] = {"ch"};
static const sim_type *const pt_adc[] = {&t_u8};
static const char *const pn_log[] = {"v"};
static const sim_type *const pt_log[] = {&t_u16};
static const char *const pn_len[] = {"s"};
static const sim_type *const pt_len[] = {&t_char_ptr};
static const char *const pn_duty[] = {"temp"};
static const sim_type *const pt_duty[] = {&t_s16};
static const char *const pn_mul[] = {"a", "b"};
static const sim_type *const pt_mul[] = {&t_int, &t_int};
static const char *const pn_filter[] = {"x"};
static const sim_type *const pt_filter[] = {&t_s16};
static const char *const pn_set_duty[] = {"d"};
static const sim_type *const pt_set_duty[] = {&t_float};

static const sim_func g_funcs[] = {
    {"eng_judge_gear", "src/engine.c", 0x1000, &t_u8, 3, pn_judge, pt_judge, 0, 1},
    {"eng_sum", "src/engine.c", 0x1040, &t_u16, 2, pn_sum, pt_sum, 0, 1},
    {"eng_update", "src/engine.c", 0x1080, NULL, 0, NULL, NULL, 0, 1},
    {"intr_tm00", "src/timer.c", 0x10C0, NULL, 0, NULL, NULL, 0, 1},
    {"TEST_cycle", "winams/TEST_drv.c", 0x1100, NULL, 0, NULL, NULL, 0, 1},
    {"AMSTB_adc_read", "winams/AMSTB_SrcFile.c", 0x1140, &t_u16, 1, pn_adc, pt_adc, 0, 1},
    {"log_push", "src/engine.c", 0x1180, &t_u8, 1, pn_log, pt_log, 0, 1},
    {"eng_name_len", "src/engine.c", 0x11C0, &t_u16, 1, pn_len, pt_len, 0, 1},
    {"calc_duty", "src/engine.c", 0x1200, &t_float, 1, pn_duty, pt_duty, 0, 1},
    {"mul16", "src/engine.c", 0x1240, &t_int, 2, pn_mul, pt_mul, 0, 1},
    {"sensor_filter", "src/sensor.c", 0x1280, &t_s16, 1, pn_filter, pt_filter, 0, 1},
    {"set_duty", "src/engine.c", 0x12C0, NULL, 1, pn_set_duty, pt_set_duty, 0, 1},
};
static const impl_fn g_impls[] = {
    i_judge, i_sum, i_update, i_intr, i_cycle, i_adc, i_log, i_len, i_duty, i_mul, i_filter, i_set_duty,
};
#define NFUNCS (sizeof g_funcs / sizeof g_funcs[0])

/* ---------------------------------------------------------------- バックエンド */

static size_t b_var_count(sim_backend *b) { (void)b; return NVARS; }
static const sim_var *b_var_at(sim_backend *b, size_t i) { (void)b; return i < NVARS ? &g_vars[i] : NULL; }
static size_t b_func_count(sim_backend *b) { (void)b; return NFUNCS; }
static const sim_func *b_func_at(sim_backend *b, size_t i) { (void)b; return i < NFUNCS ? &g_funcs[i] : NULL; }

static int range_ok(uint64_t addr, size_t n, char *err, size_t errlen)
{
    if (addr >= MEM_SIZE || n > MEM_SIZE || addr + n > MEM_SIZE) {
        snprintf(err, errlen, "アドレス 0x%llX（%zu バイト）はターゲットのアドレス空間（64KB）の外です", (unsigned long long)addr, n);
        return 0;
    }
    return 1;
}

static int b_read(sim_backend *b, uint64_t addr, void *buf, size_t n, char *err, size_t errlen)
{
    mock_t *m = b->impl;
    if (!range_ok(addr, n, err, errlen)) return -1;
    memcpy(buf, m->mem + addr, n);
    return 0;
}

static int b_write(sim_backend *b, uint64_t addr, const void *buf, size_t n, char *err, size_t errlen)
{
    mock_t *m = b->impl;
    if (!range_ok(addr, n, err, errlen)) return -1;
    memcpy(m->mem + addr, buf, n);
    return 0;
}

static int b_call(sim_backend *b, const sim_func *f, const int64_t *args, int nargs, uint64_t *ret_raw,
                  char *err, size_t errlen)
{
    mock_t *m = b->impl;
    size_t i;
    int64_t a[8];
    memset(a, 0, sizeof a);
    for (i = 0; i < (size_t)nargs && i < 8; i++) a[i] = args[i];
    for (i = 0; i < NFUNCS; i++) {
        if (&g_funcs[i] == f) {
            *ret_raw = g_impls[i](m, a);
            return 0;
        }
    }
    snprintf(err, errlen, "関数 '%s' はこのバックエンドにありません", f->name);
    return -1;
}

static int b_alloc(sim_backend *b, size_t size, uint64_t *addr, char *err, size_t errlen)
{
    mock_t *m = b->impl;
    uint32_t a = (m->heap + 1u) & ~1u;
    /* 引き算の前に終端を越えていないか確かめる（HEAP_END が奇数になっても範囲外を返さない） */
    if (a > HEAP_END || size > (size_t)(HEAP_END - a)) {
        snprintf(err, errlen, "ターゲットのヒープ（0x%X〜0x%X）が足りません（%zu バイト要求）", HEAP_BEGIN, HEAP_END - 1, size);
        return -1;
    }
    memset(m->mem + a, 0, size);
    m->heap = a + (uint32_t)size;
    *addr = a;
    return 0;
}

static int b_reset(sim_backend *b, char *err, size_t errlen)
{
    mock_t *m = b->impl;
    (void)err;
    (void)errlen;
    memcpy(m->mem, m->init, MEM_SIZE);
    m->heap = HEAP_BEGIN;
    return 0;
}

static void b_close(sim_backend *b)
{
    free(b->impl);
    free(b);
}

sim_backend *sim_backend_open(const sim_open_opts *o, char *err, size_t errlen)
{
    sim_backend *b = calloc(1, sizeof *b);
    mock_t *m = calloc(1, sizeof *m);
    static const uint16_t gear_up[5] = {0, 1500, 2500, 3500, 4500};
    static const uint16_t lut[4] = {100, 200, 400, 800};
    int i;
    sim_cfg cfg;
    int cfg_rc, init_mode;
    sim_cfg_init(&cfg);
    cfg_rc = sim_cfg_load(&cfg, o->cfg_path ? o->cfg_path : "sim.cfg", err, errlen);
    if (cfg_rc == -2 || (cfg_rc == -1 && o->cfg_path)) {
        if (cfg_rc == -1) snprintf(err, errlen, "設定ファイル %s を読めません", o->cfg_path);
        sim_cfg_free(&cfg); free(b); free(m); return NULL;
    }
    init_mode = sim_cfg_csv_init(&cfg, o->csv_init, err, errlen);
    sim_cfg_free(&cfg);
    if (init_mode < 0) { free(b); free(m); return NULL; }
    if (!b || !m) { snprintf(err, errlen, "メモリが足りません"); free(b); free(m); return NULL; }
    /* 初期値（C の初期化子にあたるもの） */
    for (i = 0; i < 5; i++) wr16(m, A_GEAR_UP + 2u * (uint32_t)i, gear_up[i]);
    for (i = 0; i < 4; i++) wr16(m, A_LUT + 2u * (uint32_t)i, lut[i]);
    memcpy(m->mem + A_NAME, "ENG-A", 5);
    wr16(m, A_GP_ENG, A_ENG);
    wr16(m, 0xD300, A_CAL);
    wr8(m, A_PSW_IE, 1);
    wr16(m, A_CAL, 0x1234);
    wr16(m, A_CAL + 2, (uint16_t)-5);
    memcpy(m->init, m->mem, MEM_SIZE);
    m->heap = HEAP_BEGIN;

    b->csv_init = init_mode;
    b->name = "mock";
    b->ptr_size = 2;
    b->big_endian = 0;
    b->default_func = "TEST_cycle";
    b->var_count = b_var_count;
    b->var_at = b_var_at;
    b->func_count = b_func_count;
    b->func_at = b_func_at;
    b->read = b_read;
    b->write = b_write;
    b->call = b_call;
    b->alloc = b_alloc;
    b->reset = b_reset;
    b->lookup_var = NULL;
    b->close = b_close;
    b->impl = m;
    return b;
}
