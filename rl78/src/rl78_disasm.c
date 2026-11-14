/*
 * rl78_disasm.c - RL78 disassembler (instruction maps of R01US0015 5.7).
 *
 * Template operand codes (consume instruction bytes in order):
 *   $b #byte   $w #word   $s saddr   $f sfr    $F sfrp   $a !addr16
 *   $A !!addr20  $r $addr20 (rel8)   $R $!addr20 (rel16)
 *   $o byte offset   $W word base   %  position of "ES:" when prefixed
 */
#include <stdio.h>
#include <string.h>

#include "rl78_internal.h"

static const char *const map1[256] = {
    /* 0x */ "NOP", "ADDW AX, AX", "ADDW AX, %$a", "ADDW AX, BC", "ADDW AX, $w", "ADDW AX, DE",
    "ADDW AX, $s", "ADDW AX, HL", "XCH A, X", "MOV A, %$W[B]", "ADD $s, $b", "ADD A, $s",
    "ADD A, $b", "ADD A, %[HL]", "ADD A, %[HL+$o]", "ADD A, %$a",
    /* 1x */ "ADDW SP, $b", NULL, "MOVW BC, AX", "MOVW AX, BC", "MOVW DE, AX", "MOVW AX, DE",
    "MOVW HL, AX", "MOVW AX, HL", "MOV %$W[B], A", "MOV %$W[B], $b", "ADDC $s, $b",
    "ADDC A, $s", "ADDC A, $b", "ADDC A, %[HL]", "ADDC A, %[HL+$o]", "ADDC A, %$a",
    /* 2x */ "SUBW SP, $b", NULL, "SUBW AX, %$a", "SUBW AX, BC", "SUBW AX, $w", "SUBW AX, DE",
    "SUBW AX, $s", "SUBW AX, HL", "MOV %$W[C], A", "MOV A, %$W[C]", "SUB $s, $b", "SUB A, $s",
    "SUB A, $b", "SUB A, %[HL]", "SUB A, %[HL+$o]", "SUB A, %$a",
    /* 3x */ "MOVW AX, $w", NULL, "MOVW BC, $w", "XCHW AX, BC", "MOVW DE, $w", "XCHW AX, DE",
    "MOVW HL, $w", "XCHW AX, HL", "MOV %$W[C], $b", "MOV %$W[BC], $b", "SUBC $s, $b",
    "SUBC A, $s", "SUBC A, $b", "SUBC A, %[HL]", "SUBC A, %[HL+$o]", "SUBC A, %$a",
    /* 4x */ "CMP %$a, $b", "MOV ES, $b", "CMPW AX, %$a", "CMPW AX, BC", "CMPW AX, $w",
    "CMPW AX, DE", "CMPW AX, $s", "CMPW AX, HL", "MOV %$W[BC], A", "MOV A, %$W[BC]",
    "CMP $s, $b", "CMP A, $s", "CMP A, $b", "CMP A, %[HL]", "CMP A, %[HL+$o]", "CMP A, %$a",
    /* 5x */ "MOV X, $b", "MOV A, $b", "MOV C, $b", "MOV B, $b", "MOV E, $b", "MOV D, $b",
    "MOV L, $b", "MOV H, $b", "MOVW %$W[B], AX", "MOVW AX, %$W[B]", "AND $s, $b", "AND A, $s",
    "AND A, $b", "AND A, %[HL]", "AND A, %[HL+$o]", "AND A, %$a",
    /* 6x */ "MOV A, X", NULL, "MOV A, C", "MOV A, B", "MOV A, E", "MOV A, D", "MOV A, L",
    "MOV A, H", "MOVW %$W[C], AX", "MOVW AX, %$W[C]", "OR $s, $b", "OR A, $s", "OR A, $b",
    "OR A, %[HL]", "OR A, %[HL+$o]", "OR A, %$a",
    /* 7x */ "MOV X, A", NULL, "MOV C, A", "MOV B, A", "MOV E, A", "MOV D, A", "MOV L, A",
    "MOV H, A", "MOVW %$W[BC], AX", "MOVW AX, %$W[BC]", "XOR $s, $b", "XOR A, $s",
    "XOR A, $b", "XOR A, %[HL]", "XOR A, %[HL+$o]", "XOR A, %$a",
    /* 8x */ "INC X", "INC A", "INC C", "INC B", "INC E", "INC D", "INC L", "INC H",
    "MOV A, [SP+$o]", "MOV A, %[DE]", "MOV A, %[DE+$o]", "MOV A, %[HL]", "MOV A, %[HL+$o]",
    "MOV A, $s", "MOV A, $f", "MOV A, %$a",
    /* 9x */ "DEC X", "DEC A", "DEC C", "DEC B", "DEC E", "DEC D", "DEC L", "DEC H",
    "MOV [SP+$o], A", "MOV %[DE], A", "MOV %[DE+$o], A", "MOV %[HL], A", "MOV %[HL+$o], A",
    "MOV $s, A", "MOV $f, A", "MOV %$a, A",
    /* Ax */ "INC %$a", "INCW AX", "INCW %$a", "INCW BC", "INC $s", "INCW DE", "INCW $s",
    "INCW HL", "MOVW AX, [SP+$o]", "MOVW AX, %[DE]", "MOVW AX, %[DE+$o]", "MOVW AX, %[HL]",
    "MOVW AX, %[HL+$o]", "MOVW AX, $s", "MOVW AX, $F", "MOVW AX, %$a",
    /* Bx */ "DEC %$a", "DECW AX", "DECW %$a", "DECW BC", "DEC $s", "DECW DE", "DECW $s",
    "DECW HL", "MOVW [SP+$o], AX", "MOVW %[DE], AX", "MOVW %[DE+$o], AX", "MOVW %[HL], AX",
    "MOVW %[HL+$o], AX", "MOVW $s, AX", "MOVW $F, AX", "MOVW %$a, AX",
    /* Cx */ "POP AX", "PUSH AX", "POP BC", "PUSH BC", "POP DE", "PUSH DE", "POP HL", "PUSH HL",
    "MOV [SP+$o], $b", "MOVW $s, $w", "MOV %[DE+$o], $b", "MOVW $F, $w", "MOV %[HL+$o], $b",
    "MOV $s, $b", "MOV $f, $b", "MOV %$a, $b",
    /* Dx */ "CMP0 X", "CMP0 A", "CMP0 C", "CMP0 B", "CMP0 $s", "CMP0 %$a", "MULU X", "RET",
    "MOV X, $s", "MOV X, %$a", "MOVW BC, $s", "MOVW BC, %$a", "BC $r", "BZ $r", "BNC $r",
    "BNZ $r",
    /* Ex */ "ONEB X", "ONEB A", "ONEB C", "ONEB B", "ONEB $s", "ONEB %$a", "ONEW AX", "ONEW BC",
    "MOV B, $s", "MOV B, %$a", "MOVW DE, $s", "MOVW DE, %$a", "BR $A", "BR $a", "BR $R",
    "BR $r",
    /* Fx */ "CLRB X", "CLRB A", "CLRB C", "CLRB B", "CLRB $s", "CLRB %$a", "CLRW AX", "CLRW BC",
    "MOV C, $s", "MOV C, %$a", "MOVW HL, $s", "MOVW HL, %$a", "CALL $A", "CALL $a", "CALL $R",
    NULL,
};

static const char *const alu_names[8] = {"ADD", "ADDC", "SUB", "SUBC", "CMP", "AND", "OR", "XOR"};
static const char *const reg8[8] = {"X", "A", "C", "B", "E", "D", "L", "H"};

/* template for the 2nd map (61 xx); buf receives generated text */
static const char *map2(unsigned o, char *buf, size_t n)
{
    unsigned lo = o & 0xFu, hi = o >> 4;
    if (hi <= 7) {
        if (lo < 8) { snprintf(buf, n, "%s %s, A", alu_names[hi], reg8[lo]); return buf; }
        if (lo != 9) { snprintf(buf, n, "%s A, %s", alu_names[hi], reg8[lo & 7u]); return buf; }
        switch (hi) {
        case 0: return "ADDW AX, %[HL+$o]";
        case 2: return "SUBW AX, %[HL+$o]";
        case 4: return "CMPW AX, %[HL+$o]";
        case 5: return "INC %[HL+$o]";
        case 6: return "DEC %[HL+$o]";
        case 7: return "INCW %[HL+$o]";
        default: return NULL;
        }
    }
    if (lo == 0) { snprintf(buf, n, "%s A, %%[HL+B]", alu_names[hi - 8]); return buf; }
    if (lo == 2) { snprintf(buf, n, "%s A, %%[HL+C]", alu_names[hi - 8]); return buf; }
    if (lo >= 4 && lo <= 7) {
        snprintf(buf, n, "CALLT [%04XH]", 0x80u + ((lo - 4u) << 4) + ((hi - 8u) << 1));
        if (buf[7] >= 'A' && buf[7] <= 'F') { /* never: table is 0080H..00BEH */ }
        return buf;
    }
    switch (o) {
    case 0x89: return "DECW %[HL+$o]";
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        snprintf(buf, n, "XCH A, %s", reg8[o & 7u]);
        return buf;
    case 0xA8: return "XCH A, $s";
    case 0xA9: return "XCH A, %[HL+C]";
    case 0xAA: return "XCH A, %$a";
    case 0xAB: return "XCH A, $f";
    case 0xAC: return "XCH A, %[HL]";
    case 0xAD: return "XCH A, %[HL+$o]";
    case 0xAE: return "XCH A, %[DE]";
    case 0xAF: return "XCH A, %[DE+$o]";
    case 0xB8: return "MOV ES, $s";
    case 0xB9: return "XCH A, %[HL+B]";
    case 0xC3: return "BH $r";
    case 0xD3: return "BNH $r";
    case 0xE3: return "SKH";
    case 0xF3: return "SKNH";
    case 0xC8: return "SKC";
    case 0xD8: return "SKNC";
    case 0xE8: return "SKZ";
    case 0xF8: return "SKNZ";
    case 0xC9: return "MOV A, %[HL+B]";
    case 0xD9: return "MOV %[HL+B], A";
    case 0xE9: return "MOV A, %[HL+C]";
    case 0xF9: return "MOV %[HL+C], A";
    case 0xCA: return "CALL AX";
    case 0xDA: return "CALL BC";
    case 0xEA: return "CALL DE";
    case 0xFA: return "CALL HL";
    case 0xCB: return "BR AX";
    case 0xCC: return "BRK";
    case 0xCD: return "POP PSW";
    case 0xDD: return "PUSH PSW";
    case 0xCE: return "MOVS %[HL+$o], X";
    case 0xDE: return "CMPS X, %[HL+$o]";
    case 0xCF: return "SEL RB0";
    case 0xDF: return "SEL RB1";
    case 0xEF: return "SEL RB2";
    case 0xFF: return "SEL RB3";
    case 0xDB: return "ROR A, 1";
    case 0xEB: return "ROL A, 1";
    case 0xFB: return "RORC A, 1";
    case 0xDC: return "ROLC A, 1";
    case 0xEE: return "ROLWC AX, 1";
    case 0xFE: return "ROLWC BC, 1";
    case 0xEC: return "RETB";
    case 0xFC: return "RETI";
    case 0xED: return "HALT";
    case 0xFD: return "STOP";
    default: return NULL;
    }
}

static const char *map3(unsigned o, char *buf, size_t n)
{
    unsigned lo = o & 0xFu, b = (o >> 4) & 7u;
    static const char *const ops[8] = {"MOV1 %s, CY", "SET1 %s", "CLR1 %s", "MOV1 CY, %s",
                                       "AND1 CY, %s", "OR1 CY, %s", "XOR1 CY, %s", NULL};
    char opnd[32];
    if (o < 0x80u) {
        if (lo == 0) { snprintf(buf, n, "SET1 %%$a.%u", b); return buf; }
        if (lo == 8) { snprintf(buf, n, "CLR1 %%$a.%u", b); return buf; }
        snprintf(opnd, sizeof opnd, "%s.%u", lo < 8 ? "$s" : "$f", b);
    } else {
        if (o == 0x80u) return "SET1 CY";
        if (o == 0x88u) return "CLR1 CY";
        if (o == 0xC0u) return "NOT1 CY";
        if (lo == 0 || lo == 8) return NULL;
        snprintf(opnd, sizeof opnd, "%s.%u", lo < 8 ? "%[HL]" : "A", b);
    }
    {
        unsigned k = (lo & 7u) - 1u; /* 1..7 -> 0..6 */
        const char *t = ops[k];
        if (k == 0 && lo >= 8) t = "MOV1 %s, CY";
        snprintf(buf, n, t, opnd);
    }
    return buf;
}

static const char *map4(unsigned o, char *buf, size_t n)
{
    unsigned lo = o & 0xFu, hi = o >> 4, b = hi & 7u;
    static const char *const bops[3] = {"BTCLR", "BT", "BF"};
    if (lo <= 5) {
        const char *opd = (o < 0x80u) ? ((lo & 1u) ? "A" : "$s") : ((lo & 1u) ? "%[HL]" : "$f");
        snprintf(buf, n, "%s %s.%u, $r", bops[lo >> 1], opd, b);
        return buf;
    }
    if (lo == 6 || hi == 0) return NULL;
    switch (lo) {
    case 7: if (hi > 7) return NULL; snprintf(buf, n, "SHL C, %u", hi); return buf;
    case 8: if (hi > 7) return NULL; snprintf(buf, n, "SHL B, %u", hi); return buf;
    case 9: if (hi > 7) return NULL; snprintf(buf, n, "SHL A, %u", hi); return buf;
    case 10: if (hi > 7) return NULL; snprintf(buf, n, "SHR A, %u", hi); return buf;
    case 11: if (hi > 7) return NULL; snprintf(buf, n, "SAR A, %u", hi); return buf;
    case 12: snprintf(buf, n, "SHLW BC, %u", hi); return buf;
    case 13: snprintf(buf, n, "SHLW AX, %u", hi); return buf;
    case 14: snprintf(buf, n, "SHRW AX, %u", hi); return buf;
    default: snprintf(buf, n, "SARW AX, %u", hi); return buf;
    }
}

static void hexh(char *out, size_t n, unsigned v, int digits)
{
    char t[16];
    snprintf(t, sizeof t, "%0*X", digits, v);
    if (t[0] >= 'A' && t[0] <= 'F') snprintf(out, n, "0%sH", t);
    else snprintf(out, n, "%sH", t);
}

static const char *sfr_name(unsigned s)
{
    switch (s) {
    case 0xF8: return "SPL";
    case 0xF9: return "SPH";
    case 0xFA: return "PSW";
    case 0xFC: return "CS";
    case 0xFD: return "ES";
    case 0xFE: return "PMC";
    case 0xFF: return "MEM";
    default: return NULL;
    }
}

/* expand template; returns bytes consumed after `p`, or -1 if out of bytes */
static int expand(const char *t, const uint8_t *p, size_t avail, uint32_t pc_next_base,
                  int es, char *out, size_t outn, int *pcrel_fix)
{
    size_t k = 0, o = 0;
    int pending_rel = 0, rel16 = 0;
    char tmp[32];
    (void)pc_next_base;
    out[0] = 0;
    while (*t && o + 24 < outn) {
        if (*t == '%') {
            if (es) { memcpy(out + o, "ES:", 3); o += 3; }
            t++;
            continue;
        }
        if (*t != '$' || !t[1] || !strchr("bwsfFaArRoW", t[1])) {
            out[o++] = *t++;
            continue;
        }
        switch (t[1]) {
        case 'b':
            if (k + 1 > avail) return -1;
            out[o++] = '#';
            hexh(tmp, sizeof tmp, p[k], 2);
            k += 1;
            break;
        case 'o':
            if (k + 1 > avail) return -1;
            hexh(tmp, sizeof tmp, p[k], 2);
            k += 1;
            break;
        case 'w':
            if (k + 2 > avail) return -1;
            out[o++] = '#';
            hexh(tmp, sizeof tmp, (unsigned)(p[k] | (p[k + 1] << 8)), 4);
            k += 2;
            break;
        case 'W':
            if (k + 2 > avail) return -1;
            hexh(tmp, sizeof tmp, (unsigned)(p[k] | (p[k + 1] << 8)), 4);
            k += 2;
            break;
        case 's': {
            unsigned s;
            if (k + 1 > avail) return -1;
            s = p[k];
            hexh(tmp, sizeof tmp, (0xFE00u + s + (s < 0x20u ? 0x100u : 0u)) & 0xFFFFu, 4);
            k += 1;
            break;
        }
        case 'f':
        case 'F': {
            unsigned s;
            const char *nm;
            if (k + 1 > avail) return -1;
            s = p[k];
            nm = (t[1] == 'F') ? (s == 0xF8u ? "SP" : NULL) : sfr_name(s);
            if (nm) snprintf(tmp, sizeof tmp, "%s", nm);
            else hexh(tmp, sizeof tmp, 0xFFF00u + s, 5);
            k += 1;
            break;
        }
        case 'a':
            if (k + 2 > avail) return -1;
            out[o++] = '!';
            hexh(tmp, sizeof tmp, (unsigned)(p[k] | (p[k + 1] << 8)), 4);
            k += 2;
            break;
        case 'A':
            if (k + 3 > avail) return -1;
            out[o++] = '!';
            out[o++] = '!';
            hexh(tmp, sizeof tmp, (unsigned)(p[k] | (p[k + 1] << 8) | ((p[k + 2] & 0xFu) << 16)), 5);
            k += 3;
            break;
        case 'r':
            if (k + 1 > avail) return -1;
            pending_rel = (int)(o + 1);
            rel16 = 0;
            *pcrel_fix = (int8_t)p[k];
            out[o++] = '$';
            snprintf(tmp, sizeof tmp, "@");
            k += 1;
            break;
        default: /* 'R' */
            if (k + 2 > avail) return -1;
            pending_rel = (int)(o + 2);
            rel16 = 1;
            *pcrel_fix = (int16_t)(p[k] | (p[k + 1] << 8));
            out[o++] = '$';
            out[o++] = '!';
            snprintf(tmp, sizeof tmp, "@");
            k += 2;
            break;
        }
        {
            size_t l = strlen(tmp);
            memcpy(out + o, tmp, l);
            o += l;
        }
        t += 2;
    }
    out[o] = 0;
    (void)pending_rel;
    (void)rel16;
    return (int)k;
}

int rl78_disasm_buf(rl78_core core, const uint8_t *buf, size_t avail, uint32_t pc, char *text,
                    size_t textlen)
{
    char gen[48], raw[96], tmp[16];
    const char *t = NULL;
    size_t pos = 0;
    int es = 0, oplen, k, rel = 0;
    unsigned op;
    if (textlen) text[0] = 0;
    if (avail < 1) goto undef;
    op = buf[0];
    if (op == 0x11) {
        es = 1;
        pos = 1;
        if (avail < 2) goto undef;
        op = buf[1];
    }
    if (op == 0x61 || op == 0x71 || op == 0x31) {
        unsigned o;
        if (avail < pos + 2) goto undef;
        o = buf[pos + 1];
        if (op == 0x61) {
            t = map2(o, gen, sizeof gen);
            if (t && core == RL78_CORE_S1 && (o & 0xFu) == 0xFu && o >= 0xCFu) t = NULL; /* SEL */
        } else if (op == 0x71) {
            if (o == 0x7Au && avail >= pos + 3 && buf[pos + 2] == 0xFAu) { t = "EI"; pos++; }
            else if (o == 0x7Bu && avail >= pos + 3 && buf[pos + 2] == 0xFAu) { t = "DI"; pos++; }
            else t = map3(o, gen, sizeof gen);
        } else {
            t = map4(o, gen, sizeof gen);
        }
        pos += 2;
    } else if (op == 0xCE && (core == RL78_CORE_S3 || core == RL78_CORE_AUTO) && avail >= pos + 3 && buf[pos + 1] == 0xFBu) {
        switch (buf[pos + 2]) {
        case 0x01: t = "MULHU"; break;
        case 0x02: t = "MULH"; break;
        case 0x03: t = "DIVHU"; break;
        case 0x0B: t = "DIVWU"; break;
        case 0x05: t = "MACHU"; break;
        case 0x06: t = "MACH"; break;
        default: t = NULL; break;
        }
        if (t) pos += 3;
        else { t = map1[op]; pos += 1; }
    } else {
        t = map1[op];
        pos += 1;
    }
    if (!t) goto undef;
    /* ES: allowed only where the template has a '%' marker */
    if (es && !strchr(t, '%')) goto undef;
    k = expand(t, buf + pos, avail - pos, 0, es, raw, sizeof raw, &rel);
    if (k < 0) goto undef;
    oplen = (int)pos + k;
    /* resolve relative targets: "@" placeholder */
    {
        char *at = strchr(raw, '@');
        if (at) {
            uint32_t tgt = (pc + (uint32_t)oplen + (uint32_t)rel) & 0xFFFFFu;
            hexh(tmp, sizeof tmp, tgt, 5);
            snprintf(text, textlen, "%.*s%s%s", (int)(at - raw), raw, tmp, at + 1);
            return oplen;
        }
    }
    snprintf(text, textlen, "%s", raw);
    return oplen;
undef:
    snprintf(text, textlen, "(undefined)");
    return 0;
}

int rl78_disasm(rl78_cpu *cpu, uint32_t pc, char *text, size_t textlen)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) b[i] = rl78_peek8(cpu, (pc + (uint32_t)i) & 0xFFFFFu);
    return rl78_disasm_buf(cpu->cfg.core, b, 8, pc, text, textlen);
}
