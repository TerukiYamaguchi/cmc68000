#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    uint8_t ram[0x3000];
} ctx_t;
static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)p)->ram[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    *v = (c->ram[a] << 8) | c->ram[a + 1];
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    *v = ((uint32_t)c->ram[a] << 24) | ((uint32_t)c->ram[a + 1] << 16) |
         ((uint32_t)c->ram[a + 2] << 8) | c->ram[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ((ctx_t *)p)->ram[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    c->ram[a] = v >> 8;
    c->ram[a + 1] = v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    c->ram[a] = v >> 24;
    c->ram[a + 1] = v >> 16;
    c->ram[a + 2] = v >> 8;
    c->ram[a + 3] = v;
    return true;
}
static void l(ctx_t *c, int a, uint32_t v) {
    c->ram[a] = v >> 24;
    c->ram[a + 1] = v >> 16;
    c->ram[a + 2] = v >> 8;
    c->ram[a + 3] = v;
}
int main(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    l(&c, 0, 0x2800);
    l(&c, 4, 0x100); /* LEA ([bd,A0],D0),A1: full, bd.w, post-index */
    c.ram[0x100] = 0x43;
    c.ram[0x101] = 0xf0;
    c.ram[0x102] = 0x01;
    c.ram[0x103] = 0x25;
    c.ram[0x104] = 0x00;
    c.ram[0x105] = 0x10;
    c.ram[0x110] = 0x00;
    c.ram[0x111] = 0x00;
    c.ram[0x112] = 0x02;
    c.ram[0x113] = 0x00;
    c.ram[0x106] = 0x4a;
    c.ram[0x107] = 0xfc;
    mc68k_bus_t b = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&b, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->a[0] = 0x100;
    cpu->d[0] = 4;
    mc68k_execute(cpu, 1);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.a[1] == 0x204);
    mc68k_destroy(cpu);

    /* BS and IS are independent in a full extension.  IS-only must retain
     * the normal direct-form timing row, rather than select the BS row. */
    memset(&c, 0, sizeof(c));
    l(&c, 0, 0x2800);
    l(&c, 4, 0x100);
    c.ram[0x100] = 0x43;
    c.ram[0x101] = 0xf0; /* LEA (d16,A0,D0),A1 */
    c.ram[0x102] = 0x01;
    c.ram[0x103] = 0x60; /* full, IS=1, bd.word, direct */
    c.ram[0x104] = 0x00;
    c.ram[0x105] = 0x10;
    mc68k_bus_t b2 = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    cpu = mc68k_create_model(&b2, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->a[0] = 0x100;
    mc68k_execute(cpu, 1);
    assert(cpu->last_ea.base_suppressed == 0);
    assert(cpu->last_ea.index_suppressed == 1);
    assert(ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea) == 7);
    mc68k_destroy(cpu);
    return 0;
}
