/*
 * test_fpu_arith.c — FPU共通コア Phase B（基本算術演算と条件コード）のテスト。
 * 設計は 現行仕様。
 *
 * Phase A/Bの完了時点では st->fpsr_exc / fpsr_aexc を書かないため、フラグは検査しない
 * （design §6.1第4回Opusレビュー N6）。本ファイルで検査するのは値と fpsr_cc のみ。
 * 期待値は一次資料 [881UM] の operation table・境界値・design §3.2.2 で確認済みの
 * 手計算結果から直接書く（productionのビット操作は使わない）。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

static bool is_inf(mc68k_fp80_t v) {
    return (v.sign_exp & 0x7FFF) == 0x7FFF && (v.signif & UINT64_C(0x7FFFFFFFFFFFFFFF)) == 0;
}

/* ---- 加減算: Solの反例7件（design §3.2.2、PREC=X・RN） ------------------------ */

static void test_addsub_exp_zero_examples(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t v;

    /* 1. (0000,2000...)+同値 → normalize後(-2,8000...,0)*2、加算でcarryHi、
     *    roundPack後 signExp=0000,signif=4000000000000000 */
    v = fp(0x0000, UINT64_C(0x2000000000000000));
    CHECK_FP(mc68k_fpu_add(v, v, &rn), 0x0000, UINT64_C(0x4000000000000000));

    /* 2. (0001,8000...)+(0000,8000...) → (1,c000...,0) */
    CHECK_FP(mc68k_fpu_add(fp(0x0001, SIG_J), fp(0x0000, SIG_J), &rn), 0x0001,
             UINT64_C(0xC000000000000000));

    /* 3. (0001,8000...)-(0000,8000...) → (0,8000...,0) */
    CHECK_FP(mc68k_fpu_sub(fp(0x0001, SIG_J), fp(0x0000, SIG_J), &rn), 0x0000, SIG_J);

    /* 4. (0000,c000...)-(0000,8000...) → (-1,8000...,0) → roundPack後 4000000000000000 */
    CHECK_FP(mc68k_fpu_sub(fp(0x0000, UINT64_C(0xC000000000000000)), fp(0x0000, SIG_J), &rn),
             0x0000, UINT64_C(0x4000000000000000));

    /* 5. (0001,8000...)-(0000,ffffffffffffffff) → (-63,8000...,0) → roundPack後 signif=1 */
    CHECK_FP(mc68k_fpu_sub(fp(0x0001, SIG_J), fp(0x0000, UINT64_MAX), &rn), 0x0000, 1);

    /* 6. (3fff,8000...)+同値（1+1=2） → (4000,8000...,0) */
    CHECK_FP(mc68k_fpu_add(fp(0x3FFF, SIG_J), fp(0x3FFF, SIG_J), &rn), 0x4000, SIG_J);

    /* 7. (3fff,c000...)-(3fff,8000...)（1.5-1=0.5） → (3ffe,8000...,0) */
    CHECK_FP(mc68k_fpu_sub(fp(0x3FFF, UINT64_C(0xC000000000000000)), fp(0x3FFF, SIG_J), &rn),
             0x3FFE, SIG_J);

    /* オペランド順序を交換したケース（FSUBは非交換なので符号が反転する） */
    CHECK_FP(mc68k_fpu_add(fp(0x0000, SIG_J), fp(0x0001, SIG_J), &rn), 0x0001,
             UINT64_C(0xC000000000000000));
    CHECK_FP(mc68k_fpu_sub(fp(0x0000, SIG_J), fp(0x0001, SIG_J), &rn), 0x8000,
             SIG_J); /* -(2^-16382 - 2^-16383) = -2^-16383 → roundPack後 signif=8000... */

    /* B-B4: 例7（1.5-1=0.5）をオペランド順序を交換（1-1.5=-0.5）。指数が等しく
     * |src|>|dst|の異符号減算は他のケースに無いため、bigger_is_aの判定
     * （eA==eBのときsA>=sBで大小を決める）を固定値にする変異が生存していた。 */
    CHECK_FP(mc68k_fpu_sub(fp(0x3FFF, SIG_J), fp(0x3FFF, UINT64_C(0xC000000000000000)), &rn),
             0xBFFE, SIG_J);

    /* 完全相殺のオペランド順序交換（1.0 + (-1.0)） */
    CHECK_FP(mc68k_fpu_add(fp(0x3FFF, SIG_J), fp(0xBFFF, SIG_J), &rn), 0x0000, 0);
}

/* PREC=Xの丸め・sticky（B-B3: 加減算の自前実装[design §3.2.2]の整列シフト・128bit
 * add128/sub128の丸め境界が、これまで正確な結果しか出ないケースでしか検査されて
 * いなかった。各値は整列シフトshiftRightJam128・add128/sub128・
 * softfloat_roundPackToExtF80()の契約[design §3.2.1/§3.3.1]を手でたどって独立に
 * 導出した。productionのビット操作は使わない）。 */
static void test_addsub_prec_x_rounding(void) {
    mc68k_fpu_state_t rp = make_state(MC68K_FPU_ROUND_PLUS_INF, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_state_t rz = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);

    /* 1.0 + 2^-70（指数差70、整列シフトでsigExtraに1bitだけ残る。半分未満だが
     * RPは非ゼロinexactなら常に丸め上げる） → 3FFF/8000000000000001 */
    CHECK_FP(mc68k_fpu_add(one, fp(0x3FFF - 70, SIG_J), &rp), 0x3FFF,
             UINT64_C(0x8000000000000001));

    /* 1.0 + 2^-64（指数差64。整列シフトでsigExtraがちょうど半分[0x8000000000000000]
     * になるタイ。保持ビット[sig]のLSBは偶数[0]なのでRNは丸め上げない） →
     * 3FFF/8000...（1.0のまま） */
    CHECK_FP(mc68k_fpu_add(one, fp(0x3FFF - 64, SIG_J), &rn), 0x3FFF, SIG_J);

    /* 1.0 - 2^-200（指数差200。shiftRightJam128のdist>=127分岐で全ビットが単一の
     * stickyへ畳まれる。減算→桁落ち→再正規化1bit左シフトの結果、RZは常に
     * 丸め上げない） → 3FFE/FFFFFFFFFFFFFFFF */
    CHECK_FP(mc68k_fpu_sub(one, fp(0x3FFF - 200, SIG_J), &rz), 0x3FFE, UINT64_MAX);
}

/* ---- ゼロ・無限大の早期リターン（design §3.2.2） ------------------------------ */

static void test_addsub_zero_cancellation(void) {
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);
    mc68k_fp80_t neg_one = fp(0xBFFF, SIG_J);
    static const uint8_t modes[] = {MC68K_FPU_ROUND_NEAREST, MC68K_FPU_ROUND_ZERO,
                                    MC68K_FPU_ROUND_MINUS_INF, MC68K_FPU_ROUND_PLUS_INF};
    mc68k_fp80_t pz = fp(0x0000, 0);
    mc68k_fp80_t nz = fp(0x8000, 0);
    int n = 0;

    for (size_t m = 0; m < 4; m++) {
        bool is_rm = modes[m] == MC68K_FPU_ROUND_MINUS_INF;
        mc68k_fpu_state_t st = make_state(modes[m], MC68K_FPU_PREC_EXTENDED);

        /* 非ゼロ同士の完全相殺: RM以外は+0、RMは-0（[881UM] Figure 4-2注1） */
        CHECK_FP(mc68k_fpu_add(neg_one, one, &st), is_rm ? 0x8000u : 0x0000u, 0);
        CHECK_FP(mc68k_fpu_sub(one, one, &st), is_rm ? 0x8000u : 0x0000u, 0);
        CHECK_FP(mc68k_fpu_sub(neg_one, neg_one, &st), is_rm ? 0x8000u : 0x0000u, 0);
        n += 3;

        /* 同符号ゼロ同士: 丸めモードに関わらずその符号のまま */
        CHECK_FP(mc68k_fpu_add(pz, pz, &st), 0x0000, 0);
        CHECK_FP(mc68k_fpu_add(nz, nz, &st), 0x8000, 0);
        n += 2;

        /* 異符号ゼロ同士: 非ゼロ完全相殺と同じRM条件規則 */
        CHECK_FP(mc68k_fpu_add(pz, nz, &st), is_rm ? 0x8000u : 0x0000u, 0);
        CHECK_FP(mc68k_fpu_add(nz, pz, &st), is_rm ? 0x8000u : 0x0000u, 0);
        n += 2;
    }
    CHECK(n == 4 * 7);
}

/* N3反例: ゼロ+X非正規化数（design §3.2.2）。UNFLはPhase Cで結線するため値のみ確認する。 */
static void test_addsub_zero_plus_denormal(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_state_t rn_s = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fp80_t denorm = fp(0x0000, UINT64_C(0x4000000000000000)); /* 2^-16384 */
    mc68k_fp80_t pz = fp(0x0000, 0);
    mc68k_fp80_t nz = fp(0x8000, 0);

    CHECK_FP(mc68k_fpu_add(denorm, pz, &rn), 0x0000, UINT64_C(0x4000000000000000));
    CHECK_FP(mc68k_fpu_add(pz, denorm, &rn), 0x0000, UINT64_C(0x4000000000000000));
    CHECK_FP(mc68k_fpu_sub(denorm, pz, &rn), 0x0000, UINT64_C(0x4000000000000000));

    /* B-B4: ゼロ早期リターンでもPREC=Sの丸めが効くこと（fpu_round_finite経由）。
     * 1+2^-63（拡張精度で表現できる1.0直後の値）は単精度分解能よりはるかに小さく、
     * RNで厳密に1.0へ丸まる。prec引数を80固定にする変異はこれで検出できる。 */
    CHECK_FP(mc68k_fpu_add(pz, fp(0x3FFF, SIG_J | 1), &rn_s), 0x3FFF, SIG_J);
    /* dst - (denorm)（0からの減算は符号反転が掛かる） */
    CHECK_FP(mc68k_fpu_sub(nz, denorm, &rn), 0x8000, UINT64_C(0x4000000000000000));
}

/* unnormalized zero（signExp!=0だがsig==0）の3系統×4丸めモード（design §3.2.2, §5.7）。
 * +uz+(+uz)・-uz+(-uz)は丸めモードに関わらず共通の符号のまま、+uz+(-uz)はRMのみ-0。 */
static void test_addsub_unnormalized_zero_cancellation(void) {
    mc68k_fp80_t puz = fp(0x0001, 0); /* +unnormalized zero */
    mc68k_fp80_t nuz = fp(0x8001, 0); /* -unnormalized zero */
    static const uint8_t modes[] = {MC68K_FPU_ROUND_NEAREST, MC68K_FPU_ROUND_ZERO,
                                    MC68K_FPU_ROUND_MINUS_INF, MC68K_FPU_ROUND_PLUS_INF};
    int n = 0;

    for (size_t m = 0; m < 4; m++) {
        bool is_rm = modes[m] == MC68K_FPU_ROUND_MINUS_INF;
        mc68k_fpu_state_t st = make_state(modes[m], MC68K_FPU_PREC_EXTENDED);

        CHECK_FP(mc68k_fpu_add(puz, puz, &st), 0x0000, 0);
        CHECK_FP(mc68k_fpu_add(nuz, nuz, &st), 0x8000, 0);
        CHECK_FP(mc68k_fpu_add(puz, nuz, &st), is_rm ? 0x8000u : 0x0000u, 0);
        n += 3;
    }
    CHECK(n == 4 * 3);
}

/* unnormalized入力（design §3.2.2、第3回Opusレビュー非blocking#3） */
static void test_addsub_unnormalized(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t unnorm = fp(0x0001, UINT64_C(0x4000000000000000));
    mc68k_fp80_t pz = fp(0x0000, 0);

    CHECK_FP(mc68k_fpu_add(unnorm, pz, &rn), 0x0000, SIG_J);
}

/* ゼロとNaN・無限大の組（design §3.2.2、必須ケース） */
static void test_addsub_zero_with_nan_inf(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_state_t rz_s = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t rz_x = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t qnan = fp(0x7FFF, UINT64_C(0xC000000000000001));
    mc68k_fp80_t snan = fp(0x7FFF, UINT64_C(0x9000000000000000));
    mc68k_fp80_t pz = fp(0x0000, 0);
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);

    CHECK_FP(mc68k_fpu_add(pz, qnan, &rn), 0x7FFF, UINT64_C(0xC000000000000001));
    CHECK_FP(mc68k_fpu_add(qnan, pz, &rn), 0x7FFF, UINT64_C(0xC000000000000001));
    CHECK_FP(mc68k_fpu_add(pz, snan, &rn), 0x7FFF, UINT64_C(0xD000000000000000));
    CHECK_FP(mc68k_fpu_add(pz, pinf, &rz_x), 0x7FFF, SIG_J);
    CHECK_FP(mc68k_fpu_add(pz, pinf, &rz_s), 0x7FFF, SIG_J);
}

/* 異符号無限大同士（FADD/FSUBのOPERR。design §5.4のFCMPとは別で、こちらはTable 6-2）。
 * 生成されるNaNは仮数全ビット1・符号0（[881UM §3.2.5]、design §5.6・§9。第三者レビュー
 * B-B1で、mul/divが8086の既定NaN[符号1・仮数0xC000…]のままだったことが判明したため、
 * ここも「NaNである」の緩い判定ではなくビットパターンを完全一致で検査する）。 */
static void test_addsub_infinity_operr(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);
    mc68k_fp80_t minf = fp(0xFFFF, SIG_J);

    CHECK_FP(mc68k_fpu_add(pinf, minf, &rn), 0x7FFF, UINT64_MAX);
    CHECK_FP(mc68k_fpu_add(minf, pinf, &rn), 0x7FFF, UINT64_MAX);
    CHECK_FP(mc68k_fpu_sub(pinf, pinf, &rn), 0x7FFF, UINT64_MAX);
    CHECK_FP(mc68k_fpu_sub(minf, minf, &rn), 0x7FFF, UINT64_MAX);
    /* 同符号無限大同士は素直に無限大になる */
    CHECK_FP(mc68k_fpu_add(pinf, pinf, &rn), 0x7FFF, SIG_J);
    CHECK_FP(mc68k_fpu_sub(pinf, minf, &rn), 0x7FFF, SIG_J);

    /* 片方だけがInf（有限-Inf）: 符号操作なしでInf側の値がそのまま結果になる */
    CHECK_FP(mc68k_fpu_sub(fp(0x3FFF, SIG_J), pinf, &rn), 0xFFFF, SIG_J);
}

/* ---- NaN伝播（design §5.2） --------------------------------------------------- */

static void test_nan_propagation(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t dst_nan = fp(0x7FFF, UINT64_C(0xC000000000000001));
    mc68k_fp80_t src_nan = fp(0x7FFF, UINT64_C(0xC000000000000002));
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);

    /* 片方だけがNaN */
    CHECK_FP(mc68k_fpu_add(dst_nan, one, &rn), 0x7FFF, UINT64_C(0xC000000000000001));
    CHECK_FP(mc68k_fpu_add(one, src_nan, &rn), 0x7FFF, UINT64_C(0xC000000000000002));
    CHECK_FP(mc68k_fpu_mul(dst_nan, one, &rn), 0x7FFF, UINT64_C(0xC000000000000001));
    CHECK_FP(mc68k_fpu_div(one, src_nan, &rn), 0x7FFF, UINT64_C(0xC000000000000002));

    /* 両方が非シグナリングNaN: dst優先 */
    CHECK_FP(mc68k_fpu_add(dst_nan, src_nan, &rn), 0x7FFF, UINT64_C(0xC000000000000001));
    CHECK_FP(mc68k_fpu_sub(dst_nan, src_nan, &rn), 0x7FFF, UINT64_C(0xC000000000000001));
    CHECK_FP(mc68k_fpu_mul(dst_nan, src_nan, &rn), 0x7FFF, UINT64_C(0xC000000000000001));
    CHECK_FP(mc68k_fpu_div(dst_nan, src_nan, &rn), 0x7FFF, UINT64_C(0xC000000000000001));
}

/* jビット0のSNaN（design §5.2, §9。8086版quiet化との差、必須ケース） */
static void test_nan_j_bit_zero(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t snan0 = fp(0x7FFF, UINT64_C(0x1000000000000000)); /* j=0, bit62=0, SNaN */
    mc68k_fp80_t qnan2 = fp(0x7FFF, UINT64_C(0xC000000000000002));
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);

    /* quiet化はbit62だけを立てる。jビットは0のまま */
    CHECK_FP(mc68k_fpu_add(one, snan0, &rn), 0x7FFF, UINT64_C(0x5000000000000000));
    CHECK_FP(mc68k_fpu_add(snan0, one, &rn), 0x7FFF, UINT64_C(0x5000000000000000));
    CHECK_FP(mc68k_fpu_mul(one, snan0, &rn), 0x7FFF, UINT64_C(0x5000000000000000));
    CHECK_FP(mc68k_fpu_sub(one, snan0, &rn), 0x7FFF, UINT64_C(0x5000000000000000));
    CHECK_FP(mc68k_fpu_div(one, snan0, &rn), 0x7FFF, UINT64_C(0x5000000000000000));

    /* dst=snan0・src=qnan2: quiet化してからdst優先 */
    CHECK_FP(mc68k_fpu_add(snan0, qnan2, &rn), 0x7FFF, UINT64_C(0x5000000000000000));
    CHECK_FP(mc68k_fpu_mul(snan0, qnan2, &rn), 0x7FFF, UINT64_C(0x5000000000000000));
    CHECK_FP(mc68k_fpu_div(snan0, qnan2, &rn), 0x7FFF, UINT64_C(0x5000000000000000));
    CHECK_FP(mc68k_fpu_sub(snan0, qnan2, &rn), 0x7FFF, UINT64_C(0x5000000000000000));
}

/* ---- 乗除算: 無限大・ゼロの組み合わせ（design §3.2.1, 第4回Opusレビュー N8） -------- */

static void test_mul_div_inf_zero(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t uz = fp(0x0001, 0); /* unnormalized zero (e!=0, sig==0) */
    mc68k_fp80_t pz = fp(0x0000, 0);
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);
    mc68k_fp80_t pinf_j0 = fp(0x7FFF, 0); /* jビットが0の無限大 */
    mc68k_fp80_t two = fp(0x4000, SIG_J);
    mc68k_fp80_t minf = fp(0xFFFF, SIG_J);

    /* 生成されるNaNは仮数全ビット1・符号0（B-B1、上のtest_addsub_infinity_operrと同じ根拠）。
     * A側(dst)がゼロの場合もB側(src)がゼロの場合も両方確認する（B-B2: extF80_mul.cの
     * zeroArg判定はA側・B側で別のコード分岐のため、片方だけの確認では改変漏れを検出できない）。
     */
    CHECK_FP(mc68k_fpu_mul(uz, pinf, &rn), 0x7FFF, UINT64_MAX);
    CHECK_FP(mc68k_fpu_mul(pz, pinf, &rn), 0x7FFF, UINT64_MAX);
    CHECK_FP(mc68k_fpu_mul(pinf, uz, &rn), 0x7FFF, UINT64_MAX);
    CHECK_FP(mc68k_fpu_mul(pinf, pz, &rn), 0x7FFF, UINT64_MAX);

    /* jビットが0の無限大どうしの積は無限大（ゼロと誤認しない） */
    CHECK(is_inf(mc68k_fpu_mul(pinf_j0, pinf, &rn)));
    CHECK(is_inf(mc68k_fpu_mul(pinf, pinf_j0, &rn)));

    /* 対照: Inf(j=0)÷2.0 は無限大 */
    CHECK(is_inf(mc68k_fpu_div(pinf_j0, two, &rn)));

    /* FDIV: 0/0, inf/inf はOPERR相当のNaN */
    CHECK_FP(mc68k_fpu_div(pz, pz, &rn), 0x7FFF, UINT64_MAX);
    CHECK_FP(mc68k_fpu_div(pinf, minf, &rn), 0x7FFF, UINT64_MAX);
}

/* extF80_mul/extF80_div のexp=0エントリ処理修正（design §3.2.1、B-B2: 第三者レビューで
 * 未検証と指摘された、片方が e=0（Motorolaの正規化数[j=1]・非正規化数[j=0]境界）の
 * 通常算術経路。Solの反例1（乗算 2^-16384×2^16383=0.5）のsrc側版と、除算の両方向。 */
static void test_mul_div_exp_zero_entry(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t min_denorm_half =
        fp(0x0000, UINT64_C(0x4000000000000000)); /* 2^-16384, e=0,j=0 */
    mc68k_fp80_t min_normal = fp(0x0000, SIG_J);  /* 2^-16383, e=0,j=1 */
    mc68k_fp80_t big = fp(0x7FFE, SIG_J);         /* 2^16383 */
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);

    /* mul: src側がe=0の非正規化数（Solの反例1をオペランド順序を入れ替えて確認） */
    CHECK_FP(mc68k_fpu_mul(big, min_denorm_half, &rn), 0x3FFE,
             SIG_J); /* 2^16383×2^-16384=0.5 */
    /* div: dstがe=0の非正規化数 */
    CHECK_FP(mc68k_fpu_div(min_denorm_half, min_normal, &rn), 0x3FFE, SIG_J); /* 0.5 */
    /* div: srcがe=0の非正規化数 */
    CHECK_FP(mc68k_fpu_div(one, min_normal, &rn), 0x7FFE, SIG_J); /* 1 / 2^-16383 = 2^16383 */
}

/* ---- PREC=S/D 非正規化域の反例（design §3.3.1, §5.7 必須ケース） ------------------ */

static void test_prec_s_boundary(void) {
    mc68k_fpu_state_t rn_s = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t any = make_state(0, 0);
    /* Dのビット列から厳密な2^-150・2^-175を作る（from_dは常に厳密。design §4.1）。
     * 指数field = bias(1023) - 150 = 873 = 0x369、-175 = 848 = 0x350（frac_bits=52分左詰め）。
     */
    mc68k_fp80_t a = mc68k_fpu_from_d(UINT64_C(0x3690000000000000), &any); /* 2^-150 */
    mc68k_fp80_t b = mc68k_fpu_from_d(UINT64_C(0x3500000000000000), &any); /* 2^-175 */
    mc68k_fp80_t r;

    /* 加算経路: 2^-150 + 2^-175 → 拡張精度でsignExp=3f6a,signif=8000... */
    r = mc68k_fpu_add(a, b, &rn_s);
    CHECK_FP(r, 0x3F6A, SIG_J);
    CHECK(mc68k_fpu_to_s(r, &rn_s) == 1u);
}

/* 減算経路の反例（Sol独立レビュー blocking 3。design §5.7必須ケース）:
 * 2^-149 - (2^-150 - 2^-175) をPREC=S・RNで計算すると、加算経路の反例
 * （2^-150+2^-175）と同じく単精度の最小非正規化数 0x00000001 になる。加算経路の
 * テストだけでは、自前実装の減算経路（design §3.2.2）固有の欠陥を検出できない。 */
static void test_prec_s_boundary_sub(void) {
    mc68k_fpu_state_t any = make_state(0, 0);
    mc68k_fpu_state_t rn_x = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_state_t rn_s = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    /* 指数field: 2^-149→874=0x36A、2^-150→873=0x369、2^-175→848=0x350 */
    mc68k_fp80_t p149 = mc68k_fpu_from_d(UINT64_C(0x36A0000000000000), &any);
    mc68k_fp80_t p150 = mc68k_fpu_from_d(UINT64_C(0x3690000000000000), &any);
    mc68k_fp80_t p175 = mc68k_fpu_from_d(UINT64_C(0x3500000000000000), &any);
    mc68k_fp80_t src, r;

    /* src = 2^-150 - 2^-175（拡張精度では厳密。桁の余裕が63bitよりはるかに大きい） */
    src = mc68k_fpu_sub(p150, p175, &rn_x);
    r = mc68k_fpu_sub(p149, src, &rn_s);
    /* B-B5(準ずる): to_s(r)だけだとPREC=Xのまま丸めた値（3F69/8000004000000000）も
     * to_s経由で再丸めされて同じ0x00000001になり、PREC=Sが実際に効いたことを検出
     * できない（&rn_sを&rn_xに変える変異が生存していた）。拡張精度のFPn格納値
     * （3F6A/8000000000000000）そのものを検査する。 */
    CHECK_FP(r, 0x3F6A, SIG_J);
    CHECK(mc68k_fpu_to_s(r, &rn_s) == 1u);
}

/* PREC=D非正規化域の反例（design §5.7、加算・減算それぞれ1件、keep==1） */
static void test_prec_d_boundary(void) {
    mc68k_fpu_state_t rn_d = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_DOUBLE);
    mc68k_fp80_t x3bcc = fp(0x3BCC, SIG_J);
    mc68k_fp80_t x3bcd = fp(0x3BCD, SIG_J);
    mc68k_fp80_t x3bce = fp(0x3BCE, SIG_J);

    /* 2^-1074 + 2^-1075 → 1.5×2^-1074 → 偶数側の2^-1073（3BCE）へ丸める */
    CHECK_FP(mc68k_fpu_add(x3bcd, x3bcc, &rn_d), 0x3BCE, SIG_J);
    /* 2^-1073 - 2^-1075 → 同じく3BCE */
    CHECK_FP(mc68k_fpu_sub(x3bce, x3bcc, &rn_d), 0x3BCE, SIG_J);
}

/* Xの最小非正規化数付近のFMUL（design §3.3.1、§5.7必須ケース） */
static void test_mul_x_denormal_boundary(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fpu_state_t rp = make_state(MC68K_FPU_ROUND_PLUS_INF, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t min_denorm = fp(0x0000, 1); /* 2^-16446 */

    /* ×0.5、ちょうど半分: RNは偶数側の+0 */
    CHECK_FP(mc68k_fpu_mul(min_denorm, fp(0x3FFE, SIG_J), &rn), 0x0000, 0);
    /* ×0.75: RNは1ULPへ丸め上げ */
    CHECK_FP(mc68k_fpu_mul(min_denorm, fp(0x3FFE, UINT64_C(0xC000000000000000)), &rn), 0x0000,
             1);
    /* ×0.25（正のexp=-65相当）のRP: 1ULPへ丸め上げ */
    CHECK_FP(mc68k_fpu_mul(min_denorm, fp(0x3FFD, SIG_J), &rp), 0x0000, 1);
}

/* S精度のRZオーバーフロー（design §3.3.1、§5.7必須ケース） */
static void test_mul_s_rz_overflow(void) {
    mc68k_fpu_state_t rz_s = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_SINGLE);

    CHECK_FP(mc68k_fpu_mul(fp(0x407E, SIG_J), fp(0x4000, SIG_J), &rz_s), 0x407E,
             UINT64_C(0xFFFFFF0000000000));
}

/* ---- FABS/FNEG（design §5.4） ------------------------------------------------- */

static void test_abs_neg(void) {
    mc68k_fpu_state_t rn_s = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fp80_t v = fp(0x3FFF, UINT64_C(0x8000000000400000)); /* 1+2^-30 */
    mc68k_fp80_t neg_v = fp(0xBFFF, UINT64_C(0x8000000000400000));
    mc68k_fp80_t neg_qnan = fp(0xFFFF, UINT64_C(0xC000000000000001));
    mc68k_fp80_t snan = fp(0x7FFF, UINT64_C(0x9000000000000000));
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);
    mc68k_fp80_t neg_inf = fp(0xFFFF, SIG_J);
    mc68k_fp80_t pz = fp(0x0000, 0);
    mc68k_fp80_t nz = fp(0x8000, 0);

    CHECK_FP(mc68k_fpu_abs(v, &rn_s), 0x3FFF, SIG_J);
    CHECK_FP(mc68k_fpu_neg(v, &rn_s), 0xBFFF, SIG_J);
    CHECK_FP(mc68k_fpu_abs(neg_v, &rn_s), 0x3FFF, SIG_J);

    /* NaN・±Inf・±0は符号操作のみ（丸めを経由しない）。B-N6: 正負両方の符号を確認する
     * （neg(+0)→-0・neg(+Inf)→-Infは元々未検査だった）。 */
    CHECK_FP(mc68k_fpu_abs(neg_qnan, &rn_s), 0x7FFF, UINT64_C(0xC000000000000001));
    CHECK_FP(mc68k_fpu_neg(neg_qnan, &rn_s), 0x7FFF, UINT64_C(0xC000000000000001));
    CHECK_FP(mc68k_fpu_abs(snan, &rn_s), 0x7FFF, UINT64_C(0xD000000000000000));
    CHECK_FP(mc68k_fpu_abs(pinf, &rn_s), 0x7FFF, SIG_J);
    CHECK_FP(mc68k_fpu_neg(pinf, &rn_s), 0xFFFF, SIG_J);
    CHECK_FP(mc68k_fpu_abs(neg_inf, &rn_s), 0x7FFF, SIG_J);
    CHECK_FP(mc68k_fpu_neg(neg_inf, &rn_s), 0x7FFF, SIG_J);
    CHECK_FP(mc68k_fpu_abs(pz, &rn_s), 0x0000, 0);
    CHECK_FP(mc68k_fpu_neg(pz, &rn_s), 0x8000, 0);
    CHECK_FP(mc68k_fpu_abs(nz, &rn_s), 0x0000, 0);
    CHECK_FP(mc68k_fpu_neg(nz, &rn_s), 0x0000, 0);
}

/* ---- 条件コード（design §5.5、通常算術。8パターン） --------------------------- */

static void test_condition_codes(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);
    mc68k_fp80_t two = fp(0x4000, SIG_J);
    mc68k_fp80_t nan = fp(0x7FFF, UINT64_C(0xC000000000000001));

    mc68k_fpu_add(one, one, &rn); /* 2.0: 正規化数(+) */
    CHECK(rn.fpsr_cc == 0);
    mc68k_fpu_sub(one, two, &rn); /* -1.0: 正規化数(-) */
    CHECK(rn.fpsr_cc == MC68K_FPU_CC_N);
    mc68k_fpu_sub(one, one, &rn); /* +0 */
    CHECK(rn.fpsr_cc == MC68K_FPU_CC_Z);
    mc68k_fpu_add(fp(0x8000, 0), fp(0x8000, 0), &rn); /* -0 */
    CHECK(rn.fpsr_cc == (MC68K_FPU_CC_N | MC68K_FPU_CC_Z));
    mc68k_fpu_div(one, fp(0x0000, 0), &rn); /* +Inf */
    CHECK(rn.fpsr_cc == MC68K_FPU_CC_I);
    mc68k_fpu_div(fp(0xBFFF, SIG_J), fp(0x0000, 0), &rn); /* -Inf */
    CHECK(rn.fpsr_cc == (MC68K_FPU_CC_N | MC68K_FPU_CC_I));
    mc68k_fpu_add(nan, one, &rn); /* +NaN */
    CHECK(rn.fpsr_cc == MC68K_FPU_CC_NAN);
    mc68k_fpu_add(fp(0xFFFF, UINT64_C(0xC000000000000001)), one, &rn); /* -NaN */
    CHECK(rn.fpsr_cc == (MC68K_FPU_CC_N | MC68K_FPU_CC_NAN));
}

/* ---- FCMP（design §5.4、必須ケース） ------------------------------------------ */

static void check_cmp_mode(mc68k_fp80_t dst, mc68k_fp80_t src, uint8_t mode, uint8_t expect_cc,
                           const char *label) {
    mc68k_fpu_state_t st = make_state(mode, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t dst_before = dst, src_before = src;

    mc68k_fpu_cmp(dst, src, &st);
    if (st.fpsr_cc != expect_cc) {
        g_failures++;
        fprintf(stderr, "fcmp %s: got cc=%x, expected %x\n", label, st.fpsr_cc, expect_cc);
    }
    /* dst/srcは変更されない */
    if (dst.sign_exp != dst_before.sign_exp || dst.signif != dst_before.signif ||
        src.sign_exp != src_before.sign_exp || src.signif != src_before.signif) {
        g_failures++;
        fprintf(stderr, "fcmp %s: operand mutated\n", label);
    }
}

static void check_cmp(mc68k_fp80_t dst, mc68k_fp80_t src, uint8_t expect_cc,
                      const char *label) {
    check_cmp_mode(dst, src, MC68K_FPU_ROUND_NEAREST, expect_cc, label);
}

static void test_fcmp(void) {
    mc68k_fp80_t pinf = fp(0x7FFF, SIG_J);
    mc68k_fp80_t minf = fp(0xFFFF, SIG_J);
    mc68k_fp80_t pz = fp(0x0000, 0);
    mc68k_fp80_t nz = fp(0x8000, 0);
    mc68k_fp80_t one = fp(0x3FFF, SIG_J);
    mc68k_fp80_t neg_one = fp(0xBFFF, SIG_J);
    mc68k_fp80_t two = fp(0x4000, SIG_J);
    mc68k_fp80_t neg_two = fp(0xC000, SIG_J);

    /* 表と実減算が食い違う反例3件（Opusレビュー blocking #4）。design §5.4の反例文言
     * どおり、-0/-0はRN、+0/+0はRMで検査する（B-B5: どちらもRN固定だと+0/+0・RMの
     * 反例[実減算なら-0でNZになるところ、表はZ]が未検証になっていた）。 */
    check_cmp(pinf, pinf, MC68K_FPU_CC_Z, "dst=+inf,src=+inf");
    check_cmp_mode(nz, nz, MC68K_FPU_ROUND_NEAREST, MC68K_FPU_CC_N | MC68K_FPU_CC_Z,
                   "dst=-0,src=-0 (RN)");
    check_cmp_mode(pz, pz, MC68K_FPU_ROUND_MINUS_INF, MC68K_FPU_CC_Z, "dst=+0,src=+0 (RM)");
    /* 同じ+0,+0がRN等の他モードでもZのまま（丸めモードに依存しないことの確認） */
    check_cmp_mode(pz, pz, MC68K_FPU_ROUND_NEAREST, MC68K_FPU_CC_Z, "dst=+0,src=+0 (RN)");

    /* {NZ}セルの等値ケース */
    check_cmp(neg_one, neg_one, MC68K_FPU_CC_N | MC68K_FPU_CC_Z, "dst=src=-1.0");
    check_cmp(one, one, MC68K_FPU_CC_Z, "dst=src=+1.0");
    check_cmp(neg_two, neg_one, MC68K_FPU_CC_N, "dst=-2.0,src=-1.0");
    check_cmp(neg_one, neg_two, 0, "dst=-1.0,src=-2.0");

    /* ゼロ同士の異符号 */
    check_cmp(pz, nz, MC68K_FPU_CC_Z, "dst=+0,src=-0");
    check_cmp(nz, pz, MC68K_FPU_CC_N | MC68K_FPU_CC_Z, "dst=-0,src=+0");

    /* 6x6表の代表セル */
    check_cmp(one, neg_one, 0, "dst=+有限,src=-有限");
    check_cmp(neg_one, one, MC68K_FPU_CC_N, "dst=-有限,src=+有限");
    check_cmp(pz, pinf, MC68K_FPU_CC_N, "dst=+0,src=+inf");
    check_cmp(pinf, pz, 0, "dst=+inf,src=+0");
    check_cmp(minf, minf, MC68K_FPU_CC_N | MC68K_FPU_CC_Z, "dst=-inf,src=-inf");
    check_cmp(minf, pinf, MC68K_FPU_CC_N, "dst=-inf,src=+inf");
    check_cmp(pinf, minf, 0, "dst=+inf,src=-inf");
    check_cmp(nz, pinf, MC68K_FPU_CC_N, "dst=-0,src=+inf");

    /* NaNオペランド: NANビットのみ */
    check_cmp(fp(0x7FFF, UINT64_C(0xC000000000000001)), one, MC68K_FPU_CC_NAN, "dst=NaN");
    check_cmp(one, fp(0x7FFF, UINT64_C(0xC000000000000001)), MC68K_FPU_CC_NAN, "src=NaN");
}

/* ---- FTST（design §5.5の表と一致） --------------------------------------------- */

static void test_ftst(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    static const struct {
        unsigned sexp;
        uint64_t signif;
        uint8_t cc;
    } cases[] = {
        {0x3FFF, SIG_J, 0},
        {0xBFFF, SIG_J, MC68K_FPU_CC_N},
        {0x0000, 0, MC68K_FPU_CC_Z},
        {0x8000, 0, MC68K_FPU_CC_N | MC68K_FPU_CC_Z},
        {0x7FFF, SIG_J, MC68K_FPU_CC_I},
        {0xFFFF, SIG_J, MC68K_FPU_CC_N | MC68K_FPU_CC_I},
        {0x7FFF, UINT64_C(0xC000000000000001), MC68K_FPU_CC_NAN},
        {0xFFFF, UINT64_C(0xC000000000000001), MC68K_FPU_CC_N | MC68K_FPU_CC_NAN},
    };
    int n = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mc68k_fpu_tst(fp(cases[i].sexp, cases[i].signif), &st);
        CHECK(st.fpsr_cc == cases[i].cc);
        n++;
    }
    CHECK(n == 8);
}

int main(void) {
    test_addsub_exp_zero_examples();
    test_addsub_prec_x_rounding();
    test_addsub_zero_cancellation();
    test_addsub_unnormalized_zero_cancellation();
    test_addsub_zero_plus_denormal();
    test_addsub_unnormalized();
    test_addsub_zero_with_nan_inf();
    test_addsub_infinity_operr();
    test_nan_propagation();
    test_nan_j_bit_zero();
    test_mul_div_inf_zero();
    test_mul_div_exp_zero_entry();
    test_prec_s_boundary();
    test_prec_s_boundary_sub();
    test_prec_d_boundary();
    test_mul_x_denormal_boundary();
    test_mul_s_rz_overflow();
    test_abs_neg();
    test_condition_codes();
    test_fcmp();
    test_ftst();

    if (g_failures) {
        fprintf(stderr, "test_fpu_arith: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_fpu_arith: OK\n");
    return 0;
}
