/*
 * fpu_cmp.c — FCMP/FTST共通コア。
 * 公開APIは include/mc68000/fpu.h、設計は docs/mc68000-internals/reference/fpu.md。
 *
 * FCMPは [881UM] Operation Table（p.4-32）を直接実装する。dst/srcの実際の減算結果を
 * 計算して分類する実装であってはならない（表は実減算の結果と一致しない反例を含む。
 * 例: dst=+∞,src=+∞は実減算だとNaN/OPERRだが表はZ。design §5.4参照）。
 * Iビットは常にクリア、EXC byte（BSUN/OPERR/OVFL/UNFL/DZ/INEX2は常にクリア、SNANは
 * 検出して立てる）。FTSTも同じEXC byte規則を共有する（[881UM]両ページとも
 * BSUN/OPERR/OVFL/UNFL/DZ/INEX2 Cleared・SNAN Refer to 4.5.4、実測済み）。
 */
#include <stdbool.h>
#include <stdint.h>
#include "fpu_internal.h"

enum {
    CAT_POS_FINITE = 0,
    CAT_NEG_FINITE = 1,
    CAT_POS_ZERO = 2,
    CAT_NEG_ZERO = 3,
    CAT_POS_INF = 4,
    CAT_NEG_INF = 5,
};
#define FCMP_DYN 0xFFu

static int classify(mc68k_fp80_t v) {
    if (fpu_is_zero(v))
        return fpu_sign(v) ? CAT_NEG_ZERO : CAT_POS_ZERO;
    if (fpu_is_inf(v))
        return fpu_sign(v) ? CAT_NEG_INF : CAT_POS_INF;
    return fpu_sign(v) ? CAT_NEG_FINITE : CAT_POS_FINITE;
}

/* design §5.4 の確定表。行dst・列srcの順は +有限,-有限,+0,-0,+∞,-∞。
 * 対角の同符号有限どうし（{NZ}セル）はFCMP_DYNとしてfcmp_diagonal()で動的に決める。 */
static const uint8_t fcmp_table[6][6] = {
    /*         +有限            -有限            +0               -0               +∞ -∞ */
    /* +有限 */ {FCMP_DYN, 0, 0, 0, MC68K_FPU_CC_N, 0},
    /* -有限 */ {MC68K_FPU_CC_N, FCMP_DYN, MC68K_FPU_CC_N, MC68K_FPU_CC_N, MC68K_FPU_CC_N, 0},
    /* +0    */ {MC68K_FPU_CC_N, 0, MC68K_FPU_CC_Z, MC68K_FPU_CC_Z, MC68K_FPU_CC_N, 0},
    /* -0    */
    {MC68K_FPU_CC_N, 0, (uint8_t)(MC68K_FPU_CC_N | MC68K_FPU_CC_Z),
     (uint8_t)(MC68K_FPU_CC_N | MC68K_FPU_CC_Z), MC68K_FPU_CC_N, 0},
    /* +∞   */ {0, 0, 0, 0, MC68K_FPU_CC_Z, 0},
    /* -∞   */
    {MC68K_FPU_CC_N, MC68K_FPU_CC_N, MC68K_FPU_CC_N, MC68K_FPU_CC_N, MC68K_FPU_CC_N,
     (uint8_t)(MC68K_FPU_CC_N | MC68K_FPU_CC_Z)},
};

/* {NZ}セルの規定（design §5.4）: Z = (dst == src)（数値として）。
 * N = (dst < src) || (dst == src && signbit(dst))。等しいときのNは丸めモードに
 * 依らずdstの符号ビットで決まる（"正確に引き算して符号を見る"実装だと食い違う）。 */
static uint8_t fcmp_diagonal(mc68k_fp80_t dst, mc68k_fp80_t src) {
    int32_t ed, es;
    uint64_t sd, ss;
    int cmp;
    uint8_t cc = 0;

    fpu_normalize(fpu_exp_field(dst), dst.signif, &ed, &sd);
    fpu_normalize(fpu_exp_field(src), src.signif, &es, &ss);
    if (ed != es)
        cmp = ed > es ? 1 : -1;
    else if (sd != ss)
        cmp = sd > ss ? 1 : -1;
    else
        cmp = 0;
    if (fpu_sign(dst))
        cmp = -cmp; /* 同符号どうしなので dst の符号だけで数値順を反転できる */
    if (cmp == 0)
        cc |= MC68K_FPU_CC_Z;
    if (cmp < 0 || (cmp == 0 && fpu_sign(dst)))
        cc |= MC68K_FPU_CC_N;
    return cc;
}

void mc68k_fpu_cmp(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    uint8_t cc;
    uint8_t exc = 0;

    if (fpu_is_nan(dst) || fpu_is_nan(src)) {
        /* 一次資料の表は非NaNの6分類だけを規定する。NaNオペランドはIEEE754の
         * unordered比較としてNANビットのみ立てる（推論。design §9。BSUNの設定は
         * 1aのスコープ外。§6.2・§9）。 */
        cc = MC68K_FPU_CC_NAN;
        if (fpu_is_snan(dst) || fpu_is_snan(src))
            exc = (uint8_t)MC68K_FPU_EXC_SNAN;
    } else {
        int rd = classify(dst);
        int rs = classify(src);

        cc = fcmp_table[rd][rs];
        if (cc == FCMP_DYN)
            cc = fcmp_diagonal(dst, src);
    }
    st->fpsr_cc = cc;
    fpu_commit_exc(st, exc);
}

void mc68k_fpu_tst(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    fpu_set_cc(st, src);
    fpu_commit_exc(st, fpu_is_snan(src) ? (uint8_t)MC68K_FPU_EXC_SNAN : 0);
}
