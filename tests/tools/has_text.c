/* has_text.c - findstr の代わりに、行の中の文字列をバイト単位で探す（Windows のテスト用）
 *
 *   has_text [/l] [/i] [/b] [/x] /c:文字列 [/c:文字列 ...] [ファイル]
 *
 * findstr と同じ書き方のうち、テストで使うものだけを受け付ける。
 *   /c:文字列  探す文字列（正規表現ではない）。複数あれば、どれかを含む行が一致
 *   /b         行の先頭にあるものだけ
 *   /x         行の全体が一致するものだけ
 *   /i         ASCII の大文字と小文字を区別しない
 *   /l         何もしない（findstr と書き方をそろえるため）
 * ファイルを省くと標準入力を読む。一致した行を標準出力へそのまま書き、1 行でも一致すれば 0、なければ 1、
 * 使い方の誤りやファイルを開けないときは 2 を返す。
 *
 * findstr を使わない理由: 日本語の Windows（システムのコードページが 932）の findstr は、ファイルを
 * Shift_JIS として読む。sim の出力は UTF-8 なので、たとえば「、」（E3 80 81）の最後の 0x81 が 2 バイト
 * 文字の 1 バイト目とみなされ、直後の ASCII の 1 文字が読み飛ばされる。「、e_machine 197」から
 * "e_machine 197" を探しても見つからない。ここではバイト列のまま比べるので、コードページに左右されない。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* MinGW-w64 のランタイムに、引数の * ? [ をファイル名として展開させない（探す文字列に [ がある） */
int _dowildcard = 0;

#define MAX_PAT 16

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

static int same(const char *a, const char *b, size_t n, int icase)
{
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (icase) { x = lower(x); y = lower(y); }
        if (x != y) return 0;
    }
    return 1;
}

static int line_matches(const char *line, size_t len, const char *pat, int icase, int begin, int whole)
{
    size_t n = strlen(pat);
    if (whole) return len == n && same(line, pat, n, icase);
    if (n > len) return 0;
    if (begin) return same(line, pat, n, icase);
    for (size_t i = 0; i + n <= len; i++)
        if (same(line + i, pat, n, icase)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *pats[MAX_PAT];
    int npat = 0, icase = 0, begin = 0, whole = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '/' && (a[1] == 'c' || a[1] == 'C') && a[2] == ':') {
            if (npat == MAX_PAT) { fprintf(stderr, "has_text: too many /c: patterns\n"); return 2; }
            pats[npat++] = a + 3;
        } else if (a[0] == '/' && a[1] && !a[2]) {
            switch (lower((unsigned char)a[1])) {
            case 'l': break;
            case 'i': icase = 1; break;
            case 'b': begin = 1; break;
            case 'x': whole = 1; break;
            default: fprintf(stderr, "has_text: unknown option %s\n", a); return 2;
            }
        } else if (!path) {
            path = a;
        } else {
            fprintf(stderr, "has_text: more than one file\n");
            return 2;
        }
    }
    if (npat == 0) { fprintf(stderr, "has_text: no /c: pattern\n"); return 2; }

    FILE *in = stdin;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    if (!path) _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (path) {
        in = fopen(path, "rb");
        if (!in) { fprintf(stderr, "has_text: cannot open %s\n", path); return 2; }
    }

    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) return 2;
    int found = 0, c;
    for (;;) {
        c = fgetc(in);
        if (c != EOF && c != '\n') {
            if (len + 1 >= cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) return 2;
                buf = nb;
            }
            buf[len++] = (char)c;
            continue;
        }
        if (c == EOF && len == 0) break;
        size_t text = len;                      /* 比べるのは行末の CR を除いた部分 */
        if (text > 0 && buf[text - 1] == '\r') text--;
        for (int k = 0; k < npat; k++) {
            if (line_matches(buf, text, pats[k], icase, begin, whole)) {
                found = 1;
                fwrite(buf, 1, len, stdout);
                if (c == '\n') fputc('\n', stdout);
                break;
            }
        }
        len = 0;
        if (c == EOF) break;
    }
    if (path) fclose(in);
    free(buf);
    return found ? 0 : 1;
}
