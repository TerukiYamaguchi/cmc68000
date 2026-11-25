/* MC68040 CINV/CPUSH and MOVE16 cache-visible behavior (MC68040UM §4.2--4.3). */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum { RAM_SIZE = 0x10000 };
typedef struct {
    uint8_t memory[RAM_SIZE];
    uint32_t fail_write_at;
} context_t;
static bool rb(void *p, uint32_t a, uint8_t *v) {
    context_t *c = p;
    if (a >= RAM_SIZE)
        return false;
    *v = c->memory[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    context_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    *v = (uint16_t)(c->memory[a] << 8 | c->memory[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    context_t *c = p;
    if (a + 3 >= RAM_SIZE)
        return false;
    *v = (uint32_t)c->memory[a] << 24 | (uint32_t)c->memory[a + 1] << 16 |
         (uint32_t)c->memory[a + 2] << 8 | c->memory[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    context_t *c = p;
    if (a >= RAM_SIZE)
        return false;
    c->memory[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    return wb(p, a, (uint8_t)(v >> 8)) && wb(p, a + 1, (uint8_t)v);
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    context_t *c = p;
    if (a == c->fail_write_at)
        return false;
    return ww(p, a, (uint16_t)(v >> 16)) && ww(p, a + 2, (uint16_t)v);
}
static void putl(context_t *c, uint32_t a, uint32_t v) {
    if (!wl(c, a, v))
        abort();
}
static uint8_t pk(void *p, uint32_t a) {
    context_t *c = p;
    return a < RAM_SIZE ? c->memory[a] : 0;
}
static uint32_t getl(context_t *c, uint32_t a) {
    uint32_t v;
    if (!rl(c, a, &v))
        abort();
    return v;
}

int main(void) {
    context_t c;
    memset(&c, 0, sizeof(c));
    c.fail_write_at = UINT32_MAX;
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .ctx = &c};
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68040);
    assert(cpu);
    cpu->sr = SR_S;
    cpu->cacr = MC68K_CACR_040_DE;

    putl(&c, 0x200, 0x11111111);
    assert(cpu_read_long(cpu, 0x200) == 0x11111111);
    putl(&c, 0x200, 0x22222222);
    assert(cpu_read_long(cpu, 0x200) == 0x11111111);
    cpu->a[0] = 0x200;
    assert(cpu->op_table[0xf448](cpu, 0xf448) == 9); /* CINVL D,(A0) */
    assert(cpu_read_long(cpu, 0x200) == 0x22222222);

    /* A host/DMA write remains invisible through the physical I-cache until
     * its matching line is invalidated. */
    const mc68k_access_t instruction = {.fc = 6, .read = true, .size = 2, .instruction = true};
    cpu->cacr |= MC68K_CACR_040_IE;
    if (!ww(&c, 0x280, 0x1111))
        abort();
    assert(cpu_read_word_access(cpu, 0x280, &instruction) == 0x1111);
    if (!ww(&c, 0x280, 0x2222))
        abort();
    assert(cpu_read_word_access(cpu, 0x280, &instruction) == 0x1111);
    cpu->a[0] = 0x280;
    assert(cpu->op_table[0xf488](cpu, 0xf488) == 9); /* CINVL I,(A0) */
    assert(cpu_read_word_access(cpu, 0x280, &instruction) == 0x2222);
    cpu->cacr &= ~MC68K_CACR_040_IE;

    const mc68k_access_t write = cpu_data_access(cpu, false, 4, false);
    putl(&c, 0x220, 0x33333333);
    assert(cpu_040_cache_write(cpu, 0x220, 0x220, 0x44444444, &write, 1));
    assert(getl(&c, 0x220) == 0x33333333);
    cpu->a[0] = 0x220;
    assert(cpu->op_table[0xf468](cpu, 0xf468) == 6); /* CPUSHL D,(A0) */
    assert(getl(&c, 0x220) == 0x44444444);

    /* CINV scope and cache selection: page acts on matching physical lines only,
     * and the I/D/both selectors are independent. */
    cpu->icache040[0][0] =
        (mc68k_040_cache_line_t){.physical_line = 0x1000, .valid = true, .loaded = true};
    cpu->dcache040[0][0] =
        (mc68k_040_cache_line_t){.physical_line = 0x1010, .valid = true, .loaded = true};
    cpu->dcache040[1][0] =
        (mc68k_040_cache_line_t){.physical_line = 0x2000, .valid = true, .loaded = true};
    cpu->a[0] = 0x10f0;
    assert(cpu->op_table[0xf450](cpu, 0xf450) == 266); /* CINVP D,(A0) */
    assert(cpu->icache040[0][0].valid && !cpu->dcache040[0][0].valid &&
           cpu->dcache040[1][0].valid);
    assert(cpu->op_table[0xf4d8](cpu, 0xf4d8) == 9); /* CINVA I/D */
    assert(!cpu->icache040[0][0].valid && !cpu->dcache040[1][0].valid);

    /* [PM §6 CINV/CPUSH Instruction Fields] CACHE=00 is a no-op, while
     * SCOPE=00 is an illegal instruction.  In particular, CPUSH with no
     * selected cache must not write back or invalidate a dirty D-cache line. */
    cpu->dcache040[2][0] = (mc68k_040_cache_line_t){.physical_line = 0x3000,
                                                    .data = {0x55667788},
                                                    .valid = true,
                                                    .loaded = true,
                                                    .dirty = 1};
    cpu->a[0] = 0x3000;
    assert(cpu->op_table[0xf408](cpu, 0xf408) == 9); /* CINVL none,(A0) */
    assert(cpu->op_table[0xf428](cpu, 0xf428) == 6); /* CPUSHL none,(A0) */
    assert(cpu->op_table[0xf4b8](cpu, 0xf4b8) ==
           267); /* CPUSHA I,(A0) (cc=10): Table 10-4 shares CPUSHP's best case */
    assert(cpu->dcache040[2][0].valid && cpu->dcache040[2][0].dirty == 1);
    assert(getl(&c, 0x3000) == 0);
    /* [PM §6 CINV/CPUSH Instruction Fields] CACHE=01 is data, 10 is
     * instruction.  CINVA IC must keep a dirty D-cache line, CPUSHA DC must
     * write it back (x68000#233). */
    cpu->a[0] = 0x3000;
    assert(cpu->op_table[0xf498](cpu, 0xf498) == 9); /* CINVA IC */
    assert(cpu->dcache040[2][0].valid && cpu->dcache040[2][0].dirty == 1);
    assert(getl(&c, 0x3000) == 0);
    assert(cpu->op_table[0xf478](cpu, 0xf478) == 267); /* CPUSHA DC */
    assert(!cpu->dcache040[2][0].valid);
    assert(getl(&c, 0x3000) == 0x55667788);
    putl(&c, 4 * 4, 0x800);
    cpu->a[7] = 0xff0;
    cpu->pc = 0x102;
    assert(cpu->op_table[0xf440](cpu, 0xf440) == 34); /* CINV scope=00 */
    assert(cpu->last_exception_vector == 4 && cpu->pc == 0x800);

    for (unsigned i = 0; i < 4; i++)
        putl(&c, 0x300 + i * 4u, 0x01020304u + i);
    assert(cpu_040_move16(cpu, 0x300, 0x400, true, true));
    for (unsigned i = 0; i < 4; i++)
        assert(getl(&c, 0x400 + i * 4u) == 0x01020304u + i);

    /* The five encodings cover four addressing forms: (An)+,(Am)+ plus
     * absolute/(An), (An)+/absolute, (An)/absolute, and absolute/(An). */
    for (unsigned form = 0; form < 4; form++) {
        uint32_t source = 0x500 + form * 0x40u, destination = 0x700 + form * 0x40u;
        for (unsigned i = 0; i < 4; i++)
            putl(&c, source + i * 4u, 0x11110000u | (form << 4) | i);
        cpu->a[0] = source;
        cpu->pc = 0x100;
        if (form & 1u) {
            putl(&c, 0x100, source);
            cpu->a[0] = destination;
        } else
            putl(&c, 0x100, destination);
        /* 040UM Table 10-5: forms 0/2/3 are 4 calculate + 7 execute;
         * form 1 adds postincrement work (5 + 8). */
        assert(cpu->op_table[0xf600u | (form << 3)](cpu, 0xf600u | (form << 3)) ==
               (form == 1 ? 13 : 11));
        uint32_t actual = (form & 1u) ? destination : getl(&c, 0x100);
        for (unsigned i = 0; i < 4; i++)
            assert(getl(&c, actual + i * 4u) == (0x11110000u | (form << 4) | i));
    }
    for (unsigned i = 0; i < 4; i++)
        putl(&c, 0x900 + i * 4u, 0xabcdef00u | i);
    cpu->a[0] = 0x900;
    cpu->a[1] = 0xa00;
    cpu->pc = 0x120;
    if (!ww(&c, 0x120, 0x9000))
        abort();
    assert(cpu->op_table[0xf620](cpu, 0xf620) == 13 && cpu->a[0] == 0x910 &&
           cpu->a[1] == 0xa10);
    for (unsigned i = 0; i < 4; i++)
        assert(getl(&c, 0xa00 + i * 4u) == (0xabcdef00u | i));

    /* A MOVE16 write fault stops at the failing longword; later longwords
     * are not issued and the caller observes the Group-0 fault. */
    for (unsigned i = 0; i < 4; i++) {
        putl(&c, 0xb00 + i * 4u, 0xface0000u | i);
        putl(&c, 0xc00 + i * 4u, 0);
    }
    c.fail_write_at = 0xc08;
    assert(!cpu_040_move16(cpu, 0xb00, 0xc00, false, false));
    assert(getl(&c, 0xc00) == 0xface0000u && getl(&c, 0xc04) == 0xface0001u);
    assert(getl(&c, 0xc08) == 0 && getl(&c, 0xc0c) == 0 && cpu->fault_vector == 2);
    c.fail_write_at = UINT32_MAX;

    /* Debugger reads see a dirty copyback line, which RAM does not hold yet
     * (x68000#175: stale stack reads under NetBSD/68040). */
    mc68k_set_peek_hooks(cpu, pk, NULL, NULL, &c);
    putl(&c, 0x600, 0x01020304);
    assert(cpu_040_cache_write(cpu, 0x600, 0x600, 0xaabbccdd, &write, 1));
    assert(getl(&c, 0x600) == 0x01020304);
    uint8_t dbg[8] = {0};
    assert(mc68k_debug_read_bytes(cpu, 0x600, dbg, 8) == 0);
    assert(dbg[0] == 0xaa && dbg[1] == 0xbb && dbg[2] == 0xcc && dbg[3] == 0xdd);
    assert(dbg[4] == 0 && getl(&c, 0x600) == 0x01020304); /* read is side-effect free */
    cpu->a[0] = 0x600;
    assert(cpu->op_table[0xf468](cpu, 0xf468) == 6); /* CPUSHL D,(A0) */
    assert(mc68k_debug_read_bytes(cpu, 0x600, dbg, 4) == 0 && dbg[0] == 0xaa);

    /* #99: misaligned word/long accesses are split before they reach the cache.  They must
     * read back correctly and reach memory after CPUSHA, in write-through and copyback. */
    enum { MISALIGN_BASE = 0x2000, MISALIGN_SPAN = 0x40, DTTR_ALL_COPYBACK = 0x0000e020 };
    cpu->exec_state = CPU_NORMAL; /* left by the CINV scope=00 illegal-instruction test */
    cpu->a[0] = 0;
    for (unsigned copyback = 0; copyback < 2; copyback++) {
        cpu->mmu040_dtt0 = copyback ? (DTTR_ALL_COPYBACK | 0x00ff0000u) : 0;
        assert(cpu->op_table[0xf4d8](cpu, 0xf4d8) == 9); /* CINVA I/D */
        uint8_t expect[MISALIGN_SPAN] = {0};
        memset(&c.memory[MISALIGN_BASE], 0, MISALIGN_SPAN);
        for (uint32_t off = 0; off < 0x1c; off++) {
            uint32_t value = 0x01020304u * (off + 1u);
            cpu_write_long(cpu, MISALIGN_BASE + off, value);
            for (unsigned i = 0; i < 4; i++)
                expect[off + i] = (uint8_t)(value >> (24 - 8 * i));
            assert(cpu_read_long(cpu, MISALIGN_BASE + off) == value);
        }
        for (uint32_t off = 0x20; off < 0x3f; off++) {
            uint16_t value = (uint16_t)(0xa5c3u + off * 0x111u);
            cpu_write_word(cpu, MISALIGN_BASE + off, value);
            expect[off] = (uint8_t)(value >> 8);
            expect[off + 1] = (uint8_t)value;
            assert(cpu_read_word(cpu, MISALIGN_BASE + off) == value);
        }
        assert(cpu->op_table[0xf478](cpu, 0xf478) == 267); /* CPUSHA DC */
        assert(memcmp(&c.memory[MISALIGN_BASE], expect, MISALIGN_SPAN) == 0);
    }
    cpu->mmu040_dtt0 = 0;
    mc68k_destroy(cpu);
    return 0;
}
