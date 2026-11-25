/*
 * s_roundPackToExtF80.c — softfloat_roundPackToExtF80() の自前実装。
 *
 * Berkeley SoftFloat 3e（pin a0c6494）の同名関数と同じシグネチャで、CMake が
 * submodule 側の s_roundPackToExtF80.c の代わりにこちらをリンクする。SoftFloat の
 * 実装は x87 の規約（非正規化数の指数を 1-bias 基準で扱い、S/D精度では拡張精度自身の
 * 指数範囲で固定ビット位置の丸めをする）で、MC68881 とは次の点が異なるため使えない
 * :
 *   - Motorola の拡張精度は指数フィールド e=0 を正規化数（j=1）と非正規化数（j=0）の
 *     両方に使い、e=1 への繰り上げがない（[881UM Table 3-3]）。
 *   - PREC=S/D は対象精度自身の最小指数・最大指数・有効ビット数で一段階に丸める。
 *
 * 引数 exp は「アイデアライズされた」正規化指数（value = sig*2^(exp-16383-63)。生の
 * e=0,j=1 が exp=0、非正規化数が exp<0）。sig のbit63は1（ゼロ以外の場合。事前条件）。
 * sig==0 は事前条件違反だが、sigExtra も0なら符号付きゼロを返す。
 * 丸めモードは softfloat_roundingMode、tininess は常に丸め前とする。
 *
 * 例外は SoftFloat のフラグに記録する。underflow は「丸め前の指数が対象精度の最小指数を
 * 下回った」ことを、丸めの有無（exact tiny）に関わらず記録する。inexact は丸めで値が
 * 変わったとき、overflow は overflow と inexact を立てる。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "platform.h"
#include "internals.h"
#include "softfloat.h"
#include "mc68k_unrounded.h"

#define SIG_J    UINT64_C(0x8000000000000000)
#define SIG_HALF UINT64_C(0x8000000000000000)

#define EXP_SPECIAL    0x7FFF
#define X_MIN_NORM_EXP 0
#define X_MAX_EXP      0x7FFE

/* 仮数だけを24/53bitへ丸め、指数範囲は拡張精度のままにする書式（FMOVECR の
 * PREC=S/D、FSGLMUL/FSGLDIV）。fpu_internal.h の FPU_ROUNDING_PRECISION_S_XR/D_XR と同じ値。 */
#define PREC_S_XR 96
#define PREC_D_XR 97

/* S/D精度の指数・有効ビット数の定数。exp は拡張精度の符号化のまま比較する。 */
struct round_fmt {
    int64_t min_norm_exp; /* 最小の正規化数の指数。これを下回るとtiny */
    int64_t bits;         /* 有効ビット数（整数部を含む） */
    int64_t max_exp;      /* 最大有限値の指数 */
    uint64_t max_sig;     /* 最大有限値の仮数 */
};

static const struct round_fmt fmt_single = {0x3F81, 24, 0x407E, UINT64_C(0xFFFFFF0000000000)};
static const struct round_fmt fmt_double = {0x3C01, 53, 0x43FE, UINT64_C(0xFFFFFFFFFFFFF800)};

/* 拡張精度の指数範囲で仮数だけを丸める書式。exp>=0 の範囲では tiny にならない
 * （min_norm_exp=0）。exp<0（tiny）は拡張精度の量子で丸めるので round_pack_x へ回す。
 * オーバーフロー時の最大有限値は拡張精度の仮数（全ビット1）で、PREC=X と同じ結果になる
 * （[881UM] p.4-17: FSGLMUL/FSGLDIV は OVFL 時に "an extended precision number with an
 * extended-precision mantissa value" を返す）。 */
static const struct round_fmt fmt_single_xr = {0, 24, X_MAX_EXP, UINT64_MAX};
static const struct round_fmt fmt_double_xr = {0, 53, X_MAX_EXP, UINT64_MAX};

static extFloat80_t pack(bool sign, uint_fast16_t exp, uint64_t sig) {
    union {
        struct extFloat80M s;
        extFloat80_t f;
    } uZ;

    uZ.s.signExp = (uint16_t)((sign ? 0x8000u : 0u) | exp);
    uZ.s.signif = sig;
    return uZ.f;
}

/* 余り（丸め位置より下の全ビット）と丸めビット位置の半分との比較結果。
 * half_cmp: 1=半分超、0=ちょうど半分、-1=半分未満。inexact: 余りが非ゼロ。
 * lsb_odd: 保持する最下位ビットが1（RNのタイ判定用）。 */
static bool need_increment(bool sign, uint_fast8_t mode, bool inexact, int half_cmp,
                           bool lsb_odd) {
    switch (mode) {
    case softfloat_round_minMag:
        return false;
    case softfloat_round_min:
        return sign && inexact;
    case softfloat_round_max:
        return !sign && inexact;
    default:
        return half_cmp > 0 || (half_cmp == 0 && lsb_odd);
    }
}

/* [881UM §6.1.4] 丸めモード×符号のオーバーフロー結果 */
static extFloat80_t overflow_result(bool sign, uint_fast8_t mode, int64_t max_exp,
                                    uint64_t max_sig) {
    bool to_infinity;

    softfloat_raiseFlags(softfloat_flag_overflow | softfloat_flag_inexact);
    switch (mode) {
    case softfloat_round_minMag:
        to_infinity = false;
        break;
    case softfloat_round_min:
        to_infinity = sign;
        break;
    case softfloat_round_max:
        to_infinity = !sign;
        break;
    default:
        to_infinity = true;
        break;
    }
    if (to_infinity)
        return pack(sign, EXP_SPECIAL, SIG_J);
    return pack(sign, (uint_fast16_t)max_exp, max_sig);
}

/* 拡張精度（PREC=X）。exp<0 は Motorola の非正規化域で、e=0 のまま右シフトして
 * 丸める（e=1 への繰り上げはしない）。 */
static extFloat80_t round_pack_x(bool sign, int64_t exp, uint64_t sig, uint64_t sigExtra,
                                 uint_fast8_t mode) {
    struct uint64_extra shifted;
    int64_t dist;
    bool inexact, up;
    int half_cmp;

    if (exp > X_MAX_EXP) {
        return overflow_result(sign, mode, X_MAX_EXP, UINT64_MAX);
    }
    if (exp < X_MIN_NORM_EXP) {
        softfloat_raiseFlags(softfloat_flag_underflow);
        /* 距離65以上は全ビットが sticky に畳まれるだけなので、65で打ち切る */
        dist = X_MIN_NORM_EXP - exp;
        if (dist > 65)
            dist = 65;
        shifted = softfloat_shiftRightJam64Extra(sig, sigExtra, (uint_fast32_t)dist);
        sig = shifted.v;
        sigExtra = shifted.extra;
        exp = X_MIN_NORM_EXP;
    }
    inexact = sigExtra != 0;
    half_cmp = sigExtra > SIG_HALF ? 1 : (sigExtra == SIG_HALF ? 0 : -1);
    up = need_increment(sign, mode, inexact, half_cmp, (sig & 1) != 0);
    if (inexact)
        softfloat_raiseFlags(softfloat_flag_inexact);
    if (up) {
        sig++;
        if (sig == 0) {
            sig = SIG_J;
            exp++;
        }
    }
    if (exp > X_MAX_EXP) {
        return overflow_result(sign, mode, X_MAX_EXP, UINT64_MAX);
    }
    return pack(sign, (uint_fast16_t)exp, sig);
}

/* 単精度・倍精度（PREC=S/D）。結果は拡張精度の正規化数として返し（指数は再マップ
 * しない）、仮数だけを対象精度の有効ビット数に丸める。tiny のときは保持できる
 * ビット数 keep が減り、0以下なら丸め結果は 0 か最小非正規化値になる。 */
static extFloat80_t round_pack_sd(bool sign, int64_t exp, uint64_t sig, uint64_t sigExtra,
                                  uint_fast8_t mode, const struct round_fmt *fmt) {
    int64_t keep = fmt->bits;
    bool tiny = exp < fmt->min_norm_exp;
    bool inexact, up;
    int half_cmp;
    int drop;
    uint64_t low_mask, half;

    if (exp > fmt->max_exp) {
        return overflow_result(sign, mode, fmt->max_exp, fmt->max_sig);
    }
    if (tiny) {
        softfloat_raiseFlags(softfloat_flag_underflow);
        keep = fmt->bits - (fmt->min_norm_exp - exp);
    }
    if (keep <= 0) {
        /* 保持ビットが無い。keep==0 は sig のbit63が丸めビット、それ以下が sticky。
         * keep<0 は全体が sticky（半分未満）。 */
        if (keep == 0) {
            bool round_bit = (sig & SIG_J) != 0;
            bool sticky = ((sig << 1) | sigExtra) != 0;
            half_cmp = !round_bit ? -1 : (sticky ? 1 : 0);
        } else {
            half_cmp = -1;
        }
        softfloat_raiseFlags(softfloat_flag_inexact);
        up = need_increment(sign, mode, true, half_cmp, false);
        if (!up)
            return pack(sign, 0, 0);
        return pack(sign, (uint_fast16_t)(fmt->min_norm_exp - (fmt->bits - 1)), SIG_J);
    }

    drop = (int)(64 - keep);
    low_mask = (UINT64_C(1) << drop) - 1;
    half = UINT64_C(1) << (drop - 1);
    inexact = (sig & low_mask) != 0 || sigExtra != 0;
    if ((sig & low_mask) > half)
        half_cmp = 1;
    else if ((sig & low_mask) == half)
        half_cmp = sigExtra != 0 ? 1 : 0;
    else
        half_cmp = -1;
    up = need_increment(sign, mode, inexact, half_cmp, ((sig >> drop) & 1) != 0);
    if (inexact)
        softfloat_raiseFlags(softfloat_flag_inexact);
    sig &= ~low_mask;
    if (up) {
        sig += UINT64_C(1) << drop;
        if (sig == 0) {
            sig = SIG_J;
            exp++;
        }
    }
    if (exp > fmt->max_exp) {
        return overflow_result(sign, mode, fmt->max_exp, fmt->max_sig);
    }
    return pack(sign, (uint_fast16_t)exp, sig);
}

THREAD_LOCAL struct mc68k_unrounded mc68k_last_unrounded;

static uint8_t mantissa_bits_of(uint_fast8_t roundingPrecision) {
    switch (roundingPrecision) {
    case 32:
    case PREC_S_XR:
        return 24;
    case 64:
    case PREC_D_XR:
        return 53;
    default:
        return 64;
    }
}

void mc68k_unrounded_round_mantissa(struct mc68k_unrounded *u) {
    int drop = 64 - u->mantissa_bits;
    uint64_t low_mask = drop == 0 ? 0 : (UINT64_C(1) << drop) - 1;
    uint64_t kept_lsb = UINT64_C(1) << drop;
    uint64_t low = u->sig & low_mask;
    bool inexact;
    int half_cmp;

    if (drop == 0) {
        inexact = u->sig_extra != 0;
        half_cmp = u->sig_extra > SIG_HALF ? 1 : (u->sig_extra == SIG_HALF ? 0 : -1);
    } else {
        uint64_t half = UINT64_C(1) << (drop - 1);

        inexact = low != 0 || u->sig_extra != 0;
        if (low > half)
            half_cmp = 1;
        else if (low == half)
            half_cmp = u->sig_extra != 0 ? 1 : 0;
        else
            half_cmp = -1;
    }
    if (need_increment(u->sign, u->mode, inexact, half_cmp, (u->sig & kept_lsb) != 0)) {
        u->sig = (u->sig & ~low_mask) + kept_lsb;
        if (u->sig == 0) {
            u->sig = SIG_J;
            u->exp++;
        }
    } else {
        u->sig &= ~low_mask;
    }
    u->sig_extra = 0;
}

extFloat80_t softfloat_roundPackToExtF80(bool sign, int_fast32_t exp, uint_fast64_t sig,
                                         uint_fast64_t sigExtra,
                                         uint_fast8_t roundingPrecision) {
    uint_fast8_t mode = softfloat_roundingMode;

    if (sig == 0) {
        assert(sigExtra == 0);
        return pack(sign, 0, 0);
    }
    assert((sig & SIG_J) != 0);
    mc68k_last_unrounded.valid = true;
    mc68k_last_unrounded.sign = sign;
    mc68k_last_unrounded.exp = (int32_t)exp;
    mc68k_last_unrounded.sig = sig;
    mc68k_last_unrounded.sig_extra = sigExtra;
    mc68k_last_unrounded.mantissa_bits = mantissa_bits_of(roundingPrecision);
    mc68k_last_unrounded.mode = (uint8_t)mode;
    switch (roundingPrecision) {
    case 32:
        return round_pack_sd(sign, (int64_t)exp, sig, sigExtra, mode, &fmt_single);
    case 64:
        return round_pack_sd(sign, (int64_t)exp, sig, sigExtra, mode, &fmt_double);
    case PREC_S_XR:
        if (exp < X_MIN_NORM_EXP)
            return round_pack_x(sign, (int64_t)exp, sig, sigExtra, mode);
        return round_pack_sd(sign, (int64_t)exp, sig, sigExtra, mode, &fmt_single_xr);
    case PREC_D_XR:
        if (exp < X_MIN_NORM_EXP)
            return round_pack_x(sign, (int64_t)exp, sig, sigExtra, mode);
        return round_pack_sd(sign, (int64_t)exp, sig, sigExtra, mode, &fmt_double_xr);
    default:
        return round_pack_x(sign, (int64_t)exp, sig, sigExtra, mode);
    }
}
