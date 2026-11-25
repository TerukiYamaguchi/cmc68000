/*
 * test_debug_run.c — mc68k_debug_run()のユニット
 * テスト。mc68k_execute(cpu,1)を内部で1命令ずつ回し、BP/停止状態/例外
 * ブレークマスクをチェックして早期停止する実行制御APIを検証する。
 *
 * 検証内容:
 *   1. ブレークポイントヒットで停止する
 *   2. STOP命令によるCPU_STOPPEDで停止する（stop_reasonはNONEのまま）
 *   3. ダブルフォルト（CPU_HALTED）で停止する
 *   4. 例外ブレークマスクが既定オフのとき、TRAP等の例外が発生しても
 *      debug_runは素通りする（ホスト側ループ構造を壊さない要件の確認）
 *   5. 例外ブレークマスクで該当ベクタを有効化すると、その例外で停止する
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "mc68000/debug.h"

#define RAM_SIZE     0x10000u
#define RESET_SSP    0x00008000u
#define RESET_PC     0x00001000u
#define NOP_OP       0x4E71u
#define HOLE_SSP     0x00800000u /* 未マップ領域（ダブルフォルト誘発用） */
#define HOLE_ADDR    0x00900000u
#define TRAP0_OP     0x4E40u
#define TRAP0_VECTOR 32u
#define HANDLER_ADDR 0x00002000u

typedef struct {
    uint8_t ram[RAM_SIZE];
    mc68k_state_t *cpu;
} test_ctx_t;

static bool rd_b(void *ctx, uint32_t addr, uint8_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr >= RAM_SIZE) {
        *out = 0xFF;
        return false;
    }
    *out = t->ram[addr];
    return true;
}
static bool rd_w(void *ctx, uint32_t addr, uint16_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr + 1 >= RAM_SIZE) {
        *out = 0xFFFF;
        return false;
    }
    *out = (uint16_t)((t->ram[addr] << 8) | t->ram[addr + 1]);
    return true;
}
static bool rd_l(void *ctx, uint32_t addr, uint32_t *out) {
    if (addr + 3 >= RAM_SIZE) {
        *out = 0xFFFFFFFF;
        return false;
    }
    uint16_t hi = 0, lo = 0;
    rd_w(ctx, addr, &hi);
    rd_w(ctx, addr + 2, &lo);
    *out = ((uint32_t)hi << 16) | lo;
    return true;
}
static bool wr_b(void *ctx, uint32_t addr, uint8_t v) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr >= RAM_SIZE)
        return false;
    t->ram[addr] = v;
    return true;
}
static bool wr_w(void *ctx, uint32_t addr, uint16_t v) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr + 1 >= RAM_SIZE)
        return false;
    t->ram[addr] = (uint8_t)(v >> 8);
    t->ram[addr + 1] = (uint8_t)v;
    return true;
}
static bool wr_l(void *ctx, uint32_t addr, uint32_t v) {
    bool ok1 = wr_w(ctx, addr, (uint16_t)(v >> 16));
    bool ok2 = wr_w(ctx, addr + 2, (uint16_t)v);
    return ok1 && ok2;
}
static void sync_cb(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}
static void wh_cb(void *ctx, uint32_t addr, uint32_t size) {
    (void)ctx;
    (void)addr;
    (void)size;
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t val) {
    ram[addr + 0] = (uint8_t)(val >> 24);
    ram[addr + 1] = (uint8_t)(val >> 16);
    ram[addr + 2] = (uint8_t)(val >> 8);
    ram[addr + 3] = (uint8_t)(val >> 0);
}

static mc68k_state_t *make_cpu(test_ctx_t *t) {
    mc68k_bus_t bus = {
        .read_byte = rd_b,
        .read_word = rd_w,
        .read_long = rd_l,
        .write_byte = wr_b,
        .write_word = wr_w,
        .write_long = wr_l,
        .sync = sync_cb,
        .write_hook = wh_cb,
        .iack = NULL,
        .reset_peripheral = NULL,
        .ctx = t,
    };
    t->cpu = mc68k_create(&bus);
    return t->cpu;
}

/* 1. ブレークポイントヒットで停止する */
static void test_stops_at_breakpoint(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    for (int i = 0; i < 4; i++)
        wr_w(&t, RESET_PC + (uint32_t)i * 2, NOP_OP);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_add_breakpoint(cpu, RESET_PC + 4, NULL, NULL);

    mc68k_debug_run(cpu, 1000);

    assert(mc68k_get_stop_reason(cpu) == MC68K_STOP_BREAKPOINT);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == RESET_PC + 4); /* BPアドレスの命令はまだ実行されていない */
    assert(mc68k_get_last_exception_vector(cpu) == -1); /* 例外なし */

    mc68k_destroy(cpu);
}

/* 2. STOP命令によるCPU_STOPPEDで停止する（stop_reasonはNONEのまま） */
static void test_stops_at_stop_instruction(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    wr_w(&t, RESET_PC, NOP_OP);
    wr_w(&t, RESET_PC + 2, 0x4E72); /* STOP #imm */
    wr_w(&t, RESET_PC + 4, 0x2700); /* imm = SR_RESET相当（S=1,mask=7） */
    wr_w(&t, RESET_PC + 6, NOP_OP); /* 到達しないはず */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);

    mc68k_debug_run(cpu, 1000);

    assert(mc68k_get_exec_state(cpu) == CPU_STOPPED);
    assert(mc68k_get_stop_reason(cpu) == MC68K_STOP_NONE);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == RESET_PC + 6); /* NOP(+2) + STOP opcode(+2) + 即値(+2) */

    mc68k_destroy(cpu);
}

/* 3. ダブルフォルト（CPU_HALTED）で停止する。
 * SSPを未マップ領域に置き、通常のバスエラー例外ディスパッチ自体が
 * さらにバスエラーを起こす状況を作る。 */
static void test_stops_at_halted(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, HOLE_SSP); /* 未マップSSP */
    write_long_be(t.ram, 0x000004, RESET_PC);

    uint32_t p = RESET_PC;
    wr_w(&t, p, 0x207C);
    p += 2; /* MOVEA.L #imm,A0 */
    write_long_be(t.ram, p, HOLE_ADDR);
    p += 4;
    wr_w(&t, p, 0x4A10); /* TST.B (A0) — 未マップ読み出しでバスエラー */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);

    mc68k_debug_run(cpu, 1000);

    assert(mc68k_get_exec_state(cpu) == CPU_HALTED);
    assert(mc68k_get_stop_reason(cpu) == MC68K_STOP_HALTED);

    mc68k_destroy(cpu);
}

/* 4. 例外ブレークマスクが既定オフのとき、TRAPが発生してもdebug_runは
 * 素通りする（stop_reasonはNONEのまま、ハンドラ内の命令まで実行が進む）。 */
static void test_exception_mask_default_off(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    write_long_be(t.ram, TRAP0_VECTOR * 4, HANDLER_ADDR);
    wr_w(&t, RESET_PC, TRAP0_OP); /* TRAP #0 */
    wr_w(&t, HANDLER_ADDR, NOP_OP);
    wr_w(&t, HANDLER_ADDR + 2, NOP_OP);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    assert(mc68k_get_exception_break_mask(cpu) == 0); /* 既定オフ */

    /* TRAP(34) + NOP(4) + NOP(4) = 42。ぴったり2命令分ハンドラ内へ進む予算。 */
    mc68k_debug_run(cpu, 42);

    assert(mc68k_get_stop_reason(cpu) == MC68K_STOP_NONE);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == HANDLER_ADDR + 4); /* TRAPを素通りしハンドラ内2命令目まで進んだ */

    mc68k_destroy(cpu);
}

/* 5. 例外ブレークマスクで該当ベクタを有効化すると、その例外で停止する */
static void test_exception_mask_enabled(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    write_long_be(t.ram, TRAP0_VECTOR * 4, HANDLER_ADDR);
    wr_w(&t, RESET_PC, TRAP0_OP); /* TRAP #0 */
    wr_w(&t, HANDLER_ADDR, NOP_OP);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_exception_break_mask(cpu, 1ULL << TRAP0_VECTOR);

    mc68k_debug_run(cpu, 1000);

    assert(mc68k_get_stop_reason(cpu) == MC68K_STOP_EXCEPTION);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == HANDLER_ADDR); /* ハンドラ先頭で停止、まだ1命令も実行していない */
    assert(mc68k_get_last_exception_vector(cpu) == (int)TRAP0_VECTOR); /*  */

    mc68k_destroy(cpu);
}

int main(void) {
    test_stops_at_breakpoint();
    test_stops_at_stop_instruction();
    test_stops_at_halted();
    test_exception_mask_default_off();
    test_exception_mask_enabled();
    return 0;
}
