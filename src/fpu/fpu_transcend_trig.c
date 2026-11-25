/*
 * fpu_transcend_trig.c — FSIN / FCOS / FTAN。
 *
 * 引数縮約: |x| < 0.5 は縮約なし。それ以上は Payne–Hanek 型に 2/π
 * の固定小数点ビット列（生成表）との 整数積から x·2/π の小数部と象限を求める（O4:
 * 全指数範囲で正しく縮約）。縮約後の引数 r（|r| ≤ π/4）は 相対 2^-113 程度で得て、float128 の
 * Taylor 展開で sin r・cos r を求める。
 */
#include "fpu_wide.h"

#define LIMBS        16  /* 32bit 語。積は最大 449bit */
#define WINDOW_LIMBS 13  /* 窓は最大 385bit */
#define KEEP_BITS    320 /* 小数部として保持するビット数 */
#define SIG_BITS     64
#define POS_BITS (SIG_BITS - 1 + KEEP_BITS) /* 積のビット POS が整数部の最下位（重み 2^0） */
#define HALF_EXP (WIDE_BIAS - 1)            /* |x| >= 0.5 ⇔ exp >= HALF_EXP */

static int bit_of(const uint32_t *limbs, int n) {
    return (int)((limbs[n / 32] >> (n % 32)) & 1u);
}

/* 2/π の i 番目（1 始まり、2^-i の重み）のビット */
static int pi_bit(int i) {
    return (int)((two_over_pi_words[(i - 1) / 32] >> (31 - (i - 1) % 32)) & 1u);
}

/* Payne–Hanek。|x| = sig * 2^(exp-16383-63)（sig のbit63=1、exp >= HALF_EXP）。
 * 戻り値 *q = 象限（mod 4）、*r = |x|·2/π の最近接整数との差 f を π/2 倍した値（|r| ≤ π/4）。
 */
static void reduce_large(int32_t exp, uint64_t sig, int *q, wide_t *r) {
    const int e = exp - WIDE_BIAS - (SIG_BITS - 1); /* value = sig * 2^e */
    const int i0 = e - 1 > 1 ? e - 1 : 1;
    const int i1 = e + POS_BITS;
    uint32_t window[LIMBS] = {0}, prod[LIMBS] = {0}, frac[LIMBS] = {0};
    uint64_t carry;
    bool half, neg;
    int b, q0;
    uint64_t hi = 0, lo = 0;

    /* 窓: 2/π のビット i0..i1 を整数として並べる（i0 が最上位） */
    for (int i = i0; i <= i1; i++) {
        for (int k = WINDOW_LIMBS - 1; k > 0; k--)
            window[k] = (window[k] << 1) | (window[k - 1] >> 31);
        window[0] = (window[0] << 1) | (uint32_t)pi_bit(i);
    }
    /* 積 = window × sig */
    for (int m = 0; m < 2; m++) {
        uint32_t d = (uint32_t)(sig >> (32 * m));

        carry = 0;
        for (int k = 0; k < WINDOW_LIMBS; k++) {
            uint64_t t = (uint64_t)window[k] * d + prod[k + m] + carry;
            prod[k + m] = (uint32_t)t;
            carry = t >> 32;
        }
        if (m + WINDOW_LIMBS < LIMBS)
            prod[m + WINDOW_LIMBS] += (uint32_t)carry;
    }
    /* 積のビット POS_BITS が重み 2^0、POS_BITS+1 が 2^1 */
    q0 = bit_of(prod, POS_BITS) | (bit_of(prod, POS_BITS + 1) << 1);
    half = bit_of(prod, POS_BITS - 1) != 0;
    for (int n = 0; n < POS_BITS; n++) {
        if (bit_of(prod, n))
            frac[n / 32] |= UINT32_C(1) << (n % 32);
    }
    neg = half;
    if (half) { /* f = frac - 1 < 0。大きさ M = 2^POS_BITS - frac */
        uint64_t c = 1;

        for (int k = 0; k < LIMBS; k++) {
            uint64_t t = (uint64_t)(uint32_t)~frac[k] + c;
            frac[k] = (uint32_t)t;
            c = t >> 32;
        }
        for (int n = POS_BITS; n < LIMBS * 32; n++)
            frac[n / 32] &= ~(UINT32_C(1) << (n % 32));
    }
    *q = (q0 + (half ? 1 : 0)) & 3;
    /* 最上位の 1 を探して 128bit 窓（jam 付き）を取る */
    for (b = POS_BITS - 1; b >= 0 && !bit_of(frac, b); b--) {
    }
    if (b < 0) { /* 厳密に象限境界（|x| が非ゼロの拡張精度値では起きない） */
        *r = w_one();
        *r = w_sub(*r, *r);
        return;
    }
    for (int t = 0; t < 128; t++) {
        int n = b - t;
        uint64_t v = n >= 0 ? (uint64_t)bit_of(frac, n) : 0;

        if (t < 64)
            hi |= v << (63 - t);
        else
            lo |= v << (127 - t);
    }
    for (int n = b - 128; n >= 0; n--) {
        if (bit_of(frac, n)) {
            lo |= 1;
            break;
        }
    }
    *r = w_mul(w_make(neg, WIDE_BIAS + (b - POS_BITS), hi, lo), w_pi_2);
}

/* sin r, cos r（|r| ≤ π/4 + ε）。 */
static wide_t sin_r(wide_t r) {
    return w_mul(r, wide_poly(w_sin_coef, 16, w_mul(r, r)));
}

static wide_t cos_r(wide_t r) {
    return wide_poly(w_cos_coef, 16, w_mul(r, r));
}

/* |x|（f128）の縮約: *q=象限、*r=縮約引数 */
static void reduce(int32_t exp, uint64_t sig, int *q, wide_t *r) {
    if (exp < HALF_EXP) {
        *q = 0;
        *r = w_make(false, exp, sig, 0);
        return;
    }
    reduce_large(exp, sig, q, r);
}

/* |x|（正規化した exp・sig。exp >= WIDE_TINY_EXP）の sin と cos を象限も含めて求める。 */
void wide_sincos(int32_t exp, uint64_t sig, wide_t *s, wide_t *c) {
    int q;
    wide_t red, sr, cr;

    reduce(exp, sig, &q, &red);
    sr = sin_r(red);
    cr = cos_r(red);
    switch (q) {
    case 0:
        *s = sr;
        *c = cr;
        break;
    case 1:
        *s = cr;
        *c = w_neg(sr);
        break;
    case 2:
        *s = w_neg(sr);
        *c = w_neg(cr);
        break;
    default:
        *s = w_neg(cr);
        *c = sr;
        break;
    }
}

enum trig_kind { TRIG_SIN, TRIG_COS, TRIG_TAN };

static mc68k_fp80_t trig(mc68k_fp80_t src, mc68k_fpu_state_t *st, enum trig_kind kind) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;
    bool sign = fpu_sign(src);
    wide_t s, c, res;
    /* FSIN: OVFL=0。FCOS: OVFL=UNFL=0。FTAN: なし（[881UM] p.4-102, 4-34, 4-113） */
    const uint8_t force = kind == TRIG_SIN ? (uint8_t)MC68K_FPU_EXC_OVFL
                          : kind == TRIG_COS
                              ? (uint8_t)(MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_UNFL)
                              : 0;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_inf(src))
        return fpu_operr_result(st);
    if (fpu_is_zero(src)) {
        return fpu_exact_result(st, kind == TRIG_COS ? fpu_make(false, WIDE_BIAS, FPU_SIG_J)
                                                     : fpu_make(sign, 0, 0));
    }
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    if (exp < WIDE_TINY_EXP) {
        if (kind == TRIG_COS)
            return fpu_near_one_result(st, false, false, force);
        /* sin x = x - x³/6 は x より小さい側、tan x = x + x³/3 は大きい側 */
        return fpu_tiny_result(st, sign, exp, sig, kind == TRIG_SIN, force);
    }
    w_begin();
    wide_sincos(exp, sig, &s, &c);
    res = kind == TRIG_SIN ? s : kind == TRIG_COS ? c : w_div(s, c);
    if (sign && kind != TRIG_COS)
        res = w_neg(res);
    /* sin・cos の真値の大きさは 1 以下（π/2 の近傍で 1 の直下に寄る） */
    if (kind != TRIG_TAN)
        return fpu_wide_finish_below_one(st, res, force);
    return fpu_wide_finish(st, res, 0, false, force);
}

mc68k_fp80_t mc68k_fpu_sin(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return trig(src, st, TRIG_SIN);
}
mc68k_fp80_t mc68k_fpu_cos(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return trig(src, st, TRIG_COS);
}
mc68k_fp80_t mc68k_fpu_tan(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return trig(src, st, TRIG_TAN);
}
