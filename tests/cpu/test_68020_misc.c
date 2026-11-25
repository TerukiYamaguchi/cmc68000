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
static void putl(ctx_t *c, unsigned a, uint32_t v) {
    c->m[a] = v >> 24;
    c->m[a + 1] = v >> 16;
    c->m[a + 2] = v >> 8;
    c->m[a + 3] = v;
}
static void put_word(ctx_t *c, unsigned a, uint16_t v) {
    c->m[a] = v >> 8;
    c->m[a + 1] = v;
}
static uint32_t getl(const ctx_t *c, unsigned a) {
    return ((uint32_t)c->m[a] << 24) | ((uint32_t)c->m[a + 1] << 16) |
           ((uint32_t)c->m[a + 2] << 8) | c->m[a + 3];
}
static void test_movem_predec_model_difference(void) {
    for (int model = MC68K_MODEL_68000; model <= MC68K_MODEL_68020; model += 2) {
        ctx_t c = {0};
        putl(&c, 0, 0x300);
        putl(&c, 4, 0x100);
        put_word(&c, 0x100, 0x48e7);
        put_word(&c, 0x102, 0x8001);
        mc68k_bus_t b = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
        mc68k_state_t *x = mc68k_create_model(&b, model);
        mc68k_reset(x);
        mc68k_execute(x, 1);
        assert(getl(&c, 0x2fc) == (model == MC68K_MODEL_68020 ? 0x2fcu : 0x300u));
        mc68k_destroy(x);
    }
}
int main(void) {
    static ctx_t c;
    memset(&c, 0, sizeof c);
    putl(&c, 0, 0x3000);
    putl(&c, 4, 0x100);
    put_word(&c, 0x100, 0x49c0);
    put_word(&c, 0x102, 0x60ff);
    putl(&c, 0x104, 4);
    put_word(&c, 0x108, 0xe9c0);
    put_word(&c, 0x10a, 0x1708);
    mc68k_bus_t b = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *x = mc68k_create_model(&b, MC68K_MODEL_68020);
    mc68k_reset(x);
    x->d[0] = 0x00000080;
    /* EXT.B.L=4, Bcc.L taken=6, BFTST Dn=6 (UM20 §8.2.12/15/14). */
    mc68k_execute(x, 16);
    mc68k_regs_t r;
    mc68k_get_regs(x, &r);
    assert(r.d[0] == 0xffffff80u);
    assert(r.d[1] == 0x0f);
    assert(r.pc == 0x10c);
    /* CAS succeeds only on equality; otherwise it returns the memory operand in Dc. */
    x->pc = 0x120;
    x->a[0] = 0x200;
    x->d[0] = 5;
    x->d[1] = 9;
    put_word(&c, 0x120, 0x0cd0);
    put_word(&c, 0x122, 0x0040);
    put_word(&c, 0x200, 5);
    mc68k_execute(x, 1);
    assert(c.m[0x200 + 1] == 9 && (x->sr & CCR_Z));
    x->pc = 0x128;
    x->d[0] = 5;
    put_word(&c, 0x128, 0x0cd0);
    put_word(&c, 0x12a, 0x0040);
    put_word(&c, 0x200, 7);
    mc68k_execute(x, 1);
    assert(x->d[0] == 7 && c.m[0x200 + 1] == 7 && !(x->sr & CCR_Z));
    /* CAS2 failure updates both compare registers, except equal Dc keeps operand 1. */
    x->pc = 0x130;
    x->a[0] = 0x210;
    x->a[1] = 0x220;
    x->d[0] = 7;
    x->d[2] = 3;
    put_word(&c, 0x130, 0x0cfc);
    put_word(&c, 0x132, 0x8040);
    put_word(&c, 0x134, 0x9082);
    put_word(&c, 0x210, 7);
    put_word(&c, 0x220, 4);
    mc68k_execute(x, 1);
    assert(x->d[0] == 7 && x->d[2] == 4 && c.m[0x220 + 1] == 4);
    x->pc = 0x140;
    x->d[0] = 0;
    put_word(&c, 0x140, 0x0cfc);
    put_word(&c, 0x142, 0x8040);
    put_word(&c, 0x144, 0x9000);
    put_word(&c, 0x210, 7);
    put_word(&c, 0x220, 4);
    mc68k_execute(x, 1);
    assert(x->d[0] == 7);
    /* CHK2/CMP2, long branches, LINK.L, TRAPcc operand skipping and EA extensions. */
    x->pc = 0x150;
    x->a[0] = 0x240;
    x->d[0] = 5;
    put_word(&c, 0x150, 0x00d0);
    put_word(&c, 0x152, 0x0000);
    c.m[0x240] = 3;
    c.m[0x241] = 5;
    mc68k_execute(x, 1);
    assert(x->sr & CCR_Z);
    x->pc = 0x154;
    x->d[0] = 7;
    put_word(&c, 0x154, 0x00d0);
    put_word(&c, 0x156, 0x0000);
    mc68k_execute(x, 1);
    assert(x->sr & CCR_C);
    x->pc = 0x160;
    put_word(&c, 0x160, 0x57fa);
    put_word(&c, 0x162, 0xbeef);
    mc68k_execute(x, 1);
    assert(x->pc == 0x164); /* TRAPEQ.W false */
    x->pc = 0x170;
    put_word(&c, 0x170, 0x60ff);
    putl(&c, 0x172, 2);
    mc68k_execute(x, 1);
    assert(x->pc == 0x174);
    x->pc = 0x180;
    x->a[0] = 0x280;
    put_word(&c, 0x180, 0x4808);
    putl(&c, 0x182, UINT32_C(0xfffffff0));
    mc68k_execute(x, 1);
    assert(x->a[0] == x->a[7] + 16);
    x->pc = 0x190;
    put_word(&c, 0x190, 0x4a7a);
    put_word(&c, 0x192, 2);
    put_word(&c, 0x194, 1);
    mc68k_execute(x, 1);
    assert(!(x->sr & CCR_Z));
    x->pc = 0x1a0;
    put_word(&c, 0x1a0, 0x0c7a);
    put_word(&c, 0x1a2, 0);
    put_word(&c, 0x1a4, 2);
    put_word(&c, 0x1a6, 0);
    mc68k_execute(x, 1);
    assert(x->sr & CCR_Z);
    /* CACR retains only F/E; C/CE and all reserved bits read as zero. */
    x->pc = 0x1b0;
    x->d[0] = UINT32_MAX;
    put_word(&c, 0x1b0, 0x4e7b);
    put_word(&c, 0x1b2, 0x0002);
    mc68k_execute(x, 1);
    x->pc = 0x1b4;
    put_word(&c, 0x1b4, 0x4e7a);
    put_word(&c, 0x1b6, 0x1002);
    mc68k_execute(x, 1);
    assert(x->d[1] == 3);
    /* BKPT has no responder on this bus and therefore becomes ILLEGAL. */
    putl(&c, 0x10, 0x1e0);
    x->pc = 0x1c0;
    put_word(&c, 0x1c0, 0x4848);
    mc68k_execute(x, 1);
    assert(x->pc == 0x1e0);
    /* 68020 以降の TST はワード/ロングで An を取れる（バイトは不可のまま）。
     * NetBSD/x68k の boot_ufs が TST.L A2 を実行する。 */
    x->pc = 0x1f0;
    x->a[2] = 0;
    put_word(&c, 0x1f0, 0x4a8a); /* TST.L A2 */
    mc68k_execute(x, 1);
    assert(x->pc == 0x1f2 && (x->sr & CCR_Z));
    x->pc = 0x1f4;
    x->a[2] = 0xffff8000;
    put_word(&c, 0x1f4, 0x4a4a); /* TST.W A2 */
    mc68k_execute(x, 1);
    assert(x->pc == 0x1f6 && (x->sr & CCR_N) && !(x->sr & CCR_Z));
    x->pc = 0x1f8;
    put_word(&c, 0x1f8, 0x4a0a); /* TST.B A2 は 68020 でも ILLEGAL */
    mc68k_execute(x, 1);
    assert(x->pc == 0x1e0);
    mc68k_destroy(x);
    test_movem_predec_model_difference();

    /* CMP2.B (A0),D0: 040UM Table 10-6 gives calculate 13 + execute 11. */
    memset(&c, 0, sizeof c);
    putl(&c, 0, 0x3000);
    putl(&c, 4, 0x100);
    mc68k_bus_t b40 = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    x = mc68k_create_model(&b40, MC68K_MODEL_68040);
    mc68k_reset(x);
    x->a[0] = 0x240;
    x->d[0] = 5;
    put_word(&c, 0x100, 0x00d0);
    put_word(&c, 0x102, 0x0000);
    c.m[0x240] = 3;
    c.m[0x241] = 5;
    assert(mc68k_execute(x, 1) == 24);
    assert(x->sr & CCR_Z);
    mc68k_destroy(x);

    /* BFTST D0{8:8}: 040UM Table 10-6 Dn/T1 = calculate 1 + execute 3. */
    memset(&c, 0, sizeof c);
    putl(&c, 0, 0x3000);
    putl(&c, 4, 0x100);
    x = mc68k_create_model(&b40, MC68K_MODEL_68040);
    mc68k_reset(x);
    put_word(&c, 0x100, 0xe8c0);
    put_word(&c, 0x102, 0x0208);
    x->d[0] = 0;
    assert(mc68k_execute(x, 1) == 4);
    assert(x->sr & CCR_Z);
    mc68k_destroy(x);

    /* BFTST (A0){8:8}: 040UM Table 10-6 gives calculate 9 + execute 7. */
    x = mc68k_create_model(&b40, MC68K_MODEL_68040);
    mc68k_reset(x);
    x->a[0] = 0x240;
    put_word(&c, 0x100, 0xe8d0);
    put_word(&c, 0x102, 0x0208);
    assert(mc68k_execute(x, 1) == 16);
    assert(x->sr & CCR_Z);
    mc68k_destroy(x);

    /* MOVEM.L D0,(A0): D'=1,A'=0 => (2+1)+(1+1) = 5. */
    memset(&c, 0, sizeof c);
    putl(&c, 0, 0x3000);
    putl(&c, 4, 0x100);
    x = mc68k_create_model(&b40, MC68K_MODEL_68040);
    mc68k_reset(x);
    x->a[0] = 0x240;
    x->d[0] = UINT32_C(0x12345678);
    put_word(&c, 0x100, 0x48d0);
    put_word(&c, 0x102, 0x0001);
    assert(mc68k_execute(x, 1) == 5);
    assert(getl(&c, 0x240) == UINT32_C(0x12345678));
    mc68k_destroy(x);

    /* MOVEM.L (A0),D0: D'=1,A'=0 => (3+1)+(2+1) = 7. */
    x = mc68k_create_model(&b40, MC68K_MODEL_68040);
    mc68k_reset(x);
    x->a[0] = 0x240;
    put_word(&c, 0x100, 0x4cd0);
    put_word(&c, 0x102, 0x0001);
    assert(mc68k_execute(x, 1) == 7);
    assert(x->d[0] == UINT32_C(0x12345678));
    mc68k_destroy(x);

    /* MOVES.L (A0),D0: Table 10-6 gives calculate 20 + execute base 19. */
    x = mc68k_create_model(&b40, MC68K_MODEL_68040);
    mc68k_reset(x);
    x->a[0] = 0x240;
    put_word(&c, 0x100, 0x0e90);
    put_word(&c, 0x102, 0x0000);
    assert(mc68k_execute(x, 1) == 39);
    assert(x->d[0] == UINT32_C(0x12345678));
    mc68k_destroy(x);
    return 0;
}
