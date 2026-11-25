/*
 * test_privilege.c — 特権違反例外（ベクタ8）のユニットテスト
 *
 * ユーザモード（SR の S ビット=0）で特権命令を実行すると、命令の副作用
 * （即値フェッチ・スタックポップ等）が起きる前に即座にベクタ8へ例外
 * ディスパッチされることを検証する。
 *
 * 対象命令: STOP / RESET / RTE / MOVE An,USP / MOVE USP,An / MOVE EA,SR /
 *           ANDI・ORI・EORI #imm,SR
 * 対象外（無印MC68000では非特権、回帰固定）: MOVE SR,EA / ANDI・ORI・EORI #imm,CCR
 *
 * フレーム形状はシンプルフレーム（PC 4バイト + SR 2バイト、計6バイト）。
 * pushed PC は違反命令の先頭アドレス（副作用前のため、実行前 PC のまま）。
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
#define VEC8_HANDLER 0x00001500 /* 特権違反ハンドラ（自己ループで停止） */

/* ユーザモードへ落とすための MOVE #0,SR を RESET_PC に置く。
 * この時点ではまだスーパーバイザなので MOVE to SR 自体は正常実行される。 */
#define USER_PC (RESET_PC + 4)

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

static uint32_t read_long_be(const uint8_t *ram, uint32_t addr) {
    return ((uint32_t)ram[addr] << 24) | ((uint32_t)ram[addr + 1] << 16) |
           ((uint32_t)ram[addr + 2] << 8) | (uint32_t)ram[addr + 3];
}

static uint16_t read_word_be(const uint8_t *ram, uint32_t addr) {
    return (uint16_t)((ram[addr] << 8) | ram[addr + 1]);
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

/* 共通セットアップ: リセットベクタ + ベクタ8ハンドラ（BRA * で自己ループ）+
 * MOVE.W #0x0000,SR（0x46FC 0x0000）でユーザモードへ落とす前段部分を書く。 */
static void setup_common(uint8_t *ram) {
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x20, VEC8_HANDLER);   /* ベクタ8 = 0x20 */
    write_word_be(ram, VEC8_HANDLER, 0x60FE); /* BRA * */

    write_word_be(ram, RESET_PC + 0, 0x46FC); /* MOVE.W #0x0000, SR */
    write_word_be(ram, RESET_PC + 2, 0x0000);
}

/* 特権違反が起きたことを検証する共通チェック。
 * pushed PC（違反命令の先頭アドレス）と6バイトフレームを確認する。 */
static void assert_privilege_violation(uint8_t *ram, mc68k_regs_t *r, uint32_t fault_pc) {
    assert(r->pc == VEC8_HANDLER);
    assert(r->a[7] == RESET_SSP - 6);
    uint16_t saved_sr = read_word_be(ram, r->a[7] + 0);
    uint32_t saved_pc = read_long_be(ram, r->a[7] + 2);
    assert(!(saved_sr & 0x2000)); /* 保存 SR は S=0（ユーザモードのまま）*/
    assert(saved_pc == fault_pc);
    assert(r->sr & 0x2000); /* 現在の SR は例外突入でスーパーバイザに */
}

/* ================================================================== */
/* STOP — 即値未消費（次ワードがそのまま残る）                          */
/* ================================================================== */

static void test_stop_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x4E72); /* STOP #imm */
    write_word_be(ram, USER_PC + 2, 0x2700);
    write_word_be(ram, USER_PC + 4, 0x60FE); /* BRA *（違反しなければここへ）*/

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_privilege_violation(ram, &r, USER_PC);
}

/* ================================================================== */
/* RESET                                                                */
/* ================================================================== */

static void test_reset_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x4E70); /* RESET */
    write_word_be(ram, USER_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_privilege_violation(ram, &r, USER_PC);
}

/* ================================================================== */
/* RTE — スタック未変化（ポップ前に違反）                                */
/* ================================================================== */

static void test_rte_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x4E73); /* RTE */
    write_word_be(ram, USER_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_privilege_violation(ram, &r, USER_PC);
}

/* ================================================================== */
/* MOVE An,USP / MOVE USP,An                                           */
/* ================================================================== */

static void test_move_to_usp_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x4E60); /* MOVE A0,USP */
    write_word_be(ram, USER_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_privilege_violation(ram, &r, USER_PC);
}

static void test_move_from_usp_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x4E68); /* MOVE USP,A0 */
    write_word_be(ram, USER_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_privilege_violation(ram, &r, USER_PC);
}

/* ================================================================== */
/* MOVE EA,SR ("MOVE to SR") — EA未読み出し                             */
/* ================================================================== */

static void test_move_to_sr_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x46FC); /* MOVE.W #imm, SR */
    write_word_be(ram, USER_PC + 2, 0x2700);
    write_word_be(ram, USER_PC + 4, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_privilege_violation(ram, &r, USER_PC);
    /* SR がユーザ指定の 0x2700 に書き換わっていない（まだユーザモードのはず）
     * ことは r->sr が例外突入後の値になっている時点で保証されている。 */
}

/* ================================================================== */
/* ANDI / ORI / EORI #imm,SR — 即値未消費                               */
/* ================================================================== */

static void test_andi_sr_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x027C); /* ANDI.W #imm, SR */
    write_word_be(ram, USER_PC + 2, 0xF8FF);
    write_word_be(ram, USER_PC + 4, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_privilege_violation(ram, &r, USER_PC);
}

static void test_ori_sr_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x007C); /* ORI.W #imm, SR */
    write_word_be(ram, USER_PC + 2, 0x0700);
    write_word_be(ram, USER_PC + 4, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_privilege_violation(ram, &r, USER_PC);
}

static void test_eori_sr_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x0A7C); /* EORI.W #imm, SR */
    write_word_be(ram, USER_PC + 2, 0x0700);
    write_word_be(ram, USER_PC + 4, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_privilege_violation(ram, &r, USER_PC);
}

/* ================================================================== */
/* 非特権の回帰固定: MOVE SR,EA / #imm,CCR はユーザモードでも例外にならない */
/* ================================================================== */

static void test_move_from_sr_user_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x40C1); /* MOVE SR,D1 */
    write_word_be(ram, USER_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == USER_PC + 2); /* BRA * は自身のオペコードアドレスに着地する＝正常実行 */
    assert(r.pc != VEC8_HANDLER);
}

static void test_andi_ccr_user_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, USER_PC + 0, 0x023C); /* ANDI.B #imm, CCR */
    write_word_be(ram, USER_PC + 2, 0x00FF);
    write_word_be(ram, USER_PC + 4, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc != VEC8_HANDLER);
}

/* ================================================================== */
/* スーパーバイザでは全命令が正常動作する（回帰）                        */
/* ================================================================== */

/*
 * スーパーバイザモードのまま STOP → RESET相当は割り込みで起こすと面倒なので、
 * 代表として MOVE to SR・ANDI #imm,SR・MOVE USP が例外にならず実行されることを確認する。
 */
static void test_priv_supervisor_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x20, VEC8_HANDLER);
    write_word_be(ram, VEC8_HANDLER, 0x60FE);

    /* スーパーバイザのまま: MOVE A0,USP; MOVE USP,A1; ANDI #0xFFFF,SR（実質no-op）; BRA * */
    write_word_be(ram, RESET_PC + 0, 0x4E60); /* MOVE A0,USP */
    write_word_be(ram, RESET_PC + 2, 0x4E69); /* MOVE USP,A1 */
    write_word_be(ram, RESET_PC + 4, 0x027C); /* ANDI.W #0xFFFF, SR */
    write_word_be(ram, RESET_PC + 6, 0xFFFF);
    write_word_be(ram, RESET_PC + 8, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc ==
           RESET_PC + 8); /* BRA * は自身のオペコードアドレスに着地する＝最後まで正常実行 */
    assert(r.pc != VEC8_HANDLER);
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_stop_user();
    test_reset_user();
    test_rte_user();
    test_move_to_usp_user();
    test_move_from_usp_user();
    test_move_to_sr_user();
    test_andi_sr_user();
    test_ori_sr_user();
    test_eori_sr_user();
    test_move_from_sr_user_ok();
    test_andi_ccr_user_ok();
    test_priv_supervisor_ok();
    return 0;
}
