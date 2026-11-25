/* MC68030 ATC control regressions (x68000 ).
 * All flush and PMOVE operations below execute their guest encodings. */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>

#define RAM_SIZE    0x2000u
#define OLD_TC      UINT32_C(0x82c0aa00) /* E, 4 KiB, A=10, B=10 */
#define NEW_TC      UINT32_C(0x80d09a00) /* E, 8 KiB, A=9, B=10 */
#define A_TABLE     0x400u
#define OLD_B_TABLE 0x800u
#define NEW_B_TABLE 0xa00u
#define TARGET      0x00403010u

typedef struct {
    uint8_t mem[RAM_SIZE];
    unsigned long_reads;
} ctx_t;

static bool rb(void *p, uint32_t a, uint8_t *v) {
    ctx_t *c = p;
    if (a >= RAM_SIZE)
        return false;
    *v = c->mem[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    *v = (uint16_t)((c->mem[a] << 8) | c->mem[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    c->long_reads++;
    if (a + 3 >= RAM_SIZE)
        return false;
    *v = ((uint32_t)c->mem[a] << 24) | ((uint32_t)c->mem[a + 1] << 16) |
         ((uint32_t)c->mem[a + 2] << 8) | c->mem[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    if (a >= RAM_SIZE)
        return false;
    c->mem[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    c->mem[a] = (uint8_t)(v >> 8);
    c->mem[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    return ww(p, a, (uint16_t)(v >> 16)) && ww(p, a + 2, (uint16_t)v);
}

static void pw(ctx_t *c, uint32_t a, uint16_t v) {
    if (!ww(c, a, v))
        abort();
}
static void pl(ctx_t *c, uint32_t a, uint32_t v) {
    if (!wl(c, a, v))
        abort();
}
static mc68k_bus_t make_bus(ctx_t *c) {
    return (mc68k_bus_t){rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, c};
}

static mc68k_translate_result_t translate(mc68k_state_t *cpu) {
    const mc68k_access_t access = {.fc = 5, .read = true, .size = 4};
    return cpu_mmu_translate_result(cpu, TARGET, &access, MC68K_TRANSLATE_EXECUTE);
}
static unsigned atc_valid_count(const mc68k_state_t *cpu) {
    unsigned n = 0;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        n += cpu->atc[i].valid;
    return n;
}

static void install_old_map(ctx_t *c) {
    /* A[0]/B[0] identity-map test code and PMOVE data.  A[1]/B[3] maps TARGET. */
    pl(c, A_TABLE + 0 * 4, OLD_B_TABLE | 2u);
    pl(c, A_TABLE + 1 * 4, OLD_B_TABLE | 2u);
    pl(c, OLD_B_TABLE + 0 * 4, 0x00000001u);
    pl(c, OLD_B_TABLE + 3 * 4, 0x0000c009u); /* U set: read-count is walk only */
}
static void install_new_map(ctx_t *c) {
    /* NEW_TC consumes a 9-bit A index and a 10-bit B index. */
    unsigned a = TARGET >> 23;
    unsigned b = (TARGET >> 13) & 0x3ffu;
    pl(c, A_TABLE + 0 * 4, NEW_B_TABLE | 2u);
    pl(c, A_TABLE + a * 4, NEW_B_TABLE | 2u);
    pl(c, NEW_B_TABLE + 0 * 4, 0x00000001u);
    pl(c, NEW_B_TABLE + b * 4, 0x0000e001u);
}
static void execute(mc68k_state_t *cpu, ctx_t *c, uint32_t pc, uint16_t opcode,
                    uint16_t extension) {
    pw(c, pc, opcode);
    pw(c, pc + 2, extension);
    cpu->pc = pc;
    mc68k_execute(cpu, 1);
    assert(cpu->exec_state == CPU_NORMAL);
}

int main(void) {
    ctx_t c = {0};
    mc68k_bus_t bus = make_bus(&c);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
    mc68k_reset(cpu);
    cpu->mmu_tc = OLD_TC;
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    install_old_map(&c);

    /* First access walks, then the same page hits even if its descriptor moves. */
    mc68k_translate_result_t r = translate(cpu);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.physical == 0x0000c010u);
    unsigned reads_after_walk = c.long_reads;
    r = translate(cpu);
    assert(r.ok && r.source == MC68K_TRANSLATE_ATC && c.long_reads == reads_after_walk);
    pl(&c, OLD_B_TABLE + 3 * 4, 0x0000d009u);
    r = translate(cpu);
    assert(r.ok && r.source == MC68K_TRANSLATE_ATC && r.physical == 0x0000c010u);

    /* PFLUSH #5,#7,(A0): matching FC and logical page only. */
    cpu->a[0] = TARGET;
    execute(cpu, &c, 0x100, 0xf010, 0x38f5);
    r = translate(cpu);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.physical == 0x0000d010u);

    /* A negative entry also survives table repair until guest PFLUSHA clears it. */
    execute(cpu, &c, 0x108, 0xf010, 0x38f5);
    pl(&c, OLD_B_TABLE + 3 * 4, 0);
    r = translate(cpu);
    assert(!r.ok && r.source == MC68K_TRANSLATE_WALK);
    pl(&c, OLD_B_TABLE + 3 * 4, 0x0000c009u);
    r = translate(cpu);
    assert(!r.ok && r.source == MC68K_TRANSLATE_ATC);
    execute(cpu, &c, 0x110, 0xf000, 0x2400);
    assert(atc_valid_count(cpu) == 0);
    r = translate(cpu);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.physical == 0x0000c010u);

    /* PMOVEFD TC preserves the ATC, but a changed page size cannot hit an old entry. */
    pl(&c, 0x900, NEW_TC);
    cpu->a[0] = 0x900;
    execute(cpu, &c, 0x118, 0xf010, 0x4100); /* PMOVEFD (A0),TC */
    assert(cpu->mmu_tc == NEW_TC && atc_valid_count(cpu) != 0);
    install_new_map(&c);
    r = translate(cpu);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.physical == 0x0000f010u);

    /* FD=0 flushes every entry when TC is loaded, including entries left by NEW_TC. */
    pl(&c, 0x900, OLD_TC);
    execute(cpu, &c, 0x120, 0xf010, 0x4000); /* PMOVE (A0),TC */
    assert(cpu->mmu_tc == OLD_TC && atc_valid_count(cpu) == 0);

    mc68k_destroy(cpu);
    return 0;
}
