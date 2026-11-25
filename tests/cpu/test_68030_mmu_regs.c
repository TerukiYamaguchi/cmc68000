#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stddef.h>

typedef struct {
    unsigned char m[0x1000];
    uint32_t fail_read_at, fail_write_at;
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
    if (a == c->fail_read_at)
        return false;
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
    if (a == c->fail_write_at)
        return false;
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

static uint32_t test_translate(mc68k_state_t *cpu, uint32_t logical, unsigned fc, bool read,
                               bool *ci) {
    const mc68k_access_t access = {.fc = (uint8_t)fc, .read = read, .size = 4};
    mc68k_translate_result_t result =
        cpu_mmu_translate_result(cpu, logical, &access, MC68K_TRANSLATE_EXECUTE);
    *ci = result.cache_inhibit;
    return result.physical;
}

int main(void) {
    ctx_t c = {.fail_read_at = UINT32_MAX, .fail_write_at = UINT32_MAX};
    pl(&c, 0, 0x800);
    pl(&c, 4, 0x100);
    pl(&c, 56 * 4, 0x200);
    pl(&c, 2 * 4, 0x260);
    mc68k_bus_t b = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&b, MC68K_MODEL_68030);
    mc68k_reset(cpu);
    /* PMOVE (A0),TT0; PMOVE (A0),TC; PMOVE (A0),CRP. */
    cpu->a[0] = 0x400;
    pl(&c, 0x400, 0xa5008250);
    pw(&c, 0x100, 0xf010);
    pw(&c, 0x102, 0x0800);
    mc68k_execute(cpu, 1);
    mc68k_mmu_regs_t m;
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.tt0 == 0xa5008250);
    pl(&c, 0x400, 0x02c0aa00);
    pw(&c, 0x104, 0xf010);
    pw(&c, 0x106, 0x4000);
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.tc == 0x02c0aa00);
    pl(&c, 0x400, 0x80000002);
    pl(&c, 0x404, 0x00100002);
    pw(&c, 0x108, 0xf010);
    pw(&c, 0x10a, 0x4c00);
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.crp == UINT64_C(0x8000000200100002));
    /* PMOVEFD accepts FD in bit 8 and leaves a pre-existing ATC entry alone. */
    cpu->atc[0].valid = true;
    pl(&c, 0x400, 0x02c0aa00);
    pw(&c, 0x10c, 0xf010);
    pw(&c, 0x10e, 0x4100);
    cpu->pc = 0x10c;
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.tc == 0x02c0aa00 && cpu->atc[0].valid);
    /* The root descriptor type is in the upper long. */
    pl(&c, 0x400, 0x00000000);
    pl(&c, 0x404, 0x00100002);
    pw(&c, 0x110, 0xf010);
    pw(&c, 0x112, 0x4c00);
    cpu->pc = 0x110;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x200);
    cpu->exec_state = CPU_NORMAL;
    pl(&c, 0x400, 0x00000002);
    pl(&c, 0x404, 0x00100000);
    pw(&c, 0x114, 0xf010);
    pw(&c, 0x116, 0x4c00);
    cpu->pc = 0x114;
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(cpu->exec_state == CPU_NORMAL && m.crp == UINT64_C(0x0000000200100000));
    /* Reserved PS is accepted while TC.E is clear (TC = 0 disables the MMU). */
    pl(&c, 0x400, 0x00700000);
    pw(&c, 0x118, 0xf010);
    pw(&c, 0x11a, 0x4000);
    cpu->pc = 0x118;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x11c && cpu->exec_state == CPU_NORMAL);
    pl(&c, 0x400, 0x00000000);
    pw(&c, 0x118, 0xf010);
    pw(&c, 0x11a, 0x4000);
    cpu->pc = 0x118;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x11c && cpu->exec_state == CPU_NORMAL);
    /* PFLUSHA is EA-less and must therefore accept opcode F000. */
    cpu->exec_state = CPU_NORMAL;
    pw(&c, 0x11c, 0xf000);
    pw(&c, 0x11e, 0x2400);
    cpu->pc = 0x11c;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x120);
    /* TT hit is reported by level-0 PTEST even while TC.E is clear. */
    pw(&c, 0x120, 0xf010);
    pw(&c, 0x122, 0x8215);
    cpu->pc = 0x120;
    cpu->a[0] = 0xa5123456;
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.mmusr == 0x0040);
    /* PMOVE updates TC first; invalid enabled TC clears E and takes vector 56. */
    cpu->a[0] = 0x400;
    pl(&c, 0x400, 0x80000000);
    pw(&c, 0x124, 0xf010);
    pw(&c, 0x126, 0x4000);
    cpu->pc = 0x124;
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.tc == 0);
    assert(cpu->pc == 0x200);
    /* PLOAD searches an unmapped table without turning its result into a
     * guest exception (nor a double-fault HALT). */
    cpu->exec_state = CPU_NORMAL;
    cpu->mmu_tc = 0x02c0aa00;
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0);
    pw(&c, 0x128, 0xf010);
    pw(&c, 0x12a, 0x2215);
    cpu->pc = 0x128;
    cpu->a[0] = 0x00403010;
    mc68k_execute(cpu, 1);
    assert(cpu->exec_state == CPU_NORMAL && cpu->pc == 0x12c);
    /* Table PTEST is non-faulting, records the completed depth in MMUSR.N,
     * and A/An returns the physical address of the final descriptor. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
    pl(&c, 0x404, 0x0000080a);
    pl(&c, 0x80c, 0x0000c001);
    pw(&c, 0x12c, 0xf010);
    pw(&c, 0x12e, 0x9f35);
    cpu->pc = 0x12c; /* PTESTR #5,(A0),#7,A1 */
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(cpu->exec_state == CPU_NORMAL && m.mmusr == 2 && cpu->a[1] == 0x80c);
    /* PFLUSH EA compares pages with the current TC.PS, not with an entry's
     * old page_shift. */
    cpu->atc[0] = (mc68k_atc_entry_t){
        .valid = true, .fc = 5, .logical_page = 0x00403000, .page_shift = 8};
    pw(&c, 0x132, 0xf010);
    pw(&c, 0x134, 0x38f5);
    cpu->pc = 0x132;
    cpu->a[0] = 0x00403010;
    mc68k_execute(cpu, 1);
    assert(cpu->exec_state == CPU_NORMAL && !cpu->atc[0].valid);
    /* PFLUSH MASK=0 ignores FC and therefore flushes every FC entry. */
    cpu->atc[0] =
        (mc68k_atc_entry_t){.valid = true, .fc = 1, .logical_page = 0x1000, .page_shift = 12};
    cpu->atc[1] =
        (mc68k_atc_entry_t){.valid = true, .fc = 5, .logical_page = 0x2000, .page_shift = 12};
    pw(&c, 0x136, 0xf000);
    pw(&c, 0x138, 0x3001);
    cpu->pc = 0x136; /* PFLUSH DFC,MASK=0 */
    mc68k_execute(cpu, 1);
    assert(!cpu->atc[0].valid && !cpu->atc[1].valid);
    /* PMOVE moves both ways, and MMUSR is a word transfer. */
    cpu->mmu_tt1 = 0xa5008250;
    cpu->mmu_srp = UINT64_C(0x8000000200000500);
    cpu->mmu_sr = 0x1234;
    pl(&c, 0x400, 0);
    pl(&c, 0x404, 0);
    pw(&c, 0x408, 0);
    pw(&c, 0x160, 0xf010);
    pw(&c, 0x162, 0x0e00);
    cpu->pc = 0x160; /* PMOVE TT1,(A0) */
    cpu->a[0] = 0x400;
    mc68k_execute(cpu, 1);
    uint32_t stored = 0;
    assert(rl(&c, 0x400, &stored) && stored == 0xa5008250);
    pw(&c, 0x164, 0xf010);
    pw(&c, 0x166, 0x4a00);
    cpu->pc = 0x164; /* PMOVE SRP,(A0) */
    mc68k_execute(cpu, 1);
    assert(rl(&c, 0x400, &stored) && stored == 0x80000002);
    assert(rl(&c, 0x404, &stored) && stored == 0x00000500);
    pw(&c, 0x168, 0xf010);
    pw(&c, 0x16a, 0x6200);
    cpu->pc = 0x168;
    cpu->a[0] = 0x408;
    mc68k_execute(cpu, 1); /* PMOVE MMUSR,(A0) */
    assert(c.m[0x408] == 0x12 && c.m[0x409] == 0x34);
    pw(&c, 0x408, 0x5678);
    pw(&c, 0x16c, 0xf010);
    pw(&c, 0x16e, 0x6000);
    cpu->pc = 0x16c;
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m); /* PMOVE (A0),MMUSR */
    assert(m.mmusr == 0x5678);
    cpu->a[0] = 0x400;
    pl(&c, 0x400, 0x00000002);
    pl(&c, 0x404, 0x00000600);
    pw(&c, 0x170, 0xf010);
    pw(&c, 0x172, 0x4800);
    cpu->pc = 0x170;
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m); /* PMOVE (A0),SRP */
    assert(m.srp == UINT64_C(0x0000000200000600));
    pl(&c, 0x400, 0xa5008650);
    pw(&c, 0x174, 0xf010);
    pw(&c, 0x176, 0x0c00);
    cpu->pc = 0x174;
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m); /* PMOVE (A0),TT1 */
    assert(m.tt1 == 0xa5008650);

    /* Every FD=0 PMOVE that loads a translation register flushes the ATC. */
    static const struct {
        uint32_t at;
        uint16_t ext;
    } loads[] = {
        {0x180, 0x0800}, /* TT0 */
        {0x184, 0x0c00}, /* TT1 */
        {0x188, 0x4000}, /* TC  */
        {0x18c, 0x4800}, /* SRP */
        {0x190, 0x4c00}, /* CRP */
    };
    pl(&c, 0x400, 0x00000002);
    pl(&c, 0x404, 0x00000600);
    for (unsigned i = 0; i < sizeof loads / sizeof loads[0]; ++i) {
        cpu->atc[0] = (mc68k_atc_entry_t){
            .valid = true, .fc = 5, .logical_page = 0x1000, .page_shift = 12};
        if (loads[i].ext == 0x4000)
            pl(&c, 0x400, 0x02c0aa00);
        pw(&c, loads[i].at, 0xf010);
        pw(&c, loads[i].at + 2, loads[i].ext);
        cpu->pc = loads[i].at;
        mc68k_execute(cpu, 1);
        assert(cpu->exec_state == CPU_NORMAL && !cpu->atc[0].valid);
        if (loads[i].ext == 0x4000)
            pl(&c, 0x400, 0x00000002);
    }

    /* A configuration error is a post-instruction exception: format $2 with
     * the next instruction's PC and the faulting instruction's own address. */
    pl(&c, 0x400, 0x80700000); /* E set, reserved PS */
    pw(&c, 0x194, 0xf010);
    pw(&c, 0x196, 0x4000);
    cpu->pc = 0x194;
    cpu->a[7] = 0x700;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x200 && cpu->a[7] == 0x700 - 12);
    uint32_t frame = cpu->a[7];
    uint16_t word = 0;
    assert(rw(&c, frame + 6, &word) && word == 0x20e0);
    assert(rl(&c, frame + 2, &stored) && stored == 0x198);
    assert(rl(&c, frame + 8, &stored) && stored == 0x194);
    cpu->exec_state = CPU_NORMAL;

    /* A quad transfer commits only once both longs have moved.  The register
     * side is all-or-nothing; the memory side cannot undo its first write. */
    cpu->mmu_crp = UINT64_C(0x0000000200000700);
    pl(&c, 0x400, 0x00000002);
    pl(&c, 0x404, 0x00000900);
    c.fail_read_at = 0x404;
    pw(&c, 0x198, 0xf010);
    pw(&c, 0x19a, 0x4c00);
    cpu->pc = 0x198; /* PMOVE (A0),CRP */
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(cpu->last_exception_vector == 2 && cpu->pc == 0x260 &&
           m.crp == UINT64_C(0x0000000200000700));
    c.fail_read_at = UINT32_MAX;
    pl(&c, 0x400, 0);
    pl(&c, 0x404, 0);
    c.fail_write_at = 0x404;
    pw(&c, 0x19c, 0xf010);
    pw(&c, 0x19e, 0x4e00);
    cpu->pc = 0x19c; /* PMOVE CRP,(A0) */
    mc68k_execute(cpu, 1);
    assert(cpu->last_exception_vector == 2 && cpu->pc == 0x260);
    assert(rl(&c, 0x400, &stored) && stored == 0x00000002); /* first long stands */
    assert(rl(&c, 0x404, &stored) && stored == 0);
    c.fail_write_at = UINT32_MAX;
    cpu->mmu_tc = 0x02c0aa00;
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);

    /* PFLUSH compares only the FC bits that MASK sets. */
    cpu->atc[0] =
        (mc68k_atc_entry_t){.valid = true, .fc = 5, .logical_page = 0x1000, .page_shift = 12};
    cpu->atc[1] =
        (mc68k_atc_entry_t){.valid = true, .fc = 1, .logical_page = 0x1000, .page_shift = 12};
    cpu->atc[2] =
        (mc68k_atc_entry_t){.valid = true, .fc = 7, .logical_page = 0x1000, .page_shift = 12};
    pw(&c, 0x13a, 0xf000);
    pw(&c, 0x13c, 0x3095);
    cpu->pc = 0x13a; /* PFLUSH #5,#4 */
    mc68k_execute(cpu, 1);
    assert(!cpu->atc[0].valid && cpu->atc[1].valid && !cpu->atc[2].valid);
    /* The FC operand may also name a data register or SFC. */
    cpu->mmu_tt0 = 0xa5008250; /* enabled, LAB=a5, read, FC=5 */
    cpu->d[2] = 5;
    cpu->a[0] = 0xa5123456;
    pw(&c, 0x13e, 0xf010);
    pw(&c, 0x140, 0x820a);
    cpu->pc = 0x13e; /* PTESTR D2,(A0),#0 */
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.mmusr == 0x0040);
    cpu->d[2] = 1;
    pw(&c, 0x142, 0xf010);
    pw(&c, 0x144, 0x820a);
    cpu->pc = 0x142;
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.mmusr == 0x0400);
    cpu->sfc = 5;
    cpu->atc[0] =
        (mc68k_atc_entry_t){.valid = true, .fc = 5, .logical_page = 0x1000, .page_shift = 12};
    pw(&c, 0x146, 0xf000);
    pw(&c, 0x148, 0x30e0);
    cpu->pc = 0x146; /* PFLUSH SFC,#7 */
    mc68k_execute(cpu, 1);
    assert(!cpu->atc[0].valid && cpu->atc[1].valid);
    /* PLOAD replaces the matching ATC entry with a fresh search instead of
     * adding a second one.  PLOADR marks every traversed descriptor used;
     * PLOADW also marks the final page modified. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0x00000802);
    pl(&c, 0x80c, 0x0000c001);
    cpu->atc[0] = (mc68k_atc_entry_t){.valid = true,
                                      .fc = 5,
                                      .page_shift = 12,
                                      .logical_page = 0x00403000,
                                      .physical_page = 0x00099000};
    pw(&c, 0x14a, 0xf010);
    pw(&c, 0x14c, 0x2215);
    cpu->pc = 0x14a; /* PLOADR #5,(A0) */
    cpu->a[0] = 0x00403010;
    mc68k_execute(cpu, 1);
    uint32_t d = 0;
    assert(rl(&c, 0x404, &d) && d == 0x0000080a);
    assert(rl(&c, 0x80c, &d) && d == 0x0000c009);
    unsigned loaded = 0;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        if (cpu->atc[i].valid && cpu->atc[i].fc == 5 &&
            cpu->atc[i].logical_page == 0x00403000) {
            loaded++;
            assert(cpu->atc[i].physical_page == 0x0000c000 && !cpu->atc[i].modified);
        }
    assert(loaded == 1);
    pl(&c, 0x80c, 0x0000c001);
    pw(&c, 0x14e, 0xf010);
    pw(&c, 0x150, 0x2015);
    cpu->pc = 0x14e; /* PLOADW #5,(A0) */
    mc68k_execute(cpu, 1);
    assert(rl(&c, 0x80c, &d) && d == 0x0000c019);
    loaded = 0;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        if (cpu->atc[i].valid && cpu->atc[i].fc == 5 &&
            cpu->atc[i].logical_page == 0x00403000) {
            loaded++;
            assert(cpu->atc[i].modified);
        }
    assert(loaded == 1);
    /* B2: TT applies independently of TC.E, has TT0 priority, and exposes CI. */
    bool ci = false;
    cpu->mmu_tc = 0;
    cpu->mmu_tt0 = 0xa5008650; /* enabled, CI, read, FC=5 */
    cpu->mmu_tt1 = 0xa5008250;
    assert(test_translate(cpu, 0xa5123456, 5, true, &ci) == 0xa5123456 && ci);
    ci = true;
    assert(test_translate(cpu, 0xa5123456, 5, false, &ci) == 0xa5123456 && !ci);
    cpu->mmu_tt0 = 0;
    ci = true;
    assert(test_translate(cpu, 0x00123456, 5, true, &ci) == 0x00123456 && !ci);
    /* B3: 4 KiB two-level short descriptors and the ATC cache. */
    cpu->mmu_tt0 = cpu->mmu_tt1 = 0;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
    cpu->mmu_tc = 0x82c0aa00; /* E, 4KiB, IS=0, A=10, B=10 */
    cpu->mmu_crp = UINT64_C(0x0000000200000400);
    cpu->mmu_srp = cpu->mmu_crp;
    pl(&c, 0x404, 0x0000080a); /* A[1] -> short B table, U set */
    pl(&c, 0x80c, 0x0000c041); /* B[3] -> CI page at 0x0c000 */
    assert(test_translate(cpu, 0x00403010, 5, true, &ci) == 0x0000c010 && ci);
    assert(test_translate(cpu, 0x00403345, 5, true, &ci) == 0x0000c345);
    assert(test_translate(cpu, 0x00403010, 5, true, &ci) == 0x0000c010);
    pl(&c, 0x80c, 0x0000d001); /* cached translation must still win */
    assert(test_translate(cpu, 0x00403010, 5, true, &ci) == 0x0000c010);
    pl(&c, 0x400, 0x0000080a);
    pl(&c, 0x800, 0x00000001);
    pw(&c, 0x200, 0xf000);
    pw(&c, 0x202, 0x2400);
    cpu->pc = 0x200;
    mc68k_execute(cpu, 1);
    assert(test_translate(cpu, 0x00403010, 5, true, &ci) == 0x0000d010 && !ci);
    /* Long root/table descriptors can feed a short leaf table. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8000000300000400);
    pl(&c, 0x408, 0x00000002);
    pl(&c, 0x40c, 0x00000800);
    pl(&c, 0x80c, 0x0000d001);
    assert(test_translate(cpu, 0x00403010, 5, true, &ci) == 0x0000d010);
    /* A table descriptor at the final level is an indirect page descriptor. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0x00000802);
    pl(&c, 0x80c, 0x00000902);
    pl(&c, 0x900, 0x0000c001);
    assert(test_translate(cpu, 0x00403010, 5, true, &ci) == 0x0000c010);
    /* FC2 selects CRP/SRP when SRE is set, independently of SR.S. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
    cpu->mmu_crp = UINT64_C(0x0000000200000400);
    cpu->mmu_srp = UINT64_C(0x0000000200000500);
    pl(&c, 0x404, 0x0000080a);
    pl(&c, 0x504, 0x00000902);
    pl(&c, 0x80c, 0x0000c001);
    pl(&c, 0x90c, 0x0000d001);
    assert(test_translate(cpu, 0x00403010, 1, true, &ci) == 0x0000c010);
    /* 早期終了ページ記述子: 未使用インデックスビットはページアドレス欄へ
     * 連結される。ページアドレスをブロック幅で切り捨ててはならない
     * （IPLROM30 の MMU 自己診断は 2MB ブロックに非整列の $00F00000 を置き、
     * 論理 $200000 が物理 $F00000＝CGROM を指すことを確かめる）。IS=8 で
     * 上位8bitが捨てられるため、ベクタ番号を上位バイトに埋めた $02FF0770 が
     * ROM の $00FF0770 へ変換されることも同時に確かめる。 */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
    cpu->mmu_tc = 0x80d83440; /* E, 8KiB, IS=8, A=3, B=4, C=4 */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x7fff000200000400);
    pl(&c, 0x400 + 0 * 4, 0x00000001);
    pl(&c, 0x400 + 1 * 4, 0x00f00001);
    pl(&c, 0x400 + 7 * 4, 0x00e00001);
    assert(test_translate(cpu, 0x00200000, 5, true, &ci) == 0x00f00000);
    assert(test_translate(cpu, 0x00201234, 5, true, &ci) == 0x00f01234);
    assert(test_translate(cpu, 0x02ff0770, 6, true, &ci) == 0x00ff0770);
    /* B4: an invalid translation is a logical-address bus error and is
     * retained as a negative ATC entry. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0); /* A[1] invalid */
    const mc68k_access_t access = {.fc = 5, .read = true, .size = 4};
    (void)cpu_read_long_access(cpu, 0x00403010, &access);
    assert(cpu->exec_state == CPU_EXCEPTION && cpu->fault_vector == 2 &&
           cpu->addr_err_addr == 0x00403010 && cpu->addr_err_read);
    cpu->exec_state = CPU_NORMAL;
    (void)cpu_read_long_access(cpu, 0x00403010, &access);
    assert(cpu->exec_state == CPU_EXCEPTION); /* negative ATC hit */
    /* Reset disables translation but does not flush the 68030 ATC. */
    cpu->atc[0].valid = true;
    cpu->mmu_tc = 0x82c0aa00;
    mc68k_reset(cpu);
    assert(cpu->atc[0].valid && !(cpu->mmu_tc & UINT32_C(0x80000000)));
    /* The MC68030 rebuilds the whole MMUSR word, whereas an EC030 PTEST only
     * reports its AC hit in bit 6 and leaves every other bit untouched. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
    cpu->mmu_sr = 0xffff;
    pw(&c, 0x300, 0xf010);
    pw(&c, 0x302, 0x8215);
    cpu->pc = 0x300; /* PTESTR #5,(A0),#0 */
    cpu->a[0] = 0x00123456;
    mc68k_execute(cpu, 1);
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.mmusr == 0x0400);
    mc68k_state_t *ec = mc68k_create_model(&b, MC68K_MODEL_68EC030);
    mc68k_reset(ec);
    ec->mmu_tt0 = 0xa5008250; /* AC0: enabled, LAB=a5, read, FC=5 */
    ec->mmu_sr = 0xffff;
    ec->pc = 0x300;
    ec->a[0] = 0xa5123456;
    mc68k_execute(ec, 1);
    mc68k_get_mmu_regs(ec, &m);
    assert(m.mmusr == 0xffff);
    ec->pc = 0x300;
    ec->a[0] = 0x00123456;
    mc68k_execute(ec, 1);
    mc68k_get_mmu_regs(ec, &m);
    assert(m.mmusr == 0xffbf);
    mc68k_destroy(ec);
    mc68k_destroy(cpu);
}
