/*
 * fpu_exponent.c — FGETEXP・FGETMAN・FSCALE。
 *
 * どれも指数・仮数の操作だけで、FGETEXP・FGETMAN の結果は厳密（PREC
 * の丸めをかけない。U17）。FSCALE は dst の値を 2
 * の冪倍するので、オーバーフロー・アンダーフローは roundPack が判定する。
 */
#include <stdbool.h>
#include <stdint.h>
#include "fpu_internal.h"

#define EXP_BIAS_X       16383
/* |INT(src)| >= 2^14 は dst の値によらず必ず OVFL/UNFL（[881UM] p.4-93。XEiJ も「MC68882
 * に合わせる」として 同じ扱い）。roundPack に渡す指数を大きく振って、どの丸めモード・PREC
 * でもオーバーフロー/アンダーフローさせる。 */
#define SCALE_FORCE_LOG2 14
#define SCALE_SATURATE   100000

static mc68k_fp80_t operr_result(mc68k_fpu_state_t *st) {
    mc68k_fp80_t r = fpu_default_nan();

    fpu_finish_result(st, r, (uint8_t)MC68K_FPU_EXC_OPERR, 0);
    return r;
}

mc68k_fp80_t mc68k_fpu_getexp(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_inf(src))
        return operr_result(st);
    if (fpu_is_zero(src)) {
        r = fpu_make(fpu_sign(src), 0, 0);
    } else {
        fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
        r = mc68k_fpu_from_l(exp - EXP_BIAS_X);
    }
    fpu_finish_result(st, r, 0, 0);
    return r;
}

mc68k_fp80_t mc68k_fpu_getman(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_inf(src))
        return operr_result(st);
    if (fpu_is_zero(src)) {
        r = fpu_make(fpu_sign(src), 0, 0);
    } else {
        fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
        r = fpu_make(fpu_sign(src), EXP_BIAS_X, sig);
    }
    fpu_finish_result(st, r, 0, 0);
    return r;
}

/* INT(src)（0 方向）。|INT(src)| >= 2^14 は ±SCALE_SATURATE に飽和させる。src は有限。 */
static int32_t scale_count(mc68k_fp80_t src) {
    int32_t exp, u;
    uint64_t sig, mag;

    if (fpu_is_zero(src))
        return 0;
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    u = exp - EXP_BIAS_X; /* 先頭ビットの重み 2^u */
    if (u < 0)
        return 0;
    mag = u >= SCALE_FORCE_LOG2 ? (uint64_t)SCALE_SATURATE : sig >> (63 - u);
    return fpu_sign(src) ? -(int32_t)mag : (int32_t)mag;
}

mc68k_fp80_t mc68k_fpu_scale(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig;
    uint_fast8_t sf;
    bool sign = fpu_sign(dst);
    /* src=±0 は「dst の値を返す」経路で、INEX2 を roundPack の結果から採用する（p.4-93 Note
     * 1）。 それ以外の INEX2 は Cleared（p.4-94）。 */
    const uint8_t force = fpu_is_zero(src) ? 0 : (uint8_t)MC68K_FPU_EXC_INEX2;

    if (fpu_dyadic_nan(dst, src, st, &r))
        return r;
    if (fpu_is_inf(src))
        return operr_result(st);
    if (fpu_is_inf(dst) || fpu_is_zero(dst)) {
        r = fpu_is_zero(dst) ? fpu_make(sign, 0, 0) : dst;
        fpu_finish_result(st, r, 0, 0);
        return r;
    }
    fpu_normalize(fpu_exp_field(dst), dst.signif, &exp, &sig);
    r = fpu_round_pack128(sign, exp + scale_count(src), sig, 0, st, &sf);
    fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf), force);
    return r;
}
