/* Non-stopping function-entry probes (). */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu_internal.h"
#include "mc68000/cpu.h"
#include "memory/bus.h"
#include "memory/memory.h"

#define RAM_SIZE   0x10000u
#define RESET_SSP  0x00008000u
#define RESET_PC   0x00001000u
#define VECTOR4_PC 0x00002000u

static void write_word(uint8_t *ram, uint32_t addr, uint16_t value) {
    ram[addr] = (uint8_t)(value >> 8);
    ram[addr + 1] = (uint8_t)value;
}

static void write_long(uint8_t *ram, uint32_t addr, uint32_t value) {
    ram[addr] = (uint8_t)(value >> 24);
    ram[addr + 1] = (uint8_t)(value >> 16);
    ram[addr + 2] = (uint8_t)(value >> 8);
    ram[addr + 3] = (uint8_t)value;
}

static mc68k_state_t *make_cpu(uint8_t *ram) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(region);
    region->data = ram;
    region->base = 0;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create(&bus);
    assert(cpu);
    return cpu;
}

typedef struct {
    unsigned count;
    unsigned ids[4];
    uint32_t pcs[4];
    uint32_t d0_before[4];
    uint32_t pc_after_fetch[4];
} probe_log_t;

static void record_probe(mc68k_state_t *cpu, unsigned id, uint32_t entry_pc, void *ctx) {
    probe_log_t *log = ctx;
    assert(log->count < 4);
    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);
    log->ids[log->count] = id;
    log->pcs[log->count] = entry_pc;
    log->d0_before[log->count] = regs.d[0];
    log->pc_after_fetch[log->count++] = regs.pc;
}

static void test_hits_before_instruction_and_keeps_running(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long(ram, 0, RESET_SSP);
    write_long(ram, 4, RESET_PC);
    write_word(ram, RESET_PC, 0x7001);     /* MOVEQ #1,D0 */
    write_word(ram, RESET_PC + 2, 0x4E71); /* NOP */
    write_word(ram, RESET_PC + 4, 0x4E71); /* NOP */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    const uint32_t pcs[] = {RESET_PC, RESET_PC + 4};
    probe_log_t log = {0};
    assert(mc68k_set_entry_probes(cpu, pcs, 2, record_probe, &log) == 0);
    mc68k_execute(cpu, 12);

    assert(log.count == 2);
    assert(log.ids[0] == 0 && log.pcs[0] == RESET_PC);
    assert(log.ids[1] == 1 && log.pcs[1] == RESET_PC + 4);
    assert(log.d0_before[0] == 0);
    assert(log.pc_after_fetch[0] == RESET_PC + 2);
    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);
    assert(regs.d[0] == 1);
    assert(regs.pc == RESET_PC + 6);
    mc68k_destroy(cpu);
}

static void test_same_pc_probes_all_fire_in_registration_order(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long(ram, 0, RESET_SSP);
    write_long(ram, 4, RESET_PC);
    write_word(ram, RESET_PC, 0x4E71);     /* NOP */
    write_word(ram, RESET_PC + 2, 0x4E71); /* NOP */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    const uint32_t pcs[] = {RESET_PC, RESET_PC + 2, RESET_PC, RESET_PC};
    probe_log_t log = {0};
    assert(mc68k_set_entry_probes(cpu, pcs, 4, record_probe, &log) == 0);
    mc68k_execute(cpu, 8);

    assert(log.count == 4);
    assert(log.ids[0] == 0 && log.pcs[0] == RESET_PC);
    assert(log.ids[1] == 2 && log.pcs[1] == RESET_PC);
    assert(log.ids[2] == 3 && log.pcs[2] == RESET_PC);
    assert(log.ids[3] == 1 && log.pcs[3] == RESET_PC + 2);
    mc68k_destroy(cpu);
}

static void test_clear_restores_direct_table_and_forced_illegal(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long(ram, 0, RESET_SSP);
    write_long(ram, 4, RESET_PC);
    write_long(ram, 4 * 4, VECTOR4_PC);
    write_word(ram, RESET_PC, 0x4E71); /* NOP */

    mc68k_state_t *cpu = make_cpu(ram);
    op_handler_t original = cpu->op_table[0x4E71];
    const uint32_t pcs[] = {RESET_PC};
    probe_log_t log = {0};
    assert(mc68k_set_entry_probes(cpu, pcs, 1, record_probe, &log) == 0);
    assert(cpu->op_table[0x4E71] != original);
    mc68k_set_opcode_illegal(cpu, 0x4E71);
    mc68k_clear_entry_probes(cpu);
    assert(cpu->op_table[0x4E71] != original);

    mc68k_reset(cpu);
    mc68k_execute(cpu, 34);
    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);
    assert(regs.pc == VECTOR4_PC);
    assert(log.count == 0);
    mc68k_destroy(cpu);
}

static void test_rejects_invalid_definitions(void) {
    uint8_t ram[RAM_SIZE] = {0};
    mc68k_state_t *cpu = make_cpu(ram);
    const uint32_t odd[] = {RESET_PC + 1};
    const uint32_t valid[] = {RESET_PC};
    assert(mc68k_set_entry_probes(cpu, NULL, 1, record_probe, NULL) == -1);
    assert(mc68k_set_entry_probes(cpu, valid, 0, record_probe, NULL) == -1);
    assert(mc68k_set_entry_probes(cpu, valid, 1, NULL, NULL) == -1);
    assert(mc68k_set_entry_probes(cpu, odd, 1, record_probe, NULL) == -1);
    mc68k_destroy(cpu);
}

int main(void) {
    test_hits_before_instruction_and_keeps_running();
    test_same_pc_probes_all_fire_in_registration_order();
    test_clear_restores_direct_table_and_forced_illegal();
    test_rejects_invalid_definitions();
    return 0;
}
