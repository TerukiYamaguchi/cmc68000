/*
 * test_fpu_c.c — 1b Phase C（packed decimal の入出力。FMOVE.P）のテスト（Issue
 * #59、現行仕様）。
 *
 * 期待値: ベクタ（tests/fpu/vectors/c_*.txt）は gen_fpu_1b_vectors.py が fractions.Fraction
 * の厳密な有理数演算で求めた もの。p.4-69 の k-factor 表 7 行は [881UM] の印字値（12345.678765
 * を k 別に出力した文字列）をそのまま使う。 特殊値は [881UM] Table 3-4・p.4-67〜4-69
 * の記述から手で書く。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/fpu.h"

#define SIG_J     UINT64_C(0x8000000000000000)
#define QUIET     UINT64_C(0x4000000000000000)
#define CC_POISON 0x0Fu
#define EXC_INEX1 MC68K_FPU_EXC_INEX1
#define EXC_INEX2 MC68K_FPU_EXC_INEX2
#define EXC_OPERR MC68K_FPU_EXC_OPERR
#define EXC_SNAN  MC68K_FPU_EXC_SNAN
#define RN        MC68K_FPU_ROUND_NEAREST
#define PX        MC68K_FPU_PREC_EXTENDED

static int g_failures;
static long g_checks;
static long g_rows;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            g_failures++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static mc68k_fpu_state_t make_state(uint8_t mode, uint8_t prec) {
    mc68k_fpu_state_t st;

    mc68k_fpu_reset(&st);
    st.fpcr_round_mode = mode;
    st.fpcr_round_prec = prec;
    st.fpsr_cc = CC_POISON;
    return st;
}

static mc68k_fp80_t fp(unsigned sexp, uint64_t signif) {
    mc68k_fp80_t v;

    v.sign_exp = (uint16_t)sexp;
    v.signif = signif;
    return v;
}

static bool same_fp(mc68k_fp80_t v, unsigned sexp, uint64_t sig) {
    return v.sign_exp == sexp && v.signif == sig;
}

/* [881UM] §6.1.10 の式で EXC から期待する AEXC（AEXC=0 から始める）。packed で現れるのは
 * INEX1・INEX2・OPERR・SNAN だけ */
static uint8_t expected_aexc(unsigned exc) {
    uint8_t a = 0;

    if (exc & (EXC_OPERR | EXC_SNAN))
        a |= MC68K_FPU_AEXC_IOP;
    if (exc & (EXC_INEX1 | EXC_INEX2))
        a |= MC68K_FPU_AEXC_INEX;
    return a;
}

static bool same_words(const uint32_t a[3], uint32_t w0, uint32_t w1, uint32_t w2) {
    return a[0] == w0 && a[1] == w1 && a[2] == w2;
}

/* ---- ベクタ ------------------------------------------------------------------- */

typedef bool (*vector_fn)(const char *line);

static void run_vectors(const char *name, long expect_count, vector_fn fn) {
    char path[512], line[256];
    long declared = 0, executed = 0, checks_before = g_checks, shown = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", MC68K_FPU_VECTOR_DIR, name);
    f = fopen(path, "r");
    if (f == NULL || fgets(line, sizeof(line), f) == NULL ||
        sscanf(line, "count %ld", &declared) != 1) {
        fprintf(stderr, "cannot read vector file %s\n", path);
        exit(1);
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (!fn(line)) {
            g_failures++;
            if (shown++ < 5)
                fprintf(stderr, "%s: mismatch: %s", name, line);
        }
        executed++;
    }
    fclose(f);
    if (executed != declared || declared != expect_count ||
        g_checks - checks_before != executed) {
        g_failures++;
        fprintf(stderr, "%s: executed %ld, declared %ld, expected %ld, checks %ld\n", name,
                executed, declared, expect_count, g_checks - checks_before);
    }
}

static bool vec_from_p(const char *line) {
    int mode;
    uint32_t w[3];
    unsigned esexp, eexc;
    uint64_t esig;
    mc68k_fpu_state_t st;
    mc68k_fp80_t got;

    if (sscanf(line, "from_p %d %" SCNx32 " %" SCNx32 " %" SCNx32 " => %x %" SCNx64 " %x",
               &mode, &w[0], &w[1], &w[2], &esexp, &esig, &eexc) != 7)
        return false;
    st = make_state((uint8_t)mode, PX);
    got = mc68k_fpu_from_p(w, &st);
    g_checks++;
    /* INEX1 だけが立つ。PREC は無視される（S・D でも同じ）。条件コードは変更しない */
    if (!same_fp(got, esexp, esig) || st.fpsr_exc != eexc ||
        st.fpsr_aexc != expected_aexc(eexc) || st.fpsr_cc != CC_POISON)
        return false;
    for (uint8_t prec = MC68K_FPU_PREC_SINGLE; prec <= MC68K_FPU_PREC_DOUBLE; prec++) {
        mc68k_fpu_state_t sp = make_state((uint8_t)mode, prec);

        got = mc68k_fpu_from_p(w, &sp);
        if (!same_fp(got, esexp, esig) || sp.fpsr_exc != eexc)
            return false;
    }
    return true;
}

static bool vec_to_p(const char *line) {
    int mode, k;
    unsigned sexp, eexc;
    uint64_t sig;
    uint32_t e[3], got[3] = {0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u};
    mc68k_fpu_state_t st;

    if (sscanf(line, "to_p %d %d %x %" SCNx64 " => %" SCNx32 " %" SCNx32 " %" SCNx32 " %x",
               &mode, &k, &sexp, &sig, &e[0], &e[1], &e[2], &eexc) != 8)
        return false;
    st = make_state((uint8_t)mode, PX);
    mc68k_fpu_to_p(fp(sexp, sig), (int8_t)k, got, &st);
    g_checks++;
    if (!same_words(got, e[0], e[1], e[2]) || st.fpsr_exc != eexc ||
        st.fpsr_aexc != expected_aexc(eexc) || st.fpsr_cc != CC_POISON)
        return false;
    /* PREC は無視される */
    st = make_state((uint8_t)mode, MC68K_FPU_PREC_SINGLE);
    mc68k_fpu_to_p(fp(sexp, sig), (int8_t)k, got, &st);
    return same_words(got, e[0], e[1], e[2]) && st.fpsr_exc == eexc;
}

/* ---- p.4-69 の k-factor 表（12345.678765 を k 別に出力。印字値そのまま）
 * ---------------------------------- */

/* 仮数の数字列 digits（先頭が整数 1 桁）と指数 e10 から期待する 3 ワードを作る（正の数、指数 3
 * 桁以内） */
static void expect_words(const char *digits, int e10, uint32_t w[3]) {
    uint64_t frac = 0;
    unsigned mag = (unsigned)(e10 < 0 ? -e10 : e10);
    size_t len = strlen(digits);

    for (size_t i = 1; i < 17; i++)
        frac = frac << 4 | (i < len ? (unsigned)(digits[i] - '0') : 0u);
    w[0] = (e10 < 0 ? UINT32_C(1) << 30 : 0) | ((mag / 100) << 24) | (((mag / 10) % 10) << 20) |
           ((mag % 10) << 16) | (unsigned)(digits[0] - '0');
    w[1] = (uint32_t)(frac >> 32);
    w[2] = (uint32_t)frac;
}

static void test_manual_table(void) {
    static const struct {
        int k;
        const char *digits;
    } t[] = {
        {-5, "1234567877"}, {-3, "12345679"}, {-1, "123457"}, {0, "12346"},
        {1, "1"},           {3, "123"},       {5, "12346"},
    };
    const mc68k_fp80_t v =
        fp(0x400C, UINT64_C(0xC0E6B70E2C12AD82)); /* 12345.678765（拡張精度 RN） */

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        mc68k_fpu_state_t st = make_state(RN, PX);
        uint32_t got[3], want[3];

        g_checks++;
        mc68k_fpu_to_p(v, (int8_t)t[i].k, got, &st);
        expect_words(t[i].digits, 4, want);
        if (!same_words(got, want[0], want[1], want[2]) || st.fpsr_exc != EXC_INEX2) {
            g_failures++;
            fprintf(stderr, "manual k=%d: got %08x %08x %08x exc %02x\n", t[i].k, got[0],
                    got[1], got[2], st.fpsr_exc);
        }
    }
    CHECK(sizeof(t) / sizeof(t[0]) == 7);
    CHECK(g_checks == 7);
    g_checks = 0;
}

/* ---- 特殊値 -------------------------------------------------------------------- */

static void test_from_p_special(void) {
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;
    uint32_t w[3];

    /* Inf・NaN は SE=1・yy=11・指数 $FFF の型（Table 3-4）。整数桁・don't care は見ない。NaN の
     * bit63 は 0 のままでも写す */
    st = make_state(RN, PX);
    w[0] = UINT32_C(0x7FFF0FF7);
    w[1] = 0;
    w[2] = 0;
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == 0);
    w[0] = UINT32_C(0xFFFF0FF0);
    w[1] = UINT32_C(0x00000001);
    w[2] = 0;
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0xFFFF, UINT64_C(0x0000000100000000)) && st.fpsr_exc == 0);
    /* SE・yy が型と違う $FFF は Inf・NaN ではない（注 2）: 仮数 0 なら真のゼロ、非 0 なら
     * 10^1649 系の有限値 */
    static const uint32_t not_infnan[] = {UINT32_C(0x0FFF0000), UINT32_C(0x3FFF0000),
                                          UINT32_C(0x4FFF0000), UINT32_C(0x1FFF0000),
                                          UINT32_C(0x6FFF0000)};
    for (size_t i = 0; i < sizeof(not_infnan) / sizeof(not_infnan[0]); i++) {
        w[0] = not_infnan[i];
        w[1] = 0;
        w[2] = 0;
        st = make_state(RN, PX);
        v = mc68k_fpu_from_p(w, &st);
        g_rows++;
        CHECK(same_fp(v, 0x0000, 0) && st.fpsr_exc == 0);
        w[2] = 1;
        st = make_state(RN, PX);
        v = mc68k_fpu_from_p(w, &st);
        CHECK((v.sign_exp & 0x7FFF) != 0x7FFF && v.signif != 0);
    }
    CHECK(g_rows == 5);
    /* Inf: 指数 $FFF・仮数 0。符号は SM */
    st = make_state(RN, PX);
    w[0] = UINT32_C(0x7FFF0000);
    w[1] = 0;
    w[2] = 0;
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == 0);
    w[0] = UINT32_C(0xFFFF0000);
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0xFFFF, SIG_J) && st.fpsr_exc == 0);
    /* NaN: 仮数を bit 単位で写す（SNaN のまま。from_x と同じ） */
    w[0] = UINT32_C(0x7FFF0000);
    w[1] = UINT32_C(0xC0000000) | 5;
    w[2] = 7;
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0x7FFF, UINT64_C(0xC000000500000007)) && st.fpsr_exc == 0);
    w[0] = UINT32_C(0xFFFF0000);
    w[1] = UINT32_C(0xA0000000);
    w[2] = 1; /* SNaN（bit62 = 0）・負 */
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0xFFFF, UINT64_C(0xA000000000000001)) && st.fpsr_exc == 0);
    /* ゼロ: 仮数が 0 なら指数の桁が非 10 進でも真のゼロ（Table 3-4 注 2）。符号を保つ */
    w[0] = UINT32_C(0x0AAA0000);
    w[1] = 0;
    w[2] = 0;
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0x0000, 0) && st.fpsr_exc == 0);
    w[0] = UINT32_C(0xC5F90000) | 0x0FF0; /* 負・負の指数・don't care あり */
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0x8000, 0) && st.fpsr_exc == 0);
    /* 1.0E+0 は厳密で INEX1 なし、0.1 は端数があり INEX1 */
    w[0] = 1;
    w[1] = 0;
    w[2] = 0;
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0x3FFF, SIG_J) && st.fpsr_exc == 0);
    w[0] = UINT32_C(0x40010001); /* 1 × 10^-1 */
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0x3FFB, UINT64_C(0xCCCCCCCCCCCCCCCD)) && st.fpsr_exc == EXC_INEX1);
    /* 0.1 の RZ は下へ、RP は上へ */
    st = make_state(MC68K_FPU_ROUND_ZERO, PX);
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0x3FFB, UINT64_C(0xCCCCCCCCCCCCCCCC)));
    /* EXC は OR のみ */
    st = make_state(RN, PX);
    st.fpsr_exc = EXC_OPERR;
    v = mc68k_fpu_from_p(w, &st);
    CHECK(st.fpsr_exc == (EXC_OPERR | EXC_INEX1));
    /* 2^56×10^27 は拡張精度で厳密（packed 004300072057594037927936 ↔ 拡張精度 4090…。XEiJ
     * のコメントの値） */
    w[0] = UINT32_C(0x00430007);
    w[1] = UINT32_C(0x20575940);
    w[2] = UINT32_C(0x37927936);
    st = make_state(RN, PX);
    v = mc68k_fpu_from_p(w, &st);
    CHECK(same_fp(v, 0x4090, UINT64_C(0xCECB8F27F4200F3A)) && st.fpsr_exc == 0);
}

static void test_to_p_special(void) {
    mc68k_fpu_state_t st;
    uint32_t w[3];

    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x7FFF, SIG_J), 17, w, &st); /* +Inf */
    CHECK(same_words(w, UINT32_C(0x7FFF0000), 0, 0) && st.fpsr_exc == 0);
    mc68k_fpu_to_p(fp(0xFFFF, SIG_J), 3, w, &st); /* -Inf */
    CHECK(same_words(w, UINT32_C(0xFFFF0000), 0, 0) && st.fpsr_exc == 0);
    mc68k_fpu_to_p(fp(0x0000, 0), 5, w, &st); /* +0 */
    CHECK(same_words(w, 0, 0, 0) && st.fpsr_exc == 0);
    mc68k_fpu_to_p(fp(0x8000, 0), -3, w, &st); /* -0 */
    CHECK(same_words(w, UINT32_C(0x80000000), 0, 0) && st.fpsr_exc == 0);
    mc68k_fpu_to_p(fp(0x7FFF, SIG_J | QUIET | 0x12345), 17, w,
                   &st); /* qNaN: 仮数を bit 単位で */
    CHECK(same_words(w, UINT32_C(0x7FFF0000), UINT32_C(0xC0000000), UINT32_C(0x00012345)) &&
          st.fpsr_exc == 0);
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0xFFFF, SIG_J | 1), 17, w, &st); /* 負の SNaN は quiet 化して SNAN */
    CHECK(same_words(w, UINT32_C(0xFFFF0000), UINT32_C(0xC0000000), 1) &&
          st.fpsr_exc == EXC_SNAN);
    /* k > 17 は値によらず OPERR（[881UM] p.4-67）。Inf・NaN・ゼロでも立つ（出力は k
     * によらない） */
    memset(w, 0xA5, sizeof(w));
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x7FFF, SIG_J), 18, w, &st);
    CHECK(same_words(w, UINT32_C(0x7FFF0000), 0, 0) && st.fpsr_exc == EXC_OPERR);
    memset(w, 0xA5, sizeof(w));
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x8000, 0), 40, w, &st);
    CHECK(same_words(w, UINT32_C(0x80000000), 0, 0) && st.fpsr_exc == EXC_OPERR);
    memset(w, 0xA5, sizeof(w));
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x7FFF, SIG_J | QUIET | 3), 63, w, &st);
    CHECK(same_words(w, UINT32_C(0x7FFF0000), UINT32_C(0xC0000000), 3) &&
          st.fpsr_exc == EXC_OPERR);
    /* AEXC: OPERR は IOP、INEX2 は INEX */
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FFF, SIG_J), 18, w, &st);
    CHECK(st.fpsr_aexc == MC68K_FPU_AEXC_IOP);
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FFB, UINT64_C(0xCCCCCCCCCCCCCCCD)), 17, w, &st);
    CHECK(st.fpsr_exc == EXC_INEX2 && st.fpsr_aexc == MC68K_FPU_AEXC_INEX);
    /* 7bit の符号拡張: bit7 は無視（0x80 は k=0、0xC0 と 0x40 は k=-64）。1.125 で見る */
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FFF, UINT64_C(0x9000000000000000)), (int8_t)0x80, w,
                   &st); /* k=0: 有効 1 桁 → 1E+0（INEX2） */
    CHECK(same_words(w, 1, 0, 0) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FFF, UINT64_C(0x9000000000000000)), (int8_t)0xC0, w,
                   &st); /* k=-64: LEN は 17 に切り詰め */
    CHECK(same_words(w, 1, UINT32_C(0x12500000), 0) && st.fpsr_exc == 0);
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FFF, UINT64_C(0x9000000000000000)), (int8_t)0x40, w, &st);
    CHECK(same_words(w, 1, UINT32_C(0x12500000), 0) && st.fpsr_exc == 0);
    /* k > 17 は OPERR で k=17 として出力する（1.0 → 1.0000000000000000E+0 は厳密なので INEX2
     * なし） */
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FFF, SIG_J), 18, w, &st);
    CHECK(same_words(w, 1, 0, 0) && st.fpsr_exc == EXC_OPERR);
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FFF, SIG_J), 63, w, &st);
    CHECK(same_words(w, 1, 0, 0) && st.fpsr_exc == EXC_OPERR);
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FFF, SIG_J), 17, w, &st);
    CHECK(st.fpsr_exc == 0);
    /* 7bit の符号拡張: 0x40 は -64、0x7F は -1（k は int8_t の下位 7bit だけが意味を持つ） */
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FFF, SIG_J), (int8_t)0x7F, w, &st); /* k=-1 */
    CHECK(same_words(w, 1, 0, 0) && st.fpsr_exc == 0);
    /* 指数が 3 桁を超える: 10^1000 → EXP3=1、OPERR と INEX2（拡張精度の 10^1000 は近似値） */
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x4CF8, UINT64_C(0xF38DB1F9DD3DAC05)), 17, w, &st);
    CHECK(same_words(w, UINT32_C(0x00001001), 0, 0) && st.fpsr_exc == (EXC_OPERR | EXC_INEX2));
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3305, UINT64_C(0x868A9188A89E1467)), 17, w, &st); /* 10^-1000 */
    CHECK(same_words(w, UINT32_C(0x40001001), 0, 0) && st.fpsr_exc == (EXC_OPERR | EXC_INEX2));
    /* 0.0001 を k=0 で出力: LEN の下限 1 桁 → 1E-4 */
    st = make_state(RN, PX);
    mc68k_fpu_to_p(fp(0x3FF1, UINT64_C(0xD1B71758E219652C)), 0, w, &st);
    CHECK(same_words(w, UINT32_C(0x40040001), 0, 0) && st.fpsr_exc == EXC_INEX2);
    /* EXC は OR のみ */
    st = make_state(RN, PX);
    st.fpsr_exc = MC68K_FPU_EXC_DZ;
    mc68k_fpu_to_p(fp(0x3FFF, SIG_J), 18, w, &st);
    CHECK(st.fpsr_exc == (MC68K_FPU_EXC_DZ | EXC_OPERR));
}

/* 往復: 正規化した 17 桁の packed → 拡張精度（RN）→ packed（k=+17、RN）で元の桁が復元される
 * （拡張精度の 64bit は 10 進 19 桁弱を表せるので 17 桁は往復できる。設計書 §7.4） */
#define ROUND_TRIP_COUNT 3000

static uint32_t g_lcg = 20260930u;

static unsigned lcg_range(unsigned n) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return (g_lcg >> 8) % n;
}

static void test_round_trip(void) {
    long done = 0;

    for (int i = 0; i < ROUND_TRIP_COUNT; i++) {
        uint32_t in[3], out[3];
        uint64_t frac = 0;
        unsigned e = i < 40 ? (unsigned)(i < 20 ? 999 - (unsigned)i : (unsigned)(i - 20))
                            : lcg_range(1000);
        bool se = lcg_range(2) != 0, sm = lcg_range(2) != 0;

        if (e == 0)
            se = false; /* 出力の指数の符号は 0 のとき正 */
        mc68k_fpu_state_t st = make_state(RN, PX);
        mc68k_fp80_t v;

        for (int d = 0; d < 16; d++)
            frac = frac << 4 | lcg_range(10);
        in[0] = (sm ? UINT32_C(1) << 31 : 0) | (se ? UINT32_C(1) << 30 : 0) |
                ((e / 100) << 24) | (((e / 10) % 10) << 20) | ((e % 10) << 16) |
                (1 + lcg_range(9));
        in[1] = (uint32_t)(frac >> 32);
        in[2] = (uint32_t)frac;
        v = mc68k_fpu_from_p(in, &st);
        mc68k_fpu_to_p(v, 17, out, &st);
        done++;
        if (!same_words(out, in[0], in[1], in[2])) {
            g_failures++;
            if (g_failures < 10) {
                fprintf(stderr, "round trip: in %08x %08x %08x out %08x %08x %08x\n", in[0],
                        in[1], in[2], out[0], out[1], out[2]);
            }
        }
    }
    CHECK(done == 3000); /* リテラル（ROUND_TRIP_COUNT との自己参照にしない） */
}

int main(void) {
    test_manual_table();
    run_vectors("c_from_p.txt", 1456, vec_from_p);
    run_vectors("c_to_p.txt", 13020, vec_to_p);
    test_from_p_special();
    test_to_p_special();
    test_round_trip();
    if (g_failures != 0) {
        fprintf(stderr, "test_fpu_c: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_fpu_c: all passed\n");
    return 0;
}
