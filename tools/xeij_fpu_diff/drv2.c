#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "mc68000/fpu.h"
int main(void) {
    char fn[16], line[256];
    unsigned mode;
    int k;
    unsigned se;
    uint64_t sg;
    unsigned w0, w1, w2;
    while (fgets(line, sizeof line, stdin)) {
        mc68k_fpu_state_t st;
        mc68k_fpu_reset(&st);
        uint32_t w[3];
        sscanf(line, "%15s %u", fn, &mode);
        st.fpcr_round_mode = mode;
        if (!strcmp(fn, "pout")) {
            sscanf(line, "%*s %*u %d %x %" SCNx64, &k, &se, &sg);
            mc68k_fp80_t x = {se, sg};
            mc68k_fpu_to_p(x, (int8_t)k, w, &st);
            printf("pout %08x%08x%08x %08x\n", w[0], w[1], w[2], (unsigned)st.fpsr_exc << 8);
        } else {
            sscanf(line, "%*s %*u %x %x %x", &w0, &w1, &w2);
            w[0] = w0;
            w[1] = w1;
            w[2] = w2;
            mc68k_fp80_t r = mc68k_fpu_from_p(w, &st);
            printf("pin %04x %016" PRIx64 " %08x\n", r.sign_exp, r.signif,
                   (unsigned)st.fpsr_exc << 8);
        }
    }
    return 0;
}
