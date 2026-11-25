#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <string.h>

typedef struct {
    uint8_t ram[0x4000];
} ctx_t;
static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)p)->ram[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    *v = ((uint16_t)c->ram[a] << 8) | c->ram[a + 1];
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
static void word(ctx_t *c, uint32_t a, uint16_t v) {
    c->ram[a] = v >> 8;
    c->ram[a + 1] = v;
}
static void lng(ctx_t *c, uint32_t a, uint32_t v) {
    c->ram[a] = v >> 24;
    c->ram[a + 1] = v >> 16;
    c->ram[a + 2] = v >> 8;
    c->ram[a + 3] = v;
}
static mc68k_state_t *make_cpu_model(ctx_t *c, mc68k_model_t model) {
    mc68k_bus_t b = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, c};
    lng(c, 0, 0x3000);
    lng(c, 4, 0x100);
    mc68k_state_t *cpu = mc68k_create_model(&b, model);
    mc68k_reset(cpu);
    return cpu;
}

static mc68k_state_t *make_cpu(ctx_t *c) {
    return make_cpu_model(c, MC68K_MODEL_68020);
}

int main(void) {
    ctx_t c;
    memset(&c, 0, sizeof c);
    mc68k_state_t *cpu = make_cpu(&c);
    /* MULU.L #2,D0 and MULS.L #(-3),D1:D0 (64-bit result). */
    word(&c, 0x100, 0x4C3C);
    word(&c, 0x102, 0x0000);
    lng(&c, 0x104, 2);
    word(&c, 0x108, 0x4C3C);
    word(&c, 0x10a, 0x0C01);
    lng(&c, 0x10c, UINT32_C(0xfffffffd));
    cpu->d[0] = 3;
    mc68k_execute(cpu, 2);
    assert(cpu->d[0] == 6 && cpu->d[1] == 0);

    /* DIVS.L #2,D1:D0 exercises a negative 64-bit dividend. */
    cpu->pc = 0x120;
    word(&c, 0x120, 0x4C7C);
    word(&c, 0x122, 0x0C01);
    lng(&c, 0x124, 2);
    cpu->d[0] = UINT32_C(0xfffffffc);
    cpu->d[1] = UINT32_MAX;
    mc68k_execute(cpu, 1);
    assert(cpu->d[0] == UINT32_C(0xfffffffe) && cpu->d[1] == 0);

    /* With Dr == Dq, DIVUL leaves only the quotient. */
    cpu->pc = 0x130;
    word(&c, 0x130, 0x4C7C);
    word(&c, 0x132, 0);
    lng(&c, 0x134, 3);
    cpu->d[0] = 10;
    mc68k_execute(cpu, 1);
    assert(cpu->d[0] == 3);

    cpu->pc = 0x140;
    word(&c, 0x140, 0x49C0);
    cpu->d[0] = 0x80;
    mc68k_execute(cpu, 1);
    assert(cpu->d[0] == UINT32_C(0xffffff80));

    /* PACK/UNPK register and predecrement-memory forms. */
    cpu->pc = 0x150;
    word(&c, 0x150, 0x8140);
    word(&c, 0x152, 0);
    cpu->d[0] = 0x1234;
    mc68k_execute(cpu, 1);
    assert((cpu->d[0] & 0xff) == 0x24);
    cpu->pc = 0x154;
    word(&c, 0x154, 0x8180);
    word(&c, 0x156, 0);
    mc68k_execute(cpu, 1);
    assert((cpu->d[0] & 0xffff) == 0x0204);
    cpu->pc = 0x160;
    word(&c, 0x160, 0x8348);
    word(&c, 0x162, 0);
    cpu->a[0] = 0x202;
    cpu->a[1] = 0x300;
    c.ram[0x200] = 2;
    c.ram[0x201] = 4;
    mc68k_execute(cpu, 1);
    assert(cpu->a[0] == 0x200 && cpu->a[1] == 0x2ff && c.ram[0x2ff] == 0x24);
    cpu->pc = 0x164;
    word(&c, 0x164, 0x8388);
    word(&c, 0x166, 0);
    cpu->a[0] = 0x201;
    cpu->a[1] = 0x300;
    c.ram[0x200] = 0x24;
    mc68k_execute(cpu, 1);
    assert(cpu->a[0] == 0x200 && cpu->a[1] == 0x2fe && c.ram[0x2fe] == 2 && c.ram[0x2ff] == 4);
    mc68k_destroy(cpu);

    /* MULU.L #2,D0: Table 10-6 long column = calculate 1 + execute 20. */
    memset(&c, 0, sizeof c);
    cpu = make_cpu_model(&c, MC68K_MODEL_68040);
    word(&c, 0x100, 0x4C3C);
    word(&c, 0x102, 0x0000);
    lng(&c, 0x104, 2);
    cpu->d[0] = 3;
    assert(mc68k_execute(cpu, 1) == 21);
    assert(cpu->d[0] == 6);
    mc68k_destroy(cpu);
    return 0;
}
