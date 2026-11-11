/*
 * t_insn.c - per-instruction behaviour tests.  Each vector is
 *   core, initial state, instruction(s) (assembled by tasm), expected state.
 * Expected values were derived by hand from the operation/flag definitions
 * in R01US0015 section 6 (comments cite the rule where it is not obvious).
 *
 * State syntax: space separated NAME=HEX items.
 *   A X B C D E H L AX BC DE HL PSW SP ES CS PC PMC
 *   CY Z AC IE (single flags, 0/1)       [addr]=byte  W[addr]=word
 *   CLK=n (clocks used, decimal)  STOP=halt|undefined|break
 * addr is 20-bit (e.g. FFE80).  Unlisted items are not checked; the initial
 * state starts from PSW=06 (reset), SP=FE00, ES=0F, CS=0, registers 0.
 */
#include <stdlib.h>
#include <string.h>

#include "tcommon.h"

typedef struct {
    int core;
    const char *init, *code, *expect;
} vec;

static const vec V[] = {
    /* ---- 8-bit add / sub: Z, AC (carry 3->4 / borrow 4->3), CY ---- */
    {3, "A=0F PSW=00", "ADD A, #01H", "A=10 Z=0 AC=1 CY=0"},
    {3, "A=FF PSW=00", "ADD A, #01H", "A=00 Z=1 AC=1 CY=1"},
    {3, "A=80 PSW=00", "ADD A, #80H", "A=00 Z=1 AC=0 CY=1"},
    {3, "A=7F PSW=00", "ADD A, #01H", "A=80 Z=0 AC=1 CY=0"},
    {3, "A=0E PSW=01", "ADDC A, #01H", "A=10 Z=0 AC=1 CY=0"},
    {3, "A=FF PSW=01", "ADDC A, #00H", "A=00 Z=1 AC=1 CY=1"},
    {3, "A=00 PSW=01", "ADDC A, #0FFH", "A=00 Z=1 AC=1 CY=1"},
    {3, "A=10 PSW=00", "SUB A, #01H", "A=0F Z=0 AC=1 CY=0"},
    {3, "A=00 PSW=00", "SUB A, #01H", "A=0FF Z=0 AC=1 CY=1"},
    {3, "A=55 PSW=11", "SUB A, #55H", "A=00 Z=1 AC=0 CY=0"},
    {3, "A=10 PSW=01", "SUBC A, #0FH", "A=00 Z=1 AC=1 CY=0"},
    {3, "A=00 PSW=01", "SUBC A, #00H", "A=0FF Z=0 AC=1 CY=1"},
    {3, "A=80 PSW=00", "CMP A, #81H", "A=80 Z=0 AC=1 CY=1"},
    {3, "A=42 PSW=00", "CMP A, #42H", "A=42 Z=1 AC=0 CY=0"},
    {3, "A=0F0 PSW=11", "AND A, #0FH", "A=00 Z=1 AC=1 CY=1"},   /* AND: only Z */
    {3, "A=0F0 PSW=40", "OR A, #01H", "A=0F1 Z=0 AC=0 CY=0"},
    {3, "A=0FF PSW=00", "XOR A, #0FFH", "A=00 Z=1"},
    {3, "B=10 A=01 PSW=00", "SUB B, A", "B=0F A=01 AC=1 CY=0"},
    {3, "A=01 PSW=00", "ADD A, A", "A=02"},
    {3, "A=20 PSW=00 HL=0FE80 [FFE85]=20", "ADD A, [HL+05H]", "A=40 Z=0 CY=0"},
    {3, "A=20 PSW=00 HL=0FE80 B=3 [FFE83]=0E0", "ADD A, [HL+B]", "A=00 Z=1 CY=1"},
    {3, "A=01 PSW=00 HL=0FE80 C=4 [FFE84]=02", "SUB A, [HL+C]", "A=0FF CY=1"},
    {3, "[FFE20]=0FF PSW=00", "ADD 0FE20H, #01H", "[FFE20]=00 Z=1 CY=1 AC=1"},
    {3, "[FFE20]=05 PSW=00", "CMP 0FE20H, #05H", "[FFE20]=05 Z=1"},
    {3, "[FFE80]=05 PSW=00", "CMP !0FE80H, #06H", "Z=0 CY=1 AC=1"},
    /* ---- CMP0 / CMPS / MOVS (special flag rules, section 6.3 / 6.1) ---- */
    {3, "X=00 PSW=11", "CMP0 X", "Z=1 AC=0 CY=0"},
    {3, "B=80 PSW=51", "CMP0 B", "Z=0 AC=0 CY=0"},
    {3, "[FFE21]=00 PSW=00", "CMP0 0FE21H", "Z=1"},
    /* CMPS: Z = (X-src == 0); CY = result != 0 || A == 0 || X == 0 */
    {3, "A=01 X=05 HL=0FE80 [FFE82]=05 PSW=01", "CMPS X, [HL+02H]", "Z=1 CY=0 AC=0"},
    {3, "A=00 X=05 HL=0FE80 [FFE82]=05 PSW=00", "CMPS X, [HL+02H]", "Z=1 CY=1"},
    {3, "A=01 X=05 HL=0FE80 [FFE82]=04 PSW=00", "CMPS X, [HL+02H]", "Z=0 CY=1 AC=0"},
    {3, "A=01 X=10 HL=0FE80 [FFE82]=01 PSW=00", "CMPS X, [HL+02H]", "Z=0 CY=1 AC=1"},
    /* MOVS: Z = (X == 0); CY = (A == 0 || X == 0); AC unchanged */
    {3, "A=01 X=05 HL=0FE80 PSW=51", "MOVS [HL+03H], X", "[FFE83]=05 Z=0 CY=0 AC=1"},
    {3, "A=00 X=05 HL=0FE80 PSW=00", "MOVS [HL+03H], X", "[FFE83]=05 Z=0 CY=1"},
    {3, "A=01 X=00 HL=0FE80 PSW=00", "MOVS [HL+03H], X", "[FFE83]=00 Z=1 CY=1"},
    /* ---- INC/DEC: Z, AC; CY unchanged ---- */
    {3, "B=0FF PSW=01", "INC B", "B=00 Z=1 AC=1 CY=1"},
    {3, "B=0F PSW=00", "INC B", "B=10 Z=0 AC=1 CY=0"},
    {3, "B=00 PSW=01", "DEC B", "B=0FF Z=0 AC=1 CY=1"},   /* AC: borrow 4->3 (interpretation) */
    {3, "B=01 PSW=10", "DEC B", "B=00 Z=1 AC=0 CY=0"},
    {3, "B=10 PSW=00", "DEC B", "B=0F AC=1"},
    {3, "HL=0FE80 [FFE90]=7F PSW=00", "INC [HL+10H]", "[FFE90]=80 AC=1 Z=0"},
    {3, "[FFE22]=01 PSW=00", "DEC 0FE22H", "[FFE22]=00 Z=1"},
    {3, "AX=0FFFF PSW=46", "INCW AX", "AX=0000 PSW=46"},          /* INCW: no flags */
    {3, "DE=0000 PSW=00", "DECW DE", "DE=0FFFF PSW=00"},
    {3, "W[FFE80]=0FFFF PSW=00", "INCW !0FE80H", "W[FFE80]=0000 PSW=00"},
    {3, "W[FFE20]=1000 PSW=00", "DECW 0FE20H", "W[FFE20]=0FFF PSW=00"},
    /* ---- 16-bit add/sub: Z, CY (AC undefined in the manual) ---- */
    {3, "AX=0FFFF PSW=00", "ADDW AX, #0001H", "AX=0000 Z=1 CY=1"},
    {3, "AX=7FFF PSW=01", "ADDW AX, #0001H", "AX=8000 Z=0 CY=0"},
    {3, "AX=0000 BC=0001 PSW=00", "SUBW AX, BC", "AX=0FFFF Z=0 CY=1"},
    {3, "AX=1234 DE=1234 PSW=00", "CMPW AX, DE", "AX=1234 Z=1 CY=0"},
    {3, "AX=1000 HL=0FE80 W[FFE90]=0F000 PSW=00", "ADDW AX, [HL+10H]", "AX=0000 Z=1 CY=1"},
    {3, "AX=0002 PSW=00", "ADDW AX, AX", "AX=0004"},
    /* ---- shifts / rotates (only CY) ---- */
    {3, "A=81 PSW=00", "SHL A, 1", "A=02 CY=1"},
    {3, "A=81 PSW=00", "SHR A, 1", "A=40 CY=1"},
    {3, "A=81 PSW=00", "SAR A, 1", "A=0C0 CY=1"},
    {3, "A=81 PSW=00", "SAR A, 7", "A=0FF CY=0"},
    {3, "B=40 PSW=40", "SHL B, 2", "B=00 CY=1 Z=1"},             /* Z untouched */
    {3, "C=01 PSW=00", "SHL C, 7", "C=80 CY=0"},
    {3, "AX=0003 PSW=00", "SHLW AX, 15", "AX=8000 CY=1"},
    {3, "BC=0001 PSW=00", "SHLW BC, 15", "BC=8000 CY=0"},
    {3, "AX=8000 PSW=00", "SARW AX, 4", "AX=0F800 CY=0"},
    {3, "AX=8008 PSW=00", "SHRW AX, 4", "AX=0800 CY=1"},
    {3, "A=01 PSW=00", "ROR A, 1", "A=80 CY=1"},
    {3, "A=80 PSW=00", "ROL A, 1", "A=01 CY=1"},
    {3, "A=01 PSW=00", "RORC A, 1", "A=00 CY=1"},
    {3, "A=00 PSW=01", "ROLC A, 1", "A=01 CY=0"},
    {3, "AX=8000 PSW=01", "ROLWC AX, 1", "AX=0001 CY=1"},
    {3, "BC=4000 PSW=00", "ROLWC BC, 1", "BC=8000 CY=0"},
    /* ---- multiply / divide / MAC (S3; section 6.5) ---- */
    {3, "A=0FF X=0FF PSW=0D7", "MULU X", "AX=0FE01 PSW=0D7"},
    {1, "A=0FF X=02", "MULU X", "AX=01FE"},
    {3, "AX=0FFFF BC=0FFFF", "MULHU", "AX=0001 BC=0FFFE"},
    {3, "AX=0FFFF BC=0002", "MULH", "AX=0FFFE BC=0FFFF"},
    {3, "AX=8000 BC=8000", "MULH", "AX=0000 BC=4000"},
    {3, "AX=1234 DE=0010 PSW=00", "DIVHU", "AX=0123 DE=0004 PSW=00"},
    {3, "AX=1234 DE=0000", "DIVHU", "AX=0FFFF DE=1234"},       /* divide by 0 rule */
    {3, "AX=0000 BC=0001 DE=0003 HL=0000", "DIVWU", "AX=5555 BC=0000 DE=0001 HL=0000"},
    {3, "AX=5678 BC=1234 DE=0000 HL=0000", "DIVWU", "AX=0FFFF BC=0FFFF DE=5678 HL=1234"},
    {3, "W[FFFF0]=0FFFF W[FFFF2]=0FFFF AX=0001 BC=0001 PSW=10", "MACHU", "W[FFFF0]=0000 W[FFFF2]=0000 CY=1 AC=0"},
    {3, "W[FFFF0]=5678 W[FFFF2]=1234 AX=1234 BC=5678 PSW=00", "MACHU", "W[FFFF0]=56D8 W[FFFF2]=185A CY=0"},
    {3, "W[FFFF0]=0FFFF W[FFFF2]=7FFF AX=0001 BC=0001 PSW=00", "MACH", "W[FFFF0]=0000 W[FFFF2]=8000 CY=1 AC=1"},
    {3, "W[FFFF0]=0000 W[FFFF2]=8000 AX=0FFFF BC=0001 PSW=00", "MACH", "W[FFFF0]=0FFFF W[FFFF2]=7FFF CY=1 AC=0"},
    {3, "W[FFFF0]=0000 W[FFFF2]=0000 AX=0FFFF BC=0001 PSW=00", "MACH", "W[FFFF0]=0FFFF W[FFFF2]=0FFFF CY=0 AC=1"},
    {2, "AX=0FFFF BC=0002", "MULHU", "AX=0FFFF BC=0002 [FFFFB]=01"}, /* S2: CE FB 01 = MOV 0FFFFBH, #01H */
    /* ---- transfers ---- */
    {3, "A=12 X=34", "XCH A, X", "A=34 X=12"},
    /* XCH A, PSW switching banks: A is the register of the bank at instruction start */
    {3, "PSW=00 A=08 [FFEF1]=77", "XCH A, PSW", "PSW=08 [FFEF9]=00 [FFEF1]=77"},
    {3, "A=12 H=56", "XCH A, H", "A=56 H=12"},
    {3, "A=12 [FFE20]=99", "XCH A, 0FE20H", "A=99 [FFE20]=12"},
    {3, "A=12 HL=0FE80 B=1 [FFE81]=77", "XCH A, [HL+B]", "A=77 [FFE81]=12"},
    {3, "AX=1234 BC=5678", "XCHW AX, BC", "AX=5678 BC=1234"},
    {3, "A=55", "ONEB A", "A=01"},
    {3, "[FFE80]=55", "CLRB !0FE80H", "[FFE80]=00"},
    {3, "BC=1234", "ONEW BC", "BC=0001"},
    {3, "AX=1234", "CLRW AX", "AX=0000"},
    {3, "BC=0002 [FFE82]=0AB", "MOV A, 0FE80H[BC]", "A=0AB"},
    {3, "B=02 [FFE82]=0CD", "MOV A, 0FE80H[B]", "A=0CD"},
    {3, "C=03 A=11", "MOV 0FE80H[C], A", "[FFE83]=11"},
    {3, "SP=0FE00 [FFE03]=0EE", "MOV A, [SP+03H]", "A=0EE"},
    {3, "SP=0FE00 AX=0BEEF", "MOVW [SP+02H], AX", "W[FFE02]=0BEEF"},
    {3, "DE=0FE80 [FFE85]=42", "MOV A, [DE+05H]", "A=42"},
    {3, "DE=0FE80", "MOV [DE+05H], #33H", "[FFE85]=33"},
    {3, "W[FFE80]=1234", "MOVW AX, !0FE81H", "AX=1234"},        /* odd address: bit 0 forced to 0 */
    {3, "HL=0FE81 W[FFE80]=5678", "MOVW AX, [HL]", "AX=5678"},
    {3, "A=0FF", "MOV ES, A", "ES=0F"},                          /* ES bits 7-4 are 0 */
    {3, "A=0F3", "MOV CS, A", "CS=03"},
    {3, "[FFE20]=0F7", "MOV ES, 0FE20H", "ES=07"},
    {3, "A=37", "MOV 0FFFFDH, A", "ES=07"},                      /* ES via its SFR address */
    {3, "", "MOVW SP, #0FE01H", "SP=0FE00"},                     /* SP bit 0 always 0 */
    {3, "AX=1235", "MOVW SP, AX", "SP=1234"},
    {3, "SP=0FE00", "MOVW HL, SP", "HL=0FE00"},
    {3, "SP=0FE00", "ADDW SP, #10H", "SP=0FE10 PSW=06"},
    {3, "SP=0FE00", "SUBW SP, #10H", "SP=0FDF0 PSW=06"},
    {3, "A=0", "MOV PSW, #0C3H", "PSW=0C3"},
    {1, "A=0", "MOV PSW, #0FFH", "PSW=0D7"},                     /* S1: no RBS bits */
    /* saddr 00H-1FH map to FFF00H-FFF1FH */
    {3, "A=5A", "MOV 0FF05H, A", "[FFF05]=5A"},
    {3, "[FFF1F]=77", "MOV A, 0FF1FH", "A=77"},
    /* ES: prefix (data in code flash, test machine rom 0-FFFF) */
    {3, "ES=00 HL=0100", "MOV A, ES:[HL]", "A=11"},               /* reads its own first byte (11H) */
    {3, "ES=0F HL=0FE80 [FFE80]=99", "MOV A, ES:[HL]", "A=99"},
    {3, "ES=0F BC=0004 [FFE84]=12", "MOV A, ES:0FE80H[BC]", "A=12"},
    /* ---- bit manipulation ---- */
    {3, "A=80 PSW=00", "MOV1 CY, A.7", "CY=1"},
    {3, "A=00 PSW=01", "MOV1 A.3, CY", "A=08"},
    {3, "A=0FF PSW=00", "MOV1 A.0, CY", "A=0FE"},
    {3, "A=01 PSW=01", "AND1 CY, A.1", "CY=0"},
    {3, "A=02 PSW=00", "OR1 CY, A.1", "CY=1"},
    {3, "A=02 PSW=01", "XOR1 CY, A.1", "CY=0"},
    {3, "PSW=00", "SET1 CY", "CY=1"},
    {3, "PSW=0FF", "CLR1 CY", "PSW=0FE"},
    {3, "PSW=00", "NOT1 CY", "CY=1"},
    {3, "[FFE20]=00", "SET1 0FE20H.7", "[FFE20]=80"},
    {3, "[FFE80]=0FF", "CLR1 !0FE80H.0", "[FFE80]=0FE"},
    {3, "HL=0FE80 [FFE80]=00", "SET1 [HL].4", "[FFE80]=10"},
    {3, "PSW=00", "MOV1 PSW.6, CY", "PSW=00"},
    {3, "PSW=01", "MOV1 PSW.6, CY", "PSW=41"},
    {3, "PSW=00", "SET1 PSW.4", "PSW=10"},
    {3, "PSW=00", "EI", "PSW=80"},
    {3, "PSW=86", "DI", "PSW=06"},
    {3, "PSW=00", "SET1 PSW.3", "PSW=08"},                        /* RBS0 -> bank 1 */
    {3, "PSW=40", "MOV1 CY, PSW.6", "CY=1"},
    {3, "[FFF20]=04 PSW=00", "MOV1 CY, 0FFF20H.2", "CY=1"},
    /* ---- branches (PC relative to the instruction at 0100H) ---- */
    {3, "PSW=01", "BC $0110H", "PC=0110 CLK=4"},
    {3, "PSW=00", "BC $0110H", "PC=0102 CLK=2"},
    {3, "PSW=00", "BNC $00F0H", "PC=00F0"},
    {3, "PSW=40", "BZ $0110H", "PC=0110"},
    {3, "PSW=40", "BNZ $0110H", "PC=0102"},
    {3, "PSW=00", "BH $0110H", "PC=0110 CLK=4"},                  /* Z|CY == 0 */
    {3, "PSW=01", "BH $0110H", "PC=0103 CLK=2"},
    {3, "PSW=40", "BNH $0110H", "PC=0110"},
    {3, "", "BR $0181H", "PC=0181"},                              /* 0102+7F */
    {3, "", "BR $0082H", "PC=0082"},                              /* 0102-80 */
    {3, "", "BR $!8000H", "PC=8000"},
    {3, "", "BR !1234H", "PC=1234"},
    {3, "", "BR !!12345H", "PC=12345"},
    {3, "AX=0300 CS=01", "BR AX", "PC=10300"},
    {3, "A=01", "BT A.0, $0110H", "PC=0110 CLK=5"},
    {3, "A=00", "BT A.0, $0110H", "PC=0103 CLK=3"},
    {3, "A=00", "BF A.0, $0110H", "PC=0110"},
    {3, "[FFE20]=08", "BTCLR 0FE20H.3, $0110H", "PC=0110 [FFE20]=00"},
    {3, "[FFE20]=00", "BTCLR 0FE20H.3, $0110H", "PC=0104 [FFE20]=00"},
    {3, "PSW=41", "BTCLR PSW.0, $0110H", "PC=0110 PSW=40"},
    {3, "HL=0FE80 [FFE80]=02", "BT [HL].1, $0110H", "PC=0110"},
    {3, "HL=0FE80 [FFE80]=02", "BTCLR [HL].1, $0110H", "PC=0110 [FFE80]=00"},
    /* ---- skips: next instruction skipped (counted as an instruction, 1 clock) ---- */
    {3, "PSW=01 A=00", "SKC\nMOV A, #01H\nMOV X, #02H", "PC=0106 A=00"},
    {3, "PSW=00 A=00", "SKC\nMOV A, #01H", "PC=0104 A=01"},
    {3, "PSW=40", "SKZ\nMOV !0FE80H, #05H", "PC=0106"},
    {3, "PSW=00", "SKH\nNOP", "PC=0103"},
    {3, "PSW=01", "SKNH\nNOP", "PC=0103"},
    {3, "PSW=01", "SKC\nMOV ES:!1234H, #05H", "PC=0107 CLK=3"},  /* skipped ES: insn = 2 clocks */
    /* ---- calls / returns / stack ---- */
    {3, "SP=0FE00 [FFDFF]=55", "CALL !1234H", "PC=1234 SP=0FDFC [FFDFC]=03 [FFDFD]=01 [FFDFE]=00 [FFDFF]=00 CLK=3"},
    {3, "SP=0FE00", "CALL !!23456H", "PC=23456 [FFDFC]=04 [FFDFD]=01 [FFDFE]=00"},
    {3, "SP=0FE00", "CALL $!0200H", "PC=0200 [FFDFC]=03"},
    {3, "SP=0FE00 HL=0400 CS=02", "CALL HL", "PC=20400 [FFDFC]=02 [FFDFD]=01 [FFDFE]=00"},
    {3, "SP=0FDFC [FFDFC]=34 [FFDFD]=12 [FFDFE]=05", "RET", "PC=51234 SP=0FE00 CLK=6"},
    {1, "SP=0FDFC [FFDFC]=34 [FFDFD]=12 [FFDFE]=00", "RET", "PC=1234 CLK=7"},
    {3, "SP=0FDFC [FFDFC]=34 [FFDFD]=12 [FFDFE]=00 [FFDFF]=0C1 PSW=00", "RETI", "PC=1234 SP=0FE00 PSW=0C1"},
    {3, "SP=0FDFC [FFDFC]=34 [FFDFD]=12 [FFDFE]=00 [FFDFF]=82 PSW=00", "RETB", "PC=1234 PSW=82"},
    {3, "SP=0FE00 PSW=0C5", "PUSH PSW", "SP=0FDFE [FFDFF]=0C5 [FFDFE]=00"},
    {3, "SP=0FDFE [FFDFF]=46 [FFDFE]=99 PSW=00", "POP PSW", "SP=0FE00 PSW=46"},
    {3, "SP=0FE00 AX=1234", "PUSH AX", "SP=0FDFE [FFDFF]=12 [FFDFE]=34"},
    {3, "SP=0FDFE [FFDFF]=12 [FFDFE]=34", "POP HL", "HL=1234 SP=0FE00"},
    {3, "SP=0FE00 PSW=86", "BRK", "PC=0B000 SP=0FDFC [FFDFF]=86 [FFDFE]=00 [FFDFD]=01 [FFDFC]=02 PSW=06 CLK=5"},
    {3, "SP=0FE00", "CALLT [0084H]", "PC=0C000 SP=0FDFC [FFDFC]=02 [FFDFD]=01"},
    /* ---- register banks ---- */
    {3, "PSW=00", "SEL RB1\nMOV A, #12H", "[FFEF1]=12 PSW=08"},
    {3, "PSW=00", "SEL RB2\nMOV A, #12H", "[FFEE9]=12 PSW=20"},
    {3, "PSW=00", "SEL RB3\nMOVW AX, #1234H", "[FFEE0]=34 [FFEE1]=12 PSW=28"},
    {1, "PSW=00", "SEL RB1", "STOP=undefined"},
    {3, "PSW=00", "MOV PSW, #08H\nMOV X, #77H", "[FFEF0]=77"},
    {3, "PSW=00", "MOV !0FEF9H, #5AH\nMOV A, X", "X=00 A=00"},   /* A of bank 0 is at FFEF9H */
    {3, "PSW=00", "MOV !0FEF9H, #5AH\nNOP", "A=5A"},
    /* ---- CPU control ---- */
    {3, "", "NOP", "PC=0101 CLK=1"},
    {3, "", "HALT", "STOP=halt PC=0102"},
    {3, "", "STOP", "STOP=stop PC=0102"},
    {3, "", ".db 0FFH", "STOP=undefined PC=0100"},
    {3, "", ".db 21H", "STOP=undefined PC=0100"},
    {3, "", ".db 61H, 81H", "STOP=undefined"},
    {3, "", ".db 11H, 00H", "STOP=undefined"},                    /* ES: on NOP */
    {3, "", ".db 31H, 06H", "STOP=undefined"},
    /* ---- mirror (test machine: F2000H-FBFFFH <- 02000H) and PMC.MAA ---- */
    {3, "", "MOV A, !2000H", "A=0A5 CLK=4"},                     /* flash read: Note 2 clocks */
    {3, "PMC=01", "MOV A, !2000H", "A=5A"},                      /* MAA=1: 12000H */
    {3, "", "MOV !2000H, #00H\nMOV A, !2000H", "A=0A5"},          /* mirror is read-only */
    {3, "ES=00", "MOV ES:!2000H, #00H\nMOV A, ES:!2000H", "A=0A5"}, /* code flash: writes ignored */
};

static int parse_hex(const char *s, unsigned long *v)
{
    char *e;
    *v = strtoul(s, &e, 16);
    return e != s;
}

static void apply(rl78_cpu *cpu, const char *spec, int check, int idx, rl78_stop_reason stop, uint64_t clk)
{
    char buf[512];
    char *p = buf, *tok;
    snprintf(buf, sizeof buf, "%s", spec);
    while ((tok = strtok(p, " ")) != NULL) {
        char *eq = strchr(tok, '=');
        unsigned long v = 0;
        p = NULL;
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(tok, "STOP")) {
            rl78_stop_reason want = !strcmp(eq + 1, "halt") ? RL78_STOP_HALT
                                    : !strcmp(eq + 1, "stop") ? RL78_STOP_STOPMODE
                                    : !strcmp(eq + 1, "break") ? RL78_STOP_BREAKPOINT
                                                               : RL78_STOP_UNDEFINED;
            if (check) CHECKF(stop == want, "vector %d: stop %d expected %s", idx, (int)stop, eq + 1);
            continue;
        }
        if (!strcmp(tok, "CLK")) {
            if (check) CHECKF(clk == strtoul(eq + 1, NULL, 10), "vector %d (%s): clocks %llu expected %s", idx, V[idx].code,
                              (unsigned long long)clk, eq + 1);
            continue;
        }
        parse_hex(eq + 1, &v);
        if (tok[0] == '[' || (tok[0] == 'W' && tok[1] == '[')) {
            int w = tok[0] == 'W';
            unsigned long a = strtoul(tok + (w ? 2 : 1), NULL, 16);
            if (check) {
                unsigned got = w ? (unsigned)(rl78_peek8(cpu, (uint32_t)a) | (rl78_peek8(cpu, (uint32_t)a + 1) << 8))
                                 : rl78_peek8(cpu, (uint32_t)a);
                CHECKF(got == v, "vector %d (%s): %s] = %X expected %lX", idx, V[idx].code, tok, got, v);
            } else {
                uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
                rl78_load(cpu, (uint32_t)a, b, w ? 2 : 1);
            }
            continue;
        }
        {
            static const struct { const char *n; rl78_reg r; } regs[] = {
                {"A", RL78_REG_A}, {"X", RL78_REG_X}, {"B", RL78_REG_B}, {"C", RL78_REG_C},
                {"D", RL78_REG_D}, {"E", RL78_REG_E}, {"H", RL78_REG_H}, {"L", RL78_REG_L},
                {"AX", RL78_REG_AX}, {"BC", RL78_REG_BC}, {"DE", RL78_REG_DE}, {"HL", RL78_REG_HL},
                {"PSW", RL78_REG_PSW}, {"SP", RL78_REG_SP}, {"ES", RL78_REG_ES}, {"CS", RL78_REG_CS},
                {"PC", RL78_REG_PC}, {"PMC", RL78_REG_PMC},
            };
            static const struct { const char *n; unsigned m; } flags[] = {
                {"CY", RL78_PSW_CY}, {"Z", RL78_PSW_Z}, {"AC", RL78_PSW_AC}, {"IE", RL78_PSW_IE},
            };
            size_t i;
            int done = 0;
            for (i = 0; i < sizeof regs / sizeof regs[0]; i++)
                if (!strcmp(tok, regs[i].n)) {
                    if (check)
                        CHECKF(rl78_get_reg(cpu, regs[i].r) == v, "vector %d (%s): %s = %X expected %lX", idx,
                               V[idx].code, tok, (unsigned)rl78_get_reg(cpu, regs[i].r), v);
                    else
                        rl78_set_reg(cpu, regs[i].r, (uint32_t)v);
                    done = 1;
                }
            for (i = 0; !done && i < sizeof flags / sizeof flags[0]; i++)
                if (!strcmp(tok, flags[i].n)) {
                    unsigned psw = rl78_get_reg(cpu, RL78_REG_PSW);
                    if (check)
                        CHECKF(((psw & flags[i].m) != 0) == (v != 0), "vector %d (%s): flag %s = %d expected %lu",
                               idx, V[idx].code, tok, (psw & flags[i].m) != 0, v);
                    else
                        rl78_set_reg(cpu, RL78_REG_PSW, v ? (psw | flags[i].m) : (psw & ~flags[i].m));
                    done = 1;
                }
            if (!done) CHECKF(0, "vector %d: unknown item %s", idx, tok);
        }
    }
}

void suite_insn(void)
{
    size_t i;
    for (i = 0; i < sizeof V / sizeof V[0]; i++) {
        const vec *v = &V[i];
        rl78_cpu *cpu = t_cpu((rl78_core)v->core);
        char src[512];
        int n_insn = 1;
        const char *q;
        rl78_stop_reason r;
        uint64_t c0;
        for (q = v->code; *q; q++)
            if (*q == '\n') n_insn++;
        /* fixed fixtures: CALLT table, BRK vector, flash/mirror data */
        snprintf(src, sizeof src,
                 ".org 7EH\n.dw 0B000H\n.org 84H\n.dw 0C000H\n"
                 ".org 2000H\n.db 0A5H\n.org 12000H\n.db 5AH\n"
                 ".org 0100H\nstart:\n%s\n", v->code);
        if (t_load(cpu, src)) { rl78_destroy(cpu); continue; }
        rl78_set_reg(cpu, RL78_REG_SP, 0xFE00);
        rl78_set_reg(cpu, RL78_REG_PSW, 0x06);
        rl78_set_reg(cpu, RL78_REG_PC, 0x100);
        apply(cpu, v->init, 0, (int)i, RL78_STOP_LIMIT, 0);
        c0 = rl78_clock_count(cpu);
        r = rl78_run(cpu, (uint64_t)n_insn);
        /* a skipped instruction counts as executed: run until the code ends or stop */
        apply(cpu, v->expect, 1, (int)i, r, rl78_clock_count(cpu) - c0);
        if (!strstr(v->expect, "STOP=")) CHECKF(r == RL78_STOP_LIMIT, "vector %zu (%s): stopped with %d", i, v->code, (int)r);
        rl78_destroy(cpu);
    }
    printf("  instruction vectors: %zu\n", sizeof V / sizeof V[0]);
}
