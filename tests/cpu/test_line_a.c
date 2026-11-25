/*
 * test_line_a.c — Line-A 例外（ベクタ10）のユニットテスト
 *
 * Line-A 命令 (0xA000-0xAFFF、上位4bitが1010の未定義オペコード) を実行すると
 * 即座にベクタ10 (アドレス 0x28) へディスパッチされることを検証する。
 *
 * フレーム形状は TRAP と同じシンプルフレーム（PC 4バイト + SR 2バイト、
 * 計6バイト、cpu_dispatch_exception() 共通実装）だが、TRAP と異なり積まれる
 * 戻り PC は「次の命令」ではなく「トラップした Line-A 命令自体のアドレス」
 * になる（m68000.c の op_line_a: cpu_dispatch_exception(cpu, cpu->pc - 2, ...)）。
 * これはソフトウェアエミュレーション用途（ハンドラが命令語自体を読んで
 * エミュレートする）を前提にした 68000 の仕様。
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

#define RAM_SIZE     0x10000
#define RESET_SSP    0x00008000
#define RESET_PC     0x00001000
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
/* ベクタジャンプと RTE 復帰                                            */
/* ================================================================== */

/*
 * Line-A 命令実行時にベクタ10 (アドレス 0x28) 経由でハンドラへジャンプし、
 * RTE で正しく復帰することを確認する。TRAP と違い戻り PC はトラップ命令
 * 自体のアドレスなので、ハンドラは RTE 前にスタック上の戻り PC を +2 して
 * 次の命令から再開させる（実機の Line-A エミュレータハンドラと同じ手法）。
 * フレームは [A7+0]=SR（2バイト）, [A7+2]=戻りPC（4バイト）の順（test_trap.c
 * のスタックフレームコメント参照）なので、PC 側だけを d16(A7) で直接補正する。
 *
 * プログラム:
 *   0x1000: DC.W $A000        ← Line-A 命令
 *   0x1002: MOVE.W #0x99, D0  ← RTE 後にここから再開されるはず
 *   0x1006: BRA *
 *
 * ハンドラ (HANDLER_ADDR):
 *   MOVE.W #0x4A, D1     ; ハンドラ到達マーカー
 *   ADDQ.L #2, 2(A7)     ; 戻り PC（[A7+2]）を次命令へ補正
 *   RTE
 *
 * 期待: D1=0x4A（ハンドラ到達）、D0=0x99（トラップ命令の次から再開）、
 *       A7=RESET_SSP（スタック正常巻き戻し）
 */
static void test_line_a_vector_jump(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_long_be(ram, 0x28, HANDLER_ADDR); /* ベクタ10 */

    write_word_be(ram, RESET_PC + 0, 0xA000); /* Line-A 命令 */
    write_word_be(ram, RESET_PC + 2, 0x303C); /* MOVE.W #0x99, D0 */
    write_word_be(ram, RESET_PC + 4, 0x0099);
    write_word_be(ram, RESET_PC + 6, 0x60FE); /* BRA * */

    write_word_be(ram, HANDLER_ADDR + 0, 0x323C); /* MOVE.W #0x4A, D1 */
    write_word_be(ram, HANDLER_ADDR + 2, 0x004A);
    write_word_be(ram, HANDLER_ADDR + 4, 0x54AF); /* ADDQ.L #2, 2(A7) */
    write_word_be(ram, HANDLER_ADDR + 6, 0x0002); /* d16 = 2 */
    write_word_be(ram, HANDLER_ADDR + 8, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 400);
    mc68k_destroy(cpu);

    assert((r.d[1] & 0xFFFF) == 0x4A);
    assert((r.d[0] & 0xFFFF) == 0x99);
    assert(r.a[7] == RESET_SSP);
}

/* ================================================================== */
/* スタックフレームの中身                                               */
/* ================================================================== */

/*
 * 積まれる PC が「トラップした Line-A 命令自体のアドレス」であること、
 * 積まれる SR が実行前の SR と一致することを直接検証する。
 * フレームは [A7+0]=SR（2バイト）, [A7+2]=戻りPC（4バイト）の順
 * （test_trap.c のスタックフレームコメント参照）。
 *
 * ハンドラ (HANDLER_ADDR):
 *   MOVE.W (A7)+, D4   ; 保存された SR を取り出す
 *   MOVE.L (A7)+, D3   ; 保存された戻り PC を取り出す
 *   BRA *
 *
 * 初期 SR = SR_RESET = 0x2700
 * 期待: D4 = 0x2700、D3 = RESET_PC
 */
static void test_line_a_stack_frame(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_long_be(ram, 0x28, HANDLER_ADDR);

    write_word_be(ram, RESET_PC + 0, 0xABCD); /* Line-A 命令（任意のビットパターン） */

    write_word_be(ram, HANDLER_ADDR + 0, 0x381F); /* MOVE.W (A7)+, D4 */
    write_word_be(ram, HANDLER_ADDR + 2, 0x261F); /* MOVE.L (A7)+, D3 */
    write_word_be(ram, HANDLER_ADDR + 4, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 400);
    mc68k_destroy(cpu);

    assert(r.d[3] == RESET_PC);
    assert((r.d[4] & 0xFFFF) == 0x2700);
}

/* ================================================================== */
/* オペコード範囲全体の確認                                             */
/* ================================================================== */

/*
 * Line-A 範囲 (0xA000-0xAFFF) の境界値 2 点が、下位ビットパターンによらず
 * すべて同じベクタ10 経由でハンドラへ落ちることを確認する
 * （decode.c 側の登録ループが範囲全体を正しくカバーしているかの回帰確認）。
 */
static void test_line_a_full_range(void) {
    static const uint16_t opcodes[2] = {0xA000, 0xAFFF};

    for (int i = 0; i < 2; i++) {
        uint8_t ram[RAM_SIZE] = {0};
        write_long_be(ram, 0, RESET_SSP);
        write_long_be(ram, 4, RESET_PC);
        write_long_be(ram, 0x28, HANDLER_ADDR);

        write_word_be(ram, RESET_PC + 0, opcodes[i]);

        write_word_be(ram, HANDLER_ADDR + 0, 0x303C); /* MOVE.W #0x77, D0 */
        write_word_be(ram, HANDLER_ADDR + 2, 0x0077);
        write_word_be(ram, HANDLER_ADDR + 4, 0x60FE); /* BRA * */

        mc68k_state_t *cpu = make_cpu(ram);
        mc68k_regs_t r = run(cpu, 400);
        mc68k_destroy(cpu);

        assert((r.d[0] & 0xFFFF) == 0x77);
    }
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_line_a_vector_jump();
    test_line_a_stack_frame();
    test_line_a_full_range();
    return 0;
}
