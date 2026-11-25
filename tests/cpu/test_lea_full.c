/*
 * test_lea_full.c — LEA 全アドレッシングモード網羅テスト
 *
 * LEA opcode: 0x41C0 | (An<<9) | (mode<<3) | reg
 *   An=0 (A0): base 0x41C0
 *   An=4 (A4): base 0x49C0   ← HISTORY.X が cmd_buf を a4 にロードするケース
 *
 * 対応モード（制御アドレッシングのみ）:
 *   2  (An)            0x_xD0
 *   5  d16(An)         0x_xE8
 *   6  d8(An,Xn)       0x_xF0
 *   7/0 abs.W          0x_xF8
 *   7/1 abs.L          0x_xF9   ← cmd_buf(0x05EA3F) はこれ
 *   7/2 d16(PC)        0x_xFA
 *   7/3 d8(PC,Xn)      0x_xFB
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "memory/memory.h"
#include "memory/bus.h"

#define RAM_SIZE  0x100000 /* 1MB: 0x05EA3F まで届くように */
#define RESET_SSP 0x00008000
#define RESET_PC  0x00001000

static void w16(uint8_t *ram, uint32_t a, uint16_t v) {
    ram[a] = v >> 8;
    ram[a + 1] = v;
}
static void w32(uint8_t *ram, uint32_t a, uint32_t v) {
    ram[a] = v >> 24;
    ram[a + 1] = v >> 16;
    ram[a + 2] = v >> 8;
    ram[a + 3] = v;
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

static mc68k_regs_t run(uint8_t *ram) {
    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 300);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);
    return r;
}

static int fails = 0;
#define CHECK(name, got, exp)                                                                 \
    do {                                                                                      \
        if ((got) != (exp)) {                                                                 \
            printf("FAIL %-22s got=%08X exp=%08X\n", name, (unsigned)(got), (unsigned)(exp)); \
            fails++;                                                                          \
        } else                                                                                \
            printf("ok   %-22s = %08X\n", name, (unsigned)(exp));                             \
    } while (0)

/* LEA (abs.L), A4 — HISTORY.X の `lea.l (cmd_buf),a4` 相当 */
static void test_lea_absl_a4(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    /* LEA (0x0005EA3F).L, A4 : 0x49C0|(7<<3)|1 = 0x49F9 */
    w16(ram, RESET_PC + 0, 0x49F9);
    w32(ram, RESET_PC + 2, 0x0005EA3F);
    w16(ram, RESET_PC + 6, 0x60FE); /* BRA * */
    mc68k_regs_t r = run(ram);
    CHECK("LEA abs.L,A4", r.a[4], 0x0005EA3F);
    free(ram);
}

/* LEA (abs.L), A1 — L001d7a の `lea.l (cmd_buf),a1` 相当 */
static void test_lea_absl_a1(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    /* LEA (0x0005EA3F).L, A1 : 0x43C0|(7<<3)|1 = 0x43F9 */
    w16(ram, RESET_PC + 0, 0x43F9);
    w32(ram, RESET_PC + 2, 0x0005EA3F);
    w16(ram, RESET_PC + 6, 0x60FE);
    mc68k_regs_t r = run(ram);
    CHECK("LEA abs.L,A1", r.a[1], 0x0005EA3F);
    free(ram);
}

/* LEA (abs.L), A0 — 上位バイトに 0 以外を含むアドレス */
static void test_lea_absl_high(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x41F9); /* LEA abs.L, A0 */
    w32(ram, RESET_PC + 2, 0x00FEDCBA);
    w16(ram, RESET_PC + 6, 0x60FE);
    mc68k_regs_t r = run(ram);
    CHECK("LEA abs.L high", r.a[0], 0x00FEDCBA);
    free(ram);
}

/* LEA abs.W, A0 — 符号拡張なし（正） */
static void test_lea_absw_pos(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x41F8); /* LEA abs.W, A0 */
    w16(ram, RESET_PC + 2, 0x3000);
    w16(ram, RESET_PC + 4, 0x60FE);
    mc68k_regs_t r = run(ram);
    CHECK("LEA abs.W pos", r.a[0], 0x00003000);
    free(ram);
}

/* LEA abs.W, A0 — 符号拡張（負: 0x8000→0xFFFF8000） */
static void test_lea_absw_neg(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x41F8);
    w16(ram, RESET_PC + 2, 0x8000);
    w16(ram, RESET_PC + 4, 0x60FE);
    mc68k_regs_t r = run(ram);
    CHECK("LEA abs.W neg", r.a[0], 0xFFFF8000);
    free(ram);
}

/* LEA d16(An), A1 */
static void test_lea_d16an(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x207C); /* MOVEA.L #0x4000, A0 */
    w32(ram, RESET_PC + 2, 0x00004000);
    w16(ram, RESET_PC + 6, 0x43E8); /* LEA d16(A0), A1 : 0x43C0|(5<<3)|0 */
    w16(ram, RESET_PC + 8, 0x0010);
    w16(ram, RESET_PC + 10, 0x60FE);
    mc68k_regs_t r = run(ram);
    CHECK("LEA d16(A0),A1", r.a[1], 0x00004010);
    free(ram);
}

/* LEA d16(An), A1 — 負の変位 */
static void test_lea_d16an_neg(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x207C);
    w32(ram, RESET_PC + 2, 0x00004000);
    w16(ram, RESET_PC + 6, 0x43E8);
    w16(ram, RESET_PC + 8, 0xFFFE); /* -2 */
    w16(ram, RESET_PC + 10, 0x60FE);
    mc68k_regs_t r = run(ram);
    CHECK("LEA d16(A0),A1 neg", r.a[1], 0x00003FFE);
    free(ram);
}

/* LEA (An), A1 */
static void test_lea_an(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x207C);
    w32(ram, RESET_PC + 2, 0x00004000);
    w16(ram, RESET_PC + 6, 0x43D0); /* LEA (A0), A1 */
    w16(ram, RESET_PC + 8, 0x60FE);
    mc68k_regs_t r = run(ram);
    CHECK("LEA (A0),A1", r.a[1], 0x00004000);
    free(ram);
}

/* LEA d16(PC), A0 */
static void test_lea_d16pc(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    /* LEA d16(PC), A0 : 0x41C0|(7<<3)|2 = 0x41FA。
     * PC は拡張ワード取得時点 = RESET_PC+2。disp=0x0010 → A0 = RESET_PC+2+0x10 */
    w16(ram, RESET_PC + 0, 0x41FA);
    w16(ram, RESET_PC + 2, 0x0010);
    w16(ram, RESET_PC + 4, 0x60FE);
    mc68k_regs_t r = run(ram);
    CHECK("LEA d16(PC),A0", r.a[0], RESET_PC + 2 + 0x10);
    free(ram);
}

/* LEA d8(An,Xn), A1 — ブリーフ拡張ワード */
static void test_lea_d8anxn(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    w32(ram, 0, RESET_SSP);
    w32(ram, 4, RESET_PC);
    w16(ram, RESET_PC + 0, 0x207C); /* MOVEA.L #0x4000, A0 */
    w32(ram, RESET_PC + 2, 0x00004000);
    w16(ram, RESET_PC + 6, 0x223C); /* MOVE.L #0x10, D1 */
    w32(ram, RESET_PC + 8, 0x00000010);
    /* LEA d8(A0,D1.L), A1 : 0x43C0|(6<<3)|0 = 0x43F0
     * brief: D1 index, L サイズ(bit11=1), disp8=0x04 → A0+D1+4 = 0x4014 */
    w16(ram, RESET_PC + 12, 0x43F0);
    w16(ram, RESET_PC + 14,
        0x1804); /* idx reg=D1(bit15=0,bits14-12=001), bit11=1(L), disp=0x04 */
    w16(ram, RESET_PC + 16, 0x60FE);
    mc68k_regs_t r = run(ram);
    CHECK("LEA d8(A0,D1.L),A1", r.a[1], 0x00004014);
    free(ram);
}

int main(void) {
    test_lea_absl_a4();
    test_lea_absl_a1();
    test_lea_absl_high();
    test_lea_absw_pos();
    test_lea_absw_neg();
    test_lea_d16an();
    test_lea_d16an_neg();
    test_lea_an();
    test_lea_d16pc();
    test_lea_d8anxn();
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
