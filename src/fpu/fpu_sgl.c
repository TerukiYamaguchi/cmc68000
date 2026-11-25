/*
 * fpu_sgl.c — FSGLMUL・FSGLDIV。
 *
 * FPCR の丸め精度を無視し（丸めモードは使う）、入力の仮数を有効数字 24bit
 * へ切り詰めて乗除算し、結果は 「仮数だけ 24bit・指数範囲は拡張精度」の S_XR 書式（§5.1-5）で 1
 * 回だけ丸める。切り詰めた仮数の積・商は 64bit 整数に収まるので、SoftFloat
 * を使わず整数演算で正確に求める。
 */
#include <stdbool.h>
#include <stdint.h>
#include "fpu_internal.h"

#define SGL_DROP_BITS 40u /* 64bit 仮数から捨てる下位ビット数（残り 24bit） */
#define SGL_MUL_LEAD  17  /* 積（47〜48bit）を bit63 へ寄せるシフト量の基準 */
#define SGL_DIV_LEAD  23  /* 商（40〜41bit）を bit63 へ寄せるシフト量の基準 */
#define EXP_BIAS_X    16383

/* 非ゼロの有限値を正規化し、仮数を有効数字 24bit へ切り詰める（0 方向。設計書 O6・U19）。 */
static void truncate24(mc68k_fp80_t v, int32_t *exp, uint64_t *sig24) {
    uint64_t sig;

    fpu_normalize(fpu_exp_field(v), v.signif, exp, &sig);
    *sig24 = sig >> SGL_DROP_BITS;
}

static mc68k_fp80_t finish_pack(mc68k_fpu_state_t *st, bool sign, int32_t exp, uint64_t hi,
                                uint64_t lo) {
    uint_fast8_t sf;
    mc68k_fp80_t r =
        fpu_round_pack128_prec(sign, exp, hi, lo, st, FPU_ROUNDING_PRECISION_S_XR, &sf);

    fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf), 0);
    return r;
}

static mc68k_fp80_t finish_special(mc68k_fpu_state_t *st, mc68k_fp80_t r, uint8_t exc) {
    fpu_finish_result(st, r, exc, 0);
    return r;
}

mc68k_fp80_t mc68k_fpu_sglmul(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    bool sign = fpu_sign(dst) != fpu_sign(src);
    int32_t ea, eb;
    uint64_t a, b, p;
    int lead;

    if (fpu_dyadic_nan(dst, src, st, &r))
        return r;
    if (fpu_is_inf(dst) || fpu_is_inf(src)) {
        if (fpu_is_zero(dst) || fpu_is_zero(src)) {
            return finish_special(st, fpu_default_nan(), (uint8_t)MC68K_FPU_EXC_OPERR);
        }
        return finish_special(st, fpu_make(sign, FPU_EXP_MASK, FPU_SIG_J), 0);
    }
    if (fpu_is_zero(dst) || fpu_is_zero(src))
        return finish_special(st, fpu_make(sign, 0, 0), 0);
    truncate24(dst, &ea, &a);
    truncate24(src, &eb, &b);
    p = a * b; /* 各 24bit なので 48bit 以内で厳密 */
    lead = fpu_clz64(p);
    return finish_pack(st, sign, ea + eb - EXP_BIAS_X + SGL_MUL_LEAD - lead, p << lead, 0);
}

mc68k_fp80_t mc68k_fpu_sgldiv(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    bool sign = fpu_sign(dst) != fpu_sign(src);
    int32_t ea, eb;
    uint64_t a, b, q1, r1, q2, r2, hi, lo;
    int lead;
    unsigned q2_low;

    if (fpu_dyadic_nan(dst, src, st, &r))
        return r;
    if (fpu_is_inf(dst)) {
        if (fpu_is_inf(src))
            return finish_special(st, fpu_default_nan(), (uint8_t)MC68K_FPU_EXC_OPERR);
        return finish_special(st, fpu_make(sign, FPU_EXP_MASK, FPU_SIG_J),
                              0); /* ∞÷0 は DZ を立てない */
    }
    if (fpu_is_inf(src))
        return finish_special(st, fpu_make(sign, 0, 0), 0);
    if (fpu_is_zero(src)) {
        if (fpu_is_zero(dst))
            return finish_special(st, fpu_default_nan(), (uint8_t)MC68K_FPU_EXC_OPERR);
        return finish_special(st, fpu_make(sign, FPU_EXP_MASK, FPU_SIG_J),
                              (uint8_t)MC68K_FPU_EXC_DZ);
    }
    if (fpu_is_zero(dst))
        return finish_special(st, fpu_make(sign, 0, 0), 0);
    truncate24(dst, &ea, &a);
    truncate24(src, &eb, &b);
    /* 商は 2 段（各 40bit）で 80〜81bit まで求める。仮数は 24bit で丸めるが、tiny
     * の領域は拡張精度の量子 （最大 64bit）で丸めるので、41bit の商だけでは足りない。余りは
     * sticky に畳む。 */
    q1 = (a << SGL_DROP_BITS) / b;
    r1 = (a << SGL_DROP_BITS) % b;
    q2 = (r1 << SGL_DROP_BITS) / b;
    r2 = (r1 << SGL_DROP_BITS) % b;
    lead = fpu_clz64(q1); /* q1 は 40〜41bit。商 (q1:q2) の上位 64bit が hi */
    q2_low = SGL_DROP_BITS - (unsigned)lead; /* q2 のうち lo へ回る下位ビット数（16 か 17） */
    hi = (q1 << lead) | (q2 >> q2_low);
    lo = (q2 << (64u - q2_low)) | (r2 != 0 ? 1u : 0u);
    return finish_pack(st, sign, ea - eb + EXP_BIAS_X + SGL_DIV_LEAD - lead, hi, lo);
}
