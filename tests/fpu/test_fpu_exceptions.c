/*
 * test_fpu_exceptions.c — FPU共通コア Phase C（IEEE754例外状態の検出）のテスト。
 * 設計は 現行仕様。
 *
 * Phase A/Bのテスト（test_fpu_core.c, test_fpu_arith.c）で値だけを確認した入力の一部を
 * ここで再利用し、フラグ（st->fpsr_exc / fpsr_aexc）の検査を追加する。値そのものの再検証は
 * 目的ではない（既に別ファイルで検証済み）ため、フラグ検査を主眼に置く。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "mc68000/fpu.h"

#define SIG_J UINT64_C(0x8000000000000000)

static int g_failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            g_failures++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define CHECK_EXC(st, expect)                                                            \
    do {                                                                                 \
        if ((st).fpsr_exc != (uint8_t)(expect)) {                                        \
            g_failures++;                                                                \
            fprintf(stderr, "%s:%d: fpsr_exc=%02x, expected %02x\n", __FILE__, __LINE__, \
                    (st).fpsr_exc, (unsigned)(expect));                                  \
        }                                                                                \
    } while (0)

#define CHECK_FP(got, exp_sexp, exp_signif)                                                   \
    do {                                                                                      \
        mc68k_fp80_t g_ = (got);                                                              \
        if (g_.sign_exp != (uint16_t)(exp_sexp) || g_.signif != (uint64_t)(exp_signif)) {     \
            g_failures++;                                                                     \
            fprintf(stderr, "%s:%d: got %04x/%016" PRIx64 ", expected %04x/%016" PRIx64 "\n", \
                    __FILE__, __LINE__, g_.sign_exp, g_.signif,                               \
                    (unsigned)(uint16_t)(exp_sexp), (uint64_t)(exp_signif));                  \
        }                                                                                     \
    } while (0)

static mc68k_fpu_state_t make_state(uint8_t mode, uint8_t prec) {
    mc68k_fpu_state_t st;

    mc68k_fpu_reset(&st);
    st.fpcr_round_mode = mode;
    st.fpcr_round_prec = prec;
    return st;
}

static mc68k_fp80_t fp(unsigned sexp, uint64_t signif) {
    mc68k_fp80_t v;

    v.sign_exp = (uint16_t)sexp;
    v.signif = signif;
    return v;
}

static const uint8_t g_modes[] = {MC68K_FPU_ROUND_NEAREST, MC68K_FPU_ROUND_ZERO,
                                  MC68K_FPU_ROUND_MINUS_INF, MC68K_FPU_ROUND_PLUS_INF};

/* ---- 完全相殺・ゼロ同士の結果（design §6.6、必須ケース。4式×3精度×4モード=48件） ------- */

static void test_zero_result_no_flags(void) {
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);
    mc68k_fp80_t neg_one = fp(0xBFFF, SIG_J);
    mc68k_fp80_t pz = fp(0x0000, 0);
    mc68k_fp80_t nz = fp(0x8000, 0);
    static const uint8_t precs[] = {MC68K_FPU_PREC_EXTENDED, MC68K_FPU_PREC_SINGLE,
                                    MC68K_FPU_PREC_DOUBLE};
    int n = 0;

    for (size_t p = 0; p < 3; p++) {
        for (size_t m = 0; m < 4; m++) {
            bool is_rm = g_modes[m] == MC68K_FPU_ROUND_MINUS_INF;
            mc68k_fpu_state_t st;

            /* 1.0-1.0 */
            st = make_state(g_modes[m], precs[p]);
            CHECK_FP(mc68k_fpu_sub(one, one, &st), is_rm ? 0x8000u : 0x0000u, 0);
            CHECK_EXC(st, 0);
            n++;
            /* (-1.0)+1.0 */
            st = make_state(g_modes[m], precs[p]);
            CHECK_FP(mc68k_fpu_add(neg_one, one, &st), is_rm ? 0x8000u : 0x0000u, 0);
            CHECK_EXC(st, 0);
            n++;
            /* (-1.0)-(-1.0) */
            st = make_state(g_modes[m], precs[p]);
            CHECK_FP(mc68k_fpu_sub(neg_one, neg_one, &st), is_rm ? 0x8000u : 0x0000u, 0);
            CHECK_EXC(st, 0);
            n++;
            /* (+0)+(-0) */
            st = make_state(g_modes[m], precs[p]);
            CHECK_FP(mc68k_fpu_add(pz, nz, &st), is_rm ? 0x8000u : 0x0000u, 0);
            CHECK_EXC(st, 0);
            n++;
        }
    }
    CHECK(n == 3 * 4 * 4);
}

/* ---- ストアの±0（design §6.6、必須ケース。4入力×3形式×4モード=48件） ------------------ */

static void test_store_zero_no_flags(void) {
    mc68k_fp80_t inputs[] = {fp(0x0000, 0), fp(0x8000, 0), fp(0x0001, 0), fp(0x8001, 0)};
    int n = 0;

    for (size_t i = 0; i < 4; i++) {
        bool neg = (inputs[i].sign_exp & 0x8000u) != 0;

        for (size_t m = 0; m < 4; m++) {
            mc68k_fpu_state_t st;

            st = make_state(g_modes[m], 0);
            CHECK(mc68k_fpu_to_s(inputs[i], &st) == (neg ? 0x80000000u : 0u));
            CHECK_EXC(st, 0);
            n++;

            st = make_state(g_modes[m], 0);
            CHECK(mc68k_fpu_to_d(inputs[i], &st) == (neg ? UINT64_C(0x8000000000000000) : 0));
            CHECK_EXC(st, 0);
            n++;

            st = make_state(g_modes[m], 0);
            CHECK_FP(mc68k_fpu_to_x(inputs[i], &st), neg ? 0x8000u : 0x0000u, 0);
            CHECK_EXC(st, 0);
            n++;
        }
    }
    CHECK(n == 4 * 4 * 3);
}

/* ---- UNFL 3ケース（exact tiny / inexact tiny / 境界ちょうど）: PREC=X ------------------
 * design §3.3.1・§6.4のisTiny機構。算術（add: zero+finite分岐で対象精度自身のroundPackを
 * 通す）とストア（to_x）の両方で確認する。 */

static void test_unfl_matrix_x(void) {
    mc68k_fpu_state_t st;
    /* 境界ちょうど（生e=0,j=1、Xの最小正規化数）: exp==0はtinyではない（§3.3.1 N1）。 */
    mc68k_fp80_t boundary = fp(0x0000, SIG_J);
    /* exact tiny: normalize後 (exp=-1,sig=8000...,sigExtra=0)。
     * v=(0000,2000000000000000)+同値 → carryHiでexpZ=-1,sigHi=8000...,sigLo=0
     * （test_fpu_arith.c test_addsub_exp_zero_examples #1と同じ構成）。 */
    mc68k_fp80_t exact_tiny_src = fp(0x0000, UINT64_C(0x2000000000000000));
    /* inexact tiny: Xの最小非正規化数×0.5（design §3.3.1・§6.6、ちょうど半分・偶数丸めで+0）。
     */
    mc68k_fp80_t min_denorm = fp(0x0000, 1);
    mc68k_fp80_t half = fp(0x3FFE, SIG_J);
    mc68k_fp80_t pz = fp(0x0000, 0);

    /* 算術（zero+finite分岐。fpu_round_finite経由） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    CHECK_FP(mc68k_fpu_add(boundary, pz, &st), 0x0000, SIG_J);
    CHECK_EXC(st, 0);

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    CHECK_FP(mc68k_fpu_add(exact_tiny_src, exact_tiny_src, &st), 0x0000,
             UINT64_C(0x4000000000000000));
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_UNFL);
    CHECK(st.fpsr_aexc ==
          0); /* AEXC.UNFL = EXC.UNFL & EXC.INEX2（design §6.5）。INEX2=0なので0 */

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    CHECK_FP(mc68k_fpu_mul(min_denorm, half, &st), 0x0000, 0);
    CHECK_EXC(st, (uint8_t)(MC68K_FPU_EXC_UNFL | MC68K_FPU_EXC_INEX2));
    CHECK((st.fpsr_aexc & MC68K_FPU_AEXC_UNFL) != 0);

    /* ストア（to_x）。to_xはsigExtraを持たないため常にINEX2=0（実装上の不変条件）。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    CHECK_FP(mc68k_fpu_to_x(boundary, &st), 0x0000, SIG_J);
    CHECK_EXC(st, 0);

    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    CHECK_FP(mc68k_fpu_to_x(fp(0x0000, UINT64_C(0x4000000000000000)), &st), 0x0000,
             UINT64_C(0x4000000000000000));
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_UNFL);
}

/* ---- UNFL 3ケース: PREC=S（design §3.3.1のmin_norm_exp_S=0x3F81） ---------------------- */

static void test_unfl_matrix_s(void) {
    mc68k_fpu_state_t st;
    mc68k_fp80_t pz = fp(0x0000, 0);
    /* 境界ちょうど: exp=0x3F81（Sの最小正規化数2^-126） */
    mc68k_fp80_t boundary = fp(0x3F81, SIG_J);
    /* exact tiny: exp=0x3F80（2^-127）。keep=23,drop=41、sig=8000...は下位41bitが0なので厳密。
     */
    mc68k_fp80_t exact_tiny = fp(0x3F80, SIG_J);
    /* inexact tiny: 2^-150+2^-175（test_fpu_arith.c test_prec_s_boundaryと同じ構成、keep==0）。
     */
    mc68k_fpu_state_t any = make_state(0, 0);
    mc68k_fp80_t a, b;

    a = mc68k_fpu_from_d(UINT64_C(0x3690000000000000), &any); /* 2^-150 */
    b = mc68k_fpu_from_d(UINT64_C(0x3500000000000000), &any); /* 2^-175 */

    /* 算術（zero+finite分岐、およびadd） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    CHECK_FP(mc68k_fpu_add(boundary, pz, &st), 0x3F81, SIG_J);
    CHECK_EXC(st, 0);

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    CHECK_FP(mc68k_fpu_add(exact_tiny, pz, &st), 0x3F80, SIG_J);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_UNFL);
    CHECK(st.fpsr_aexc == 0);

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    CHECK_FP(mc68k_fpu_add(a, b, &st), 0x3F6A, SIG_J);
    CHECK_EXC(st, (uint8_t)(MC68K_FPU_EXC_UNFL | MC68K_FPU_EXC_INEX2));
    CHECK((st.fpsr_aexc & MC68K_FPU_AEXC_UNFL) != 0);

    /* ストア（to_s）。値は§4.4等で検証済みの丸めロジックの帰結のためフラグのみ検査する。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_s(boundary, &st);
    CHECK_EXC(st, 0);

    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_s(exact_tiny, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_UNFL);

    /* keep=23,drop=41でbit40（低位41bit内）を立てて真に切り捨てが起きる値。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_s(fp(0x3F80, UINT64_C(0x8000010000000000)), &st);
    CHECK_EXC(st, (uint8_t)(MC68K_FPU_EXC_UNFL | MC68K_FPU_EXC_INEX2));
}

/* ---- UNFL 3ケース: PREC=D（design §3.3.1のmin_norm_exp_D=0x3C01） ---------------------- */

static void test_unfl_matrix_d(void) {
    mc68k_fpu_state_t st;
    mc68k_fp80_t pz = fp(0x0000, 0);
    /* 境界ちょうど: exp=0x3C01（Dの最小正規化数2^-1022） */
    mc68k_fp80_t boundary = fp(0x3C01, SIG_J);
    /* exact tiny: exp=0x3C00（2^-1023）。keep=52,drop=12、sig=8000...は下位12bitが0なので厳密。
     */
    mc68k_fp80_t exact_tiny = fp(0x3C00, SIG_J);
    /* inexact tiny: test_fpu_arith.c test_prec_d_boundaryと同じ構成（keep==1、丸め上げ）。 */
    mc68k_fp80_t x3bcc = fp(0x3BCC, SIG_J);
    mc68k_fp80_t x3bcd = fp(0x3BCD, SIG_J);

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_DOUBLE);
    CHECK_FP(mc68k_fpu_add(boundary, pz, &st), 0x3C01, SIG_J);
    CHECK_EXC(st, 0);

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_DOUBLE);
    CHECK_FP(mc68k_fpu_add(exact_tiny, pz, &st), 0x3C00, SIG_J);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_UNFL);
    CHECK(st.fpsr_aexc == 0);

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_DOUBLE);
    CHECK_FP(mc68k_fpu_add(x3bcd, x3bcc, &st), 0x3BCE, SIG_J);
    CHECK_EXC(st, (uint8_t)(MC68K_FPU_EXC_UNFL | MC68K_FPU_EXC_INEX2));
    CHECK((st.fpsr_aexc & MC68K_FPU_AEXC_UNFL) != 0);

    /* ストア（to_d）。境界ちょうど→Dの最小正規化数、exact tiny→Dの最小非正規化数。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    CHECK(mc68k_fpu_to_d(boundary, &st) == UINT64_C(0x0010000000000000));
    CHECK_EXC(st, 0);

    /* exp=0x3C00（2^-1023）はDの非正規化域で frac=2^51（0x0008000000000000）に格納される
     * （Dの最小非正規化値2^-1074の2^51倍）。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    CHECK(mc68k_fpu_to_d(exact_tiny, &st) == UINT64_C(0x0008000000000000));
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_UNFL);

    /* keep=52,drop=12でbit0を立てて真に切り捨てが起きる値。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_d(fp(0x3C00, SIG_J | 1), &st);
    CHECK_EXC(st, (uint8_t)(MC68K_FPU_EXC_UNFL | MC68K_FPU_EXC_INEX2));
}

/* ---- S/D・OVFLトリガ（design §3.3.1・§6.6必須ケース） -------------------------------- */

static void test_overflow_flags(void) {
    mc68k_fpu_state_t rz_s = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t rz_d = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_DOUBLE);

    /* test_fpu_arith.c test_mul_s_rz_overflowと同じ構成 */
    CHECK_FP(mc68k_fpu_mul(fp(0x407E, SIG_J), fp(0x4000, SIG_J), &rz_s), 0x407E,
             UINT64_C(0xFFFFFF0000000000));
    CHECK_EXC(rz_s, (uint8_t)(MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_INEX2));
    CHECK((rz_s.fpsr_aexc & (MC68K_FPU_AEXC_OVFL | MC68K_FPU_AEXC_INEX)) ==
          (MC68K_FPU_AEXC_OVFL | MC68K_FPU_AEXC_INEX));

    /* PREC=D版: 2^1023 × 2 = 2^1024（Dの最大有限値を超える） */
    CHECK_FP(mc68k_fpu_mul(fp(0x43FE, SIG_J), fp(0x4000, SIG_J), &rz_d), 0x43FE,
             UINT64_C(0xFFFFFFFFFFFFF800));
    CHECK_EXC(rz_d, (uint8_t)(MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_INEX2));
}

/* ---- mc68k_fpu_move()の値の範囲制限とフラグ（design §4.1・§6.6必須ケース） ------------- */

static void test_move_range_flags(void) {
    mc68k_fpu_state_t any = make_state(0, 0);
    mc68k_fpu_state_t rn_s = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t rz_s = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t rp_s = make_state(MC68K_FPU_ROUND_PLUS_INF, MC68K_FPU_PREC_SINGLE);
    /* Xの非正規化数（design §4.1「sourceが拡張精度の非正規化数」の対照。値は
     * test_addsub_zero_plus_denormalと同じ2^-16384）。 */
    mc68k_fp80_t x_denorm = fp(0x0000, UINT64_C(0x4000000000000000));
    mc68k_fp80_t big =
        mc68k_fpu_from_d(UINT64_C(0x7FEFFFFFFFFFFFFF), &any); /* 2^1024相当、S範囲外 */
    mc68k_fp80_t tiny_d =
        mc68k_fpu_from_d(UINT64_C(0x0000000000000001), &any); /* 2^-1074, Xの正規化範囲内 */

    /* OVFLは常にクリア（結果が±Inf/最大有限値になっても）。 */
    CHECK_FP(mc68k_fpu_move(big, &rn_s), 0x7FFF, SIG_J);
    CHECK_EXC(
        rn_s,
        (uint8_t)MC68K_FPU_EXC_INEX2); /* roundPackのinexactをそのまま採用（design §4.1・§9） */
    CHECK_FP(mc68k_fpu_move(big, &rz_s), 0x407E, UINT64_C(0xFFFFFF0000000000));
    CHECK_EXC(rz_s, (uint8_t)MC68K_FPU_EXC_INEX2);

    /* UNFLはsource自体がXの非正規化数のときだけ。2^-1074はXの正規化範囲内なのでUNFL=0。 */
    CHECK_FP(mc68k_fpu_move(tiny_d, &rn_s), 0x0000, 0);
    CHECK_EXC(rn_s, (uint8_t)MC68K_FPU_EXC_INEX2);
    CHECK_FP(mc68k_fpu_move(tiny_d, &rp_s), 0x3F6A, SIG_J);
    CHECK_EXC(rp_s, (uint8_t)MC68K_FPU_EXC_INEX2);

    /* 対照: source自体がXの非正規化数なら、destinationのPRECに関わらずUNFLが立つ。 */
    {
        mc68k_fpu_state_t rn_x = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);

        CHECK_FP(mc68k_fpu_move(x_denorm, &rn_x), 0x0000, UINT64_C(0x4000000000000000));
        CHECK_EXC(rn_x, (uint8_t)MC68K_FPU_EXC_UNFL);
    }

    /* fpu_src_is_x_denormal()自身の境界（exp==0ちょうど、Xの最小正規化数e=0,j=1）。
     * exp<0ではなくexp<=0にする変異はarithmetic経路の他のUNFLテストでは検出できない
     * （move/abs/negは別のロジックでUNFLを判定するため。自己レビューC-B2で追加）。 */
    {
        mc68k_fpu_state_t rn_x = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
        mc68k_fp80_t x_min_normal = fp(0x0000, SIG_J);

        CHECK_FP(mc68k_fpu_move(x_min_normal, &rn_x), 0x0000, SIG_J);
        CHECK_EXC(rn_x, 0);
    }
}

/* ---- mc68k_fpu_move()のINEX2（design §4.1・§6.6必須ケース） -------------------------- */

static void test_move_inex2(void) {
    mc68k_fpu_state_t any = make_state(0, 0);
    mc68k_fp80_t from_l_val = mc68k_fpu_from_l(16777217); /* 2^24+1 */
    mc68k_fp80_t from_d_val =
        mc68k_fpu_from_d(UINT64_C(0x3FF0000000400000), &any); /* 1+2^-30 */
    mc68k_fp80_t from_b_val = mc68k_fpu_from_b(100);
    mc68k_fp80_t from_w_val = mc68k_fpu_from_w(30000);
    mc68k_fp80_t from_s_val = mc68k_fpu_from_s(UINT32_C(0x3F800000), &any); /* 1.0f */
    static const uint8_t precs[] = {MC68K_FPU_PREC_SINGLE, MC68K_FPU_PREC_DOUBLE,
                                    MC68K_FPU_PREC_EXTENDED};

    for (size_t i = 0; i < 3; i++) {
        mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, precs[i]);
        bool expect_inex2 = (precs[i] == MC68K_FPU_PREC_SINGLE);

        mc68k_fpu_move(from_l_val, &st);
        CHECK_EXC(st, expect_inex2 ? (uint8_t)MC68K_FPU_EXC_INEX2 : 0);

        st = make_state(MC68K_FPU_ROUND_NEAREST, precs[i]);
        mc68k_fpu_move(from_d_val, &st);
        CHECK_EXC(st, expect_inex2 ? (uint8_t)MC68K_FPU_EXC_INEX2 : 0);

        /* B/W/Sはどのprecでも厳密（design §4.1のdotテーブル） */
        st = make_state(MC68K_FPU_ROUND_NEAREST, precs[i]);
        mc68k_fpu_move(from_b_val, &st);
        CHECK_EXC(st, 0);
        st = make_state(MC68K_FPU_ROUND_NEAREST, precs[i]);
        mc68k_fpu_move(from_w_val, &st);
        CHECK_EXC(st, 0);
        st = make_state(MC68K_FPU_ROUND_NEAREST, precs[i]);
        mc68k_fpu_move(from_s_val, &st);
        CHECK_EXC(st, 0);
    }
}

/* ---- FABS/FNEGのフラグ（design §5.4・§6.6必須ケース） -------------------------------- */

static void test_abs_neg_flags(void) {
    mc68k_fpu_state_t any = make_state(0, 0);
    mc68k_fpu_state_t rn_s = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t rn_x = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t big = mc68k_fpu_from_d(UINT64_C(0x7FEFFFFFFFFFFFFF), &any);
    mc68k_fp80_t x_denorm = fp(0x0000, UINT64_C(0x4000000000000000));
    mc68k_fp80_t snan = fp(0x7FFF, UINT64_C(0x9000000000000000));

    /* OVFL/INEX2は常にクリア（roundPackの検出結果は使わない）。同じ入力でmove()は
     * INEX2=1になる（test_move_range_flags）ため、abs/negが独立実装であることの確認になる。 */
    mc68k_fpu_abs(big, &rn_s);
    CHECK_EXC(rn_s, 0);
    rn_s = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_neg(big, &rn_s);
    CHECK_EXC(rn_s, 0);

    /* UNFLはmove()と同じ規則（source自体がXの非正規化数か）。 */
    mc68k_fpu_abs(x_denorm, &rn_x);
    CHECK_EXC(rn_x, (uint8_t)MC68K_FPU_EXC_UNFL);

    /* fpu_src_is_x_denormal()の境界（exp==0ちょうど）。move()と同じ経路を共有するため
     * ここでも確認する（自己レビューC-B2）。 */
    rn_x = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_abs(fp(0x0000, SIG_J), &rn_x);
    CHECK_EXC(rn_x, 0);

    /* SNaN検出 */
    rn_x = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_abs(snan, &rn_x);
    CHECK_EXC(rn_x, (uint8_t)MC68K_FPU_EXC_SNAN);
}

/* ---- OPERR: FADD/FSUB（[881UM Table 6-2]、design §6.2） ------------------------------ */

static void test_operr_add_sub(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);
    mc68k_fp80_t minf = fp(0xFFFF, SIG_J);
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);
    mc68k_fp80_t snan = fp(0x7FFF, UINT64_C(0x9000000000000000));

    /* FADD: 異符号の無限大同士 */
    mc68k_fpu_add(pinf, minf, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_add(minf, pinf, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);

    /* 対照: 同符号無限大同士は正常（OPERR=0） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_add(pinf, pinf, &st);
    CHECK_EXC(st, 0);

    /* FSUB: 共に+∞、共に-∞ */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_sub(pinf, pinf, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_sub(minf, minf, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);

    /* 対照: FSUBの異符号無限大は正常 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_sub(pinf, minf, &st);
    CHECK_EXC(st, 0);

    /* SNaN入力単体ではOPERRは立たない（design §6.2・§6.6必須ケース）。add/subの両方を
     * 確認する（Opus第三者レビュー C-B6: subのSNaN検出commitは未検証だった）。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_add(one, snan, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_sub(snan, one, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);

    /* FSUBの異符号・非相殺経路（design §6.2の通常経路。Opus第三者レビュー C-B4:
     * add_signedの異符号roundPack呼び出しのフラグ結線[163行]が無検証だった）。
     * 1.0-2^-70は指数差70で整列シフトがsigExtraへ食い込みINEX2が立つ
     * （test_fpu_arith.c test_addsub_prec_x_roundingの加算版と同じ構成）。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_sub(one, fp(0x3FFF - 70, SIG_J), &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_INEX2);

    /* jビットが0の無限大（design §3.3.1・§6.6） */
    {
        mc68k_fp80_t pinf_j0 = fp(0x7FFF, 0);

        st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
        mc68k_fpu_add(pinf_j0, minf, &st); /* Inf(j=0)+(-Inf) */
        CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    }
}

/* ---- OPERR/DZ: FMUL/FDIV（[881UM Table 6-2]、design §6.2・§6.6） --------------------- */

static void test_operr_dz_muldiv(void) {
    mc68k_fpu_state_t st;
    mc68k_fp80_t uz = fp(0x0001, 0);
    mc68k_fp80_t pz = fp(0x0000, 0);
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);
    mc68k_fp80_t pinf_j0 = fp(0x7FFF, 0);
    mc68k_fp80_t minf = fp(0xFFFF, SIG_J);
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);
    mc68k_fp80_t two = fp(0x4000, SIG_J);
    mc68k_fp80_t snan = fp(0x7FFF, UINT64_C(0x9000000000000000));

    /* FMUL: 0×∞（unnormalized zeroと通常ゼロの両方、順序も両方） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_mul(uz, pinf, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_mul(pinf, uz, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_mul(pz, pinf, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);

    /* 対照: jビット0の無限大同士の積は正常（値は有限） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_mul(pinf_j0, pinf, &st);
    CHECK_EXC(st, 0);
    /* jビット0の無限大×ゼロはOPERR */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_mul(pinf_j0, pz, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);

    /* FDIV: 0/0（unnormalized zero同士、通常ゼロ同士） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_div(uz, uz, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_div(pz, pz, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);

    /* FDIV: ∞/∞（jビット0を含む） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_div(pinf, minf, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_div(pinf_j0, pinf, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);

    /* FDIV: 非零÷0はDZ（OPERRではない） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_div(one, uz, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_DZ);
    CHECK((st.fpsr_aexc & MC68K_FPU_AEXC_DZ) != 0);

    /* 対照: jビット0の無限大÷有限は正常 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_div(pinf_j0, two, &st);
    CHECK_EXC(st, 0);

    /* FDIVのINEX2（Opus第三者レビュー C-B5: invalid/infiniteを伴わない通常のINEX2/UNFL/
     * OVFLがdivで無検証だった）。1/3は非零・非OPERR・非DZだが割り切れないためINEX2=1。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_div(one, fp(0x4000, UINT64_C(0xC000000000000000)), &st); /* 1.0/3.0 */
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_INEX2);

    /* SNaN入力単体ではOPERRは立たない */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_mul(one, snan, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_div(one, snan, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);
}

/* ---- OPERR: B/W/L destination（[881UM Table 6-2]「FMOVE to B,W,or L」） -------------- */

static void test_operr_int_dest(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fp80_t qnan = fp(0x7FFF, UINT64_C(0xC000000000000000));
    mc68k_fp80_t snan = fp(0x7FFF, UINT64_C(0xA000000000000000));
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);
    mc68k_fp80_t big = fp(0x401E + 1, SIG_J); /* 2^32、Lの範囲外 */
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);

    mc68k_fpu_to_l(qnan, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    /* [881UM Table 6-2]の条件は文字どおり"Non-Signaling NAN"。SNaN入力は§6.1.2の
     * 自己完結した経路（SNANビットのみ）でOPERRは立たない（自己レビューC-B1で訂正）。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_l(snan, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_l(pinf, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_l(big, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_b(big, &st); /* Bとしても範囲外 */
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);

    /* 対照: 範囲内の値はOPERRなし */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_l(one, &st);
    CHECK_EXC(st, 0);
}

/* ---- B/W/LのINEX2（[881UM §6.1.7]、Opus第三者レビュー C-B3対応、必須ケース） ---------
 * round_to_magnitude()がinexactを計算しながら呼び出し側へ返していなかった実バグ。 */

static void test_int_inexact(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fp80_t one_half = fp(0x3FFF, UINT64_C(0xC000000000000000)); /* 1.5 */
    mc68k_fp80_t quarter = fp(0x3FFD, SIG_J);                         /* 0.25 */
    mc68k_fp80_t two = fp(0x4000, SIG_J);

    /* 1.5 → RNで偶数丸め2、端数が切り捨てられるのでINEX2=1 */
    CHECK(mc68k_fpu_to_l(one_half, &st) == 2);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_INEX2);
    CHECK((st.fpsr_aexc & MC68K_FPU_AEXC_INEX) != 0);

    /* 0.25 → RNで0、INEX2=1（Bとしても同様） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    CHECK(mc68k_fpu_to_b(quarter, &st) == 0);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_INEX2);

    /* 対照: 厳密な整数はINEX2=0 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    CHECK(mc68k_fpu_to_l(two, &st) == 2);
    CHECK_EXC(st, 0);

    /* オーバーフロー（OPERR）のときはINEX2を立てない */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_l(fp(0x401E + 1, SIG_J), &st); /* 2^32、Lの範囲外 */
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_OPERR);
}

/* ---- to_x/to_s/to_dのSNaN検出（design §6.2、Opus第三者レビュー C-B6対応） --------------
 * OPERRはS/D/X destinationでは常にクリア（[881UM §6.1.3]）だが、SNANビットは検出する。 */

static void test_store_snan(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fp80_t snan = fp(0x7FFF, UINT64_C(0x9000000000000000));

    mc68k_fpu_to_x(snan, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);

    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_s(snan, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);

    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_to_d(snan, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);
}

/* ---- FCMP/FTSTのEXC byte（design §5.4）: SNAN以外は常にクリア -------------------------- */

static void test_fcmp_ftst_flags(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);
    mc68k_fp80_t minf = fp(0xFFFF, SIG_J);
    mc68k_fp80_t pz = fp(0x0000, 0);
    mc68k_fp80_t snan = fp(0x7FFF, UINT64_C(0x9000000000000000));

    /* 実演算なら異符号無限大同士でOPERRが立つ入力でも、FCMPは常にクリア。 */
    mc68k_fpu_cmp(pinf, minf, &st);
    CHECK_EXC(st, 0);
    /* 実演算なら非零÷0でDZが立つ組み合わせに相当する入力でも、FCMPは無関係にクリア。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_cmp(one, pz, &st);
    CHECK_EXC(st, 0);
    /* SNaNオペランドはSNANのみ */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_cmp(snan, one, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);

    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_tst(one, &st);
    CHECK_EXC(st, 0);
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_tst(snan, &st);
    CHECK_EXC(st, (uint8_t)MC68K_FPU_EXC_SNAN);
}

/* ---- AEXC byteの5本の等式（design §6.5） --------------------------------------------- */

static void test_aexc_equations(void) {
    mc68k_fpu_state_t st;
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);

    /* IOP = BSUN|SNAN|OPERR。BSUNは1aのどの関数も設定しないが、fpu_commit_exc()自体の
     * 等式は独立に検証できる（st.fpsr_excを人工的に立ててmc68k_fpu_tst()でAEXC再計算
     * だけを起こす。Opus第三者レビュー C-N1: 以前はBSUN単体の検証が抜けていた）。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    st.fpsr_exc = (uint8_t)MC68K_FPU_EXC_BSUN;
    mc68k_fpu_tst(one, &st);
    CHECK(st.fpsr_aexc == (uint8_t)MC68K_FPU_AEXC_IOP);

    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    st.fpsr_exc = (uint8_t)MC68K_FPU_EXC_SNAN;
    mc68k_fpu_tst(one, &st); /* fpsr_exc|=0のままAEXC再計算だけが走る */
    CHECK(st.fpsr_aexc == (uint8_t)MC68K_FPU_AEXC_IOP);

    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    st.fpsr_exc = (uint8_t)MC68K_FPU_EXC_OPERR;
    mc68k_fpu_tst(one, &st);
    CHECK(st.fpsr_aexc == (uint8_t)MC68K_FPU_AEXC_IOP);

    /* OVFL = OVFL、かつINEX(OVFLも含む) */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    st.fpsr_exc = (uint8_t)MC68K_FPU_EXC_OVFL;
    mc68k_fpu_tst(one, &st);
    CHECK(st.fpsr_aexc == (uint8_t)(MC68K_FPU_AEXC_OVFL | MC68K_FPU_AEXC_INEX));

    /* UNFL単独ではAEXC.UNFLは立たない（INEX2との論理積） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    st.fpsr_exc = (uint8_t)MC68K_FPU_EXC_UNFL;
    mc68k_fpu_tst(one, &st);
    CHECK(st.fpsr_aexc == 0);

    /* UNFL & INEX2 → AEXC.UNFL、かつINEX(INEX2も含む) */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    st.fpsr_exc = (uint8_t)(MC68K_FPU_EXC_UNFL | MC68K_FPU_EXC_INEX2);
    mc68k_fpu_tst(one, &st);
    CHECK(st.fpsr_aexc == (uint8_t)(MC68K_FPU_AEXC_UNFL | MC68K_FPU_AEXC_INEX));

    /* DZ */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    st.fpsr_exc = (uint8_t)MC68K_FPU_EXC_DZ;
    mc68k_fpu_tst(one, &st);
    CHECK(st.fpsr_aexc == (uint8_t)MC68K_FPU_AEXC_DZ);

    /* INEX1 → AEXC.INEX（INEX1|INEX2|OVFLのいずれか） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    st.fpsr_exc = (uint8_t)MC68K_FPU_EXC_INEX1;
    mc68k_fpu_tst(one, &st);
    CHECK(st.fpsr_aexc == (uint8_t)MC68K_FPU_AEXC_INEX);
}

/* ---- resetとAEXCの永続性（design §6.1・§6.5） ---------------------------------------- */

static void test_reset_and_aexc_persistence(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);

    st.fpsr_exc = 0xFF;
    st.fpsr_aexc = 0xFF;
    mc68k_fpu_reset(&st);
    CHECK(st.fpsr_exc == 0);
    CHECK(st.fpsr_aexc == 0);

    /* ユーザーが明示的に書き込んだAEXCは、EXCを増やさない後続の演算でクリアされない
     * （design §6.5「誰もクリアしない」）。 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    st.fpsr_aexc = (uint8_t)MC68K_FPU_AEXC_DZ;
    mc68k_fpu_tst(one, &st);
    CHECK((st.fpsr_aexc & MC68K_FPU_AEXC_DZ) != 0);
}

/* ---- 複数の共通コア呼び出しでのOR積み上げ（design §6.1・§6.6） ----------------------- */

static void test_multi_call_accumulation(void) {
    /* 1命令相当のシミュレーション: バックエンドが呼び出し列の先頭でEXCをクリアし、
     * 個々の共通コア呼び出しはORのみで積み上げる。ここではmove()でSNANを、
     * 続くadd()でOPERRを立て、両方が同じstに残ることを確認する。 */
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t snan = fp(0x7FFF, UINT64_C(0x9000000000000000));
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);
    mc68k_fp80_t minf = fp(0xFFFF, SIG_J);

    st.fpsr_exc = 0; /* 呼び出し列の先頭（バックエンドの責務） */
    mc68k_fpu_move(snan, &st);
    mc68k_fpu_add(pinf, minf, &st);
    CHECK_EXC(st, (uint8_t)(MC68K_FPU_EXC_SNAN | MC68K_FPU_EXC_OPERR));

    /* 次の命令: バックエンドが先頭でクリアする */
    st.fpsr_exc = 0;
    mc68k_fpu_tst(fp(0x3FFF, SIG_J), &st);
    CHECK_EXC(st, 0);
}

int main(void) {
    test_zero_result_no_flags();
    test_store_zero_no_flags();
    test_unfl_matrix_x();
    test_unfl_matrix_s();
    test_unfl_matrix_d();
    test_overflow_flags();
    test_move_range_flags();
    test_move_inex2();
    test_abs_neg_flags();
    test_operr_add_sub();
    test_operr_dz_muldiv();
    test_operr_int_dest();
    test_int_inexact();
    test_store_snan();
    test_fcmp_ftst_flags();
    test_aexc_equations();
    test_reset_and_aexc_persistence();
    test_multi_call_accumulation();

    if (g_failures) {
        fprintf(stderr, "test_fpu_exceptions: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_fpu_exceptions: OK\n");
    return 0;
}
