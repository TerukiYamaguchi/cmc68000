#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>

typedef struct {
    unsigned char m[0x1000];
} ctx_t;
static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)p)->m[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    *v = (uint16_t)((c->m[a] << 8) | c->m[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    *v = ((uint32_t)c->m[a] << 24) | ((uint32_t)c->m[a + 1] << 16) |
         ((uint32_t)c->m[a + 2] << 8) | c->m[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ((ctx_t *)p)->m[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    c->m[a] = v >> 8;
    c->m[a + 1] = v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    c->m[a] = v >> 24;
    c->m[a + 1] = v >> 16;
    c->m[a + 2] = v >> 8;
    c->m[a + 3] = v;
    return true;
}
static void pw(ctx_t *c, unsigned a, uint16_t v) {
    c->m[a] = v >> 8;
    c->m[a + 1] = v;
}
static void pl(ctx_t *c, unsigned a, uint32_t v) {
    c->m[a] = v >> 24;
    c->m[a + 1] = v >> 16;
    c->m[a + 2] = v >> 8;
    c->m[a + 3] = v;
}

int main(void) {
    ctx_t c = {0};
    pl(&c, 0, 0x800);
    pl(&c, 4, 0x100);
    pl(&c, 11 * 4, 0x300);
    pl(&c, 8 * 4, 0x320);
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
    mc68k_reset(cpu);

    /* Undefined PMMU class takes vector 11 with the instruction PC. */
    pw(&c, 0x100, 0xf010);
    pw(&c, 0x102, 0xa000);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 11);

    /* LEVEL=0 with A is explicitly undefined for MC68030 PTEST. */
    pw(&c, 0x104, 0xf010);
    pw(&c, 0x106, 0x8315);
    cpu->pc = 0x104;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 11);

    /* Encodings that belong to another model or to the 68851 are outside the
     * MC68030's implementation and take vector 11, not a configuration error.
     * Each pair is opcode + extension word. */
    static const struct {
        uint32_t at;
        uint16_t opcode, ext;
    } outside[] = {
        {0x110, 0xf010, 0x0400}, /* klass 0 P-reg 001: the EC030's AC0 encoding */
        {0x114, 0xf010, 0x4400}, /* klass 2 P-reg 001: 68851 DRP */
        {0x118, 0xf010, 0x5400}, /* klass 2 P-reg 101: 68851 VAL */
        {0x11c, 0xf010, 0x6400}, /* klass 3 P-reg 001: 68851 PCSR */
        {0x120, 0xf010, 0x8202}, /* PTEST with an undefined FC encoding */
        {0x124, 0xf000, 0x30ff}, /* PFLUSH with an undefined FC encoding */
        {0x128, 0xf000, 0x0800}, /* PMOVE TT0 with Dn as its EA */
        {0x12c, 0xf018, 0x8215}, /* PTEST with (An)+ as its EA */
        {0x130, 0xf020, 0x2215}, /* PLOAD with -(An) as its EA */
    };
    for (unsigned i = 0; i < sizeof outside / sizeof outside[0]; ++i) {
        pw(&c, outside[i].at, outside[i].opcode);
        pw(&c, outside[i].at + 2, outside[i].ext);
        cpu->pc = outside[i].at;
        cpu->last_exception_vector = 0;
        mc68k_execute(cpu, 1);
        assert(cpu->pc == 0x300 && cpu->last_exception_vector == 11);
    }

    /* PMMU instructions are privileged before their extension is consumed. */
    cpu->sr &= (uint16_t)~SR_S;
    cpu->pc = 0x108;
    pw(&c, 0x108, 0xf010);
    pw(&c, 0x10a, 0x2400);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x320 && cpu->last_exception_vector == 8);
    /* A non-zero cp-ID is not a PMMU instruction at all, so it stays vector 11
     * even in user mode where cp-ID 0 gives a privilege violation. */
    pw(&c, 0x10c, 0xf200);
    cpu->pc = 0x10c;
    cpu->last_exception_vector = 0;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 11);
    cpu->sr |= SR_S;
    cpu->pc = 0x10c;
    cpu->last_exception_vector = 0;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 11);
    /* CALLM and RTM were removed on the MC68030, so their 68020 encodings are
     * unassigned opcodes (vector 4), not F-line. */
    cpu->sr |= SR_S;
    pl(&c, 4 * 4, 0x340);
    static const uint16_t removed[] = {0x06c0 /* RTM D0 */, 0x06d0 /* CALLM (A0) */};
    for (unsigned i = 0; i < sizeof removed / sizeof removed[0]; ++i) {
        pw(&c, 0x140 + i * 4, removed[i]);
        cpu->pc = 0x140 + i * 4;
        cpu->last_exception_vector = 0;
        mc68k_execute(cpu, 1);
        assert(cpu->pc == 0x340 && cpu->last_exception_vector == 4);
    }
    assert(!cpu_is_68040(cpu));
    assert(!cpu_has_68040_cache(cpu));
    assert(!cpu_has_integrated_fpu(cpu));
    mc68k_destroy(cpu);

    /* The MC68040 has its own MMU instruction encoding.  The 68030 PMOVE and
     * PLOAD encodings must remain F-line exceptions until the 040 PFLUSH and
     * PTEST handlers are installed. */
    ctx_t c40 = {0};
    pl(&c40, 0, 0x800);
    pl(&c40, 4, 0x100);
    pl(&c40, 11 * 4, 0x300);
    mc68k_bus_t bus40 = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c40};
    cpu = mc68k_create_model(&bus40, MC68K_MODEL_68040);
    assert(cpu);
    mc68k_reset(cpu);
    static const struct {
        uint16_t opcode, ext;
    } invalid_040_pmmu[] = {
        {0xf010, 0x4000}, /* PMOVE (A0),TC */
        {0xf020, 0x2200}, /* PLOADR (A0) */
    };
    for (unsigned i = 0; i < sizeof invalid_040_pmmu / sizeof invalid_040_pmmu[0]; i++) {
        pw(&c40, 0x120, invalid_040_pmmu[i].opcode);
        pw(&c40, 0x122, invalid_040_pmmu[i].ext);
        cpu->pc = 0x120;
        cpu->last_exception_vector = 0;
        mc68k_execute(cpu, 1);
        assert(cpu->pc == 0x300 && cpu->last_exception_vector == 11);
    }
    assert(cpu_is_68040(cpu));
    assert(cpu_has_68040_cache(cpu));
    assert(!cpu_has_integrated_fpu(cpu));
    mc68k_destroy(cpu);

    cpu = mc68k_create_model_with_fpu(&bus40, MC68K_MODEL_68040, MC68K_FPU_68040);
    assert(cpu);
    assert(cpu_has_integrated_fpu(cpu));
    mc68k_destroy(cpu);

    /* A 68020 has no PMMU at all: every cp-ID 0 encoding stays F-line.  The
     * 68020 instruction set and its exception frames are otherwise identical
     * on both models. */
    static const uint16_t pmmu[] = {0x4000 /* PMOVE (A0),TC */, 0x2400 /* PFLUSHA */,
                                    0x8215 /* PTESTR #5,(A0),#0 */};
    ctx_t c20 = {0}, c30 = {0};
    ctx_t *pair[2] = {&c20, &c30};
    mc68k_state_t *cpus[2];
    for (unsigned k = 0; k < 2; ++k) {
        pl(pair[k], 0, 0x800);
        pl(pair[k], 4, 0x100);
        pl(pair[k], 11 * 4, 0x300);
        pl(pair[k], 4 * 4, 0x340);
        pw(pair[k], 0x100, 0x70ff); /* MOVEQ #-1,D0 */
        pw(pair[k], 0x102, 0x49c0); /* EXTB.L D0 : 68020 and later */
        pw(pair[k], 0x104, 0x4afc); /* ILLEGAL   */
        mc68k_bus_t b = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, pair[k]};
        cpus[k] = mc68k_create_model(&b, k == 0 ? MC68K_MODEL_68020 : MC68K_MODEL_68030);
        mc68k_reset(cpus[k]);
        for (unsigned i = 0; i < sizeof pmmu / sizeof pmmu[0]; ++i) {
            pw(pair[k], 0x120, 0xf010);
            pw(pair[k], 0x122, pmmu[i]);
            cpus[k]->pc = 0x120;
            cpus[k]->last_exception_vector = 0;
            mc68k_execute(cpus[k], 1);
            if (k == 0)
                assert(cpus[k]->pc == 0x300 && cpus[k]->last_exception_vector == 11);
        }
        cpus[k]->a[7] = 0x800;
        cpus[k]->pc = 0x100;
        mc68k_execute(cpus[k], 1);
        mc68k_execute(cpus[k], 1);
        mc68k_execute(cpus[k], 1);
    }
    mc68k_regs_t r20, r30;
    mc68k_get_regs(cpus[0], &r20);
    mc68k_get_regs(cpus[1], &r30);
    assert(r20.d[0] == 0xffffffff && r30.d[0] == r20.d[0]);
    assert(r20.pc == 0x340 && r30.pc == r20.pc && r20.a[7] == r30.a[7]);
    for (unsigned i = 0; i < 8; ++i)
        assert(c20.m[r20.a[7] + i] == c30.m[r30.a[7] + i]); /* same frame bytes */
    mc68k_destroy(cpus[0]);
    mc68k_destroy(cpus[1]);
}
