/*
 * fpu_model.c — コプロセッサモデルの生成・破棄・reset・観測。
 * 設計は docs/mc68000-internals/reference/fpu.md。
 */
#include <stdlib.h>
#include <string.h>
#include "fpu_model.h"

/* FPCR（[881UM Figure 2-3]）: bit15-8 が ENABLE byte、bit7-6 が PREC、bit5-4 が RND。
 * 内部の ENABLE は bit7=BSUN … bit0=INEX1 でハードウェアの bit15-8 と同じ並び。 */
#define FPCR_ENABLE_SHIFT   8
#define FPCR_PREC_SHIFT     6
#define FPCR_RND_SHIFT      4
#define FPCR_FIELD_MASK     3u
/* FPSR（[881UM Figure 2-4〜2-7]）: bit27-24 が FPCC（N,Z,I,NAN）、bit23-16 が商 byte、
 * bit15-8 が EXC、bit7-3 が AEXC（内部は bit4-0 で、IOP,OVFL,UNFL,DZ,INEX の並び） */
#define FPSR_CC_SHIFT       24
#define FPSR_QUOTIENT_SHIFT 16
#define FPSR_EXC_SHIFT      8
#define FPSR_AEXC_SHIFT     3
#define FPSR_AEXC_MASK      0x1Fu
#define FPSR_QUOTIENT_SIGN  0x80u
#define FPSR_QUOTIENT_MASK  0x7Fu
#define FPU_BYTE_MASK       0xFFu

/* FPCC は内部が N=bit0,Z=bit1,I=bit2,NAN=bit3、ハードウェアは N=bit3 … NAN=bit0 */
static uint8_t reverse_cc(uint8_t cc) {
    return (uint8_t)(((cc & MC68K_FPU_CC_N) ? 8u : 0u) | ((cc & MC68K_FPU_CC_Z) ? 4u : 0u) |
                     ((cc & MC68K_FPU_CC_I) ? 2u : 0u) | ((cc & MC68K_FPU_CC_NAN) ? 1u : 0u));
}

uint32_t fpu_pack_fpcr(const mc68k_fpu_state_t *st) {
    return ((uint32_t)st->fpcr_enable << FPCR_ENABLE_SHIFT) |
           ((uint32_t)(st->fpcr_round_prec & FPCR_FIELD_MASK) << FPCR_PREC_SHIFT) |
           ((uint32_t)(st->fpcr_round_mode & FPCR_FIELD_MASK) << FPCR_RND_SHIFT);
}

uint32_t fpu_pack_fpsr(const mc68k_fpu_state_t *st) {
    uint32_t quotient = (uint32_t)(st->fpsr_quotient & FPSR_QUOTIENT_MASK) |
                        (st->fpsr_quotient_sign ? FPSR_QUOTIENT_SIGN : 0u);
    return ((uint32_t)reverse_cc(st->fpsr_cc) << FPSR_CC_SHIFT) |
           (quotient << FPSR_QUOTIENT_SHIFT) | ((uint32_t)st->fpsr_exc << FPSR_EXC_SHIFT) |
           ((uint32_t)(st->fpsr_aexc & FPSR_AEXC_MASK) << FPSR_AEXC_SHIFT);
}

void fpu_unpack_fpcr(mc68k_fpu_state_t *st, uint32_t v) {
    uint8_t prec = (uint8_t)((v >> FPCR_PREC_SHIFT) & FPCR_FIELD_MASK);

    st->fpcr_enable = (uint8_t)((v >> FPCR_ENABLE_SHIFT) & FPU_BYTE_MASK);
    /* PREC の 11 は予約値。共通コアは 0〜2 だけを扱うので拡張精度に読み替える */
    st->fpcr_round_prec = (prec == FPU_PREC_RESERVED) ? (uint8_t)MC68K_FPU_PREC_EXTENDED : prec;
    st->fpcr_round_mode = (uint8_t)((v >> FPCR_RND_SHIFT) & FPCR_FIELD_MASK);
}

void fpu_unpack_fpsr(mc68k_fpu_state_t *st, uint32_t v) {
    uint32_t quotient = (v >> FPSR_QUOTIENT_SHIFT) & FPU_BYTE_MASK;

    st->fpsr_cc = reverse_cc((uint8_t)((v >> FPSR_CC_SHIFT) & 0xFu));
    st->fpsr_quotient = (uint8_t)(quotient & FPSR_QUOTIENT_MASK);
    st->fpsr_quotient_sign = (quotient & FPSR_QUOTIENT_SIGN) ? 1u : 0u;
    st->fpsr_exc = (uint8_t)((v >> FPSR_EXC_SHIFT) & FPU_BYTE_MASK);
    st->fpsr_aexc = (uint8_t)((v >> FPSR_AEXC_SHIFT) & FPSR_AEXC_MASK);
}

void mc68k_fpu_model_reset(struct mc68k_fpu *f) {
    mc68k_fpu_reset(&f->state);
    f->phase = FPU_PH_IDLE;
    f->step = FPU_STEP_NONE;
    memset(&f->dlg, 0, sizeof(f->dlg));
    f->arith = FPU_ARITH_NONE;
    memset(&f->unstarted, 0, sizeof(f->unstarted));
    f->violation_pending = false;
    f->context_valid = false;
    memset(&f->hold, 0, sizeof(f->hold));
    f->body_words = 0;
    f->body_pos = 0;
    f->restore_busy = false;
}

mc68k_fpu_t *mc68k_fpu_create(mc68k_fpu_config_t config) {
    struct mc68k_fpu *f;

    if (config != MC68K_FPU_68881 && config != MC68K_FPU_68882)
        return NULL;
    f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->config = config;
    mc68k_fpu_model_reset(f);
    return f;
}

void mc68k_fpu_destroy(mc68k_fpu_t *fpu) {
    free(fpu);
}

mc68k_fpu_config_t mc68k_fpu_get_config(const mc68k_fpu_t *fpu) {
    return fpu ? fpu->config : MC68K_FPU_NONE;
}

void mc68k_get_fpu_regs(const mc68k_fpu_t *fpu, mc68k_fpu_regs_t *out) {
    if (!fpu || !out)
        return;
    memcpy(out->fp, fpu->state.fp, sizeof(out->fp));
    out->fpcr = fpu_pack_fpcr(&fpu->state);
    out->fpsr = fpu_pack_fpsr(&fpu->state);
    out->fpiar = fpu->state.fpiar;
}
