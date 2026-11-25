/*
 * test_fpu_b1.c — 1b Phase B1（FSQRT・FINT・FINTRZ・FMOD と超越関数 14 種）のテスト（Issue
 * #59）。
 *
 * 期待値は tests/fpu/gen_fpu_1b_vectors.py が独立に求めたもの（tests/fpu/vectors/b1_*.txt。IEEE
 * 規定演算と ±π/2・π の特殊値は fractions.Fraction の厳密演算、超越関数は mpmath）。特殊値は
 * [881UM] の各命令ページの表 （p.4-109 FSQRT、p.4-50 FINT、p.4-62 FMOD、超越関数は設計書 §5.2
 * の表の各ページ）から手で書く。
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

/* 呼び出し前の FPCC・商 byte の毒値。FPCC は結果から必ず設定し直されること、商 byte は FMOD
 * 以外が 変えないこと（設計書 §5.3）と FMOD が先頭でクリアすることを、0
 * 初期化では見分けられないため */
#define CC_POISON       0x0Fu
#define QUOTIENT_POISON 0x55u
#define QSIGN_POISON    1u

static int g_failures;
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

typedef mc68k_fp80_t (*unary_fn)(mc68k_fp80_t, mc68k_fpu_state_t *);

static bool same_fp(mc68k_fp80_t v, unsigned sexp, uint64_t sig) {
    return v.sign_exp == sexp && v.signif == sig;
}

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
 * checks_per_line: 1 行あたりの判定回数（vec_tr は丸めモード 4 通り）。判定回数 g_checks
 * も突き合わせ、 runner の早期 return や内側のループの縮小を検出する。 */
static void run_vectors(const char *name, long expect_count, long checks_per_line,
                        vector_fn fn) {
    long declared, executed = 0, checks_before = g_checks;
    char line[256];
    FILE *f = open_vectors(name, &declared);

    while (fgets(line, sizeof(line), f) != NULL) {
        if (!fn(line)) {
            g_failures++;
            if (g_failures < 20)
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
           st->fpsr_cc == expected_cc(esexp, esig);
}

static bool vec_sqrt(const char *line) {
    int prec, mode;
    unsigned sexp, esexp, eexc;
    uint64_t sig, esig;
    mc68k_fpu_state_t st;
    mc68k_fp80_t got;

    if (sscanf(line, "sqrt %d %d %x %" SCNx64 " => %x %" SCNx64 " %x", &prec, &mode, &sexp,
               &sig, &esexp, &esig, &eexc) != 7)
        return false;
    st = make_state((uint8_t)mode, (uint8_t)prec);
    got = mc68k_fpu_sqrt(fp(sexp, sig), &st);
    return result_matches(got, esexp, esig, &st, eexc) && quotient_untouched(&st);
}

static bool vec_int(const char *line) {
    char fn[16];
    int prec, mode;
    unsigned sexp, esexp, eexc;
    uint64_t sig, esig;
    mc68k_fpu_state_t st;
    mc68k_fp80_t got;

    if (sscanf(line, "%15s %d %d %x %" SCNx64 " => %x %" SCNx64 " %x", fn, &prec, &mode, &sexp,
               &sig, &esexp, &esig, &eexc) != 8)
        return false;
    st = make_state((uint8_t)mode, (uint8_t)prec);
    got = strcmp(fn, "int") == 0 ? mc68k_fpu_int(fp(sexp, sig), &st)
                                 : mc68k_fpu_intrz(fp(sexp, sig), &st);
    return result_matches(got, esexp, esig, &st, eexc) && quotient_untouched(&st);
}

static bool vec_mod(const char *line) {
    int prec, mode;
    unsigned dsexp, ssexp, esexp, eexc, qsign, q;
    uint64_t dsig, ssig, esig;
    mc68k_fpu_state_t st;
    mc68k_fp80_t got;

    if (sscanf(line, "mod %d %d %x %" SCNx64 " %x %" SCNx64 " => %x %" SCNx64 " %x %u %u",
               &prec, &mode, &dsexp, &dsig, &ssexp, &ssig, &esexp, &esig, &eexc, &qsign,
               &q) != 11) {
        return false;
    }
    st = make_state((uint8_t)mode, (uint8_t)prec);
    got = mc68k_fpu_mod(fp(dsexp, dsig), fp(ssexp, ssig), &st);
    return result_matches(got, esexp, esig, &st, eexc) && st.fpsr_quotient_sign == qsign &&
           st.fpsr_quotient == q;
}

/* ---- 特殊値 -------------------------------------------------------------------- */

/* NaN 入力（1a design §5.2・[881UM] §4.5.4）: qNaN はそのまま（EXC なし）、SNaN は quiet 化して
 * SNAN。 符号と仮数は保つ。FPCC は NAN（負なら N も）。単項演算の共通ヘルパー fpu_monadic_nan()
 * の検査。 */
static bool monadic_nan_ok(unary_fn fn) {
    static const struct {
        unsigned sexp;
        uint64_t sig, out_sig;
        unsigned exc;
    } t[] = {
        {0x7FFF, SIG_J | QUIET | 5, SIG_J | QUIET | 5, 0},
        {0xFFFF, SIG_J | QUIET | 5, SIG_J | QUIET | 5, 0},
        {0x7FFF, SIG_J | 1, SIG_J | QUIET | 1, MC68K_FPU_EXC_SNAN},
        {0xFFFF, SIG_J | 1, SIG_J | QUIET | 1, MC68K_FPU_EXC_SNAN},
    };
    bool ok = true;

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
        mc68k_fp80_t got = fn(fp(t[i].sexp, t[i].sig), &st);

        if (!same_fp(got, t[i].sexp, t[i].out_sig) || st.fpsr_exc != t[i].exc ||
            st.fpsr_cc != expected_cc(t[i].sexp, t[i].out_sig)) {
            ok = false;
        }
    }
    return ok;
}

static void test_sqrt_special(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t v;

    v = mc68k_fpu_sqrt(fp(0x0000, 0), &st); /* +0 → +0 */
    CHECK(v.sign_exp == 0 && v.signif == 0 && st.fpsr_cc == MC68K_FPU_CC_Z && st.fpsr_exc == 0);
    v = mc68k_fpu_sqrt(fp(0x8000, 0), &st); /* -0 → -0 */
    CHECK(v.sign_exp == 0x8000 && v.signif == 0 &&
          st.fpsr_cc == (MC68K_FPU_CC_Z | MC68K_FPU_CC_N));
    v = mc68k_fpu_sqrt(fp(0x7FFF, SIG_J), &st); /* +Inf → +Inf */
    CHECK(v.sign_exp == 0x7FFF && v.signif == SIG_J && st.fpsr_cc == MC68K_FPU_CC_I &&
          st.fpsr_exc == 0);
    v = mc68k_fpu_sqrt(fp(0xFFFF, SIG_J), &st); /* -Inf → NaN, OPERR */
    CHECK(is_default_nan(v) && (st.fpsr_exc & MC68K_FPU_EXC_OPERR) &&
          st.fpsr_cc == MC68K_FPU_CC_NAN);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_sqrt(fp(0xBFFF, SIG_J), &st); /* -1 → NaN, OPERR */
    CHECK(is_default_nan(v) && st.fpsr_exc == MC68K_FPU_EXC_OPERR &&
          (st.fpsr_aexc & MC68K_FPU_AEXC_IOP));
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_sqrt(fp(0x7FFF, SIG_J | 1), &st); /* SNaN → quiet NaN, SNAN */
    CHECK(v.sign_exp == 0x7FFF && v.signif == (SIG_J | 1 | QUIET) &&
          st.fpsr_exc == MC68K_FPU_EXC_SNAN);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_sqrt(fp(0x4001, SIG_J), &st); /* sqrt(4)=2 は厳密 */
    CHECK(v.sign_exp == 0x4000 && v.signif == SIG_J && st.fpsr_exc == 0);
    /* PREC=S で √(約 2^16384) = 約 2^8192 は単精度の範囲を超え、RN では +Inf になるが OVFL は 0
     * に強制（design §5.2） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    v = mc68k_fpu_sqrt(fp(0x7FFE, ALL_ONES), &st);
    CHECK(v.sign_exp == 0x7FFF && v.signif == SIG_J &&
          (st.fpsr_exc & (MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_UNFL)) == 0 &&
          (st.fpsr_exc & MC68K_FPU_EXC_INEX2));
    CHECK(monadic_nan_ok(mc68k_fpu_sqrt));
}

static void test_int_special(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t v;

    v = mc68k_fpu_intrz(fp(0x4006, UINT64_C(0x8992000000000000)),
                        &st); /* 137.57 → 137.0 (p.4-52) */
    CHECK(v.sign_exp == 0x4006 && v.signif == UINT64_C(0x8900000000000000) &&
          st.fpsr_exc == MC68K_FPU_EXC_INEX2);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_int(fp(0x8000, 0), &st); /* -0 → -0 */
    CHECK(v.sign_exp == 0x8000 && v.signif == 0 &&
          st.fpsr_cc == (MC68K_FPU_CC_Z | MC68K_FPU_CC_N));
    v = mc68k_fpu_int(fp(0xFFFF, SIG_J), &st); /* -Inf → -Inf */
    CHECK(v.sign_exp == 0xFFFF && v.signif == SIG_J && st.fpsr_exc == 0);
    v = mc68k_fpu_int(fp(0x403E, ALL_ONES), &st); /* 2^63 未満最大の整数 */
    CHECK(v.sign_exp == 0x403E && v.signif == ALL_ONES && st.fpsr_exc == 0);
    v = mc68k_fpu_int(fp(0x3FFE, SIG_J), &st); /* 0.5 → RN 偶数丸めで +0 */
    CHECK(v.sign_exp == 0 && v.signif == 0);
    v = mc68k_fpu_int(fp(0x7FFF, SIG_J | 1), &st); /* SNaN */
    CHECK(v.signif == (SIG_J | 1 | QUIET) && (st.fpsr_exc & MC68K_FPU_EXC_SNAN));
    CHECK(monadic_nan_ok(mc68k_fpu_int));
    CHECK(monadic_nan_ok(mc68k_fpu_intrz));
}

static void test_mod_special(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    mc68k_fp80_t v;
    const mc68k_fp80_t one = fp(0x3FFF, SIG_J), inf = fp(0x7FFF, SIG_J);

    /* 商 byte は命令の先頭で 0 にし、OPERR・NaN の経路では 0 のまま（設計書 §5.3
     * の暫定決定。U17 で XEiJ の rem() と一致）。make_state() が商 byte
     * を毒値にしているので、クリアしない実装はここで落ちる */
    v = mc68k_fpu_mod(one, fp(0x8000, 0),
                      &st); /* src=-0 → NaN, OPERR（符号の XOR は 1 でも 0） */
    CHECK(is_default_nan(v) && st.fpsr_exc == MC68K_FPU_EXC_OPERR && st.fpsr_quotient == 0 &&
          st.fpsr_quotient_sign == 0 && st.fpsr_cc == MC68K_FPU_CC_NAN);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_mod(fp(0xFFFF, SIG_J), one, &st); /* dst=-Inf, src=+1 → NaN, OPERR（同上） */
    CHECK(is_default_nan(v) && st.fpsr_exc == MC68K_FPU_EXC_OPERR && st.fpsr_quotient == 0 &&
          st.fpsr_quotient_sign == 0);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_mod(inf, one, &st); /* dst=Inf → NaN, OPERR */
    CHECK(is_default_nan(v) && st.fpsr_exc == MC68K_FPU_EXC_OPERR);
    /* NaN: dst 優先、src だけが NaN なら src。符号・仮数を保ち、SNaN は quiet 化して SNAN。商
     * byte は 0 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_mod(fp(0xBFFF, SIG_J), fp(0xFFFF, SIG_J | QUIET | 7), &st);
    CHECK(v.sign_exp == 0xFFFF && v.signif == (SIG_J | QUIET | 7) && st.fpsr_exc == 0 &&
          st.fpsr_quotient == 0 && st.fpsr_quotient_sign == 0 &&
          st.fpsr_cc == (MC68K_FPU_CC_NAN | MC68K_FPU_CC_N));
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_mod(fp(0xFFFF, SIG_J | QUIET | 9), one,
                      &st); /* dst が qNaN: SNAN は立てない */
    CHECK(v.sign_exp == 0xFFFF && v.signif == (SIG_J | QUIET | 9) && st.fpsr_exc == 0);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_mod(one, fp(0x7FFF, SIG_J | 3), &st); /* src が SNaN */
    CHECK(v.sign_exp == 0x7FFF && v.signif == (SIG_J | QUIET | 3) &&
          st.fpsr_exc == MC68K_FPU_EXC_SNAN);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_mod(fp(0xBFFF, SIG_J), inf,
                      &st); /* dst=-1, src=Inf → -1（符号だけ商 byte） */
    CHECK(v.sign_exp == 0xBFFF && v.signif == SIG_J && st.fpsr_quotient_sign == 1 &&
          st.fpsr_quotient == 0);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_mod(fp(0x8000, 0), one, &st); /* dst=-0 → -0 */
    CHECK(v.sign_exp == 0x8000 && v.signif == 0 && st.fpsr_quotient_sign == 1);
    /* src=±Inf で dst（有限）の値を返す経路（PREC=S・大きい dst）は Note 2（p.4-62）のとおり
     * INEX2 が起き、 OVFL も起き得る */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    v = mc68k_fpu_mod(fp(0x7FFE, ALL_ONES), inf, &st);
    CHECK((st.fpsr_exc & MC68K_FPU_EXC_OVFL) && (st.fpsr_exc & MC68K_FPU_EXC_INEX2));
    /* 商 byte は次の命令まで残る: 10 mod 3 = 1、N=3。続けて dst=-0 の mod
     * では符号だけ更新される */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_mod(fp(0x4002, UINT64_C(0xA000000000000000)),
                      fp(0x4000, UINT64_C(0xC000000000000000)), &st);
    CHECK(v.sign_exp == 0x3FFF && v.signif == SIG_J && st.fpsr_quotient == 3 &&
          st.fpsr_quotient_sign == 0);
    v = mc68k_fpu_mod(fp(0x4002, UINT64_C(0xA000000000000000)),
                      fp(0xC000, UINT64_C(0xC000000000000000)), &st);
    CHECK(st.fpsr_quotient == 3 && st.fpsr_quotient_sign == 1);
    /* NaN は dst 優先 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_mod(fp(0x7FFF, SIG_J | 5 | QUIET), fp(0x7FFF, SIG_J | 1), &st);
    CHECK(v.signif == (SIG_J | 5 | QUIET) && st.fpsr_exc == MC68K_FPU_EXC_SNAN);
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
        if (st.fpsr_exc != eexc || !quotient_untouched(&st))
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

static void test_transcend_vectors(void) {
    run_tr("b1_tr_sin.txt", mc68k_fpu_sin, 547);
    run_tr("b1_tr_cos.txt", mc68k_fpu_cos, 551);
    run_tr("b1_tr_tan.txt", mc68k_fpu_tan, 547);
    run_tr("b1_tr_atan.txt", mc68k_fpu_atan, 442);
    run_tr("b1_tr_asin.txt", mc68k_fpu_asin, 518);
    run_tr("b1_tr_acos.txt", mc68k_fpu_acos, 448);
    run_tr("b1_tr_atanh.txt", mc68k_fpu_atanh, 518);
    run_tr("b1_tr_sinh.txt", mc68k_fpu_sinh, 378);
    run_tr("b1_tr_cosh.txt", mc68k_fpu_cosh, 374);
    run_tr("b1_tr_tanh.txt", mc68k_fpu_tanh, 393);
    run_tr("b1_tr_etox.txt", mc68k_fpu_etox, 419);
    run_tr("b1_tr_logn.txt", mc68k_fpu_logn, 396);
    run_tr("b1_tr_log2.txt", mc68k_fpu_log2, 423);
    run_tr("b1_tr_log10.txt", mc68k_fpu_log10, 477);
}

/* ±π/2・π を返す特殊値（atan(±Inf)・asin(±1)・acos(±0)・acos(-1)）。無理数なので PREC・RM
 * で値が変わり、 INEX2 が立つ（設計書 §3.2 の表）。期待値は生成スクリプトが Fraction
 * で正しく丸めたもの（b1_const.txt） */
static bool vec_const(const char *line) {
    static const struct {
        const char *name;
        unary_fn fn;
    } fns[] = {
        {"atan", mc68k_fpu_atan},
        {"asin", mc68k_fpu_asin},
        {"acos", mc68k_fpu_acos},
    };
    char name[16];
    int prec, mode;
    unsigned sexp, esexp, eexc;
    uint64_t sig, esig;

    if (sscanf(line, "const %15s %d %d %x %" SCNx64 " => %x %" SCNx64 " %x", name, &prec, &mode,
               &sexp, &sig, &esexp, &esig, &eexc) != 8)
        return false;
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        if (strcmp(name, fns[i].name) == 0) {
            mc68k_fpu_state_t st = make_state((uint8_t)mode, (uint8_t)prec);
            mc68k_fp80_t got = fns[i].fn(fp(sexp, sig), &st);

            return result_matches(got, esexp, esig, &st, eexc) && quotient_untouched(&st);
        }
    }
    return false;
}

/* ---- 中間精度（float128）の値を mpmath の真値と直接比べる（設計書 §5.4 の誤差予算）
 * ---------------- 最終の丸めは 2^-40 ULP
 * の誤差を隠すため、係数の欠落などの小さな誤りは忠実丸めのベクタでは検出できない。
 * ここでは丸め前の相対誤差が CORE_TOLERANCE_EXP 以下であることを直接検査する。 */

#define CORE_TOLERANCE_EXP \
    110 /* 相対誤差 < 2^-110（設計書の予算 2^-104 より 6 bit 厳しい。実測の最大は 2^-112） */

static int g_core_worst; /* 観測した最大の相対誤差 2^-g_core_worst（診断用） */

static wide_t core_eval(const char *name, bool sign, int32_t exp, uint64_t sig) {
    wide_t x = w_make(sign, exp, sig, 0), s, c, m;
    int32_t k;
    uint64_t hi, lo;

    if (strcmp(name, "sin") == 0 || strcmp(name, "cos") == 0) {
        wide_sincos(exp, sig, &s, &c);
        return name[0] == 's' ? s : c;
    }
    if (strcmp(name, "etox") == 0) {
        wide_exp(x, &m, &k);
        return w_scale(m, k);
    }
    if (strcmp(name, "atan") == 0)
        return wide_atan(x);
    if (strcmp(name, "asin") == 0)
        return wide_asin(x);
    if (strcmp(name, "acos") == 0)
        return wide_acos(x);
    if (strcmp(name, "sinh") == 0) {
        m = wide_sinh(x, &k);
        return w_scale(m, k);
    }
    if (strcmp(name, "cosh") == 0) {
        m = wide_cosh(x, &k);
        return w_scale(m, k);
    }
    if (strcmp(name, "tanh") == 0)
        return wide_tanh(x);
    if (strcmp(name, "atanh") == 0)
        return wide_atanh(x);
    w_sig128(x, &hi, &lo);
    m = w_make(false, WIDE_BIAS, hi, lo);
    if (strcmp(name, "ln") == 0)
        return wide_ln_mu(m, exp - WIDE_BIAS);
    if (strcmp(name, "log2") == 0)
        return wide_log2_mu(m, exp - WIDE_BIAS);
    return wide_log10_mu(m, exp - WIDE_BIAS);
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

/* ---- 超越関数の特殊値・EXC・条件コード（[881UM] の各命令ページの表。設計書 §5.2）
 * --------------- */

#define PI_2_X_HI 0x3FFF, UINT64_C(0xC90FDAA22168C235) /* π/2 の RN・PREC=X */
#define PI_X_HI   0x4000, UINT64_C(0xC90FDAA22168C235)
#define ONE_X     0x3FFF, SIG_J
#define INF_X(s)  ((s) | 0x7FFF), SIG_J
#define NAN_DEF   0x7FFF, ALL_ONES

struct special {
    const char *name;
    unary_fn fn;
    unsigned in_sexp;
    uint64_t in_sig;
    unsigned out_sexp;
    uint64_t out_sig;
    unsigned exc;
};

static void test_transcend_specials(void) {
    static const struct special t[] = {
        {"sin(+0)", mc68k_fpu_sin, 0x0000, 0, 0x0000, 0, 0},
        {"sin(-0)", mc68k_fpu_sin, 0x8000, 0, 0x8000, 0, 0},
        {"sin(+Inf)", mc68k_fpu_sin, INF_X(0), NAN_DEF, 0x20},
        {"sin(-Inf)", mc68k_fpu_sin, INF_X(0x8000), NAN_DEF, 0x20},
        {"cos(+0)", mc68k_fpu_cos, 0x0000, 0, ONE_X, 0},
        {"cos(-0)", mc68k_fpu_cos, 0x8000, 0, ONE_X, 0},
        {"cos(+Inf)", mc68k_fpu_cos, INF_X(0), NAN_DEF, 0x20},
        {"tan(+0)", mc68k_fpu_tan, 0x0000, 0, 0x0000, 0, 0},
        {"tan(-0)", mc68k_fpu_tan, 0x8000, 0, 0x8000, 0, 0},
        {"tan(-Inf)", mc68k_fpu_tan, INF_X(0x8000), NAN_DEF, 0x20},
        {"atan(+0)", mc68k_fpu_atan, 0x0000, 0, 0x0000, 0, 0},
        {"atan(-0)", mc68k_fpu_atan, 0x8000, 0, 0x8000, 0, 0},
        {"atan(+Inf)", mc68k_fpu_atan, INF_X(0), PI_2_X_HI, 0x02},
        {"atan(-Inf)", mc68k_fpu_atan, INF_X(0x8000), 0xBFFF, UINT64_C(0xC90FDAA22168C235),
         0x02},
        {"asin(+0)", mc68k_fpu_asin, 0x0000, 0, 0x0000, 0, 0},
        {"asin(-0)", mc68k_fpu_asin, 0x8000, 0, 0x8000, 0, 0},
        {"asin(1)", mc68k_fpu_asin, ONE_X, PI_2_X_HI, 0x02},
        {"asin(-1)", mc68k_fpu_asin, 0xBFFF, SIG_J, 0xBFFF, UINT64_C(0xC90FDAA22168C235), 0x02},
        {"asin(2)", mc68k_fpu_asin, 0x4000, SIG_J, NAN_DEF, 0x20},
        {"asin(-Inf)", mc68k_fpu_asin, INF_X(0x8000), NAN_DEF, 0x20},
        {"acos(+0)", mc68k_fpu_acos, 0x0000, 0, PI_2_X_HI, 0x02},
        {"acos(-0)", mc68k_fpu_acos, 0x8000, 0, PI_2_X_HI, 0x02},
        {"acos(1)", mc68k_fpu_acos, ONE_X, 0x0000, 0, 0},
        {"acos(-1)", mc68k_fpu_acos, 0xBFFF, SIG_J, PI_X_HI, 0x02},
        {"acos(2)", mc68k_fpu_acos, 0x4000, SIG_J, NAN_DEF, 0x20},
        {"acos(+Inf)", mc68k_fpu_acos, INF_X(0), NAN_DEF, 0x20},
        {"atanh(+0)", mc68k_fpu_atanh, 0x0000, 0, 0x0000, 0, 0},
        {"atanh(-0)", mc68k_fpu_atanh, 0x8000, 0, 0x8000, 0, 0},
        {"atanh(1)", mc68k_fpu_atanh, ONE_X, INF_X(0), 0x04},
        {"atanh(-1)", mc68k_fpu_atanh, 0xBFFF, SIG_J, INF_X(0x8000), 0x04},
        {"atanh(2)", mc68k_fpu_atanh, 0x4000, SIG_J, NAN_DEF, 0x20},
        {"atanh(+Inf)", mc68k_fpu_atanh, INF_X(0), NAN_DEF, 0x20},
        {"sinh(+0)", mc68k_fpu_sinh, 0x0000, 0, 0x0000, 0, 0},
        {"sinh(-0)", mc68k_fpu_sinh, 0x8000, 0, 0x8000, 0, 0},
        {"sinh(+Inf)", mc68k_fpu_sinh, INF_X(0), INF_X(0), 0},
        {"sinh(-Inf)", mc68k_fpu_sinh, INF_X(0x8000), INF_X(0x8000), 0},
        {"cosh(+0)", mc68k_fpu_cosh, 0x0000, 0, ONE_X, 0},
        {"cosh(-0)", mc68k_fpu_cosh, 0x8000, 0, ONE_X, 0},
        {"cosh(-Inf)", mc68k_fpu_cosh, INF_X(0x8000), INF_X(0), 0},
        {"tanh(+0)", mc68k_fpu_tanh, 0x0000, 0, 0x0000, 0, 0},
        {"tanh(-0)", mc68k_fpu_tanh, 0x8000, 0, 0x8000, 0, 0},
        {"tanh(+Inf)", mc68k_fpu_tanh, INF_X(0), ONE_X, 0},
        {"tanh(-Inf)", mc68k_fpu_tanh, INF_X(0x8000), 0xBFFF, SIG_J, 0},
        {"etox(+0)", mc68k_fpu_etox, 0x0000, 0, ONE_X, 0},
        {"etox(-0)", mc68k_fpu_etox, 0x8000, 0, ONE_X, 0},
        {"etox(+Inf)", mc68k_fpu_etox, INF_X(0), INF_X(0), 0},
        {"etox(-Inf)", mc68k_fpu_etox, INF_X(0x8000), 0x0000, 0, 0},
        {"logn(+0)", mc68k_fpu_logn, 0x0000, 0, INF_X(0x8000), 0x04},
        {"logn(-0)", mc68k_fpu_logn, 0x8000, 0, INF_X(0x8000), 0x04},
        {"logn(1)", mc68k_fpu_logn, ONE_X, 0x0000, 0, 0},
        {"logn(+Inf)", mc68k_fpu_logn, INF_X(0), INF_X(0), 0},
        {"logn(-1)", mc68k_fpu_logn, 0xBFFF, SIG_J, NAN_DEF, 0x20},
        {"logn(-Inf)", mc68k_fpu_logn, INF_X(0x8000), NAN_DEF, 0x20},
        {"log2(-0)", mc68k_fpu_log2, 0x8000, 0, INF_X(0x8000), 0x04},
        {"log2(2)", mc68k_fpu_log2, 0x4000, SIG_J, ONE_X, 0},
        {"log2(-2)", mc68k_fpu_log2, 0xC000, SIG_J, NAN_DEF, 0x20},
        {"log10(+0)", mc68k_fpu_log10, 0x0000, 0, INF_X(0x8000), 0x04},
        {"log10(1000)", mc68k_fpu_log10, 0x4008, UINT64_C(0xFA00000000000000), 0x4000,
         UINT64_C(0xC000000000000000), 0},
        {"log10(-Inf)", mc68k_fpu_log10, INF_X(0x8000), NAN_DEF, 0x20},
        /* 定義域外の境界: |x| が 1 の次の拡張精度値（1 + 2^-63）は OPERR（p.4-24・4-20・4-28
           の「> +1 or < -1」） */
        {"asin(1+ulp)", mc68k_fpu_asin, 0x3FFF, SIG_J | 1, NAN_DEF, 0x20},
        {"asin(-1-ulp)", mc68k_fpu_asin, 0xBFFF, SIG_J | 1, NAN_DEF, 0x20},
        {"acos(1+ulp)", mc68k_fpu_acos, 0x3FFF, SIG_J | 1, NAN_DEF, 0x20},
        {"acos(-1-ulp)", mc68k_fpu_acos, 0xBFFF, SIG_J | 1, NAN_DEF, 0x20},
        {"atanh(1+ulp)", mc68k_fpu_atanh, 0x3FFF, SIG_J | 1, NAN_DEF, 0x20},
        {"atanh(-1-ulp)", mc68k_fpu_atanh, 0xBFFF, SIG_J | 1, NAN_DEF, 0x20},
        {"asin(+Inf)", mc68k_fpu_asin, INF_X(0), NAN_DEF, 0x20},
        {"acos(-Inf)", mc68k_fpu_acos, INF_X(0x8000), NAN_DEF, 0x20},
        {"atanh(-Inf)", mc68k_fpu_atanh, INF_X(0x8000), NAN_DEF, 0x20},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
        mc68k_fp80_t got = t[i].fn(fp(t[i].in_sexp, t[i].in_sig), &st);

        if (!same_fp(got, t[i].out_sexp, t[i].out_sig) || st.fpsr_exc != t[i].exc ||
            st.fpsr_cc != expected_cc(t[i].out_sexp, t[i].out_sig) ||
            !quotient_untouched(&st)) {
            g_failures++;
            fprintf(stderr, "special %s: got %04x/%016" PRIx64 " exc %02x cc %x\n", t[i].name,
                    got.sign_exp, got.signif, st.fpsr_exc, st.fpsr_cc);
        }
        /* NaN 入力（qNaN・SNaN、正負）は全命令共通 */
        if (!monadic_nan_ok(t[i].fn)) {
            g_failures++;
            fprintf(stderr, "special %s: NaN input\n", t[i].name);
        }
    }
    CHECK((int)(sizeof(t) / sizeof(t[0])) == 68); /* 表の行数（縮小に気づけるように） */
}

/* EXC byte のマスク（設計書 §5.2 の表）と OVFL/UNFL の検出 */
static void test_transcend_flags(void) {
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;
    const mc68k_fp80_t denorm = fp(0x0000, UINT64_C(0x0000000000000400));

    /* FASIN・FACOS の UNFL は 0 に強制。FATAN・FSIN・FTAN・FSINH・FTANH・FATANH は UNFL を採用
     */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_asin(denorm, &st);
    CHECK(v.sign_exp == 0 && st.fpsr_exc == MC68K_FPU_EXC_INEX2);
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    (void)mc68k_fpu_atan(denorm, &st);
    CHECK(st.fpsr_exc == (MC68K_FPU_EXC_INEX2 | MC68K_FPU_EXC_UNFL));
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    (void)mc68k_fpu_sinh(denorm, &st);
    CHECK(st.fpsr_exc == (MC68K_FPU_EXC_INEX2 | MC68K_FPU_EXC_UNFL));
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    (void)mc68k_fpu_atanh(denorm, &st);
    CHECK(st.fpsr_exc == (MC68K_FPU_EXC_INEX2 | MC68K_FPU_EXC_UNFL));
    /* FCOS・FCOSH の結果は 1 付近で UNFL・OVFL は起きない */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    (void)mc68k_fpu_cos(denorm, &st);
    CHECK(st.fpsr_exc == MC68K_FPU_EXC_INEX2);
    /* FETOX・FSINH・FCOSH のオーバーフロー: OVFL と INEX2、結果は Inf（RN） */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_etox(fp(0x400C, UINT64_C(0xB400000000000000)),
                       &st); /* x = 11520 > ln(2^16384) ≈ 11356.5 */
    CHECK(v.sign_exp == 0x7FFF && v.signif == SIG_J &&
          st.fpsr_exc == (MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_INEX2));
    st = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_sinh(fp(0x400D, UINT64_C(0xC000000000000000)), &st); /* RZ は最大値へ */
    CHECK(v.sign_exp == 0x7FFE && v.signif == ALL_ONES && (st.fpsr_exc & MC68K_FPU_EXC_OVFL));
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_SINGLE);
    v = mc68k_fpu_cosh(fp(0x4005, UINT64_C(0xC800000000000000)),
                       &st); /* cosh(100) > 単精度最大 */
    CHECK(v.sign_exp == 0x7FFF && (st.fpsr_exc & MC68K_FPU_EXC_OVFL));
    /* FETOX のアンダーフロー: -20000 は RN で +0、UNFL と INEX2 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_etox(fp(0xC00C, UINT64_C(0xB400000000000000)),
                       &st); /* x = -11520: e^x ≈ 2^-16620 */
    CHECK(v.sign_exp == 0 && v.signif == 0 &&
          st.fpsr_exc == (MC68K_FPU_EXC_UNFL | MC68K_FPU_EXC_INEX2));
    st = make_state(MC68K_FPU_ROUND_PLUS_INF, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_etox(fp(0xC00C, UINT64_C(0xB400000000000000)),
                       &st); /* RP は最小の非正規化数 */
    CHECK(v.sign_exp == 0 && v.signif == 1);
    /* 厳密な結果は INEX2 を立てない: log2(2^-3) = -3、log10(10^27) = 27 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    v = mc68k_fpu_log2(fp(0x3FFC, SIG_J), &st);
    CHECK(v.sign_exp == 0xC000 && v.signif == UINT64_C(0xC000000000000000) && st.fpsr_exc == 0);
    /* 有限入力の INEX2: sin(1) は不正確 */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    (void)mc68k_fpu_sin(fp(ONE_X), &st);
    CHECK(st.fpsr_exc == MC68K_FPU_EXC_INEX2);
    /* EXC は OR のみ: 既にある DZ を消さない */
    st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    st.fpsr_exc = MC68K_FPU_EXC_DZ;
    (void)mc68k_fpu_sin(fp(ONE_X), &st);
    CHECK(st.fpsr_exc == (MC68K_FPU_EXC_DZ | MC68K_FPU_EXC_INEX2));
}

/* fpu_wide_finish() の sticky（丸め位置より下のビットを不正確として強制する OR。design
 * §5.2「INEX2 と
 * 厳密ケース」）が効くことを直接検証する（セルフレビューで見つかった検出漏れ。#59 Phase B1）。
 * w をちょうど半分（sigExtra=0x8000...0。snap_to_grid の対象外になる位置。fpu_wide.c
 * 参照）に人為的に 置くと、sticky が無ければ round-to-nearest-even
 * は「タイ」のまま偶数丸め（変化なし）を選ぶが、 sticky
 * が正しく効いていれば「半分よりわずかに大きい」として常に切り上げる。b1_core.txt・b1_tr_*.txt
 * の 有限値ベクタは sticky の有無で値が変わるほど正確に f128
 * の格子点へ乗る入力を含まないため、この境界は 通常のベクタでは検出できない。 */
static void test_wide_sticky_tie(void) {
    mc68k_fpu_state_t st = make_state(MC68K_FPU_ROUND_NEAREST, MC68K_FPU_PREC_EXTENDED);
    wide_t w = w_make(false, 0x3FFF, SIG_J, UINT64_C(0x8000000000000000));
    mc68k_fp80_t v = fpu_wide_finish(&st, w, 0, false, 0);

    CHECK(v.sign_exp == 0x3FFF && v.signif == (SIG_J | 1) &&
          (st.fpsr_exc & MC68K_FPU_EXC_INEX2));
}

/* snap_to_grid()（fpu_wide.c。設計書
 * §5.2「近格子スナップ」）の残りの分岐を直接検証する（第三者レビュー B1-B4。
 * 通常のベクタは真値と f128
 * の中間値が格子点の同じ側にあり、スナップの有無で結果が変わらないため検出できない）。
 * どの丸めモードでも格子点そのものを INEX2 つきで返すことを、jam だけでは格子点を返さない
 * RZ（上側）・RP（下側）で見る。 */
static void test_wide_snap(void) {
    mc68k_fpu_state_t st;
    mc68k_fp80_t v;
    /* 2 - 2^-112（f128 で 2 の直下）: 次の格子点 2.0 の 2^-49 ULP
     * 下。上側へのスナップで仮数が桁上がりし binade をまたぐ */
    const wide_t below_two = w_make(false, 0x3FFF, ALL_ONES, ALL_ONES);
    /* 1 + 2^-100 を 2^-16393 倍（idealized 指数 -10、PREC=X の非正規化域で有効
     * 54bit）。格子点からの距離は、 非正規化域の量子（2^-53 相対）で測ると 2^-47 ULP（2^-40 ULP
     * 以内）、正規化数の量子（2^-63 相対）なら 2^-37 ULP */
    const wide_t above_grid = w_make(false, 0x3FFF, SIG_J, UINT64_C(1) << 27);

    st = make_state(MC68K_FPU_ROUND_ZERO, MC68K_FPU_PREC_EXTENDED);
    v = fpu_wide_finish(&st, below_two, 0, false, 0);
    CHECK(v.sign_exp == 0x4000 && v.signif == SIG_J && st.fpsr_exc == MC68K_FPU_EXC_INEX2);
    st = make_state(MC68K_FPU_ROUND_PLUS_INF, MC68K_FPU_PREC_EXTENDED);
    v = fpu_wide_finish(&st, above_grid, -(0x3FFF + 10), false, 0);
    CHECK(v.sign_exp == 0 && v.signif == (SIG_J >> 10) &&
          st.fpsr_exc == (MC68K_FPU_EXC_INEX2 | MC68K_FPU_EXC_UNFL));
}

int main(void) {
    run_vectors("b1_sqrt.txt", 3276, 1, vec_sqrt);
    run_vectors("b1_int.txt", 10080, 1, vec_int);
    run_vectors("b1_mod.txt", 3060, 1, vec_mod);
    run_vectors("b1_const.txt", 84, 1, vec_const);
    test_transcend_vectors();
    run_vectors("b1_core.txt", 4315, 1, vec_core);
    if (getenv("FPU_B1_VERBOSE") != NULL)
        printf("core worst relative error: 2^-%d\n", g_core_worst);
    test_transcend_specials();
    test_transcend_flags();
    test_sqrt_special();
    test_int_special();
    test_mod_special();
    test_wide_sticky_tie();
    test_wide_snap();
    if (g_failures != 0) {
        fprintf(stderr, "test_fpu_b1: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_fpu_b1: all passed\n");
    return 0;
}
