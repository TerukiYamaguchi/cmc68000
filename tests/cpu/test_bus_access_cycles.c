/* MC68060 の bus callback 時刻公開 API の回帰試験。 */
#include "mc68000/cpu.h"
#include <assert.h>
#include <string.h>

enum { RAM_SIZE = 0x4000, PROG_PC = 0x100, SSP = 0x3000, LOG_MAX = 32, WAIT_CYCLES = 3 };

#define EXPECT_FIRST_FETCH  UINT64_C(0)
#define EXPECT_STOP_FETCH   UINT64_C(12)
#define EXPECT_STOP_OPERAND UINT64_C(15)

typedef struct {
    uint8_t ram[RAM_SIZE];
    mc68k_state_t *cpu;
    uint64_t cycles[LOG_MAX];
    unsigned log_count;
    bool expect_available;
} context_t;

static uint16_t read_word_raw(const uint8_t *ram, uint32_t addr) {
    return (uint16_t)((ram[addr] << 8) | ram[addr + 1]);
}
static void write_word_raw(uint8_t *ram, uint32_t addr, uint16_t value) {
    ram[addr] = (uint8_t)(value >> 8);
    ram[addr + 1] = (uint8_t)value;
}
static void write_long_raw(uint8_t *ram, uint32_t addr, uint32_t value) {
    write_word_raw(ram, addr, (uint16_t)(value >> 16));
    write_word_raw(ram, addr + 2, (uint16_t)value);
}
static void record_access(context_t *ctx) {
    uint64_t value = UINT64_C(0xfeedface);
    bool available = mc68k_bus_access_cycles(ctx->cpu, &value);
    assert(available == ctx->expect_available);
    if (!available) {
        assert(value == UINT64_C(0xfeedface));
        return;
    }
    assert(ctx->log_count < LOG_MAX);
    ctx->cycles[ctx->log_count++] = value;
}
static bool read_byte(void *p, uint32_t addr, uint8_t *out) {
    context_t *ctx = p;
    record_access(ctx);
    *out = ctx->ram[addr];
    return true;
}
static bool read_word(void *p, uint32_t addr, uint16_t *out) {
    context_t *ctx = p;
    record_access(ctx);
    *out = read_word_raw(ctx->ram, addr);
    return true;
}
static bool read_long(void *p, uint32_t addr, uint32_t *out) {
    context_t *ctx = p;
    record_access(ctx);
    *out = ((uint32_t)ctx->ram[addr] << 24) | ((uint32_t)ctx->ram[addr + 1] << 16) |
           ((uint32_t)ctx->ram[addr + 2] << 8) | ctx->ram[addr + 3];
    return true;
}
static bool write_byte(void *p, uint32_t addr, uint8_t value) {
    context_t *ctx = p;
    record_access(ctx);
    ctx->ram[addr] = value;
    return true;
}
static bool write_word(void *p, uint32_t addr, uint16_t value) {
    context_t *ctx = p;
    record_access(ctx);
    write_word_raw(ctx->ram, addr, value);
    return true;
}
static bool write_long(void *p, uint32_t addr, uint32_t value) {
    context_t *ctx = p;
    record_access(ctx);
    write_long_raw(ctx->ram, addr, value);
    return true;
}
static void access_timing(void *p, const mc68k_bus_access_t *access, mc68k_bus_timing_t *out) {
    (void)p;
    (void)access;
    out->wait_cycles = WAIT_CYCLES;
    out->cacheable = false;
}
static void prepare_program(context_t *ctx) {
    memset(ctx->ram, 0, sizeof(ctx->ram));
    write_long_raw(ctx->ram, 0, SSP);
    write_long_raw(ctx->ram, 4, PROG_PC);
    write_word_raw(ctx->ram, PROG_PC, 0x4e71);     /* NOP */
    write_word_raw(ctx->ram, PROG_PC + 2, 0x4e72); /* STOP */
    write_word_raw(ctx->ram, PROG_PC + 4, 0x2700);
}
static void run_case(mc68k_model_t model, bool expect_available) {
    context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.expect_available = false;
    prepare_program(&ctx);
    mc68k_bus_t bus = {.read_byte = read_byte,
                       .read_word = read_word,
                       .read_long = read_long,
                       .write_byte = write_byte,
                       .write_word = write_word,
                       .write_long = write_long,
                       .ctx = &ctx,
                       .access_timing = access_timing};
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    assert(cpu);
    ctx.cpu = cpu;
    mc68k_reset(cpu);
    ctx.log_count = 0; /* reset の raw read は公開対象外 */
    ctx.expect_available = expect_available;
    uint64_t outside = UINT64_C(0x12345678);
    assert(!mc68k_bus_access_cycles(cpu, &outside));
    assert(outside == UINT64_C(0x12345678));
    (void)mc68k_execute(cpu, 100);
    if (expect_available) {
        assert(ctx.log_count >= 3);
        for (unsigned i = 1; i < ctx.log_count; i++)
            assert(ctx.cycles[i - 1] <= ctx.cycles[i]);
        /* 060-IB-1: NOP=9 cycle と先行 callback の wait=3 を足す。 */
        assert(ctx.cycles[0] == EXPECT_FIRST_FETCH);
        assert(ctx.cycles[1] == EXPECT_STOP_FETCH);
        assert(ctx.cycles[2] == EXPECT_STOP_OPERAND);
    } else {
        assert(ctx.log_count == 0);
    }
    mc68k_destroy(cpu);
}
int main(void) {
    uint64_t value = UINT64_C(0x55);
    assert(!mc68k_bus_access_cycles(NULL, &value));
    assert(value == UINT64_C(0x55));
    assert(!mc68k_bus_access_cycles(NULL, NULL));
    run_case(MC68K_MODEL_68060, true);
    run_case(MC68K_MODEL_68000, false);
    return 0;
}
