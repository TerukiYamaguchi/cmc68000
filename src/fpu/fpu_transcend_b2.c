/*
 * fpu_transcend_b2.c — FETOXM1 / FLOGNP1 / FTWOTOX / FTENTOX / FSINCOS。
 * float128 の中間値を 1 回だけ丸める（B1 と同じ方式）。
 */
#include "fpu_wide.h"

#define TINY_EXP_ONE_SIDED \
    (WIDE_BIAS -           \
     100) /* |x| < 2^-100 は補正項の向きだけが要る（線形項があるため 2^-40 では足りない） */
#define EXACT_INT_BITS 63 /* 拡張精度の仮数の小数部ビット数（整数判定に使う） */
#define POW2_INT_LOG2 \
    16 /* 2^x の x が整数で |x| >= 2^16 なら必ずオーバーフロー/アンダーフロー */
#define POW2_SATURATE_N 100000
#define POW10_EXACT_MAX 27 /* 10^27 = 5^27·2^27 で 5^27 < 2^64 なので拡張精度で厳密 */
#define POW10_INT_LOG2  5  /* 27 < 2^5 */

mc68k_fp80_t mc68k_fpu_etoxm1(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp, k;
    uint64_t sig;
    bool sign = fpu_sign(src);
    bool near_one;
    wide_t res;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src))
        return fpu_exact_result(st, fpu_make(sign, 0, 0));
    if (fpu_is_inf(src))
        return fpu_exact_result(st, sign ? fpu_make(true, WIDE_BIAS, FPU_SIG_J) : src);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp < TINY_EXP_ONE_SIDED)
        return fpu_tiny_result(st, sign, exp, sig, sign, 0); /* x + x²/2 */
    w_begin();
    res = wide_expm1(w_make(sign, exp, sig, 0), &k, &near_one);
    if (near_one)
        return fpu_near_one_result(st, true, false, 0); /* -1 + 1.42·2^-120 以下 */
    if (sign && k == 0)
        return fpu_wide_finish_below_one(st, res, 0); /* x<0 の e^x-1 は (-1, 0) */
    return fpu_wide_finish(st, res, k, false, 0);
}

mc68k_fp80_t mc68k_fpu_lognp1(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
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
        return sign ? fpu_operr_result(st) : fpu_exact_result(st, src);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (sign && exp == WIDE_BIAS && sig == FPU_SIG_J) { /* x = -1 → -Inf、DZ */
        r = fpu_make(true, FPU_EXP_MASK, FPU_SIG_J);
        fpu_finish_result(st, r, (uint8_t)MC68K_FPU_EXC_DZ, 0);
        return r;
    }
    if (sign && exp >= WIDE_BIAS)
        return fpu_operr_result(st); /* x < -1 */
    if (exp < TINY_EXP_ONE_SIDED)
        return fpu_tiny_result(st, sign, exp, sig, !sign, force); /* x - x²/2 */
    w_begin();
    return fpu_wide_finish(st, wide_log1p(w_make(sign, exp, sig, 0)), 0, false, force);
}

/* src が整数なら true で *n に値（|src| >= 2^limit_log2 なら ±saturate に飽和）を入れる。src
 * は有限非ゼロで正規化済み。 */
static bool integer_value(bool sign, int32_t exp, uint64_t sig, int limit_log2,
                          int32_t saturate, int32_t *n) {
    int32_t u = exp - WIDE_BIAS; /* 先頭ビットの重み 2^u */
    uint64_t mag;

    if (u < 0)
        return false;
    if (u < EXACT_INT_BITS && (sig & ((UINT64_C(1) << (EXACT_INT_BITS - u)) - 1)) != 0)
        return false;
    mag = u >= limit_log2 ? (uint64_t)saturate : sig >> (EXACT_INT_BITS - u);
    *n = sign ? -(int32_t)mag : (int32_t)mag;
    return true;
}

mc68k_fp80_t mc68k_fpu_twotox(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp, k, n;
    uint64_t sig;
    uint_fast8_t sf;
    bool sign = fpu_sign(src);
    wide_t mant;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src))
        return fpu_exact_result(st, fpu_make(false, WIDE_BIAS, FPU_SIG_J));
    if (fpu_is_inf(src))
        return fpu_exact_result(st, sign ? fpu_make(false, 0, 0) : src);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp < TINY_EXP_ONE_SIDED)
        return fpu_near_one_result(st, false, !sign, 0);
    if (integer_value(sign, exp, sig, POW2_INT_LOG2, POW2_SATURATE_N, &n)) { /* 2^n は厳密 */
        r = fpu_round_pack128(false, WIDE_BIAS + n, FPU_SIG_J, 0, st, &sf);
        fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf), 0);
        return r;
    }
    w_begin();
    wide_exp2(w_make(sign, exp, sig, 0), &mant, &k);
    return fpu_wide_finish(st, mant, k, false, 0);
}

mc68k_fp80_t mc68k_fpu_tentox(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp, k, n;
    uint64_t sig;
    uint_fast8_t sf;
    bool sign = fpu_sign(src);
    wide_t mant;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src))
        return fpu_exact_result(st, fpu_make(false, WIDE_BIAS, FPU_SIG_J));
    if (fpu_is_inf(src))
        return fpu_exact_result(st, sign ? fpu_make(false, 0, 0) : src);
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp < TINY_EXP_ONE_SIDED)
        return fpu_near_one_result(st, false, !sign, 0);
    if (!sign && exp - WIDE_BIAS <= POW10_INT_LOG2 &&
        integer_value(false, exp, sig, POW10_INT_LOG2 + 1, 0, &n) &&
        n <= POW10_EXACT_MAX) { /* 10^n（n = 1..27）は厳密 */
        uint64_t p = 1;
        int shift;

        for (int i = 0; i < n; i++)
            p *= 5;
        shift = fpu_clz64(p);
        r = fpu_round_pack128(false, WIDE_BIAS + EXACT_INT_BITS - shift + n, p << shift, 0, st,
                              &sf);
        fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf), 0);
        return r;
    }
    w_begin();
    wide_exp10(w_make(sign, exp, sig, 0), &mant, &k);
    return fpu_wide_finish(st, mant, k, false, 0);
}

mc68k_fp80_t mc68k_fpu_sincos(mc68k_fp80_t src, mc68k_fp80_t *out_cos, mc68k_fpu_state_t *st) {
    mc68k_fp80_t s = mc68k_fpu_sin(src, st);
    uint8_t cc = st->fpsr_cc;

    /* EXC は sin と cos の OR（INEX2 は両方、UNFL は sin だけ。cos の UNFL・OVFL は 0
     * 強制）。条件コードは sin の結果 */
    *out_cos = mc68k_fpu_cos(src, st);
    st->fpsr_cc = cc;
    return s;
}
