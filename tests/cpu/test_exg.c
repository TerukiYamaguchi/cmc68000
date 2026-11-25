/*
 * test_exg.c — EXG 全モード網羅テスト
 *
 * EXG opcode: 1100 Rx 1 mode Ry
 *   Dn,Dn : mode=01000 → 0xC140 | (Rx<<9) | Ry
 *   An,An : mode=01001 → 0xC148 | (Rx<<9) | Ry
 *   Dn,An : mode=10001 → 0xC188 | (Rx<<9) | Ry
 *
 * 回帰防止: AND/OR の Dn→EA 登録が EXG オペコードを上書きしていたバグ
 *           （exg.l a0,a1 = 0xC149 が AND として誤実行されていた）。
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "memory/memory.h"
#include "memory/bus.h"

#define RAM_SIZE  0x10000
#define RESET_SSP 0x00008000
#define RESET_PC  0x00001000

static void w16(uint8_t *r, uint32_t a, uint16_t v) {
    r[a] = v >> 8;
    r[a + 1] = v;
}
static void w32(uint8_t *r, uint32_t a, uint32_t v) {
    r[a] = v >> 24;
    r[a + 1] = v >> 16;
    r[a + 2] = v >> 8;
    r[a + 3] = v;
}

static mc68k_state_t *make_cpu(uint8_t *ram) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = ram;
    r->base = 0;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);
    mc68k_bus_t bus = bus_make_hooks(mem);
    return mc68k_create(&bus);
}

static int fails = 0;
#define CHECK(name, got, exp)                                                                 \
    do {                                                                                      \
        if ((got) != (exp)) {                                                                 \
            printf("FAIL %-20s got=%08X exp=%08X\n", name, (unsigned)(got), (unsigned)(exp)); \
            fails++;                                                                          \
        } else                                                                                \
            printf("ok   %-20s = %08X\n", name, (unsigned)(exp));                             \
    } while (0)

/* EXG.L A0,A1 (0xC149) — dir バグの再現ケース */
static void test_exg_aa(void) {
    uint8_t ram[RAM_SIZE] = {0};
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x207C);
    w32(ram, RESET_PC + 2, 0x00060836); /* MOVEA.L #0x60836, A0 */
    w16(ram, RESET_PC + 6, 0x227C);
    w32(ram, RESET_PC + 8, 0x0005EA3F); /* MOVEA.L #0x5EA3F, A1 */
    w16(ram, RESET_PC + 12, 0xC149);    /* EXG.L A0,A1 */
    w16(ram, RESET_PC + 14, 0x60FE);
    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 300);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);
    CHECK("EXG.L A0,A1 -> A0", r.a[0], 0x0005EA3F);
    CHECK("EXG.L A0,A1 -> A1", r.a[1], 0x00060836);
}

/* EXG.L D0,D1 (0xC141) */
static void test_exg_dd(void) {
    uint8_t ram[RAM_SIZE] = {0};
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x203C);
    w32(ram, RESET_PC + 2, 0x11112222); /* MOVE.L #..., D0 */
    w16(ram, RESET_PC + 6, 0x223C);
    w32(ram, RESET_PC + 8, 0x33334444); /* MOVE.L #..., D1 */
    w16(ram, RESET_PC + 12, 0xC141);    /* EXG.L D0,D1 */
    w16(ram, RESET_PC + 14, 0x60FE);
    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 300);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);
    CHECK("EXG.L D0,D1 -> D0", r.d[0], 0x33334444);
    CHECK("EXG.L D0,D1 -> D1", r.d[1], 0x11112222);
}

/* EXG.L D2,A3 (0xC58B) — Dn,An モード */
static void test_exg_da(void) {
    uint8_t ram[RAM_SIZE] = {0};
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x243C);
    w32(ram, RESET_PC + 2, 0xAAAABBBB); /* MOVE.L #..., D2 */
    w16(ram, RESET_PC + 6, 0x267C);
    w32(ram, RESET_PC + 8, 0x0000CCCC); /* MOVEA.L #..., A3 */
    w16(ram, RESET_PC + 12, 0xC58B);    /* EXG.L D2,A3 */
    w16(ram, RESET_PC + 14, 0x60FE);
    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 300);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);
    CHECK("EXG.L D2,A3 -> D2", r.d[2], 0x0000CCCC);
    CHECK("EXG.L D2,A3 -> A3", r.a[3], 0xAAAABBBB);
}

/* AND.W D0,(A1) — Dn→EA メモリ宛先が依然動くことを確認（回帰チェック） */
static void test_and_to_mem(void) {
    uint8_t ram[RAM_SIZE] = {0};
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, 0x3000, 0xFF0F); /* メモリ初期値 */
    w16(ram, RESET_PC + 0, 0x303C);
    w16(ram, RESET_PC + 2, 0x0FF0); /* MOVE.W #0x0FF0,D0 */
    w16(ram, RESET_PC + 4, 0x227C);
    w32(ram, RESET_PC + 6, 0x00003000); /* MOVEA.L #0x3000,A1 */
    w16(ram, RESET_PC + 10, 0xC151);    /* AND.W D0,(A1) */
    w16(ram, RESET_PC + 12, 0x60FE);
    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 300);
    mc68k_destroy(cpu);
    uint16_t mem = (uint16_t)((ram[0x3000] << 8) | ram[0x3001]);
    CHECK("AND.W D0,(A1) mem", mem, 0x0F00); /* 0xFF0F & 0x0FF0 = 0x0F00 */
}

int main(void) {
    test_exg_aa();
    test_exg_dd();
    test_exg_da();
    test_and_to_mem();
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
