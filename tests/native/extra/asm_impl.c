/* 自動で作る代わりより、C の定義が優先されることを調べる */
unsigned short asm_get_value(unsigned char n) { return (unsigned short)(n * 2); }
