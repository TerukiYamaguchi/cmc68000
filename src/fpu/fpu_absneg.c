/*
 * fpu_absneg.c — FABS/FNEG共通コア。
 * 公開APIは include/mc68000/fpu.h、設計は docs/mc68000-internals/reference/fpu.md。
 *
 * 符号を操作した値を mc68k_fpu_move() と同じ経路（normalize()→差し替え済みroundPack、
 * roundingPrecisionはfpcr_round_prec）に通して丸める（[881UM §2.2.2]の"any operation"。
 * 第3回Opusレビュー blocking C1、ユーザー決定: 案1）。NaN・±Infは符号操作のみ（丸めを
 * 経由しない）。NaNはSNaNをquiet化してから符号を操作する。±0は符号操作した正規のゼロ。
 * EXC byteのマスク（§5.4のFABS/FNEG表）: SNANは検出、UNFLはmove()と同じ規則
 * （sourceが拡張精度の非正規化数か。destinationのPRECには依らない）、OVFL/INEX2は
 * roundPackの検出結果を使わず常にクリア。BSUN/OPERR/DZ/INEX1もクリア。
 */
#include <stdbool.h>
#include <stdint.h>
#include "fpu_internal.h"

static mc68k_fp80_t abs_or_neg(mc68k_fp80_t src, mc68k_fpu_state_t *st, bool force_positive) {
    mc68k_fp80_t r;
    bool new_sign = force_positive ? false : !fpu_sign(src);
    uint8_t exc = 0;

    if (fpu_is_nan(src)) {
        if (fpu_is_snan(src))
            exc = (uint8_t)MC68K_FPU_EXC_SNAN;
        r = fpu_make(new_sign, fpu_exp_field(src), fpu_quiet_signif(src.signif));
    } else if (fpu_is_inf(src)) {
        r = fpu_make(new_sign, fpu_exp_field(src), src.signif);
    } else if (fpu_is_zero(src)) {
        r = fpu_make(new_sign, 0, 0);
    } else {
        uint_fast8_t flags;

        if (fpu_src_is_x_denormal(src))
            exc = (uint8_t)MC68K_FPU_EXC_UNFL;
        r = fpu_round_finite(fpu_make(new_sign, fpu_exp_field(src), src.signif), st,
                             fpu_rounding_precision(st->fpcr_round_prec), &flags);
        (void)flags; /* INEX2/OVFLは常にクリア（design §5.4）。roundPackの検出は使わない */
    }
    fpu_commit_exc(st, exc);
    fpu_set_cc(st, r);
    return r;
}

mc68k_fp80_t mc68k_fpu_abs(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return abs_or_neg(src, st, true);
}

mc68k_fp80_t mc68k_fpu_neg(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return abs_or_neg(src, st, false);
}
