/*
 * test_misc.c — MOVEQ / AND / OR / EOR / CLR / NEG / NOT / TST /
 *               SWAP / SHIFT / ROTATE / LEA / MOVEM / LINK / MULU テスト
 *
 * MC68000 の雑多な命令群を検証する。
 *
 * opcode クイックリファレンス:
 *   MOVEQ #n, Dn   : 0x7000 | (dn<<9) | (n&0xFF)
 *   AND.W D1, D0   : 0xC041  (EA→Dn, op=1=word)
 *   OR.W  D1, D0   : 0x8041
 *   EOR.W D1, D0   : 0xB340  (Dn=1 → EA D0, op=5=word)
 *   CLR.L D0       : 0x4280
 *   NEG.W D0       : 0x4440
 *   NOT.W D0       : 0x4640
 *   TST.W D0       : 0x4A40
 *   SWAP D0        : 0x4840
 *   EXT.W D0       : 0x4880  (byte→word)
 *   EXT.L D0       : 0x48C0  (word→long)
 *   LSL.W #1, D0   : 0xE348  (creg=1, dr=1, ss=01, ir=0, type=1)
 *   LSR.W #1, D0   : 0xE248  (dr=0)
 *   ASR.W #1, D0   : 0xE240  (type=0, dr=0)
 *   LEA addr.W,A0  : 0x41F8 + addr16
 *   LEA (A0),A1    : 0x43D0
 *   MOVEM.L D0-D1,-(A7): 0x48E7 + mask(0x0003)
 *   MOVEM.L (A7)+,D0-D1: 0x4CDF + mask(0xC000)
 *   LINK A6,#disp  : 0x4E56 + disp16
 *   UNLK A6        : 0x4E5E
 *   MULU.W D1,D0   : 0xC0C1  (dn=0, opmode=3=MULU, ea=D1)
 *   BRA *          : 0x60FE  (実行停止用)
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

static uint16_t ccr(const mc68k_regs_t *r) {
    return r->sr & 0x1F;
}

/* ================================================================== */
/* MOVEQ — 8bit 即値の 32bit 符号拡張                                  */
/* ================================================================== */

/*
 * MOVEQ #42, D0 → D0 = 0x0000002A（正数: ゼロ拡張）
 * CCR: Z=0, N=0
 */
static void test_moveq_positive(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x702A); /* MOVEQ #42, D0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 100);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x0000002A);
    assert(ccr(&r) == 0x00);
}

/*
 * MOVEQ #-1 (0xFF), D0 → D0 = 0xFFFFFFFF（負数: 符号拡張）
 * CCR: N=1
 */
static void test_moveq_negative(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x70FF); /* MOVEQ #-1, D0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 100);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0xFFFFFFFF);
    assert(ccr(&r) == 0x08); /* N=1 */
}

/*
 * MOVEQ #0, D0 → D0 = 0, CCR: Z=1
 */
static void test_moveq_zero(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x7000); /* MOVEQ #0, D0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 100);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x00000000);
    assert(ccr(&r) == 0x04); /* Z=1 */
}

/* ================================================================== */
/* AND.W — 論理積                                                        */
/* ================================================================== */

/*
 * D0=0xF0F0, D1=0x0F0F → AND.W D1, D0 → 0x0000, Z=1
 * AND.W D1, D0: dn=0, op=1(word), ea=D1 → opcode 0xC041
 */
static void test_and_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0xF0F0, D0 */
    write_word_be(ram, RESET_PC + 2, 0xF0F0);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #0x0F0F, D1 */
    write_word_be(ram, RESET_PC + 6, 0x0F0F);
    write_word_be(ram, RESET_PC + 8, 0xC041); /* AND.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0000);
    assert(ccr(&r) == 0x04); /* Z=1 */
}

/* ================================================================== */
/* OR.W — 論理和                                                         */
/* ================================================================== */

/*
 * D0=0x00FF, D1=0xFF00 → OR.W D1, D0 → 0xFFFF, N=1
 * OR.W D1, D0: dn=0, op=1(word), ea=D1 → opcode 0x8041
 */
static void test_or_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0x00FF, D0 */
    write_word_be(ram, RESET_PC + 2, 0x00FF);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #0xFF00, D1 */
    write_word_be(ram, RESET_PC + 6, 0xFF00);
    write_word_be(ram, RESET_PC + 8, 0x8041); /* OR.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0xFFFF);
    assert(ccr(&r) == 0x08); /* N=1 */
}

/* ================================================================== */
/* EOR.W — 排他的論理和                                                  */
/* ================================================================== */

/*
 * D0=0xFFFF, D1=0x5555 → EOR.W D1, D0 → 0xAAAA, N=1
 *
 * EOR.W D1, D0: dn=1(source), op=5(word), ea=D0
 *   opcode = 0xB000 | (1<<9) | (5<<6) | 0 = 0xB340
 */
static void test_eor_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0xFFFF, D0 */
    write_word_be(ram, RESET_PC + 2, 0xFFFF);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #0x5555, D1 */
    write_word_be(ram, RESET_PC + 6, 0x5555);
    write_word_be(ram, RESET_PC + 8, 0xB340); /* EOR.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0xAAAA);
    assert(ccr(&r) == 0x08); /* N=1 */
}

/* ================================================================== */
/* CLR.L — ロングワード全体をゼロクリア                                  */
/* ================================================================== */

/*
 * D0=0xDEADBEEF → CLR.L D0 → D0=0, Z=1
 * CLR.L D0: sc=2(long), ea=D0 → 0x4280
 */
static void test_clr_long(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x203C); /* MOVE.L #0xDEADBEEF, D0 */
    write_long_be(ram, RESET_PC + 2, 0xDEADBEEF);
    write_word_be(ram, RESET_PC + 6, 0x4280); /* CLR.L D0 */
    write_word_be(ram, RESET_PC + 8, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x00000000);
    assert(ccr(&r) == 0x04); /* Z=1 */
}

/* ================================================================== */
/* NEG.W — 符号反転（0 - src）                                          */
/* ================================================================== */

/*
 * D0=5 → NEG.W D0 → D0 = 0xFFFB (-5 as word), C=1, X=1, N=1
 * NEG.W D0: sc=1(word), ea=D0 → 0x4440
 */
static void test_neg_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #5, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0005);
    write_word_be(ram, RESET_PC + 4, 0x4440); /* NEG.W D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0xFFFB);
    /* C=1, X=1 (ボロー), N=1 → CCR bits: X=0x10, N=0x08, C=0x01 = 0x19 */
    assert(ccr(&r) == 0x19);
}

/* ================================================================== */
/* NOT.W — ビット反転                                                    */
/* ================================================================== */

/*
 * D0=0x00FF → NOT.W D0 → D0 = 0xFF00, N=1
 * NOT.W D0: sc=1(word), ea=D0 → 0x4640
 */
static void test_not_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0x00FF, D0 */
    write_word_be(ram, RESET_PC + 2, 0x00FF);
    write_word_be(ram, RESET_PC + 4, 0x4640); /* NOT.W D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0xFF00);
    assert(ccr(&r) == 0x08); /* N=1 */
}

/* ================================================================== */
/* TST.W — フラグ更新のみ（レジスタ値は変化しない）                     */
/* ================================================================== */

/*
 * TST は dest の値でフラグを更新するだけで dest は変化しない。
 * D0=0 → TST.W D0 → Z=1 （D0 は 0 のまま）
 * TST.W D0: sc=1(word), ea=D0 → 0x4A40
 */
static void test_tst_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0000);
    write_word_be(ram, RESET_PC + 4, 0x4A40); /* TST.W D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0000); /* 値変化なし */
    assert(ccr(&r) == 0x04);             /* Z=1 */
}

/* ================================================================== */
/* SWAP — 上位/下位ワードの交換                                          */
/* ================================================================== */

/*
 * D0=0x12345678 → SWAP D0 → D0=0x56781234
 * SWAP D0: 0x4840
 */
static void test_swap(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x203C); /* MOVE.L #0x12345678, D0 */
    write_long_be(ram, RESET_PC + 2, 0x12345678);
    write_word_be(ram, RESET_PC + 6, 0x4840); /* SWAP D0 */
    write_word_be(ram, RESET_PC + 8, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x56781234);
    assert(ccr(&r) == 0x00); /* 結果は正の非ゼロ */
}

/* ================================================================== */
/* LSL.W #1, D0 — 論理左シフト                                          */
/* ================================================================== */

/*
 * D0=0x0001 → LSL.W #1, D0 → D0=0x0002, C=0
 * LSL.W #1, D0: creg=1, dr=1, ss=01, ir=0, type=1
 *   = 1110 001 1 01 0 01 000 = 0xE348
 */
static void test_lsl_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0x0001, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0001);
    write_word_be(ram, RESET_PC + 4, 0xE348); /* LSL.W #1, D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0002);
    assert(ccr(&r) == 0x00); /* C=0, Z=0, N=0 */
}

/*
 * D0=0x8000 → LSL.W #1, D0 → D0=0x0000, C=1 (MSB が押し出される), Z=1
 */
static void test_lsl_carry(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0x8000, D0 */
    write_word_be(ram, RESET_PC + 2, 0x8000);
    write_word_be(ram, RESET_PC + 4, 0xE348); /* LSL.W #1, D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0000);
    /* C=1, X=1, Z=1 → 0x15 */
    assert(ccr(&r) == 0x15);
}

/* ================================================================== */
/* LSR.W #1, D0 — 論理右シフト                                          */
/* ================================================================== */

/*
 * D0=0x0002 → LSR.W #1, D0 → D0=0x0001
 * LSR.W #1, D0: creg=1, dr=0, ss=01, ir=0, type=1
 *   = 1110 001 0 01 0 01 000 = 0xE248
 */
static void test_lsr_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0x0002, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0002);
    write_word_be(ram, RESET_PC + 4, 0xE248); /* LSR.W #1, D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0001);
    assert(ccr(&r) == 0x00);
}

/* ================================================================== */
/* ASR.W #1, D0 — 算術右シフト（符号ビットを保持）                      */
/* ================================================================== */

/*
 * D0=0xFFFE (-2 as word) → ASR.W #1 → 0xFFFF (-1), 符号ビット保持
 * ASR.W #1, D0: creg=1, dr=0, ss=01, ir=0, type=0
 *   = 1110 001 0 01 0 00 000 = 0xE240
 */
static void test_asr_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0xFFFE, D0 */
    write_word_be(ram, RESET_PC + 2, 0xFFFE);
    write_word_be(ram, RESET_PC + 4, 0xE240); /* ASR.W #1, D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0xFFFF);
    assert(ccr(&r) == 0x08); /* N=1, C=0 (last bit out was 0) */
}

/* ================================================================== */
/* LEA — 実効アドレスをアドレスレジスタにロード                         */
/* ================================================================== */

/*
 * LEA 0x3000.W, A0: addr.W アドレッシング
 * opcode = 0x41C0 | (0<<9) | (7<<3) | 0 = 0x41F8
 * 続けて符号拡張ワードアドレス 0x3000
 * A0 = 0x00003000
 */
static void test_lea_abs(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x41F8); /* LEA 0x3000.W, A0 */
    write_word_be(ram, RESET_PC + 2, 0x3000);
    write_word_be(ram, RESET_PC + 4, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert(r.a[0] == 0x00003000);
}

/*
 * LEA (A0), A1: A0 の値をそのまま A1 にロードする。
 * opcode = 0x41C0 | (1<<9) | (2<<3) | 0 = 0x43D0
 */
static void test_lea_indirect(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x207C); /* MOVEA.L #0x4000, A0 */
    write_long_be(ram, RESET_PC + 2, 0x00004000);
    write_word_be(ram, RESET_PC + 6, 0x43D0); /* LEA (A0), A1 */
    write_word_be(ram, RESET_PC + 8, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert(r.a[1] == 0x00004000);
}

/* ================================================================== */
/* MOVEM.L — 複数レジスタのスタックへの保存と復元                       */
/* ================================================================== */

/*
 * D0-D1 をスタックに保存し、レジスタをクリアしてから復元する。
 *
 * MC68000 MOVEM マスク規則:
 *   レジスタ→メモリ（全モード）: bit15=D0, bit14=D1, ..., bit8=D7, bit7=A0, ..., bit0=A7
 *   メモリ→レジスタ（全モード）: bit0=D0, bit1=D1, ..., bit7=D7, bit8=A0, ..., bit15=A7
 *
 * MOVEM.L D0-D1, -(A7): プレデクリメント、マスク=0xC000 (bit15=D0, bit14=D1)
 *   opcode = 0x48C0|(4<<3)|7 = 0x48E7
 *
 * MOVEM.L (A7)+, D0-D1: ポストインクリメント、マスク=0x0003 (bit0=D0, bit1=D1)
 *   opcode = 0x4CC0|(3<<3)|7 = 0x4CDF
 */
static void test_movem_save_restore(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVE.L #0x11111111, D0 */
    write_word_be(ram, RESET_PC + 0, 0x203C);
    write_long_be(ram, RESET_PC + 2, 0x11111111);
    /* MOVE.L #0x22222222, D1 */
    write_word_be(ram, RESET_PC + 6, 0x223C);
    write_long_be(ram, RESET_PC + 8, 0x22222222);
    /* MOVEM.L D0-D1, -(A7): mask=0xC000 (bit15=D0, bit14=D1) */
    write_word_be(ram, RESET_PC + 12, 0x48E7); /* opcode */
    write_word_be(ram, RESET_PC + 14, 0xC000); /* mask: reg→mem convention */
    /* CLR.L D0, CLR.L D1 */
    write_word_be(ram, RESET_PC + 16, 0x4280); /* CLR.L D0 */
    write_word_be(ram, RESET_PC + 18, 0x4281); /* CLR.L D1 */
    /* MOVEM.L (A7)+, D0-D1: mask=0x0003 (bit0=D0, bit1=D1) */
    write_word_be(ram, RESET_PC + 20, 0x4CDF); /* opcode */
    write_word_be(ram, RESET_PC + 22, 0x0003); /* mask: mem→reg convention */
    /* BRA * */
    write_word_be(ram, RESET_PC + 24, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 400);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x11111111);
    assert(r.d[1] == 0x22222222);
    assert(r.a[7] == RESET_SSP); /* スタック正常巻き戻し */
}

/* ================================================================== */
/* LINK / UNLK — C 言語スタックフレーム構築と解放                       */
/* ================================================================== */

/*
 * LINK A6, #-8: フレームポインタ A6 を初期化し、8 バイトのローカル領域を確保
 *   opcode = 0x4E50 | 6 = 0x4E56、続けて displacement -8 = 0xFFF8
 *
 * UNLK A6: A6 と A7 を LINK 前の状態に復元
 *   opcode = 0x4E58 | 6 = 0x4E5E
 *
 * 初期: A6=0, A7=0x8000
 * LINK 後: A6=0x7FFC (フレームポインタ), A7=0x7FF4
 * UNLK 後: A6=0, A7=0x8000（完全復元）
 */
static void test_link_unlk(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* LINK 後の A6/A7 を D0/D1 に保存してから UNLK する */
    write_word_be(ram, RESET_PC + 0, 0x4E56); /* LINK A6, #-8 */
    write_word_be(ram, RESET_PC + 2, 0xFFF8); /* displacement -8 */
    /*
     * MOVE.L A6, D0: opcode = 0x2000|(0<<9)|(0<<6)|(1<<3)|6 = 0x200E
     * MOVE.L A7, D1: opcode = 0x2000|(1<<9)|(0<<6)|(1<<3)|7 = 0x220F
     */
    write_word_be(ram, RESET_PC + 4, 0x200E); /* MOVE.L A6, D0 (frame ptr) */
    write_word_be(ram, RESET_PC + 6, 0x220F); /* MOVE.L A7, D1 (SP after alloc) */
    write_word_be(ram, RESET_PC + 8, 0x4E5E); /* UNLK A6 */
    write_word_be(ram, RESET_PC + 10, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 400);
    mc68k_destroy(cpu);

    /* LINK 後: A6 = 0x7FFC（フレームポインタ）, A7 = 0x7FF4（-8 バイト確保） */
    assert(r.d[0] == 0x00007FFC);
    assert(r.d[1] == 0x00007FF4);
    /* UNLK 後: A6 = 0（元の値に復元）, A7 = RESET_SSP（完全巻き戻し） */
    assert(r.a[6] == 0x00000000);
    assert(r.a[7] == RESET_SSP);
}

/* ================================================================== */
/* MULU.W — 符号なし 16×16→32bit 乗算                                  */
/* ================================================================== */

/*
 * D0=3, D1=7 → MULU.W D1, D0 → D0=21
 * MULU.W D1, D0: ライン C, dn=0, opmode=3(MULU), ea=D1
 *   opcode = 0xC000 | (0<<9) | (3<<6) | (0<<3) | 1 = 0xC0C1
 */
static void test_mulu(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #3, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0003);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #7, D1 */
    write_word_be(ram, RESET_PC + 6, 0x0007);
    write_word_be(ram, RESET_PC + 8, 0xC0C1); /* MULU.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 400);
    mc68k_destroy(cpu);

    assert(r.d[0] == 21);
    assert(ccr(&r) == 0x00); /* 結果は正の非ゼロ */
}

/* ================================================================== */
/* cpu_set_sr() — S ビット遷移時の A7↔SSP/USP スワップ検証             */
/* ================================================================== */

/*
 * MOVE.W #imm, SR で S=1→0 に変わるとき A7 が USP に切り替わることを確認。
 *
 * opcode:
 *   MOVEA.L #0x4000, A1 : 0x227C 0x0000 0x4000
 *   MOVE.L  A1, USP     : 0x4E61  (MOVE An, USP; an=1)
 *   MOVE.W  #0x0000, SR : 0x46FC 0x0000  (MOVE #imm, SR; imm=0x0000 clears S)
 *   BRA *               : 0x60FE
 */
static void test_move_to_sr_super_to_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP); /* SSP = 0x8000 */
    write_long_be(ram, 4, RESET_PC);

    int off = RESET_PC;
    write_word_be(ram, off + 0, 0x227C); /* MOVEA.L #0x4000, A1 */
    write_word_be(ram, off + 2, 0x0000);
    write_word_be(ram, off + 4, 0x4000);
    write_word_be(ram, off + 6, 0x4E61); /* MOVE.L A1, USP */
    write_word_be(ram, off + 8, 0x46FC); /* MOVE.W #0x0000, SR */
    write_word_be(ram, off + 10, 0x0000);
    write_word_be(ram, off + 12, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    /* S=0 に変わったので A7 は USP(0x4000) になる */
    assert(r.a[7] == 0x4000);
    /* SSP には旧 A7(0x8000) が退避されている */
    assert(r.ssp == RESET_SSP);
    /* SR の S ビットが落ちている */
    assert(!(r.sr & 0x2000));
}

/*
 * ANDI.W #imm, SR で S=1→0 に変わるとき A7 が USP に切り替わることを確認。
 *
 * opcode:
 *   MOVEA.L #0x3000, A2 : 0x247C 0x0000 0x3000
 *   MOVE.L  A2, USP     : 0x4E62  (MOVE An, USP; an=2)
 *   ANDI.W  #0xDFFF, SR : 0x027C 0xDFFF  (ANDI #imm, SR; clears bit13=S)
 *   BRA *               : 0x60FE
 */
static void test_andi_sr_super_to_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    int off = RESET_PC;
    write_word_be(ram, off + 0, 0x247C); /* MOVEA.L #0x3000, A2 */
    write_word_be(ram, off + 2, 0x0000);
    write_word_be(ram, off + 4, 0x3000);
    write_word_be(ram, off + 6, 0x4E62); /* MOVE.L A2, USP */
    write_word_be(ram, off + 8, 0x027C); /* ANDI.W #0xDFFF, SR */
    write_word_be(ram, off + 10, 0xDFFF);
    write_word_be(ram, off + 12, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert(r.a[7] == 0x3000);
    assert(r.ssp == RESET_SSP);
    assert(!(r.sr & 0x2000));
}

/*
 * RTE でユーザーモードに戻るとき A7 が USP に切り替わることを確認。
 *
 * RTE フレームのスタック積み順（低アドレス側が先にポップされる）:
 *   先に PC を積み、後から SR を積む → [SP]=SR, [SP+2]=PC
 *
 * opcode:
 *   MOVEA.L #0x5000, A0 : 0x207C 0x0000 0x5000
 *   MOVE.L  A0, USP     : 0x4E60
 *   MOVE.L  #0x1020, -(A7) : 0x2F3C 0x0000 0x1020  (PC を先に積む)
 *   MOVE.W  #0x0000, -(A7) : 0x3F3C 0x0000          (SR を後に積む)
 *   RTE                  : 0x4E73
 */
static void test_rte_super_to_user(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    int off = RESET_PC;
    write_word_be(ram, off + 0, 0x207C); /* MOVEA.L #0x5000, A0 */
    write_word_be(ram, off + 2, 0x0000);
    write_word_be(ram, off + 4, 0x5000);
    write_word_be(ram, off + 6, 0x4E60); /* MOVE.L A0, USP */
    write_word_be(ram, off + 8, 0x2F3C); /* MOVE.L #0x00001020, -(A7) ← PC */
    write_word_be(ram, off + 10, 0x0000);
    write_word_be(ram, off + 12, 0x1020);
    write_word_be(ram, off + 14, 0x3F3C); /* MOVE.W #0x0000, -(A7) ← SR(S=0) */
    write_word_be(ram, off + 16, 0x0000);
    write_word_be(ram, off + 18, 0x4E73); /* RTE */
    write_word_be(ram, 0x1020, 0x60FE);   /* BRA * (RTE 後の着地点) */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 500);
    mc68k_destroy(cpu);

    /* RTE 後はユーザーモード: A7=USP=0x5000 */
    assert(r.a[7] == 0x5000);
    assert(r.ssp == RESET_SSP);
    assert(!(r.sr & 0x2000));
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_moveq_positive();
    test_moveq_negative();
    test_moveq_zero();
    test_and_word();
    test_or_word();
    test_eor_word();
    test_clr_long();
    test_neg_word();
    test_not_word();
    test_tst_word();
    test_swap();
    test_lsl_word();
    test_lsl_carry();
    test_lsr_word();
    test_asr_word();
    test_lea_abs();
    test_lea_indirect();
    test_movem_save_restore();
    test_link_unlk();
    test_mulu();
    test_move_to_sr_super_to_user();
    test_andi_sr_super_to_user();
    test_rte_super_to_user();
    return 0;
}
