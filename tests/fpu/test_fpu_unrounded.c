/*
 * test_fpu_unrounded.c — 共通コアが返す丸め前の中間結果。
 * 設計は 現行仕様。期待値は演算の定義から手で求めた値（production
 * の写しでない）。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "mc68000/fpu.h"

#define SIG_J    UINT64_C(0x8000000000000000)
#define SIG_ONES UINT64_C(0xFFFFFFFFFFFFFFFF)
#define BIAS     0x3FFF

static int g_failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            g_failures++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static mc68k_fp80_t fp(bool sign, uint16_t exp, uint64_t sig) {
    mc68k_fp80_t v = {(uint16_t)((sign ? 0x8000u : 0u) | exp), sig};
    return v;
}

static mc68k_fpu_state_t make_state(uint8_t mode, uint8_t prec) {
    mc68k_fpu_state_t st;

    mc68k_fpu_reset(&st);
    st.fpcr_round_mode = mode;
    st.fpcr_round_prec = prec;
    return st;
}

static void check_u(int line, const mc68k_fpu_unrounded_t *u, bool sign, int32_t exp,
                    uint64_t sig, uint64_t extra, uint8_t bits, uint8_t mode) {
    if (!u->valid || u->sign != sign || u->exp != exp || u->sig != sig ||
        u->sig_extra != extra || u->mantissa_bits != bits || u->round_mode != mode) {
        g_failures++;
        fprintf(stderr,
                "line %d: got valid=%d sign=%d exp=%d sig=%016" PRIx64 " extra=%016" PRIx64
                " bits=%u mode=%u\n",
                line, u->valid, u->sign, (int)u->exp, u->sig, u->sig_extra, u->mantissa_bits,
                u->round_mode);
    }
}
#define CHECK_U(u, sign, exp, sig, extra, bits, mode) \
    check_u(__LINE__, &(u), sign, exp, sig, extra, bits, mode)

static void test_none_when_not_rounded(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_unrounded_t u;
    const mc68k_fp80_t nan = fp(false, 0x7FFF, SIG_J | 1), inf = fp(false, 0x7FFF, SIG_J);
    const mc68k_fp80_t one = fp(false, BIAS, SIG_J);

    mc68k_fpu_unrounded_reset();
    CHECK(!mc68k_fpu_unrounded_get(&u));
    mc68k_fpu_add(nan, one, &st);
    CHECK(!mc68k_fpu_unrounded_get(&u));
    mc68k_fpu_add(inf, one, &st);
    CHECK(!mc68k_fpu_unrounded_get(&u));
    mc68k_fpu_sub(one, one, &st); /* 厳密に打ち消し合う */
    CHECK(!mc68k_fpu_unrounded_get(&u));
    /* 直前の演算の値が残らない: 丸めを通る演算の後に、丸めを通らない演算を reset なしで続ける
     */
    mc68k_fpu_add(one, one, &st);
    CHECK(mc68k_fpu_unrounded_get(&u));
    mc68k_fpu_mul(fp(false, 0, 0), one, &st); /* 0 × 1。roundPack は仮数 0 を通さない */
    CHECK(!mc68k_fpu_unrounded_get(&u));
    CHECK(!mc68k_fpu_unrounded_get(NULL));
}

static void test_add(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_PLUS_INF, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_unrounded_t u;
    /* 1.0 + 2^-65 = 1.0 の下 128 bit 目の位置 65 に 1。guard = 0、round = 1、sticky = 0 */
    const mc68k_fp80_t r =
        mc68k_fpu_add(fp(false, BIAS, SIG_J), fp(false, BIAS - 65, SIG_J), &st);

    CHECK(mc68k_fpu_unrounded_get(&u));
    CHECK_U(u, false, BIAS, SIG_J, UINT64_C(0x4000000000000000), 64, MC68K_FPU_ROUND_PLUS_INF);
    CHECK(r.signif == (SIG_J | 1)); /* RP は丸め上げる。中間結果は丸め前のまま残る */

    for (uint8_t mode = 0; mode < 4; mode++) { /* 丸めモードが全部区別して返る */
        st = make_state(mode, MC68K_FPU_PREC_EXTENDED);
        mc68k_fpu_unrounded_reset();
        mc68k_fpu_add(fp(false, BIAS, SIG_J), fp(false, BIAS - 65, SIG_J), &st);
        CHECK(mc68k_fpu_unrounded_get(&u) && u.round_mode == mode);
    }

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_unrounded_reset();
    mc68k_fpu_add(fp(true, BIAS, SIG_J), fp(true, BIAS - 2, SIG_J), &st); /* -1.25。厳密 */
    CHECK(mc68k_fpu_unrounded_get(&u));
    CHECK_U(u, true, BIAS, UINT64_C(0xA000000000000000), 0, 24, MC68K_FPU_ROUND_NEAREST);
}

static void test_mul_div_overflow_underflow(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_unrounded_t u;

    /* 2^16383 × 2 = 2^16384 は指数フィールド 0x7FFF で OVFL */
    mc68k_fpu_unrounded_reset();
    mc68k_fpu_mul(fp(false, 0x7FFE, SIG_J), fp(false, BIAS + 1, SIG_J), &st);
    CHECK(st.fpsr_exc & MC68K_FPU_EXC_OVFL);
    CHECK(mc68k_fpu_unrounded_get(&u));
    CHECK_U(u, false, 0x7FFF, SIG_J, 0, 64, MC68K_FPU_ROUND_NEAREST);

    /* 2^-16383（指数フィールド 0）÷ 2 は指数 -1 の非正規化域で UNFL */
    st = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_DOUBLE);
    mc68k_fpu_unrounded_reset();
    mc68k_fpu_div(fp(true, 0, SIG_J), fp(false, BIAS + 1, SIG_J), &st);
    CHECK(st.fpsr_exc & MC68K_FPU_EXC_UNFL);
    CHECK(mc68k_fpu_unrounded_get(&u));
    CHECK_U(u, true, -1, SIG_J, 0, 53, MC68K_FPU_ROUND_ZERO);
}

static void test_sqrt_and_sgl(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_unrounded_t u;

    /* sqrt(2) = 0xB504F333F9DE6484 597D89B3... × 2^-63。guard 0・round 1・sticky 1 */
    mc68k_fpu_sqrt(fp(false, BIAS + 1, SIG_J), &st);
    CHECK(mc68k_fpu_unrounded_get(&u));
    CHECK(!u.sign && u.exp == BIAS && u.sig == UINT64_C(0xB504F333F9DE6484));
    CHECK((u.sig_extra >> 60) == 0x5 && (u.sig_extra & UINT64_C(0x3FFFFFFFFFFFFFFF)) != 0);
    CHECK(u.mantissa_bits == 64);

    /* FSGLMUL は FPCR の精度を無視し、仮数 24 bit・指数は拡張精度で丸める */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_DOUBLE);
    mc68k_fpu_sglmul(fp(false, BIAS, SIG_J), fp(false, BIAS, SIG_J), &st);
    CHECK(mc68k_fpu_unrounded_get(&u));
    CHECK(u.mantissa_bits == 24);
}

static void test_round_mantissa(void) {
    mc68k_fpu_unrounded_t
        in = {true, false, 0x7FFF, SIG_ONES, SIG_J, 64, MC68K_FPU_ROUND_NEAREST},
        out;

    /* ちょうど半分で保持側の最下位ビットが 1 → 繰り上がり、桁上がりで指数 +1 */
    mc68k_fpu_unrounded_round_mantissa(&in, &out);
    CHECK_U(out, false, 0x8000, SIG_J, 0, 64, MC68K_FPU_ROUND_NEAREST);

    in.round_mode = MC68K_FPU_ROUND_ZERO; /* RZ は切り捨て */
    mc68k_fpu_unrounded_round_mantissa(&in, &out);
    CHECK_U(out, false, 0x7FFF, SIG_ONES, 0, 64, MC68K_FPU_ROUND_ZERO);

    in.round_mode = MC68K_FPU_ROUND_MINUS_INF; /* 正数の RM は切り捨て、負数は繰り上げ */
    mc68k_fpu_unrounded_round_mantissa(&in, &out);
    CHECK(out.sig == SIG_ONES && out.exp == 0x7FFF);
    in.sign = true;
    mc68k_fpu_unrounded_round_mantissa(&in, &out);
    CHECK(out.sig == SIG_J && out.exp == 0x8000);

    /* 倍精度（53 bit）: 下位 11 bit が 0x4FF（半分 0x400 超）なので繰り上げ */
    mc68k_fpu_unrounded_t d = {
        true, false, BIAS, UINT64_C(0x80000000000004FF), 0, 53, MC68K_FPU_ROUND_NEAREST};
    mc68k_fpu_unrounded_round_mantissa(&d, &out);
    CHECK(out.sig == UINT64_C(0x8000000000000800) && out.exp == BIAS);

    /* 拡張（drop = 0）: ちょうど半分で保持側の最下位ビットが 0
     * なら繰り上げない（偶数への丸め）。sticky があれば上げる。 */
    mc68k_fpu_unrounded_t x = {true, false, 0x4000, SIG_J, SIG_J, 64, MC68K_FPU_ROUND_NEAREST};
    mc68k_fpu_unrounded_round_mantissa(&x, &out);
    CHECK(out.sig == SIG_J && out.exp == 0x4000);
    x.sig_extra = SIG_J | 1;
    mc68k_fpu_unrounded_round_mantissa(&x, &out);
    CHECK(out.sig == (SIG_J | 1));
    x.sig_extra = SIG_J - 1; /* 半分未満（奇数 lsb でも上げない） */
    x.sig = SIG_J | 1;
    mc68k_fpu_unrounded_round_mantissa(&x, &out);
    CHECK(out.sig == (SIG_J | 1));
    x.sig_extra = 1; /* 端数があるだけ（RP で上げ、RN・RZ・RM（正）で上げない） */
    x.round_mode = MC68K_FPU_ROUND_PLUS_INF;
    mc68k_fpu_unrounded_round_mantissa(&x, &out);
    CHECK(out.sig == (SIG_J | 2));
    x.round_mode = MC68K_FPU_ROUND_MINUS_INF;
    mc68k_fpu_unrounded_round_mantissa(&x, &out);
    CHECK(out.sig == (SIG_J | 1));
    x.sign = true; /* RM は負数で上げる */
    mc68k_fpu_unrounded_round_mantissa(&x, &out);
    CHECK(out.sig == (SIG_J | 2));
    x.sign = false;
    x.sig_extra = 0; /* 端数なしは動かさない */
    x.round_mode = MC68K_FPU_ROUND_PLUS_INF;
    mc68k_fpu_unrounded_round_mantissa(&x, &out);
    CHECK(out.sig == (SIG_J | 1));

    /* 単精度（24 bit、drop = 40）の場合分け。RP: 端数が sig の下位だけ／sig_extra
     * だけでも上げる。 */
    mc68k_fpu_unrounded_t y = {
        true, false, BIAS, UINT64_C(0x8000000000000001), 0, 24, MC68K_FPU_ROUND_PLUS_INF};
    mc68k_fpu_unrounded_round_mantissa(&y, &out);
    CHECK(out.sig == UINT64_C(0x8000010000000000));
    y.sig = SIG_J;
    y.sig_extra = 1;
    mc68k_fpu_unrounded_round_mantissa(&y, &out);
    CHECK(out.sig == UINT64_C(0x8000010000000000));
    y.round_mode = MC68K_FPU_ROUND_NEAREST; /* ちょうど半分で偶数 lsb は上げない。半分を超えるか
                                               odd lsb なら上げる */
    y.sig = UINT64_C(0x8000008000000000);
    y.sig_extra = 0;
    mc68k_fpu_unrounded_round_mantissa(&y, &out);
    CHECK(out.sig == SIG_J);
    y.sig = UINT64_C(0x8000018000000000); /* 奇数 lsb のタイ */
    mc68k_fpu_unrounded_round_mantissa(&y, &out);
    CHECK(out.sig == UINT64_C(0x8000020000000000));
    y.sig = UINT64_C(0x8000010000000001); /* 奇数 lsb で半分未満: 上げない */
    mc68k_fpu_unrounded_round_mantissa(&y, &out);
    CHECK(out.sig == UINT64_C(0x8000010000000000));
    y.sig = UINT64_C(0x80000080000000FF); /* 半分超 */
    mc68k_fpu_unrounded_round_mantissa(&y, &out);
    CHECK(out.sig == UINT64_C(0x8000010000000000));

    /* 単精度（24 bit、drop = 40）: 下位 40 bit の半分未満は切り捨て、sig_extra の sticky
     * だけでは動かない */
    mc68k_fpu_unrounded_t f = {
        true, false, BIAS, UINT64_C(0x8000007FFFFFFFFF), 1, 24, MC68K_FPU_ROUND_NEAREST};
    mc68k_fpu_unrounded_round_mantissa(&f, &out);
    CHECK(out.sig == UINT64_C(0x8000000000000000));
    f.sig = UINT64_C(0x8000008000000000); /* ちょうど半分 + sticky → 繰り上げ */
    mc68k_fpu_unrounded_round_mantissa(&f, &out);
    CHECK(out.sig == UINT64_C(0x8000010000000000));
}

int main(void) {
    test_none_when_not_rounded();
    test_add();
    test_mul_div_overflow_underflow();
    test_sqrt_and_sgl();
    test_round_mantissa();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_fpu_unrounded: OK\n");
    return 0;
}
