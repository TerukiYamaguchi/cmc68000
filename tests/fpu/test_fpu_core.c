/*
 * test_fpu_core.c — FPU共通コア Phase A（状態表現とデータ型変換）のテスト。
 * 設計は 現行仕様。
 *
 * 大量のケースは tests/fpu/vectors/ 配下の .txt から読む。期待値は
 * tests/fpu/gen_fpu_core_vectors.py が fractions.Fraction の厳密演算で独立に求めたもの
 * （production のビット操作は使わない）。各ファイルの先頭行 `count N` と実行件数を照合する。
 * 個別の名前付きケースは、設計書・一次資料で値が決まっているものを直接書く。
 *
 * Phase A は st->fpsr_exc / fpsr_aexc を書かないため、フラグは検査しない（roundPack の
 * 内部フラグだけ softfloat_roundPackToExtF80() を直接呼んで検査する）。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/fpu.h"
#include "platform.h"
#include "internals.h"
#include "softfloat.h"

#define SIG_J UINT64_C(0x8000000000000000)

static int g_failures;

/* mc68k_fpu_round_mode_t → SoftFloat の丸めモード */
static const uint_fast8_t sf_mode[4] = {softfloat_round_near_even, softfloat_round_minMag,
                                        softfloat_round_min, softfloat_round_max};

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

/* ---- ベクタファイル ---------------------------------------------------- */

static FILE *open_vectors(const char *name, long *count) {
    char path[1024];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", MC68K_FPU_VECTOR_DIR, name);
    f = fopen(path, "r");
    if (!f || fscanf(f, "count %ld\n", count) != 1) {
        fprintf(stderr, "cannot read vector file %s\n", path);
        g_failures++;
        if (f)
            fclose(f);
        return NULL;
    }
    return f;
}

/* 1件の検査。line は末尾改行を含む1行。失敗ならメッセージ用に line を出力する。 */
typedef bool (*vector_fn)(const char *line);

static void run_vectors(const char *name, long min_count, vector_fn fn) {
    long declared = 0, executed = 0;
    char line[512];
    FILE *f = open_vectors(name, &declared);

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (!fn(line)) {
            g_failures++;
            fprintf(stderr, "%s: mismatch: %s", name, line);
        }
        executed++;
    }
    fclose(f);
    if (executed != declared || declared < min_count) {
        g_failures++;
        fprintf(stderr, "%s: executed %ld, declared %ld, minimum %ld\n", name, executed,
                declared, min_count);
    }
}

static bool fp_equals(mc68k_fp80_t v, unsigned sexp, uint64_t signif) {
    return v.sign_exp == (uint16_t)sexp && v.signif == signif;
}

static bool vec_roundpack(const char *line) {
    int sign, prec, mode;
    long exp;
    uint64_t sig, extra, signif;
    unsigned sexp, flags;
    extFloat80_t r;
    uint_fast8_t got_flags;

    if (sscanf(line, "rp %d %ld %" SCNx64 " %" SCNx64 " %d %d => %x %" SCNx64 " %x", &sign,
               &exp, &sig, &extra, &prec, &mode, &sexp, &signif, &flags) != 9) {
        return false;
    }
    if (mode < 0 || mode > 3)
        return false;
    softfloat_roundingMode = sf_mode[mode];
    softfloat_exceptionFlags = 0;
    r = softfloat_roundPackToExtF80(sign != 0, (int_fast32_t)exp, sig, extra,
                                    (uint_fast8_t)prec);
    got_flags = softfloat_exceptionFlags;
    softfloat_exceptionFlags = 0;
    return r.signExp == (uint16_t)sexp && r.signif == signif && got_flags == flags;
}

static bool vec_from_x(const char *line) {
    unsigned sexp, esexp;
    uint64_t signif, esignif;
    mc68k_fpu_state_t st = make_state(0, 0);
    mc68k_fp80_t v;

    if (sscanf(line, "from_x %x %" SCNx64 " => %x %" SCNx64, &sexp, &signif, &esexp,
               &esignif) != 4) {
        return false;
    }
    v.sign_exp = (uint16_t)sexp;
    v.signif = signif;
    return fp_equals(mc68k_fpu_from_x(v, &st), esexp, esignif);
}

static bool vec_to_x(const char *line) {
    int mode;
    unsigned sexp, esexp;
    uint64_t signif, esignif;
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    if (sscanf(line, "to_x %d %x %" SCNx64 " => %x %" SCNx64, &mode, &sexp, &signif, &esexp,
               &esignif) != 5) {
        return false;
    }
    st = make_state((uint8_t)mode, MC68K_FPU_PREC_SINGLE);
    v.sign_exp = (uint16_t)sexp;
    v.signif = signif;
    return fp_equals(mc68k_fpu_to_x(v, &st), esexp, esignif);
}

static bool vec_to_s(const char *line) {
    int mode;
    unsigned sexp;
    uint64_t signif, bits;
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    if (sscanf(line, "to_s %d %x %" SCNx64 " => %" SCNx64, &mode, &sexp, &signif, &bits) != 4) {
        return false;
    }
    /* PRECはストアに影響しない。わざと拡張精度以外を入れておく。 */
    st = make_state((uint8_t)mode, MC68K_FPU_PREC_DOUBLE);
    v.sign_exp = (uint16_t)sexp;
    v.signif = signif;
    return mc68k_fpu_to_s(v, &st) == (uint32_t)bits;
}

static bool vec_to_d(const char *line) {
    int mode;
    unsigned sexp;
    uint64_t signif, bits;
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    if (sscanf(line, "to_d %d %x %" SCNx64 " => %" SCNx64, &mode, &sexp, &signif, &bits) != 4) {
        return false;
    }
    st = make_state((uint8_t)mode, MC68K_FPU_PREC_SINGLE);
    v.sign_exp = (uint16_t)sexp;
    v.signif = signif;
    return mc68k_fpu_to_d(v, &st) == bits;
}

static bool vec_from_s(const char *line) {
    uint64_t bits, esignif;
    unsigned esexp;
    mc68k_fpu_state_t st = make_state(0, 0);

    if (sscanf(line, "from_s %" SCNx64 " => %x %" SCNx64, &bits, &esexp, &esignif) != 3)
        return false;
    return fp_equals(mc68k_fpu_from_s((uint32_t)bits, &st), esexp, esignif);
}

static bool vec_from_d(const char *line) {
    uint64_t bits, esignif;
    unsigned esexp;
    mc68k_fpu_state_t st = make_state(0, 0);

    if (sscanf(line, "from_d %" SCNx64 " => %x %" SCNx64, &bits, &esexp, &esignif) != 3)
        return false;
    return fp_equals(mc68k_fpu_from_d(bits, &st), esexp, esignif);
}

static bool vec_move(const char *line) {
    int prec, mode;
    unsigned sexp, esexp;
    uint64_t signif, esignif;
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    if (sscanf(line, "move %d %d %x %" SCNx64 " => %x %" SCNx64, &prec, &mode, &sexp, &signif,
               &esexp, &esignif) != 6) {
        return false;
    }
    st = make_state((uint8_t)mode, (uint8_t)prec);
    v.sign_exp = (uint16_t)sexp;
    v.signif = signif;
    return fp_equals(mc68k_fpu_move(v, &st), esexp, esignif);
}

static bool vec_to_int(const char *line) {
    int bits, mode;
    unsigned sexp;
    uint64_t signif;
    long long expected;
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    if (sscanf(line, "to_int %d %d %x %" SCNx64 " => %lld", &bits, &mode, &sexp, &signif,
               &expected) != 5) {
        return false;
    }
    st = make_state((uint8_t)mode, MC68K_FPU_PREC_SINGLE);
    v.sign_exp = (uint16_t)sexp;
    v.signif = signif;
    switch (bits) {
    case 8:
        return mc68k_fpu_to_b(v, &st) == expected;
    case 16:
        return mc68k_fpu_to_w(v, &st) == expected;
    case 32:
        return mc68k_fpu_to_l(v, &st) == expected;
    default:
        return false;
    }
}

static bool vec_from_int(const char *line) {
    int bits;
    long long value;
    unsigned esexp;
    uint64_t esignif;
    mc68k_fp80_t v;

    if (sscanf(line, "from_int %d %lld => %x %" SCNx64, &bits, &value, &esexp, &esignif) != 4)
        return false;
    switch (bits) {
    case 8:
        v = mc68k_fpu_from_b((int8_t)value);
        break;
    case 16:
        v = mc68k_fpu_from_w((int16_t)value);
        break;
    case 32:
        v = mc68k_fpu_from_l((int32_t)value);
        break;
    default:
        return false;
    }
    return fp_equals(v, esexp, esignif);
}

/* ---- 名前付きケース ---------------------------------------------------- */

static void test_reset(void) {
    mc68k_fpu_state_t st;

    int n = 0;

    memset(&st, 0xA5, sizeof(st));
    mc68k_fpu_reset(&st);
    for (int i = 0; i < 8; i++) {
        n++;
        CHECK(st.fp[i].signif == UINT64_MAX);
        /* 正の非シグナリングNaN（指数7FFF、フラクション最上位ビット=1） */
        CHECK(st.fp[i].sign_exp == 0x7FFF);
        CHECK((st.fp[i].signif & UINT64_C(0x4000000000000000)) != 0);
        CHECK((st.fp[i].signif & UINT64_C(0x7FFFFFFFFFFFFFFF)) != 0);
    }
    CHECK(st.fpcr_enable == 0 && st.fpcr_round_mode == 0 && st.fpcr_round_prec == 0);
    CHECK(st.fpsr_cc == 0 && st.fpsr_exc == 0 && st.fpsr_aexc == 0);
    CHECK(st.fpsr_quotient == 0 && st.fpsr_quotient_sign == 0 && st.fpiar == 0);
    CHECK(n == 8);
}

static void test_int_roundtrip(void) {
    static const int32_t values[] = {
        0,
        1,
        -1,
        2,
        INT32_MAX,
        INT32_MIN,
        INT32_MAX - 1,
        INT32_MIN + 1,
        INT16_MAX,
        INT16_MIN,
        INT8_MAX,
        INT8_MIN,
        16777217,
        -16777217,
    };
    long n = 0;

    for (int prec = 0; prec < 3; prec++) {
        mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, (uint8_t)prec);

        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            CHECK(mc68k_fpu_to_l(mc68k_fpu_from_l(values[i]), &st) == values[i]);
            n++;
            if (values[i] >= INT16_MIN && values[i] <= INT16_MAX) {
                CHECK(mc68k_fpu_to_w(mc68k_fpu_from_w((int16_t)values[i]), &st) == values[i]);
                n++;
            }
            if (values[i] >= INT8_MIN && values[i] <= INT8_MAX) {
                CHECK(mc68k_fpu_to_b(mc68k_fpu_from_b((int8_t)values[i]), &st) == values[i]);
                n++;
            }
        }
    }
    CHECK(n == 3 * (14 + 8 + 6));
}

/* 名前付きの丸め: 2.5 / 3.5 / -2.5 を4モードでLへ */
static void test_int_rounding_modes(void) {
    mc68k_fp80_t p25 = {0x4000, UINT64_C(0xA000000000000000)};
    mc68k_fp80_t p35 = {0x4000, UINT64_C(0xE000000000000000)};
    mc68k_fp80_t m25 = {0xC000, UINT64_C(0xA000000000000000)};
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fpu_state_t rz = make_state(MC68K_FPU_ROUND_ZERO, 0);
    mc68k_fpu_state_t rm = make_state(MC68K_FPU_ROUND_MINUS_INF, 0);
    mc68k_fpu_state_t rp = make_state(MC68K_FPU_ROUND_PLUS_INF, 0);

    CHECK(mc68k_fpu_to_l(p25, &rn) == 2);
    CHECK(mc68k_fpu_to_l(p35, &rn) == 4);
    CHECK(mc68k_fpu_to_l(m25, &rn) == -2);
    CHECK(mc68k_fpu_to_l(p25, &rz) == 2);
    CHECK(mc68k_fpu_to_l(m25, &rz) == -2);
    CHECK(mc68k_fpu_to_l(p25, &rm) == 2);
    CHECK(mc68k_fpu_to_l(m25, &rm) == -3);
    CHECK(mc68k_fpu_to_l(p25, &rp) == 3);
    CHECK(mc68k_fpu_to_l(m25, &rp) == -2);
}

/* B/W/L destination の特殊ケース（[881UM Table 6-2, §6.1.3]） */
static void test_int_operr_values(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fp80_t qnan = {0x7FFF, UINT64_C(0xC000000000000000)};
    mc68k_fp80_t snan = {0x7FFF, UINT64_C(0xA000000000000000)};
    mc68k_fp80_t pinf = {0x7FFF, SIG_J};
    mc68k_fp80_t minf = {0xFFFF, SIG_J};
    mc68k_fp80_t big = {0x401E + 1, SIG_J};                             /* 2^32 */
    mc68k_fp80_t neg_over = {0xC01E, UINT64_C(0x8000000100000000)};     /* -(2^31+1) */
    mc68k_fp80_t b_over = {0x4006, UINT64_C(0xFF00000000000000)};       /* 255 */
    mc68k_fp80_t b_round_over = {0x4005, UINT64_C(0xFF00000000000000)}; /* 127.5 */

    /* NaN: 仮数の上位ビット。SNaNはSNaNビットを立てたもの */
    CHECK(mc68k_fpu_to_b(qnan, &st) == (int8_t)0xC0);
    CHECK(mc68k_fpu_to_w(qnan, &st) == (int16_t)0xC000);
    CHECK(mc68k_fpu_to_l(qnan, &st) == (int32_t)0xC0000000);
    CHECK(mc68k_fpu_to_b(snan, &st) == (int8_t)0xE0);
    CHECK(mc68k_fpu_to_w(snan, &st) == (int16_t)0xE000);
    CHECK(mc68k_fpu_to_l(snan, &st) == (int32_t)0xE0000000);
    /* 無限大 */
    CHECK(mc68k_fpu_to_b(pinf, &st) == INT8_MAX && mc68k_fpu_to_b(minf, &st) == INT8_MIN);
    CHECK(mc68k_fpu_to_w(pinf, &st) == INT16_MAX && mc68k_fpu_to_w(minf, &st) == INT16_MIN);
    CHECK(mc68k_fpu_to_l(pinf, &st) == INT32_MAX && mc68k_fpu_to_l(minf, &st) == INT32_MIN);
    /* 整数レンジ外 */
    CHECK(mc68k_fpu_to_l(big, &st) == INT32_MAX);
    CHECK(mc68k_fpu_to_l(neg_over, &st) == INT32_MIN);
    CHECK(mc68k_fpu_to_b(b_over, &st) == INT8_MAX);
    /* 丸めた結果が範囲外（127.5 → 128）でも最大値 */
    CHECK(mc68k_fpu_to_b(b_round_over, &st) == INT8_MAX);
}

/* S/D のNaN: SNaNビットを立てて仮数を切り詰める / 取り込む（[881UM §6.1.2]）。
 * 期待値は仮数のビット位置から手で求めた値。 */
static void test_nan_binary(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, 0);
    mc68k_fp80_t qnan = {0x7FFF, UINT64_C(0xE000000000000000)};
    mc68k_fp80_t snan = {0x7FFF, UINT64_C(0xA000000000000000)};
    mc68k_fp80_t nsnan = {0xFFFF, UINT64_C(0xA000000000000000)};

    CHECK(mc68k_fpu_to_s(qnan, &st) == 0x7FE00000u);
    CHECK(mc68k_fpu_to_s(snan, &st) == 0x7FE00000u);
    CHECK(mc68k_fpu_to_d(nsnan, &st) == UINT64_C(0xFFFC000000000000));
    CHECK_FP(mc68k_fpu_from_s(0x7FA00000u, &st), 0x7FFF, UINT64_C(0xA000000000000000));
    CHECK_FP(mc68k_fpu_from_d(UINT64_C(0xFFF4000000000000), &st), 0xFFFF,
             UINT64_C(0xA000000000000000));
}

static void test_from_x_exponent_zero(void) {
    mc68k_fpu_state_t st = make_state(0, 0);
    mc68k_fp80_t min_normal = {0x0000, SIG_J};
    mc68k_fp80_t denorm = {0x0000, UINT64_C(0x4000000000000000)};
    mc68k_fp80_t min_denorm = {0x0000, 1};
    mc68k_fp80_t e1 = {0x0001, SIG_J};
    mc68k_fp80_t unnorm_e5 = {0x0005, 1};
    mc68k_fp80_t unnorm_e40 = {0x0040, 1};
    mc68k_fp80_t unnorm_zero = {0x8001, 0};
    int n = 0;

    for (int prec = 0; prec < 3; prec++) {
        n++;
        st.fpcr_round_prec = (uint8_t)prec;
        CHECK_FP(mc68k_fpu_to_x(mc68k_fpu_from_x(min_normal, &st), &st), 0x0000, SIG_J);
        CHECK_FP(mc68k_fpu_to_x(mc68k_fpu_from_x(denorm, &st), &st), 0x0000,
                 UINT64_C(0x4000000000000000));
        CHECK_FP(mc68k_fpu_to_x(mc68k_fpu_from_x(min_denorm, &st), &st), 0x0000, 1);
        CHECK_FP(mc68k_fpu_to_x(mc68k_fpu_from_x(e1, &st), &st), 0x0001, SIG_J);
    }
    /* unnormalized: 指数の許す範囲でシフトし、足りなければ非正規化数 */
    CHECK_FP(mc68k_fpu_from_x(unnorm_e5, &st), 0x0000, 0x20);
    CHECK_FP(mc68k_fpu_from_x(unnorm_e40, &st), 0x0001, SIG_J);
    CHECK_FP(mc68k_fpu_from_x(unnorm_zero, &st), 0x8000, 0);
    CHECK(n == 3);
}

/* from_* は常に厳密（PRECで変わらない） */
static void test_from_is_exact(void) {
    static const uint8_t precs[] = {MC68K_FPU_PREC_EXTENDED, MC68K_FPU_PREC_SINGLE,
                                    MC68K_FPU_PREC_DOUBLE};
    int n = 0;

    for (size_t i = 0; i < sizeof(precs); i++) {
        n++;
        mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, precs[i]);
        mc68k_fp80_t x = {0x3FFF, UINT64_C(0x8000000000000001)};

        CHECK_FP(mc68k_fpu_from_l(16777217), 0x4017, UINT64_C(0x8000008000000000));
        CHECK_FP(mc68k_fpu_from_d(UINT64_C(0x3FF0000000000001), &st), 0x3FFF,
                 UINT64_C(0x8000000000000800));
        CHECK_FP(mc68k_fpu_from_x(x, &st), 0x3FFF, UINT64_C(0x8000000000000001));
        CHECK_FP(mc68k_fpu_from_s(0x3F800001, &st), 0x3FFF, UINT64_C(0x8000010000000000));
    }
    CHECK(n == 3);
}

/* mc68k_fpu_move() のPREC依存の丸め */
static void test_move_prec(void) {
    mc68k_fpu_state_t single = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t dbl = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_DOUBLE);
    mc68k_fpu_state_t ext = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t l = mc68k_fpu_from_l(16777217); /* 2^24 + 1 */
    mc68k_fp80_t s = mc68k_fpu_from_s(0x3F800001, &ext);
    mc68k_fp80_t d = mc68k_fpu_from_d(UINT64_C(0x3FF0000000000001), &ext);
    mc68k_fp80_t b = mc68k_fpu_from_b(-100);

    CHECK_FP(mc68k_fpu_move(l, &single), 0x4017, SIG_J); /* 2^24 */
    CHECK_FP(mc68k_fpu_move(l, &dbl), 0x4017, UINT64_C(0x8000008000000000));
    CHECK_FP(mc68k_fpu_move(l, &ext), 0x4017, UINT64_C(0x8000008000000000));
    CHECK_FP(mc68k_fpu_move(d, &single), 0x3FFF, SIG_J); /* 1+2^-52 → 1 */
    CHECK_FP(mc68k_fpu_move(d, &dbl), 0x3FFF, UINT64_C(0x8000000000000800));
    /* B/W/S 由来の値は、どのPRECでも変わらない */
    CHECK_FP(mc68k_fpu_move(s, &single), 0x3FFF, UINT64_C(0x8000010000000000));
    CHECK_FP(mc68k_fpu_move(b, &single), 0xC005, UINT64_C(0xC800000000000000));
}

/* 値の範囲制限（[881UM §2.2.2]。design §4.4 必須ケース） */
static void test_move_range(void) {
    mc68k_fpu_state_t rn = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t rz = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t rp = make_state(MC68K_FPU_ROUND_PLUS_INF, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_state_t rm = make_state(MC68K_FPU_ROUND_MINUS_INF, MC68K_FPU_PREC_SINGLE);
    mc68k_fp80_t big = mc68k_fpu_from_d(UINT64_C(0x7FEFFFFFFFFFFFFF), &rn);
    mc68k_fp80_t tiny = mc68k_fpu_from_d(1, &rn);
    mc68k_fp80_t ntiny = mc68k_fpu_from_d(UINT64_C(0x8000000000000001), &rn);

    CHECK_FP(mc68k_fpu_move(big, &rn), 0x7FFF, SIG_J); /* +Inf */
    CHECK_FP(mc68k_fpu_move(big, &rz), 0x407E, UINT64_C(0xFFFFFF0000000000));
    CHECK_FP(mc68k_fpu_move(tiny, &rn), 0x0000, 0);
    CHECK_FP(mc68k_fpu_move(tiny, &rp), 0x3F6A, SIG_J);
    /* 負符号と RM/RP の方向（[881UM §6.1.5] Trap Disabled 表） */
    CHECK_FP(mc68k_fpu_move(ntiny, &rn), 0x8000, 0);
    CHECK_FP(mc68k_fpu_move(ntiny, &rz), 0x8000, 0);
    CHECK_FP(mc68k_fpu_move(ntiny, &rm), 0xBF6A, SIG_J);
    CHECK_FP(mc68k_fpu_move(ntiny, &rp), 0x8000, 0);
}

/* keep==0（S: 2^-150 近傍）の分岐 */
static void test_move_keep_zero(void) {
    mc68k_fpu_state_t ext = make_state(0, 0);
    mc68k_fp80_t half = mc68k_fpu_from_d(UINT64_C(0x3690000000000000), &ext); /* 2^-150 */
    mc68k_fp80_t over =
        mc68k_fpu_from_d(UINT64_C(0x3690000008000000), &ext); /* 2^-150 + 2^-175 */
    mc68k_fp80_t nhalf = mc68k_fpu_from_d(UINT64_C(0xB690000000000000), &ext);
    mc68k_fp80_t nover = mc68k_fpu_from_d(UINT64_C(0xB690000008000000), &ext);
    static const uint8_t modes[] = {MC68K_FPU_ROUND_NEAREST, MC68K_FPU_ROUND_ZERO,
                                    MC68K_FPU_ROUND_MINUS_INF, MC68K_FPU_ROUND_PLUS_INF};
    /* 出典: [881UM §6.1.5] Trap Disabled 表（RZ/RM/RP、およびちょうど半分のRN）。
     * 半分超のRNは IEEE754 に基づく推論（設計書 §9）。 */
    static const struct {
        int which;
        uint16_t sexp[4];
        uint64_t sig[4];
    } cases[] = {
        {0, {0x0000, 0x0000, 0x0000, 0x3F6A}, {0, 0, 0, SIG_J}},
        {1, {0x3F6A, 0x0000, 0x0000, 0x3F6A}, {SIG_J, 0, 0, SIG_J}},
        {2, {0x8000, 0x8000, 0xBF6A, 0x8000}, {0, 0, SIG_J, 0}},
        {3, {0xBF6A, 0x8000, 0xBF6A, 0x8000}, {SIG_J, 0, SIG_J, 0}},
    };
    mc68k_fp80_t inputs[4];
    int n = 0;

    inputs[0] = half;
    inputs[1] = over;
    inputs[2] = nhalf;
    inputs[3] = nover;
    for (size_t c = 0; c < 4; c++) {
        for (size_t m = 0; m < 4; m++) {
            mc68k_fpu_state_t st = make_state(modes[m], MC68K_FPU_PREC_SINGLE);
            CHECK_FP(mc68k_fpu_move(inputs[cases[c].which], &st), cases[c].sexp[m],
                     cases[c].sig[m]);
            n++;
        }
    }
    CHECK(n == 16);
}

/* ゼロ判定: unnormalized zero は符号付きの正規のゼロになる */
static void test_move_zero(void) {
    mc68k_fp80_t pz = {0x0001, 0};
    mc68k_fp80_t nz = {0x8001, 0};
    int n = 0;

    for (int prec = 0; prec < 3; prec++) {
        n++;
        mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, (uint8_t)prec);
        CHECK_FP(mc68k_fpu_move(pz, &st), 0x0000, 0);
        CHECK_FP(mc68k_fpu_move(nz, &st), 0x8000, 0);
    }
    CHECK(n == 3);
}

/* S/D オーバーフロー結果（[881UM §6.1.4]）: 精度×モード×符号 = 16通り */
static void test_move_overflow_table(void) {
    static const struct {
        uint8_t prec;
        uint16_t over_exp; /* 最大指数+1 */
        uint16_t max_exp;
        uint64_t max_sig;
    } precs[] = {
        {MC68K_FPU_PREC_SINGLE, 0x407F, 0x407E, UINT64_C(0xFFFFFF0000000000)},
        {MC68K_FPU_PREC_DOUBLE, 0x43FF, 0x43FE, UINT64_C(0xFFFFFFFFFFFFF800)},
    };
    /* [mode][sign] → 1: Inf, 0: 最大有限値 */
    static const int to_inf[4][2] = {{1, 1}, {0, 0}, {0, 1}, {1, 0}};
    int n = 0;

    for (size_t p = 0; p < 2; p++) {
        for (int mode = 0; mode < 4; mode++) {
            for (int sign = 0; sign < 2; sign++) {
                mc68k_fpu_state_t st = make_state((uint8_t)mode, precs[p].prec);
                mc68k_fp80_t v = {(uint16_t)((sign ? 0x8000 : 0) | precs[p].over_exp), SIG_J};
                mc68k_fp80_t r = mc68k_fpu_move(v, &st);

                if (to_inf[mode][sign]) {
                    CHECK(r.sign_exp == (uint16_t)((sign ? 0x8000 : 0) | 0x7FFF));
                    CHECK((r.signif & UINT64_C(0x7FFFFFFFFFFFFFFF)) == 0);
                } else {
                    CHECK_FP(r, (sign ? 0x8000 : 0) | precs[p].max_exp, precs[p].max_sig);
                }
                n++;
            }
        }
    }
    CHECK(n == 16);
}

/* 差し替えた roundPack の出力パック（[881UM Table 3-3]、design §3.3.1 の反例表） */
static void test_roundpack_named(void) {
    struct {
        int sign;
        long exp;
        uint64_t sig, extra;
        int prec, mode;
        unsigned sexp;
        uint64_t signif;
        unsigned flags;
    } cases[] = {
        /* Xの非正規化域。x87規約のままだと半分の値/誤ったUNFL/+0になる */
        {0, -1, SIG_J, 0, 80, 0, 0x0000, UINT64_C(0x4000000000000000), 2},
        {0, 0, SIG_J, 0, 80, 0, 0x0000, SIG_J, 0},
        {0, -63, SIG_J, 0, 80, 0, 0x0000, 1, 2},
        /* e=0 境界: 丸め上げで最小正規化数から e=1 へ繰り上がる */
        {0, 0, UINT64_MAX, UINT64_C(0x8000000000000001), 80, 0, 0x0001, SIG_J, 1},
        /* exp<=-64（シフト量64以上）でも未定義動作にならない */
        {0, -64, SIG_J, 0, 80, 0, 0x0000, 0, 3},
        {0, -64, UINT64_C(0xC000000000000000), 0, 80, 0, 0x0000, 1, 3},
        {0, -65, SIG_J, 0, 80, 3, 0x0000, 1, 3},
        {0, -100, SIG_J, 0, 80, 0, 0x0000, 0, 3},
        {1, -64, SIG_J, 0, 80, 0, 0x8000, 0, 3},
        {1, -65, SIG_J, 0, 80, 2, 0x8000, 1, 3},
        {1, -65, SIG_J, 0, 80, 3, 0x8000, 0, 3},
        /* S: 2^-150 + 2^-175 は最小非正規化値（keep==0。半分超のRNは§9の推論） */
        {0, 0x3FFF - 150, UINT64_C(0x8000004000000000), 0, 32, 0, 0x3F6A, SIG_J, 3},
        /* S/D のオーバーフロー: 最大有限値の指数・仮数は対象精度のもの */
        {0, 0x407F, SIG_J, 0, 32, 1, 0x407E, UINT64_C(0xFFFFFF0000000000), 5},
        {0, 0x43FF, SIG_J, 0, 64, 1, 0x43FE, UINT64_C(0xFFFFFFFFFFFFF800), 5},
        /* sig==0 の防御: 符号付きゼロ、フラグなし */
        {1, 5, 0, 0, 80, 0, 0x8000, 0, 0},
        {1, 5, 0, 0, 32, 0, 0x8000, 0, 0},
        {1, 5, 0, 0, 64, 2, 0x8000, 0, 0},
        /* 範囲内の丸め上げは、S/Dの範囲へ再マップせず桁上がりの+1だけ行う */
        {0, 0x3FFF, UINT64_C(0xFFFFFFFFFFFFFFFF), 0, 32, 0, 0x4000, SIG_J, 1},
        /* Opusレビュー B2: 丸め上げの桁上がりでmax_expちょうどからオーバーフローに
         * 達する経路（直接exp>max_expで入る経路とは別）でもフラグが正しく立つこと。
         * X: exp=X_MAX_EXP(0x7FFE)ちょうどからの桁上がり */
        {0, 0x7FFE, UINT64_MAX, SIG_J + 1, 80, 0, 0x7FFF, SIG_J, 5}, /* RN→+Inf */
        {0, 0x7FFE, UINT64_MAX, SIG_J + 1, 80, 1, 0x7FFE, UINT64_MAX,
         1},                                                         /* RZ→最大有限値のまま */
        {1, 0x7FFE, UINT64_MAX, SIG_J + 1, 80, 2, 0xFFFF, SIG_J, 5}, /* RM・負→-Inf */
        {0, 0x7FFE, UINT64_MAX, SIG_J + 1, 80, 3, 0x7FFF, SIG_J, 5}, /* RP・正→+Inf */
        /* S: exp=max_exp(0x407E)ちょうどからの桁上がり */
        {0, 0x407E, UINT64_MAX, 0, 32, 0, 0x7FFF, SIG_J, 5},
        {0, 0x407E, UINT64_MAX, 0, 32, 1, 0x407E, UINT64_C(0xFFFFFF0000000000), 1},
        {1, 0x407E, UINT64_MAX, 0, 32, 2, 0xFFFF, SIG_J, 5},
        {0, 0x407E, UINT64_MAX, 0, 32, 3, 0x7FFF, SIG_J, 5},
        /* D: exp=max_exp(0x43FE)ちょうどからの桁上がり */
        {0, 0x43FE, UINT64_MAX, 0, 64, 0, 0x7FFF, SIG_J, 5},
        {0, 0x43FE, UINT64_MAX, 0, 64, 1, 0x43FE, UINT64_C(0xFFFFFFFFFFFFF800), 1},
        {1, 0x43FE, UINT64_MAX, 0, 64, 2, 0xFFFF, SIG_J, 5},
        {0, 0x43FE, UINT64_MAX, 0, 64, 3, 0x7FFF, SIG_J, 5},
    };
    int n = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        extFloat80_t r;
        uint_fast8_t flags;

        softfloat_roundingMode = sf_mode[cases[i].mode];
        softfloat_exceptionFlags = 0;
        r = softfloat_roundPackToExtF80(cases[i].sign != 0, (int_fast32_t)cases[i].exp,
                                        cases[i].sig, cases[i].extra,
                                        (uint_fast8_t)cases[i].prec);
        flags = softfloat_exceptionFlags;
        softfloat_exceptionFlags = 0;
        if (r.signExp != (uint16_t)cases[i].sexp || r.signif != cases[i].signif ||
            flags != cases[i].flags) {
            g_failures++;
            fprintf(stderr,
                    "roundpack named #%zu: got %04x/%016" PRIx64
                    " flags %x, expected %04x/%016" PRIx64 " flags %x\n",
                    i, r.signExp, r.signif, (unsigned)flags, cases[i].sexp, cases[i].signif,
                    cases[i].flags);
        }
        n++;
    }
    CHECK(n == 30);
}

/* 状態の非干渉（design §3.4） */
static void test_state_isolation(void) {
    mc68k_fpu_state_t rp = make_state(MC68K_FPU_ROUND_PLUS_INF, 0);
    mc68k_fpu_state_t rm = make_state(MC68K_FPU_ROUND_MINUS_INF, 0);
    mc68k_fp80_t v = {0x3FFF, UINT64_C(0x8000004000000000)}; /* 1 + 2^-25。RNは下、RPは上 */
    int n = 0;

    for (int i = 0; i < 4; i++) {
        CHECK(mc68k_fpu_to_s(v, &rp) == 0x3F800001u);
        CHECK(mc68k_fpu_to_s(v, &rm) == 0x3F800000u);
        n++;
    }
    CHECK(n == 4);
    /* 各呼び出しは SoftFloat の例外フラグを持ち越さない（design §3.4 手順3） */
    softfloat_exceptionFlags = 0;
    mc68k_fpu_to_s(v, &rp);
    CHECK(softfloat_exceptionFlags == 0);
}

/* to_d は PREC=Single・SoftFloat の roundingPrecision グローバルが32でも倍精度で丸める */
static void test_store_ignores_precision(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    mc68k_fp80_t v = {0x3FFF,
                      UINT64_C(0x8000000000000C00)}; /* 1 + 2^-52*1.5 → 倍精度の丸め対象 */

    extF80_roundingPrecision = 32;
    CHECK(mc68k_fpu_to_d(v, &st) == UINT64_C(0x3FF0000000000002));
    CHECK(mc68k_fpu_to_s(v, &st) == 0x3F800000u);
    extF80_roundingPrecision = 80;
}

int main(void) {
    test_reset();
    test_int_roundtrip();
    test_int_rounding_modes();
    test_int_operr_values();
    test_nan_binary();
    test_from_x_exponent_zero();
    test_from_is_exact();
    test_move_prec();
    test_move_range();
    test_move_keep_zero();
    test_move_zero();
    test_move_overflow_table();
    test_roundpack_named();
    test_state_isolation();
    test_store_ignores_precision();

    run_vectors("roundpack.txt", 8000, vec_roundpack);
    run_vectors("from_x.txt", 100, vec_from_x);
    run_vectors("to_x.txt", 400, vec_to_x);
    run_vectors("to_s.txt", 2000, vec_to_s);
    run_vectors("to_d.txt", 3000, vec_to_d);
    run_vectors("from_s.txt", 100, vec_from_s);
    run_vectors("from_d.txt", 100, vec_from_d);
    run_vectors("move.txt", 5000, vec_move);
    run_vectors("to_int.txt", 4000, vec_to_int);
    run_vectors("from_int.txt", 40, vec_from_int);

    if (g_failures) {
        fprintf(stderr, "test_fpu_core: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_fpu_core: OK\n");
    return 0;
}
