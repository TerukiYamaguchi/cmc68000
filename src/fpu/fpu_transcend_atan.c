/*
 * fpu_transcend_atan.c — FATAN / FASIN / FACOS。
 * asin x = atan(x / √((1-x)(1+x)))、acos x = 2 atan(√((1-x)/(1+x)))。
 */
#include "fpu_wide.h"

#define ACOS_TINY_EXP                                                         \
    (WIDE_BIAS - 70) /* |x| がこれ未満なら acos x は π/2（差は 2^-70 以下） \
                      */

/* asin ax（0 < ax < 1）。 */
wide_t wide_asin(wide_t ax) {
    wide_t den = w_sqrt(w_mul(w_sub(w_one(), ax), w_add(w_one(), ax)));

    return wide_atan(w_div(ax, den));
}

/* acos x（|x| < 1、符号つき）。 */
wide_t wide_acos(wide_t x) {
    return w_scale(wide_atan(w_sqrt(w_div(w_sub(w_one(), x), w_add(w_one(), x)))), 1);
}

/* |x| と 1 の比較: -1 未満、0 等しい、1 より大 */
static int cmp_one(int32_t exp, uint64_t sig) {
    if (exp < WIDE_BIAS)
        return -1;
    if (exp > WIDE_BIAS || sig > FPU_SIG_J)
        return 1;
    return 0;
}

mc68k_fp80_t mc68k_fpu_atan(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;
    bool sign = fpu_sign(src);
    const uint8_t force = (uint8_t)MC68K_FPU_EXC_OVFL;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src))
        return fpu_exact_result(st, fpu_make(sign, 0, 0));
    if (fpu_is_inf(src))
        return fpu_const_result(st, &c128_pi_2, sign, force);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp < WIDE_TINY_EXP)
        return fpu_tiny_result(st, sign, exp, sig, true, force);
    w_begin();
    {
        wide_t res = wide_atan(w_make(false, exp, sig, 0));

        if (sign)
            res = w_neg(res);
        return fpu_wide_finish(st, res, 0, false, force);
    }
}

mc68k_fp80_t mc68k_fpu_asin(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;
    bool sign = fpu_sign(src);
    int c;
    const uint8_t force = (uint8_t)(MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_UNFL);

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_inf(src))
        return fpu_operr_result(st);
    if (fpu_is_zero(src))
        return fpu_exact_result(st, fpu_make(sign, 0, 0));
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    c = cmp_one(exp, sig);
    if (c > 0)
        return fpu_operr_result(st);
    if (c == 0)
        return fpu_const_result(st, &c128_pi_2, sign, force);
    if (exp < WIDE_TINY_EXP)
        return fpu_tiny_result(st, sign, exp, sig, false, force);
    w_begin();
    {
        wide_t res = wide_asin(w_make(false, exp, sig, 0));

        if (sign)
            res = w_neg(res);
        return fpu_wide_finish(st, res, 0, false, force);
    }
}

mc68k_fp80_t mc68k_fpu_acos(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;
    bool sign = fpu_sign(src);
    int c;
    const uint8_t force = (uint8_t)(MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_UNFL);

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_inf(src))
        return fpu_operr_result(st);
    if (fpu_is_zero(src))
        return fpu_const_result(st, &c128_pi_2, false, force);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    c = cmp_one(exp, sig);
    if (c > 0)
        return fpu_operr_result(st);
    if (c == 0) {
        if (sign)
            return fpu_const_result(st, &c128_pi, false, force); /* acos(-1) = π */
        return fpu_exact_result(st, fpu_make(false, 0, 0));      /* acos(1) = +0 */
    }
    if (exp < ACOS_TINY_EXP)
        return fpu_const_result(st, &c128_pi_2, false, force);
    w_begin();
    return fpu_wide_finish(st, wide_acos(w_make(sign, exp, sig, 0)), 0, false, force);
}
