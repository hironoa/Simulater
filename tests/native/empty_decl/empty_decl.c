/* 外部の宣言を必要としないソース。 */
static const unsigned char table[] = { 3, 7 };

unsigned char empty_decl_lookup(void)
{
    return table[1];
}
