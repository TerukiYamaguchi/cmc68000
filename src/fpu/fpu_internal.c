/* fpu_internal.c — FPU共通コアの内部ヘルパー。宣言は fpu_internal.h。 */
#include "fpu_internal.h"
#include <stddef.h>
#include "platform.h"
#include "internals.h"
#include "softfloat.h"
#include "mc68k_unrounded.h"

int fpu_clz64(uint64_t sig) {
    int n = 0;

    if (sig == 0)
        return 64;
    while ((sig & FPU_SIG_J) == 0) {
        sig <<= 1;
        n++;
    }
    return n;
}

void fpu_normalize(int32_t exp, uint64_t sig, int32_t *out_exp, uint64_t *out_sig) {
    if (sig == 0) {
        *out_exp = 0;
        *out_sig = 0;
        return;
    }
    int shift = fpu_clz64(sig);
    *out_exp = exp - shift;
    *out_sig = sig << shift;
}

uint_fast8_t fpu_rounding_precision(uint8_t prec) {
    switch (prec) {
    case MC68K_FPU_PREC_SINGLE:
        return FPU_ROUNDING_PRECISION_S;
    case MC68K_FPU_PREC_DOUBLE:
        return FPU_ROUNDING_PRECISION_D;
    default:
        return FPU_ROUNDING_PRECISION_X;
    }
}

static uint_fast8_t softfloat_rounding_mode(uint8_t mode) {
    switch (mode) {
    case MC68K_FPU_ROUND_ZERO:
        return softfloat_round_minMag;
    case MC68K_FPU_ROUND_MINUS_INF:
        return softfloat_round_min;
    case MC68K_FPU_ROUND_PLUS_INF:
        return softfloat_round_max;
    default:
        return softfloat_round_near_even;
    }
}

void fpu_sf_begin(const mc68k_fpu_state_t *st) {
    softfloat_roundingMode = softfloat_rounding_mode(st->fpcr_round_mode);
    softfloat_detectTininess = softfloat_tininess_beforeRounding;
    softfloat_exceptionFlags = 0;
    mc68k_last_unrounded.valid = false;
}

uint_fast8_t fpu_sf_end(void) {
    uint_fast8_t flags = softfloat_exceptionFlags;
    softfloat_exceptionFlags = 0;
    return flags;
}

void fpu_set_cc(mc68k_fpu_state_t *st, mc68k_fp80_t result) {
    uint8_t cc = fpu_sign(result) ? (uint8_t)MC68K_FPU_CC_N : 0;

    if (fpu_is_nan(result))
        cc |= MC68K_FPU_CC_NAN;
    else if (fpu_is_inf(result))
        cc |= MC68K_FPU_CC_I;
    else if (fpu_is_zero(result))
        cc |= MC68K_FPU_CC_Z;
    st->fpsr_cc = cc;
}

uint8_t fpu_sf_flags_to_exc(uint_fast8_t sf_flags) {
    uint8_t exc = 0;

    if (sf_flags & softfloat_flag_inexact)
        exc |= (uint8_t)MC68K_FPU_EXC_INEX2;
    if (sf_flags & softfloat_flag_underflow)
        exc |= (uint8_t)MC68K_FPU_EXC_UNFL;
    if (sf_flags & softfloat_flag_overflow)
        exc |= (uint8_t)MC68K_FPU_EXC_OVFL;
    return exc;
}

bool fpu_src_is_x_denormal(mc68k_fp80_t v) {
    int32_t exp;
    uint64_t sig;

    fpu_normalize(fpu_exp_field(v), v.signif, &exp, &sig);
    return exp < 0;
}

void fpu_commit_exc(mc68k_fpu_state_t *st, uint8_t bits) {
    st->fpsr_exc |= bits;
    if (st->fpsr_exc & (MC68K_FPU_EXC_BSUN | MC68K_FPU_EXC_SNAN | MC68K_FPU_EXC_OPERR)) {
        st->fpsr_aexc |= MC68K_FPU_AEXC_IOP;
    }
    if (st->fpsr_exc & MC68K_FPU_EXC_OVFL)
        st->fpsr_aexc |= MC68K_FPU_AEXC_OVFL;
    if ((st->fpsr_exc & MC68K_FPU_EXC_UNFL) && (st->fpsr_exc & MC68K_FPU_EXC_INEX2)) {
        st->fpsr_aexc |= MC68K_FPU_AEXC_UNFL;
    }
    if (st->fpsr_exc & MC68K_FPU_EXC_DZ)
        st->fpsr_aexc |= MC68K_FPU_AEXC_DZ;
    if (st->fpsr_exc & (MC68K_FPU_EXC_INEX1 | MC68K_FPU_EXC_INEX2 | MC68K_FPU_EXC_OVFL)) {
        st->fpsr_aexc |= MC68K_FPU_AEXC_INEX;
    }
}

bool fpu_monadic_nan(mc68k_fp80_t src, mc68k_fpu_state_t *st, mc68k_fp80_t *out) {
    if (!fpu_is_nan(src))
        return false;
    if (fpu_is_snan(src))
        fpu_commit_exc(st, (uint8_t)MC68K_FPU_EXC_SNAN);
    *out = fpu_make(fpu_sign(src), fpu_exp_field(src), fpu_quiet_signif(src.signif));
    fpu_set_cc(st, *out);
    return true;
}

bool fpu_dyadic_nan(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st,
                    mc68k_fp80_t *out) {
    mc68k_fp80_t pick;

    if (!fpu_is_nan(dst) && !fpu_is_nan(src))
        return false;
    if (fpu_is_snan(dst) || fpu_is_snan(src))
        fpu_commit_exc(st, (uint8_t)MC68K_FPU_EXC_SNAN);
    pick = fpu_is_nan(dst) ? dst : src;
    *out = fpu_make(fpu_sign(pick), fpu_exp_field(pick), fpu_quiet_signif(pick.signif));
    fpu_set_cc(st, *out);
    return true;
}

mc68k_fp80_t fpu_round_pack128_prec(bool sign, int32_t exp, uint64_t hi, uint64_t lo,
                                    const mc68k_fpu_state_t *st,
                                    uint_fast8_t rounding_precision,
                                    uint_fast8_t *out_sf_flags) {
    extFloat80_t r;

    fpu_sf_begin(st);
    r = softfloat_roundPackToExtF80(sign, exp, hi, lo, rounding_precision);
    *out_sf_flags = fpu_sf_end();
    return fpu_make((r.signExp & FPU_SIGN_BIT) != 0, (uint16_t)(r.signExp & FPU_EXP_MASK),
                    r.signif);
}

mc68k_fp80_t fpu_round_pack128(bool sign, int32_t exp, uint64_t hi, uint64_t lo,
                               const mc68k_fpu_state_t *st, uint_fast8_t *out_sf_flags) {
    return fpu_round_pack128_prec(sign, exp, hi, lo, st,
                                  fpu_rounding_precision(st->fpcr_round_prec), out_sf_flags);
}

void fpu_finish_result(mc68k_fpu_state_t *st, mc68k_fp80_t result, uint8_t exc_bits,
                       uint8_t force_clear) {
    fpu_commit_exc(st, (uint8_t)(exc_bits & ~force_clear));
    fpu_set_cc(st, result);
}

/* softfloat の丸めモード → mc68k_fpu_round_mode_t（softfloat_rounding_mode() の逆写像） */
static uint8_t mc68k_round_mode(uint8_t sf_mode) {
    switch (sf_mode) {
    case softfloat_round_minMag:
        return MC68K_FPU_ROUND_ZERO;
    case softfloat_round_min:
        return MC68K_FPU_ROUND_MINUS_INF;
    case softfloat_round_max:
        return MC68K_FPU_ROUND_PLUS_INF;
    default:
        return MC68K_FPU_ROUND_NEAREST;
    }
}

void mc68k_fpu_unrounded_reset(void) {
    mc68k_last_unrounded.valid = false;
}

bool mc68k_fpu_unrounded_get(mc68k_fpu_unrounded_t *out) {
    if (out == NULL || !mc68k_last_unrounded.valid)
        return false;
    out->valid = true;
    out->sign = mc68k_last_unrounded.sign;
    out->exp = mc68k_last_unrounded.exp;
    out->sig = mc68k_last_unrounded.sig;
    out->sig_extra = mc68k_last_unrounded.sig_extra;
    out->mantissa_bits = mc68k_last_unrounded.mantissa_bits;
    out->round_mode = mc68k_round_mode(mc68k_last_unrounded.mode);
    return true;
}

void mc68k_fpu_unrounded_round_mantissa(const mc68k_fpu_unrounded_t *in,
                                        mc68k_fpu_unrounded_t *out) {
    struct mc68k_unrounded u;

    u.valid = in->valid;
    u.sign = in->sign;
    u.exp = in->exp;
    u.sig = in->sig;
    u.sig_extra = in->sig_extra;
    u.mantissa_bits = in->mantissa_bits;
    u.mode = (uint8_t)softfloat_rounding_mode(in->round_mode);
    mc68k_unrounded_round_mantissa(&u);
    *out = *in;
    out->exp = u.exp;
    out->sig = u.sig;
    out->sig_extra = 0;
}
