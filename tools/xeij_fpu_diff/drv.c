#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "mc68000/fpu.h"
typedef mc68k_fp80_t (*u1)(mc68k_fp80_t, mc68k_fpu_state_t *);
typedef mc68k_fp80_t (*u2)(mc68k_fp80_t, mc68k_fp80_t, mc68k_fpu_state_t *);
static const struct {
    const char *n;
    u1 f;
} U[] = {
    {"sin", mc68k_fpu_sin},       {"cos", mc68k_fpu_cos},       {"tan", mc68k_fpu_tan},
    {"atan", mc68k_fpu_atan},     {"asin", mc68k_fpu_asin},     {"acos", mc68k_fpu_acos},
    {"sinh", mc68k_fpu_sinh},     {"cosh", mc68k_fpu_cosh},     {"tanh", mc68k_fpu_tanh},
    {"atanh", mc68k_fpu_atanh},   {"etox", mc68k_fpu_etox},     {"logn", mc68k_fpu_logn},
    {"log2", mc68k_fpu_log2},     {"log10", mc68k_fpu_log10},   {"etoxm1", mc68k_fpu_etoxm1},
    {"lognp1", mc68k_fpu_lognp1}, {"twotox", mc68k_fpu_twotox}, {"tentox", mc68k_fpu_tentox},
    {"sqrt", mc68k_fpu_sqrt},     {"getexp", mc68k_fpu_getexp}, {"getman", mc68k_fpu_getman}};
static const struct {
    const char *n;
    u2 f;
} B[] = {{"sglmul", mc68k_fpu_sglmul},  {"sgldiv", mc68k_fpu_sgldiv},
         {"sgldivt", mc68k_fpu_sgldiv}, {"rem", mc68k_fpu_rem},
         {"mod", mc68k_fpu_mod},        {"scale", mc68k_fpu_scale}};
int main(void) {
    char fn[24];
    unsigned prec;
    unsigned mode, se, se2;
    uint64_t sg, sg2;
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        prec = 0;
        char *at;
        int n =
            sscanf(line, "%23s %u %x %" SCNx64 " %x %" SCNx64, fn, &mode, &se, &sg, &se2, &sg2);
        if ((at = strchr(fn, '@'))) {
            prec = atoi(at + 1);
            *at = 0;
        }
        mc68k_fpu_state_t st;
        mc68k_fpu_reset(&st);
        st.fpcr_round_mode = mode;
        st.fpcr_round_prec = prec;
        if (!strcmp(fn, "movecr")) {
            mc68k_fp80_t m = mc68k_fpu_movecr((uint8_t)se, &st);
            printf("%s %04x %016" PRIx64 " %08x\n", fn, m.sign_exp, m.signif,
                   (unsigned)st.fpsr_exc << 8);
            continue;
        }
        mc68k_fp80_t x = {se, sg}, z = {se2, sg2}, r = {0, 0};
        int ok = 0;
        for (size_t i = 0; i < sizeof U / sizeof U[0]; i++)
            if (!strcmp(fn, U[i].n)) {
                r = U[i].f(x, &st);
                ok = 1;
            }
        if (n >= 6)
            for (size_t i = 0; i < sizeof B / sizeof B[0]; i++)
                if (!strcmp(fn, B[i].n)) {
                    r = B[i].f(x, z, &st);
                    ok = 1;
                }
        if (!ok) {
            puts("?");
            continue;
        }
        printf("%s %04x %016" PRIx64 " %08x\n", fn, r.sign_exp, r.signif,
               ((unsigned)st.fpsr_exc << 8) | ((unsigned)st.fpsr_quotient_sign << 23) |
                   ((unsigned)st.fpsr_quotient << 16));
    }
    return 0;
}
