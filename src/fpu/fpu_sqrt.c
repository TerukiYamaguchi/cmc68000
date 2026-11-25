/*
 * fpu_sqrt.c — FSQRT。
 *
 * 正規化した 64bit 仮数から整数平方根を 128bit 求め、余りが非零なら最下位ビットへ jam して
 * roundPack へ渡す（PREC/RM で 1 回だけ丸める。二重丸めなし）。SoftFloat の extF80_sqrt は
 * 指数フィールド 0 の欠陥があるため使わない（§5.1-1）。
 */
#include <stdbool.h>
#include <stdint.h>
#include "platform.h"
#include "internals.h"
#include "softfloat.h"
#include "fpu_internal.h"

#define EXP_BIAS_X 16383
#define SIG_BITS   64
#define ROOT_BITS  128
#define LIMBS      3 /* 余りは最大 130bit */

/* X = sig * 2^shift（256bit）の 2 ビット対（i 番目 = bit 2i+1, 2i）を返す。shift は 191 か
 * 192。 */
static unsigned pair_of_x(uint64_t sig, int shift, int i) {
    int bit = 2 * i;
    unsigned v = 0;

    for (int b = 1; b >= 0; b--) {
        int src = bit + b - shift;
        v <<= 1;
        if (src >= 0 && src < SIG_BITS)
            v |= (unsigned)((sig >> src) & 1u);
    }
    return v;
}

/* 3 limb（下位から）の左シフト・比較・減算 */
static void shl(uint64_t a[LIMBS], int n) {
    for (int i = LIMBS - 1; i > 0; i--)
        a[i] = (a[i] << n) | (a[i - 1] >> (64 - n));
    a[0] <<= n;
}

static bool ge(const uint64_t a[LIMBS], const uint64_t b[LIMBS]) {
    for (int i = LIMBS - 1; i >= 0; i--) {
        if (a[i] != b[i])
            return a[i] > b[i];
    }
    return true;
}

static void sub(uint64_t a[LIMBS], const uint64_t b[LIMBS]) {
    uint64_t borrow = 0;

    for (int i = 0; i < LIMBS; i++) {
        uint64_t t = a[i] - b[i] - borrow;
        borrow = (a[i] < b[i] || (a[i] == b[i] && borrow)) ? 1 : 0;
        a[i] = t;
    }
}

/* floor(sqrt(sig * 2^shift)) を 128bit (*hi:*lo) で返し、余りが非零なら true。 */
static bool isqrt_256(uint64_t sig, int shift, uint64_t *hi, uint64_t *lo) {
    uint64_t rem[LIMBS] = {0, 0, 0};
    uint64_t root[LIMBS] = {0, 0, 0};

    for (int i = ROOT_BITS - 1; i >= 0; i--) {
        uint64_t trial[LIMBS];

        shl(rem, 2);
        rem[0] |= pair_of_x(sig, shift, i);
        for (int k = 0; k < LIMBS; k++)
            trial[k] = root[k];
        shl(trial, 2);
        trial[0] |= 1;
        shl(root, 1);
        if (ge(rem, trial)) {
            sub(rem, trial);
            root[0] |= 1;
        }
    }
    *lo = root[0];
    *hi = root[1];
    return (rem[0] | rem[1] | rem[2]) != 0;
}

mc68k_fp80_t mc68k_fpu_sqrt(mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;
    int32_t exp;
    uint64_t sig, hi, lo;
    int p, shift;
    uint_fast8_t sf;
    bool sticky;

    if (fpu_monadic_nan(src, st, &r))
        return r;
    if (fpu_is_zero(src)) {
        r = fpu_make(fpu_sign(src), 0, 0);
        fpu_finish_result(st, r, 0, 0);
        return r;
    }
    if (fpu_sign(src)) { /* 非ゼロの負値（-Inf を含む） */
        r = fpu_default_nan();
        fpu_finish_result(st, r, (uint8_t)MC68K_FPU_EXC_OPERR, 0);
        return r;
    }
    if (fpu_is_inf(src)) {
        r = src;
        fpu_finish_result(st, r, 0, 0);
        return r;
    }
    fpu_normalize(fpu_exp_field(src), src.signif, &exp, &sig);
    p = exp - EXP_BIAS_X - (SIG_BITS - 1); /* value = sig * 2^p */
    shift = (p % 2 == 0) ? 192 : 191;      /* p - shift が偶数になるよう選ぶ */
    sticky = isqrt_256(sig, shift, &hi, &lo);
    if (sticky)
        lo |= 1;
    /* sqrt(value) ≈ root * 2^((p-shift)/2)。root のbit127 の重みが結果の先頭ビット */
    r = fpu_round_pack128(false, (ROOT_BITS - 1) + (p - shift) / 2 + EXP_BIAS_X, hi, lo, st,
                          &sf);
    /* OVFL・UNFL は 0 に強制（[881UM] p.4-109、design §5.2）。INEX2 は roundPack の結果 */
    fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf),
                      (uint8_t)(MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_UNFL));
    return r;
}
