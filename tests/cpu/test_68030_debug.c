/* The public debug API reads logical addresses through the PMMU while the
 * physical API bypasses it, and neither disturbs CPU or PMMU state. */
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <string.h>

#define RAM_SIZE 0x4000u

typedef struct {
    unsigned char m[RAM_SIZE];
} ctx_t;

static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = a < RAM_SIZE ? ((ctx_t *)p)->m[a] : 0xff;
    return a < RAM_SIZE;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    uint8_t hi = 0, lo = 0;
    bool ok = rb(p, a, &hi) && rb(p, a + 1, &lo);
    *v = (uint16_t)((hi << 8) | lo);
    return ok;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    uint16_t hi = 0, lo = 0;
    bool ok = rw(p, a, &hi) && rw(p, a + 2, &lo);
    *v = ((uint32_t)hi << 16) | lo;
    return ok;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    if (a < RAM_SIZE)
        ((ctx_t *)p)->m[a] = v;
    return a < RAM_SIZE;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    return wb(p, a, (uint8_t)(v >> 8)) & wb(p, a + 1, (uint8_t)v);
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    return ww(p, a, (uint16_t)(v >> 16)) & ww(p, a + 2, (uint16_t)v);
}
/* peek never signals a bus error: an unmapped physical address reads as $FF. */
static uint8_t peek_b(void *p, uint32_t a) {
    return a < RAM_SIZE ? ((ctx_t *)p)->m[a] : 0xff;
}
static uint16_t peek_w(void *p, uint32_t a) {
    return (uint16_t)((peek_b(p, a) << 8) | peek_b(p, a + 1));
}
static uint32_t peek_l(void *p, uint32_t a) {
    return ((uint32_t)peek_w(p, a) << 16) | peek_w(p, a + 2);
}
static void pw(ctx_t *c, unsigned a, uint16_t v) {
    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
}
static void pl(ctx_t *c, unsigned a, uint32_t v) {
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
}

int main(void) {
    ctx_t c = {0};
    pl(&c, 0, 0x0f00);
    pl(&c, 4, 0x100);
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
    mc68k_reset(cpu);
    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, &c);

    /* Long root at $400.  Logical $00000000-$00000fff is identity mapped so
     * that vectors and code stay reachable, logical $00001000 is deliberately
     * displaced to physical $3000, and logical $00403000 lands on physical
     * $1000 — the page that the displaced logical address vacated. */
    cpu->mmu_tc = 0x82c0aa00; /* E, 4 KiB, IS=0, TIA=10, TIB=10 */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8000000300000400);
    pl(&c, 0x400, 0x0000000a);
    pl(&c, 0x404, 0x00000900); /* A[0] -> table $900 */
    pl(&c, 0x408, 0x0000000a);
    pl(&c, 0x40c, 0x00000800); /* A[1] -> table $800 */
    pl(&c, 0x900, 0x00000009); /* logical $0000xxx -> physical $0000xxx */
    pl(&c, 0x904, 0x00003009); /* logical $0001xxx -> physical $3000 */
    pl(&c, 0x80c, 0x00001009); /* logical $0403xxx -> physical $1000 */
    pw(&c, 0x1000, 0x4e71);    /* NOP, at the physical address $1000 */
    pw(&c, 0x3000, 0x2200);    /* MOVE.L D0,D1, where logical $1000 really lives */

    /* D-1/D-2 are covered by test_68030_translate.c; here the point is that
     * the byte-level debug API agrees with the translation. */
    mc68k_debug_translate_result_t t;
    assert(mc68k_debug_translate(cpu, 0x1000, 5, 1, &t) == 0 && t.physical == 0x3000);
    assert(mc68k_debug_translate(cpu, 0x00403000, 5, 1, &t) == 0 && t.physical == 0x1000);

    /* mc68k_disassemble() reads the logical address, so it decodes the page
     * the MMU actually maps there rather than the identically numbered
     * physical page. */
    char text[64];
    assert(mc68k_disassemble(cpu, 0x1000, text, sizeof text) == 2);
    assert(strcmp(text, "MOVE.L D0,D1") == 0);
    assert(mc68k_disassemble(cpu, 0x00403000, text, sizeof text) == 2);
    assert(strcmp(text, "NOP") == 0);

    /* mc68k_debug_read_physical_bytes() performs no translation at all. */
    uint8_t buf[4];
    assert(mc68k_debug_read_physical_bytes(cpu, 0x1000, buf, 2) == 0 && buf[0] == 0x4e &&
           buf[1] == 0x71);
    assert(mc68k_debug_read_bytes(cpu, 0x1000, buf, 2) == 0 && buf[0] == 0x22 &&
           buf[1] == 0x00);

    /* A value watchpoint follows the logical address: a write to the page it
     * is mapped onto fires it, a write to the identically numbered physical
     * page does not. */
    int wp = mc68k_add_watchpoint(cpu, 0x1000, 2);
    assert(wp >= 0 && mc68k_watchpoint_check(cpu) < 0);
    pw(&c, 0x1000, 0x1234); /* physical $1000 is not what logical $1000 maps to */
    assert(mc68k_watchpoint_check(cpu) < 0);
    pw(&c, 0x3000, 0x4e71);
    assert(mc68k_watchpoint_check(cpu) == wp);
    uint32_t old_value = 0, new_value = 0;
    assert(mc68k_get_watchpoint_hit(cpu, &old_value, &new_value) == wp && old_value == 0x2200 &&
           new_value == 0x4e71);
    pw(&c, 0x1000, 0x4e71);

    /* An unmapped logical address fails without raising an exception, and the
     * buffer is left untouched from the failing byte onward. */
    memset(buf, 0xaa, sizeof buf);
    assert(mc68k_debug_read_bytes(cpu, 0x00404000, buf, 4) == -1);
    assert(buf[0] == 0xaa && buf[1] == 0xaa && buf[2] == 0xaa && buf[3] == 0xaa);
    pw(&c, 0x1ffe, 0x5566);
    memset(buf, 0xaa, sizeof buf);
    assert(mc68k_debug_read_bytes(cpu, 0x00403ffe, buf, 4) == -1);
    assert(buf[0] == 0x55 && buf[1] == 0x66 && buf[2] == 0xaa && buf[3] == 0xaa);
    assert(mc68k_debug_translate(cpu, 0x00404000, 5, 1, &t) == -1 &&
           t.fault == MC68K_DEBUG_MMU_INVALID);

    /* None of the debug paths above may disturb the CPU: no exception state,
     * no ATC entry, and SFC/DFC unchanged. */
    assert(cpu->exec_state == CPU_NORMAL && !cpu->group0_fault_latched);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        assert(!cpu->atc[i].valid);
    cpu->sfc = 3;
    cpu->dfc = 6;
    assert(mc68k_debug_translate(cpu, 0x00403000, 1, 0, &t) == 0);
    assert(mc68k_debug_read_bytes(cpu, 0x00403000, buf, 2) == 0);
    assert(mc68k_debug_read_physical_bytes(cpu, 0x1000, buf, 2) == 0);
    assert(cpu->sfc == 3 && cpu->dfc == 6);

    mc68k_destroy(cpu);
    return 0;
}
