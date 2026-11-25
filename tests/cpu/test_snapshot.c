/* test_snapshot.c — CPU snapshot の公開 API。 */
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE 0x1000u
#define RESET_PC 0x100u
#define RESET_SP 0x0F00u
#define BUS_ERR_HANDLER 0x500u
#define MASTER_SR 0x3000u
#define MSP_INIT 0x0800u
#define IRQ_FRAME_FAIL (RESET_SP - 8u)
#define FMOVE_LONG_TO_FP0 0x4000u

typedef struct {
    uint8_t ram[RAM_SIZE];
    bool fail_write;
    uint32_t fail_addr;
} ram_t;

static bool rb(void *ctx, uint32_t addr, uint8_t *out) {
    if (addr >= RAM_SIZE)
        return false;
    *out = ((ram_t *)ctx)->ram[addr];
    return true;
}
static bool rw(void *ctx, uint32_t addr, uint16_t *out) {
    ram_t *ram = ctx;
    if (addr + 1 >= RAM_SIZE)
        return false;
    *out = (uint16_t)((ram->ram[addr] << 8) | ram->ram[addr + 1]);
    return true;
}
static bool rl(void *ctx, uint32_t addr, uint32_t *out) {
    ram_t *ram = ctx;
    if (addr + 3 >= RAM_SIZE)
        return false;
    *out = ((uint32_t)ram->ram[addr] << 24) | ((uint32_t)ram->ram[addr + 1] << 16) |
           ((uint32_t)ram->ram[addr + 2] << 8) | ram->ram[addr + 3];
    return true;
}
static bool wb(void *ctx, uint32_t addr, uint8_t value) {
    ram_t *ram = ctx;
    if (addr >= RAM_SIZE || (ram->fail_write && addr == ram->fail_addr))
        return false;
    ram->ram[addr] = value;
    return true;
}
static bool ww(void *ctx, uint32_t addr, uint16_t value) {
    ram_t *ram = ctx;
    if (addr + 1 >= RAM_SIZE || (ram->fail_write && addr == ram->fail_addr))
        return false;
    ram->ram[addr] = (uint8_t)(value >> 8);
    ram->ram[addr + 1] = (uint8_t)value;
    return true;
}
static bool wl(void *ctx, uint32_t addr, uint32_t value) {
    ram_t *ram = ctx;
    if (addr + 3 >= RAM_SIZE || (ram->fail_write && addr == ram->fail_addr))
        return false;
    ram->ram[addr] = (uint8_t)(value >> 24);
    ram->ram[addr + 1] = (uint8_t)(value >> 16);
    ram->ram[addr + 2] = (uint8_t)(value >> 8);
    ram->ram[addr + 3] = (uint8_t)value;
    return true;
}
static void put_long(ram_t *ram, uint32_t addr, uint32_t value) {
    if (!wl(ram, addr, value))
        abort();
}
static void put_word(ram_t *ram, uint32_t addr, uint16_t value) {
    if (!ww(ram, addr, value))
        abort();
}

static mc68k_state_t *make_cpu(ram_t *ram, mc68k_model_t model) {
    mc68k_bus_t bus = {.ctx = ram, .read_byte = rb, .read_word = rw, .read_long = rl,
                       .write_byte = wb, .write_word = ww, .write_long = wl};
    put_long(ram, 0, RESET_SP);
    put_long(ram, 4, RESET_PC);
    put_word(ram, RESET_PC, 0x4E71);     /* NOP */
    put_word(ram, RESET_PC + 2, 0x4E71); /* NOP */
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    assert(cpu);
    mc68k_reset(cpu);
    return cpu;
}

static void test_round_trip(mc68k_model_t model) {
    ram_t ram = {0};
    mc68k_snapshot_status_t status;
    mc68k_state_t *cpu = make_cpu(&ram, model);
    mc68k_regs_t before, after;

    assert(mc68k_execute(cpu, 1) > 0);
    mc68k_get_regs(cpu, &before);
    mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu, &status);
    assert(snap && status == MC68K_SNAPSHOT_OK);
    assert(mc68k_execute(cpu, 1) > 0);
    mc68k_restore_plan_t *plan = mc68k_snapshot_prepare(cpu, snap, &status);
    assert(plan && status == MC68K_SNAPSHOT_OK);
    mc68k_snapshot_commit(cpu, snap, plan);
    mc68k_get_regs(cpu, &after);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    mc68k_snapshot_destroy(snap);
    mc68k_destroy(cpu);
}

static void test_mismatch(void) {
    ram_t a = {0}, b = {0};
    mc68k_snapshot_status_t status;
    mc68k_state_t *cpu_a = make_cpu(&a, MC68K_MODEL_68030);
    mc68k_state_t *cpu_b = make_cpu(&b, MC68K_MODEL_68040);
    mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu_a, &status);
    assert(snap && status == MC68K_SNAPSHOT_OK);
    assert(!mc68k_snapshot_prepare(cpu_b, snap, &status));
    assert(status == MC68K_SNAPSHOT_MISMATCH);
    mc68k_snapshot_destroy(snap);
    mc68k_destroy(cpu_a);
    mc68k_destroy(cpu_b);
}

static uint8_t peek_b(void *ctx, uint32_t addr) {
    return ((ram_t *)ctx)->ram[addr];
}
static uint16_t peek_w(void *ctx, uint32_t addr) {
    ram_t *ram = ctx;
    return (uint16_t)((ram->ram[addr] << 8) | ram->ram[addr + 1]);
}
static uint32_t peek_l(void *ctx, uint32_t addr) {
    ram_t *ram = ctx;
    return ((uint32_t)ram->ram[addr] << 24) | ((uint32_t)ram->ram[addr + 1] << 16) |
           ((uint32_t)ram->ram[addr + 2] << 8) | ram->ram[addr + 3];
}

static void test_stopped_round_trip(void) {
    ram_t ram = {0};
    mc68k_snapshot_status_t status;
    mc68k_state_t *cpu = make_cpu(&ram, MC68K_MODEL_68000);
    put_word(&ram, RESET_PC, 0x4E72);     /* STOP */
    put_word(&ram, RESET_PC + 2, 0x2700); /* SR */
    assert(mc68k_execute(cpu, 1) > 0);
    assert(mc68k_get_exec_state(cpu) == CPU_STOPPED);
    mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu, &status);
    assert(snap && status == MC68K_SNAPSHOT_OK);
    mc68k_reset(cpu);
    mc68k_restore_plan_t *plan = mc68k_snapshot_prepare(cpu, snap, &status);
    assert(plan && status == MC68K_SNAPSHOT_OK);
    mc68k_snapshot_commit(cpu, snap, plan);
    assert(mc68k_get_exec_state(cpu) == CPU_STOPPED);
    mc68k_snapshot_destroy(snap);
    mc68k_destroy(cpu);
}

static void test_halted_round_trip(void) {
    ram_t ram = {0};
    mc68k_snapshot_status_t status;
    mc68k_state_t *cpu = make_cpu(&ram, MC68K_MODEL_68000);
    cpu->exec_state = CPU_HALTED;
    mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu, &status);
    assert(snap && status == MC68K_SNAPSHOT_OK);
    cpu->exec_state = CPU_NORMAL;
    mc68k_restore_plan_t *plan = mc68k_snapshot_prepare(cpu, snap, &status);
    assert(plan && status == MC68K_SNAPSHOT_OK);
    mc68k_snapshot_commit(cpu, snap, plan);
    assert(mc68k_get_exec_state(cpu) == CPU_HALTED);
    mc68k_snapshot_destroy(snap);
    mc68k_destroy(cpu);
}

static void test_debug_rebase_and_history_clear(void) {
    ram_t ram = {0};
    mc68k_state_t *cpu = make_cpu(&ram, MC68K_MODEL_68000);
    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, &ram);
    assert(mc68k_add_watchpoint(cpu, 0x200, 1) == 0);
    ram.ram[0x200] = 1;
    assert(mc68k_watchpoint_check(cpu) == 0);
    ram.ram[0x200] = 2;
    mc68k_rearm_watchpoints(cpu);
    assert(mc68k_watchpoint_check(cpu) == -1);
    mc68k_set_pc_ring(cpu, 1);
    assert(mc68k_execute(cpu, 1) > 0);
    assert(mc68k_read_pc_ring(cpu, NULL, 0) == 1);
    mc68k_clear_history_rings(cpu);
    assert(mc68k_get_pc_ring(cpu) == 1);
    assert(mc68k_read_pc_ring(cpu, NULL, 0) == 0);
    mc68k_destroy(cpu);
}

static void test_pending_group0_round_trip(void) {
    ram_t ram = {0};
    mc68k_snapshot_status_t status;
    mc68k_state_t *cpu = make_cpu(&ram, MC68K_MODEL_68020);
    put_long(&ram, 8, BUS_ERR_HANDLER);
    put_word(&ram, RESET_PC, 0x46FC);      /* MOVE.W #imm,SR */
    put_word(&ram, RESET_PC + 2, MASTER_SR);
    put_word(&ram, RESET_PC + 4, 0x4E71); /* NOP */
    put_word(&ram, BUS_ERR_HANDLER, 0x4E71);
    cpu->msp = MSP_INIT;
    assert(mc68k_execute(cpu, 1) > 0);
    ram.fail_write = true;
    ram.fail_addr = IRQ_FRAME_FAIL;
    mc68k_set_irq(cpu, 1);
    assert(mc68k_execute(cpu, 1) > 0);
    assert(mc68k_get_exec_state(cpu) == CPU_EXCEPTION);
    mc68k_set_irq(cpu, 0);
    ram.fail_write = false;
    mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu, &status);
    assert(snap && status == MC68K_SNAPSHOT_OK);
    assert(mc68k_execute(cpu, 1) > 0);
    assert(mc68k_get_exec_state(cpu) == CPU_NORMAL);
    assert(mc68k_get_pc(cpu) == BUS_ERR_HANDLER);
    mc68k_restore_plan_t *plan = mc68k_snapshot_prepare(cpu, snap, &status);
    assert(plan && status == MC68K_SNAPSHOT_OK);
    mc68k_snapshot_commit(cpu, snap, plan);
    assert(mc68k_get_exec_state(cpu) == CPU_EXCEPTION);
    assert(mc68k_execute(cpu, 1) > 0);
    assert(mc68k_get_exec_state(cpu) == CPU_NORMAL);
    assert(mc68k_get_pc(cpu) == BUS_ERR_HANDLER);
    mc68k_snapshot_destroy(snap);
    mc68k_destroy(cpu);
}

static void test_cpu_owned_fpu_round_trip(void) {
    ram_t ram = {0};
    mc68k_snapshot_status_t status;
    mc68k_bus_t bus = {.ctx = &ram, .read_byte = rb, .read_word = rw, .read_long = rl,
                       .write_byte = wb, .write_word = ww, .write_long = wl};
    mc68k_state_t *cpu = mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68020, MC68K_FPU_68881);
    assert(cpu && mc68k_get_fpu(cpu));
    mc68k_fpu_t *fpu = mc68k_get_fpu(cpu);
    assert(mc68k_fpu_write_command_word(fpu, FMOVE_LONG_TO_FP0));
    assert(mc68k_fpu_read_response(fpu).kind == MC68K_FPU_RESPONSE_EA_TRANSFER);
    assert(mc68k_fpu_write_operand(fpu, 7));
    mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu, &status);
    assert(snap && status == MC68K_SNAPSHOT_OK);
    assert(mc68k_fpu_write_command_word(fpu, FMOVE_LONG_TO_FP0));
    (void)mc68k_fpu_read_response(fpu);
    assert(mc68k_fpu_write_operand(fpu, 42));
    mc68k_restore_plan_t *plan = mc68k_snapshot_prepare(cpu, snap, &status);
    assert(plan && status == MC68K_SNAPSHOT_OK);
    mc68k_snapshot_commit(cpu, snap, plan);
    mc68k_fpu_regs_t regs;
    mc68k_get_fpu_regs(fpu, &regs);
    assert(regs.fp[0].sign_exp == 0x4001);
    assert(regs.fp[0].signif == UINT64_C(0xE000000000000000));
    mc68k_snapshot_destroy(snap);
    mc68k_destroy(cpu);
}

static void test_integrated_fpu_and_mmu_round_trip(void) {
    const mc68k_model_t models[] = {MC68K_MODEL_68030, MC68K_MODEL_68040, MC68K_MODEL_68060};
    const mc68k_fpu_config_t fpus[] = {MC68K_FPU_NONE, MC68K_FPU_68040, MC68K_FPU_68060};
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); ++i) {
        ram_t ram = {0};
        mc68k_snapshot_status_t status;
        mc68k_bus_t bus = {.ctx = &ram, .read_byte = rb, .read_word = rw, .read_long = rl,
                           .write_byte = wb, .write_word = ww, .write_long = wl};
        mc68k_state_t *cpu = mc68k_create_model_with_fpu(&bus, models[i], fpus[i]);
        assert(cpu);
        cpu->mmu_tc = 0x12345678u;
        cpu->atc[0].valid = true;
        cpu->mmu040_tc = 0x8123u;
        cpu->icache040[0][0].valid = true;
        cpu->icache040[0][0].data[0] = 0xA5;
        cpu->mmu060_tc = 0x56789ABCu;
        cpu->atc060_i[0].valid = true;
        cpu->t060_bc_tag[0] = 0x11111111u;
        if (cpu->fpu040) {
            cpu->fpu040->state.fp[0].sign_exp = 0x4001;
            cpu->fpu040->state.fp[0].signif = UINT64_C(0xE000000000000000);
        }
        mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu, &status);
        assert(snap && status == MC68K_SNAPSHOT_OK);
        memset(cpu->atc, 0, sizeof(cpu->atc));
        memset(cpu->icache040, 0, sizeof(cpu->icache040));
        memset(cpu->atc060_i, 0, sizeof(cpu->atc060_i));
        cpu->mmu_tc = cpu->mmu040_tc = cpu->mmu060_tc = 0;
        cpu->t060_bc_tag[0] = 0;
        if (cpu->fpu040)
            memset(cpu->fpu040, 0, sizeof(*cpu->fpu040));
        mc68k_restore_plan_t *plan = mc68k_snapshot_prepare(cpu, snap, &status);
        assert(plan && status == MC68K_SNAPSHOT_OK);
        mc68k_snapshot_commit(cpu, snap, plan);
        assert(cpu->mmu_tc == 0x12345678u && cpu->atc[0].valid);
        assert(cpu->mmu040_tc == 0x8123u && cpu->icache040[0][0].data[0] == 0xA5);
        assert(cpu->mmu060_tc == 0x56789ABCu && cpu->atc060_i[0].valid);
        assert(cpu->t060_bc_tag[0] == 0x11111111u);
        if (cpu->fpu040)
            assert(cpu->fpu040->state.fp[0].signif == UINT64_C(0xE000000000000000));
        mc68k_snapshot_destroy(snap);
        mc68k_destroy(cpu);
    }
}

static void entry_probe_counter(mc68k_state_t *cpu, unsigned id, uint32_t pc, void *ctx) {
    (void)cpu;
    (void)id;
    (void)pc;
    ++*(int *)ctx;
}

static void test_observer_and_origin_preservation(void) {
    ram_t ram = {0};
    mc68k_snapshot_status_t status;
    mc68k_state_t *cpu = make_cpu(&ram, MC68K_MODEL_68020);
    uint32_t probe_pc = RESET_PC;
    int probe_count = 0;
    assert(mc68k_add_breakpoint(cpu, RESET_PC, NULL, NULL) == 0);
    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, &ram);
    assert(mc68k_add_watchpoint(cpu, 0x200, 1) == 0);
    mc68k_set_exception_break_mask(cpu, UINT64_C(1) << 4);
    assert(mc68k_set_entry_probes(cpu, &probe_pc, 1, entry_probe_counter, &probe_count) == 0);
    mc68k_exception_frame_origin_t *origin = calloc(1, sizeof(*origin));
    assert(origin);
    origin->saved_pc = 0x12345678u;
    origin->bound = true;
    cpu->exception_frame_origins = origin;
    mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu, &status);
    assert(snap && status == MC68K_SNAPSHOT_OK);
    origin->saved_pc = 0;
    mc68k_restore_plan_t *plan = mc68k_snapshot_prepare(cpu, snap, &status);
    assert(plan && status == MC68K_SNAPSHOT_OK);
    mc68k_snapshot_commit(cpu, snap, plan);
    assert(cpu->exception_frame_origins->saved_pc == 0x12345678u);
    assert(mc68k_breakpoint_hit(cpu) == 0);
    assert(mc68k_get_exception_break_mask(cpu) == (UINT64_C(1) << 4));
    assert(mc68k_execute(cpu, 1) > 0);
    assert(probe_count == 1);
    mc68k_snapshot_destroy(snap);
    mc68k_destroy(cpu);
}

static void test_prepare_nomem_is_unchanged(void) {
    ram_t ram = {0};
    mc68k_snapshot_status_t status;
    mc68k_state_t *cpu = make_cpu(&ram, MC68K_MODEL_68020);
    mc68k_exception_frame_origin_t *origin = calloc(1, sizeof(*origin));
    assert(origin);
    origin->saved_pc = 0xCAFEBABEu;
    cpu->exception_frame_origins = origin;
    mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu, &status);
    assert(snap && status == MC68K_SNAPSHOT_OK);
    mc68k_state_t before = *cpu;
    cpu_snapshot_test_fail_alloc_after(0);
    assert(!mc68k_snapshot_prepare(cpu, snap, &status));
    assert(status == MC68K_SNAPSHOT_NOMEM);
    assert(memcmp(&before, cpu, sizeof(before)) == 0);
    cpu_snapshot_test_fail_alloc_after(-1);
    mc68k_snapshot_destroy(snap);
    mc68k_destroy(cpu);
}

int main(void) {
    test_round_trip(MC68K_MODEL_68000);
    test_round_trip(MC68K_MODEL_68030);
    test_round_trip(MC68K_MODEL_68040);
    test_round_trip(MC68K_MODEL_68060);
    test_mismatch();
    test_stopped_round_trip();
    test_halted_round_trip();
    test_debug_rebase_and_history_clear();
    assert(cpu_snapshot_ranges_valid());
    test_pending_group0_round_trip();
    test_cpu_owned_fpu_round_trip();
    test_integrated_fpu_and_mmu_round_trip();
    test_observer_and_origin_preservation();
    test_prepare_nomem_is_unchanged();
    return 0;
}
