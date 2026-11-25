/*
 * fpu_transcend_hyp.c — FSINH / FCOSH / FTANH / FATANH。
 * |x| < 0.5 は Taylor 展開（全項が正で桁落ちなし）、それ以上は e^|x| から求める。
 */
#include "fpu_wide.h"

#define SMALL_EXP (WIDE_BIAS - 1) /* |x| < 0.5 ⇔ exp < SMALL_EXP */
#define TANH_ONE_EXP                                                                 \
    (WIDE_BIAS + 6)       /* |x| >= 64: tanh は 1 - 2^-128 を丸める（真値と 1 の差 \
                             2e^(-2|x|)/(1+e^(-2|x|)) は 2^-183 未満） */
#define INV_TERM_MAX_K 62 /* k がこれを超えると e^-|x| の項は 2^-124 未満で無視できる */
#define ATANH_SERIES_EXP \
    (WIDE_BIAS - 3) /* |x| < 0.125 は級数、それ以上は 1/2 ln((1+x)/(1-x)) */

/* e^|x| = E·2^k から sinh/cosh の (E ∓ 2^-2k/E)。結果 = 値·2^(k-1)。 */
static wide_t exp_pair(wide_t ax, bool minus, int32_t *k) {
    wide_t e;

    wide_exp(ax, &e, k);
    if (*k > INV_TERM_MAX_K)
        return e;
    {
        wide_t inv = w_scale(w_div(w_one(), e), -2 * *k);

        return minus ? w_sub(e, inv) : w_add(e, inv);
    }
}

/* ax > 0（指数フィールド >= 1）の sinh。結果は値·2^*k。 */
wide_t wide_sinh(wide_t ax, int32_t *k) {
    if (w_exp(ax) < SMALL_EXP) {
        *k = 0;
        return w_mul(ax, wide_poly(w_sinh_coef, 16, w_mul(ax, ax)));
    }
    {
        wide_t res = exp_pair(ax, true, k);

        (*k)--;
        return res;
    }
}

wide_t wide_cosh(wide_t ax, int32_t *k) {
    if (w_exp(ax) < SMALL_EXP) {
        *k = 0;
        return wide_poly(w_cosh_coef, 16, w_mul(ax, ax));
    }
    {
        wide_t res = exp_pair(ax, false, k);

        (*k)--;
        return res;
    }
}

/* tanh ax（0 < ax、|x| < 64）。 */
wide_t wide_tanh(wide_t ax) {
    int32_t k;
    wide_t e, t;

    if (w_exp(ax) < SMALL_EXP) {
        wide_t x2 = w_mul(ax, ax);

        return w_div(w_mul(ax, wide_poly(w_sinh_coef, 16, x2)), wide_poly(w_cosh_coef, 16, x2));
    }
    wide_exp(w_neg(w_scale(ax, 1)), &e, &k); /* e^(-2|x|) */
    t = w_scale(e, k);
    return w_div(w_sub(w_one(), t), w_add(w_one(), t));
}

/* atanh ax（0 < ax < 1）。 */
wide_t wide_atanh(wide_t ax) {
    if (w_exp(ax) < ATANH_SERIES_EXP)
        return w_mul(ax, wide_poly(w_odd_recip, 31, w_mul(ax, ax)));
    return w_scale(wide_ln(w_div(w_add(w_one(), ax), w_sub(w_one(), ax))), -1);
}

mc68k_fp80_t mc68k_fpu_sinh(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp, k;
    uint64_t sig;
    bool sign = fpu_sign(src);
    wide_t res;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src) || fpu_is_inf(src))
        return fpu_exact_result(st, src);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp < WIDE_TINY_EXP)
        return fpu_tiny_result(st, sign, exp, sig, false, 0);
    w_begin();
    res = wide_sinh(w_make(false, exp, sig, 0), &k);
    if (sign)
        res = w_neg(res);
    return fpu_wide_finish(st, res, k, false, 0);
}

mc68k_fp80_t mc68k_fpu_cosh(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp, k;
    uint64_t sig;
    wide_t res;
    const uint8_t force = (uint8_t)MC68K_FPU_EXC_UNFL;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src))
        return fpu_exact_result(st, fpu_make(false, WIDE_BIAS, FPU_SIG_J));
    if (fpu_is_inf(src))
        return fpu_exact_result(st, fpu_make(false, FPU_EXP_MASK, FPU_SIG_J));
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp < WIDE_TINY_EXP)
        return fpu_near_one_result(st, false, true, force);
    w_begin();
    res = wide_cosh(w_make(false, exp, sig, 0), &k);
    return fpu_wide_finish(st, res, k, false, force);
}

mc68k_fp80_t mc68k_fpu_tanh(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;
    bool sign = fpu_sign(src);
    wide_t res;
    const uint8_t force = (uint8_t)MC68K_FPU_EXC_OVFL;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src))
        return fpu_exact_result(st, src);
    if (fpu_is_inf(src))
        return fpu_exact_result(st, fpu_make(sign, WIDE_BIAS, FPU_SIG_J));
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp < WIDE_TINY_EXP)
        return fpu_tiny_result(st, sign, exp, sig, true, force);
    if (exp >= TANH_ONE_EXP)
        return fpu_near_one_result(st, sign, false, force);
    w_begin();
    res = wide_tanh(w_make(false, exp, sig, 0));
    if (sign)
        res = w_neg(res);
    return fpu_wide_finish_below_one(st, res, force); /* |tanh x| < 1 */
}

mc68k_fp80_t mc68k_fpu_atanh(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;
    bool sign = fpu_sign(src);
    wide_t res;
    const uint8_t force = (uint8_t)MC68K_FPU_EXC_OVFL;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_inf(src))
        return fpu_operr_result(st);
    if (fpu_is_zero(src))
        return fpu_exact_result(st, src);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp > WIDE_BIAS || (exp == WIDE_BIAS && sig > FPU_SIG_J))
        return fpu_operr_result(st); /* |x| > 1 */
    if (exp == WIDE_BIAS &&
        sig == FPU_SIG_J) { /* x = ±1 → ±Inf、DZ（符号は数学的な向き。§5.2） */
        r = fpu_make(sign, FPU_EXP_MASK, FPU_SIG_J);
        fpu_finish_result(st, r, (uint8_t)MC68K_FPU_EXC_DZ, 0);
        return r;
    }
    if (exp < WIDE_TINY_EXP)
        return fpu_tiny_result(st, sign, exp, sig, false, force);
    w_begin();
    res = wide_atanh(w_make(false, exp, sig, 0));
    if (sign)
        res = w_neg(res);
    return fpu_wide_finish(st, res, 0, false, force);
}
