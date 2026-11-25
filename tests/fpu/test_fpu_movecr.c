/*
 * test_fpu_movecr.c — FMOVECR（定数ROM）のテスト（ Phase A、設計書 1b §4.4）。
 *
 * 期待値は tests/data/fpu/fpu_1b_rom_constants.csv（tests/fpu/gen_fpu_1b_vectors.py が
 * fractions.Fraction で独立に丸めた値。値の出典は [881UM]
 * ではなく数学的な真値）から読む。製品側の128bit表は CSV の exp/hi/lo_jam 列と照合する。FMOVECR
 * が使う roundPack の S_XR/D_XR 書式のうち、 FMOVECR
 * の値では通らない経路（tiny・オーバーフロー・exp=0 境界）は test_xr_formats() で直接検査する。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/fpu.h"
#include "fpu/fpu_internal.h"
#include "platform.h"
#include "internals.h"
#include "softfloat.h"

#define DEFINED_OFFSETS 22
#define MODE_COUNT      4
#define PREC_COUNT      3
#define CSV_COLUMNS     19 /* offset,name,value,exp,hi,lo_jam + 12 セル + note */
#define CSV_CELL_FIRST  6
#define SIG_J           UINT64_C(0x8000000000000000)
/* 実行前の FPCC の毒。FMOVECR が条件コードを必ず上書きすることを検査する */
#define CC_POISON       (MC68K_FPU_CC_N | MC68K_FPU_CC_Z | MC68K_FPU_CC_I | MC68K_FPU_CC_NAN)

static int g_failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            g_failures++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

/* CSV の列順は <モード RN,RZ,RM,RP> × <PREC X,S,D>。 */
static const uint8_t k_prec_of_column[PREC_COUNT] = {
    MC68K_FPU_PREC_EXTENDED, MC68K_FPU_PREC_SINGLE, MC68K_FPU_PREC_DOUBLE};

struct csv_row {
    unsigned offset;
    unsigned exp;
    uint64_t hi, lo;
    unsigned sexp[MODE_COUNT][PREC_COUNT];
    uint64_t signif[MODE_COUNT][PREC_COUNT];
    unsigned inex[MODE_COUNT][PREC_COUNT];
};

static bool parse_row(char *line, struct csv_row *row) {
    char *field[CSV_COLUMNS];
    int n = 0;
    char *p = line;

    for (;;) {
        field[n++] = p;
        p = strchr(p, ',');
        if (p == NULL || n == CSV_COLUMNS)
            break;
        *p++ = '\0';
    }
    if (n != CSV_COLUMNS)
        return false;
    row->offset = (unsigned)strtoul(field[0], NULL, 16);
    row->exp = (unsigned)strtoul(field[3], NULL, 16);
    row->hi = strtoull(field[4], NULL, 16);
    row->lo = strtoull(field[5], NULL, 16);
    for (int m = 0; m < MODE_COUNT; m++) {
        for (int c = 0; c < PREC_COUNT; c++) {
            const char *cell = field[CSV_CELL_FIRST + m * PREC_COUNT + c];
            if (sscanf(cell, "%x/%" SCNx64 "/%u", &row->sexp[m][c], &row->signif[m][c],
                       &row->inex[m][c]) != 3) {
                return false;
            }
        }
    }
    return true;
}

static mc68k_fpu_state_t make_state(uint8_t mode, uint8_t prec) {
    mc68k_fpu_state_t st;

    mc68k_fpu_reset(&st);
    st.fpcr_round_mode = mode;
    st.fpcr_round_prec = prec;
    st.fpsr_exc = 0;
    st.fpsr_aexc = 0;
    st.fpsr_cc = CC_POISON;
    return st;
}

static void check_row(const struct csv_row *row, int *cases) {
    const struct fpu_rom_entry *tbl = NULL;

    for (int i = 0; i < fpu_rom_table_count; i++) {
        if (fpu_rom_table[i].offset == row->offset)
            tbl = &fpu_rom_table[i];
    }
    CHECK(tbl != NULL);
    if (tbl != NULL && tbl->nonzero) {
        CHECK(tbl->exp == row->exp && tbl->hi == row->hi && tbl->lo == row->lo);
    }
    for (int m = 0; m < MODE_COUNT; m++) {
        for (int c = 0; c < PREC_COUNT; c++) {
            mc68k_fpu_state_t st = make_state((uint8_t)m, k_prec_of_column[c]);
            mc68k_fp80_t got = mc68k_fpu_movecr((uint8_t)row->offset, &st);
            uint8_t want_exc = row->inex[m][c] ? (uint8_t)MC68K_FPU_EXC_INEX2 : 0;
            /* AEXC は 1a design §6.5 の等式（INEX2 → AEXC.INEX）。[881UM] p.4-72 は 6.1.10
             * を参照 */
            uint8_t want_aexc = row->inex[m][c] ? (uint8_t)MC68K_FPU_AEXC_INEX : 0;
            uint8_t want_cc = row->signif[m][c] == 0 ? (uint8_t)MC68K_FPU_CC_Z : 0;

            if (got.sign_exp != row->sexp[m][c] || got.signif != row->signif[m][c]) {
                fprintf(stderr,
                        "offset %02x mode %d prec %d: got %04x/%016" PRIx64
                        ", expected %04x/%016" PRIx64 "\n",
                        row->offset, m, k_prec_of_column[c], got.sign_exp, got.signif,
                        row->sexp[m][c], row->signif[m][c]);
                g_failures++;
            }
            CHECK(st.fpsr_exc == want_exc);
            CHECK(st.fpsr_aexc == want_aexc);
            CHECK(st.fpsr_cc == want_cc);
            (*cases)++;
        }
    }
}

static void test_csv_cases(void) {
    char path[512];
    char line[1024];
    FILE *f;
    int rows = 0, cases = 0;
    bool seen_header = false;

    snprintf(path, sizeof(path), "%s", MC68K_FPU_ROM_CSV);
    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        g_failures++;
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        struct csv_row row;

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (!seen_header) {
            seen_header = true;
            continue;
        }
        if (!parse_row(line, &row)) {
            fprintf(stderr, "bad CSV row: %s\n", line);
            g_failures++;
            continue;
        }
        check_row(&row, &cases);
        rows++;
    }
    fclose(f);
    CHECK(rows == DEFINED_OFFSETS);
    CHECK(cases == DEFINED_OFFSETS * MODE_COUNT * PREC_COUNT); /* 264 */
    CHECK(fpu_rom_table_count == DEFINED_OFFSETS);
}

/* FLOAT3.S は `#$5c00`（FMOVECR #0,FP0）の直後に `$4423`（FMUL.S <ea>,FP0）または `$5423`
 * （FMUL.D <ea>,FP0）を送る（設計書 §4.4）。PREC=X/S/D の π を dst、S/D 形式の 1.0 を src
 * にした通しケース。 1.0 倍は厳密なので、結果は FMOVECR が返した π（CSV の rn_x/rn_s/rn_d
 * 列）のまま。 */
static void test_float3_fmul_through(void) {
    static const struct {
        uint8_t prec;
        uint64_t pi_signif;
    } cases[] = {
        {MC68K_FPU_PREC_EXTENDED, UINT64_C(0xC90FDAA22168C235)},
        {MC68K_FPU_PREC_SINGLE, UINT64_C(0xC90FDB0000000000)},
        {MC68K_FPU_PREC_DOUBLE, UINT64_C(0xC90FDAA22168C000)},
    };
    int n = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, cases[i].prec);
        mc68k_fp80_t pi = mc68k_fpu_movecr(0x00, &st);
        mc68k_fp80_t one_s = mc68k_fpu_from_s(UINT32_C(0x3F800000), &st);
        mc68k_fp80_t one_d = mc68k_fpu_from_d(UINT64_C(0x3FF0000000000000), &st);
        mc68k_fp80_t v;

        CHECK(pi.sign_exp == 0x4000 && pi.signif == cases[i].pi_signif);
        v = mc68k_fpu_mul(pi, one_s, &st); /* FMUL.S */
        CHECK(v.sign_exp == 0x4000 && v.signif == cases[i].pi_signif);
        v = mc68k_fpu_mul(pi, one_d, &st); /* FMUL.D */
        CHECK(v.sign_exp == 0x4000 && v.signif == cases[i].pi_signif);
        n++;
    }
    CHECK(n == 3);
}

/* FLOAT3.X が使う $00（π）と $32（1.0）。CSV とは別に、手で確認した値を直接書く。 */
static void test_float3_constants(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t v;

    v = mc68k_fpu_movecr(0x00, &st); /* π = C90FDAA22168C234 C4C6…: RN で ...C235 */
    CHECK(v.sign_exp == 0x4000 && v.signif == UINT64_C(0xC90FDAA22168C235));
    CHECK(st.fpsr_exc == MC68K_FPU_EXC_INEX2);

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_movecr(0x32, &st); /* 10^0 = 1.0 は厳密 */
    CHECK(v.sign_exp == 0x3FFF && v.signif == UINT64_C(0x8000000000000000));
    CHECK(st.fpsr_exc == 0 && st.fpsr_cc == 0);

    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    v = mc68k_fpu_movecr(0x00, &st); /* π の単精度仮数 C90FDB */
    CHECK(v.sign_exp == 0x4000 && v.signif == UINT64_C(0xC90FDB0000000000));
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_DOUBLE);
    v = mc68k_fpu_movecr(0x00, &st); /* π の倍精度仮数（53bit） */
    CHECK(v.sign_exp == 0x4000 && v.signif == UINT64_C(0xC90FDAA22168C000));

    test_float3_fmul_through();
}

/* PREC=S/D でも指数範囲は拡張精度のまま: 10^4096 が ±Inf・最大値にならない（設計書 §4.1-5）。
 */
static void test_extended_exponent_range(void) {
    static const uint8_t precs[] = {MC68K_FPU_PREC_SINGLE, MC68K_FPU_PREC_DOUBLE};
    int n = 0;

    for (size_t i = 0; i < sizeof(precs); i++) {
        mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, precs[i]);
        mc68k_fp80_t v = mc68k_fpu_movecr(0x3F, &st);

        CHECK(v.sign_exp == 0x7525); /* 10^4096 = 2^13606.x なので指数 16383+13606。有限 */
        CHECK((st.fpsr_exc & (MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_UNFL)) == 0);
        n++;
    }
    CHECK(n == 2);
}

static void test_undefined_offsets(void) {
    static const uint8_t offs[] = {0x01, 0x0A, 0x10, 0x2F, 0x42, 0x40, 0x7F, 0xFF};
    int n = 0;

    for (size_t i = 0; i < sizeof(offs); i++) {
        mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
        mc68k_fp80_t v = mc68k_fpu_movecr(offs[i], &st);

        CHECK(v.sign_exp == 0 && v.signif == 0);
        CHECK(st.fpsr_cc == MC68K_FPU_CC_Z && st.fpsr_exc == 0 && st.fpsr_aexc == 0);
        n++;
    }
    CHECK(n == 8);
}

/* roundPack の S_XR/D_XR 書式（設計書 §5.1-5「実装済みの規則」）を直接呼ぶ。FMOVECR の定数は
 * 指数が 0x3FFD〜0x7525 なので、tiny・オーバーフロー・exp=0 境界はここでしか通らない。
 * 期待値は §5.1-5 の規則を手でたどったもの:
 *   - exp>=0: 仮数だけを 24/53bit へ丸め、UNFL は立てない（exp=0 も tiny でない）。
 *   - exp<0: PREC=X と同じく拡張精度の量子で丸める（§13 の推論）。exact tiny も underflow
 * を記録する （s_roundPackToExtF80.c 冒頭の契約）。
 *   - 0x7FFE 超（丸め前、または繰り上がり後）: PREC=X と同じオーバーフロー結果。RZ
 * 等の最大有限値は 拡張精度の仮数（全ビット1）（[881UM] p.4-17 "an extended precision number
 * with an extended-precision mantissa value"）。0x7FFE
 * のまま切り捨てた値はオーバーフローではない。 flags は SoftFloat
 * のフラグ（inexact=1、underflow=2、overflow=4）。 */
static void test_xr_formats(void) {
    static const uint_fast8_t sf_mode[MODE_COUNT] = {softfloat_round_near_even,
                                                     softfloat_round_minMag,
                                                     softfloat_round_min, softfloat_round_max};
    static const struct {
        int sign;
        long exp;
        uint64_t sig;
        int prec, mode;
        unsigned sexp;
        uint64_t signif;
        unsigned flags;
    } cases[] = {
        /* S_XR: 丸め前のオーバーフロー */
        {0, 0x7FFF, SIG_J, FPU_ROUNDING_PRECISION_S_XR, 1, 0x7FFE, UINT64_MAX, 5},
        {0, 0x7FFF, SIG_J, FPU_ROUNDING_PRECISION_S_XR, 0, 0x7FFF, SIG_J, 5},
        /* S_XR: 0x7FFE ちょうどからの繰り上がり */
        {0, 0x7FFE, UINT64_MAX, FPU_ROUNDING_PRECISION_S_XR, 0, 0x7FFF, SIG_J, 5},
        {0, 0x7FFE, UINT64_MAX, FPU_ROUNDING_PRECISION_S_XR, 1, 0x7FFE,
         UINT64_C(0xFFFFFF0000000000), 1},
        {1, 0x7FFE, UINT64_MAX, FPU_ROUNDING_PRECISION_S_XR, 2, 0xFFFF, SIG_J, 5},
        {1, 0x7FFE, UINT64_MAX, FPU_ROUNDING_PRECISION_S_XR, 3, 0xFFFE,
         UINT64_C(0xFFFFFF0000000000), 1},
        /* S_XR: exp=0 と exp=1 は tiny でなく 24bit へ丸める。exp=-1 は拡張精度の量子 */
        {0, 0, SIG_J | 1, FPU_ROUNDING_PRECISION_S_XR, 0, 0x0000, SIG_J, 1},
        {0, 1, SIG_J | 1, FPU_ROUNDING_PRECISION_S_XR, 0, 0x0001, SIG_J, 1},
        {0, -1, SIG_J | 2, FPU_ROUNDING_PRECISION_S_XR, 0, 0x0000, UINT64_C(0x4000000000000001),
         2},
        /* D_XR: 同じ境界 */
        {0, 0x7FFF, SIG_J, FPU_ROUNDING_PRECISION_D_XR, 1, 0x7FFE, UINT64_MAX, 5},
        {0, 0x7FFE, UINT64_MAX, FPU_ROUNDING_PRECISION_D_XR, 0, 0x7FFF, SIG_J, 5},
        {0, 0x7FFE, UINT64_MAX, FPU_ROUNDING_PRECISION_D_XR, 1, 0x7FFE,
         UINT64_C(0xFFFFFFFFFFFFF800), 1},
        {0, 0, SIG_J | 1, FPU_ROUNDING_PRECISION_D_XR, 0, 0x0000, SIG_J, 1},
        {0, 1, SIG_J | 1, FPU_ROUNDING_PRECISION_D_XR, 0, 0x0001, SIG_J, 1},
        {0, -1, SIG_J | 2, FPU_ROUNDING_PRECISION_D_XR, 0, 0x0000, UINT64_C(0x4000000000000001),
         2},
    };
    int n = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        extFloat80_t r;
        uint_fast8_t flags;

        softfloat_roundingMode = sf_mode[cases[i].mode];
        softfloat_exceptionFlags = 0;
        r = softfloat_roundPackToExtF80(cases[i].sign != 0, (int_fast32_t)cases[i].exp,
                                        cases[i].sig, 0, (uint_fast8_t)cases[i].prec);
        flags = softfloat_exceptionFlags;
        softfloat_exceptionFlags = 0;
        if (r.signExp != (uint16_t)cases[i].sexp || r.signif != cases[i].signif ||
            flags != cases[i].flags) {
            g_failures++;
            fprintf(stderr,
                    "xr #%zu: got %04x/%016" PRIx64 " flags %x, expected %04x/%016" PRIx64
                    " flags %x\n",
                    i, r.signExp, r.signif, (unsigned)flags, cases[i].sexp, cases[i].signif,
                    cases[i].flags);
        }
        n++;
    }
    CHECK(n == 15);
}

int main(void) {
    test_csv_cases();
    test_float3_constants();
    test_extended_exponent_range();
    test_undefined_offsets();
    test_xr_formats();
    if (g_failures != 0) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("test_fpu_movecr: all passed\n");
    return 0;
}
