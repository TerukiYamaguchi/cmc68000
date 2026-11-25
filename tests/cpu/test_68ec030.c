#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <string.h>

typedef struct {
    unsigned char m[0x4000];
} ctx_t;
static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)p)->m[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    *v = (c->m[a] << 8) | c->m[a + 1];
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
    pl(&c, 0, 0x3000);
    pl(&c, 4, 0x100);
    pl(&c, 11 * 4, 0x200);
    mc68k_bus_t b = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&b, MC68K_MODEL_68EC030);
    mc68k_reset(cpu);
    assert(mc68k_get_model(cpu) == MC68K_MODEL_68EC030);
    /* Actual EC030 encoding: PMOVE (A7),AC0 / AC1. */
    cpu->a[7] = 0x400;
    pl(&c, 0x400, 0xa5008250);
    pw(&c, 0x100, 0xf017);
    pw(&c, 0x102, 0x0400);
    mc68k_execute(cpu, 1);
    mc68k_mmu_regs_t r;
    mc68k_get_mmu_regs(cpu, &r);
    assert(r.tt0 == 0xa5008250 && r.tc == 0 && r.crp == 0 && r.srp == 0);
    cpu->pc = 0x104;
    pw(&c, 0x104, 0xf017);
    pw(&c, 0x106, 0x0c00);
    pl(&c, 0x400, 0x5a008125);
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &r);
    assert(r.tt1 == 0x5a008125);
    /* PTESTR with immediate FC=5 hits AC0; PTESTW does not because AC0 wants reads. */
    cpu->pc = 0x108;
    cpu->a[0] = 0xa5123456;
    pw(&c, 0x108, 0xf010);
    pw(&c, 0x10a, 0x8215);
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &r);
    assert(r.mmusr & 0x40);
    cpu->pc = 0x10c;
    pw(&c, 0x10c, 0xf010);
    pw(&c, 0x10e, 0x8015);
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &r);
    assert(!(r.mmusr & 0x40));
    /* 68030 TT0 encoding is not valid on EC030 and remains F-line. */
    cpu->pc = 0x110;
    pw(&c, 0x110, 0xf017);
    pw(&c, 0x112, 0x0800);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x200);
    /* PFLUSHA and PMOVE (An),CRP/TC (klass 1/2) are accepted as NOP instead
     * of F-line: EC030 has no on-chip PMMU, but X68030 IPLROM's CPU probe
     * executes these expecting normal completion (). */
    cpu->pc = 0x130;
    pw(&c, 0x130, 0xf000);
    pw(&c, 0x132, 0x2400);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x134);
    cpu->pc = 0x134;
    cpu->a[7] = 0x400;
    pw(&c, 0x134, 0xf017);
    pw(&c, 0x136, 0x4c00);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x138);
    cpu->pc = 0x138;
    cpu->a[1] = 0x400;
    pw(&c, 0x138, 0xf011);
    pw(&c, 0x13a, 0x4000);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x13c);
    /* 030 CACR keeps only persistent bits; CAAR is full-width. */
    cpu->pc = 0x120;
    cpu->d[0] = 0xffffffffu;
    pw(&c, 0x120, 0x4e7b);
    pw(&c, 0x122, 0x0002);
    mc68k_execute(cpu, 1);
    cpu->pc = 0x124;
    pw(&c, 0x124, 0x4e7a);
    pw(&c, 0x126, 0x1002);
    mc68k_execute(cpu, 1);
    assert(cpu->d[1] == 0x3313);
    cpu->pc = 0x128;
    cpu->d[0] = 0xdeadbeef;
    pw(&c, 0x128, 0x4e7b);
    pw(&c, 0x12a, 0x0802);
    mc68k_execute(cpu, 1);
    cpu->pc = 0x12c;
    pw(&c, 0x12c, 0x4e7a);
    pw(&c, 0x12e, 0x1802);
    mc68k_execute(cpu, 1);
    assert(cpu->d[1] == 0xdeadbeef);
    mc68k_destroy(cpu);
}
