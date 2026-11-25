/* fpu_wide.c — 超越関数の結果生成と共有の中間計算。 */
#include "fpu_wide.h"

#define ONES UINT64_C(0xFFFFFFFFFFFFFFFF)

/* 拡張精度・単精度・倍精度の有効ビット数と、最小の正規化数の idealized
 * 指数（s_roundPackToExtF80.c の書式と同じ） */
/* snap_to_grid() の近格子帯の幅（結果の格子の 1 ULP の 2^-SNAP_ULP_BITS）。設計書 §3.2 の 2^-40
 * ULP */
#define SNAP_ULP_BITS 40

struct grid_fmt {
    int bits;
    int32_t min_norm_exp;
};

static struct grid_fmt grid_of(uint8_t prec) {
    struct grid_fmt g;

    switch (prec) {
    case MC68K_FPU_PREC_SINGLE:
        g.bits = 24;
        g.min_norm_exp = 0x3F81;
        break;
    case MC68K_FPU_PREC_DOUBLE:
        g.bits = 53;
        g.min_norm_exp = 0x3C01;
        break;
    default:
        g.bits = 64;
        g.min_norm_exp = 0;
        break;
    }
    return g;
}

static bool lt128(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl) {
    return ah < bh || (ah == bh && al < bl);
}

/* w（真値の近似。誤差は 2^-40 ULP より十分小さい）が結果の格子点 g の 2^-40 ULP
 * 以内にあるとき、真値が g の どちら側にあるかは分からない。jam で片側を決めると RP/RM
 * でもう片側の隣接値（忠実丸めの外）を返し得るので、 その場合は g
 * そのものを返す（どの丸めモードでも g は真値を挟む隣接 2 値のどちらか。設計書 §3.2・§5.2）。
 * 該当すれば (*hi:*lo) を g（厳密）へ置き換えて true。 */
static bool snap_to_grid(int32_t *exp, uint64_t *hi, uint64_t *lo, uint8_t prec) {
    const struct grid_fmt g = grid_of(prec);
    int p = g.bits, drop;
    uint64_t rh, rl, mh, ml, zh, zl, th, tl;

    if (*exp < g.min_norm_exp)
        p -= g.min_norm_exp - *exp; /* 非正規化域は量子が粗い */
    if (p < 1)
        return false;
    drop = 128 - p;
    if (drop < 41 || drop > 127)
        return false;
    /* mask = 2^drop - 1、zone = 2^(drop-40) */
    mh = drop >= 64 ? (drop == 64 ? 0 : (UINT64_C(1) << (drop - 64)) - 1) : 0;
    ml = drop >= 64 ? ONES : (UINT64_C(1) << drop) - 1;
    zh = drop - 40 >= 64 ? UINT64_C(1) << (drop - 40 - 64) : 0;
    zl = drop - 40 >= 64 ? 0 : UINT64_C(1) << (drop - 40);
    rh = *hi & mh;
    rl = *lo & ml;
    if (lt128(rh, rl, zh, zl)) { /* g のすぐ上 */
        *hi &= ~mh;
        *lo &= ~ml;
        return true;
    }
    tl = (~rl & ml) + 1; /* 次の格子点までの距離 2^drop - rem */
    th = (~rh & mh) + (tl == 0 ? 1 : 0);
    if (lt128(th, tl, zh, zl)) { /* 次の格子点のすぐ下 */
        uint64_t nl = *lo + tl;
        uint64_t nh = *hi + th;
        bool carry = nl < *lo, overflow = nh < *hi;

        if (carry) {
            nh++;
            if (nh == 0)
                overflow = true;
        }
        *lo = nl;
        *hi = nh;
        if (overflow) { /* 2^128 → 1.0 * 2^(exp+1) */
            *hi = FPU_SIG_J;
            *lo = 0;
            (*exp)++;
        }
        return true;
    }
    return false;
}

mc68k_fp80_t fpu_wide_finish(mc68k_fpu_state_t *st, wide_t w, int32_t k, bool exact,
                             uint8_t force_clear) {
    uint64_t hi, lo;
    uint_fast8_t sf;
    int32_t exp = (int32_t)w_exp(w) + k;
    uint8_t extra = 0;
    mc68k_fp80_t r;

    w_sig128(w, &hi, &lo);
    if (!exact) {
        if (snap_to_grid(&exp, &hi, &lo, st->fpcr_round_prec))
            extra = (uint8_t)MC68K_FPU_EXC_INEX2;
        else
            lo |= 1;
    }
    r = fpu_round_pack128(w_sign(w), exp, hi, lo, st, &sf);
    fpu_finish_result(st, r, (uint8_t)(fpu_sf_flags_to_exc(sf) | extra), force_clear);
    return r;
}

mc68k_fp80_t fpu_wide_finish_below_one(mc68k_fpu_state_t *st, wide_t w, uint8_t force_clear) {
    const int bits = grid_of(st->fpcr_round_prec).bits;

    if (w_exp(w) >= WIDE_BIAS - 1) { /* 0.5 <= |w| */
        wide_t gap = w_sub(w_one(), w_abs(w));

        if (w_is_zero(gap) || w_sign(gap) ||
            (int32_t)w_exp(gap) < WIDE_BIAS - bits - SNAP_ULP_BITS) {
            return fpu_near_one_result(st, w_sign(w), false, force_clear);
        }
    }
    return fpu_wide_finish(st, w, 0, false, force_clear);
}

mc68k_fp80_t fpu_const_result(mc68k_fpu_state_t *st, const struct wide_const128 *c, bool sign,
                              uint8_t force_clear) {
    uint_fast8_t sf;
    mc68k_fp80_t r = fpu_round_pack128(sign, c->exp, c->hi, c->lo, st, &sf);

    fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf), force_clear);
    return r;
}

mc68k_fp80_t fpu_exact_result(mc68k_fpu_state_t *st, mc68k_fp80_t v) {
    fpu_finish_result(st, v, 0, 0);
    return v;
}

mc68k_fp80_t fpu_operr_result(mc68k_fpu_state_t *st) {
    mc68k_fp80_t r = fpu_default_nan();

    fpu_finish_result(st, r, (uint8_t)MC68K_FPU_EXC_OPERR, 0);
    return r;
}

mc68k_fp80_t fpu_tiny_result(mc68k_fpu_state_t *st, bool sign, int32_t exp, uint64_t sig,
                             bool negative_corr, uint8_t force_clear) {
    uint64_t hi = sig, lo = 1;
    uint_fast8_t sf;
    mc68k_fp80_t r;

    if (negative_corr) { /* (sig:0) - 1。先頭ビットが落ちたら正規化し直す */
        hi = sig - 1;
        lo = ONES;
        if ((hi & FPU_SIG_J) == 0) {
            hi = (hi << 1) | (lo >> 63);
            lo = ONES;
            exp--;
        }
    }
    r = fpu_round_pack128(sign, exp, hi, lo, st, &sf);
    fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf), force_clear);
    return r;
}

mc68k_fp80_t fpu_near_one_result(mc68k_fpu_state_t *st, bool sign, bool above,
                                 uint8_t force_clear) {
    uint_fast8_t sf;
    mc68k_fp80_t r;

    if (above)
        r = fpu_round_pack128(sign, WIDE_BIAS, FPU_SIG_J, 1, st, &sf);
    else
        r = fpu_round_pack128(sign, WIDE_BIAS - 1, ONES, ONES, st, &sf);
    fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf), force_clear);
    return r;
}

wide_t wide_poly(const wide_t *c, int count, wide_t z) {
    wide_t p = c[count - 1];

    for (int n = count - 2; n >= 0; n--)
        p = w_add(w_mul(p, z), c[n]);
    return p;
}

/* |x| >= 2^14 なら e^x は必ずオーバーフロー/アンダーフロー。roundPack
 * へ渡す指数だけを大きく振る。 */
#define EXP_SATURATE_K     100000
#define EXP_SATURATE_FIELD (WIDE_BIAS + 14)

void wide_exp(wide_t x, wide_t *mant, int32_t *k) {
    wide_t kw, r;

    if (w_exp(x) >= EXP_SATURATE_FIELD) {
        *mant = w_one();
        *k = w_sign(x) ? -EXP_SATURATE_K : EXP_SATURATE_K;
        return;
    }
    *k = w_round_int(w_mul(x, w_inv_ln2));
    kw = w_from_int(*k);
    r = w_sub(w_sub(x, w_mul(kw, w_ln2_hi)), w_mul(kw, w_ln2_lo));
    *mant = wide_poly(w_exp_coef, 27, r);
}

/* |x| >= 2^15 なら 2^x は必ずオーバーフロー/アンダーフロー（最小の非正規化数は 2^-16446） */
#define EXP2_SATURATE_FIELD  (WIDE_BIAS + 15)
/* |x| >= 2^13 なら 10^x は必ずオーバーフロー/アンダーフロー（10^4932 付近が境界） */
#define EXP10_SATURATE_FIELD (WIDE_BIAS + 13)

void wide_exp2(wide_t x, wide_t *mant, int32_t *k) {
    wide_t r;

    if (w_exp(x) >= EXP2_SATURATE_FIELD) {
        *mant = w_one();
        *k = w_sign(x) ? -EXP_SATURATE_K : EXP_SATURATE_K;
        return;
    }
    *k = w_round_int(x);
    r = w_sub(x, w_from_int(*k)); /* |r| <= 0.5。x が 64bit 仮数なので厳密 */
    *mant = wide_poly(w_exp_coef, 27, w_add(w_mul(r, w_ln2_hi), w_mul(r, w_ln2_lo)));
}

/* x·ln10 - k·ln2。x·LN10_A は 64bit × 48bit = 112bit で厳密、k·LN2_HI（k は 15bit）も厳密。 */
void wide_exp10(wide_t x, wide_t *mant, int32_t *k) {
    wide_t kw, r;

    if (w_exp(x) >= EXP10_SATURATE_FIELD) {
        *mant = w_one();
        *k = w_sign(x) ? -EXP_SATURATE_K : EXP_SATURATE_K;
        return;
    }
    *k = w_round_int(w_mul(x, w_log2_10));
    kw = w_from_int(*k);
    r = w_add(w_sub(w_mul(x, w_ln10_a), w_mul(kw, w_ln2_hi)),
              w_sub(w_mul(x, w_ln10_b), w_mul(kw, w_ln2_lo)));
    *mant = wide_poly(w_exp_coef, 27, r);
}

wide_t wide_log1p_small(wide_t x) {
    wide_t s = w_div(x, w_add(x, w_make(false, WIDE_BIAS + 1, FPU_SIG_J, 0))); /* x / (2 + x) */

    return w_scale(w_mul(s, wide_poly(w_odd_recip, 31, w_mul(s, s))), 1);
}

#define SMALL_EXP_LOG1P \
    (WIDE_BIAS - 2) /* |x| < 0.25 は ln(1+x) を x/(2+x) の級数で直接求める */
#define SMALL_EXP_EXPM1 (WIDE_BIAS - 1) /* |x| < 0.5 は e^x-1 を級数で直接求める */
#define EXPM1_BIG_K     120 /* 2^k·mant の -1 が f128 に見えない/-1 に近すぎる k の大きさ */

wide_t wide_log1p(wide_t x) {
    if (w_exp(x) < SMALL_EXP_LOG1P)
        return wide_log1p_small(x);
    return wide_ln(w_add(w_one(), x));
}

wide_t wide_expm1(wide_t x, int32_t *k, bool *near_one) {
    wide_t mant;

    *k = 0;
    *near_one = false;
    if (w_exp(x) < SMALL_EXP_EXPM1)
        return wide_expm1_small(x);
    wide_exp(x, &mant, k);
    if (*k >= EXPM1_BIG_K)
        return mant;
    if (*k <= -EXPM1_BIG_K) {
        *near_one = true;
        *k = 0;
        return w_neg(w_one());
    }
    mant = w_sub(w_scale(mant, *k), w_one());
    *k = 0;
    return mant;
}

wide_t wide_expm1_small(wide_t x) {
    return w_mul(x, wide_poly(w_exp_coef + 1, 26, x));
}

/* 1.0 以上 2.0 未満の仮数 m に対し、m > √2 なら m/2 と指数 +1 に付け替える。√2
 * の上位ビットとの比較で足りる （境界がずれても |s| ≤ 0.1716 の範囲に収まる）。 */
#define SQRT2_HI UINT64_C(0xB504F333F9DE6484)

/* ln(m·2^u) の m 側。m は指数フィールド BIAS の値（1 <= m < 2）。m > √2 なら m/2 に付け替えて
 * *u を 1 増やし、 ln(m') を返す（|s| ≤ 0.1716 の範囲に入れる）。 */
wide_t wide_ln_frac(wide_t m, int32_t *u) {
    wide_t s, s2, series;
    uint64_t hi, lo;

    w_sig128(m, &hi, &lo);
    if (hi > SQRT2_HI) {
        m = w_scale(m, -1);
        (*u)++;
    }
    /* ln m = 2 atanh(s)、s = (m-1)/(m+1) */
    s = w_div(w_sub(m, w_one()), w_add(m, w_one()));
    s2 = w_mul(s, s);
    series = w_mul(s, wide_poly(w_odd_recip, 31, s2));
    return w_scale(series, 1);
}

/* ln(m·2^u) = u·ln2 + ln m。u·LN2_HI は厳密（|u| < 2^15、LN2_HI は 64bit）。 */
wide_t wide_ln_mu(wide_t m, int32_t u) {
    wide_t res = wide_ln_frac(m, &u);

    if (u != 0) {
        wide_t uw = w_from_int(u);

        res = w_add(w_mul(uw, w_ln2_hi), w_add(res, w_mul(uw, w_ln2_lo)));
    }
    return res;
}

wide_t wide_ln(wide_t y) {
    uint64_t hi, lo;

    w_sig128(y, &hi, &lo);
    return wide_ln_mu(w_make(false, WIDE_BIAS, hi, lo), (int32_t)w_exp(y) - WIDE_BIAS);
}

/* atan(y)。y > 1 は π/2 - atan(1/y)。y ∈ [0,1] は atan(c_j) + atan((y-c_j)/(1+y c_j))、c_j =
 * j/8。 */
wide_t wide_atan(wide_t y) {
    bool inv = w_lt(w_one(), y);
    wide_t t, a;
    int j;

    if (inv)
        y = w_div(w_one(), y);
    j = w_round_int(w_mul(y, w_make(false, WIDE_BIAS + 3, FPU_SIG_J, 0))); /* round(8y) */
    t = w_div(w_sub(y, w_atan_c[j]), w_add(w_one(), w_mul(y, w_atan_c[j])));
    a = w_add(w_atan_tab[j], w_mul(t, wide_poly(w_atan_coef, 16, w_mul(t, t))));
    return inv ? w_sub(w_pi_2, a) : a;
}
