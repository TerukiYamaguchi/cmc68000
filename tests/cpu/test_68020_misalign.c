#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t ram[0x2000];
    int rb, rw, rl, wb, ww, wl;
} ctx_t;
static bool rb(void *p, uint32_t a, uint8_t *v) {
    ctx_t *c = p;
    c->rb++;
    *v = c->ram[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    c->rw++;
    *v = (c->ram[a] << 8) | c->ram[a + 1];
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    c->rl++;
    *v = ((uint32_t)c->ram[a] << 24) | ((uint32_t)c->ram[a + 1] << 16) |
         ((uint32_t)c->ram[a + 2] << 8) | c->ram[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    c->wb++;
    c->ram[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    c->ww++;
    c->ram[a] = v >> 8;
    c->ram[a + 1] = v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    c->wl++;
    c->ram[a] = v >> 24;
    c->ram[a + 1] = v >> 16;
    c->ram[a + 2] = v >> 8;
    c->ram[a + 3] = v;
    return true;
}
static void putl(ctx_t *c, uint32_t a, uint32_t v) {
    c->ram[a] = v >> 24;
    c->ram[a + 1] = v >> 16;
    c->ram[a + 2] = v >> 8;
    c->ram[a + 3] = v;
}
int main(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    putl(&c, 0, 0x1800);
    putl(&c, 4, 0x100);
    c.ram[0x100] = 0x20;
    c.ram[0x101] = 0x10; /* MOVE.L (A0),D0 */
    c.ram[0x102] = 0x4a;
    c.ram[0x103] = 0xfc;
    c.ram[0x201] = 0x12;
    c.ram[0x202] = 0x34;
    c.ram[0x203] = 0x56;
    c.ram[0x204] = 0x78;
    mc68k_bus_t b = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&b, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->a[0] = 0x201;
    c.rb = c.rw = c.rl = 0;
    mc68k_execute(cpu, 1);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.d[0] == 0x12345678);
    assert(c.rb == 2 && c.rw == 2 && c.rl == 0);
    mc68k_destroy(cpu);
    return 0;
}
