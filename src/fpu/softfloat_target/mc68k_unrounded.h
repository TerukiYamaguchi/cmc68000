/*
 * mc68k_unrounded.h — softfloat_roundPackToExtF80() に渡った丸め前の中間結果。
 *
 * 自前の roundPack は丸めを通る全経路（add/sub/mul/div/sqrt/sglmul/sgldiv、move、to_s/to_d/to_x
 * など）の合流点なので、 最後の呼出し 1 回分の入力をここへ保持する。演算の結果・EXC/AEXC/CC
 * には影響しない（書込みだけ）。公開 API は include/mc68000/fpu.h の
 * mc68k_fpu_unrounded_*（fpu_internal.c）。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "softfloat.h"

struct mc68k_unrounded {
    bool valid;
    bool sign;
    int32_t exp;  /* idealized 指数（指数フィールド 0・整数部 1 が 0。非正規化域は負） */
    uint64_t sig; /* 64 bit 仮数。bit63 = 1 */
    uint64_t
        sig_extra; /* 仮数の下の 64 bit（bit63 = guard、bit62 = round、残りの OR = sticky） */
    uint8_t mantissa_bits; /* 丸め先の仮数の有効ビット数（64 / 53 / 24） */
    uint8_t mode;          /* softfloat_round_* */
};

extern THREAD_LOCAL struct mc68k_unrounded mc68k_last_unrounded;

/* 指数の範囲を制限せずに仮数だけを mantissa_bits へ丸める（桁上がりは exp に反映）。sig_extra
 * は 0 になる。 */
void mc68k_unrounded_round_mantissa(struct mc68k_unrounded *u);
