; CA78K0R ABI の自己完結テスト。実物のコードは含めない。
.text
.hword _start
.org 0x100
.global _start
_start:
 movw sp, #0xFDE0
 movw ax, #0x1234
 movw !0xE000, ax
 call !!_main
.global _main
_main:
 br $_main
.global _echo8
_echo8:
 movw bc, ax
 ret
.global _echo16
_echo16:
 movw bc, ax
 ret
.global _second8
_second8:
 push hl
 movw hl, sp
 mov a, [hl+6]
 mov c, a
 pop hl
 ret
.global _second16
_second16:
 push hl
 movw hl, sp
 movw ax, [hl+6]
 movw bc, ax
 pop hl
 ret
.global _second32
_second32:
 push hl
 movw hl, sp
 movw ax, [hl+6]
 movw bc, ax
 movw ax, [hl+8]
 movw de, ax
 pop hl
 ret
.global _ret32
_ret32:
 movw bc, #0x1234
 movw de, #0x5678
 ret
.global _readptr
_readptr:
 push hl
 movw hl, ax
 mov a, [hl]
 mov c, a
 pop hl
 ret
.global _echoptr
_echoptr:
 movw bc, ax
 ret
.global _cycle
_cycle:
 movw ax, !0xE002
 incw ax
 movw !0xE002, ax
 ret
.global _first32
_first32:
 ret

.global _third32
_third32:
 push hl
 movw hl, sp
 movw ax, [hl+8]
 movw bc, ax
 movw ax, [hl+10]
 movw de, ax
 pop hl
 ret

; ROM checksum through the mirror; range includes init_end, stub and skip.
; Independent expected sum of the unchanged instruction bytes, modulo 256.
_rom_check:
 movw hl, #(_main + 0x8000)
 mov b, #0
.Lrom_sum:
 mov a, [hl]
 add a, b
 mov b, a
 incw hl
 movw ax, hl
 cmpw ax, #(_third32 + 0x8000)
 bnz $.Lrom_sum
 mov a, b
 cmp a, #0x47
 bz $.Lrom_ok
_rom_reset:
 .byte 0xFF
 br $_rom_reset
.Lrom_ok:
 br !!_start
