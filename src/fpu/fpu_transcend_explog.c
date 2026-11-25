/*
 * fpu_transcend_explog.c — FETOX / FLOGN / FLOG2 / FLOG10。
 */
#include "fpu_wide.h"

#define ETOX_TINY_EXP \
    (WIDE_BIAS -      \
     100) /* |x| < 2^-100 は 1 ± x の方向だけが要る（線形項のため 2^-40 では足りない） */
#define POW10_EXACT_MAX 27 /* 10^27 = 5^27·2^27 で 5^27 < 2^64 なので拡張精度で厳密 */

mc68k_fp80_t mc68k_fpu_etox(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp, k;
    uint64_t sig;
    bool sign = fpu_sign(src);
    wide_t x, mant;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src))
        return fpu_exact_result(st, fpu_make(false, WIDE_BIAS, FPU_SIG_J));
    if (fpu_is_inf(src))
        return fpu_exact_result(st, sign ? fpu_make(false, 0, 0) : src);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp < ETOX_TINY_EXP)
        return fpu_near_one_result(st, false, !sign, 0);
    w_begin();
    x = w_make(sign, exp, sig, 0);
    wide_exp(x, &mant, &k);
    return fpu_wide_finish(st, mant, k, false, 0);
}

/* x = 10^n（n = 1..27）か。一致したら n、そうでなければ 0。 */
static int pow10_exponent(int32_t exp, uint64_t sig) {
    uint64_t p = 1;

    for (int n = 1; n <= POW10_EXACT_MAX; n++) {
        int shift;

        p *= 5;
        shift = fpu_clz64(p);
        if (sig == (p << shift) && exp == WIDE_BIAS + 63 - shift + n)
            return n;
    }
    return 0;
}

/* log2(m·2^u) = u' + ln(m')·log2 e。m は指数フィールド BIAS の値。 */
wide_t wide_log2_mu(wide_t m, int32_t u) {
    wide_t frac = wide_ln_frac(m, &u);

    return w_add(w_from_int(u), w_mul(frac, w_inv_ln2));
}

wide_t wide_log10_mu(wide_t m, int32_t u) {
    return w_mul(wide_ln_mu(m, u), w_inv_ln10);
}

enum log_kind { LOG_E, LOG_2, LOG_10 };

static mc68k_fp80_t logarithm(mc68k_fp80_t src, mc68k_fpu_state_t *st, enum log_kind kind) {
    mc68k_fp80_t r;
    int32_t exp, u;
    uint64_t sig;
    wide_t m, res;
    int n;
    const uint8_t force = (uint8_t)(MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_UNFL);

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src)) { /* ±0 → -Inf、DZ */
        r = fpu_make(true, FPU_EXP_MASK, FPU_SIG_J);
        fpu_finish_result(st, r, (uint8_t)MC68K_FPU_EXC_DZ, 0);
        return r;
    }
    if (fpu_sign(src))
        return fpu_operr_result(st); /* 負値（-Inf を含む） */
    if (fpu_is_inf(src))
        return fpu_exact_result(st, src);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    u = exp - WIDE_BIAS;
    if (exp == WIDE_BIAS && sig == FPU_SIG_J)
        return fpu_exact_result(st, fpu_make(false, 0, 0)); /* ln 1 = 0 */
    if (kind == LOG_2 && sig == FPU_SIG_J)
        return fpu_exact_result(st, mc68k_fpu_from_l(u)); /* log2(2^n) = n */
    if (kind == LOG_10 && (n = pow10_exponent(exp, sig)) != 0) {
        return fpu_exact_result(st, mc68k_fpu_from_l(n)); /* log10(10^n) = n */
    }
    w_begin();
    m = w_make(false, WIDE_BIAS, sig, 0);
    res = kind == LOG_2    ? wide_log2_mu(m, u)
          : kind == LOG_10 ? wide_log10_mu(m, u)
                           : wide_ln_mu(m, u);
    return fpu_wide_finish(st, res, 0, false, force);
}

mc68k_fp80_t mc68k_fpu_logn(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return logarithm(src, st, LOG_E);
}
mc68k_fp80_t mc68k_fpu_log2(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return logarithm(src, st, LOG_2);
}
mc68k_fp80_t mc68k_fpu_log10(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return logarithm(src, st, LOG_10);
}
