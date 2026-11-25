/*
 * fpu_bcd.c — packed decimal の入出力。
 *
 * 変換は多倍長整数（uint32 の配列）で厳密に行い、丸めは 1 回だけ（入力は拡張精度への
 * roundPack、 出力は 10 進の桁での RM 丸め）。10 の冪は 10^9
 * の乗算の繰り返しで作る（表を持たない）。
 */
#include <stdbool.h>
#include <stdint.h>
#include "fpu_internal.h"

#define BN_LIMBS           544 /* 17408bit。最大は 10^4969·2^64（約 16572bit）と 2^16384 */
#define LIMB_BITS          32
#define EXP_BIAS_X         16383
#define MANT_DIGITS        17 /* packed の仮数: 整数 1 桁 + 小数 16 桁 */
#define GUARD_DIGITS       18 /* 出力で求める先頭からの桁数（最大 17 桁 + 丸め用 1 桁） */
#define SIG_BITS           64
#define PACKED_INFNAN_MASK UINT32_C(0x7FFF0000) /* SE・yy・指数 3 桁（$FFF） */
#define NIBBLE_BITS        4
#define NIBBLE_MASK        0xFu
#define K_MAX              17 /* k-factor の有効桁数の上限 */
#define K_BITS             7
#define K_SIGN_BIT         0x40
#define EXP_DIGITS_MAX     999 /* 指数を 3 桁で表せる上限 */
#define QUOTIENT_BITS      132 /* 入力の除算で求める商のビット数の下限（128bit + 余裕） */
#define LOG10_2_NUM                                                            \
    78913 /* log10(2) ≈ 78913 / 2^18（推定だけに使い、桁は検算して補正する） \
           */
#define LOG10_2_SHIFT      18
#define POW10_CHUNK        UINT32_C(1000000000)
#define POW10_CHUNK_EXP    9
#define E_ESTIMATE_RETRIES 3
#define WIDE_HI_BITS       128

typedef struct {
    uint32_t d[BN_LIMBS];
    int n; /* 使用中の limb 数。0 は値 0 */
} bignum_t;

static void bn_trim(bignum_t *a) {
    while (a->n > 0 && a->d[a->n - 1] == 0)
        a->n--;
}

static uint32_t bn_limb(const bignum_t *a, int i) {
    return i >= 0 && i < a->n ? a->d[i] : 0;
}

static void bn_set_u64(bignum_t *a, uint64_t v) {
    a->d[0] = (uint32_t)v;
    a->d[1] = (uint32_t)(v >> LIMB_BITS);
    a->n = 2;
    bn_trim(a);
}

static int bn_bitlen(const bignum_t *a) {
    uint32_t top;
    int bits = 0;

    if (a->n == 0)
        return 0;
    top = a->d[a->n - 1];
    while (top != 0) {
        bits++;
        top >>= 1;
    }
    return (a->n - 1) * LIMB_BITS + bits;
}

static void bn_set_bit(bignum_t *a, int bit) {
    int limb = bit / LIMB_BITS;

    while (a->n <= limb)
        a->d[a->n++] = 0;
    a->d[limb] |= UINT32_C(1) << (bit % LIMB_BITS);
}

static void bn_mul_u32(bignum_t *a, uint32_t m) {
    uint64_t carry = 0;

    for (int i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;

        a->d[i] = (uint32_t)t;
        carry = t >> LIMB_BITS;
    }
    if (carry != 0)
        a->d[a->n++] = (uint32_t)carry;
    bn_trim(a);
}

/* a *= 10^e */
static void bn_mul_pow10(bignum_t *a, int e) {
    while (e >= POW10_CHUNK_EXP) {
        bn_mul_u32(a, POW10_CHUNK);
        e -= POW10_CHUNK_EXP;
    }
    if (e > 0) {
        uint32_t m = 1;

        for (int i = 0; i < e; i++)
            m *= 10;
        bn_mul_u32(a, m);
    }
}

static void bn_shl(bignum_t *a, int bits) {
    int limbs = bits / LIMB_BITS, rem = bits % LIMB_BITS, new_n;

    if (a->n == 0 || bits == 0)
        return; /* 防御（呼び出し側は非ゼロかつ bits > 0 で呼ぶ。テストでは到達しない） */
    new_n = a->n + limbs + 1;
    for (int i = new_n - 1; i >= 0; i--) {
        uint32_t hi = bn_limb(a, i - limbs), lo = bn_limb(a, i - limbs - 1);

        a->d[i] = rem != 0 ? (hi << rem) | (lo >> (LIMB_BITS - rem)) : hi;
    }
    a->n = new_n;
    bn_trim(a);
}

/* a >>= bits（切り捨て）。捨てたビットに 1 があれば true（sticky）。 */
static bool bn_shr(bignum_t *a, int bits) {
    int limbs = bits / LIMB_BITS, rem = bits % LIMB_BITS;
    bool sticky = false;

    if (bits == 0)
        return false; /* 防御（到達しない） */
    for (int i = 0; i < limbs && i < a->n; i++)
        sticky = sticky || a->d[i] != 0;
    if (rem != 0 && (bn_limb(a, limbs) & ((UINT32_C(1) << rem) - 1)) != 0)
        sticky = true;
    if (limbs >= a->n) { /* 防御: 全ビットを捨てる。現在の呼び出しでは到達しない */
        a->n = 0;
        return sticky;
    }
    for (int i = 0; i < a->n - limbs; i++) {
        uint32_t lo = bn_limb(a, i + limbs), hi = bn_limb(a, i + limbs + 1);

        a->d[i] = rem != 0 ? (lo >> rem) | (hi << (LIMB_BITS - rem)) : lo;
    }
    a->n -= limbs;
    bn_trim(a);
    return sticky;
}

static int bn_cmp(const bignum_t *a, const bignum_t *b) {
    if (a->n != b->n)
        return a->n > b->n ? 1 : -1;
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i])
            return a->d[i] > b->d[i] ? 1 : -1;
    }
    return 0;
}

/* a -= b（a >= b） */
static void bn_sub(bignum_t *a, const bignum_t *b) {
    int64_t borrow = 0;

    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - bn_limb(b, i) - borrow;

        borrow = t < 0 ? 1 : 0;
        a->d[i] = (uint32_t)t;
    }
    bn_trim(a);
}

/* n（被除数。関数の後は余り）を d で割った商を q
 * に入れる。商のビット数が少ないときに速いよう、d を n の先頭に 揃えてから 1 ビットずつ引く。
 */
static void bn_divmod(bignum_t *n, const bignum_t *d, bignum_t *q) {
    bignum_t dp = *d;
    int shift = bn_bitlen(n) - bn_bitlen(d);

    q->n = 0;
    if (shift < 0)
        return; /* 防御: n < d なら商 0（現在の呼び出しでは到達しない） */
    bn_shl(&dp, shift);
    for (int j = shift; j >= 0; j--) {
        if (bn_cmp(n, &dp) >= 0) {
            bn_sub(n, &dp);
            bn_set_bit(q, j);
        }
        (void)bn_shr(&dp, 1);
    }
}

/* a の先頭 128bit を (*hi:*lo)（bit127=1）に取り出し、指数の重み（a の bitlen -
 * 1）を返す。捨てた下位に 1 があれば *sticky を立てる。a は非ゼロ。 */
static int bn_top128(const bignum_t *a, uint64_t *hi, uint64_t *lo, bool *sticky) {
    bignum_t t = *a;
    int len = bn_bitlen(a);

    if (len > WIDE_HI_BITS) {
        if (bn_shr(&t, len - WIDE_HI_BITS))
            *sticky = true;
    } else {
        bn_shl(&t, WIDE_HI_BITS - len);
    }
    *lo = (uint64_t)bn_limb(&t, 1) << LIMB_BITS | bn_limb(&t, 0);
    *hi = (uint64_t)bn_limb(&t, 3) << LIMB_BITS | bn_limb(&t, 2);
    return len - 1;
}

/* ---- 入力: packed → 拡張精度
 * ----------------------------------------------------------------- */

static unsigned nibble(uint32_t w, int shift) {
    return (w >> shift) & NIBBLE_MASK;
}

mc68k_fp80_t mc68k_fpu_from_p(const uint32_t words[3], mc68k_fpu_state_t *st) {
    const uint32_t w0 = words[0];
    const bool mant_sign = (w0 >> 31) != 0, exp_sign = ((w0 >> 30) & 1u) != 0;
    const uint64_t frac = (uint64_t)words[1] << LIMB_BITS | words[2];
    const unsigned e2 = nibble(w0, 24), e1 = nibble(w0, 20), e0 = nibble(w0, 16);
    uint64_t mant = nibble(w0, 0);
    int e, n, top_weight;
    uint64_t hi, lo;
    bool sticky = false;
    bignum_t num;
    uint_fast8_t sf;
    mc68k_fp80_t r;

    /* Inf・NaN は Table 3-4 の型（SE=1・yy=11・指数 $FFF）。SE・yy が違う $FFF は範囲内の（非
     * 10 進の指数の）数 */
    if ((w0 & PACKED_INFNAN_MASK) == PACKED_INFNAN_MASK) {
        return fpu_make(mant_sign, FPU_EXP_MASK,
                        frac == 0 ? FPU_SIG_J : frac); /* NaN の仮数は bit 単位で写す */
    }
    for (int i = 15; i >= 0; i--)
        mant = mant * 10 + ((frac >> (i * NIBBLE_BITS)) & NIBBLE_MASK);
    if (mant == 0)
        return fpu_make(mant_sign, 0, 0); /* 指数の桁が非 10 進でも真のゼロ（Table 3-4 注 2） */
    e = (int)(e2 * 100 + e1 * 10 + e0);
    n = (exp_sign ? -e : e) - (MANT_DIGITS - 1); /* 値 = mant × 10^n */
    bn_set_u64(&num, mant);
    if (n >= 0) {
        bn_mul_pow10(&num, n);
        top_weight = bn_top128(&num, &hi, &lo, &sticky);
    } else {
        bignum_t den, quo;
        int scale;

        den.n = 0;
        bn_set_u64(&den, 1);
        bn_mul_pow10(&den, -n);
        scale = bn_bitlen(&den) - bn_bitlen(&num) + QUOTIENT_BITS;
        if (scale < 0)
            scale = 0; /* 防御（den >= 10・num < 2^58 なので scale > 0。到達しない） */
        bn_shl(&num, scale);
        bn_divmod(&num, &den, &quo);
        sticky = num.n != 0;
        top_weight = bn_top128(&quo, &hi, &lo, &sticky) - scale;
    }
    if (sticky)
        lo |= 1;
    /* 値は範囲内（オーバーフロー・アンダーフローなし。[881UM] §4.3.3）。RM
     * で拡張精度へ丸める（PREC は無視） */
    r = fpu_round_pack128_prec(mant_sign, EXP_BIAS_X + top_weight, hi, lo, st,
                               FPU_ROUNDING_PRECISION_X, &sf);
    if ((fpu_sf_flags_to_exc(sf) & MC68K_FPU_EXC_INEX2) != 0)
        fpu_commit_exc(st, (uint8_t)MC68K_FPU_EXC_INEX1); /* 変換の端数は INEX1 */
    return r;
}

/* ---- 出力: 拡張精度 → packed
 * ------------------------------------------------------------------ */

static uint64_t bcd_from_digits(const int *digits, int first, int count) {
    uint64_t v = 0;

    for (int i = 0; i < count; i++)
        v = v << NIBBLE_BITS | (uint64_t)digits[first + i];
    return v;
}

/* |v| = sig × 2^e2 に対し、Q = floor(|v| × 10^(GUARD_DIGITS - 1 - E)) と、切り捨てた端数の有無
 * *rest を求める。 Q は 10^19 未満（E の推定が 1 ずれても u64 に収まる）。 */
static uint64_t scaled_quotient(uint64_t sig, int e2, int e10, bool *rest) {
    bignum_t num, den, quo;
    const int p = GUARD_DIGITS - 1 - e10;
    uint64_t q = 0;

    bn_set_u64(&num, sig);
    *rest = false;
    if (p >= 0)
        bn_mul_pow10(&num, p);
    if (e2 >= 0)
        bn_shl(&num, e2);
    else if (bn_shr(&num, -e2))
        *rest = true;
    if (p < 0) {
        den.n = 0;
        bn_set_u64(&den, 1);
        bn_mul_pow10(&den, -p);
        bn_divmod(&num, &den, &quo);
        if (num.n != 0)
            *rest = true;
        num = quo;
    }
    for (int i = num.n - 1; i >= 0; i--)
        q = q << LIMB_BITS | num.d[i];
    return q;
}

static bool round_up_decimal(uint8_t mode, bool sign, int half_cmp, bool inexact, bool odd) {
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

static void put_bcd_words(uint32_t words[3], bool sign, int e10, const int *digits) {
    const unsigned mag = (unsigned)(e10 < 0 ? -e10 : e10);
    const unsigned exp3 = (mag / 1000) % 10, exp210 = mag % 1000;
    const uint64_t frac = bcd_from_digits(digits, 1, MANT_DIGITS - 1);

    words[0] = (sign ? UINT32_C(1) << 31 : 0) | (e10 < 0 ? UINT32_C(1) << 30 : 0) |
               ((exp210 / 100) << 24) | (((exp210 / 10) % 10) << 20) | ((exp210 % 10) << 16) |
               (exp3 << 12) | (unsigned)digits[0];
    words[1] = (uint32_t)(frac >> LIMB_BITS);
    words[2] = (uint32_t)frac;
}

void mc68k_fpu_to_p(mc68k_fp80_t v, int8_t k_in, uint32_t words[3], mc68k_fpu_state_t *st) {
    const bool sign = fpu_sign(v);
    int k = ((int)((unsigned)k_in & ((1u << K_BITS) - 1)) ^ K_SIGN_BIT) -
            K_SIGN_BIT; /* 下位 7bit を符号拡張 */
    int32_t exp;
    uint64_t sig, q = 0;
    int e2, e10, len, digits[GUARD_DIGITS], kept[MANT_DIGITS] = {0};
    bool rest, inexact, up;
    int half_cmp;
    uint8_t exc = 0;

    if (k > K_MAX) { /* k > +17 は値によらず OPERR（[881UM] p.4-67）。k=+17 として出力 */
        exc |= (uint8_t)MC68K_FPU_EXC_OPERR;
        k = K_MAX;
    }
    if (fpu_is_nan(v)) { /* 仮数を bit 単位で書く。SNaN は quiet 化して SNAN */
        if (fpu_is_snan(v))
            exc |= (uint8_t)MC68K_FPU_EXC_SNAN;
        words[0] = (sign ? UINT32_C(1) << 31 : 0) | UINT32_C(0x7FFF0000);
        words[1] = (uint32_t)(fpu_quiet_signif(v.signif) >> LIMB_BITS);
        words[2] = (uint32_t)v.signif;
        fpu_commit_exc(st, exc);
        return;
    }
    if (fpu_is_inf(v) || fpu_is_zero(v)) {
        words[0] = (sign ? UINT32_C(1) << 31 : 0) | (fpu_is_inf(v) ? UINT32_C(0x7FFF0000) : 0);
        words[1] = 0;
        words[2] = 0;
        fpu_commit_exc(st, exc);
        return;
    }
    fpu_normalize(fpu_exp_field(v), v.signif, &exp, &sig);
    e2 = exp - EXP_BIAS_X - (SIG_BITS - 1);
    /* 推定（負のときは floor になるよう割る。C99 の負数の右シフトは処理系定義）。以下で検算する
     */
    e10 = (exp - EXP_BIAS_X) * LOG10_2_NUM;
    e10 = e10 >= 0 ? e10 >> LOG10_2_SHIFT
                   : -(((-e10) + (1 << LOG10_2_SHIFT) - 1) >> LOG10_2_SHIFT);
    for (int i = 0; i < E_ESTIMATE_RETRIES; i++) {
        q = scaled_quotient(sig, e2, e10, &rest);
        if (q >= UINT64_C(1000000000000000000))
            e10++;
        else if (q < UINT64_C(100000000000000000))
            e10--;
        else
            break;
    }
    for (int i = GUARD_DIGITS - 1; i >= 0; i--) {
        digits[i] = (int)(q % 10);
        q /= 10;
    }
    len = k > 0 ? k : e10 + 1 - k; /* k <= 0: 先頭の数字から小数点以下 -k 桁まで */
    if (len < 1)
        len = 1;
    if (len > MANT_DIGITS)
        len = MANT_DIGITS;
    {
        bool others = rest;

        for (int i = len + 1; i < GUARD_DIGITS; i++)
            others = others || digits[i] != 0;
        inexact = digits[len] != 0 || others;
        half_cmp =
            digits[len] > 5 || (digits[len] == 5 && others) ? 1 : (digits[len] == 5 ? 0 : -1);
    }
    for (int i = 0; i < len; i++)
        kept[i] = digits[i];
    up = round_up_decimal(st->fpcr_round_mode, sign, half_cmp, inexact,
                          (kept[len - 1] & 1) != 0);
    if (up) {
        int i = len - 1;

        while (i >= 0 && kept[i] == 9)
            kept[i--] = 0;
        if (i >= 0) {
            kept[i]++;
        } else { /* 9.99… → 10.0…: 先頭を 1 にして指数を 1 上げる */
            kept[0] = 1;
            e10++;
        }
    }
    if (inexact)
        exc |= (uint8_t)MC68K_FPU_EXC_INEX2;
    if (e10 > EXP_DIGITS_MAX || e10 < -EXP_DIGITS_MAX)
        exc |= (uint8_t)MC68K_FPU_EXC_OPERR;
    put_bcd_words(words, sign, e10, kept);
    fpu_commit_exc(st, exc);
}
