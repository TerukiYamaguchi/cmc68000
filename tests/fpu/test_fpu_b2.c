/*
 * test_fpu_b2.c — 1b Phase
 * B2（FREM・FSGLMUL・FSGLDIV・FGETEXP・FGETMAN・FSCALE・FSINCOS・FETOXM1・FLOGNP1・
 * FTWOTOX・FTENTOX）のテスト。
 *
 * 期待値は tests/fpu/gen_fpu_1b_vectors.py
 * が独立に求めたもの（tests/fpu/vectors/b2_*.txt。FREM・FSGLMUL・FSGLDIV・
 * FSCALE・FGETEXP・FGETMAN は fractions.Fraction の厳密演算、超越関数は mpmath）。特殊値は
 * [881UM] の各命令 ページの表（FREM p.4-86〜87、FSCALE p.4-93、FGETEXP p.4-46、FGETMAN
 * p.4-48、FSGLMUL p.4-100、FSGLDIV p.4-98、 FSINCOS p.4-104、FETOXM1 p.4-44、FLOGNP1
 * p.4-60、FTWOTOX p.4-123、FTENTOX p.4-117）から手で書く。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/fpu.h"
#include "fpu/fpu_wide.h"

#define SIG_J    UINT64_C(0x8000000000000000)
#define QUIET    UINT64_C(0x4000000000000000)
#define ALL_ONES UINT64_C(0xFFFFFFFFFFFFFFFF)

/* 呼び出し前の FPCC・商 byte の毒値（test_fpu_b1.c と同じ理由）。FREM 以外は商 byte を変えない
 */
#define CC_POISON       0x0Fu
#define QUOTIENT_POISON 0x55u
#define QSIGN_POISON    1u

#define EXC_INEX2 MC68K_FPU_EXC_INEX2
#define EXC_UNFL  MC68K_FPU_EXC_UNFL
#define EXC_OVFL  MC68K_FPU_EXC_OVFL
#define EXC_DZ    MC68K_FPU_EXC_DZ
#define EXC_OPERR MC68K_FPU_EXC_OPERR
#define EXC_SNAN  MC68K_FPU_EXC_SNAN

#define RN MC68K_FPU_ROUND_NEAREST
#define RZ MC68K_FPU_ROUND_ZERO
#define RM MC68K_FPU_ROUND_MINUS_INF
#define RP MC68K_FPU_ROUND_PLUS_INF
#define PX MC68K_FPU_PREC_EXTENDED
#define PS MC68K_FPU_PREC_SINGLE
#define PD MC68K_FPU_PREC_DOUBLE

#define EXPECT_ROWS                                                                            \
    406 /* 表の行数の合計（65+19+28+13+36）と特殊経路の OR 検査 13 件と NaN 検査（monadic 34 \
           回×4 行、dyadic 4 回×24 行）から手計算 */

static int g_failures;
static long
    g_rows; /* 特殊値表・NaN 検査の行を実際に実行した回数（表・ループの縮小の検出用。main
               で総数と照合） */
static long
    g_checks; /* ベクタの判定を実際に行った回数（runner の早期 return・ループの縮小の検出用） */

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
    st.fpsr_quotient = QUOTIENT_POISON;
    st.fpsr_quotient_sign = QSIGN_POISON;
    return st;
}

static bool quotient_untouched(const mc68k_fpu_state_t *st) {
    return st->fpsr_quotient == QUOTIENT_POISON && st->fpsr_quotient_sign == QSIGN_POISON;
}

/* [881UM] §6.1.10 の式で EXC から期待する AEXC（テストは AEXC=0 から始めるので EXC
 * だけで決まる） */
static uint8_t expected_aexc(unsigned exc) {
    uint8_t a = 0;

    if (exc & (MC68K_FPU_EXC_BSUN | MC68K_FPU_EXC_SNAN | MC68K_FPU_EXC_OPERR))
        a |= MC68K_FPU_AEXC_IOP;
    if (exc & MC68K_FPU_EXC_OVFL)
        a |= MC68K_FPU_AEXC_OVFL;
    if ((exc & MC68K_FPU_EXC_UNFL) && (exc & MC68K_FPU_EXC_INEX2))
        a |= MC68K_FPU_AEXC_UNFL;
    if (exc & MC68K_FPU_EXC_DZ)
        a |= MC68K_FPU_AEXC_DZ;
    if (exc & (MC68K_FPU_EXC_INEX1 | MC68K_FPU_EXC_INEX2 | MC68K_FPU_EXC_OVFL))
        a |= MC68K_FPU_AEXC_INEX;
    return a;
}

/* [881UM] Table 2-1（1a の fpu_set_cc() と同じ規則）で結果の値から期待する FPCC */
static uint8_t expected_cc(unsigned sexp, uint64_t sig) {
    uint8_t cc = (sexp & 0x8000) ? (uint8_t)MC68K_FPU_CC_N : 0;

    if ((sexp & 0x7FFF) == 0x7FFF)
        return (uint8_t)(cc | ((sig & ~SIG_J) != 0 ? MC68K_FPU_CC_NAN : MC68K_FPU_CC_I));
    if (sig == 0)
        cc |= (uint8_t)MC68K_FPU_CC_Z;
    return cc;
}

static mc68k_fp80_t fp(unsigned sexp, uint64_t signif) {
    mc68k_fp80_t v;

    v.sign_exp = (uint16_t)sexp;
    v.signif = signif;
    return v;
}

static bool is_default_nan(mc68k_fp80_t v) {
    return v.sign_exp == 0x7FFF && v.signif == ALL_ONES;
}

static bool same_fp(mc68k_fp80_t v, unsigned sexp, uint64_t sig) {
    return v.sign_exp == sexp && v.signif == sig;
}

typedef mc68k_fp80_t (*unary_fn)(mc68k_fp80_t, mc68k_fpu_state_t *);
typedef mc68k_fp80_t (*binary_fn)(mc68k_fp80_t, mc68k_fp80_t, mc68k_fpu_state_t *);

/* ---- ベクタ ------------------------------------------------------------------- */

static FILE *open_vectors(const char *name, long *count) {
    char path[512];
    char line[128];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", MC68K_FPU_VECTOR_DIR, name);
    f = fopen(path, "r");
    if (f == NULL || fgets(line, sizeof(line), f) == NULL ||
        sscanf(line, "count %ld", count) != 1) {
        fprintf(stderr, "cannot read vector file %s\n", path);
        exit(1);
    }
    return f;
}

typedef bool (*vector_fn)(const char *line);

/* expect_count:
 * 生成スクリプトの出力件数（生成側のループの縮小に気づけるよう完全一致で検査する）。
 * checks_per_line: 1 行あたりの判定回数。判定回数 g_checks も突き合わせ、runner の早期 return
 * や 内側のループの縮小を検出する。 */
static void run_vectors(const char *name, long expect_count, long checks_per_line,
                        vector_fn fn) {
    long declared, executed = 0, checks_before = g_checks, shown = 0;
    char line[256];
    FILE *f = open_vectors(name, &declared);

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
        g_checks - checks_before != executed * checks_per_line) {
        g_failures++;
        fprintf(stderr, "%s: executed %ld, declared %ld, expected %ld, checks %ld\n", name,
                executed, declared, expect_count, g_checks - checks_before);
    }
}

/* 値・EXC・FPCC（結果の値から Table 2-1 で決まる）がすべて期待どおりか */
static bool result_matches(mc68k_fp80_t got, unsigned esexp, uint64_t esig,
                           const mc68k_fpu_state_t *st, unsigned eexc) {
    g_checks++;
    return got.sign_exp == esexp && got.signif == esig && st->fpsr_exc == eexc &&
           st->fpsr_aexc == expected_aexc(eexc) && st->fpsr_cc == expected_cc(esexp, esig);
}

static bool vec_rem(const char *line) {
    int prec, mode;
    unsigned dsexp, ssexp, esexp, eexc, qsign, q;
    uint64_t dsig, ssig, esig;
    mc68k_fpu_state_t st;
    mc68k_fp80_t got;

    if (sscanf(line, "rem %d %d %x %" SCNx64 " %x %" SCNx64 " => %x %" SCNx64 " %x %u %u",
               &prec, &mode, &dsexp, &dsig, &ssexp, &ssig, &esexp, &esig, &eexc, &qsign,
               &q) != 11) {
        return false;
    }
    st = make_state((uint8_t)mode, (uint8_t)prec);
    got = mc68k_fpu_rem(fp(dsexp, dsig), fp(ssexp, ssig), &st);
    return result_matches(got, esexp, esig, &st, eexc) && st.fpsr_quotient_sign == qsign &&
           st.fpsr_quotient == q;
}

static bool vec_sgl(const char *line) {
    char op[8];
    int prec, mode;
    unsigned dsexp, ssexp, esexp, eexc;
    uint64_t dsig, ssig, esig;
    mc68k_fpu_state_t st;
    mc68k_fp80_t got;

    if (sscanf(line, "sgl %7s %d %d %x %" SCNx64 " %x %" SCNx64 " => %x %" SCNx64 " %x", op,
               &prec, &mode, &dsexp, &dsig, &ssexp, &ssig, &esexp, &esig, &eexc) != 10)
        return false;
    st = make_state((uint8_t)mode, (uint8_t)prec);
    got = strcmp(op, "mul") == 0 ? mc68k_fpu_sglmul(fp(dsexp, dsig), fp(ssexp, ssig), &st)
                                 : mc68k_fpu_sgldiv(fp(dsexp, dsig), fp(ssexp, ssig), &st);
    return result_matches(got, esexp, esig, &st, eexc) && quotient_untouched(&st);
}

static bool vec_scale(const char *line) {
    int prec, mode;
    unsigned dsexp, ssexp, esexp, eexc;
    uint64_t dsig, ssig, esig;
    mc68k_fpu_state_t st;
    mc68k_fp80_t got;

    if (sscanf(line, "scale %d %d %x %" SCNx64 " %x %" SCNx64 " => %x %" SCNx64 " %x", &prec,
               &mode, &dsexp, &dsig, &ssexp, &ssig, &esexp, &esig, &eexc) != 9)
        return false;
    st = make_state((uint8_t)mode, (uint8_t)prec);
    got = mc68k_fpu_scale(fp(dsexp, dsig), fp(ssexp, ssig), &st);
    return result_matches(got, esexp, esig, &st, eexc) && quotient_untouched(&st);
}

/* FGETEXP と FGETMAN。値は PREC・RM によらず厳密で、EXC は常に 0 */
static bool vec_getexp(const char *line) {
    int prec, mode;
    unsigned sexp, esexp, msexp;
    uint64_t sig, esig, msig;
    mc68k_fpu_state_t st;
    mc68k_fp80_t got;
    bool ok;

    if (sscanf(line, "getexp %d %d %x %" SCNx64 " => %x %" SCNx64 " %x %" SCNx64, &prec, &mode,
               &sexp, &sig, &esexp, &esig, &msexp, &msig) != 8)
        return false;
    st = make_state((uint8_t)mode, (uint8_t)prec);
    got = mc68k_fpu_getexp(fp(sexp, sig), &st);
    ok = result_matches(got, esexp, esig, &st, 0) && quotient_untouched(&st);
    st = make_state((uint8_t)mode, (uint8_t)prec);
    got = mc68k_fpu_getman(fp(sexp, sig), &st);
    return result_matches(got, msexp, msig, &st, 0) && quotient_untouched(&st) && ok;
}

/* ---- 超越関数（有限値。忠実丸め。設計書 §3.2・§5.6）
 * ------------------------------------------ */

#define TR_MODES 4 /* vec_tr が 1 行で試す丸めモード数（RN/RZ/RM/RP） */

static unary_fn g_tr_fn;

static bool vec_tr(const char *line) {
    int prec, directed, neg;
    unsigned sexp, lsexp, hsexp, eexc;
    uint64_t sig, lsig, hsig;
    bool ok = true;

    if (sscanf(line, "tr %d %x %" SCNx64 " => %x %" SCNx64 " %x %" SCNx64 " %d %d %x", &prec,
               &sexp, &sig, &lsexp, &lsig, &hsexp, &hsig, &directed, &neg, &eexc) != 10)
        return false;
    for (int mode = 0; mode < TR_MODES; mode++) {
        mc68k_fpu_state_t st = make_state((uint8_t)mode, (uint8_t)prec);
        mc68k_fp80_t got = g_tr_fn(fp(sexp, sig), &st);
        bool is_lo = same_fp(got, lsexp, lsig), is_hi = same_fp(got, hsexp, hsig);
        bool exact = lsexp == hsexp && lsig == hsig;

        g_checks++;
        if (st.fpsr_exc != eexc || st.fpsr_aexc != expected_aexc(eexc) ||
            !quotient_untouched(&st))
            ok = false;
        if (!is_lo && !is_hi)
            ok = false;
        else if (st.fpsr_cc != (is_lo ? expected_cc(lsexp, lsig) : expected_cc(hsexp, hsig)))
            ok = false;
        if (exact && !is_lo)
            ok = false;
        if (directed && !exact) {
            switch (mode) {
            case MC68K_FPU_ROUND_ZERO:
                if (!(neg ? is_hi : is_lo))
                    ok = false;
                break;
            case MC68K_FPU_ROUND_MINUS_INF:
                if (!is_lo)
                    ok = false;
                break;
            case MC68K_FPU_ROUND_PLUS_INF:
                if (!is_hi)
                    ok = false;
                break;
            default:
                break;
            }
        }
    }
    return ok;
}

static void run_tr(const char *name, unary_fn fn, long expect_count) {
    g_tr_fn = fn;
    /* 判定回数は TR_MODES ではなく丸めモードの列挙から数える（TR_MODES
     * 自体の縮小に気づけるように） */
    run_vectors(name, expect_count, MC68K_FPU_ROUND_PLUS_INF + 1, vec_tr);
}

/* ---- 中間精度（float128）の値を mpmath の真値と直接比べる（設計書 §5.4 の誤差予算）
 * ---------------- */

#define CORE_TOLERANCE_EXP                                              \
    110 /* 相対誤差 < 2^-110（設計書の予算 2^-104 より 6 bit 厳しい） \
         */

static int g_core_worst; /* 観測した最大の相対誤差 2^-g_core_worst（診断用） */

static wide_t core_eval(const char *name, bool sign, int32_t exp, uint64_t sig) {
    wide_t x = w_make(sign, exp, sig, 0), m;
    int32_t k;

    if (strcmp(name, "expm1") == 0) {
        bool near_one;

        m = wide_expm1(x, &k, &near_one);
        return near_one ? m : w_scale(m, k);
    }
    if (strcmp(name, "log1p") == 0)
        return wide_log1p(x);
    if (strcmp(name, "expm1s") == 0)
        return wide_expm1_small(x);
    if (strcmp(name, "log1ps") == 0)
        return wide_log1p_small(x);
    if (strcmp(name, "exp2") == 0)
        wide_exp2(x, &m, &k);
    else
        wide_exp10(x, &m, &k);
    return w_scale(m, k);
}

static bool vec_core(const char *line) {
    char name[16];
    unsigned sexp;
    uint64_t sig, hi, lo;
    wide_t want, got, err;
    int32_t err_exp;

    if (sscanf(line, "core %15s %x %" SCNx64 " => %" SCNx64 " %" SCNx64, name, &sexp, &sig, &hi,
               &lo) != 5) {
        return false;
    }
    want.v[1] = hi;
    want.v[0] = lo;
    g_checks++;
    w_begin();
    got = core_eval(name, (sexp & 0x8000) != 0, (int32_t)(sexp & 0x7FFF), sig);
    err = w_div(w_abs(w_sub(got, want)), w_abs(want));
    if (w_is_zero(err))
        return true;
    err_exp = WIDE_BIAS - (int32_t)w_exp(err); /* err ≈ 2^-err_exp */
    if (g_core_worst == 0 || err_exp < g_core_worst)
        g_core_worst = err_exp;
    return err_exp >= CORE_TOLERANCE_EXP;
}

/* ---- NaN 入力（1a design §5.2・[881UM] §4.5.4）
 * ------------------------------------------------ */

static bool monadic_nan_ok(unary_fn fn) {
    static const struct {
        unsigned sexp;
        uint64_t sig, out_sig;
        unsigned exc;
    } t[] = {
        {0x7FFF, SIG_J | QUIET | 5, SIG_J | QUIET | 5, 0},
        {0xFFFF, SIG_J | QUIET | 5, SIG_J | QUIET | 5, 0},
        {0x7FFF, SIG_J | 1, SIG_J | QUIET | 1, EXC_SNAN},
        {0xFFFF, SIG_J | 1, SIG_J | QUIET | 1, EXC_SNAN},
    };
    bool ok = true;

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        mc68k_fpu_state_t st = make_state(RN, PX);
        mc68k_fp80_t got = fn(fp(t[i].sexp, t[i].sig), &st);

        g_rows++;
        if (!same_fp(got, t[i].sexp, t[i].out_sig) || st.fpsr_exc != t[i].exc ||
            st.fpsr_aexc != expected_aexc(t[i].exc) ||
            st.fpsr_cc != expected_cc(t[i].sexp, t[i].out_sig) || !quotient_untouched(&st)) {
            ok = false;
        }
    }
    return ok;
}

/* 二項演算の NaN 1 件の検査（値・EXC・FPCC・商 byte） */
static bool dyadic_row_ok(binary_fn fn, bool clears_quotient, unsigned dsexp, uint64_t dsig,
                          unsigned ssexp, uint64_t ssig, unsigned osexp, uint64_t osig,
                          unsigned exc) {
    mc68k_fpu_state_t st = make_state(RN, PX);
    mc68k_fp80_t got = fn(fp(dsexp, dsig), fp(ssexp, ssig), &st);
    bool q_ok = clears_quotient ? (st.fpsr_quotient == 0 && st.fpsr_quotient_sign == 0)
                                : quotient_untouched(&st);

    g_rows++;
    return same_fp(got, osexp, osig) && st.fpsr_exc == exc &&
           st.fpsr_aexc == expected_aexc(exc) && st.fpsr_cc == expected_cc(osexp, osig) && q_ok;
}

/* 二項演算: dst 優先。qNaN はそのまま、SNaN は quiet 化して
 * SNAN。符号と仮数を保つ。相手が有限・±0・±Inf でも NaN
 * が優先される（OPERR・DZ・特殊値の規則より先に判定する）。 */
static bool dyadic_nan_ok(binary_fn fn, bool clears_quotient) {
    static const struct {
        unsigned sexp;
        uint64_t sig;
    } partner[] = {
        {0x3FFF, SIG_J}, {0x0000, 0}, {0x8000, 0}, {0x7FFF, SIG_J}, {0xFFFF, SIG_J},
    };
    static const struct {
        unsigned sexp;
        uint64_t sig, out_sig;
        unsigned exc;
    } nan[] = {
        {0x7FFF, SIG_J | QUIET | 5, SIG_J | QUIET | 5, 0},
        {0xFFFF, SIG_J | 1, SIG_J | QUIET | 1, EXC_SNAN},
    };
    bool ok = true;

    for (size_t i = 0; i < sizeof(partner) / sizeof(partner[0]); i++) {
        for (size_t j = 0; j < sizeof(nan) / sizeof(nan[0]); j++) {
            ok = dyadic_row_ok(fn, clears_quotient, nan[j].sexp, nan[j].sig, partner[i].sexp,
                               partner[i].sig, nan[j].sexp, nan[j].out_sig, nan[j].exc) &&
                 ok; /* dst=NaN */
            ok =
                dyadic_row_ok(fn, clears_quotient, partner[i].sexp, partner[i].sig, nan[j].sexp,
                              nan[j].sig, nan[j].sexp, nan[j].out_sig, nan[j].exc) &&
                ok; /* src=NaN */
        }
    }
    /* 両方 NaN は dst 優先。SNaN は相手の種類によらず SNAN を立てる */
    ok = dyadic_row_ok(fn, clears_quotient, 0x7FFF, SIG_J | QUIET | 7, 0xFFFF,
                       SIG_J | QUIET | 8, 0x7FFF, SIG_J | QUIET | 7, 0) &&
         ok;
    ok = dyadic_row_ok(fn, clears_quotient, 0xFFFF, SIG_J | QUIET | 9, 0x7FFF, SIG_J | 3,
                       0xFFFF, SIG_J | QUIET | 9, EXC_SNAN) &&
         ok;
    ok = dyadic_row_ok(fn, clears_quotient, 0x7FFF, SIG_J | 4, 0x7FFF, SIG_J | QUIET | 1,
                       0x7FFF, SIG_J | QUIET | 4, EXC_SNAN) &&
         ok;
    ok = dyadic_row_ok(fn, clears_quotient, 0x7FFF, SIG_J | 4, 0xFFFF, SIG_J | 2, 0x7FFF,
                       SIG_J | QUIET | 4, EXC_SNAN) &&
         ok;
    return ok;
}

/* ---- 二項演算の特殊値・EXC・条件コード
 * ------------------------------------------------------------ */

#define ONE_X     0x3FFF, SIG_J
#define TWO_X     0x4000, SIG_J
#define HALF_X    0x3FFE, SIG_J
#define INF_X(s)  ((s) | 0x7FFF), SIG_J
#define NAN_DEF   0x7FFF, ALL_ONES
#define ZERO_X(s) (s), 0

struct special2 {
    const char *name;
    binary_fn fn;
    unsigned dsexp;
    uint64_t dsig;
    unsigned ssexp;
    uint64_t ssig;
    unsigned out_sexp;
    uint64_t out_sig;
    unsigned exc;
};

static void run_special2(const struct special2 *t, size_t n, bool clears_quotient) {
    for (size_t i = 0; i < n; i++) {
        mc68k_fpu_state_t st = make_state(RN, PX);
        mc68k_fp80_t got = t[i].fn(fp(t[i].dsexp, t[i].dsig), fp(t[i].ssexp, t[i].ssig), &st);

        g_rows++;
        if (!same_fp(got, t[i].out_sexp, t[i].out_sig) || st.fpsr_exc != t[i].exc ||
            st.fpsr_aexc != expected_aexc(t[i].exc) ||
            st.fpsr_cc != expected_cc(t[i].out_sexp, t[i].out_sig) ||
            (!clears_quotient && !quotient_untouched(&st))) {
            g_failures++;
            fprintf(stderr, "special %s: got %04x/%016" PRIx64 " exc %02x cc %x\n", t[i].name,
                    got.sign_exp, got.signif, st.fpsr_exc, st.fpsr_cc);
        }
    }
}

static void test_sgl_special(void) {
    static const struct special2 mul[] = {
        {"sglmul(0*Inf)", mc68k_fpu_sglmul, ZERO_X(0), INF_X(0), NAN_DEF, EXC_OPERR},
        {"sglmul(-Inf*0)", mc68k_fpu_sglmul, INF_X(0x8000), ZERO_X(0), NAN_DEF, EXC_OPERR},
        {"sglmul(Inf*2)", mc68k_fpu_sglmul, INF_X(0), TWO_X, INF_X(0), 0},
        {"sglmul(-Inf*-1)", mc68k_fpu_sglmul, INF_X(0x8000), 0xBFFF, SIG_J, INF_X(0), 0},
        {"sglmul(2*-Inf)", mc68k_fpu_sglmul, TWO_X, INF_X(0x8000), INF_X(0x8000), 0},
        {"sglmul(0*-5)", mc68k_fpu_sglmul, ZERO_X(0), 0xC001, UINT64_C(0xA000000000000000),
         ZERO_X(0x8000), 0},
        {"sglmul(-0*-5)", mc68k_fpu_sglmul, ZERO_X(0x8000), 0xC001,
         UINT64_C(0xA000000000000000), ZERO_X(0), 0},
        /* 有限の非ゼロ × ±0（符号は XOR。OPERR にならない） */
        {"sglmul(2*+0)", mc68k_fpu_sglmul, TWO_X, ZERO_X(0), ZERO_X(0), 0},
        {"sglmul(-2*+0)", mc68k_fpu_sglmul, 0xC000, SIG_J, ZERO_X(0), ZERO_X(0x8000), 0},
        {"sglmul(2*-0)", mc68k_fpu_sglmul, TWO_X, ZERO_X(0x8000), ZERO_X(0x8000), 0},
        {"sglmul(+0*-0)", mc68k_fpu_sglmul, ZERO_X(0), ZERO_X(0x8000), ZERO_X(0x8000), 0},
        {"sglmul(2*3)", mc68k_fpu_sglmul, TWO_X, 0x4000, UINT64_C(0xC000000000000000), 0x4001,
         UINT64_C(0xC000000000000000), 0},
        /* 入力の仮数は有効数字 24bit へ切り詰める: 1 + 2^-30 は 1.0、1 + 2^-23 はそのまま */
        {"sglmul(1+2^-30)", mc68k_fpu_sglmul, 0x3FFF, SIG_J | (UINT64_C(1) << 33), ONE_X, ONE_X,
         0},
        {"sglmul(1+2^-23)", mc68k_fpu_sglmul, 0x3FFF, SIG_J | (UINT64_C(1) << 40), ONE_X,
         0x3FFF, SIG_J | (UINT64_C(1) << 40), 0},
        {"sglmul(1+2^-24)", mc68k_fpu_sglmul, 0x3FFF, SIG_J | (UINT64_C(1) << 39), ONE_X, ONE_X,
         0},
        /* 指数範囲は拡張精度のまま: 2^200 × 2^200 は単精度なら OVFL だがここでは正常値 */
        {"sglmul(2^200*2^200)", mc68k_fpu_sglmul, 0x40C7, SIG_J, 0x40C7, SIG_J, 0x418F, SIG_J,
         0},
        {"sglmul(2^-200*2^-200)", mc68k_fpu_sglmul, 0x3F37, SIG_J, 0x3F37, SIG_J, 0x3E6F, SIG_J,
         0},
    };
    static const struct special2 div[] = {
        {"sgldiv(0/0)", mc68k_fpu_sgldiv, ZERO_X(0), ZERO_X(0), NAN_DEF, EXC_OPERR},
        {"sgldiv(Inf/Inf)", mc68k_fpu_sgldiv, INF_X(0), INF_X(0x8000), NAN_DEF, EXC_OPERR},
        {"sgldiv(1/0)", mc68k_fpu_sgldiv, ONE_X, ZERO_X(0), INF_X(0), EXC_DZ},
        {"sgldiv(-1/0)", mc68k_fpu_sgldiv, 0xBFFF, SIG_J, ZERO_X(0), INF_X(0x8000), EXC_DZ},
        {"sgldiv(1/-0)", mc68k_fpu_sgldiv, ONE_X, ZERO_X(0x8000), INF_X(0x8000), EXC_DZ},
        {"sgldiv(Inf/0)", mc68k_fpu_sgldiv, INF_X(0), ZERO_X(0), INF_X(0),
         0}, /* ∞÷0 は DZ を立てない */
        {"sgldiv(-Inf/0)", mc68k_fpu_sgldiv, INF_X(0x8000), ZERO_X(0), INF_X(0x8000), 0},
        {"sgldiv(Inf/-2)", mc68k_fpu_sgldiv, INF_X(0), 0xC000, SIG_J, INF_X(0x8000), 0},
        {"sgldiv(0/-1)", mc68k_fpu_sgldiv, ZERO_X(0), 0xBFFF, SIG_J, ZERO_X(0x8000), 0},
        {"sgldiv(1/Inf)", mc68k_fpu_sgldiv, ONE_X, INF_X(0), ZERO_X(0), 0},
        {"sgldiv(-1/Inf)", mc68k_fpu_sgldiv, 0xBFFF, SIG_J, INF_X(0), ZERO_X(0x8000), 0},
        {"sgldiv(-1/-Inf)", mc68k_fpu_sgldiv, 0xBFFF, SIG_J, INF_X(0x8000), ZERO_X(0), 0},
        {"sgldiv(1/2)", mc68k_fpu_sgldiv, ONE_X, TWO_X, HALF_X, 0},
        {"sgldiv(1/3)", mc68k_fpu_sgldiv, ONE_X, 0x4000, UINT64_C(0xC000000000000000), 0x3FFD,
         UINT64_C(0xAAAAAB0000000000), EXC_INEX2},
    };
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    run_special2(mul, sizeof(mul) / sizeof(mul[0]), false);
    run_special2(div, sizeof(div) / sizeof(div[0]), false);
    CHECK(sizeof(mul) / sizeof(mul[0]) == 17); /* 表の行数（縮小に気づけるように） */
    CHECK(sizeof(div) / sizeof(div[0]) == 14);
    CHECK(dyadic_nan_ok(mc68k_fpu_sglmul, false));
    CHECK(dyadic_nan_ok(mc68k_fpu_sgldiv, false));
    /* オーバーフロー: RN は Inf、RZ は拡張精度の最大値（S_XR の最大仮数は全ビット 1。[881UM]
     * p.4-17）。 OVFL と INEX2 の両方 */
    st = make_state(RN, PX);
    v = mc68k_fpu_sglmul(fp(0x7FFE, SIG_J), fp(TWO_X), &st);
    CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
    st = make_state(RZ, PS);
    v = mc68k_fpu_sglmul(fp(0x7FFE, SIG_J), fp(TWO_X), &st);
    CHECK(same_fp(v, 0x7FFE, ALL_ONES) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
    st = make_state(RM, PD);
    v = mc68k_fpu_sglmul(fp(0xFFFE, SIG_J), fp(TWO_X), &st);
    CHECK(same_fp(v, 0xFFFF, SIG_J) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
    st = make_state(RP, PS);
    v = mc68k_fpu_sglmul(fp(0xFFFE, SIG_J), fp(TWO_X), &st);
    CHECK(same_fp(v, 0xFFFE, ALL_ONES) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
    /* アンダーフロー（拡張精度の最小指数未満）: UNFL と INEX2。値は拡張精度の量子で丸める */
    st = make_state(RN, PX);
    v = mc68k_fpu_sgldiv(fp(0x0000, SIG_J), fp(0x4000, SIG_J),
                         &st); /* 2^-16383 / 2 = 2^-16384（非正規化数で厳密） */
    CHECK(same_fp(v, 0x0000, SIG_J >> 1) && st.fpsr_exc == EXC_UNFL);
    /* FPCR の丸め精度を無視する: PREC=D でも仮数は 24bit で丸まる */
    st = make_state(RN, PD);
    v = mc68k_fpu_sgldiv(fp(ONE_X), fp(0x4000, UINT64_C(0xC000000000000000)), &st);
    CHECK(same_fp(v, 0x3FFD, UINT64_C(0xAAAAAB0000000000)) && st.fpsr_exc == EXC_INEX2);
}

static void test_scale_special(void) {
    static const struct special2 t[] = {
        {"scale(1,+Inf)", mc68k_fpu_scale, ONE_X, INF_X(0), NAN_DEF, EXC_OPERR},
        {"scale(1,-Inf)", mc68k_fpu_scale, ONE_X, INF_X(0x8000), NAN_DEF, EXC_OPERR},
        {"scale(Inf,Inf)", mc68k_fpu_scale, INF_X(0), INF_X(0), NAN_DEF, EXC_OPERR},
        {"scale(0,Inf)", mc68k_fpu_scale, ZERO_X(0), INF_X(0), NAN_DEF, EXC_OPERR},
        {"scale(+Inf,3)", mc68k_fpu_scale, INF_X(0), 0x4000, UINT64_C(0xC000000000000000),
         INF_X(0), 0},
        {"scale(-Inf,-3)", mc68k_fpu_scale, INF_X(0x8000), 0xC000, UINT64_C(0xC000000000000000),
         INF_X(0x8000), 0},
        {"scale(+0,5)", mc68k_fpu_scale, ZERO_X(0), 0x4001, UINT64_C(0xA000000000000000),
         ZERO_X(0), 0},
        {"scale(-0,5)", mc68k_fpu_scale, ZERO_X(0x8000), 0x4001, UINT64_C(0xA000000000000000),
         ZERO_X(0x8000), 0},
        {"scale(1,3)", mc68k_fpu_scale, ONE_X, 0x4000, UINT64_C(0xC000000000000000), 0x4002,
         SIG_J, 0},
        {"scale(1,-3)", mc68k_fpu_scale, ONE_X, 0xC000, UINT64_C(0xC000000000000000), 0x3FFC,
         SIG_J, 0},
        /* src は 0 方向に整数化する: 3.9 → 3、-3.9 → -3、0.9 → 0 */
        {"scale(1,3.9)", mc68k_fpu_scale, ONE_X, 0x4000, UINT64_C(0xF333333333333333), 0x4002,
         SIG_J, 0},
        {"scale(1,-3.9)", mc68k_fpu_scale, ONE_X, 0xC000, UINT64_C(0xF333333333333333), 0x3FFC,
         SIG_J, 0},
        {"scale(1,0.9)", mc68k_fpu_scale, ONE_X, 0x3FFE, UINT64_C(0xE666666666666666), ONE_X,
         0},
        {"scale(-1,0)", mc68k_fpu_scale, 0xBFFF, SIG_J, ZERO_X(0), 0xBFFF, SIG_J, 0},
        /* 大きな src: |src| >= 2^14 は OVFL/UNFL（設計書 §5.3 の |INT(src)| の飽和） */
        {"scale(1,2^14)", mc68k_fpu_scale, ONE_X, 0x400D, SIG_J, INF_X(0), EXC_OVFL},
        {"scale(-1,-2^14-1)", mc68k_fpu_scale, 0xBFFF, SIG_J, 0xC00D,
         UINT64_C(0x8002000000000000), ZERO_X(0x8000), EXC_UNFL},
        /* 2^14 未満は厳密に計算する: 2^16383（最大の冪）、2^-16383（最小の正規化数）は UNFL
           なし */
        {"scale(1,2^14-1)", mc68k_fpu_scale, ONE_X, 0x400C, UINT64_C(0xFFFC000000000000),
         0x7FFE, SIG_J, 0},
        {"scale(1,-2^14+1)", mc68k_fpu_scale, ONE_X, 0xC00C, UINT64_C(0xFFFC000000000000),
         0x0000, SIG_J, 0},
        /* |INT(src)| >= 2^14 は dst によらず OVFL/UNFL。2^-100 × 2^16384
           は数学的には有限だが強制される */
        {"scale(2^-100,2^14)", mc68k_fpu_scale, 0x3F9B, SIG_J, 0x400D, SIG_J, INF_X(0),
         EXC_OVFL},
        {"scale(2^100,-2^14)", mc68k_fpu_scale, 0x4063, SIG_J, 0xC00D, SIG_J, ZERO_X(0),
         EXC_UNFL},
        {"scale(2^-16446,2^14)", mc68k_fpu_scale, 0x0000, 1, 0x400D, SIG_J, INF_X(0), EXC_OVFL},
        /* 2^-100 に 2^20: 指数が飽和しても OVFL になる（2^14 で飽和させると誤って有限値になる）
         */
        {"scale(2^-100,2^20)", mc68k_fpu_scale, 0x3F9B, SIG_J, 0x4013, SIG_J, INF_X(0),
         EXC_OVFL},
        {"scale(2^100,-2^20)", mc68k_fpu_scale, 0x4063, SIG_J, 0xC013, SIG_J, ZERO_X(0),
         EXC_UNFL},
    };
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    run_special2(t, sizeof(t) / sizeof(t[0]), false);
    CHECK(sizeof(t) / sizeof(t[0]) == 23);
    CHECK(dyadic_nan_ok(mc68k_fpu_scale, false));
    /* INEX2: src=±0（dst の値を返す経路）は roundPack の結果を採用、それ以外は Cleared（p.4-93
     * Note 1・p.4-94）。 PREC=S で 1 + 2^-40 は丸めで値が変わる */
    st = make_state(RN, PS);
    v = mc68k_fpu_scale(fp(0x3FFF, SIG_J | (UINT64_C(1) << 23)), fp(ZERO_X(0)), &st);
    CHECK(same_fp(v, ONE_X) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RN, PS);
    v = mc68k_fpu_scale(fp(0x3FFF, SIG_J | (UINT64_C(1) << 23)), fp(ZERO_X(0x8000)), &st);
    CHECK(same_fp(v, ONE_X) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RN, PS);
    v = mc68k_fpu_scale(fp(0x3FFF, SIG_J | (UINT64_C(1) << 23)), fp(0x3FFE, SIG_J),
                        &st); /* src = 0.5 → N=0 */
    CHECK(same_fp(v, ONE_X) && st.fpsr_exc == 0);
    st = make_state(RN, PS);
    v = mc68k_fpu_scale(fp(0x3FFF, SIG_J | (UINT64_C(1) << 23)), fp(0x4000, SIG_J),
                        &st); /* src = 2 */
    CHECK(same_fp(v, 0x4001, SIG_J) && st.fpsr_exc == 0);
    /* src=0 経路の UNFL: 非正規化数の dst は PREC=X でも UNFL（Note 1） */
    st = make_state(RN, PX);
    v = mc68k_fpu_scale(fp(0x0000, 1), fp(ZERO_X(0)), &st);
    CHECK(same_fp(v, 0x0000, 1) && st.fpsr_exc == EXC_UNFL);
    /* 通常経路の INEX2 は 0 に強制。オーバーフロー（OVFL と INEX2）でも INEX2 は立たない */
    st = make_state(RZ, PX);
    v = mc68k_fpu_scale(fp(0x7FFE, SIG_J), fp(0x4000, SIG_J), &st);
    CHECK(same_fp(v, 0x7FFE, ALL_ONES) && st.fpsr_exc == EXC_OVFL);
    /* dst=非正規化数 2^-16446 × 2^100 = 2^-16346: 正規化数（UNFL なし） */
    st = make_state(RN, PX);
    v = mc68k_fpu_scale(fp(0x0000, 1), fp(0x4005, UINT64_C(0xC800000000000000)), &st);
    CHECK(same_fp(v, 0x0025, SIG_J) && st.fpsr_exc == 0);
}

static void test_getexp_special(void) {
    static const struct {
        const char *name;
        unary_fn fn;
        unsigned sexp;
        uint64_t sig;
        unsigned osexp;
        uint64_t osig;
        unsigned exc;
    } t[] = {
        {"getexp(+0)", mc68k_fpu_getexp, ZERO_X(0), ZERO_X(0), 0},
        {"getexp(-0)", mc68k_fpu_getexp, ZERO_X(0x8000), ZERO_X(0x8000), 0},
        {"getexp(+Inf)", mc68k_fpu_getexp, INF_X(0), NAN_DEF, EXC_OPERR},
        {"getexp(-Inf)", mc68k_fpu_getexp, INF_X(0x8000), NAN_DEF, EXC_OPERR},
        {"getexp(1)", mc68k_fpu_getexp, ONE_X, ZERO_X(0), 0},
        {"getexp(-1)", mc68k_fpu_getexp, 0xBFFF, SIG_J, ZERO_X(0), 0},
        {"getexp(4)", mc68k_fpu_getexp, 0x4001, SIG_J, 0x4000, SIG_J, 0},
        {"getexp(0.5)", mc68k_fpu_getexp, HALF_X, 0xBFFF, SIG_J, 0},
        {"getexp(max)", mc68k_fpu_getexp, 0x7FFE, ALL_ONES, 0x400C,
         UINT64_C(0xFFFC000000000000), 0}, /* 16383 */
        {"getexp(min normal)", mc68k_fpu_getexp, 0x0000, SIG_J, 0xC00C,
         UINT64_C(0xFFFC000000000000), 0}, /* -16383 */
        {"getexp(min denormal)", mc68k_fpu_getexp, 0x0000, 1, 0xC00D,
         UINT64_C(0x807C000000000000), 0}, /* -16446 */
        {"getman(+0)", mc68k_fpu_getman, ZERO_X(0), ZERO_X(0), 0},
        {"getman(-0)", mc68k_fpu_getman, ZERO_X(0x8000), ZERO_X(0x8000), 0},
        {"getman(+Inf)", mc68k_fpu_getman, INF_X(0), NAN_DEF, EXC_OPERR},
        {"getman(-Inf)", mc68k_fpu_getman, INF_X(0x8000), NAN_DEF, EXC_OPERR},
        {"getman(1)", mc68k_fpu_getman, ONE_X, ONE_X, 0},
        {"getman(-10)", mc68k_fpu_getman, 0xC002, UINT64_C(0xA000000000000000), 0xBFFF,
         UINT64_C(0xA000000000000000), 0},
        {"getman(min denormal)", mc68k_fpu_getman, 0x0000, 1, ONE_X, 0},
        {"getman(-denormal 3)", mc68k_fpu_getman, 0x8000, 3, 0xBFFF,
         UINT64_C(0xC000000000000000), 0},
    };

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        mc68k_fpu_state_t st = make_state(RN, PX);
        mc68k_fp80_t got = t[i].fn(fp(t[i].sexp, t[i].sig), &st);

        g_rows++;
        if (!same_fp(got, t[i].osexp, t[i].osig) || st.fpsr_exc != t[i].exc ||
            st.fpsr_aexc != expected_aexc(t[i].exc) ||
            st.fpsr_cc != expected_cc(t[i].osexp, t[i].osig) || !quotient_untouched(&st)) {
            g_failures++;
            fprintf(stderr, "special %s: got %04x/%016" PRIx64 " exc %02x cc %x\n", t[i].name,
                    got.sign_exp, got.signif, st.fpsr_exc, st.fpsr_cc);
        }
    }
    CHECK(sizeof(t) / sizeof(t[0]) == 19);
    CHECK(monadic_nan_ok(mc68k_fpu_getexp));
    CHECK(monadic_nan_ok(mc68k_fpu_getman));
}

static void test_rem_special(void) {
    static const struct special2 t[] = {
        {"rem(1,+0)", mc68k_fpu_rem, ONE_X, ZERO_X(0), NAN_DEF, EXC_OPERR},
        {"rem(1,-0)", mc68k_fpu_rem, ONE_X, ZERO_X(0x8000), NAN_DEF, EXC_OPERR},
        {"rem(+Inf,1)", mc68k_fpu_rem, INF_X(0), ONE_X, NAN_DEF, EXC_OPERR},
        {"rem(-Inf,1)", mc68k_fpu_rem, INF_X(0x8000), ONE_X, NAN_DEF, EXC_OPERR},
        {"rem(Inf,Inf)", mc68k_fpu_rem, INF_X(0), INF_X(0), NAN_DEF, EXC_OPERR},
        {"rem(0,0)", mc68k_fpu_rem, ZERO_X(0), ZERO_X(0), NAN_DEF, EXC_OPERR},
        {"rem(-1,Inf)", mc68k_fpu_rem, 0xBFFF, SIG_J, INF_X(0), 0xBFFF, SIG_J, 0},
        {"rem(+0,1)", mc68k_fpu_rem, ZERO_X(0), ONE_X, ZERO_X(0), 0},
        {"rem(-0,1)", mc68k_fpu_rem, ZERO_X(0x8000), ONE_X, ZERO_X(0x8000), 0},
        {"rem(+0,Inf)", mc68k_fpu_rem, ZERO_X(0), INF_X(0), ZERO_X(0), 0},
        {"rem(5,3)", mc68k_fpu_rem, 0x4001, UINT64_C(0xA000000000000000), 0x4000,
         UINT64_C(0xC000000000000000), 0xBFFF, SIG_J, 0}, /* N=2、剰余 -1 */
    };
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    /* 商 byte は命令の先頭で 0 にし、OPERR・NaN の経路では 0 のまま（FMOD と同じ暫定決定。U17）
     */
    run_special2(t, sizeof(t) / sizeof(t[0]), true);
    CHECK(sizeof(t) / sizeof(t[0]) == 11);
    CHECK(dyadic_nan_ok(mc68k_fpu_rem, true));
    st = make_state(RN, PX);
    v = mc68k_fpu_rem(fp(ONE_X), fp(ZERO_X(0x8000)), &st);
    CHECK(is_default_nan(v) && st.fpsr_quotient == 0 && st.fpsr_quotient_sign == 0);
    st = make_state(RN, PX);
    v = mc68k_fpu_rem(fp(0xBFFF, SIG_J), fp(INF_X(0)),
                      &st); /* dst=-1, src=Inf: 符号だけ商 byte */
    CHECK(same_fp(v, 0xBFFF, SIG_J) && st.fpsr_quotient_sign == 1 && st.fpsr_quotient == 0);
    st = make_state(RN, PX);
    v = mc68k_fpu_rem(fp(ZERO_X(0x8000)), fp(ONE_X), &st); /* dst=-0 */
    CHECK(same_fp(v, 0x8000, 0) && st.fpsr_quotient_sign == 1 && st.fpsr_quotient == 0);
    /* 最近接偶数: 2.5 ÷ 1 → N=2、剰余 +0.5。3.5 ÷ 1 → N=4、剰余 -0.5。-3.5 ÷ 1 → N=-4（商の符号
     * 1）、剰余 +0.5 */
    st = make_state(RN, PX);
    v = mc68k_fpu_rem(fp(0x4000, UINT64_C(0xA000000000000000)), fp(ONE_X), &st);
    CHECK(same_fp(v, 0x3FFE, SIG_J) && st.fpsr_quotient == 2 && st.fpsr_quotient_sign == 0 &&
          st.fpsr_exc == 0);
    v = mc68k_fpu_rem(fp(0x4000, UINT64_C(0xE000000000000000)), fp(ONE_X), &st);
    CHECK(same_fp(v, 0xBFFE, SIG_J) && st.fpsr_quotient == 4 && st.fpsr_quotient_sign == 0 &&
          st.fpsr_exc == 0);
    v = mc68k_fpu_rem(fp(0xC000, UINT64_C(0xE000000000000000)), fp(ONE_X), &st);
    CHECK(same_fp(v, 0x3FFE, SIG_J) && st.fpsr_quotient == 4 && st.fpsr_quotient_sign == 1);
    /* 商 byte は下位 7bit: 200 ÷ 1 → N=200、剰余 0（dst の符号のゼロ）、商 byte 72 */
    st = make_state(RN, PX);
    v = mc68k_fpu_rem(fp(0x4006, UINT64_C(0xC800000000000000)), fp(ONE_X), &st);
    CHECK(same_fp(v, 0x0000, 0) && st.fpsr_quotient == 72 && st.fpsr_quotient_sign == 0);
    v = mc68k_fpu_rem(fp(0xC006, UINT64_C(0xC800000000000000)), fp(ONE_X), &st);
    CHECK(same_fp(v, 0x8000, 0) && st.fpsr_quotient == 72 && st.fpsr_quotient_sign == 1);
    /* INEX2: FMOD は通常経路で roundPack の結果を採用するが、FREM は Cleared（p.4-87）。dst = 1
     * + 2^-40、src = 4、 PREC=S（N=0 で剰余は dst そのもの。単精度で表せないので丸めが起きる）
     */
    st = make_state(RN, PS);
    v = mc68k_fpu_rem(fp(0x3FFF, SIG_J | (UINT64_C(1) << 23)), fp(0x4001, SIG_J), &st);
    CHECK(same_fp(v, ONE_X) && st.fpsr_exc == 0);
    st = make_state(RN, PS);
    v = mc68k_fpu_mod(fp(0x3FFF, SIG_J | (UINT64_C(1) << 23)), fp(0x4001, SIG_J), &st);
    CHECK(same_fp(v, ONE_X) && st.fpsr_exc == EXC_INEX2);
    /* src=±Inf で dst の値を返す経路: INEX2 は roundPack の結果を採用、OVFL は 0 に強制（FMOD
     * は OVFL も採用） */
    st = make_state(RN, PS);
    v = mc68k_fpu_rem(fp(0x3FFF, SIG_J | (UINT64_C(1) << 23)), fp(INF_X(0)), &st);
    CHECK(same_fp(v, ONE_X) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RN, PS);
    v = mc68k_fpu_rem(fp(0x7FFE, ALL_ONES), fp(INF_X(0)), &st);
    CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RN, PS);
    v = mc68k_fpu_mod(fp(0x7FFE, ALL_ONES), fp(INF_X(0)), &st);
    CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == (EXC_INEX2 | EXC_OVFL));
    /* UNFL は Refer（通常経路もこの経路も roundPack の結果）: 非正規化数の dst */
    st = make_state(RN, PX);
    v = mc68k_fpu_rem(fp(0x0000, 1), fp(INF_X(0)), &st);
    CHECK(same_fp(v, 0x0000, 1) && st.fpsr_exc == EXC_UNFL);
    st = make_state(RN, PX);
    v = mc68k_fpu_rem(fp(0x0000, 5), fp(0x0000, 2),
                      &st); /* 5/2: N=2（偶数）、剰余 +1 = 非正規化数 */
    CHECK(same_fp(v, 0x0000, 1) && st.fpsr_exc == EXC_UNFL && st.fpsr_quotient == 2);
    /* 商の丸めが上がる境界（d=-1 と d=0）で剰余の符号が反転する: 0.75 ÷ 1 → N=1、剰余 -0.25 */
    st = make_state(RN, PX);
    v = mc68k_fpu_rem(fp(0x3FFE, UINT64_C(0xC000000000000000)), fp(ONE_X), &st);
    CHECK(same_fp(v, 0xBFFD, SIG_J) && st.fpsr_quotient == 1 && st.fpsr_quotient_sign == 0);
    v = mc68k_fpu_rem(fp(0x3FFE, SIG_J), fp(ONE_X),
                      &st); /* 0.5 ÷ 1: タイ、N=0（偶数）、剰余 +0.5 */
    CHECK(same_fp(v, 0x3FFE, SIG_J) && st.fpsr_quotient == 0);
}

/* ---- 超越関数の特殊値・EXC・条件コード
 * ------------------------------------------------------------ */

struct special {
    const char *name;
    unary_fn fn;
    unsigned in_sexp;
    uint64_t in_sig;
    unsigned out_sexp;
    uint64_t out_sig;
    unsigned exc;
};

static void run_special(const struct special *t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        mc68k_fpu_state_t st = make_state(RN, PX);
        mc68k_fp80_t got = t[i].fn(fp(t[i].in_sexp, t[i].in_sig), &st);

        g_rows++;
        if (!same_fp(got, t[i].out_sexp, t[i].out_sig) || st.fpsr_exc != t[i].exc ||
            st.fpsr_aexc != expected_aexc(t[i].exc) ||
            st.fpsr_cc != expected_cc(t[i].out_sexp, t[i].out_sig) ||
            !quotient_untouched(&st)) {
            g_failures++;
            fprintf(stderr, "special %s: got %04x/%016" PRIx64 " exc %02x cc %x\n", t[i].name,
                    got.sign_exp, got.signif, st.fpsr_exc, st.fpsr_cc);
        }
        if (!monadic_nan_ok(t[i].fn)) {
            g_failures++;
            fprintf(stderr, "special %s: NaN input\n", t[i].name);
        }
    }
}

static void test_transcend_specials(void) {
    static const struct special t[] = {
        {"etoxm1(+0)", mc68k_fpu_etoxm1, ZERO_X(0), ZERO_X(0), 0},
        {"etoxm1(-0)", mc68k_fpu_etoxm1, ZERO_X(0x8000), ZERO_X(0x8000), 0},
        {"etoxm1(+Inf)", mc68k_fpu_etoxm1, INF_X(0), INF_X(0), 0},
        {"etoxm1(-Inf)", mc68k_fpu_etoxm1, INF_X(0x8000), 0xBFFF, SIG_J, 0},
        {"lognp1(+0)", mc68k_fpu_lognp1, ZERO_X(0), ZERO_X(0), 0},
        {"lognp1(-0)", mc68k_fpu_lognp1, ZERO_X(0x8000), ZERO_X(0x8000), 0},
        {"lognp1(+Inf)", mc68k_fpu_lognp1, INF_X(0), INF_X(0), 0},
        {"lognp1(-Inf)", mc68k_fpu_lognp1, INF_X(0x8000), NAN_DEF, EXC_OPERR},
        {"lognp1(-1)", mc68k_fpu_lognp1, 0xBFFF, SIG_J, INF_X(0x8000), EXC_DZ},
        {"lognp1(-2)", mc68k_fpu_lognp1, 0xC000, SIG_J, NAN_DEF, EXC_OPERR},
        {"lognp1(-1-ulp)", mc68k_fpu_lognp1, 0xBFFF, SIG_J | 1, NAN_DEF, EXC_OPERR},
        {"lognp1(-2^100)", mc68k_fpu_lognp1, 0xC063, SIG_J, NAN_DEF, EXC_OPERR},
        {"lognp1(1)", mc68k_fpu_lognp1, ONE_X, 0x3FFE, UINT64_C(0xB17217F7D1CF79AC),
         EXC_INEX2}, /* ln 2 */
        {"twotox(+0)", mc68k_fpu_twotox, ZERO_X(0), ONE_X, 0},
        {"twotox(-0)", mc68k_fpu_twotox, ZERO_X(0x8000), ONE_X, 0},
        {"twotox(+Inf)", mc68k_fpu_twotox, INF_X(0), INF_X(0), 0},
        {"twotox(-Inf)", mc68k_fpu_twotox, INF_X(0x8000), ZERO_X(0), 0},
        {"twotox(3)", mc68k_fpu_twotox, 0x4000, UINT64_C(0xC000000000000000), 0x4002, SIG_J, 0},
        {"twotox(-1)", mc68k_fpu_twotox, 0xBFFF, SIG_J, HALF_X, 0},
        {"twotox(0.5)", mc68k_fpu_twotox, HALF_X, 0x3FFF, UINT64_C(0xB504F333F9DE6484),
         EXC_INEX2}, /* √2 */
        {"tentox(+0)", mc68k_fpu_tentox, ZERO_X(0), ONE_X, 0},
        {"tentox(-0)", mc68k_fpu_tentox, ZERO_X(0x8000), ONE_X, 0},
        {"tentox(+Inf)", mc68k_fpu_tentox, INF_X(0), INF_X(0), 0},
        {"tentox(-Inf)", mc68k_fpu_tentox, INF_X(0x8000), ZERO_X(0), 0},
        {"tentox(1)", mc68k_fpu_tentox, ONE_X, 0x4002, UINT64_C(0xA000000000000000),
         0}, /* 10 は厳密 */
        {"tentox(2)", mc68k_fpu_tentox, TWO_X, 0x4005, UINT64_C(0xC800000000000000),
         0}, /* 100 */
        {"tentox(27)", mc68k_fpu_tentox, 0x4003, UINT64_C(0xD800000000000000), 0x4058,
         UINT64_C(0xCECB8F27F4200F3A), 0},
        {"tentox(-1)", mc68k_fpu_tentox, 0xBFFF, SIG_J, 0x3FFB, UINT64_C(0xCCCCCCCCCCCCCCCD),
         EXC_INEX2}, /* 0.1 */
    };
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    run_special(t, sizeof(t) / sizeof(t[0]));
    CHECK(sizeof(t) / sizeof(t[0]) == 28);
    /* オーバーフロー・アンダーフロー（OVFL/UNFL は各節に従い、INEX2 も立つ） */
    st = make_state(RN, PX);
    v = mc68k_fpu_etoxm1(fp(0x400C, UINT64_C(0xB400000000000000)), &st); /* x = 11520 */
    CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
    st = make_state(RZ, PX);
    v = mc68k_fpu_twotox(fp(0x400D, UINT64_C(0x8000000000000000)), &st); /* 2^16384 */
    CHECK(same_fp(v, 0x7FFE, ALL_ONES) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
    st = make_state(RN, PX);
    v = mc68k_fpu_twotox(fp(0xC00D, UINT64_C(0x807F000000000000)),
                         &st); /* x = -16447.5: 最小の非正規化数の 1/2 未満 */
    CHECK(same_fp(v, 0, 0) && st.fpsr_exc == (EXC_INEX2 | EXC_UNFL));
    st = make_state(RP, PX);
    v = mc68k_fpu_twotox(fp(0xC00D, UINT64_C(0x807F000000000000)), &st);
    CHECK(same_fp(v, 0, 1) && st.fpsr_exc == (EXC_INEX2 | EXC_UNFL));
    st = make_state(RN, PX);
    v = mc68k_fpu_twotox(fp(0xC00E, UINT64_C(0xC000000000000000)),
                         &st); /* x = -49152: 2^x は +0 */
    CHECK(same_fp(v, 0, 0) && st.fpsr_exc == (EXC_INEX2 | EXC_UNFL));
    st = make_state(RN, PX);
    v = mc68k_fpu_twotox(fp(0xC010, SIG_J), &st); /* x = -65536（整数の飽和） */
    CHECK(same_fp(v, 0, 0) && st.fpsr_exc == (EXC_INEX2 | EXC_UNFL));
    st = make_state(RN, PX);
    v = mc68k_fpu_twotox(fp(0x4011, SIG_J), &st); /* x = 262144（整数の飽和） */
    CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
    st = make_state(RN, PX);
    v = mc68k_fpu_tentox(fp(0x400B, UINT64_C(0x9C40000000000000)), &st); /* x = 5000 */
    CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
    st = make_state(RN, PX);
    v = mc68k_fpu_tentox(fp(0xC00B, UINT64_C(0x9C40000000000000)), &st); /* x = -5000 */
    CHECK(same_fp(v, 0, 0) && st.fpsr_exc == (EXC_INEX2 | EXC_UNFL));
    st = make_state(RN, PX);
    v = mc68k_fpu_tentox(fp(0x400D, SIG_J), &st); /* x = 16384（飽和） */
    CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
    st = make_state(RN, PX);
    v = mc68k_fpu_tentox(fp(0xC00D, SIG_J), &st);
    CHECK(same_fp(v, 0, 0) && st.fpsr_exc == (EXC_INEX2 | EXC_UNFL));
    /* 大きな |x| の飽和（wide_exp2/wide_exp10/wide_exp の閾値と w_round_int の範囲外）。正は
     * OVFL|INEX2 の +Inf、 負は UNFL|INEX2 の +0（FETOXM1 は -1 の内側で INEX2
     * のみ）。2^15+0.5・2^33+0.5（w_round_int の範囲外）・2^40+0.5・2^62+0.5・2^63・2^16383 */
    {
        static const struct {
            unsigned sexp;
            uint64_t sig;
        } big[] = {
            /* 6 件 */
            {0x400E, SIG_J | (UINT64_C(1) << 47)},
            {0x4020, SIG_J | (UINT64_C(1) << 29)},
            {0x4027, SIG_J | (UINT64_C(1) << 22)},
            {0x403D, SIG_J | 1},
            {0x403E, SIG_J},
            {0x7FFE, SIG_J},
        };
        static const struct {
            const char *name;
            unary_fn fn;
        } fns[] = {
            {"twotox", mc68k_fpu_twotox},
            {"tentox", mc68k_fpu_tentox},
            {"etoxm1", mc68k_fpu_etoxm1},
        };

        for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); i++) {
            for (size_t f = 0; f < sizeof(fns) / sizeof(fns[0]); f++) {
                st = make_state(RN, PX);
                v = fns[f].fn(fp(big[i].sexp, big[i].sig), &st);
                g_rows++;
                CHECK(same_fp(v, 0x7FFF, SIG_J) && st.fpsr_exc == (EXC_OVFL | EXC_INEX2));
                st = make_state(RN, PX);
                v = fns[f].fn(fp(big[i].sexp | 0x8000, big[i].sig), &st);
                g_rows++;
                if (f == 2)
                    CHECK(same_fp(v, 0xBFFF, SIG_J) && st.fpsr_exc == EXC_INEX2);
                else
                    CHECK(same_fp(v, 0, 0) && st.fpsr_exc == (EXC_INEX2 | EXC_UNFL));
            }
        }
    }
    /* FETOXM1 は x → -Inf 側で -1 に近づく（値は -1 の直内側。INEX2 のみ。RN は -1、RZ は -(1 -
     * 2^-64)） */
    st = make_state(RN, PX);
    v = mc68k_fpu_etoxm1(fp(0xC00C, UINT64_C(0xB400000000000000)), &st); /* x = -11520 */
    CHECK(same_fp(v, 0xBFFF, SIG_J) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RZ, PX);
    v = mc68k_fpu_etoxm1(fp(0xC00C, UINT64_C(0xB400000000000000)), &st);
    CHECK(same_fp(v, 0xBFFE, ALL_ONES) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RM, PX);
    v = mc68k_fpu_etoxm1(fp(0xC010, SIG_J), &st); /* x = -65536（飽和） */
    CHECK(same_fp(v, 0xBFFF, SIG_J) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RP, PX);
    v = mc68k_fpu_etoxm1(fp(0xC010, SIG_J), &st);
    CHECK(same_fp(v, 0xBFFE, ALL_ONES) && st.fpsr_exc == EXC_INEX2);
    /* UNFL は各節に従う: 非正規化数の入力（FETOXM1 の結果は入力に近い非正規化数、FLOGNP1 も）
     */
    st = make_state(RN, PX);
    v = mc68k_fpu_etoxm1(fp(0x0000, 3), &st);
    CHECK(same_fp(v, 0x0000, 3) && st.fpsr_exc == (EXC_INEX2 | EXC_UNFL));
    st = make_state(RN, PX);
    v = mc68k_fpu_lognp1(fp(0x0000, 3), &st);
    CHECK(same_fp(v, 0x0000, 3) && st.fpsr_exc == (EXC_INEX2 | EXC_UNFL));
    /* 補正項の向き: FETOXM1(x) = x + x²/2 は x より 0 から遠い側（x>0 の RP は x
     * の次の値）か近い側 （x<0 は真値が x の 0 側にあり、RM は x、RP は 0 側の隣）。FLOGNP1(x)
     * = x - x²/2 は逆 */
    st = make_state(RP, PX);
    v = mc68k_fpu_etoxm1(fp(0x3F00, SIG_J), &st);
    CHECK(same_fp(v, 0x3F00, SIG_J | 1));
    st = make_state(RM, PX);
    v = mc68k_fpu_etoxm1(fp(0xBF00, SIG_J), &st);
    CHECK(same_fp(v, 0xBF00, SIG_J));
    st = make_state(RP, PX);
    v = mc68k_fpu_etoxm1(fp(0xBF00, SIG_J), &st);
    CHECK(same_fp(v, 0xBEFF, ALL_ONES));
    st = make_state(RZ, PX);
    v = mc68k_fpu_lognp1(fp(0x3F00, SIG_J), &st);
    CHECK(same_fp(v, 0x3EFF, ALL_ONES));
    st = make_state(RM, PX);
    v = mc68k_fpu_lognp1(fp(0xBF00, SIG_J), &st);
    CHECK(same_fp(v, 0xBF00, SIG_J | 1));
    st = make_state(RP, PX);
    v = mc68k_fpu_twotox(fp(0x3F00, SIG_J), &st); /* 1 + x·ln2 > 1 */
    CHECK(same_fp(v, 0x3FFF, SIG_J | 1));
    st = make_state(RM, PX);
    v = mc68k_fpu_tentox(fp(0xBF00, SIG_J), &st); /* 1 - |x|·ln10 < 1 */
    CHECK(same_fp(v, 0x3FFE, ALL_ONES));
    /* 厳密な結果の PREC=S: 10^11 は拡張精度で厳密だが単精度では丸めが起きる */
    st = make_state(RN, PS);
    v = mc68k_fpu_tentox(fp(0x4002, UINT64_C(0xB000000000000000)), &st);
    CHECK(same_fp(v, 0x4023, UINT64_C(0xBA43B70000000000)) && st.fpsr_exc == EXC_INEX2);
    CHECK(monadic_nan_ok(mc68k_fpu_etoxm1));
    CHECK(monadic_nan_ok(mc68k_fpu_lognp1));
    CHECK(monadic_nan_ok(mc68k_fpu_twotox));
    CHECK(monadic_nan_ok(mc68k_fpu_tentox));
}

/* FSINCOS（[881UM] p.4-104）: 戻り値が sin、*out_cos が cos。条件コードは sin の結果、EXC は
 * sin と cos の OR */
/* 期待値は sin・cos 単体（B1 のベクタと b2_tr_* で検証済み）の呼び出し結果との合成検査。FSINCOS
 * 自体の規則（EXC は OR、 条件コードは sin）の検査で、値の正しさは単体側に依る。 */
static void test_sincos(void) {
    static const struct {
        unsigned sexp;
        uint64_t sig;
        uint8_t mode, prec;
    } in[] = {
        {0x0000, 0, RN, PX},
        {0x8000, 0, RN, PX},
        {0x3FFF, SIG_J, RN, PX},
        {0xBFFF, SIG_J, RZ, PS},
        {0x4003, UINT64_C(0xC800000000000000), RM, PD},
        {0x0000, 3, RN, PX},
        {0x8000, 3, RP, PX},
        {0x3F00, SIG_J | 5, RN, PX},
        {0x7FFF, SIG_J, RN, PX},
        {0xFFFF, SIG_J, RN, PX},
        {0x7FFF, SIG_J | QUIET | 7, RN, PX},
        {0x7FFF, SIG_J | 1, RN, PX},
        {0x4020, SIG_J | 12345, RN, PX},
    };
    mc68k_fpu_state_t st;
    mc68k_fp80_t s, c;

    for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        mc68k_fpu_state_t ss = make_state(in[i].mode, in[i].prec);
        mc68k_fpu_state_t sc = make_state(in[i].mode, in[i].prec);
        mc68k_fp80_t want_s = mc68k_fpu_sin(fp(in[i].sexp, in[i].sig), &ss);
        mc68k_fp80_t want_c = mc68k_fpu_cos(fp(in[i].sexp, in[i].sig), &sc);

        st = make_state(in[i].mode, in[i].prec);
        s = mc68k_fpu_sincos(fp(in[i].sexp, in[i].sig), &c, &st);
        g_rows++;
        if (!same_fp(s, want_s.sign_exp, want_s.signif) ||
            !same_fp(c, want_c.sign_exp, want_c.signif) ||
            st.fpsr_exc != (ss.fpsr_exc | sc.fpsr_exc) || st.fpsr_cc != ss.fpsr_cc ||
            !quotient_untouched(&st)) {
            g_failures++;
            fprintf(stderr,
                    "sincos case %zu: sin %04x/%016" PRIx64 " cos %04x/%016" PRIx64
                    " exc %02x cc %x\n",
                    i, s.sign_exp, s.signif, c.sign_exp, c.signif, st.fpsr_exc, st.fpsr_cc);
        }
    }
    CHECK(sizeof(in) / sizeof(in[0]) == 13);
    /* 具体値: sincos(0) は sin=+0（cc=Z）、cos=1。sincos(-0) は sin=-0（cc=Z|N）。cc は cos
     * ではなく sin の結果から */
    st = make_state(RN, PX);
    s = mc68k_fpu_sincos(fp(ZERO_X(0)), &c, &st);
    CHECK(same_fp(s, 0, 0) && same_fp(c, ONE_X) && st.fpsr_cc == MC68K_FPU_CC_Z &&
          st.fpsr_exc == 0);
    st = make_state(RN, PX);
    s = mc68k_fpu_sincos(fp(ZERO_X(0x8000)), &c, &st);
    CHECK(same_fp(s, 0x8000, 0) && same_fp(c, ONE_X) &&
          st.fpsr_cc == (MC68K_FPU_CC_Z | MC68K_FPU_CC_N));
    /* ±Inf は両方 NaN（OPERR）。条件コードは NaN */
    st = make_state(RN, PX);
    s = mc68k_fpu_sincos(fp(INF_X(0x8000)), &c, &st);
    CHECK(is_default_nan(s) && is_default_nan(c) && st.fpsr_exc == EXC_OPERR &&
          st.fpsr_cc == MC68K_FPU_CC_NAN);
    /* 非正規化数: sin は UNFL・INEX2、cos は INEX2 だけ（UNFL は sin のみ。EXC は OR） */
    st = make_state(RN, PX);
    s = mc68k_fpu_sincos(fp(0x0000, 3), &c, &st);
    CHECK(same_fp(s, 0x0000, 3) && same_fp(c, ONE_X) && st.fpsr_exc == (EXC_UNFL | EXC_INEX2));
    /* cos の INEX2 は 1 付近の丸めでも立つ: sin(2^-60) の cos は 1 - 2^-121 → RN
     * は 1.0（INEX2） */
    st = make_state(RN, PX);
    s = mc68k_fpu_sincos(fp(0x3FC3, SIG_J), &c, &st);
    CHECK(same_fp(c, ONE_X) && (st.fpsr_exc & EXC_INEX2));
}

/* 真値の大きさが 1 未満と分かる関数（sin・cos・tanh・x<0 の e^x-1）が、1 の直下の近格子帯でも
 * RZ/RM/RP の向きを守る （snap_to_grid() のように ±1 を返さない。XEiJ
 * との差分照合で見つかった。設計書 §9.2） */
static void test_below_one_direction(void) {
    const uint64_t ONES = UINT64_C(0xFFFFFFFFFFFFFFFF);
    const uint64_t S24 = UINT64_C(0xFFFFFF0000000000);
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;

    /* tanh(40): 1 - 3.6e-35（PREC=X の近格子帯 2^-104 の内側）。RN・RP は 1.0、RZ・RM は 1 -
     * 2^-64 */
    st = make_state(RN, PX);
    v = mc68k_fpu_tanh(fp(0x4004, UINT64_C(0xA000000000000000)), &st);
    CHECK(same_fp(v, ONE_X) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RZ, PX);
    v = mc68k_fpu_tanh(fp(0x4004, UINT64_C(0xA000000000000000)), &st);
    CHECK(same_fp(v, 0x3FFE, ONES) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RM, PX);
    v = mc68k_fpu_tanh(fp(0x4004, UINT64_C(0xA000000000000000)), &st);
    CHECK(same_fp(v, 0x3FFE, ONES));
    st = make_state(RP, PX);
    v = mc68k_fpu_tanh(fp(0x4004, UINT64_C(0xA000000000000000)), &st);
    CHECK(same_fp(v, ONE_X));
    st = make_state(RM, PX);
    v = mc68k_fpu_tanh(fp(0xC004, UINT64_C(0xA000000000000000)), &st); /* tanh(-40) */
    CHECK(same_fp(v, 0xBFFF, SIG_J));
    st = make_state(RP, PX);
    v = mc68k_fpu_tanh(fp(0xC004, UINT64_C(0xA000000000000000)), &st);
    CHECK(same_fp(v, 0xBFFE, ONES));
    /* PREC=S の近格子帯は 2^-64: tanh(25) = 1 - 3.9e-22 */
    st = make_state(RZ, PS);
    v = mc68k_fpu_tanh(fp(0x4003, UINT64_C(0xC800000000000000)), &st);
    CHECK(same_fp(v, 0x3FFE, S24));
    st = make_state(RN, PS);
    v = mc68k_fpu_tanh(fp(0x4003, UINT64_C(0xC800000000000000)), &st);
    CHECK(same_fp(v, ONE_X));
    /* e^-50 - 1（PREC=S）: -1 の直内側。RZ・RP は -(1-2^-24)、RM・RN は -1.0 */
    st = make_state(RZ, PS);
    v = mc68k_fpu_etoxm1(fp(0xC004, UINT64_C(0xC800000000000000)), &st);
    CHECK(same_fp(v, 0xBFFE, S24) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RP, PS);
    v = mc68k_fpu_etoxm1(fp(0xC004, UINT64_C(0xC800000000000000)), &st);
    CHECK(same_fp(v, 0xBFFE, S24));
    st = make_state(RM, PS);
    v = mc68k_fpu_etoxm1(fp(0xC004, UINT64_C(0xC800000000000000)), &st);
    CHECK(same_fp(v, 0xBFFF, SIG_J));
    /* sin(π/2 に最も近い拡張精度値)・cos(同) = 1 - 約 2^-132（sin）/ 約 0（cos
     * の近傍は別）。sin は RZ で 1 - 2^-64 */
    st = make_state(RZ, PX);
    v = mc68k_fpu_sin(fp(0x3FFF, UINT64_C(0xC90FDAA22168C235)), &st);
    CHECK(same_fp(v, 0x3FFE, ONES) && st.fpsr_exc == EXC_INEX2);
    st = make_state(RN, PX);
    v = mc68k_fpu_sin(fp(0x3FFF, UINT64_C(0xC90FDAA22168C235)), &st);
    CHECK(same_fp(v, ONE_X));
    st = make_state(RP, PX);
    v = mc68k_fpu_sin(fp(0x3FFF, UINT64_C(0xC90FDAA22168C235)), &st);
    CHECK(same_fp(v, ONE_X));
    st = make_state(RM, PX);
    v = mc68k_fpu_sin(fp(0xBFFF, UINT64_C(0xC90FDAA22168C235)), &st); /* sin(-π/2 付近) */
    CHECK(same_fp(v, 0xBFFF, SIG_J));
    st = make_state(RP, PX);
    v = mc68k_fpu_sin(fp(0xBFFF, UINT64_C(0xC90FDAA22168C235)), &st);
    CHECK(same_fp(v, 0xBFFE, ONES));
}

/* 呼び出し前の EXC は保たれる（OR のみ。設計書 §6） */
/* 特殊値・ゼロ剰余などの早期 return の経路でも、呼び出し前の EXC（ここでは
 * DZ）を代入で消さない（OR のみ） */
static void test_exc_or_only_special(void) {
    static const struct {
        const char *name;
        int kind;
        unsigned a_sexp, b_sexp;
        uint64_t a_sig, b_sig;
    } t[] = {
        {"sglmul(2*0)", 0, 0x4000, 0x0000, SIG_J, 0},
        {"sgldiv(0/1)", 1, 0x0000, 0x3FFF, 0, SIG_J},
        {"getexp(Inf)", 2, 0x7FFF, 0, SIG_J, 0},
        {"getman(0)", 3, 0x0000, 0, 0, 0},
        {"rem(6,2)", 4, 0x4001, 0x4000, UINT64_C(0xC000000000000000), SIG_J}, /* 剰余 0 */
        {"rem(-0,1)", 4, 0x8000, 0x3FFF, 0, SIG_J},
        {"scale(0,1)", 5, 0x0000, 0x3FFF, 0, SIG_J},
        {"scale(1,Inf)", 5, 0x3FFF, 0x7FFF, SIG_J, SIG_J},
        {"lognp1(-2)", 6, 0xC000, 0, SIG_J, 0},
        {"lognp1(0)", 6, 0x0000, 0, 0, 0},
        {"etoxm1(+Inf)", 7, 0x7FFF, 0, SIG_J, 0},
        {"twotox(0)", 8, 0x0000, 0, 0, 0},
        {"tentox(-Inf)", 9, 0xFFFF, 0, SIG_J, 0},
    };

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        mc68k_fpu_state_t st = make_state(RN, PX);
        mc68k_fp80_t a = fp(t[i].a_sexp, t[i].a_sig), b = fp(t[i].b_sexp, t[i].b_sig);

        st.fpsr_exc = EXC_DZ;
        switch (t[i].kind) {
        case 0:
            (void)mc68k_fpu_sglmul(a, b, &st);
            break;
        case 1:
            (void)mc68k_fpu_sgldiv(a, b, &st);
            break;
        case 2:
            (void)mc68k_fpu_getexp(a, &st);
            break;
        case 3:
            (void)mc68k_fpu_getman(a, &st);
            break;
        case 4:
            (void)mc68k_fpu_rem(a, b, &st);
            break;
        case 5:
            (void)mc68k_fpu_scale(a, b, &st);
            break;
        case 6:
            (void)mc68k_fpu_lognp1(a, &st);
            break;
        case 7:
            (void)mc68k_fpu_etoxm1(a, &st);
            break;
        case 8:
            (void)mc68k_fpu_twotox(a, &st);
            break;
        default:
            (void)mc68k_fpu_tentox(a, &st);
            break;
        }
        g_rows++;
        if ((st.fpsr_exc & EXC_DZ) == 0) {
            g_failures++;
            fprintf(stderr, "or_only_special %s: DZ was cleared\n", t[i].name);
        }
    }
    CHECK(sizeof(t) / sizeof(t[0]) == 13);
}

static void test_exc_or_only(void) {
    mc68k_fpu_state_t st = make_state(RN, PX);

    st.fpsr_exc = EXC_DZ;
    (void)mc68k_fpu_etoxm1(fp(ONE_X), &st);
    CHECK(st.fpsr_exc == (EXC_DZ | EXC_INEX2));
    st = make_state(RN, PX);
    st.fpsr_exc = EXC_DZ;
    (void)mc68k_fpu_sglmul(fp(TWO_X), fp(ONE_X), &st);
    CHECK(st.fpsr_exc == EXC_DZ);
    st = make_state(RN, PX);
    st.fpsr_exc = EXC_INEX2;
    (void)mc68k_fpu_scale(fp(ONE_X), fp(ZERO_X(0)), &st);
    CHECK(st.fpsr_exc == EXC_INEX2);
    st = make_state(RN, PX);
    st.fpsr_exc = EXC_INEX2;
    (void)mc68k_fpu_rem(fp(ONE_X), fp(TWO_X), &st);
    CHECK(st.fpsr_exc == EXC_INEX2);
}

static void test_vectors(void) {
    run_vectors("b2_rem.txt", 3600, 1, vec_rem);
    run_vectors("b2_sgl.txt", 6888, 1, vec_sgl);
    run_vectors("b2_scale.txt", 16056, 1, vec_scale);
    run_vectors("b2_getexp.txt", 1092, 2, vec_getexp);
    run_tr("b2_tr_etoxm1.txt", mc68k_fpu_etoxm1, 524);
    run_tr("b2_tr_lognp1.txt", mc68k_fpu_lognp1, 480);
    run_tr("b2_tr_twotox.txt", mc68k_fpu_twotox, 430);
    run_tr("b2_tr_tentox.txt", mc68k_fpu_tentox, 465);
    run_vectors("b2_core.txt", 2392, 1, vec_core);
    if (getenv("FPU_B2_VERBOSE") != NULL)
        printf("core worst relative error: 2^-%d\n", g_core_worst);
}

int main(void) {
    test_vectors();
    test_rem_special();
    test_sgl_special();
    test_scale_special();
    test_getexp_special();
    test_transcend_specials();
    test_sincos();
    test_exc_or_only();
    test_exc_or_only_special();
    test_below_one_direction();
    if (g_rows != EXPECT_ROWS) {
        g_failures++;
        fprintf(stderr, "special rows executed %ld, expected %d\n", g_rows, EXPECT_ROWS);
    }
    if (g_failures != 0) {
        fprintf(stderr, "test_fpu_b2: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_fpu_b2: all passed\n");
    return 0;
}
