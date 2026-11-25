/* MC68040 exception frames: [040UM §8.4.3, §8.4.6]. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"

enum {
    RAM_SIZE = 0x10000,
    RESET_SP = 0x8000,
    RESET_PC = 0x1000,
    BUS_HANDLER = 0x300,
    FORMAT_HANDLER = 0x340,
    TRACE_HANDLER = 0x380,
    HOLE = 0x800000,
    MMU_ROOT = 0x2000,
    MMU_POINTER = 0x4000,
    MMU_PAGE = 0x6000
};

typedef struct {
    uint8_t ram[RAM_SIZE];
    uint32_t read_fault_from;
    uint32_t read_fault_once;
} ctx_t;

static uint16_t rw(const ctx_t *c, uint32_t a) {
    return (uint16_t)((uint16_t)c->ram[a] << 8 | c->ram[a + 1]);
}
static uint32_t rl(const ctx_t *c, uint32_t a) {
    return (uint32_t)rw(c, a) << 16 | rw(c, a + 2);
}
static void ww(ctx_t *c, uint32_t a, uint16_t v) {
    c->ram[a] = (uint8_t)(v >> 8);
    c->ram[a + 1] = (uint8_t)v;
}
static void wl(ctx_t *c, uint32_t a, uint32_t v) {
    ww(c, a, (uint16_t)(v >> 16));
    ww(c, a + 2, (uint16_t)v);
}
static void map_mmu_page(ctx_t *c, uint32_t logical) {
    unsigned ri = logical >> 25;
    unsigned pi = (logical >> 18) & 0x7f;
    unsigned pgi = (logical >> 12) & 0x3f;
    wl(c, MMU_ROOT + ri * 4u, MMU_POINTER | 2u);
    wl(c, MMU_POINTER + pi * 4u, MMU_PAGE | 2u);
    wl(c, MMU_PAGE + pgi * 4u, (logical & ~UINT32_C(0xfff)) | 1u);
}
static bool rb(void *p, uint32_t a, uint8_t *v) {
    ctx_t *c = p;
    if (a >= RAM_SIZE || a >= c->read_fault_from)
        return false;
    *v = c->ram[a];
    return true;
}
static bool rw_cb(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE || a >= c->read_fault_from)
        return false;
    if (a == c->read_fault_once) {
        c->read_fault_once = UINT32_MAX;
        return false;
    }
    *v = rw(c, a);
    return true;
}
static bool rl_cb(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    if (a + 3 >= RAM_SIZE || a >= c->read_fault_from)
        return false;
    if (a == c->read_fault_once) {
        c->read_fault_once = UINT32_MAX;
        return false;
    }
    *v = rl(c, a);
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    if (a >= RAM_SIZE)
        return false;
    c->ram[a] = v;
    return true;
}
static bool ww_cb(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    ww(c, a, v);
    return true;
}
static bool wl_cb(void *p, uint32_t a, uint32_t v) {
    return ww_cb(p, a, (uint16_t)(v >> 16)) && ww_cb(p, a + 2, (uint16_t)v);
}
static mc68k_state_t *make(ctx_t *c) {
    mc68k_bus_t b = {.read_byte = rb,
                     .read_word = rw_cb,
                     .read_long = rl_cb,
                     .write_byte = wb,
                     .write_word = ww_cb,
                     .write_long = wl_cb,
                     .ctx = c};
    return mc68k_create_model(&b, MC68K_MODEL_68040);
}
static void setup(ctx_t *c) {
    c->read_fault_from = UINT32_MAX;
    c->read_fault_once = UINT32_MAX;
    wl(c, 0, RESET_SP);
    wl(c, 4, RESET_PC);
    wl(c, 8, BUS_HANDLER);
    wl(c, 9 * 4, TRACE_HANDLER);
    wl(c, 14 * 4, FORMAT_HANDLER);
    ww(c, BUS_HANDLER, 0x60fe);
    ww(c, FORMAT_HANDLER, 0x60fe);
    ww(c, TRACE_HANDLER, 0x60fe);
}

static void test_access_error_frame(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    ww(&c, RESET_PC, 0x207c);
    wl(&c, RESET_PC + 2, HOLE);   /* MOVEA.L #hole,A0 */
    ww(&c, RESET_PC + 6, 0x4a10); /* TST.B (A0) */
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 20);
    uint32_t f = cpu->a[7];
    assert(cpu->pc == BUS_HANDLER && f == RESET_SP - 60);
    assert(rw(&c, f + 6) == 0x7008);
    assert(rl(&c, f + 8) == 0);       /* no continuation EA */
    assert(rw(&c, f + 12) == 0x0125); /* RW, byte, supervisor data TM */
    assert(rw(&c, f + 14) == 0 && rw(&c, f + 16) == 0 && rw(&c, f + 18) == 0);
    assert(rl(&c, f + 20) == HOLE);
    assert(rl(&c, f + 24) == 0 && rl(&c, f + 56) == 0);
    mc68k_destroy(cpu);
}

static void test_mmu_fault_populates_access_error_frame(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    ww(&c, RESET_PC, 0x207c);
    wl(&c, RESET_PC + 2, 0x5000);   /* MOVEA.L #$5000,A0 */
    ww(&c, RESET_PC + 6, 0x4a10);   /* TST.B (A0) */
    map_mmu_page(&c, 0);            /* vectors and handler */
    map_mmu_page(&c, RESET_PC);     /* instruction stream */
    map_mmu_page(&c, RESET_SP - 4); /* format-$7 frame */
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    cpu->mmu040_tc = 0x8000;
    cpu->mmu040_srp = cpu->mmu040_urp = MMU_ROOT;
    mc68k_set_fault_ring(cpu, 1);
    mc68k_execute(cpu, 20);
    uint32_t f = cpu->a[7];
    assert(cpu->pc == BUS_HANDLER && f == RESET_SP - 60);
    assert(rw(&c, f + 6) == 0x7008);
    assert(rl(&c, f + 8) == 0);       /* MMU faults have no continuation EA. */
    assert(rw(&c, f + 12) == 0x0525); /* ATC, read, byte, supervisor data */
    assert(rl(&c, f + 20) == 0x5000); /* FA is the logical address. */
    mc68k_fault_ring_entry_t entries[2];
    assert(mc68k_read_fault_ring(cpu, entries, 2) == 1);
    assert(entries[0].mmu_source == MC68K_FAULT_MMU_SOURCE_WALK &&
           entries[0].mmu_fault == MC68K_DEBUG_MMU_INVALID);

    /* The negative ATC entry must produce an equivalent access-error frame
     * without repeating the table walk. */
    cpu->pc = RESET_PC + 6;
    cpu->a[0] = 0x5000;
    mc68k_execute(cpu, 20);
    f = cpu->a[7];
    assert(cpu->pc == BUS_HANDLER && rw(&c, f + 12) == 0x0525 && rl(&c, f + 20) == 0x5000);
    assert(mc68k_read_fault_ring(cpu, entries, 2) == 2);
    assert(entries[1].mmu_source == MC68K_FAULT_MMU_SOURCE_ATC &&
           entries[1].mmu_fault == MC68K_DEBUG_MMU_INVALID);
    mc68k_destroy(cpu);
}

static void test_mmu_table_bus_error_populates_access_error_frame(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    ww(&c, RESET_PC, 0x207c);
    wl(&c, RESET_PC + 2, HOLE);     /* MOVEA.L #hole,A0 */
    ww(&c, RESET_PC + 6, 0x4a10);   /* TST.B (A0) */
    map_mmu_page(&c, 0);            /* vectors and handler */
    map_mmu_page(&c, RESET_PC);     /* instruction stream */
    map_mmu_page(&c, RESET_SP - 4); /* format-$7 frame */
    c.read_fault_once = MMU_POINTER + ((HOLE >> 18) & 0x7fu) * 4u;
    /* second-level descriptor for HOLE */
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    cpu->mmu040_tc = 0x8000;
    cpu->mmu040_srp = cpu->mmu040_urp = MMU_ROOT;
    mc68k_set_fault_ring(cpu, 1);
    mc68k_execute(cpu, 20);
    uint32_t f = cpu->a[7];
    assert(cpu->pc == BUS_HANDLER && rw(&c, f + 12) == 0x0525 && rl(&c, f + 20) == HOLE);
    mc68k_fault_ring_entry_t entry;
    assert(mc68k_read_fault_ring(cpu, &entry, 1) == 1);
    assert(entry.mmu_source == MC68K_FAULT_MMU_SOURCE_WALK &&
           entry.mmu_fault == MC68K_DEBUG_MMU_TABLE_BUS_ERROR &&
           entry.table_bus_error_physical == MMU_POINTER + ((HOLE >> 18) & 0x7fu) * 4u);
    mc68k_destroy(cpu);
}

static void test_ttr_write_protect_populates_access_error_frame(void) {
    enum { TTR_TARGET = 0x12005000 };
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    ww(&c, RESET_PC, 0x207c);
    wl(&c, RESET_PC + 2, TTR_TARGET); /* MOVEA.L #target,A0 */
    ww(&c, RESET_PC + 6, 0x1080);     /* MOVE.B D0,(A0) */
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    cpu->d[0] = 0xa5;
    cpu->mmu040_dtt0 = 0x1200a004; /* base=$12, supervisor, enabled, write-protect */
    mc68k_set_fault_ring(cpu, 1);
    mc68k_execute(cpu, 20);
    uint32_t f = cpu->a[7];
    assert(cpu->pc == BUS_HANDLER && rw(&c, f + 12) == 0x0425 && rl(&c, f + 20) == TTR_TARGET);
    mc68k_fault_ring_entry_t entry;
    assert(mc68k_read_fault_ring(cpu, &entry, 1) == 1);
    assert(entry.mmu_source == MC68K_FAULT_MMU_SOURCE_TT &&
           entry.mmu_fault == MC68K_DEBUG_MMU_WRITE_PROTECT);
    mc68k_destroy(cpu);
}

static void test_access_error_context(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    ww(&c, RESET_PC, 0x207c);
    wl(&c, RESET_PC + 2, HOLE);
    ww(&c, RESET_PC + 6, 0x4a10);
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    cpu->fault040.valid = true;
    cpu->fault040.effective_address = 0x12345678;
    cpu->fault040.ssw = 0x0b25;
    cpu->fault040.wb_status[0] = 0x81;
    cpu->fault040.wb_status[1] = 0x42;
    cpu->fault040.wb_status[2] = 0x23;
    cpu->fault040.fault_address = 0x23456780;
    cpu->fault040.post_vector = 52;
    for (unsigned i = 0; i < 3; ++i) {
        cpu->fault040.wb_address[i] = 0x30000000u + i;
        cpu->fault040.wb_data[i] = 0x40000000u + i;
        cpu->fault040.push_data[i] = 0x50000000u + i;
    }
    mc68k_execute(cpu, 20);
    uint32_t f = cpu->a[7];
    assert(rl(&c, f + 8) == 0x12345678 && rw(&c, f + 12) == 0x0b25);
    assert(c.ram[f + 15] == 0x81 && c.ram[f + 17] == 0x42 && c.ram[f + 19] == 0x23);
    assert(rl(&c, f + 20) == 0x23456780);
    assert(rl(&c, f + 24) == 0x30000000 && rl(&c, f + 28) == 0x40000000);
    assert(rl(&c, f + 32) == 0x30000001 && rl(&c, f + 36) == 0x40000001);
    assert(rl(&c, f + 40) == 0x30000002 && rl(&c, f + 44) == 0x40000002);
    assert(rl(&c, f + 48) == 0x50000000 && rl(&c, f + 56) == 0x50000002);
    assert(!cpu->fault040.valid);
    mc68k_destroy(cpu);
}

static void test_access_error_double_faults_halt(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    ww(&c, RESET_PC, 0x207c);
    wl(&c, RESET_PC + 2, HOLE);
    ww(&c, RESET_PC + 6, 0x4a10);

    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    cpu->a[7] = HOLE; /* The format-$7 push itself faults. */
    mc68k_execute(cpu, 20);
    assert(cpu->exec_state == CPU_HALTED && cpu->last_exception_vector == 0);
    mc68k_destroy(cpu);

    memset(&c, 0, sizeof(c));
    setup(&c);
    ww(&c, RESET_PC, 0x207c);
    wl(&c, RESET_PC + 2, HOLE);
    ww(&c, RESET_PC + 6, 0x4a10);
    cpu = make(&c);
    mc68k_reset(cpu);
    cpu->vbr = HOLE; /* The vector fetch after a complete $7 push faults. */
    mc68k_execute(cpu, 20);
    assert(cpu->exec_state == CPU_HALTED && cpu->a[7] == RESET_SP - 60);
    mc68k_destroy(cpu);
}

static void test_rte_validation_fault_is_access_error(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    ww(&c, 0x1200, 0x4e73);
    cpu->pc = 0x1200;
    cpu->a[7] = 0x7000;
    cpu->sr = SR_S;
    ww(&c, 0x7000, SR_S);
    wl(&c, 0x7002, 0x2222);
    ww(&c, 0x7006, 0x7000);
    /* The format word is readable, but the required EA read at +$08 faults. */
    c.read_fault_from = 0x7008;
    mc68k_execute(cpu, 20);
    c.read_fault_from = UINT32_MAX;
    assert(cpu->exec_state == CPU_NORMAL && cpu->pc == BUS_HANDLER && cpu->a[7] == 0x7000 - 60);
    assert(rw(&c, cpu->a[7] + 6) == 0x7008);
    mc68k_destroy(cpu);
}

static void test_movem_cm_continuation(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    /* MOVEM.L (d8,A0,D1.L),D0.  The first read faults; the handler changes
     * D1 before RTE, so only a saved CM EA can still read 0x2000. */
    ww(&c, RESET_PC, 0x4cf0);
    ww(&c, RESET_PC + 2, 0x0001);
    ww(&c, RESET_PC + 4, 0x1800);
    wl(&c, 0x2000, 0xcafebabe);
    ww(&c, BUS_HANDLER, 0x7201);
    ww(&c, BUS_HANDLER + 2, 0x4e73);
    c.read_fault_once = 0x2000;
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    cpu->a[0] = 0x2000;
    cpu->d[1] = 0;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == BUS_HANDLER && cpu->movem040_continuation_valid);
    assert(rw(&c, cpu->a[7] + 12) & 0x1000u);
    mc68k_execute(cpu, 1); /* MOVEQ #1,D1 */
    mc68k_execute(cpu, 1); /* RTE → MOVEM instruction start */
    mc68k_execute(cpu, 1); /* resumed MOVEM */
    assert(cpu->d[0] == 0xcafebabe && cpu->d[1] == 1);
    assert(!cpu->movem040_continuation_valid && !cpu->movem040_resume);
    mc68k_destroy(cpu);
}

static void test_movem_cm_restarts_at_calculated_ea(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    /* MOVEM.L (d8,A0,D1.L),D0/D2.  D2's access faults.  The handler changes
     * D1 before RTE; CM must preserve the calculated EA, so reexecution
     * repeats D0 at 0x2000 before reading D2 at 0x2004.  [040UM §8.4.6.2,
     * §8.4.6.7] */
    ww(&c, RESET_PC, 0x4cf0);
    ww(&c, RESET_PC + 2, 0x0005);
    ww(&c, RESET_PC + 4, 0x1800);
    wl(&c, 0x2000, 0x10203040);
    wl(&c, 0x2004, 0x50607080);
    wl(&c, 0x2008, 0x90a0b0c0);
    ww(&c, BUS_HANDLER, 0x7201);
    ww(&c, BUS_HANDLER + 2, 0x4e73);
    c.read_fault_once = 0x2004;
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    cpu->a[0] = 0x2000;
    cpu->d[1] = 0;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == BUS_HANDLER && cpu->movem040_continuation_valid);
    assert(rl(&c, cpu->a[7] + 8) == 0x2000);
    mc68k_execute(cpu, 1); /* MOVEQ #1,D1 */
    mc68k_execute(cpu, 1); /* RTE → MOVEM instruction start */
    mc68k_execute(cpu, 1); /* resumed MOVEM */
    assert(cpu->d[0] == 0x10203040 && cpu->d[2] == 0x50607080 && cpu->d[1] == 1);
    assert(!cpu->movem040_continuation_valid && !cpu->movem040_resume);
    mc68k_destroy(cpu);
}

static void test_cp_context_survives_handler(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    ww(&c, RESET_PC, 0x207c);
    wl(&c, RESET_PC + 2, HOLE);
    ww(&c, RESET_PC + 6, 0x4a10);
    ww(&c, BUS_HANDLER, 0x4e73);
    wl(&c, 52 * 4, FORMAT_HANDLER);
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    cpu->fault040.valid = true;
    cpu->fault040.ssw = 0x8000;
    cpu->fault040.effective_address = 0x01234567;
    cpu->fault040.post_vector = 52;
    mc68k_execute(cpu, 20); /* data fault → $7 */
    assert(cpu->pc == BUS_HANDLER && cpu->fpu040_post_pending && !cpu->fault040.valid);
    mc68k_execute(cpu, 1); /* RTE → format $3 / vector 52 */
    assert(cpu->pc == FORMAT_HANDLER && cpu->last_exception_vector == 52);
    assert(rw(&c, cpu->a[7] + 6) == 0x30d0);
    assert(rl(&c, cpu->a[7] + 8) == 0x01234567);
    mc68k_destroy(cpu);
}

static void test_integer_exception_frames(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    wl(&c, 4 * 4, FORMAT_HANDLER);
    ww(&c, RESET_PC, 0x4afc); /* ILLEGAL */
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 20);
    assert(cpu->pc == FORMAT_HANDLER && cpu->a[7] == RESET_SP - 8);
    assert(rw(&c, cpu->a[7] + 6) == 0x0010);
    mc68k_destroy(cpu);

    memset(&c, 0, sizeof(c));
    setup(&c);
    ww(&c, RESET_PC, 0x4e71); /* NOP */
    cpu = make(&c);
    mc68k_reset(cpu);
    cpu->sr = (uint16_t)(SR_S | SR_T1);
    mc68k_execute(cpu, 20);
    assert(cpu->pc == TRACE_HANDLER && cpu->a[7] == RESET_SP - 12);
    assert(rw(&c, cpu->a[7] + 6) == 0x2024);
    mc68k_destroy(cpu);
}

static void test_interrupt_throwaway_frame(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    wl(&c, 31 * 4, TRACE_HANDLER);
    ww(&c, TRACE_HANDLER, 0x4e73);
    ww(&c, RESET_PC, 0x4e71); /* NOP: return destination after the IRQ */
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    cpu->msp = 0x7800;
    cpu->a[7] = cpu->msp;
    cpu->sr = (uint16_t)(SR_S | SR_M);
    mc68k_set_irq(cpu, 7);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == TRACE_HANDLER && cpu->a[7] == RESET_SP - 8);
    assert(rw(&c, RESET_SP - 2) == 0x107c); /* $1, vector 31 */
    assert(rw(&c, 0x77fe) == 0x007c);       /* real $0 frame on MSP */
    mc68k_execute(cpu, 1);                  /* RTE $1 then RTE $0 */
    assert(cpu->pc == RESET_PC && cpu->a[7] == 0x7800);
    mc68k_destroy(cpu);
}

static void test_rte_formats(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    setup(&c);
    mc68k_state_t *cpu = make(&c);
    mc68k_reset(cpu);
    ww(&c, 0x1200, 0x4e73);
    cpu->pc = 0x1200;
    cpu->a[7] = 0x7000;
    cpu->sr = SR_S;
    ww(&c, 0x7000, SR_S);
    wl(&c, 0x7002, 0x2222);
    ww(&c, 0x7006, 0x7000);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x2222 && cpu->a[7] == 0x703c);

    /* $0/$2/$3 are the other non-throwaway formats recognized by MC68040. */
    const struct {
        uint16_t format;
        uint32_t bytes;
        uint32_t pc;
    } valid[] = {
        {0x0000, 8, 0x2400},
        {0x2000, 12, 0x2500},
        {0x3000, 12, 0x2600},
    };
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        cpu->pc = 0x1200;
        cpu->a[7] = 0x7200;
        cpu->sr = SR_S;
        ww(&c, 0x7200, SR_S);
        wl(&c, 0x7202, valid[i].pc);
        ww(&c, 0x7206, valid[i].format);
        mc68k_execute(cpu, 1);
        assert(cpu->pc == valid[i].pc && cpu->a[7] == 0x7200 + valid[i].bytes);
    }

    /* RTE validates only $0-$D, never guest-owned write-back data. */
    cpu->pc = 0x1200;
    cpu->a[7] = 0x7600;
    cpu->sr = SR_S;
    ww(&c, 0x7600, SR_S);
    wl(&c, 0x7602, 0x2700);
    ww(&c, 0x7606, 0x7000);
    wl(&c, 0x7608, 0);
    ww(&c, 0x760c, 0);
    wl(&c, 0x760e, 0xdeadbeef);
    c.read_fault_from = 0x760e;
    mc68k_execute(cpu, 1);
    c.read_fault_from = UINT32_MAX;
    assert(cpu->pc == 0x2700 && cpu->a[7] == 0x763c);

    /* CT transforms $7 to a trace format-$2 frame at old SP+$30. */
    cpu->pc = 0x1200;
    cpu->a[7] = 0x7400;
    cpu->sr = SR_S;
    ww(&c, 0x7400, 0x0000);
    wl(&c, 0x7402, 0x2a00);
    ww(&c, 0x7406, 0x7008);
    wl(&c, 0x7408, 0x12345678);
    ww(&c, 0x740c, 0x2000);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == TRACE_HANDLER && cpu->last_exception_vector == 9 && cpu->a[7] == 0x7430);
    assert(rw(&c, 0x7430) == 0x0000 && rl(&c, 0x7432) == 0x2a00);
    assert(rw(&c, 0x7436) == 0x2024 && rl(&c, 0x7438) == 0x12345678);

    /* CU becomes an unimplemented-FP format-$2 frame for vector 11. */
    wl(&c, 11 * 4, FORMAT_HANDLER);
    cpu->pc = 0x1200;
    cpu->a[7] = 0x7500;
    cpu->sr = SR_S;
    ww(&c, 0x7500, SR_S);
    wl(&c, 0x7502, 0x2b00);
    ww(&c, 0x7506, 0x7008);
    wl(&c, 0x7508, 0x89abcdef);
    ww(&c, 0x750c, 0x4000);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == FORMAT_HANDLER && cpu->last_exception_vector == 11 &&
           cpu->a[7] == 0x7530);
    assert(rw(&c, 0x7536) == 0x202c && rl(&c, 0x7538) == 0x89abcdef);

    /* CP regenerates the saved FPU post-instruction exception as format $3. */
    wl(&c, 52 * 4, FORMAT_HANDLER);
    cpu->pc = 0x1200;
    cpu->a[7] = 0x7700;
    cpu->sr = SR_S;
    cpu->fpu040_post_pending = true;
    cpu->fpu040_post_vector = 52;
    ww(&c, 0x7700, 0x0000);
    wl(&c, 0x7702, 0x2c00);
    ww(&c, 0x7706, 0x7008);
    wl(&c, 0x7708, 0x01234567);
    ww(&c, 0x770c, 0x8000);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == FORMAT_HANDLER && cpu->last_exception_vector == 52 &&
           cpu->a[7] == 0x7730);
    assert(rw(&c, 0x7736) == 0x30d0 && rl(&c, 0x7738) == 0x01234567);
    assert(rw(&c, 0x7730) == 0x0000 &&
           rl(&c, 0x7732) == 0x2c00); /* SR と PC は RTE で復元した値 */
    assert(!cpu->fpu040_post_pending);

    /* $4/$A/$B are not valid on MC68040 and therefore take vector 14. */
    const uint16_t invalid[] = {0x4000, 0xa000, 0xb000};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        cpu->pc = 0x1200;
        cpu->a[7] = 0x7100;
        cpu->sr = SR_S;
        ww(&c, 0x7100, SR_S);
        wl(&c, 0x7102, 0x3333);
        ww(&c, 0x7106, invalid[i]);
        mc68k_execute(cpu, 1);
        assert(cpu->pc == FORMAT_HANDLER && cpu->last_exception_vector == 14);
    }
    mc68k_destroy(cpu);
}

int main(void) {
    test_access_error_frame();
    test_mmu_fault_populates_access_error_frame();
    test_mmu_table_bus_error_populates_access_error_frame();
    test_ttr_write_protect_populates_access_error_frame();
    test_access_error_context();
    test_access_error_double_faults_halt();
    test_rte_validation_fault_is_access_error();
    test_movem_cm_continuation();
    test_movem_cm_restarts_at_calculated_ea();
    test_cp_context_survives_handler();
    test_integer_exception_frames();
    test_interrupt_throwaway_frame();
    test_rte_formats();
    puts("test_68040_exception: OK");
    return 0;
}
