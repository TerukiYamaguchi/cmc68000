/*
 * test_fetch_word_fault_stop.c — 上位語の取得失敗後に下位語を取得しないこと (mc68000#121)
 *
 * MC68000 以外のモデルでも、BSR.L/Bcc.L と long 即値の上位語がバスエラーに
 * なったら、下位語の取得を行わない。上位語のアドレスへの read が 1 回だけで、
 * 下位語のアドレスへは read が出ないこと、vector 2 へ進むことを確かめる。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/bus_hooks.h"
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"

#define RAM_SIZE        0x10000u
#define RESET_SSP       0x00008000u
#define RESET_PC        0x00001000u
#define HI_WORD_ADDR    (RESET_PC + 2u)
#define LO_WORD_ADDR    (RESET_PC + 4u)
#define BUS_ERR_HANDLER 0x00000300u
#define BUS_ERR_VECTOR  2u
#define MAX_STEPS       16

typedef struct {
    uint8_t ram[RAM_SIZE];
    uint32_t fail_at;  /* このアドレスの読みを失敗させる */
    unsigned hi_reads; /* 上位語アドレスへの read 回数 */
    unsigned lo_reads; /* 下位語アドレスへの read 回数 */
} ctx_t;

static void put_word(ctx_t *t, uint32_t a, uint16_t v) {
    t->ram[a] = (uint8_t)(v >> 8);
    t->ram[a + 1] = (uint8_t)v;
}

static void put_long(ctx_t *t, uint32_t a, uint32_t v) {
    put_word(t, a, (uint16_t)(v >> 16));
    put_word(t, a + 2, (uint16_t)v);
}

static bool bus_read_byte(void *ctx, uint32_t a, uint8_t *out) {
    ctx_t *t = ctx;
    if (a >= RAM_SIZE) {
        *out = 0xff;
        return false;
    }
    *out = t->ram[a];
    return true;
}

static bool bus_read_word(void *ctx, uint32_t a, uint16_t *out) {
    ctx_t *t = ctx;
    if (a == HI_WORD_ADDR)
        t->hi_reads++;
    if (a == LO_WORD_ADDR)
        t->lo_reads++;
    if (a == t->fail_at || a + 1 >= RAM_SIZE) {
        *out = 0xffff;
        return false;
    }
    *out = (uint16_t)((t->ram[a] << 8) | t->ram[a + 1]);
    return true;
}

static bool bus_read_long(void *ctx, uint32_t a, uint32_t *out) {
    uint16_t hi = 0, lo = 0;
    if (!bus_read_word(ctx, a, &hi) || !bus_read_word(ctx, a + 2, &lo)) {
        *out = 0xffffffffu;
        return false;
    }
    *out = ((uint32_t)hi << 16) | lo;
    return true;
}

static bool bus_write_byte(void *ctx, uint32_t a, uint8_t v) {
    ctx_t *t = ctx;
    if (a >= RAM_SIZE)
        return false;
    t->ram[a] = v;
    return true;
}

static bool bus_write_word(void *ctx, uint32_t a, uint16_t v) {
    return bus_write_byte(ctx, a, (uint8_t)(v >> 8)) && bus_write_byte(ctx, a + 1, (uint8_t)v);
}

static bool bus_write_long(void *ctx, uint32_t a, uint32_t v) {
    return bus_write_word(ctx, a, (uint16_t)(v >> 16)) &&
           bus_write_word(ctx, a + 2, (uint16_t)v);
}

static void bus_sync(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}

/* opcode + 上位語/下位語 の組を RESET_PC に置き、fail_at で失敗させて実行する。 */
static void run_case(mc68k_model_t model, uint16_t opcode, uint32_t imm32, uint32_t fail_at,
                     const char *name) {
    ctx_t *t = calloc(1, sizeof *t);
    assert(t != NULL);
    put_long(t, 0x0, RESET_SSP);
    put_long(t, 0x4, RESET_PC);
    put_long(t, BUS_ERR_VECTOR * 4u, BUS_ERR_HANDLER);
    put_word(t, BUS_ERR_HANDLER, 0x60fe); /* BRA * */
    put_word(t, RESET_PC, opcode);
    put_long(t, RESET_PC + 2u, imm32);
    t->fail_at = fail_at;

    mc68k_bus_t bus = {
        .read_byte = bus_read_byte,
        .read_word = bus_read_word,
        .read_long = bus_read_long,
        .write_byte = bus_write_byte,
        .write_word = bus_write_word,
        .write_long = bus_write_long,
        .sync = bus_sync,
        .ctx = t,
    };
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    assert(cpu != NULL);
    mc68k_reset(cpu);
    t->hi_reads = t->lo_reads = 0;
    for (int i = 0; i < MAX_STEPS; ++i)
        mc68k_execute(cpu, 1);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == BUS_ERR_HANDLER);
    assert(t->hi_reads == 1);
    assert(t->lo_reads == 0);
    mc68k_destroy(cpu);
    free(t);
    printf("  %s: OK\n", name);
}

int main(void) {
    static const mc68k_model_t models[] = {MC68K_MODEL_68020, MC68K_MODEL_68030,
                                           MC68K_MODEL_68040, MC68K_MODEL_68060};
    static const struct {
        uint16_t opcode;
        const char *name;
    } ops[] = {
        {0x61ff, "BSR.L"},  {0x60ff, "BRA.L"},  {0x67ff, "BEQ.L"},
        {0x0680, "ADDI.L"}, {0x0480, "SUBI.L"}, {0x0c80, "CMPI.L"},
        {0x0280, "ANDI.L"}, {0x0080, "ORI.L"},  {0x0a80, "EORI.L"},
    };
    for (unsigned m = 0; m < sizeof models / sizeof models[0]; ++m) {
        printf("model %u\n", (unsigned)models[m]);
        for (unsigned o = 0; o < sizeof ops / sizeof ops[0]; ++o)
            run_case(models[m], ops[o].opcode, 0x00000100u, HI_WORD_ADDR, ops[o].name);
    }
    printf("test_fetch_word_fault_stop: all tests passed\n");
    return 0;
}
