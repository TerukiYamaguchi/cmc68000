/* MC68030 PMMU faults: every failure reason reaches vector 2, the format
 * $A/$B frame describes the faulting bus cycle, and RTE restarts it. */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>

typedef struct {
    unsigned char m[0x2000];
    uint32_t fail_read_at, watch_write;
    unsigned watch_write_hits;
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
    if (a == c->fail_read_at)
        return false;
    *v = ((uint32_t)c->m[a] << 24) | ((uint32_t)c->m[a + 1] << 16) |
         ((uint32_t)c->m[a + 2] << 8) | c->m[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    if (a == c->watch_write)
        c->watch_write_hits++;
    c->m[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    if (a == c->watch_write)
        c->watch_write_hits++;
    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    if (a == c->watch_write)
        c->watch_write_hits++;
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
    return true;
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
static uint32_t gl(ctx_t *c, unsigned a) {
    uint32_t v = 0;
    (void)rl(c, a, &v);
    return v;
}
static uint16_t gw(ctx_t *c, unsigned a) {
    return (uint16_t)((c->m[a] << 8) | c->m[a + 1]);
}

/* Long-format root: A[0] maps the low 4 MiB onto itself so that code, vectors
 * and the stack stay reachable, and A[1] points at a short leaf table whose
 * entry B[3] covers the logical page under test. */
#define FAULT_VA      0x00403010u
#define LEAF_PHYSICAL 0x00001000u

static void map(ctx_t *c, uint32_t a1_attributes, uint32_t leaf) {
    pl(c, 0x400, 0x00000009);
    pl(c, 0x404, 0x00000000); /* A[0]: identity page */
    pl(c, 0x408, a1_attributes);
    pl(c, 0x40c, 0x00000800); /* A[1]: short B table */
    pl(c, 0x80c, leaf);
}

static void rearm(mc68k_state_t *cpu) {
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    cpu->exec_state = CPU_NORMAL;
    cpu->group0_fault_latched = false;
    cpu->a[7] = 0x1f00;
}

int main(void) {
    ctx_t c = {.fail_read_at = UINT32_MAX, .watch_write = UINT32_MAX};
    pl(&c, 0, 0x1f00);
    pl(&c, 4, 0x100);
    pl(&c, 2 * 4, 0x300);
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
    mc68k_reset(cpu);
    cpu->mmu_tc = 0x82c0aa00; /* E, 4 KiB, IS=0, TIA=10, TIB=10 */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8000000300000400);

    const mc68k_access_t read_super = {.fc = 5, .read = true, .size = 4};
    const mc68k_access_t write_super = {.fc = 5, .read = false, .size = 4};
    const mc68k_access_t read_user = {.fc = 1, .read = true, .size = 4};

    /* Every failure reason becomes a vector 2 bus error whose fault address is
     * the untranslated logical address. */
    map(&c, 0x0000000a, LEAF_PHYSICAL | 0x0du); /* leaf page is write protected */
    rearm(cpu);
    cpu_write_long_access(cpu, FAULT_VA, 0x12345678, &write_super);
    assert(cpu->exec_state == CPU_EXCEPTION && cpu->fault_vector == 2 &&
           cpu->addr_err_addr == FAULT_VA && !cpu->addr_err_read);
    map(&c, 0x0000010a, LEAF_PHYSICAL | 9u); /* A[1] is supervisor only */
    rearm(cpu);
    (void)cpu_read_long_access(cpu, FAULT_VA, &read_user);
    assert(cpu->exec_state == CPU_EXCEPTION && cpu->fault_vector == 2 &&
           cpu->addr_err_addr == FAULT_VA && cpu->addr_err_read);
    map(&c, 0x0000000a, LEAF_PHYSICAL | 9u);
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8002000300000400); /* lower LIMIT 2 */
    rearm(cpu);
    (void)cpu_read_long_access(cpu, FAULT_VA, &read_super);
    assert(cpu->exec_state == CPU_EXCEPTION && cpu->fault_vector == 2 &&
           cpu->addr_err_addr == FAULT_VA);
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8000000300000400);
    c.fail_read_at = 0x80c;
    rearm(cpu);
    (void)cpu_read_long_access(cpu, FAULT_VA, &read_super);
    assert(cpu->exec_state == CPU_EXCEPTION && cpu->fault_vector == 2 &&
           cpu->addr_err_addr == FAULT_VA);
    c.fail_read_at = UINT32_MAX;

    /* A translation that fails must not put the target access on the host bus,
     * even where the walk did resolve a physical address. */
    map(&c, 0x0000000a, LEAF_PHYSICAL | 0x0du); /* write protected */
    rearm(cpu);
    c.watch_write = LEAF_PHYSICAL + (FAULT_VA & 0xfff);
    c.watch_write_hits = 0;
    cpu_write_long_access(cpu, FAULT_VA, 0x12345678, &write_super);
    assert(cpu->exec_state == CPU_EXCEPTION && c.watch_write_hits == 0);
    c.watch_write = UINT32_MAX;

    /* SSW describes the faulting bus cycle: DF, RM, RW, SIZE and FC.  A data
     * fault takes the long ($B) frame.  BTST only reads its operand, so it is
     * a plain read without RM: a guest that sees RM treats the fault as a
     * write (NetBSD WRFAULT_2030) and refuses a read-only page ().
     * ADDQ and the static BSET form are ordinary read-modify-write
     * instructions: like BTST, the fault here lands on their read cycle, so
     * they must show RM=0 too.  Only TAS/CAS/CAS2 use a genuinely locked bus
     * cycle (RM=1), which is why TAS and CAS keep RM set (). */
    static const struct {
        uint16_t opcode, ext, ssw;
    } cycles[] = {
        {0x2010, 0x0000, 0x0145}, /* MOVE.L (A0),D0    : data read, long          */
        {0x1080, 0x0000, 0x0115}, /* MOVE.B D0,(A0)    : data write, byte         */
        {0x4ad0, 0x0000, 0x01d5}, /* TAS (A0)          : locked RMW, byte         */
        {0x3010, 0x0000, 0x0165}, /* MOVE.W (A0),D0    : data read, word          */
        {0x0310, 0x0000, 0x0155}, /* BTST D1,(A0)      : data read, byte          */
        {0x0810, 0x0002, 0x0155}, /* BTST #2,(A0)      : data read, byte          */
        {0x5210, 0x0000, 0x0155}, /* ADDQ.B #1,(A0)    : RMW read cycle, byte     */
        {0x08d0, 0x0000, 0x0155}, /* BSET #0,(A0)      : RMW read cycle, byte     */
        {0x0ad0, 0x0000, 0x01d5}, /* CAS.B D0,D0,(A0)  : locked RMW, byte         */
    };
    map(&c, 0x0000000a, 0); /* B[3] invalid */
    for (unsigned i = 0; i < sizeof cycles / sizeof cycles[0]; ++i) {
        rearm(cpu);
        cpu->a[0] = FAULT_VA;
        cpu->pc = 0x100;
        pw(&c, 0x100, cycles[i].opcode);
        pw(&c, 0x102, cycles[i].ext);
        mc68k_execute(cpu, 1);
        assert(cpu->pc == 0x300 && cpu->last_exception_vector == 2);
        uint32_t frame = cpu->a[7];
        assert(frame == 0x1f00 - 92);
        assert(gw(&c, frame + 0x06) == 0xb008);
        assert(gw(&c, frame + 0x0a) == cycles[i].ssw);
        assert(gl(&c, frame + 0x02) == 0x100);
        assert(gl(&c, frame + 0x10) == FAULT_VA);
        assert(gl(&c, frame + 0x24) == FAULT_VA + 2); /* stage B address */
    }

    /* The read half of ADDQ.B above shares its fault signature with BTST
     * because the whole page is invalid there.  A write-protected (but
     * readable) page instead lets the read succeed and faults only the
     * write half, which is the case that exposed : since ADDQ is
     * not a locked bus cycle, that write-cycle fault must show RM=0, RW=0
     * (write) just like a plain MOVE.B write, not RM=1 like TAS/CAS. */
    map(&c, 0x0000000a, LEAF_PHYSICAL | 0x0du); /* write protected, still readable */
    rearm(cpu);
    cpu->a[0] = FAULT_VA;
    cpu->pc = 0x100;
    pw(&c, 0x100, 0x5210); /* ADDQ.B #1,(A0) */
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 2);
    {
        uint32_t frame = cpu->a[7];
        assert(frame == 0x1f00 - 92);
        assert(gw(&c, frame + 0x06) == 0xb008);
        assert(gw(&c, frame + 0x0a) == 0x0115); /* RM=0, RW=0 (write), byte */
        assert(gl(&c, frame + 0x10) == FAULT_VA);
    }
    map(&c, 0x0000000a, 0); /* B[3] invalid again, as the tests below assume */

    /* An instruction fetch takes the short ($A) frame and reports program
     * space, but leaves FC/FB clear: NetBSD's busaddrerr2030 (Lbe0) reads the
     * PC field for a short frame and adds 2 only when FC is set, and the true
     * fault address here already equals that PC (the instruction's own
     * start), so setting FC would push a fault VA that is off by +2
     * (). */
    rearm(cpu);
    cpu->pc = 0x00404000; /* B[4] is invalid */
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 2);
    assert(cpu->a[7] == 0x1f00 - 32);
    assert(gw(&c, cpu->a[7] + 0x06) == 0xa008);
    assert(gw(&c, cpu->a[7] + 0x0a) == 0x0066);
    assert(gl(&c, cpu->a[7] + 0x10) == 0x00404000);

    /* An extension-word fault (the displacement/immediate fetch after a
     * successful opcode fetch) always takes the long ($B) frame and must set
     * FC/RC: NetBSD's Lbe4 path uses the stage-B address (base+0x24) as fault
     * VA and subtracts 2 from it only when FC is set.  Stage-B is always
     * pushed as addr_err_addr+2 (the word after the one that faulted), so FC
     * must be set here for that -2 to land back on the true fault address
     * ().
     *
     * With TIA=TIB=10 and 4 KiB pages (§ map() above), logical bits 31-22
     * select the A-table index and bits 21-12 the B-table index, so B[2]
     * covers 0x00402000-0x00402fff and B[3] (still invalid from the loop
     * above) covers 0x00403000-0x00403fff.  Put the opcode word at the last
     * two bytes of B[2]'s page so the opcode fetch succeeds, and let the
     * (unwritten) extension word fetch fault at 0x00403000, the first byte
     * of B[3]. */
    pl(&c, 0x800 + 2 * 4, LEAF_PHYSICAL | 9u); /* B[2]: valid, backs the opcode word */
    pw(&c, LEAF_PHYSICAL + 0xffe, 0x203c);     /* MOVE.L #imm,D0 at phys 0x1ffe */
    rearm(cpu);
    cpu->pc = 0x00402ffe;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 2);
    assert(cpu->a[7] == 0x1f00 - 92);
    assert(gw(&c, cpu->a[7] + 0x06) == 0xb008);
    assert(gw(&c, cpu->a[7] + 0x0a) == 0xa066);
    assert(gl(&c, cpu->a[7] + 0x02) == 0x00402ffe);
    assert(gl(&c, cpu->a[7] + 0x10) == 0x00403000);
    assert(gl(&c, cpu->a[7] + 0x24) == 0x00403000 + 2);

    /* RTE resumes at the start of the faulting instruction, which then runs to
     * completion once the mapping is repaired. */
    rearm(cpu);
    cpu->a[0] = FAULT_VA;
    cpu->pc = 0x100;
    pw(&c, 0x100, 0x2010); /* MOVE.L (A0),D0 */
    pw(&c, 0x300, 0x4e73); /* RTE */
    cpu->d[0] = 0;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x300 && gl(&c, cpu->a[7] + 0x02) == 0x100);
    map(&c, 0x0000000a, LEAF_PHYSICAL | 9u);
    pl(&c, LEAF_PHYSICAL + (FAULT_VA & 0xfff), 0xcafebabe);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x100 && cpu->a[7] == 0x1f00);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x102 && cpu->d[0] == 0xcafebabe);

    /* A stack that cannot be translated turns the fault into a halt, and the
     * original fault information survives the failed frame push. */
    map(&c, 0x0000000a, 0);
    rearm(cpu);
    cpu->a[7] = 0x00404000; /* B[4] is invalid, so the frame cannot be pushed */
    cpu->a[0] = FAULT_VA;
    cpu->pc = 0x100;
    mc68k_execute(cpu, 1);
    assert(cpu->exec_state == CPU_HALTED && cpu->addr_err_addr == FAULT_VA &&
           cpu->fault_vector == 2);

    mc68k_destroy(cpu);
    return 0;
}
