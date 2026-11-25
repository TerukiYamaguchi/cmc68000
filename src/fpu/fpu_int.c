/*
 * fpu_int.c — FINT / FINTRZ。
 *
 * 整数へ丸めた値は拡張精度の浮動小数点数として返し、PREC
 * による丸めはかけない（暫定決定。U17）。 OPERR・OVFL・UNFL は常にクリア。INEX2
 * は整数化で値が変わったとき。
 */
#include <stdbool.h>
#include <stdint.h>
#include "fpu_internal.h"

#define EXP_BIAS_X 16383
#define SIG_BITS   64

/* 端数（捨てる部分）と、保持する最下位ビットから、大きさを 1 つ上げるか。 */
static bool round_up(uint8_t mode, bool sign, int half_cmp, bool inexact, bool odd) {
    switch (mode) {
    case MC68K_FPU_ROUND_ZERO:
        return false;
    case MC68K_FPU_ROUND_MINUS_INF:
        return sign && inexact;
    case MC68K_FPU_ROUND_PLUS_INF:
        return !sign && inexact;
    default:
        return half_cmp > 0 || (half_cmp == 0 && odd);
    }
}

static mc68k_fp80_t round_to_int(mc68k_fp80_t src, uint8_t mode, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;
    bool sign = fpu_sign(src);
    bool inexact, up;
    int half_cmp, u, frac_bits;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_inf(src) || fpu_is_zero(src)) {
        r = fpu_is_zero(src) ? fpu_make(sign, 0, 0) : src;
        fpu_finish_result(st, r, 0, 0);
        return r;
    }
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    u = exp - EXP_BIAS_X;    /* 先頭ビットの重み 2^u */
    if (u >= SIG_BITS - 1) { /* 全ビットが整数部 */
        r = fpu_make(sign, (uint16_t)exp, sig);
        fpu_finish_result(st, r, 0, 0);
        return r;
    }
    if (u < 0) {             /* |x| < 1: 結果は 0 か ±1 */
        bool half = u == -1; /* |x| >= 0.5 */
        half_cmp = !half ? -1 : ((sig << 1) != 0 ? 1 : 0);
        up = round_up(mode, sign, half_cmp, true, false);
        r = up ? fpu_make(sign, EXP_BIAS_X, FPU_SIG_J) : fpu_make(sign, 0, 0);
        fpu_finish_result(st, r, (uint8_t)MC68K_FPU_EXC_INEX2, 0);
        return r;
    }
    frac_bits = SIG_BITS - 1 - u; /* 1..63 */
    {
        uint64_t frac_mask = (UINT64_C(1) << frac_bits) - 1;
        uint64_t frac = sig & frac_mask;
        uint64_t half_bit = UINT64_C(1) << (frac_bits - 1);

        inexact = frac != 0;
        half_cmp = frac > half_bit ? 1 : (frac == half_bit ? 0 : -1);
        up = round_up(mode, sign, half_cmp, inexact, ((sig >> frac_bits) & 1u) != 0);
        sig &= ~frac_mask;
        if (up) {
            sig += UINT64_C(1) << frac_bits;
            if (sig == 0) {
                sig = FPU_SIG_J;
                exp++;
            }
        }
    }
    r = fpu_make(sign, (uint16_t)exp, sig);
    fpu_finish_result(st, r, inexact ? (uint8_t)MC68K_FPU_EXC_INEX2 : 0, 0);
    return r;
}

mc68k_fp80_t mc68k_fpu_int(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return round_to_int(src, st->fpcr_round_mode, st);
}

mc68k_fp80_t mc68k_fpu_intrz(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return round_to_int(src, MC68K_FPU_ROUND_ZERO, st);
}
