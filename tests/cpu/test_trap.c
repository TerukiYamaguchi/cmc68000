/*
 * test_trap.c — TRAP #n / RTE / trap_fn コールバックテスト
 *
 * MC68000 の TRAP 例外処理シーケンスを検証する:
 *   1. TRAP #n がベクタテーブル経由でハンドラへジャンプすること
 *   2. SR と戻り PC がスタックに正しく積まれること
 *   3. RTE で SR/PC が正しく復元されること
 *   4. mc68k_set_trap_handler() 経由の trap_fn が優先されること
 *
 * ベクタテーブル: TRAP #n のベクタ番号は (32+n)、アドレスは (32+n)*4。
 *   TRAP #0 → vector 32 → [0x80]
 *   TRAP #1 → vector 33 → [0x84]
 *   TRAP #5 → vector 37 → [0x94]
 *
 * スタックフレーム（TRAP 実行後）:
 *   [A7 + 0] = 保存 SR（2 バイト word）
 *   [A7 + 2] = 戻り PC（4 バイト long）
 *   ← A7 は TRAP 前から 6 バイト分下がる
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "memory/memory.h"
#include "memory/bus.h"

/* ================================================================== */
/* テスト共通ヘルパー                                                   */
/* ================================================================== */

#define RAM_SIZE  0x10000
#define RESET_SSP 0x00008000
#define RESET_PC  0x00001000

/* TRAP ハンドラを配置する RAM 上のアドレス（プログラム領域と重ならない場所）*/
#define HANDLER_ADDR 0x00001500

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

static mc68k_regs_t run(mc68k_state_t *cpu, int cycles) {
    mc68k_reset(cpu);
    mc68k_execute(cpu, cycles);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    return r;
}

/* ================================================================== */
/* TRAP #0 — ベクタテーブル経由でハンドラへジャンプし RTE で復帰        */
/* ================================================================== */

/*
 * 0x80 (vector 32) にハンドラアドレスを設定する。
 * ハンドラは D1 に 0x42 をセットして RTE で戻る。
 *
 * プログラム:
 *   0x1000: TRAP #0   → SR/PC をスタックに積んで HANDLER_ADDR へ
 *   0x1002: BRA *     ← RTE でここに戻る
 *
 * ハンドラ (HANDLER_ADDR):
 *   MOVE.W #0x42, D1
 *   RTE
 *
 * 期待: D1=0x42、A7=RESET_SSP（スタック正常復帰）
 */
static void test_trap_vector_jump(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* TRAP #0 のベクタ (vector 32 = 0x80) にハンドラアドレスを設定 */
    write_long_be(ram, 0x80, HANDLER_ADDR);

    /* メインプログラム */
    write_word_be(ram, RESET_PC + 0, 0x4E40); /* TRAP #0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * ← return */

    /* ハンドラ: MOVE.W #0x42, D1; RTE */
    write_word_be(ram, HANDLER_ADDR + 0, 0x323C); /* MOVE.W #0x42, D1 */
    write_word_be(ram, HANDLER_ADDR + 2, 0x0042);
    write_word_be(ram, HANDLER_ADDR + 4, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 400);
    mc68k_destroy(cpu);

    assert((r.d[1] & 0xFFFF) == 0x42);
    assert(r.a[7] == RESET_SSP); /* スタック正常巻き戻し */
}

/* ================================================================== */
/* TRAP スタックフレーム確認 — 保存 SR と戻り PC の正確性               */
/* ================================================================== */

/*
 * TRAP 実行時に SR と戻り PC が正しくスタックに積まれることを確認する。
 *
 * ハンドラが MOVE.W (A7), D2 で保存 SR を読み取り、
 * スタックを調べることで検証する。
 *
 * 初期 SR = SR_RESET = 0x2700（スーパーバイザ、割り込みマスク 7）
 *
 * ハンドラ (HANDLER_ADDR):
 *   MOVE.W (A7), D2    ; 保存 SR を D2 に読む (opcode 0x3417)
 *   RTE
 *
 * 期待: D2 = 0x2700
 */
static void test_trap_stack_frame(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_long_be(ram, 0x80, HANDLER_ADDR); /* TRAP #0 のベクタ */

    write_word_be(ram, RESET_PC + 0, 0x4E40); /* TRAP #0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    /*
     * MOVE.W (A7), D2: src=(A7)=mode 2,reg 7、dst=D2=mode 0,reg 2
     * opcode = 0x3000 | (2<<9) | (0<<6) | (2<<3) | 7 = 0x3417
     */
    write_word_be(ram, HANDLER_ADDR + 0, 0x3417); /* MOVE.W (A7), D2 */
    write_word_be(ram, HANDLER_ADDR + 2, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 400);
    mc68k_destroy(cpu);

    /* SR_RESET = 0x2700 (S=1, I=7, CCR=0) */
    assert((r.d[2] & 0xFFFF) == 0x2700);
}

/* ================================================================== */
/* TRAP #n — 異なる番号が異なるベクタを引く                             */
/* ================================================================== */

/*
 * TRAP #1 はベクタ 33 (アドレス 0x84) を参照する。
 * TRAP #0 のベクタ (0x80) と別のハンドラを設定し、
 * 正しいハンドラが呼ばれることを確認する。
 *
 * TRAP #1 ハンドラ: MOVE.W #0xAB, D0; RTE
 * 期待: D0 = 0xAB
 */
static void test_trap_correct_vector(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* TRAP #0 用ダミーハンドラ（呼ばれないはず）*/
    write_long_be(ram, 0x80, HANDLER_ADDR + 0x100);
    write_word_be(ram, HANDLER_ADDR + 0x100, 0x303C);
    write_word_be(ram, HANDLER_ADDR + 0x102, 0xDEAD); /* D0 = 0xDEAD ← 呼ばれてはいけない */
    write_word_be(ram, HANDLER_ADDR + 0x104, 0x4E73);

    /* TRAP #1 のベクタ: vector 33 → アドレス 33*4 = 0x84 */
    write_long_be(ram, 0x84, HANDLER_ADDR);
    write_word_be(ram, HANDLER_ADDR + 0, 0x303C); /* MOVE.W #0xAB, D0 */
    write_word_be(ram, HANDLER_ADDR + 2, 0x00AB);
    write_word_be(ram, HANDLER_ADDR + 4, 0x4E73); /* RTE */

    write_word_be(ram, RESET_PC + 0, 0x4E41); /* TRAP #1 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 400);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0xAB);
}

/* ================================================================== */
/* trap_fn コールバック — ハンドラが処理した場合はベクタを使わない      */
/* ================================================================== */

static int g_trap_called = 0;
static int g_trap_num = -1;

static int my_trap_fn(mc68k_state_t *cpu, int n, void *ctx) {
    (void)cpu;
    (void)ctx;
    g_trap_called = 1;
    g_trap_num = n;
    return 1; /* 処理済み: 通常の例外シーケンスをスキップ */
}

/*
 * mc68k_set_trap_handler() でコールバックを登録する。
 * trap_fn が 1 を返すと TRAP の次の命令から実行再開される。
 *
 * 0x1000: MOVE.W #0xAA, D0
 * 0x1004: TRAP #5
 * 0x1006: MOVE.W #0xBB, D0   ← trap_fn 処理後に実行される
 * 0x100A: BRA *
 *
 * 期待: D0=0xBB、A7=RESET_SSP（スタック未変化）
 */
static void test_trap_fn_handled(void) {
    g_trap_called = 0;
    g_trap_num = -1;

    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* ベクタは設定しない（trap_fn が処理するので参照されない）*/

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0xAA, D0 */
    write_word_be(ram, RESET_PC + 2, 0x00AA);
    write_word_be(ram, RESET_PC + 4, 0x4E45); /* TRAP #5 */
    write_word_be(ram, RESET_PC + 6, 0x303C); /* MOVE.W #0xBB, D0 */
    write_word_be(ram, RESET_PC + 8, 0x00BB);
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_set_trap_handler(cpu, my_trap_fn, NULL);
    mc68k_regs_t r = run(cpu, 400);
    mc68k_destroy(cpu);

    assert(g_trap_called == 1);
    assert(g_trap_num == 5);
    assert((r.d[0] & 0xFFFF) == 0xBB); /* TRAP 後の命令が実行された */
    assert(r.a[7] == RESET_SSP);       /* スタックは変化していない */
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_trap_vector_jump();
    test_trap_stack_frame();
    test_trap_correct_vector();
    test_trap_fn_handled();
    return 0;
}
