/*
 * fpu040.h — MC68040 内蔵 FPU の CPU 内部状態。
 *
 * 68881/68882 の CIR ダイアログ状態機械とは意図的に分離する。共通コア状態と、FSAVE まで
 * 残る例外状態（算術例外・BSUN・未対応データ型。未実装命令は次の報告可能命令まで）を持つ。命令の実行と
 * FSAVE/FRESTORE は ops_fpu.c。
 */
#pragma once

#include <string.h>

#include "mc68000/fpu.h"

/* 例外状態。FSAVE が消すまで、例外を報告できる FP 命令は
 * 同じ vector を pre-instruction で報告し続ける（[040UM §9.8]）。ただし unimpl（未実装命令）の
 * 状態は報告せず、次に例外を報告できる FP
 * 命令（未実装命令を含む）が実行を始めるときに捨てる。フィールドは FSAVE の busy/unimplemented
 * frame に載せる値。導出できない内部状態（CU_SAVEPC 等）は持たない。 */
typedef struct {
    bool valid;
    uint8_t vector;
    bool unimpl; /* true: unimplemented-instruction frame（$30）、false: busy frame（$60） */
    bool e1, e3,
        t; /* e3: FADD/FSUB/FMUL/FDIV/FSQRT 系の OVFL/UNFL/INEX2（WBTEMP を持つ。設計書 §6） */
    uint16_t cmdreg1b;
    uint16_t cmdreg3b;   /* e3 のときだけ。CMDREG1B の並べ替え（設計書 推論 19） */
    mc68k_fp80_t wbtemp; /* e3 のときだけ。符号と biased 指数の下位 15 bit、64 bit
                            仮数（WBTS/WBTE/WBTM） */
    bool wbte15, wbtm_guard, wbtm_round,
        wbtm_sticky;    /* WBTM66 は常に 0（設計書 推論 22）で持たない */
    uint8_t stag, dtag; /* 3 bit のデータ型タグ（[040UM §9.8 STAG/DTAG]） */
    mc68k_fp80_t etemp, fptemp;
    mc68k_fp80_t operand060; /* 060: EXCP frame の exception operand（FRESTORE で戻した値を
                                FSAVE が書く） */
} mc68k_fpu040_exc_t;

typedef struct {
    mc68k_fpu_state_t state;
    mc68k_fpu040_exc_t exc;
    /* 非条件 FP 命令を 1 つ以上実行したか。FSAVE が null か idle かを決める（[040UM §9.8]）。
     */
    bool idle;
} mc68k_fpu040_t;

static inline void mc68k_fpu040_reset(mc68k_fpu040_t *fpu) {
    mc68k_fpu_reset(&fpu->state);
    memset(&fpu->exc, 0, sizeof(fpu->exc));
    fpu->idle = false;
}
