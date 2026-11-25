/*
 * test_branch.c — BRA / BSR / Bcc / JMP / JSR / RTS テスト
 *
 * MC68000 の分岐・サブルーチン命令の動作を検証する。
 *
 * opcode クイックリファレンス:
 *   BRA.S disp8  : 0x60xx (cond=0, d8 displacement)
 *   BRA.W disp16 : 0x6000 + word (d8=0 の場合は次ワードが displacement)
 *   BSR.S disp8  : 0x61xx
 *   BNE.S disp8  : 0x66xx (Z=0 で分岐)
 *   BEQ.S disp8  : 0x67xx (Z=1 で分岐)
 *   JMP (An)     : 0x4EC0 | (2<<3) | reg → 例: JMP (A0) = 0x4ED0
 *   JSR (An)     : 0x4E80 | (2<<3) | reg → 例: JSR (A0) = 0x4E90
 *   RTS          : 0x4E75
 *   BRA *        : 0x60FE (自己ループ: 実行停止用)
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include "memory/bus.h"

/* ================================================================== */
/* テスト共通ヘルパー                                                   */
/* ================================================================== */

#define RAM_SIZE  0x10000
#define RESET_SSP 0x00008000
#define RESET_PC  0x00001000

static void write_word_be(uint8_t *ram, uint32_t addr, uint16_t val) {
    ram[addr + 0] = (uint8_t)(val >> 8);
    ram[addr + 1] = (uint8_t)(val & 0xFF);
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t val) {
    ram[addr + 0] = (uint8_t)(val >> 24);
    ram[addr + 1] = (uint8_t)(val >> 16);
    ram[addr + 2] = (uint8_t)(val >> 8);
    ram[addr + 3] = (uint8_t)(val >> 0);
}

static mc68k_state_t *make_cpu(uint8_t *ram) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = ram;
    r->base = 0x000000;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);
    mc68k_bus_t bus = bus_make_hooks(mem);
    return mc68k_create(&bus);
}

static mc68k_state_t *make_cpu_model(uint8_t *ram, mc68k_model_t model) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = ram;
    r->base = 0x000000;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);
    mc68k_bus_t bus = bus_make_hooks(mem);
    return mc68k_create_model(&bus, model);
}

static mc68k_regs_t run(mc68k_state_t *cpu, int cycles) {
    mc68k_reset(cpu);
    mc68k_execute(cpu, cycles);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    return r;
}

/* ================================================================== */
/* BRA.S — 8bit 即値ディスプレースメント短分岐                          */
/* ================================================================== */

/*
 * BRA.S +4 で MOVE.W #99 をスキップする。
 *
 * 0x1000: MOVE.W #42, D0
 * 0x1004: BRA.S +4              ; base=0x1006, target=0x100A
 * 0x1006: MOVE.W #99, D0        ; ← スキップされる
 * 0x100A: BRA *
 *
 * D0 = 42 になるはず。
 */
static void test_bra_short(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #42, D0 */
    write_word_be(ram, RESET_PC + 2, 0x002A);
    write_word_be(ram, RESET_PC + 4, 0x6004); /* BRA.S +4 → target=0x100A */
    write_word_be(ram, RESET_PC + 6, 0x303C); /* MOVE.W #99, D0 ← skipped */
    write_word_be(ram, RESET_PC + 8, 0x0063);
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 42);
}

/* ================================================================== */
/* BRA.W — 16bit ディスプレースメント長分岐                             */
/* ================================================================== */

/*
 * BRA.W +6 で MOVE.W #0xBAD をスキップし、MOVE.W #42 を実行する。
 *
 * 0x1000: BRA.W +6              ; 0x6000 0x0006 → base=0x1002, target=0x1008
 * 0x1004: MOVE.W #0xBAD, D0     ; ← スキップ
 * 0x1008: MOVE.W #42, D0
 * 0x100C: BRA *
 */
static void test_bra_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x6000); /* BRA.W (d8=0) */
    write_word_be(ram, RESET_PC + 2, 0x0006); /* displacement +6 */
    write_word_be(ram, RESET_PC + 4, 0x303C); /* MOVE.W #0xBAD, D0 ← skipped */
    write_word_be(ram, RESET_PC + 6, 0x0BAD);
    write_word_be(ram, RESET_PC + 8, 0x303C); /* MOVE.W #42, D0 */
    write_word_be(ram, RESET_PC + 10, 0x002A);
    write_word_be(ram, RESET_PC + 12, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 42);
}

/* ================================================================== */
/* BSR.S + RTS — サブルーティン呼び出しと復帰                          */
/* ================================================================== */

/*
 * BSR.S +6: base=0x1002, return_addr=0x1002, target=0x1008
 *
 * 0x1000: BSR.S +6              ; スタックに 0x1002 を積んで 0x1008 へ
 * 0x1002: BRA *                 ; ← RTS でここへ戻る
 * 0x1004: (2 バイトパディング)
 * 0x1008: MOVE.W #0x55, D0
 * 0x100C: RTS
 *
 * D0 = 0x55、A7 = SSP（スタック巻き戻し確認）。
 */
static void test_bsr_rts(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x6106); /* BSR.S +6 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * ← return point */
    /* 0x1004-0x1007: パディング (0 = 未使用) */
    write_word_be(ram, RESET_PC + 8, 0x303C); /* MOVE.W #0x55, D0 */
    write_word_be(ram, RESET_PC + 10, 0x0055);
    write_word_be(ram, RESET_PC + 12, 0x4E75); /* RTS */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x55);
    assert(r.a[7] == RESET_SSP); /* RTS後スタックは元に戻る */
}

/* ================================================================== */
/* BNE.S — Z=0 のとき分岐する（taken）                                  */
/* ================================================================== */

/*
 * 5 ≠ 3 なので Z=0 → BNE 成立し MOVE.W #0 をスキップ。
 *
 * 0x1000: MOVE.W #5, D0
 * 0x1004: CMPI.W #3, D0        ; Z=0
 * 0x1008: BNE.S +6             ; 0x6606 → base=0x100A, target=0x1010
 * 0x100A: MOVE.W #0, D0        ; ← スキップ
 * 0x1010: BRA *
 */
static void test_bne_taken(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #5, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0005);
    write_word_be(ram, RESET_PC + 4, 0x0C40); /* CMPI.W #3, D0 */
    write_word_be(ram, RESET_PC + 6, 0x0003);
    write_word_be(ram, RESET_PC + 8, 0x6606);  /* BNE.S +6 */
    write_word_be(ram, RESET_PC + 10, 0x303C); /* MOVE.W #0, D0 ← skipped */
    write_word_be(ram, RESET_PC + 12, 0x0000);
    /* 0x100E: 2 バイトギャップ */
    write_word_be(ram, RESET_PC + 16, 0x60FE); /* BRA * (= 0x1010) */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 5); /* D0 は書き換えられていない */
}

/* ================================================================== */
/* BNE.S — Z=1 のとき分岐しない（not taken）                           */
/* ================================================================== */

/*
 * 5 == 5 なので Z=1 → BNE 不成立、次の MOVE.W を実行する。
 *
 * 0x1000: MOVE.W #5, D0
 * 0x1004: CMPI.W #5, D0        ; Z=1
 * 0x1008: BNE.S +4             ; not taken
 * 0x100A: MOVE.W #0x99, D0     ; ← 実行される
 * 0x100E: BRA *
 */
static void test_bne_not_taken(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #5, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0005);
    write_word_be(ram, RESET_PC + 4, 0x0C40); /* CMPI.W #5, D0 → Z=1 */
    write_word_be(ram, RESET_PC + 6, 0x0005);
    write_word_be(ram, RESET_PC + 8, 0x6604);  /* BNE.S +4 (not taken) */
    write_word_be(ram, RESET_PC + 10, 0x303C); /* MOVE.W #0x99, D0 ← executed */
    write_word_be(ram, RESET_PC + 12, 0x0099);
    write_word_be(ram, RESET_PC + 14, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x99);
}

/* ================================================================== */
/* BEQ.S — Z=1 のとき分岐する（taken）                                  */
/* ================================================================== */

/*
 * 7 == 7 なので Z=1 → BEQ 成立し MOVE.W #0 をスキップ。
 */
static void test_beq(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #7, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0007);
    write_word_be(ram, RESET_PC + 4, 0x0C40); /* CMPI.W #7, D0 → Z=1 */
    write_word_be(ram, RESET_PC + 6, 0x0007);
    write_word_be(ram, RESET_PC + 8, 0x6706);  /* BEQ.S +6 → target=0x1010 */
    write_word_be(ram, RESET_PC + 10, 0x303C); /* MOVE.W #0, D0 ← skipped */
    write_word_be(ram, RESET_PC + 12, 0x0000);
    write_word_be(ram, RESET_PC + 16, 0x60FE); /* BRA * (= 0x1010) */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 7);
}

/* ================================================================== */
/* JMP (An) — アドレスレジスタ間接ジャンプ                              */
/* ================================================================== */

/*
 * MOVEA.L #target, A0 でジャンプ先アドレスを A0 にセットし、
 * JMP (A0) でそこへ飛ぶ。途中の MOVE.W #0xBAD はスキップされる。
 *
 * JMP (A0): mode=2(An indirect), reg=0 → 0x4EC0|(2<<3)|0 = 0x4ED0
 */
static void test_jmp_an(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEA.L #0x100C, A0 */
    write_word_be(ram, RESET_PC + 0, 0x207C);
    write_long_be(ram, RESET_PC + 2, 0x0000100C);
    /* JMP (A0) = 0x4ED0 */
    write_word_be(ram, RESET_PC + 6, 0x4ED0);
    /* 0x1008: MOVE.W #0xBAD, D0 ← skipped */
    write_word_be(ram, RESET_PC + 8, 0x303C);
    write_word_be(ram, RESET_PC + 10, 0x0BAD);
    /* 0x100C: landing point */
    write_word_be(ram, RESET_PC + 12, 0x303C); /* MOVE.W #42, D0 */
    write_word_be(ram, RESET_PC + 14, 0x002A);
    write_word_be(ram, RESET_PC + 16, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 42);
}

/* ================================================================== */
/* JSR (An) + RTS — アドレスレジスタ間接サブルーティン呼び出し          */
/* ================================================================== */

/*
 * JSR (A0): mode=2(An indirect), reg=0 → 0x4E80|(2<<3)|0 = 0x4E90
 *
 * 0x1000: MOVEA.L #0x100A, A0
 * 0x1006: JSR (A0)              ; 0x1008 をスタックに積んで 0x100A へ
 * 0x1008: BRA *                 ; ← RTS でここに戻る
 * sub (0x100A):
 *   0x100A: MOVE.W #0xAB, D1
 *   0x100E: RTS
 */
static void test_jsr_rts_an(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEA.L #0x100A, A0 (6 bytes) */
    write_word_be(ram, RESET_PC + 0, 0x207C);
    write_long_be(ram, RESET_PC + 2, 0x0000100A);
    /* JSR (A0) = 0x4E90 */
    write_word_be(ram, RESET_PC + 6, 0x4E90);
    /* BRA * ← return addr */
    write_word_be(ram, RESET_PC + 8, 0x60FE);
    /* sub at 0x100A: MOVE.W #0xAB, D1; RTS */
    write_word_be(ram, RESET_PC + 10, 0x323C); /* MOVE.W #0xAB, D1 */
    write_word_be(ram, RESET_PC + 12, 0x00AB);
    write_word_be(ram, RESET_PC + 14, 0x4E75); /* RTS */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[1] & 0xFFFF) == 0xAB);
    assert(r.a[7] == RESET_SSP);
}

/* Table 10-6 (PDF 311--312, printed 10-20--10-21): JMP/JSR (An) are
 * calculate 3 + execute base 1 after excluding the printed execute lead. */
static void test_jmp_jsr_68040_table10_6(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x4ED0); /* JMP (A0) */
    mc68k_state_t *cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 4);
    assert(cpu->pc == 0x0200);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x4E90); /* JSR (A0) */
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 4);
    assert(cpu->pc == 0x0200);
    assert(cpu->a[7] == RESET_SSP - 4);
    mc68k_destroy(cpu);
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_bra_short();
    test_bra_word();
    test_bsr_rts();
    test_bne_taken();
    test_bne_not_taken();
    test_beq();
    test_jmp_an();
    test_jsr_rts_an();
    test_jmp_jsr_68040_table10_6();
    return 0;
}
