; testprog.s - 本物のバックエンドのテスト用プログラム（RL78、CC-RL の呼び出し規約どおりに手書き）
;
; tests/tools/gen_testprog.sh が GNU の rl78-elf-as / ld でアセンブルし、機械語のバイト列とラベルのアドレスを
; tests/tools/testprog_code.h に書き出す。テスト用の .abs（ELF + DWARF）は tests/tools/mkabs.c がその .h から組み立てる。
; CI や利用者の PC では GNU ツールは要らない（.h をコミットしてある）。
;
; メモリ配置（mkabs.c の DWARF と合わせること）
;   0x00000 リセットベクタ       0x00100 コード
;   0x01800 .data の初期値（ROM） 0x01840 .sdata の初期値（ROM）  0x02000 .const
;   0xFE000 .dataR  0xFE100 .bss  0xFF000〜0xFFE1F スタック  0xFFE40 .sdataR
;
; 変数（RAM。mkabs.c の表と同じアドレス）
	.set	g_init16,	0xFE000		; uint16_t = 0x1234
	.set	g_init8,	0xFE002		; uint8_t = 0x56
	.set	g_initarr,	0xFE003		; uint8_t[3] = {1, 2, 3}
	.set	gp_init,	0xFE006		; uint8_t * = &g_initarr
	.set	gfp_init,	0xFE008		; const uint8_t __far * = &g_lut（0x02000）
	.set	DATAR,		0xFE000
	.set	DATA_SIZE,	12
	.set	SDATAR,		0xFFE40
	.set	SDATA_SIZE,	2
	.set	BSS,		0xFE100
	.set	BSS_END,	0xFE160

	.set	g_a,		0xFE100		; uint8_t
	.set	g_c,		0xFE101		; uint8_t
	.set	g_b,		0xFE102		; uint16_t
	.set	g_l,		0xFE104		; uint32_t
	.set	g_w,		0xFE108		; uint16_t
	.set	g_sa,		0xFE10A		; uint16_t
	.set	g_sb,		0xFE10C		; uint16_t
	.set	g_sc,		0xFE10E		; uint16_t
	.set	g_sdd,		0xFE110		; uint16_t
	.set	g_se,		0xFE112		; uint8_t
	.set	g_out,		0xFE113		; uint8_t
	.set	g_sf,		0xFE114		; uint32_t
	.set	g_sum,		0xFE118		; uint16_t
	.set	g_b8,		0xFE11A		; uint8_t[8]
	.set	g_m_a,		0xFE122		; uint8_t
	.set	g_m_d,		0xFE123		; uint8_t
	.set	g_m_b,		0xFE124		; uint32_t
	.set	g_m_c,		0xFE128		; uint16_t
	.set	g_fp,		0xFE12A		; uint8_t __far *
	.set	g_stub_val,	0xFE12E		; uint16_t
	.set	TEST_time_us,	0xFE130		; uint32_t
	.set	g_ms_tick,	0xFE134		; uint32_t
	.set	TEST_cycle_cnt,	0xFE138		; uint16_t
	.set	s_i,		0xFE13A		; TEST_drv.c のファイル内 static uint16_t
	.set	sub_cnt,	0xFE13C		; TEST_cycle 内の static uint8_t sub
	.set	g_isr_cnt,	0xFE13D		; uint8_t
	.set	g_st,		0xFE13E		; struct S（6 バイト）
	.set	g_w16,		0xFE144		; struct W（2 バイト、16 ビットのビットフィールド）
	.set	eng_s_cnt,	0xFE146		; engine.c の static uint8_t s_cnt
	.set	sen_s_cnt,	0xFE147		; sensor.c の static uint8_t s_cnt
	.set	calls,		0xFE148		; cnt_call 内の static uint16_t calls
	.set	gp_st,		0xFE14A		; struct S *

	.set	ADCR,		0xFFF1E		; SFR（読むだけ）
	.set	MDAL,		0xFFFF0
	.set	MDAH,		0xFFFF2
	.set	MDBL,		0xFFFF6

	.text
	.global	_start
_vect:
	.hword	_start			; リセットベクタ

	.org	0x100
;---------------------------------------------------------------- スタートアップ
_start:
	movw	sp, #0xFE20		; SP = FFE20H
	; bss を 0 に
	movw	hl, #BSS & 0xFFFF
.Lbss:
	movw	ax, hl
	cmpw	ax, #BSS_END & 0xFFFF
	bz	$.Lbss_done
	mov	[hl+0], #0
	incw	hl
	br	$.Lbss
.Lbss_done:
	; .data（ROM 0x01800）を .dataR へ
	mov	es, #0
	movw	bc, #DATA_SIZE
.Ldata:
	clrw	ax
	cmpw	ax, bc
	bz	$.Ldata_done
	decw	bc
	mov	a, es:0x1800[bc]
	mov	DATAR & 0xFFFF[bc], a
	br	$.Ldata
.Ldata_done:
	; .sdata（ROM 0x01840）を .sdataR へ
	movw	bc, #SDATA_SIZE
.Lsdata:
	clrw	ax
	cmpw	ax, bc
	bz	$.Lsdata_done
	decw	bc
	mov	a, es:0x1840[bc]
	mov	SDATAR & 0xFFFF[bc], a
	br	$.Lsdata
.Lsdata_done:
	call	!!_main
_exit:
	br	$_exit

	.global	_main
_main:					; void main(void) { for (;;) ; }
	br	$_main

;---------------------------------------------------------------- 引数と戻り値
	.global	_f_mix
_f_mix:					; uint16_t f_mix(uint8_t a /*A*/, uint16_t b /*BC*/, uint8_t c /*X*/) { g_a=a; g_c=c; g_b=b; return b+1; }
	mov	!g_a, a
	mov	a, x
	mov	!g_c, a
	movw	ax, bc
	movw	!g_b, ax
	incw	ax
	ret

	.global	_f_long
_f_long:				; uint32_t f_long(uint32_t x /*BC-AX*/, uint16_t y /*DE*/) { g_l=x; g_w=y; return x+y; }
	movw	!g_l, ax
	xchw	ax, bc
	movw	!g_l+2, ax
	xchw	ax, bc
	movw	ax, de
	movw	!g_w, ax
	movw	ax, !g_l
	addw	ax, de
	sknc
	incw	bc
	ret

	.global	_f_stack
_f_stack:				; uint16_t f_stack(uint16_t a /*AX*/, uint16_t b /*BC*/, uint16_t c /*DE*/,
					;                  uint16_t d /*[SP+4]*/, uint8_t e /*[SP+6]*/, uint32_t f /*[SP+8]*/) { ...; return d; }
	movw	!g_sa, ax
	movw	ax, bc
	movw	!g_sb, ax
	movw	ax, de
	movw	!g_sc, ax
	movw	ax, [sp+4]
	movw	!g_sdd, ax
	mov	a, [sp+6]
	mov	!g_se, a
	movw	ax, [sp+8]
	movw	!g_sf, ax
	movw	ax, [sp+10]
	movw	!g_sf+2, ax
	movw	ax, [sp+4]
	ret

	.global	_f_args8
_f_args8:				; uint8_t f_args8(uint8_t a1..a8): A, X, C, B, E, D, [SP+4], [SP+6]。g_b8[i] = ai; return a8;
	mov	!g_b8+0, a
	mov	a, x
	mov	!g_b8+1, a
	mov	a, c
	mov	!g_b8+2, a
	mov	a, b
	mov	!g_b8+3, a
	mov	a, e
	mov	!g_b8+4, a
	mov	a, d
	mov	!g_b8+5, a
	mov	a, [sp+4]
	mov	!g_b8+6, a
	mov	a, [sp+6]
	mov	!g_b8+7, a
	ret

	.global	_f_mix2
_f_mix2:				; void f_mix2(uint8_t a /*A*/, uint32_t b /*DE-BC*/, uint16_t c /*[SP+4]*/, uint8_t d /*X*/)
	mov	!g_m_a, a
	mov	a, x
	mov	!g_m_d, a
	movw	ax, bc
	movw	!g_m_b, ax
	movw	ax, de
	movw	!g_m_b+2, ax
	movw	ax, [sp+4]
	movw	!g_m_c, ax
	ret

	.global	_f_far_read
_f_far_read:				; uint8_t f_far_read(const uint8_t __far *p /*A-DE*/) { return *p; }
	mov	es, a
	mov	a, es:[de]
	ret

	.global	_f_far_write
_f_far_write:				; uint8_t __far *f_far_write(uint8_t __far *p /*A-DE*/, uint8_t v /*X*/) { *p = v; return p; }
	mov	es, a
	mov	a, x
	mov	es:[de], a
	mov	a, es
	ret

	.global	_f_far2
_f_far2:				; void f_far2(uint8_t a /*A*/, uint16_t w /*BC*/, uint8_t __far *p /*X-DE*/) { g_m_a=a; g_m_c=w; g_fp=p; }
	mov	!g_m_a, a
	mov	a, x			; ページ（X）を先に。AX を使うと X が壊れる
	mov	!g_fp+2, a
	movw	ax, bc
	movw	!g_m_c, ax
	movw	ax, de
	movw	!g_fp, ax
	ret

	.global	_f_far3
_f_far3:				; void f_far3(uint16_t w1 /*AX*/, uint16_t w2 /*BC*/, uint8_t __far *p /*[SP+4]*/) { g_sa=w1; g_sb=w2; g_fp=p; }
	movw	!g_sa, ax
	movw	ax, bc
	movw	!g_sb, ax
	movw	ax, [sp+4]
	movw	!g_fp, ax
	mov	a, [sp+6]
	mov	!g_fp+2, a
	ret

	.global	_f_sum
_f_sum:					; uint16_t f_sum(const uint8_t *p /*AX*/, uint8_t n /*C*/)
	movw	hl, ax
	clrw	ax
	movw	!g_sum, ax
	mov	a, c
	mov	b, a
.Lsum:
	mov	a, b
	cmp0	a
	bz	$.Lsum_done
	mov	a, [hl]
	mov	x, a
	clrb	a
	addw	ax, !g_sum
	movw	!g_sum, ax
	incw	hl
	dec	b
	br	$.Lsum
.Lsum_done:
	movw	ax, !g_sum
	ret

	.global	_f_neg
_f_neg:					; int8_t f_neg(void) { return -3; }
	mov	a, #0xFD
	ret

	.global	_f_float
_f_float:				; float f_float(void) { return 1.5f; }  BC-AX = 0x3FC00000
	movw	bc, #0x3FC0
	clrw	ax
	ret

	.global	_f_ll
_f_ll:					; long long f_ll(void) { return 0x0123456789ABCDEF; }  戻り値の領域へのポインタが AX
	movw	hl, ax
	movw	ax, #0xCDEF
	movw	[hl], ax
	movw	ax, #0x89AB
	movw	[hl+2], ax
	movw	ax, #0x4567
	movw	[hl+4], ax
	movw	ax, #0x0123
	movw	[hl+6], ax
	ret

	.global	_f_ptr_struct
_f_ptr_struct:				; uint16_t f_ptr_struct(const struct S *p /*AX*/) { return p->w; }
	movw	de, ax
	movw	ax, [de+2]
	ret

	.global	_f_bits
_f_bits:				; void f_bits(void) { g_out = g_st.fb; }  fa:3（bit0-2）、fb:4（bit3-6）、fc:1（bit7）
	mov	a, !g_st
	shr	a, 3
	and	a, #0x0F
	mov	!g_out, a
	ret

;---------------------------------------------------------------- 乗除算
	.global	_f_mulhu
_f_mulhu:				; uint32_t f_mulhu(uint16_t a /*AX*/, uint16_t b /*BC*/) { return (uint32_t)a * b; }（S3 の MULHU）
	mulhu
	ret

	.global	_f_mdu
_f_mdu:					; uint16_t f_mdu(uint16_t a, uint16_t b)  乗除算器 SFR（MDUC = 0: 符号なし乗算）の下位 16 ビット
	movw	!MDAL, ax
	movw	ax, bc
	movw	!MDAH, ax
	movw	ax, !MDBL
	ret

;---------------------------------------------------------------- スタブ
	.global	_read_io
_read_io:				; uint16_t read_io(void) { return ADCR; }
	movw	ax, !ADCR
	ret

	.global	_AMSTB_read_io
_AMSTB_read_io:				; uint16_t AMSTB_read_io(void) { return g_stub_val; }
	movw	ax, !g_stub_val
	ret

	.global	_use_io
_use_io:				; uint16_t use_io(void) { return read_io() + 1; }
	call	!!_read_io
	incw	ax
	ret

;---------------------------------------------------------------- 10us インナーループ型のテストドライバ
	.global	_TEST_cycle
_TEST_cycle:				; void TEST_cycle(void): 1000 回 { TEST_time_us += 10; 100 回ごとに intr_tm00(); } TEST_cycle_cnt++;
	clrw	ax
	movw	!s_i, ax
.Lcyc:
	movw	ax, !TEST_time_us
	addw	ax, #10
	movw	!TEST_time_us, ax
	bnc	$.Lcyc_nc
	incw	!TEST_time_us+2
.Lcyc_nc:
	inc	!sub_cnt
	cmp	!sub_cnt, #100
	bnz	$.Lcyc_no
	mov	!sub_cnt, #0
	call	!!_intr_tm00
.Lcyc_no:
	incw	!s_i
	movw	ax, !s_i
	cmpw	ax, #1000
	bnz	$.Lcyc
	incw	!TEST_cycle_cnt
	ret

	.global	_intr_tm00
_intr_tm00:				; void intr_tm00(void) { g_ms_tick++; }
	movw	ax, !g_ms_tick
	addw	ax, #1
	movw	!g_ms_tick, ax
	bnc	$.Ltm_nc
	incw	!g_ms_tick+2
.Ltm_nc:
	ret

;---------------------------------------------------------------- static 変数
	.global	_cnt_call
_cnt_call:				; uint16_t cnt_call(void) { static uint16_t calls; return ++calls; }
	incw	!calls
	movw	ax, !calls
	ret

	.global	_engine_inc
_engine_inc:				; engine.c: void engine_inc(void) { s_cnt++; }
	inc	!eng_s_cnt
	ret

	.global	_sensor_inc
_sensor_inc:				; sensor.c: void sensor_inc(void) { s_cnt += 2; }
	inc	!sen_s_cnt
	inc	!sen_s_cnt
	ret

;---------------------------------------------------------------- 割り込みハンドラ（RETI で戻る）
	.global	_intr_reti
_intr_reti:				; #pragma interrupt: void intr_reti(void) { g_isr_cnt++; }
	inc	!g_isr_cnt
	reti

;---------------------------------------------------------------- 異常系
	.global	_f_loop
_f_loop:				; void f_loop(void) { for (;;) ; }
	br	$_f_loop

	.global	_f_undef
_f_undef:				; 未定義命令
	.byte	0x61, 0x19
	ret

	.global	_f_halt
_f_halt:				; void f_halt(void) { HALT(); g_out = 0x77; }
	halt
	mov	!g_out, #0x77
	ret
_code_end:

;---------------------------------------------------------------- ROM のデータ
	.org	0x1800
_data_rom:				; .data の初期値（.dataR 0xFE000 へコピーされる）
	.hword	0x1234			; g_init16
	.byte	0x56			; g_init8
	.byte	1, 2, 3			; g_initarr
	.hword	0xE003			; gp_init = &g_initarr（near）
	.hword	0x2000, 0x0000		; gfp_init = (const uint8_t __far *)0x02000
_data_rom_end:
	.org	0x1840
_sdata_rom:
	.hword	0x5A5A			; g_sd
_sdata_rom_end:

	.org	0x2000
_const_rom:				; .const
	.hword	100, 200, 400, 800	; const uint16_t g_lut[4]
	.ascii	"RL78SIM\0"		; const char g_name[8]
_const_end:
