/* 変数の型と名前による読み書きの試験。 */
#include "types.h"

u1 g_byte = 1;
u2 g_word = 2;
s2 g_signed = -3;
u4 g_long = 4;
static u2 s_value = 5;
struct inner { u2 value; };
struct record { u1 tag; struct inner nested; } g_record = {1, {2}};
u2 g_array[4] = {1, 2, 3, 4};
u2 g_matrix[2][3] = {{1, 2, 3}, {4, 5, 6}};
struct flags { u1 ready : 1; u1 mode : 3; } g_flags = {0, 1};
enum state { IDLE = 0, ACTIVE = 2 } g_state = IDLE;
u2 *g_pointer = &g_word;
const u2 g_table[3] = {10, 20, 30};

u4 vars_scalars(u1 delta)
{
    g_byte = (u1)(g_byte + delta);
    g_word++;
    g_signed--;
    g_long += 2;
    return (u4)(g_byte + g_word + g_signed + g_long);
}

static u2 vars_static_add(u2 delta)
{
    s_value = (u2)(s_value + delta);
    return s_value;
}

u2 vars_static(u2 delta) { return vars_static_add(delta); }
u2 vars_record(void) { return (u2)(g_record.tag + g_record.nested.value); }
u2 vars_arrays(void)
{
    u2 i, j, sum = 0;
    for (i = 0; i < 4; i++) sum = (u2)(sum + g_array[i]);
    for (i = 0; i < 2; i++)
        for (j = 0; j < 3; j++) sum = (u2)(sum + g_matrix[i][j]);
    g_array[0]++;
    g_matrix[0][0]++;
    return sum;
}
u1 vars_flags(void)
{
    g_flags.ready = 1;
    return (u1)(g_flags.ready + g_flags.mode);
}
u2 vars_enum(void) { g_state = ACTIVE; return (u2)g_state; }
u2 vars_counter(void)
{
    static u2 count = 7;
    return ++count;
}
u2 vars_pointer(u2 value) { *g_pointer = value; return *g_pointer; }
u2 vars_table(u1 index) { return g_table[index]; }
