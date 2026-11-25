/*
 * fpu_movecr.c — FMOVECR（定数ROMの読み出し）。設計は docs/mc68000-internals/reference/fpu.md。
 *
 * 値は 128bit の仮数（fpu_rom_table.c）を実行時に FPCR の丸めモード・丸め精度で 1 回だけ
 * 丸める。PREC=S/D は仮数だけを丸め、指数範囲は拡張精度のまま（§4.1-5）なので
 * OVFL/UNFL は起きない。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "platform.h"
#include "internals.h"
#include "softfloat.h"
#include "fpu_internal.h"

static uint_fast8_t movecr_rounding_precision(uint8_t prec) {
    switch (prec) {
    case MC68K_FPU_PREC_SINGLE:
        return FPU_ROUNDING_PRECISION_S_XR;
    case MC68K_FPU_PREC_DOUBLE:
        return FPU_ROUNDING_PRECISION_D_XR;
    default:
        return FPU_ROUNDING_PRECISION_X;
    }
}

/* 定義済みオフセットの表項目。未定義は NULL。 */
static const struct fpu_rom_entry *find_entry(uint8_t offset) {
    for (int i = 0; i < fpu_rom_table_count; i++) {
        if (fpu_rom_table[i].offset == offset)
            return &fpu_rom_table[i];
    }
    return NULL;
}

mc68k_fp80_t mc68k_fpu_movecr(uint8_t offset, mc68k_fpu_state_t *st) {
    const struct fpu_rom_entry *e = find_entry(offset);
    mc68k_fp80_t result;
    extFloat80_t r;
    uint8_t exc = 0;

    if (e == NULL || !e->nonzero) {
        result = fpu_make(false, 0, 0);
    } else {
        fpu_sf_begin(st);
        r = softfloat_roundPackToExtF80(false, e->exp, e->hi, e->lo,
                                        movecr_rounding_precision(st->fpcr_round_prec));
        if (fpu_sf_end() & softfloat_flag_inexact)
            exc |= (uint8_t)MC68K_FPU_EXC_INEX2;
        result = fpu_make(false, r.signExp, r.signif);
    }
    fpu_set_cc(st, result);
    fpu_commit_exc(st, exc);
    return result;
}
