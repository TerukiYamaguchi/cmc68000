/*
 * fpu_core.c — FPU共通コアの状態表現とデータ型変換。
 * 公開APIは include/mc68000/fpu.h、設計は docs/mc68000-internals/reference/fpu.md。
 *
 * 拡張精度の値は、指数フィールド e と仮数 sig から
 *   value = (-1)^s * sig * 2^(e - 16383 - 63)
 * で決まる（e=0 も e>=1 と同じ式。[881UM Table 3-3]）。
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "platform.h"
#include "internals.h"
#include "softfloat.h"
#include "fpu_internal.h"

#define EXP_BIAS_X 16383
#define SIG_BITS   64

/* S/D外部形式の寸法 */
struct binary_fmt {
    int frac_bits; /* フラクションのビット数 */
    int exp_bits;  /* 指数フィールドのビット数 */
    int bias;      /* 指数バイアス */
    uint_fast8_t rounding_precision;
};

static const struct binary_fmt fmt_single = {23, 8, 127, FPU_ROUNDING_PRECISION_S};
static const struct binary_fmt fmt_double = {52, 11, 1023, FPU_ROUNDING_PRECISION_D};

void mc68k_fpu_reset(mc68k_fpu_state_t *st) {
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < 8; i++) {
        st->fp[i] = fpu_make(false, FPU_EXP_MASK, UINT64_MAX);
    }
}

/* 符号付き整数 → 拡張精度。厳密。 */
static mc68k_fp80_t from_int(int64_t v) {
    bool sign = v < 0;
    uint64_t mag = sign ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    int shift;

    if (mag == 0)
        return fpu_make(false, 0, 0);
    shift = fpu_clz64(mag);
    return fpu_make(sign, (uint16_t)(EXP_BIAS_X + (SIG_BITS - 1) - shift), mag << shift);
}

mc68k_fp80_t mc68k_fpu_from_b(int8_t v) {
    return from_int(v);
}
mc68k_fp80_t mc68k_fpu_from_w(int16_t v) {
    return from_int(v);
}
mc68k_fp80_t mc68k_fpu_from_l(int32_t v) {
    return from_int(v);
}

/* 正規化に使う指数（e=0 は非正規化数の e=0 と同じ扱い）から拡張精度値を作る。
 * mant は暗黙の整数部を含む仮数、mant_exp は value = mant * 2^mant_exp の mant_exp。 */
static mc68k_fp80_t from_binary(bool sign, uint64_t mant, int32_t mant_exp) {
    int32_t exp;
    uint64_t sig;

    if (mant == 0)
        return fpu_make(sign, 0, 0);
    fpu_normalize(mant_exp + EXP_BIAS_X + (SIG_BITS - 1), mant, &exp, &sig);
    return fpu_make(sign, (uint16_t)exp, sig);
}

static mc68k_fp80_t from_binary_bits(uint64_t bits, const struct binary_fmt *fmt) {
    int sign_shift = fmt->frac_bits + fmt->exp_bits;
    uint64_t exp_max = (UINT64_C(1) << fmt->exp_bits) - 1;
    bool sign = ((bits >> sign_shift) & 1) != 0;
    uint64_t e = (bits >> fmt->frac_bits) & exp_max;
    uint64_t frac = bits & ((UINT64_C(1) << fmt->frac_bits) - 1);
    int shift_to_x = SIG_BITS - 1 - fmt->frac_bits;

    if (e == exp_max) {
        /* NaNの仮数は上位に詰める。SNaN/quietの区別（フラクション最上位）は保つ。 */
        return fpu_make(sign, FPU_EXP_MASK, FPU_SIG_J | (frac << shift_to_x));
    }
    if (e == 0) {
        return from_binary(sign, frac, 1 - fmt->bias - fmt->frac_bits);
    }
    return from_binary(sign, (UINT64_C(1) << fmt->frac_bits) | frac,
                       (int32_t)e - fmt->bias - fmt->frac_bits);
}

/* stは将来SNaN検出（EXC.SNAN）を記録できるよう受け取るが、from_*自体がSNaNを
 * quiet化・記録するかは一次資料上未確定のため現時点では使わない（design §9。
 * Opus第三者レビューC-N2対応: 以前のコメントは実装と食い違っていた）。 */
mc68k_fp80_t mc68k_fpu_from_s(uint32_t bits, mc68k_fpu_state_t *st) {
    (void)st;
    return from_binary_bits(bits, &fmt_single);
}

mc68k_fp80_t mc68k_fpu_from_d(uint64_t bits, mc68k_fpu_state_t *st) {
    (void)st;
    return from_binary_bits(bits, &fmt_double);
}

mc68k_fp80_t mc68k_fpu_from_x(mc68k_fp80_t v, mc68k_fpu_state_t *st) {
    uint16_t e = fpu_exp_field(v);
    int shift;

    (void)st;
    if (e == FPU_EXP_MASK)
        return v;
    if (v.signif == 0)
        return fpu_make(fpu_sign(v), 0, 0);
    if ((v.signif & FPU_SIG_J) != 0)
        return v;
    /* unnormalized: 指数が許す範囲で左シフトして正規化する。e が足りなければ非正規化数。 */
    shift = fpu_clz64(v.signif);
    if (shift > e)
        shift = e;
    return fpu_make(fpu_sign(v), (uint16_t)(e - shift), v.signif << shift);
}

static mc68k_fp80_t from_softfloat(extFloat80_t r) {
    mc68k_fp80_t v;

    v.sign_exp = r.signExp;
    v.signif = r.signif;
    return v;
}

/* SNaNビット（フラクション最上位ビット）を立てる。[881UM §6.1.2] */
static uint64_t quiet(uint64_t signif) {
    return signif | FPU_SIG_QUIET;
}

mc68k_fp80_t mc68k_fpu_move(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    int32_t exp;
    uint64_t sig;
    mc68k_fp80_t result;
    uint_fast8_t flags;
    uint8_t exc = 0;

    /* NaNはSNaNをquiet化してから返す（[881UM §4.5.4.2, §6.1.2]。FMOVEロードは
     * "only an FMOVEM instruction can write a SNAN" の対象外。jビットは保つ）。 */
    if (fpu_is_nan(src)) {
        if (fpu_is_snan(src))
            exc = (uint8_t)MC68K_FPU_EXC_SNAN;
        fpu_commit_exc(st, exc);
        return fpu_make(fpu_sign(src), fpu_exp_field(src), quiet(src.signif));
    }
    if (fpu_is_inf(src))
        return src; /* OVFL/UNFL/INEX2は常にクリア（design §4.1） */
    if (fpu_is_zero(src))
        return fpu_make(fpu_sign(src), 0, 0);
    /* UNFLはdestinationのPRECに関わらず、source自体がXの非正規化数かだけで決まる
     * （design §4.1・§3.3.1。ストア[to_x/to_s/to_d]のUNFL判定とは別規則）。 */
    if (fpu_src_is_x_denormal(src))
        exc = (uint8_t)MC68K_FPU_EXC_UNFL;
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    fpu_sf_begin(st);
    result = from_softfloat(softfloat_roundPackToExtF80(
        fpu_sign(src), exp, sig, 0, fpu_rounding_precision(st->fpcr_round_prec)));
    flags = fpu_sf_end();
    /* OVFLは常にクリア。INEX2はroundPackのinexactをそのまま採用する（design §4.1。
     * オーバーフロー時のINEX2の扱いは実機未確認のまま推論を採用。§9）。 */
    if (flags & softfloat_flag_inexact)
        exc |= (uint8_t)MC68K_FPU_EXC_INEX2;
    fpu_commit_exc(st, exc);
    return result;
}

/* 非ゼロの有限値を rounding_precision（80/32/64）で丸める。st のPRECは使わない。
 * *out_sf_flags に生のSoftFloatフラグを返す（呼び出し側がEXC byteへ反映する）。 */
mc68k_fp80_t fpu_round_finite(mc68k_fp80_t v, const mc68k_fpu_state_t *st,
                              uint_fast8_t rounding_precision, uint_fast8_t *out_sf_flags) {
    int32_t exp;
    uint64_t sig;
    mc68k_fp80_t result;

    fpu_normalize(fpu_exp_field(v), v.signif, &exp, &sig);
    fpu_sf_begin(st);
    result = from_softfloat(
        softfloat_roundPackToExtF80(fpu_sign(v), exp, sig, 0, rounding_precision));
    *out_sf_flags = fpu_sf_end();
    return result;
}

mc68k_fp80_t mc68k_fpu_to_x(mc68k_fp80_t v, mc68k_fpu_state_t *st) {
    uint_fast8_t flags;
    mc68k_fp80_t result;

    if (fpu_is_nan(v)) {
        fpu_commit_exc(st, fpu_is_snan(v) ? (uint8_t)MC68K_FPU_EXC_SNAN : 0);
        return fpu_make(fpu_sign(v), FPU_EXP_MASK, quiet(v.signif));
    }
    if (fpu_is_inf(v))
        return v;
    if (fpu_is_zero(v))
        return fpu_make(fpu_sign(v), 0, 0);
    result = fpu_round_finite(v, st, FPU_ROUNDING_PRECISION_X, &flags);
    fpu_commit_exc(st, fpu_sf_flags_to_exc(flags));
    return result;
}

static uint64_t make_binary(bool sign, uint64_t exp_field, uint64_t frac,
                            const struct binary_fmt *fmt) {
    return ((uint64_t)sign << (fmt->frac_bits + fmt->exp_bits)) |
           (exp_field << fmt->frac_bits) | frac;
}

/* 丸め済みの拡張精度値 r（PRECision 相当に丸め済みでフラクションの下位ビットが0）を
 * 外部形式のビット列へ切り出す。 */
static uint64_t pack_rounded(mc68k_fp80_t r, const struct binary_fmt *fmt) {
    bool sign = fpu_sign(r);
    uint64_t exp_max = (UINT64_C(1) << fmt->exp_bits) - 1;
    uint64_t frac_mask = (UINT64_C(1) << fmt->frac_bits) - 1;
    int32_t e = fpu_exp_field(r) - EXP_BIAS_X;
    int32_t min_norm = 1 - fmt->bias;

    if (fpu_exp_field(r) == FPU_EXP_MASK)
        return make_binary(sign, exp_max, 0, fmt);
    if (r.signif == 0)
        return make_binary(sign, 0, 0, fmt);
    if (e >= min_norm) {
        return make_binary(sign, (uint64_t)(e + fmt->bias),
                           (r.signif >> (SIG_BITS - 1 - fmt->frac_bits)) & frac_mask, fmt);
    }
    return make_binary(sign, 0,
                       r.signif >> (SIG_BITS - 1 - (e + fmt->bias - 1 + fmt->frac_bits)), fmt);
}

static uint64_t to_binary(mc68k_fp80_t v, mc68k_fpu_state_t *st, const struct binary_fmt *fmt) {
    uint64_t exp_max = (UINT64_C(1) << fmt->exp_bits) - 1;
    uint64_t frac_mask = (UINT64_C(1) << fmt->frac_bits) - 1;
    uint_fast8_t flags;
    mc68k_fp80_t rounded;

    if (fpu_is_nan(v)) {
        fpu_commit_exc(st, fpu_is_snan(v) ? (uint8_t)MC68K_FPU_EXC_SNAN : 0);
        return make_binary(fpu_sign(v), exp_max,
                           (quiet(v.signif) >> (SIG_BITS - 1 - fmt->frac_bits)) & frac_mask,
                           fmt);
    }
    if (fpu_is_inf(v))
        return make_binary(fpu_sign(v), exp_max, 0, fmt);
    if (fpu_is_zero(v))
        return make_binary(fpu_sign(v), 0, 0, fmt);
    /* ストアのUNFL/OVFL/INEX2は算術演算と同じtininess判定（design §4.1・§6.2）なので
     * roundPackの生フラグをそのままEXC byteへ写像する。OPERRはS/D/X destinationでは
     * 常にクリア（[881UM §6.1.3]）なので反映しない。 */
    rounded = fpu_round_finite(v, st, fmt->rounding_precision, &flags);
    fpu_commit_exc(st, fpu_sf_flags_to_exc(flags));
    return pack_rounded(rounded, fmt);
}

uint32_t mc68k_fpu_to_s(mc68k_fp80_t v, mc68k_fpu_state_t *st) {
    return (uint32_t)to_binary(v, st, &fmt_single);
}

uint64_t mc68k_fpu_to_d(mc68k_fp80_t v, mc68k_fpu_state_t *st) {
    return to_binary(v, st, &fmt_double);
}

/* 整数への丸め。magnitude が 2^64 以上になるときは false。*out_inexactに
 * 丸めで値が変わったか（design §6.1.7「選択した丸め精度**または destination data
 * format**で正確に表現できない」。B/W/L destinationのINEX2判定に使う）を返す。 */
static bool round_to_magnitude(bool sign, int32_t exp, uint64_t sig, uint8_t mode,
                               uint64_t *mag, bool *out_inexact) {
    int32_t e = exp - EXP_BIAS_X;
    uint64_t whole, rem, half;
    int shift;
    bool inexact, up;
    int half_cmp;

    if (e >= SIG_BITS)
        return false;
    if (e == SIG_BITS - 1) {
        *mag = sig;
        *out_inexact = false;
        return true;
    }
    shift = (SIG_BITS - 1) - e;
    if (shift > SIG_BITS) {
        whole = 0;
        half_cmp = -1;
        inexact = true;
    } else if (shift == SIG_BITS) {
        whole = 0;
        half_cmp = sig == FPU_SIG_J ? 0 : 1;
        inexact = true;
    } else {
        whole = sig >> shift;
        rem = sig & ((UINT64_C(1) << shift) - 1);
        half = UINT64_C(1) << (shift - 1);
        inexact = rem != 0;
        half_cmp = rem > half ? 1 : (rem == half ? 0 : -1);
    }
    switch (mode) {
    case MC68K_FPU_ROUND_ZERO:
        up = false;
        break;
    case MC68K_FPU_ROUND_MINUS_INF:
        up = sign && inexact;
        break;
    case MC68K_FPU_ROUND_PLUS_INF:
        up = !sign && inexact;
        break;
    default:
        up = half_cmp > 0 || (half_cmp == 0 && (whole & 1) != 0);
        break;
    }
    *mag = whole + (up ? 1 : 0);
    *out_inexact = inexact;
    return true;
}

/* B/W/Lへのストア。戻り値は [-2^(bits-1), 2^(bits-1)-1] の符号付き整数。
 * NaN は仮数上位 bits ビットを符号付きに解釈した値、範囲外・無限大は最大/最小整数。
 * OPERR は非シグナリングNaN・±Inf・整数レンジ超過で立つ（[881UM Table 6-2]「FMOVE to
 * B,W,or L」の条件は文字どおり"Non-Signaling NAN"。§6.1.2〔p.6-6〕はSNaN→B/W/Lを
 * 「SNANビットを立てて仮数上位ビットを書く」だけの自己完結した経路として記述し、
 * OPERRに触れない。実測済み）。SNaN入力はSNANビットのみでOPERRは立たない。 */
static int64_t to_integer(mc68k_fp80_t v, mc68k_fpu_state_t *st, int bits) {
    int64_t max = (INT64_C(1) << (bits - 1)) - 1;
    int64_t min = -max - 1;
    bool sign = fpu_sign(v);
    int32_t exp;
    uint64_t sig, mag, limit;
    uint8_t exc;

    if (fpu_is_nan(v)) {
        /* 仮数の上位 bits ビット（SNaNビットを立てたもの）。 */
        uint64_t top = quiet(v.signif) >> (SIG_BITS - bits);

        exc = fpu_is_snan(v) ? (uint8_t)MC68K_FPU_EXC_SNAN : (uint8_t)MC68K_FPU_EXC_OPERR;
        fpu_commit_exc(st, exc);
        return top > (uint64_t)max ? (int64_t)top - (INT64_C(1) << bits) : (int64_t)top;
    }
    if (fpu_is_inf(v)) {
        fpu_commit_exc(st, (uint8_t)MC68K_FPU_EXC_OPERR);
        return sign ? min : max;
    }
    if (fpu_is_zero(v))
        return 0;
    fpu_normalize(fpu_exp_field(v), v.signif, &exp, &sig);
    limit = sign ? (uint64_t)max + 1 : (uint64_t)max;
    {
        bool inexact;

        if (!round_to_magnitude(sign, exp, sig, st->fpcr_round_mode, &mag, &inexact) ||
            mag > limit) {
            fpu_commit_exc(st, (uint8_t)MC68K_FPU_EXC_OPERR);
            return sign ? min : max;
        }
        /* [881UM §6.1.7]「選択した丸め精度または destination data format で正確に
         * 表現できない」。整数への丸めで端数が切り捨てられた場合はINEX2（Opus第三者
         * レビュー C-B3対応。オーバーフロー[上のOPERR分岐]ではINEX2を立てない）。 */
        if (inexact)
            fpu_commit_exc(st, (uint8_t)MC68K_FPU_EXC_INEX2);
    }
    return sign ? -(int64_t)mag : (int64_t)mag;
}

int8_t mc68k_fpu_to_b(mc68k_fp80_t v, mc68k_fpu_state_t *st) {
    return (int8_t)to_integer(v, st, 8);
}

int16_t mc68k_fpu_to_w(mc68k_fp80_t v, mc68k_fpu_state_t *st) {
    return (int16_t)to_integer(v, st, 16);
}

int32_t mc68k_fpu_to_l(mc68k_fp80_t v, mc68k_fpu_state_t *st) {
    return (int32_t)to_integer(v, st, 32);
}
