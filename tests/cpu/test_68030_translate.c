#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>

typedef struct {
    unsigned char m[0x1000];
    uint32_t fail_read_at, fail_write_at;
    unsigned word_reads, long_reads;
} ctx_t;

static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)p)->m[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    c->word_reads++;
    *v = (uint16_t)((c->m[a] << 8) | c->m[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    c->long_reads++;
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
    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    if (a == c->fail_write_at)
        return false;
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
    return true;
}
static void pl(ctx_t *c, unsigned a, uint32_t v) {
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
}

static bool atc_has_page(const mc68k_state_t *cpu, uint32_t page) {
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        if (cpu->atc[i].valid && cpu->atc[i].fc == 5 && cpu->atc[i].logical_page == page)
            return true;
    return false;
}

static mc68k_bus_t make_bus(ctx_t *ctx) {
    return (mc68k_bus_t){rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, ctx};
}

int main(void) {
    ctx_t c = {.fail_read_at = UINT32_MAX, .fail_write_at = UINT32_MAX};
    mc68k_bus_t bus = make_bus(&c);
    const mc68k_access_t read_super = {.fc = 5, .read = true, .size = 4};

    mc68k_state_t *plain = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(plain);
    mc68k_translate_result_t r =
        cpu_mmu_translate_result(plain, 0x12345678, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.source == MC68K_TRANSLATE_IDENTITY && r.physical == 0x12345678);
    mc68k_destroy(plain);

    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
    mc68k_reset(cpu);
    r = cpu_mmu_translate_result(cpu, 0x00123456, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.source == MC68K_TRANSLATE_IDENTITY && r.physical == 0x00123456);

    cpu->mmu_tt0 = 0xa5008650; /* enabled, CI, read, FC=5 */
    r = cpu_mmu_translate_result(cpu, 0xa5123456, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(r.ok && r.source == MC68K_TRANSLATE_TT && r.physical == 0xa5123456 &&
           r.cache_inhibit && r.mmusr == 0x0040);

    cpu->mmu_tt0 = 0;
    cpu->mmu_tc = 0x82c0aa00; /* E, 4KiB, IS=0, A=10, B=10 */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0x00000802); /* A[1] -> short B table */
    pl(&c, 0x80c, 0x0000c041); /* B[3] -> CI page at 0x0c000 */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.physical == 0x0000c010 &&
           r.cache_inhibit && r.level == 2 && r.descriptor_physical == 0x80c);
    assert(cpu->exec_state == CPU_NORMAL && !cpu->atc[0].valid);
    mc68k_debug_translate_result_t debug_translation;
    assert(mc68k_debug_translate(cpu, 0x00403010, 5, 1, &debug_translation) == 0 &&
           debug_translation.physical == 0x0000c010 && debug_translation.level == 2 &&
           debug_translation.fault == MC68K_DEBUG_MMU_NONE && !cpu->atc[0].valid);
    uint32_t descriptor;
    assert(rl(&c, 0x80c, &descriptor) && descriptor == 0x0000c041);

    /* A write sets U on every traversed descriptor and M on the successful
     * final page only. */
    const mc68k_access_t write_super = {.fc = 5, .read = false, .size = 4};
    r = cpu_mmu_translate_result(cpu, 0x00403010, &write_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.physical == 0x0000c010);
    assert(rl(&c, 0x404, &descriptor) && descriptor == 0x0000080a);
    assert(rl(&c, 0x80c, &descriptor) && descriptor == 0x0000c059);

    /* A read-filled M=0 ATC entry re-walks on its first write. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    pl(&c, 0x80c, 0x0000c041);
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK);
    r = cpu_mmu_translate_result(cpu, 0x00403010, &write_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK);
    assert(rl(&c, 0x80c, &descriptor) && descriptor == 0x0000c059);

    /* A write-protected page records U but never M. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    pl(&c, 0x80c, 0x0000c045);
    r = cpu_mmu_translate_result(cpu, 0x00403010, &write_super, MC68K_TRANSLATE_EXECUTE);
    assert(!r.ok && r.fault == MMU_FAULT_WRITE_PROTECT);
    assert(rl(&c, 0x80c, &descriptor) && descriptor == 0x0000c04d);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    pl(&c, 0x80c, 0x0000c041);

    /* PLOAD/PTEST are permitted to search tables with TC.E clear; neither
     * mode turns an unsuccessful lookup into an execution exception. */
    cpu->mmu_tc &= ~UINT32_C(0x80000000);
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_PLOAD);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && cpu->exec_state == CPU_NORMAL);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    cpu->mmu_tc |= UINT32_C(0x80000000);

    pl(&c, 0x404, 0); /* invalid descriptor is data, not a CPU exception */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(!r.ok && r.source == MC68K_TRANSLATE_WALK && r.fault == MMU_FAULT_INVALID);
    assert(cpu->exec_state == CPU_NORMAL);
    /* A level-0 PTEST answers from the negative ATC entry the failed access
     * left behind, without re-reading the tables. */
    uint32_t last_descriptor = 0xdeadbeef;
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 0, &last_descriptor) == 0x8400 &&
           last_descriptor == 0);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;

    c.fail_read_at = 0x404;
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(!r.ok && r.fault == MMU_FAULT_TABLE_BUS_ERROR &&
           r.table_bus_error_physical == 0x404 && cpu->exec_state == CPU_NORMAL);
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 0, &last_descriptor) == 0x8400);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;

    /* Long root descriptors enforce their LIMIT and supervisor protection
     * against the supplied access FC, not the CPU's current SR. */
    c.fail_read_at = UINT32_MAX;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000300000400); /* upper limit 0, index is 1 */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(!r.ok && r.fault == MMU_FAULT_LIMIT);
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8002000300000400); /* lower limit 2 > index 1 */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(!r.ok && r.fault == MMU_FAULT_LIMIT);
    cpu->mmu_crp = cpu->mmu_srp =
        UINT64_C(0x8000010300000400); /* lower limit + S + long root */
    const mc68k_access_t read_user = {.fc = 1, .read = true, .size = 4};
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_user, MC68K_TRANSLATE_DEBUG);
    assert(!r.ok && r.fault == MMU_FAULT_SUPERVISOR);

    /* A page descriptor before the leaf terminates the walk early.  Its
     * offset contains every logical bit not consumed by the lookup, not
     * only the 4 KiB TC page offset. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0x00800001); /* A[1] early page, 4 MiB-aligned base */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(r.ok && r.physical == 0x00803010 && r.level == 1);

    /* The root pointer itself may be a page descriptor.  Being long-format it
     * still carries a LIMIT, which applies to the index field the first table
     * level would otherwise have used. */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8000000100000000); /* lower LIMIT 0 */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(r.ok && r.physical == 0x00403010 && r.level == 0);
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8002000100000000); /* lower LIMIT 2 > index 1 */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(!r.ok && r.fault == MMU_FAULT_LIMIT);
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000100000000); /* upper LIMIT 0 < index 1 */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(!r.ok && r.fault == MMU_FAULT_LIMIT);

    /* FCL uses FC as the first lookup index and ignores the root LIMIT. */
    cpu->mmu_tc = 0x83c0aa00; /* E, SRE, FCL, 4KiB, A=10, B=10 */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000300000400); /* upper limit 0 */
    pl(&c, 0x428, 0x00000001);
    pl(&c, 0x42c, 0x00400000); /* long A[FC=5] page */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(r.ok && r.physical == 0x00403010 && r.level == 1);

    /* PTEST LEVEL=0 searches the ATC alone.  A miss is reported as invalid
     * and must not fall back to a table search, even where one would
     * succeed. */
    cpu->mmu_tc = 0x82c0aa00; /* E, 4KiB, IS=0, A=10, B=10 */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0x00000802); /* A[1] -> short B table, U clear */
    pl(&c, 0x80c, 0x0000c001); /* B[3] -> page at 0x0c000, U/M/WP clear */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    unsigned long_reads = c.long_reads;
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 0, &last_descriptor) == 0x0400 &&
           c.long_reads == long_reads);

    /* LEVEL 1-7 searches the tables alone and stops once LEVEL descriptors
     * have been read.  Stopping at the requested level is not a fault: N
     * reports the depth reached and A/An the last descriptor read. */
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 1, &last_descriptor) == 1 &&
           last_descriptor == 0x404);
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 7, &last_descriptor) == 2 &&
           last_descriptor == 0x80c);

    /* W is the OR of WP over the whole search and is reported for a read as
     * well; M is the final page descriptor's own bit.  Neither encodes a
     * failure. */
    pl(&c, 0x404, 0x00000806); /* the parent table descriptor is write-protected */
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 7, &last_descriptor) == 0x0802);
    pl(&c, 0x404, 0x00000802);
    pl(&c, 0x80c, 0x0000c011); /* the leaf page is already modified */
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 7, &last_descriptor) == 0x0202);
    pl(&c, 0x80c, 0x0000c001);

    /* Every failure reason has its own MMUSR encoding; B and L imply I. */
    pl(&c, 0x404, 0);
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 7, &last_descriptor) == 0x0401);
    c.fail_read_at = 0x404;
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 7, &last_descriptor) == 0x8400);
    c.fail_read_at = UINT32_MAX;
    pl(&c, 0x404, 0x00000802);
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000300000400); /* upper LIMIT 0 */
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 7, &last_descriptor) == 0x4400);
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8000010300000400); /* lower LIMIT + S */
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_user, 7, &last_descriptor) == 0x2000);

    /* PTEST alters neither the used/modified bits of the tables nor the ATC
     * entries and their replacement history. */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    cpu->atc[3] = (mc68k_atc_entry_t){.logical_page = 0x00403000,
                                      .physical_page = 0x00099000,
                                      .fc = 5,
                                      .page_shift = 12,
                                      .valid = true,
                                      .history = 1};
    uint32_t atc_clock = cpu->atc_clock;
    assert(cpu_mmu_ptest(cpu, 0x00403010, &write_super, 7, &last_descriptor) == 2);
    assert(rl(&c, 0x404, &descriptor) && descriptor == 0x00000802);
    assert(rl(&c, 0x80c, &descriptor) && descriptor == 0x0000c001);
    assert(cpu->atc[3].valid && !cpu->atc[3].modified && cpu->atc[3].history == 1 &&
           cpu->atc_clock == atc_clock);
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 0, &last_descriptor) == 0 &&
           last_descriptor == 0 && cpu->atc[3].history == 1 && cpu->atc_clock == atc_clock);

    /* At LEVEL 0 the W and M bits come from the ATC entry, so the same
     * logical address answers differently once the tables move on. */
    cpu->atc[3].write_protect = cpu->atc[3].modified = true;
    pl(&c, 0x404, 0);
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 0, &last_descriptor) == 0x0a00);
    assert(cpu_mmu_ptest(cpu, 0x00403010, &read_super, 7, &last_descriptor) == 0x0401);
    pl(&c, 0x404, 0x00000802);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;

    /* A write-protected parent propagates WP to every page below it, even
     * where the leaf page descriptor clears WP itself. */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0x00000806); /* short table descriptor, WP set, U clear */
    pl(&c, 0x80c, 0x0000c001); /* the leaf page is not write-protected */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &write_super, MC68K_TRANSLATE_EXECUTE);
    assert(!r.ok && r.fault == MMU_FAULT_WRITE_PROTECT && r.write_protect);
    assert(rl(&c, 0x404, &descriptor) && descriptor == 0x0000080e);
    assert(rl(&c, 0x80c, &descriptor) && descriptor == 0x0000c009); /* U set, M clear */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;

    /* A supervisor violation is detected before the descriptor is marked
     * used, so neither it nor anything below it is written. */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x8000000300000400); /* long root, lower LIMIT 0 */
    pl(&c, 0x408, 0x00000102);
    pl(&c, 0x40c, 0x00000800); /* long A[1]: S set, U clear */
    pl(&c, 0x80c, 0x0000c001);
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_user, MC68K_TRANSLATE_EXECUTE);
    assert(!r.ok && r.fault == MMU_FAULT_SUPERVISOR);
    assert(rl(&c, 0x408, &descriptor) && descriptor == 0x00000102);
    assert(rl(&c, 0x80c, &descriptor) && descriptor == 0x0000c001);
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.physical == 0x0000c010);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;

    /* A failed U or M write-back is a table bus error naming the descriptor
     * that could not be updated. */
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0x0000080a);
    pl(&c, 0x80c, 0x0000c001);
    c.fail_write_at = 0x80c;
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(!r.ok && r.fault == MMU_FAULT_TABLE_BUS_ERROR &&
           r.table_bus_error_physical == 0x80c);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    pl(&c, 0x80c, 0x0000c009); /* U already set, so only the M write-back fails */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &write_super, MC68K_TRANSLATE_EXECUTE);
    assert(!r.ok && r.fault == MMU_FAULT_TABLE_BUS_ERROR &&
           r.table_bus_error_physical == 0x80c);
    c.fail_write_at = UINT32_MAX;

    /* With more pages in play than the ATC holds, an invalid slot is filled
     * before any valid entry is evicted, and otherwise the least recently
     * used entry goes. */
    pl(&c, 0x404, 0x0000080a);
    for (unsigned b = 0; b < 24; ++b)
        pl(&c, 0x800 + b * 4, (0x00100000u + b * 0x1000u) | 9u);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    for (unsigned b = 0; b < MC68K_ATC_ENTRIES; ++b) {
        r = cpu_mmu_translate_result(cpu, 0x00400000u + b * 0x1000u, &read_super,
                                     MC68K_TRANSLATE_EXECUTE);
        assert(r.ok && r.source == MC68K_TRANSLATE_WALK &&
               r.physical == 0x00100000u + b * 0x1000u);
    }
    r = cpu_mmu_translate_result(cpu, 0x00400000, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.source == MC68K_TRANSLATE_ATC); /* page 0 is now the newest */
    r = cpu_mmu_translate_result(cpu, 0x00400000u + 22 * 0x1000u, &read_super,
                                 MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK);
    assert(atc_has_page(cpu, 0x00400000) && !atc_has_page(cpu, 0x00401000) &&
           atc_has_page(cpu, 0x00400000u + 22 * 0x1000u));
    cpu->atc[7].valid = false;
    r = cpu_mmu_translate_result(cpu, 0x00400000u + 23 * 0x1000u, &read_super,
                                 MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && cpu->atc[7].valid && cpu->atc[7].logical_page == 0x00400000u + 23 * 0x1000u);
    unsigned valid_entries = 0;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        valid_entries += cpu->atc[i].valid;
    assert(valid_entries == MC68K_ATC_ENTRIES);

    /* An entry registered under a different TC.PS must not hit once the page
     * size changes, and must still hit when it changes back. */
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
    pl(&c, 0x80c, 0x0000c009);
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.physical == 0x0000c010);
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.source == MC68K_TRANSLATE_ATC);
    cpu->mmu_tc = 0x82d0aa00;  /* 8 KiB pages, same table geometry */
    pl(&c, 0x80c, 0x0000d009); /* the tables have moved on since the entry */
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.source == MC68K_TRANSLATE_WALK && r.physical == 0x0000d010);
    cpu->mmu_tc = 0x82c0aa00;
    r = cpu_mmu_translate_result(cpu, 0x00403010, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.source == MC68K_TRANSLATE_ATC && r.physical == 0x0000c010);

    /* An exception already pending must not prevent the frame/vector bus
     * cycle from reaching the host.  FC is carried by this access, rather
     * than inherited from an earlier guest fault. */
    cpu->mmu_tc = 0;
    const mc68k_access_t frame_read = cpu_supervisor_data_access(true, 2, false);
    unsigned before = c.word_reads;
    cpu->exec_state = CPU_EXCEPTION;
    cpu->fault_access.fc = 1; /* stale user-data FC must not leak into the new cycle */
    (void)cpu_read_word_access(cpu, 0x20, &frame_read);
    assert(c.word_reads == before + 1 && cpu->exec_state == CPU_EXCEPTION &&
           cpu->fault_access.fc == 5 && cpu_noted_size_code(cpu) == 2 &&
           !cpu->fault_access.rmw);
    cpu->exec_state = CPU_NORMAL;

    /* Opcode/extension fetches are program-space cycles (FC=6 in supervisor
     * mode), independently of the ordinary data accessor's FC=5. */
    cpu->pc = 0x20;
    cpu->opcode_fetch_complete = false;
    (void)cpu_fetch_word(cpu);
    assert(cpu->fault_access.fc == 6 && cpu->fault_access_kind == FAULT_OPCODE);

    mc68k_destroy(cpu);
    return 0;
}
