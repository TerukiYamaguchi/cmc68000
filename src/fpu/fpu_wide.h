/*
 * fpu_wide.h — 超越関数の中間精度（float128）との橋渡し。
 * 公開しない。
 *
 * float128 は 113bit の有効数字を持つ。拡張精度との変換に SoftFloat の
 * extF80_to_f128/f128_to_extF80 は使わず （指数フィールド 0 の欠陥。§5.1-1）、ここの
 * w_make()・w_sig128() で自前に組み立てる。最終の丸めは必ず fpu_round_pack128()（自前
 * roundPack、二重丸めなし）を通す。中間演算は常に RN（w_begin()）で行い、
 * 例外フラグは最終丸めの前に捨てる（§5.1-6）。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "platform.h"
#include "internals.h"
#include "softfloat.h"
#include "fpu_internal.h"

typedef float128_t wide_t;

#define WIDE_BIAS     16383
#define WIDE_SIGN_BIT UINT64_C(0x8000000000000000)
#define WIDE_EXP_MAX  0x7FFE

/* 128bit 仮数（bit127=1）と拡張精度の idealized 指数 exp。値 = (hi:lo) * 2^(exp-16383-127)。 */
struct wide_const128 {
    uint16_t exp;
    uint64_t hi;
    uint64_t lo;
};

extern const wide_t w_pi_2, w_pi, w_ln2_hi, w_ln2_lo, w_inv_ln2, w_inv_ln10, w_ln10_a, w_ln10_b,
    w_log2_10;
extern const wide_t w_exp_coef[27], w_sin_coef[16], w_cos_coef[16], w_sinh_coef[16],
    w_cosh_coef[16];
extern const wide_t w_odd_recip[31], w_atan_coef[16], w_atan_tab[9], w_atan_c[9];
extern const struct wide_const128 c128_pi_2, c128_pi;
extern const uint32_t two_over_pi_words[];
extern const int two_over_pi_bits;

static inline uint32_t w_exp(wide_t a) {
    return (uint32_t)((a.v[1] >> 48) & 0x7FFF);
}
static inline bool w_sign(wide_t a) {
    return (a.v[1] & WIDE_SIGN_BIT) != 0;
}
static inline bool w_is_zero(wide_t a) {
    return (a.v[1] & ~WIDE_SIGN_BIT) == 0 && a.v[0] == 0;
}

/* (hi:lo) の bit127 を先頭ビットとして 113bit へ切り捨てて組み立てる。1 <= exp <= 0x7FFE。 */
static inline wide_t w_make(bool sign, int32_t exp, uint64_t hi, uint64_t lo) {
    wide_t a;

    a.v[1] = (sign ? WIDE_SIGN_BIT : 0) | ((uint64_t)exp << 48) | ((hi & ~FPU_SIG_J) >> 15);
    a.v[0] = ((hi & UINT64_C(0x7FFF)) << 49) | (lo >> 15);
    return a;
}

/* 正規の非ゼロ値の 128bit 仮数（bit127=1）。lo の下位 15bit は 0。 */
static inline void w_sig128(wide_t a, uint64_t *hi, uint64_t *lo) {
    *hi = FPU_SIG_J | ((a.v[1] & UINT64_C(0xFFFFFFFFFFFF)) << 15) | (a.v[0] >> 49);
    *lo = a.v[0] << 15;
}

static inline wide_t w_neg(wide_t a) {
    a.v[1] ^= WIDE_SIGN_BIT;
    return a;
}
static inline wide_t w_abs(wide_t a) {
    a.v[1] &= ~WIDE_SIGN_BIT;
    return a;
}

/* 2^n を掛ける（結果が正規範囲に収まること）。0 は 0 のまま。 */
static inline wide_t w_scale(wide_t a, int32_t n) {
    if (w_is_zero(a))
        return a;
    a.v[1] += (uint64_t)(int64_t)n << 48;
    return a;
}

static inline wide_t w_add(wide_t a, wide_t b) {
    return f128_add(a, b);
}
static inline wide_t w_sub(wide_t a, wide_t b) {
    return f128_sub(a, b);
}
static inline wide_t w_mul(wide_t a, wide_t b) {
    return f128_mul(a, b);
}
static inline wide_t w_div(wide_t a, wide_t b) {
    return f128_div(a, b);
}
static inline wide_t w_sqrt(wide_t a) {
    return f128_sqrt(a);
}
static inline bool w_lt(wide_t a, wide_t b) {
    return f128_lt(a, b);
}
static inline wide_t w_one(void) {
    return w_make(false, WIDE_BIAS, FPU_SIG_J, 0);
}

static inline wide_t w_from_int(int32_t n) {
    uint64_t mag = n < 0 ? (uint64_t)0 - (uint64_t)(int64_t)n : (uint64_t)n;
    int shift;

    if (mag == 0) {
        wide_t z;

        z.v[0] = 0;
        z.v[1] = 0;
        return z;
    }
    shift = fpu_clz64(mag);
    return w_make(n < 0, WIDE_BIAS + 63 - shift, mag << shift, 0);
}

/* 最近接偶数で整数化（|a| < 2^31 であること） */
static inline int32_t w_round_int(wide_t a) {
    return (int32_t)f128_to_i32(a, softfloat_round_near_even, false);
}

/* 中間演算の前に呼ぶ: 丸めモードを RN にし、フラグを捨てる（§5.1-6）。 */
static inline void w_begin(void) {
    softfloat_roundingMode = softfloat_round_near_even;
    softfloat_detectTininess = softfloat_tininess_beforeRounding;
    softfloat_exceptionFlags = 0;
}

/* --- 結果の生成（fpu_wide.c）。どれも roundPack を 1 回だけ通し、fpu_finish_result() で
 * EXC・CC を設定する。 --- */

/* 中間結果 w（非ゼロ）× 2^k を丸める。exact=false なら不正確として扱う（sticky）。 */
mc68k_fp80_t fpu_wide_finish(mc68k_fpu_state_t *st, wide_t w, int32_t k, bool exact,
                             uint8_t force_clear);

/* 真値の大きさが 1 未満と分かっている w（sin・cos・tanh・x<0 の e^x-1 など。k は 0）。w が 1
 * の直下の近格子帯 （結果精度の格子間隔 2^-bits の 2^-40 倍）に入る、または f128 の丸めで 1
 * 以上になったときは、snap_to_grid() のように 格子点 ±1 を返さず、向きの分かる
 * fpu_near_one_result()（1 - 2^-128）で返す（RZ/RM/RP で 1 の次の内側の値を返すため）。 */
mc68k_fp80_t fpu_wide_finish_below_one(mc68k_fpu_state_t *st, wide_t w, uint8_t force_clear);

/* 定数（π/2 など。jam 込み）× 符号を丸める。 */
mc68k_fp80_t fpu_const_result(mc68k_fpu_state_t *st, const struct wide_const128 *c, bool sign,
                              uint8_t force_clear);

/* 厳密な結果（丸めで変わらない拡張精度値）。フラグなし。 */
mc68k_fp80_t fpu_exact_result(mc68k_fpu_state_t *st, mc68k_fp80_t v);

/* OPERR（既定NaN） */
mc68k_fp80_t fpu_operr_result(mc68k_fpu_state_t *st);

/* 極小引数（|x| < 2^-40、真値が x に極めて近い）。x の 128bit 表現を補正項の方向へ 1 ulp128
 * ずらした値を丸める （§5.2「極小引数の方向」）。negative_corr: 真値の大きさが |x| より小さい。
 */
mc68k_fp80_t fpu_tiny_result(mc68k_fpu_state_t *st, bool sign, int32_t exp, uint64_t sig,
                             bool negative_corr, uint8_t force_clear);

/* 1 のごく近く。above=true なら 1 + 2^-127（1 より大きい側）、false なら 1 - 2^-128。 */
mc68k_fp80_t fpu_near_one_result(mc68k_fpu_state_t *st, bool sign, bool above,
                                 uint8_t force_clear);

/* 極小引数とみなす境界（idealized 指数。|x| < 2^-40 ⇔ exp < TINY_EXP） */
#define WIDE_TINY_EXP (WIDE_BIAS - 40)

/* --- 共有の中間計算（fpu_transcend_explog.c ほか） --- */

/* exp(x) = *mant * 2^*k。x は f128 の有限非ゼロ。|x| >= 2^14 は
 * k=±100000（オーバーフロー・アンダーフロー）。 */
void wide_exp(wide_t x, wide_t *mant, int32_t *k);

/* 2^x = *mant * 2^*k。x は f128 の有限非ゼロ（64bit 仮数）。|x| >= 2^15 は
 * k=±100000（オーバーフロー・アンダーフロー）。 */
void wide_exp2(wide_t x, wide_t *mant, int32_t *k);
/* 10^x = *mant * 2^*k。x は f128 の有限非ゼロ（64bit 仮数）。|x| >= 2^13 は k=±100000。 */
void wide_exp10(wide_t x, wide_t *mant, int32_t *k);

/* ln(y)。y は f128 の正の有限値。 */
wide_t wide_ln(wide_t y);
/* ln(m·2^u)（m は指数フィールド BIAS の値）と、m の√2 付け替えを *u に反映して ln(m')
 * だけ返す版。 */
wide_t wide_ln_mu(wide_t m, int32_t u);
wide_t wide_ln_frac(wide_t m, int32_t *u);

/* atan(y)。y >= 0 の f128。結果は [0, π/2)。 */
wide_t wide_atan(wide_t y);

/* ln(1+x)（-1 < x、有限非零の f128）。|x| < 0.25 は wide_log1p_small()、それ以上は
 * 1+x（桁落ちなし）の wide_ln()。 */
wide_t wide_log1p(wide_t x);
/* e^x-1 = 戻り値·2^*k（x は有限非零の f128）。|x| < 0.5 は
 * wide_expm1_small()（*k=0）、それ以上は wide_exp() の mant·2^k-1。k >= 120 は -1
 * が誤差予算より小さいので引かない（戻り値=mant）。k <= -120 は -1 のごく内側で、
 * *near_one=true にして戻り値は -1（呼び出し側が fpu_near_one_result() で方向を付ける）。 */
wide_t wide_expm1(wide_t x, int32_t *k, bool *near_one);
/* ln(1+x)。|x| < 0.25 の f128（有限非ゼロ）。s = x/(2+x) の奇数級数（桁落ちなし）。 */
wide_t wide_log1p_small(wide_t x);
/* e^x - 1。|x| < 0.5 の f128（有限非ゼロ）。x·Σ x^n/(n+1)!（桁落ちなし）。 */
wide_t wide_expm1_small(wide_t x);

/* 多項式 Σ c[n] z^n（Horner、z は f128）。count 項。 */
wide_t wide_poly(const wide_t *c, int count, wide_t z);

/* --- 単体検査用の中間計算（各 .c が定義。float128 のまま返す。tests/fpu/test_fpu_b1.c が
 * mpmath の値と突き合わせる） --- */
void wide_sincos(int32_t exp, uint64_t sig, wide_t *s,
                 wide_t *c);              /* |x| の sin・cos（exp >= WIDE_TINY_EXP） */
wide_t wide_asin(wide_t ax);              /* 0 < ax < 1 */
wide_t wide_acos(wide_t x);               /* |x| < 1、符号つき */
wide_t wide_log2_mu(wide_t m, int32_t u); /* m は指数フィールド BIAS */
wide_t wide_log10_mu(wide_t m, int32_t u);
wide_t wide_sinh(wide_t ax, int32_t *k); /* 値·2^*k */
wide_t wide_cosh(wide_t ax, int32_t *k);
wide_t wide_tanh(wide_t ax);  /* 0 < ax < 64 */
wide_t wide_atanh(wide_t ax); /* 0 < ax < 1 */
