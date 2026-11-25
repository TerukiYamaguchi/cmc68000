/*
 * test_phase3_contract.c -- Phase 3 X-axis contract tests.
 *
 * The individual feature tests remain the exhaustive regressions.  This file
 * fixes the cross-cutting MC68000 exception/privilege/interrupt contract at
 * the architectural observation point: handler entry or return from RTE.
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu_internal.h"
#include "mc68000/cpu.h"
#include "memory/bus.h"
#include "memory/memory.h"

#define RAM_SIZE  0x10000u
#define RESET_SSP 0x8000u
#define RESET_PC  0x1000u
#define USER_SP   0x7000u
#define HANDLER   0x1800u

typedef struct {
    uint8_t *ram;
    mem_ctx_t *mem;
    mem_region_t *region;
    mc68k_state_t *cpu;
} fixture_t;

static void ww(uint8_t *ram, uint32_t address, uint16_t value) {
    ram[address] = (uint8_t)(value >> 8);
    ram[address + 1] = (uint8_t)value;
}

static void wl(uint8_t *ram, uint32_t address, uint32_t value) {
    ww(ram, address, (uint16_t)(value >> 16));
    ww(ram, address + 2, (uint16_t)value);
}

static uint16_t rw(const uint8_t *ram, uint32_t address) {
    return (uint16_t)((ram[address] << 8) | ram[address + 1]);
}

static uint32_t rl(const uint8_t *ram, uint32_t address) {
    return ((uint32_t)rw(ram, address) << 16) | rw(ram, address + 2);
}

static fixture_t make_fixture(void) {
    fixture_t f = {0};
    f.ram = calloc(1, RAM_SIZE);
    f.mem = mem_create();
    f.region = calloc(1, sizeof(*f.region));
    assert(f.ram && f.mem && f.region);
    f.region->data = f.ram;
    f.region->size = RAM_SIZE;
    mem_add_region(f.mem, f.region);
    wl(f.ram, 0, RESET_SSP);
    wl(f.ram, 4, RESET_PC);
    mc68k_bus_t bus = bus_make_hooks(f.mem);
    f.cpu = mc68k_create(&bus);
    assert(f.cpu);
    mc68k_reset(f.cpu);
    return f;
}

static void dispose(fixture_t *f) {
    mc68k_destroy(f->cpu);
    mem_destroy(f->mem);
    free(f->region);
    free(f->ram);
}

static void vector(fixture_t *f, unsigned number, uint32_t handler) {
    wl(f->ram, number * 4u, handler);
}

/* T-PRV-010-01, T-EXC-010-01: user-mode ILLEGAL uses SSP, preserves the
 * pre-instruction PC, and RTE restores the user stack selection. */
static void test_user_exception_and_rte(void) {
    fixture_t f = make_fixture();
    vector(&f, 4, HANDLER);
    ww(f.ram, RESET_PC, 0x4afc); /* ILLEGAL */
    ww(f.ram, HANDLER, 0x4e73);  /* RTE */
    f.cpu->usp = USER_SP;
    f.cpu->a[7] = USER_SP;
    f.cpu->sr = 0x0000;

    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == HANDLER);
    assert(f.cpu->sr == 0x2000);
    assert(f.cpu->a[7] == RESET_SSP - 6);
    assert(rw(f.ram, RESET_SSP - 6) == 0x0000);
    assert(rl(f.ram, RESET_SSP - 4) == RESET_PC);
    assert(f.cpu->usp == USER_SP);

    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == RESET_PC);
    assert(f.cpu->sr == 0x0000);
    assert(f.cpu->a[7] == USER_SP);
    assert(f.cpu->ssp == RESET_SSP);
    dispose(&f);
}

/* T-EXC-010-02: group 2 saves PCn, unlike the pre-instruction PC saved by
 * group 1 and group 0 exceptions. */
static void test_trap_saves_next_pc(void) {
    fixture_t f = make_fixture();
    vector(&f, 35, HANDLER);
    ww(f.ram, RESET_PC, 0x4e43); /* TRAP #3 */
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == HANDLER);
    assert(f.cpu->a[7] == RESET_SSP - 6);
    assert(rw(f.ram, RESET_SSP - 6) == 0x2700);
    assert(rl(f.ram, RESET_SSP - 4) == RESET_PC + 2);
    dispose(&f);
}

/* T-EXC-020-01: address error は MC68000 の14 byte Group 0 frameを使う。 */
static void test_address_error_frame(void) {
    fixture_t f = make_fixture();
    vector(&f, 3, HANDLER);
    ww(f.ram, RESET_PC, 0x4a50); /* TST.W (A0) */
    f.cpu->a[0] = RESET_PC + 1;  /* odd word address */
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == HANDLER);
    assert(f.cpu->a[7] == RESET_SSP - 14);
    assert(rw(f.ram, RESET_SSP - 14) & 0x0010u); /* read */
    assert(rl(f.ram, RESET_SSP - 12) == RESET_PC + 1);
    assert(rw(f.ram, RESET_SSP - 8) == 0x4a50);
    assert(rw(f.ram, RESET_SSP - 6) == 0x2700);
    assert(rl(f.ram, RESET_SSP - 4) == RESET_PC + 2);
    dispose(&f);
}

/* T-EXC-040-01/02: trace follows a completed instruction but not a group-1
 * exception that interrupts it. */
static void test_trace_priority(void) {
    fixture_t f = make_fixture();
    vector(&f, 9, HANDLER);
    ww(f.ram, RESET_PC, 0x4e71); /* NOP */
    f.cpu->sr = 0xa700;          /* trace + supervisor + mask 7 */
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == HANDLER);
    assert(f.cpu->last_exception_vector == 9);
    assert(rw(f.ram, RESET_SSP - 6) == 0xa700);
    assert(rl(f.ram, RESET_SSP - 4) == RESET_PC + 2);
    dispose(&f);

    f = make_fixture();
    vector(&f, 4, HANDLER);
    vector(&f, 9, HANDLER + 0x20);
    ww(f.ram, RESET_PC, 0x4afc); /* ILLEGAL is group 1 */
    f.cpu->sr = 0xa700;
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == HANDLER);
    assert(f.cpu->last_exception_vector == 4);
    dispose(&f);
}

static mc68k_state_t *iack_cpu;
static int iack_mode;
static uint8_t iack(void *ctx, int level) {
    (void)ctx;
    assert(level == 3);
    if (iack_mode == 1) {
        mc68k_signal_bus_error(iack_cpu);
        return 99; /* BERR makes this spurious, not 99 */
    }
    return iack_mode == 2 ? 64 : 0; /* vectored or autovector */
}

/* T-IRQ-010-01..03: mask acceptance, autovector/vectored choice, and IACK
 * bus error -> spurious vector all retain the standard 6-byte frame. */
static void test_irq_vector_choices(void) {
    fixture_t f = make_fixture();
    vector(&f, 27, HANDLER);
    vector(&f, 64, HANDLER + 0x20);
    vector(&f, 24, HANDLER + 0x40);
    f.cpu->bus.iack = iack;
    iack_cpu = f.cpu;
    f.cpu->sr = 0x2000; /* accept level 3 */

    iack_mode = 0;
    mc68k_set_irq(f.cpu, 3);
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == HANDLER);
    assert(f.cpu->sr == 0x2300);
    assert(rw(f.ram, RESET_SSP - 6) == 0x2000);
    assert(rl(f.ram, RESET_SSP - 4) == RESET_PC);
    dispose(&f);

    f = make_fixture();
    vector(&f, 64, HANDLER + 0x20);
    f.cpu->bus.iack = iack;
    iack_cpu = f.cpu;
    f.cpu->sr = 0x2000;
    iack_mode = 2;
    mc68k_set_irq(f.cpu, 3);
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == HANDLER + 0x20);
    dispose(&f);

    f = make_fixture();
    vector(&f, 24, HANDLER + 0x40);
    f.cpu->bus.iack = iack;
    iack_cpu = f.cpu;
    f.cpu->sr = 0x2000;
    iack_mode = 1;
    mc68k_set_irq(f.cpu, 3);
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == HANDLER + 0x40);
    dispose(&f);

    /* T-EXC-030-02: a fault during interrupt-frame construction is terminal. */
    f = make_fixture();
    f.cpu->a[7] = RESET_SSP + 1; /* odd frame write is an address error */
    f.cpu->ssp = RESET_SSP + 1;
    f.cpu->sr = 0x2000;
    iack_mode = 0;
    mc68k_set_irq(f.cpu, 3);
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->exec_state == CPU_HALTED);
    dispose(&f);
}

/* T-IRQ-020-01, T-RST-010-01: STOP waits without fetching, an acceptable IRQ
 * wakes into its exception, and host reset reloads the two reset vectors. */
static void test_stop_wake_and_reset(void) {
    fixture_t f = make_fixture();
    vector(&f, 28, HANDLER);
    ww(f.ram, RESET_PC, 0x4e72); /* STOP #$2000 */
    ww(f.ram, RESET_PC + 2, 0x2000);
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->exec_state == CPU_STOPPED);
    assert(f.cpu->pc == RESET_PC + 4);
    assert(mc68k_execute(f.cpu, 1) == 0);

    mc68k_set_irq(f.cpu, 4);
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->exec_state == CPU_NORMAL);
    assert(f.cpu->pc == HANDLER);

    wl(f.ram, 0, 0x6ff0);
    wl(f.ram, 4, 0x1234);
    f.cpu->sr = 0;
    f.cpu->a[7] = 0x1111;
    mc68k_reset(f.cpu);
    assert(f.cpu->sr == 0x2700);
    assert(f.cpu->a[7] == 0x6ff0);
    assert(f.cpu->pc == 0x1234);
    dispose(&f);
}

int main(void) {
    test_user_exception_and_rte();
    test_trap_saves_next_pc();
    test_address_error_frame();
    test_trace_priority();
    test_irq_vector_choices();
    test_stop_wake_and_reset();
    return 0;
}
