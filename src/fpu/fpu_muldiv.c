/*
 * fpu_muldiv.c — FMUL/FDIV共通コアの乗除算。
 * 公開APIは include/mc68000/fpu.h、設計は docs/mc68000-internals/reference/fpu.md。
 *
 * 乗除算そのものはsoftfloat_target/extF80_mul.c・extF80_div.c（submodule側の同名ファイルの
 * exp=0エントリ処理の欠陥を修正した改変コピー）に委ねる。NaN伝播・OPERR（0×∞・0/0・∞/∞）・
 * 無限大の扱いはこれらの関数が内部で正しく処理する（NaN伝播はsoftfloat_target/
 * s_propagateNaNExtF80UI.cの68881固有dst優先規則版を経由する。呼び出しは必ず
 * 第1引数にdst、第2引数にsrcを渡すこと）。
 */
#include <stdint.h>
#include "platform.h"
#include "internals.h"
#include "softfloat.h"
#include "fpu_internal.h"

static extFloat80_t to_ext80(mc68k_fp80_t v) {
    extFloat80_t r;

    r.signExp = v.sign_exp;
    r.signif = v.signif;
    return r;
}

static mc68k_fp80_t from_ext80(extFloat80_t r) {
    mc68k_fp80_t v;

    v.sign_exp = r.signExp;
    v.signif = r.signif;
    return v;
}

mc68k_fp80_t mc68k_fpu_mul(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    uint_fast8_t flags;
    uint8_t exc;

    if (fpu_is_snan(dst) || fpu_is_snan(src))
        fpu_commit_exc(st, (uint8_t)MC68K_FPU_EXC_SNAN);
    fpu_sf_begin(st);
    extF80_roundingPrecision = fpu_rounding_precision(st->fpcr_round_prec);
    r = from_ext80(extF80_mul(to_ext80(dst), to_ext80(src)));
    flags = fpu_sf_end();
    /* extF80_mul()の68881固有NaN伝播（softfloat_target/s_propagateNaNExtF80UI.c）は
     * invalidフラグを立てないため、ここで読むinvalidは必ず0×∞（design §6.2の表）。 */
    exc = fpu_sf_flags_to_exc(flags);
    if (flags & softfloat_flag_invalid)
        exc |= (uint8_t)MC68K_FPU_EXC_OPERR;
    fpu_commit_exc(st, exc);
    fpu_set_cc(st, r);
    return r;
}

mc68k_fp80_t mc68k_fpu_div(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    uint_fast8_t flags;
    uint8_t exc;

    if (fpu_is_snan(dst) || fpu_is_snan(src))
        fpu_commit_exc(st, (uint8_t)MC68K_FPU_EXC_SNAN);
    fpu_sf_begin(st);
    extF80_roundingPrecision = fpu_rounding_precision(st->fpcr_round_prec);
    r = from_ext80(extF80_div(to_ext80(dst), to_ext80(src)));
    flags = fpu_sf_end();
    /* invalidは0/0・∞/∞（OPERR）、infiniteは非零/0（DZ）。design §6.2の表と1:1で対応する
     * （propagateNaNはinvalidを立てない。上記mc68k_fpu_mul()と同じ理由）。 */
    exc = fpu_sf_flags_to_exc(flags);
    if (flags & softfloat_flag_invalid)
        exc |= (uint8_t)MC68K_FPU_EXC_OPERR;
    if (flags & softfloat_flag_infinite)
        exc |= (uint8_t)MC68K_FPU_EXC_DZ;
    fpu_commit_exc(st, exc);
    fpu_set_cc(st, r);
    return r;
}
