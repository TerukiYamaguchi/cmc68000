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
static mc68k_state_t *make_cpu(ctx_t *c) {
    mc68k_bus_t b = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, c};
    word(c, 0, 0x3000);
    word(c, 4, 0);
    mc68k_state_t *cpu = mc68k_create_model(&b, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->pc = 0x100;
    return cpu;
}
static void run(mc68k_state_t *cpu, ctx_t *c, uint16_t op, uint16_t ext) {
    word(c, 0x100, op);
    word(c, 0x102, ext);
    mc68k_execute(cpu, 1);
}

int main(void) {
    ctx_t c;
    memset(&c, 0, sizeof c);
    mc68k_state_t *cpu = make_cpu(&c);
    /* BFEXTU D0{28:8},D1: register fields wrap at bit 31. */
    cpu->d[0] = 0x80000001;
    run(cpu, &c, 0xE9C0, 0x1708);
    assert(cpu->d[1] == 0x18);

    memset(&c.ram[0x100], 0, 8);
    cpu->pc = 0x100;
    cpu->d[0] = 0x80000000;
    run(cpu, &c, 0xECC0, 0x0001); /* BFCLR D0{0:1}: CCR uses the old field. */
    assert((cpu->sr & (CCR_N | CCR_Z)) == CCR_N);
    assert(cpu->d[0] == 0);

    memset(&c.ram[0x100], 0, 8);
    cpu->pc = 0x100;
    cpu->d[0] = 0;
    run(cpu, &c, 0xEEC0, 0x0001); /* BFSET D0{0:1}: old field was zero. */
    assert((cpu->sr & (CCR_N | CCR_Z)) == CCR_Z);

    memset(&c.ram[0x100], 0, 8);
    cpu->pc = 0x100;
    cpu->d[0] = 0xFFFFFFFF;
    cpu->d[1] = 0;
    run(cpu, &c, 0xEFC0, 0x1004); /* BFINS uses the inserted value for CCR. */
    assert((cpu->sr & (CCR_N | CCR_Z)) == CCR_Z);

    memset(&c.ram[0x100], 0, 8);
    cpu->pc = 0x100;
    cpu->a[0] = 0x200;
    cpu->d[0] = UINT32_MAX;
    c.ram[0x1ff] = 1;
    c.ram[0x200] = 0;
    run(cpu, &c, 0xE8D0, 0x0801); /* BFTST (A0){D0:1}; D0=-1 addresses A0-1. */
    assert((cpu->sr & (CCR_N | CCR_Z)) == CCR_N);

    /* Remaining BFxxx operations, including dynamic width and memory crossing. */
    memset(&c.ram[0x100], 0, 8);
    cpu->pc = 0x100;
    cpu->d[0] = UINT32_C(0x80000000);
    run(cpu, &c, 0xEBC0, 0x1001);
    assert(cpu->d[1] == UINT32_MAX); /* BFEXTS D0{0:1},D1 */
    cpu->pc = 0x100;
    cpu->d[0] = UINT32_C(0x60000000);
    run(cpu, &c, 0xEDC0, 0x1003);
    assert(cpu->d[1] == 1); /* BFFFO */
    cpu->pc = 0x100;
    cpu->d[0] = UINT32_C(0x80000000);
    run(cpu, &c, 0xEAC0, 0x0001);
    assert(cpu->d[0] == 0); /* BFCHG */
    cpu->pc = 0x100;
    cpu->d[0] = UINT32_MAX;
    run(cpu, &c, 0xE9C0, 0x1000);
    assert(cpu->d[1] == UINT32_MAX); /* width 0 = 32 */
    cpu->pc = 0x100;
    cpu->a[0] = 0x240;
    cpu->d[2] = 4;
    cpu->d[3] = 8;
    c.ram[0x240] = 0x0f;
    c.ram[0x241] = 0xf0;
    run(cpu, &c, 0xE9D0, 0x18a3);
    assert(cpu->d[1] == 0xff); /* dynamic {D2:D3}, crossing bytes */
    /* EA が Dn のビットフィールド命令は D0 以外の全レジスタで有効。
     * NetBSD/x68k の boot_ufs が BFEXTU D2{8:24},D2 を実行する。 */
    memset(&c.ram[0x100], 0, 8);
    cpu->pc = 0x100;
    cpu->d[2] = 0x12345678;
    run(cpu, &c, 0xE9C2, 0x2218); /* BFEXTU D2{8:24},D2 */
    assert(cpu->pc == 0x104 && cpu->d[2] == 0x345678);
    cpu->pc = 0x100;
    cpu->d[1] = 0x0f;
    cpu->d[3] = 0;
    run(cpu, &c, 0xEFC3, 0x1004); /* BFINS D1,D3{0:4} */
    assert(cpu->pc == 0x104 && cpu->d[3] == 0xf0000000);
    /* BFINS D0,132(A2){5:1}: メモリオペランドの EA 拡張ワードを読み出しと
     * 書き戻しで2回フェッチすると PC が2バイト余分に進み、次の命令を
     * 丸ごと取りこぼす。NetBSD/x68k の ld.elf_so はこの直後の TST.L A6 /
     * BEQ で NULL 判定をしており、取りこぼすと NULL 参照でループする。 */
    memset(&c.ram[0x100], 0, 16);
    cpu->pc = 0x100;
    cpu->a[2] = 0x300;
    cpu->a[6] = 0;
    cpu->d[0] = 1;
    c.ram[0x384] = 0;
    word(&c, 0x100, 0xEFEA); /* BFINS D0,132(A2){5:1} */
    word(&c, 0x102, 0x0141);
    word(&c, 0x104, 0x0084);
    word(&c, 0x106, 0x4A8E); /* TST.L A6 */
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x106);
    assert(c.ram[0x384] == 0x04); /* オフセット5・幅1のビットが立つ */
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x108 && (cpu->sr & CCR_Z));
    mc68k_destroy(cpu);
    return 0;
}
