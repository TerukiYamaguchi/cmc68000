/*
 * test_68010.c — MC68010対応のユニットテスト
 *
 * CPUモデル別の挙動差（MOVE from CCR / MOVE from SR）を検証する。
 * ステップが進むごとに VBR/MOVEC/MOVES/RTD/フォーマットワード付き例外フレーム
 * 等の検証項目を追加していく（現行仕様 テスト計画）。
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

#define RAM_SIZE      0x10000
#define RESET_SSP     0x00008000u
#define RESET_PC      0x00001000u
#define VEC4_HANDLER  0x00001500u /* ILLEGAL命令ハンドラ（自己ループで停止） */
#define VEC8_HANDLER  0x00001600u /* 特権違反ハンドラ（自己ループで停止） */
#define VEC9_HANDLER  0x00001700u /* トレース例外ハンドラ */
#define VEC14_HANDLER 0x00001800u /* フォーマットエラーハンドラ */
#define VEC33_HANDLER 0x00001900u /* TRAP #1 ハンドラ */
#define ADDR3_HANDLER 0x00001A00u /* アドレスエラーハンドラ */
#define BUS2_HANDLER  0x00001B00u /* バスエラー(ベクタ2)ハンドラ */

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

static mc68k_state_t *make_cpu(uint8_t *ram, mc68k_model_t model) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = ram;
    r->base = 0x000000;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);
    mc68k_bus_t bus = bus_make_hooks(mem);
    return mc68k_create_model(&bus, model);
}

/* ================================================================== */
/* MOVE from CCR (0x42C0) のモデル差                                    */
/* 68000: 未定義命令 → ILLEGAL（ベクタ4）。68010: 正常動作。               */
/* ================================================================== */

static void test_move_from_ccr_68000_illegal(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x10, VEC4_HANDLER);   /* ベクタ4 = 0x10 */
    write_word_be(ram, VEC4_HANDLER, 0x60FE); /* BRA * */

    write_word_be(ram, RESET_PC + 0, 0x42C0); /* MOVE.W CCR, D0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA *（正常実行ならここへ） */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68000);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VEC4_HANDLER); /* ILLEGAL命令例外へディスパッチされた */
}

static void test_move_from_ccr_68010_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x10, VEC4_HANDLER);
    write_word_be(ram, VEC4_HANDLER, 0x60FE);

    write_word_be(ram, RESET_PC + 0, 0x42C0); /* MOVE.W CCR, D0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc != VEC4_HANDLER);                 /* ILLEGALにならず最後まで実行された */
    assert((r.d[0] & 0x001F) == (r.sr & 0x001F)); /* D0下位5bit = CCR */
}

/* ================================================================== */
/* BKPT (0x4848-0x484F) のモデル差                          */
/* 68000: 未定義命令 → ILLEGAL（ベクタ4、公称値38）。                     */
/* 68010: BKPTとして登録され、同じベクタ4だが公称値は42（Breakpoint       */
/* Instruction、[DS10 Table 7-22]）。フレーム形状はILLEGALと同一。         */
/* ================================================================== */

static void test_bkpt_68000_illegal(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x10, VEC4_HANDLER);
    write_word_be(ram, VEC4_HANDLER, 0x60FE);

    write_word_be(ram, RESET_PC + 0, 0x4848); /* BKPT #0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* 到達しないはず */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68000);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VEC4_HANDLER); /* 68000ではBKPTは未定義命令のままILLEGALへ */
}

static void test_bkpt_68010_dispatches_with_breakpoint_cycles(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x10, VEC4_HANDLER);
    write_word_be(ram, VEC4_HANDLER, 0x60FE);

    write_word_be(ram, RESET_PC + 0, 0x4848); /* BKPT #0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* 到達しないはず */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    int consumed = mc68k_execute(cpu, 1); /* BKPT本体のみ */
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    assert(r.pc ==
           VEC4_HANDLER);   /* ベクタ4へディスパッチされた（未登録なら同じ結果だが値が違う） */
    assert(consumed == 42); /* [DS10 Table 7-22] Breakpoint Instruction = 42 */
    assert(r.a[7] == RESET_SSP - 8); /* 68010: フォーマットワード付き8バイトフレーム */
    assert(read_long_be(ram, r.a[7] + 2) == RESET_PC); /* 命令先頭（BKPTはinstr_pcを積む） */
    assert(read_word_be(ram, r.a[7] + 6) ==
           0x0010); /* フォーマット$0 + ベクタオフセット(4*4)。ILLEGALと同じフレーム形状 */
    mc68k_destroy(cpu);
}

/* ================================================================== */
/* MOVE from SR (0x40C0) のモデル差                                     */
/* 68000: ユーザモードでも正常動作（非特権）。68010: 特権違反（ベクタ8）。   */
/* ================================================================== */

static void setup_user_mode_common(uint8_t *ram) {
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x20, VEC8_HANDLER);   /* ベクタ8 = 0x20 */
    write_word_be(ram, VEC8_HANDLER, 0x60FE); /* BRA * */

    write_word_be(ram, RESET_PC + 0, 0x46FC); /* MOVE.W #0x0000, SR（ユーザモードへ） */
    write_word_be(ram, RESET_PC + 2, 0x0000);
}

static void test_move_from_sr_68000_user_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_user_mode_common(ram);
    write_word_be(ram, RESET_PC + 4, 0x40C0); /* MOVE SR, D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68000);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == RESET_PC + 6); /* 特権違反にならず最後まで実行された */
    assert(r.pc != VEC8_HANDLER);
}

static void test_move_from_sr_68010_user_violation(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_user_mode_common(ram);
    write_word_be(ram, RESET_PC + 4, 0x40C0); /* MOVE SR, D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE); /* BRA *（違反しなければここへ） */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VEC8_HANDLER); /* 特権違反（ベクタ8）へディスパッチされた */
}

/* ================================================================== */
/* MOVEC (0x4E7A/0x4E7B) — VBR/SFC/DFC/USP アクセス、特権、未定義Cr       */
/* ================================================================== */

/* MOVEC D0,VBR → TRAP #0 が新VBR基準のベクタ（vbr + 32*4）を読むこと。 */
static void test_movec_vbr_relocation_trap(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t new_vbr = 0x00002000u;
    uint32_t handler = 0x00003000u;
    write_long_be(ram, new_vbr + 32u * 4u, handler); /* TRAP #0 = ベクタ32 */
    write_word_be(ram, handler, 0x60FE);             /* BRA * */

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x203C);
    p += 2; /* MOVE.L #imm, D0 */
    write_long_be(ram, p, new_vbr);
    p += 4;
    write_word_be(ram, p, 0x4E7B);
    p += 2; /* MOVEC D0, VBR */
    write_word_be(ram, p, 0x0801);
    p += 2;
    write_word_be(ram, p, 0x4E40); /* TRAP #0 */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.vbr == new_vbr);
    assert(r.pc == handler);
}

/* MOVEC D0,VBR → level3オートベクタ割り込みも新VBR基準（vbr + (24+3)*4）で読むこと。 */
static void test_movec_vbr_relocation_irq(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t new_vbr = 0x00004000u;
    uint32_t handler = 0x00005000u;
    int level = 3;
    write_long_be(ram, new_vbr + (uint32_t)(24 + level) * 4u, handler);
    write_word_be(ram, handler + 0, 0x323C); /* MOVE.W #0x55, D1 */
    write_word_be(ram, handler + 2, 0x0055);
    write_word_be(ram, handler + 4, 0x4E73); /* RTE */

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x203C);
    p += 2; /* MOVE.L #imm, D0 */
    write_long_be(ram, p, new_vbr);
    p += 4;
    write_word_be(ram, p, 0x4E7B);
    p += 2; /* MOVEC D0, VBR */
    write_word_be(ram, p, 0x0801);
    p += 2;
    write_word_be(ram, p, 0x027C); /* ANDI.W #0xF8FF, SR（割り込みマスクを0へ）*/
    p += 2;
    write_word_be(ram, p, 0xF8FF);
    p += 2;
    write_word_be(ram, p, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, level);
    mc68k_execute(cpu, 100);
    mc68k_set_irq(cpu, 0); /* 再トリガー防止 */
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.vbr == new_vbr);
    assert((r.d[1] & 0xFFFF) == 0x55); /* ハンドラに到達しRTEで正常復帰 */
    assert(r.a[7] == RESET_SSP);
}

/* ユーザモードで MOVEC → 特権違反（ベクタ8）、かつ拡張ワード未消費
 * （＝積まれたPCが命令先頭）であること。 */
static void test_movec_privilege_violation(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_user_mode_common(ram);
    uint32_t movec_pc = RESET_PC + 4;
    write_word_be(ram, movec_pc + 0, 0x4E7B); /* MOVEC D0, VBR */
    write_word_be(ram, movec_pc + 2, 0x0801);
    write_word_be(ram, movec_pc + 4, 0x60FE); /* 違反しなければここへ */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VEC8_HANDLER);
    assert(r.a[7] == RESET_SSP - 8); /* 68010: フォーマットワード付き8バイトフレーム */
    assert(read_long_be(ram, r.a[7] + 2) == movec_pc); /* 拡張ワード未消費＝命令先頭 */
    assert(read_word_be(ram, r.a[7] + 6) ==
           0x0020);     /* フォーマット$0 + ベクタオフセット(8*4) */
    assert(r.vbr == 0); /* VBRは変更されていない */
}

/* 未定義の制御レジスタ番号 → ILLEGAL命令例外（ベクタ4）。 */
static void test_movec_illegal_creg(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x10, VEC4_HANDLER);
    write_word_be(ram, VEC4_HANDLER, 0x60FE);

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x4E7A); /* MOVEC Rc,Rn: 未定義Cr → D0 */
    p += 2;
    write_word_be(ram, p, 0x0123); /* is_an=0, reg=0, creg=0x123（未定義） */
    p += 2;
    write_word_be(ram, p, 0x60FE); /* 到達しないはず */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VEC4_HANDLER);
    assert(r.a[7] == RESET_SSP - 8); /* 68010: フォーマットワード付き8バイトフレーム */
    assert(read_long_be(ram, r.a[7] + 2) == RESET_PC); /* 命令先頭（pc-4） */
    assert(read_word_be(ram, r.a[7] + 6) ==
           0x0010); /* フォーマット$0 + ベクタオフセット(4*4) */
}

/* SFC/DFC の書き込みは下位3bitのみ有効。読み出しはlong転送でゼロ拡張。 */
static void test_movec_sfc_dfc_mask(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x203C);
    p += 2; /* MOVE.L #0xFFFFFFFF, D0 */
    write_long_be(ram, p, 0xFFFFFFFFu);
    p += 4;
    write_word_be(ram, p, 0x4E7B);
    p += 2; /* MOVEC D0, SFC */
    write_word_be(ram, p, 0x0000);
    p += 2;
    write_word_be(ram, p, 0x4E7B);
    p += 2; /* MOVEC D0, DFC */
    write_word_be(ram, p, 0x0001);
    p += 2;
    write_word_be(ram, p, 0x4E7A);
    p += 2; /* MOVEC SFC, D1 */
    write_word_be(ram, p, 0x1000);
    p += 2;
    write_word_be(ram, p, 0x4E7A);
    p += 2; /* MOVEC DFC, D2 */
    write_word_be(ram, p, 0x2001);
    p += 2;
    write_word_be(ram, p, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.d[1] == 0x00000007u); /* SFC: 下位3bitのみ保持、long読み出しはゼロ拡張 */
    assert(r.d[2] == 0x00000007u); /* DFC: 同上 */
    assert(r.sfc == 0x07);
    assert(r.dfc == 0x07);
}

/* ================================================================== */
/* RTD (0x4E74) — PC ← (SP); SP ← SP + 4 + d（符号付き）                 */
/* ================================================================== */

static void test_rtd_positive_disp(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t return_target = 0x00003000u;
    write_long_be(ram, RESET_SSP, return_target); /* RTD が読むリターンアドレス */
    write_word_be(ram, return_target, 0x60FE);    /* BRA * */

    write_word_be(ram, RESET_PC + 0, 0x4E74); /* RTD */
    write_word_be(ram, RESET_PC + 2, 0x0010); /* #$0010 */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == return_target);
    assert(r.a[7] == RESET_SSP + 4 + 0x10);
}

static void test_rtd_negative_disp(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t return_target = 0x00003000u;
    write_long_be(ram, RESET_SSP, return_target);
    write_word_be(ram, return_target, 0x60FE);

    write_word_be(ram, RESET_PC + 0, 0x4E74); /* RTD */
    write_word_be(ram, RESET_PC + 2, 0xFFFC); /* #-4 */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == return_target);
    assert(r.a[7] == RESET_SSP); /* +4-4 = 0 */
}

/* ================================================================== */
/* MOVES (0x0E00-0x0EFF)                                                    */
/* ================================================================== */

/* MOVES.L (A0),D0: メモリ→Dnのlong転送。 */
static void test_moves_mem_to_dn_long(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t data_addr = 0x00003000u;
    write_long_be(ram, data_addr, 0x12345678u);

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm, A0 */
    p += 2;
    write_long_be(ram, p, data_addr);
    p += 4;
    write_word_be(ram, p, 0x0E90); /* MOVES.L (A0), D0（size=L, ea=mode2/reg0） */
    p += 2;
    write_word_be(ram, p, 0x0000); /* ext: is_an=0, reg=0(D0), dr=0（メモリ→レジスタ） */
    p += 2;
    write_word_be(ram, p, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x12345678u);
}

/* MOVES.B D1,(A0): Dn→メモリのbyte転送。 */
static void test_moves_dn_to_mem_byte(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t dst_addr = 0x00003100u;

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm, A0 */
    p += 2;
    write_long_be(ram, p, dst_addr);
    p += 4;
    write_word_be(ram, p, 0x727F); /* MOVEQ #0x7F, D1 */
    p += 2;
    write_word_be(ram, p, 0x0E10); /* MOVES.B D1,(A0)（size=B, ea=mode2/reg0） */
    p += 2;
    write_word_be(ram, p, 0x1800); /* ext: is_an=0, reg=1(D1), dr=1（レジスタ→メモリ） */
    p += 2;
    write_word_be(ram, p, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_destroy(cpu);

    assert(ram[dst_addr] == 0x7F);
}

/* PMMU の TT は一致時だけ変換を透過する。TC.E を立てて通常変換を必ず
 * 失敗させることで、MOVES の実データサイクルが SFC/DFC を使うことを
 * 命令実行経路で検証する。LAB=0/LAM=FF は全論理アドレスに一致させる。 */
static uint32_t tt_for_fc(unsigned fc, bool rwm) {
    return UINT32_C(0x00ff8000) | (rwm ? UINT32_C(0x100) : 0) | ((fc & 7u) << 4);
}

static void configure_moves_fc_test(mc68k_state_t *cpu, unsigned data_fc) {
    cpu->mmu_tc = UINT32_C(0x80000000); /* table walk must fail if TT misses */
    cpu->mmu_tt0 = tt_for_fc(6, true);  /* supervisor instruction fetch */
    cpu->mmu_tt1 = tt_for_fc(data_fc, true);
}

static void test_moves_uses_sfc_on_memory_read(void) {
    uint8_t ram[RAM_SIZE] = {0};
    const uint32_t data_addr = 0x00003000u;
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x0E90); /* MOVES.L (A0),D0 */
    write_word_be(ram, RESET_PC + 2, 0x0000);
    write_long_be(ram, data_addr, 0x12345678u);

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68030);
    mc68k_reset(cpu);
    cpu->a[0] = data_addr;
    cpu->sfc = 3;
    configure_moves_fc_test(cpu, 3);
    mc68k_execute(cpu, 1);

    assert(cpu->exec_state == CPU_NORMAL && cpu->d[0] == 0x12345678u);
    mc68k_destroy(cpu);
}

static void test_moves_uses_dfc_on_memory_write(void) {
    uint8_t ram[RAM_SIZE] = {0};
    const uint32_t data_addr = 0x00003100u;
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x0E10); /* MOVES.B D1,(A0) */
    write_word_be(ram, RESET_PC + 2, 0x1800);

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68030);
    mc68k_reset(cpu);
    cpu->a[0] = data_addr;
    cpu->d[1] = 0x7f;
    cpu->dfc = 4;
    configure_moves_fc_test(cpu, 4);
    mc68k_execute(cpu, 1);

    assert(cpu->exec_state == CPU_NORMAL && ram[data_addr] == 0x7f);
    mc68k_destroy(cpu);
}

/* TAS の operand cycle は RMW である。TT の RWM=0 はRMWを透過変換
 * しないため、TASは table walk の物理ページを使う。RMWフラグを落とすと
 * read は TT を通って論理ページを読み、write だけが table walk になるので
 * 物理ページの結果が異なる。この差で FC/RMW 文脈の伝播を検証する。 */
static void test_tas_marks_rmw_for_tt(void) {
    uint8_t ram[RAM_SIZE] = {0};
    const uint32_t logical = 0x00003010u, physical = 0x00004010u;
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x4AD0); /* TAS (A0) */
    ram[logical] = 0x01;
    ram[physical] = 0x02;
    /* TC=$82c0aa00: 4KiB, A/B 10bit. CRP's short A table maps A[0]
     * to B at $0800, and B[3] maps logical $3010 to physical $4010. */
    write_long_be(ram, 0x0400, 0x00000802);
    write_long_be(ram, 0x080c, 0x00004001);

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68030);
    mc68k_reset(cpu);
    cpu->a[0] = logical;
    cpu->mmu_tc = UINT32_C(0x82c0aa00);
    cpu->mmu_crp = cpu->mmu_srp = UINT64_C(0x0000000200000400);
    cpu->mmu_tt0 = tt_for_fc(6, true);                    /* opcode fetches */
    cpu->mmu_tt1 = tt_for_fc(5, false) | UINT32_C(0x200); /* read-only TT */
    mc68k_execute(cpu, 1);

    assert(cpu->exec_state == CPU_NORMAL && ram[logical] == 0x01 && ram[physical] == 0x82);
    mc68k_destroy(cpu);
}

/* MOVES.W (A0),A1: メモリ→Anのword転送は符号拡張して32bit書き込み（MOVEAと同じ規則）。 */
static void test_moves_mem_to_an_sign_extend(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t data_addr = 0x00003200u;
    write_word_be(ram, data_addr, 0x8000u); /* 16bitでは負値 */

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm, A0 */
    p += 2;
    write_long_be(ram, p, data_addr);
    p += 4;
    write_word_be(ram, p, 0x0E50); /* MOVES.W (A0),A1（size=W, ea=mode2/reg0） */
    p += 2;
    write_word_be(ram, p, 0x9000); /* ext: is_an=1, reg=1(A1), dr=0（メモリ→レジスタ） */
    p += 2;
    write_word_be(ram, p, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.a[1] == 0xFFFF8000u);
}

/* ユーザモードで MOVES → 特権違反（ベクタ8）、拡張ワード・EA共に未消費であること。 */
static void test_moves_user_mode_privilege_violation(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_user_mode_common(ram);
    uint32_t moves_pc = RESET_PC + 4;
    write_word_be(ram, moves_pc + 0, 0x0E90); /* MOVES.L (A0),D0 */
    write_word_be(ram, moves_pc + 2, 0x0000);
    write_word_be(ram, moves_pc + 4, 0x60FE); /* 違反しなければここへ */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VEC8_HANDLER);
    assert(r.a[7] == RESET_SSP - 8); /* 68010: フォーマットワード付き8バイトフレーム */
    assert(read_long_be(ram, r.a[7] + 2) == moves_pc);
    assert(read_word_be(ram, r.a[7] + 6) ==
           0x0020); /* フォーマット$0 + ベクタオフセット(8*4) */
}

/* ================================================================== */
/* フォーマットワード付き例外フレーム（ステップ7+8）                      */
/* ================================================================== */

/* TRAP #1 (ベクタ33) 後の8バイトフレームを全バイト検証。
 * フォーマットワード = $0000 | (33*4) = $0084。 */
static void test_frame_format_word_full_bytes(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 33u * 4u, VEC33_HANDLER);
    write_word_be(ram, VEC33_HANDLER, 0x60FE); /* BRA * */

    write_word_be(ram, RESET_PC + 0, 0x4E41); /* TRAP #1 */
    uint32_t next_pc = RESET_PC + 2;

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VEC33_HANDLER);
    assert(r.a[7] == RESET_SSP - 8);
    assert(read_word_be(ram, r.a[7] + 0) == 0x2700);  /* 保存SR（リセット直後の値） */
    assert(read_long_be(ram, r.a[7] + 2) == next_pc); /* TRAP次命令のアドレス */
    assert(read_word_be(ram, r.a[7] + 6) == 0x0084);  /* フォーマット$0 + オフセット(33*4) */
}

/* TRAP #1 → RTE → 次命令へ復帰、SPが元に戻ること。 */
static void test_trap_rte_roundtrip_68010(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 33u * 4u, VEC33_HANDLER);
    write_word_be(ram, VEC33_HANDLER + 0, 0x763C); /* MOVEQ #0x3C, D3（マーカー） */
    write_word_be(ram, VEC33_HANDLER + 2, 0x4E73); /* RTE */

    write_word_be(ram, RESET_PC + 0, 0x4E41); /* TRAP #1 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA *（復帰後ここへ） */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[3] & 0xFF) == 0x3C);
    assert(r.pc == RESET_PC + 2);
    assert(r.a[7] == RESET_SSP);
}

/* SP+6に不正フォーマット($1)を仕込んでRTE→フォーマットエラー（ベクタ14）。
 * SPはRTE実行前の値のまま、積むPCはRTE命令自身のアドレス。 */
static void test_format_error(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 14u * 4u, VEC14_HANDLER);
    write_word_be(ram, VEC14_HANDLER, 0x60FE); /* BRA * */

    write_word_be(ram, RESET_SSP + 6, 0x1000); /* 不正フォーマット$1 */
    write_word_be(ram, RESET_PC, 0x4E73);      /* RTE */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VEC14_HANDLER);
    assert(r.a[7] == RESET_SSP - 8); /* RTE前(RESET_SSP)から新規に8バイトフレームが積まれる */
    assert(read_long_be(ram, r.a[7] + 2) == RESET_PC); /* RTE命令自身のアドレス */
    assert(read_word_be(ram, r.a[7] + 6) == 0x0038);   /* フォーマット$0 + オフセット(14*4) */
}

/* T=1でRTEがフォーマットエラーを起こした後もトレース例外（ベクタ9）が
 * 抑止されずに続くこと（フォーマットエラーはGroup2、現行仕様）。 */
static void test_format_error_trace_not_suppressed(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 14u * 4u, VEC14_HANDLER);
    write_word_be(ram, VEC14_HANDLER,
                  0x4E71); /* NOP（トレース追随の前で止まる想定なので未実行） */
    write_long_be(ram, 9u * 4u, VEC9_HANDLER);
    write_word_be(ram, VEC9_HANDLER + 0, 0x7677); /* MOVEQ #0x77, D3（マーカー） */
    write_word_be(ram, VEC9_HANDLER + 2, 0x60FE); /* BRA * */

    write_word_be(ram, RESET_SSP + 6, 0x1000); /* 不正フォーマット$1 */
    write_word_be(ram, RESET_PC + 0,
                  0x007C); /* ORI.W #0x8000, SR（T=1、この命令自体は旧T=0なので未トレース） */
    write_word_be(ram, RESET_PC + 2, 0x8000);
    write_word_be(ram, RESET_PC + 4, 0x4E73); /* RTE（フォーマットエラー→トレース追随） */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[3] & 0xFF) == 0x77); /* ベクタ9ハンドラまで到達した */
}

/* 奇数アドレスwordアクセス（アドレスエラー）でグループ0長フレーム（フォーマット$8、
 * 29ワード=58バイト）が積まれ、フォーマットワード上位4bitが$8、フォールト
 * アドレスと命令先頭PCが正しいこと。 */
static void test_group0_long_frame_basic(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);
    write_word_be(ram, ADDR3_HANDLER, 0x60FE); /* BRA * */

    uint32_t fault_addr = 0x00003400u + 1u; /* 奇数アドレス */
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm, A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    uint32_t fault_instr_pc = p;
    write_word_be(ram, p, 0x4A50); /* TST.W (A0) → アドレスエラー */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == ADDR3_HANDLER);
    assert(r.a[7] == RESET_SSP - 58);
    assert((read_word_be(ram, r.a[7] + 6) >> 12) == 0x8);    /* フォーマット上位4bit */
    assert(read_long_be(ram, r.a[7] + 2) == fault_instr_pc); /* 命令先頭PC */
    assert(read_long_be(ram, r.a[7] + 0x0A) == fault_addr);  /* フォールトアドレス */
}

/* フォーマット$8のRTEはフレームを破棄してスタックされたPCから再開する近似
 * 。ハンドラがA0を偶数アドレスへ補正してからRTE
 * すれば、再開したTST.W (A0)は再フォルトせず先へ進む。 */
static void test_group0_long_frame_rte_resumes(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);

    uint32_t fault_addr = 0x00003500u + 1u; /* 奇数アドレス */
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm, A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x4A50); /* TST.W (A0)：ここでアドレスエラー */
    p += 2;
    write_word_be(ram, p, 0x7699); /* MOVEQ #0x99, D3（再開後に到達） */
    p += 2;
    write_word_be(ram, p, 0x60FE); /* BRA * */

    /* ハンドラ: A0 を偶数アドレスへ補正してから RTE */
    write_word_be(ram, ADDR3_HANDLER + 0, 0x207C); /* MOVEA.L #imm, A0 */
    write_long_be(ram, ADDR3_HANDLER + 2, fault_addr + 1);
    write_word_be(ram, ADDR3_HANDLER + 6, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[3] & 0xFF) == 0x99); /* 再開後の命令列まで到達 */
    assert(r.a[7] == RESET_SSP);     /* フレーム破棄後、スタックは元に戻っている */
}

/* ハンドラがフォルト条件を解消せずRTEすると同じ命令・同じアドレスで
 * 再フォルトする。再フォルト安全弁が
 * CPU_HALTEDへ落とすこと。 */
static void test_group0_refault_safety_valve_halts(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);

    uint32_t fault_addr = 0x00003600u + 1u; /* 奇数アドレス */
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm, A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x4A50); /* TST.W (A0) */
    p += 2;
    write_word_be(ram, p, 0x60FE); /* 到達しないはず */

    /* ハンドラ: A0を補正せずそのままRTE → 同一フォルトが再発する */
    write_word_be(ram, ADDR3_HANDLER, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);

    assert(mc68k_get_exec_state(cpu) == CPU_HALTED);
    mc68k_destroy(cpu);
}

/* $8フレームRTEで再開した命令が正常完了し、その後別の命令を挟んでから
 * 全く無関係に同一PC・同一アクセスアドレス・同一ベクタのフォルトが再発しても、
 * 「連続再発」ではないため誤ってCPU_HALTEDにならないこと（現行仕様
 * #25 §6.3.2、Opusレビュー2026-08-25で発見・修正した誤検出シナリオ）。
 * D2をループカウンタにして意図的に2回フォルトさせる。 */
static void test_group0_refault_guard_clears_after_success(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);

    uint32_t fault_addr = 0x00003700u + 1u; /* 奇数アドレス */

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x7402); /* MOVEQ #2, D2（ループカウンタ） */
    p += 2;
    uint32_t loop_start = p;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm, A0（毎回奇数へ再アーム） */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x4A50); /* TST.W (A0)：ここで2回フォルトする */
    p += 2;
    write_word_be(ram, p, 0x5342); /* SUBQ.W #1, D2 */
    p += 2;
    uint32_t bne_pc = p;
    write_word_be(ram, p, 0x6600); /* BNE.W loop_start */
    p += 2;
    write_word_be(ram, p, (uint16_t)(loop_start - (bne_pc + 2)));
    p += 2;
    write_word_be(ram, p, 0x7877); /* MOVEQ #0x77, D4（両方のフォルトを無事通過した目印） */
    p += 2;
    write_word_be(ram, p, 0x60FE); /* BRA * */

    /* ハンドラ: A0 を偶数アドレスへ補正してから RTE（1回目・2回目とも同じ）。 */
    write_word_be(ram, ADDR3_HANDLER + 0, 0x207C);
    write_long_be(ram, ADDR3_HANDLER + 2, fault_addr + 1);
    write_word_be(ram, ADDR3_HANDLER + 6, 0x4E73);

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 1000);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    cpu_exec_state_t exec_state = mc68k_get_exec_state(cpu);
    mc68k_destroy(cpu);

    assert(exec_state != CPU_HALTED); /* 誤検出でHALTしていないこと */
    assert(r.d[2] == 0);              /* ループがちょうど2回完走した */
    assert((r.d[4] & 0xFF) == 0x77);  /* 最終マーカーまで到達 */
}

/* ================================================================== */
/* 現行仕様 のテスト           */
/* ================================================================== */

/* R-1/R-2/R-4/R-5・S-1/S-2/S-4/S-5/S-8・C-1〜C-4 は make_cpu() の
 * mem_ctx_t ベースのバスで足りる（アドレスエラーはバスに到達する前に
 * cpu_internal.h 内で判定されるため）。R-3/S-3/S-6/S-7 だけは真の
 * バスエラー（未マップアドレス）が要るため、test_bus_error.c と同じ
 * 流儀で自前のバスフックを用意する（make_cpu() の mem_ctx_t は未マップ
 * 領域を0xFFへフォールバックするだけでバスエラーを発生させないため）。 */

#define HOLE_RAM_SIZE   0x10000u
#define HOLE_WINDOW_LEN 4u          /* HOLE_RAM_SIZE以降、hole_openがtrueの間だけ読める窓 */
#define HOLE_OPEN_ADDR  0x00020000u /* テスト専用の裏口: 書き込むとhole_openを立てる */
#define FAR_HOLE_ADDR   0x00800000u /* 常時未マップ（hole_openと無関係） */

typedef struct {
    uint8_t ram[HOLE_RAM_SIZE];
    uint8_t hole_buf[HOLE_WINDOW_LEN];
    bool hole_open;
} hole_ctx_t;

static bool hole_read_byte(void *ctxp, uint32_t addr, uint8_t *out) {
    hole_ctx_t *h = (hole_ctx_t *)ctxp;
    if (addr < HOLE_RAM_SIZE) {
        *out = h->ram[addr];
        return true;
    }
    if (h->hole_open && addr < HOLE_RAM_SIZE + HOLE_WINDOW_LEN) {
        *out = h->hole_buf[addr - HOLE_RAM_SIZE];
        return true;
    }
    *out = 0xFF;
    return false;
}

static bool hole_read_word(void *ctxp, uint32_t addr, uint16_t *out) {
    uint8_t hi, lo;
    if (!hole_read_byte(ctxp, addr, &hi) || !hole_read_byte(ctxp, addr + 1, &lo)) {
        *out = 0xFFFF;
        return false;
    }
    *out = (uint16_t)((hi << 8) | lo);
    return true;
}

static bool hole_read_long(void *ctxp, uint32_t addr, uint32_t *out) {
    uint16_t hi, lo;
    if (!hole_read_word(ctxp, addr, &hi) || !hole_read_word(ctxp, addr + 2, &lo)) {
        *out = 0xFFFFFFFF;
        return false;
    }
    *out = ((uint32_t)hi << 16) | lo;
    return true;
}

static bool hole_write_byte(void *ctxp, uint32_t addr, uint8_t val) {
    hole_ctx_t *h = (hole_ctx_t *)ctxp;
    if (addr == HOLE_OPEN_ADDR) {
        h->hole_open = true;
        return true;
    }
    if (addr < HOLE_RAM_SIZE) {
        h->ram[addr] = val;
        return true;
    }
    return false;
}

static bool hole_write_word(void *ctxp, uint32_t addr, uint16_t val) {
    return hole_write_byte(ctxp, addr, (uint8_t)(val >> 8)) &&
           hole_write_byte(ctxp, addr + 1, (uint8_t)val);
}

static bool hole_write_long(void *ctxp, uint32_t addr, uint32_t val) {
    return hole_write_word(ctxp, addr, (uint16_t)(val >> 16)) &&
           hole_write_word(ctxp, addr + 2, (uint16_t)val);
}

static void hole_sync(void *ctxp, int cycles) {
    (void)ctxp;
    (void)cycles;
}
static void hole_write_hook(void *ctxp, uint32_t addr, uint32_t size) {
    (void)ctxp;
    (void)addr;
    (void)size;
}

static mc68k_state_t *make_cpu_hole(hole_ctx_t *h, mc68k_model_t model) {
    mc68k_bus_t bus = {
        .read_byte = hole_read_byte,
        .read_word = hole_read_word,
        .read_long = hole_read_long,
        .write_byte = hole_write_byte,
        .write_word = hole_write_word,
        .write_long = hole_write_long,
        .sync = hole_sync,
        .write_hook = hole_write_hook,
        .iack = NULL,
        .reset_peripheral = NULL,
        .ctx = h,
    };
    return mc68k_create_model(&bus, model);
}

/* ---- R-1〜R-5: 命令リスタート機構のロールバック（A-1） ---- */

/* R-1: MOVE.W (A0)+,D0 を奇数A0で実行→アドレスエラー。A-1のロールバックに
 * よりハンドラが見るA0は命令開始時の値（奇数のfault_addr）に戻っている。
 * 偶数へ補正してRTEすると、命令全体が再実行され、A0は補正値+2になる
 * （ロールバックが無いと既にインクリメント済みのA0がさらに進み+4になる）。
 *
 * ハンドラは固定値をA0へ書くのではなく、入口のA0をD6へ退避してから
 * SUBQ.L #1,A0で相対補正する。固定値上書きだと、ロールバックが欠落して
 * A0が二重更新（fault_addr+2）のまま入っても補正結果が偶数アドレスに
 * なってしまい、最終状態が偶然「正しく見える」ため検出できない
 * （mc68000#39 gpt-6-Solレビュー指摘。§7.2 S-2相当の再演で確認）。
 * D6の値を命令開始時のfault_addrと比較することで、ロールバック自体を
 * 直接検証する。 */
static void test_restart_rollback_postinc_read(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);

    uint32_t even_addr = 0x00003000u;
    uint32_t fault_addr = even_addr + 1u; /* 奇数 */
    write_word_be(ram, even_addr, 0x1234);

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm, A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x3018); /* MOVE.W (A0)+,D0：ここでアドレスエラー */
    p += 2;
    write_word_be(ram, p, 0x60FE); /* BRA *（再開後に到達） */

    write_word_be(ram, ADDR3_HANDLER + 0, 0x2C08); /* MOVE.L A0,D6（入口A0を退避） */
    write_word_be(ram, ADDR3_HANDLER + 2, 0x5388); /* SUBQ.L #1,A0（相対補正） */
    write_word_be(ram, ADDR3_HANDLER + 4, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.d[6] == fault_addr);    /* ハンドラ入口のA0＝命令開始時の値（ロールバック確認） */
    assert(r.a[0] == even_addr + 2); /* +4（二重更新）にはならない */
    assert((r.d[0] & 0xFFFF) == 0x1234);
}

/* R-2: MOVE.W D0,-(A0) を奇数アドレスへ実行→アドレスエラー。プレデクリメント
 * は書き込みより先に（無条件に）実行されるが、A-1のロールバックによりハンドラ
 * が見るA0は命令開始時の値に戻っている。補正してRTEすると、A0は1回だけ
 * デクリメントされる（ロールバックが無いと再実行時にもう一度デクリメントされ
 * 二重更新になる）。
 *
 * R-1と同じ理由でハンドラは固定値ではなくSUBQ.L #1,A0による相対補正にする
 * （mc68000#39 gpt-6-Solレビュー指摘）。入口A0はD6へ退避し、命令開始時の
 * initial_a0と一致することを直接検証する。 */
static void test_restart_rollback_predec_write(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);

    uint32_t even_target = 0x00003100u;
    uint32_t initial_a0 = even_target + 2u + 1u; /* 命令開始時のA0。補正前: A0-2が奇数 */
    /* ハンドラのSUBQ.L #1,A0で initial_a0-1 == even_target+2 になり、
     * 再実行時のプレデクリメントで even_target+2-2 == even_target に一致する。 */

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x203C); /* MOVE.L #imm, D0 */
    p += 2;
    write_long_be(ram, p, 0x0000ABCDu);
    p += 4;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm, A0 */
    p += 2;
    write_long_be(ram, p, initial_a0);
    p += 4;
    write_word_be(ram, p, 0x3100); /* MOVE.W D0,-(A0)：ここでアドレスエラー */
    p += 2;
    write_word_be(ram, p, 0x60FE); /* BRA * */

    write_word_be(ram, ADDR3_HANDLER + 0, 0x2C08); /* MOVE.L A0,D6（入口A0を退避） */
    write_word_be(ram, ADDR3_HANDLER + 2, 0x5388); /* SUBQ.L #1,A0（相対補正） */
    write_word_be(ram, ADDR3_HANDLER + 4, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.d[6] == initial_a0);  /* ハンドラ入口のA0＝命令開始時の値（ロールバック確認） */
    assert(r.a[0] == even_target); /* 1回だけデクリメント */
    assert(read_word_be(ram, even_target) == 0xABCD);
}

/* R-3: MOVEM.W (A0)+,D0-D3 の転送途中でバスエラー（未マップ領域をまたぐ）。
 * D0/D1/A0は最初の（失敗した）試行で部分的に更新されるが、A-1のロールバック
 * により命令開始時の値へ戻る。ハンドラが該当ウィンドウをマップしてRTEすると
 * 命令全体が再実行され、A0とD0-D3が単一の完全な実行結果と一致すること
 * （部分転送分が二重に反映されない）。
 *
 * D0/D1は最終値（0x1111/0x2222）がMOVEM再実行で無条件に上書きされるため、
 * 最終値だけを見るassertはD0-D7のロールバックが欠落していても偶然通る
 * （mc68000#39 gpt-6-Solレビュー: D0-D7ロールバックだけを外した変異でも
 * test_68010全体がパスすると確認済み）。ハンドラの先頭でD0/D1をD6/D7へ退避し、
 * ハンドラ入口の値が命令開始時のD0/D1（未初期化＝0、部分転送前の値）と
 * 一致することを直接検証する。D2/D3は未マップ領域なので、フォルト時点で
 * まだ転送されておらずロールバックの有無に関わらず0のままのため対象外。 */
static void test_restart_rollback_movem_bus_error(void) {
    hole_ctx_t h;
    memset(&h, 0, sizeof(h));
    write_long_be(h.ram, 0, RESET_SSP);
    write_long_be(h.ram, 4, RESET_PC);
    write_long_be(h.ram, 2u * 4u, BUS2_HANDLER);

    uint32_t a0_init = HOLE_RAM_SIZE - 4u;
    write_word_be(h.ram, a0_init + 0, 0x1111); /* D0 */
    write_word_be(h.ram, a0_init + 2, 0x2222); /* D1 */
    write_word_be(h.hole_buf, 0, 0x3333);      /* D2（マップ前は未マップ） */
    write_word_be(h.hole_buf, 2, 0x4444);      /* D3 */

    uint32_t p = RESET_PC;
    write_word_be(h.ram, p, 0x207C); /* MOVEA.L #imm, A0 */
    p += 2;
    write_long_be(h.ram, p, a0_init);
    p += 4;
    write_word_be(h.ram, p, 0x4C98); /* MOVEM.W (A0)+,reglist */
    p += 2;
    write_word_be(h.ram, p, 0x000F); /* mask: D0-D3 */
    p += 2;
    write_word_be(h.ram, p, 0x60FE); /* BRA *（再開後に到達） */

    /* ハンドラ: まずD0/D1（フォルト時点で部分転送済み）をD6/D7へ退避してから、
     * HOLE_OPEN_ADDR へ書いてウィンドウをマップしてRTE。 */
    write_word_be(h.ram, BUS2_HANDLER + 0, 0x3C00); /* MOVE.W D0,D6 */
    write_word_be(h.ram, BUS2_HANDLER + 2, 0x3E01); /* MOVE.W D1,D7 */
    write_word_be(h.ram, BUS2_HANDLER + 4, 0x13FC); /* MOVE.B #0,addr.L */
    write_word_be(h.ram, BUS2_HANDLER + 6, 0x0000);
    write_long_be(h.ram, BUS2_HANDLER + 8, HOLE_OPEN_ADDR);
    write_word_be(h.ram, BUS2_HANDLER + 12, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu_hole(&h, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[6] & 0xFFFF) ==
           0x0000); /* ハンドラ入口のD0＝命令開始時の値（ロールバック確認） */
    assert((r.d[7] & 0xFFFF) ==
           0x0000); /* 同D1（ロールバックが無いと部分転送済みの0x2222が残る） */
    assert(r.a[0] == a0_init + 8);
    assert((r.d[0] & 0xFFFF) == 0x1111);
    assert((r.d[1] & 0xFFFF) == 0x2222);
    assert((r.d[2] & 0xFFFF) == 0x3333);
    assert((r.d[3] & 0xFFFF) == 0x4444);
}

/* R-4: ハンドラがA0を補正せずRTEすると、ロールバックにより再フォルトの
 * アクセスアドレスが前回と一致し、再フォルト安全弁がCPU_HALTEDへ落とす
 * こと（ロールバックが無いと二重インクリメントでアドレスが変わり、
 * 安全弁の条件4（アクセスアドレス一致）が崩れて効かなくなる）。 */
static void test_restart_rollback_refault_matches_safety_valve(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);

    uint32_t fault_addr = 0x00003800u + 1u; /* 奇数 */
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm,A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x3018); /* MOVE.W (A0)+,D0 */

    write_word_be(ram, ADDR3_HANDLER, 0x4E73); /* 何もせずRTE */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);

    assert(mc68k_get_exec_state(cpu) == CPU_HALTED);
    mc68k_destroy(cpu);
}

/* R-5: 68000モデルでは命令再実行用のロールバックが行われないこと
 * （A-1が68000のグループ0経路に一切触れないことの回帰固定）。 */
static void test_restart_rollback_not_applied_on_68000(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);
    write_word_be(ram, ADDR3_HANDLER, 0x60FE); /* BRA *（補正せず観測するだけ） */

    uint32_t fault_addr = 0x00003900u + 1u;
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm,A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x3018); /* MOVE.W (A0)+,D0 */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68000);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == ADDR3_HANDLER);
    assert(r.a[0] == fault_addr + 2); /* ロールバックされず、既にインクリメント済みのまま */
}

/* ---- S-1〜S-8: SSWの各フィールド（A-2/A-3） ---- */

/* S-1: TST.W (A0)（A0奇数、スーパーバイザ）。 */
static void test_ssw_read_word_address_error(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);
    write_word_be(ram, ADDR3_HANDLER, 0x60FE);

    uint32_t fault_addr = 0x00003200u + 1u;
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm,A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x4A50); /* TST.W (A0) */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint16_t ssw = read_word_be(ram, r.a[7] + 0x08);
    mc68k_destroy(cpu);

    assert((ssw & 0x8000) == 0); /* RR=0 */
    assert((ssw & 0x2000) == 0); /* IF=0 */
    assert((ssw & 0x1000) != 0); /* DF=1 */
    assert((ssw & 0x0800) == 0); /* RM=0 */
    assert((ssw & 0x0200) == 0); /* BY=0（ワード転送） */
    assert((ssw & 0x0100) != 0); /* RW=1（読み出し） */
    assert((ssw & 0x0007) == 5); /* FC=5（スーパーバイザデータ） */
}

/* S-2: MOVE.W D0,(A0)（A0奇数、スーパーバイザ）。DOBにD0下位ワードが積まれる。 */
static void test_ssw_write_word_address_error(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);
    write_word_be(ram, ADDR3_HANDLER, 0x60FE);

    uint32_t fault_addr = 0x00003300u + 1u;
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x203C); /* MOVE.L #imm,D0 */
    p += 2;
    write_long_be(ram, p, 0x0000BEEFu);
    p += 4;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm,A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x3080); /* MOVE.W D0,(A0) */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint16_t ssw = read_word_be(ram, r.a[7] + 0x08);
    uint16_t dob = read_word_be(ram, r.a[7] + 0x10);
    mc68k_destroy(cpu);

    assert((ssw & 0x1000) == 0); /* DF=0 */
    assert((ssw & 0x0100) == 0); /* RW=0（書き込み） */
    assert((ssw & 0x0200) == 0); /* BY=0（ワード） */
    assert((ssw & 0x0007) == 5); /* FC=5 */
    assert(dob == 0xBEEF);
}

/* S-3: MOVE.B D0,(A0)（未マップ領域への書き込み、バスエラー）。 */
static void test_ssw_write_byte_bus_error(void) {
    hole_ctx_t h;
    memset(&h, 0, sizeof(h));
    write_long_be(h.ram, 0, RESET_SSP);
    write_long_be(h.ram, 4, RESET_PC);
    write_long_be(h.ram, 2u * 4u, BUS2_HANDLER);
    write_word_be(h.ram, BUS2_HANDLER, 0x60FE); /* BRA * */

    uint32_t p = RESET_PC;
    write_word_be(h.ram, p, 0x203C); /* MOVE.L #imm,D0 */
    p += 2;
    write_long_be(h.ram, p, 0x000000AAu);
    p += 4;
    write_word_be(h.ram, p, 0x207C); /* MOVEA.L #imm,A0 */
    p += 2;
    write_long_be(h.ram, p, FAR_HOLE_ADDR);
    p += 4;
    write_word_be(h.ram, p, 0x1080); /* MOVE.B D0,(A0) */

    mc68k_state_t *cpu = make_cpu_hole(&h, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint16_t ssw = read_word_be(h.ram, r.a[7] + 0x08);
    uint16_t dob = read_word_be(h.ram, r.a[7] + 0x10);
    mc68k_destroy(cpu);

    assert(r.pc == BUS2_HANDLER);
    assert((ssw & 0x0200) != 0); /* BY=1（バイト転送） */
    assert((ssw & 0x0100) == 0); /* RW=0（書き込み） */
    assert((ssw & 0x0007) == 5); /* FC=5 */
    assert(dob == 0x00AA);       /* Data Output Buffer にD0下位ワード */
}

/* S-9: MOVE.L D0,(A0)（未マップ領域への書き込み、バスエラー）。整列ロング書き込みは
 * cpu_write_long_access() が単一の原子的バスアクセスとして扱うため、S-2/S-3（ワード/バイト）
 * とは別に固有の検証が要る。DOBはfault_data_out（32bit値そのまま）の下位ワードではなく、
 * Fault Addressの位置に一致する上位ワードでなければならない（mc68000#39 gpt-6-Solレビュー、
 * [DS10 §5.3.9]）。 */
static void test_ssw_write_long_bus_error_dob_high_word(void) {
    hole_ctx_t h;
    memset(&h, 0, sizeof(h));
    write_long_be(h.ram, 0, RESET_SSP);
    write_long_be(h.ram, 4, RESET_PC);
    write_long_be(h.ram, 2u * 4u, BUS2_HANDLER);
    write_word_be(h.ram, BUS2_HANDLER, 0x60FE); /* BRA * */

    uint32_t p = RESET_PC;
    write_word_be(h.ram, p, 0x203C); /* MOVE.L #imm,D0 */
    p += 2;
    write_long_be(h.ram, p, 0x12345678u);
    p += 4;
    write_word_be(h.ram, p, 0x207C); /* MOVEA.L #imm,A0 */
    p += 2;
    write_long_be(h.ram, p, FAR_HOLE_ADDR); /* 4の倍数（整列） */
    p += 4;
    write_word_be(h.ram, p, 0x2080); /* MOVE.L D0,(A0) */

    mc68k_state_t *cpu = make_cpu_hole(&h, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint16_t ssw = read_word_be(h.ram, r.a[7] + 0x08);
    uint32_t fault_addr = read_long_be(h.ram, r.a[7] + 0x0A);
    uint16_t dob = read_word_be(h.ram, r.a[7] + 0x10);
    mc68k_destroy(cpu);

    assert(r.pc == BUS2_HANDLER);
    assert((ssw & 0x0200) == 0); /* BY=0（ロングはワード/バイトどちらでもない） */
    assert((ssw & 0x0100) == 0); /* RW=0（書き込み） */
    assert((ssw & 0x0007) == 5); /* FC=5 */
    assert(fault_addr == FAR_HOLE_ADDR);
    assert(dob == 0x1234); /* 上位ワード＝Fault Addressの位置に書かれるはずの値 */
}

/* S-4: ユーザモードでのデータフォルト → FC=1。 */
static void test_ssw_user_mode_fc(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);
    write_word_be(ram, ADDR3_HANDLER, 0x60FE);

    uint32_t fault_addr = 0x00003600u + 1u;
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x46FC); /* MOVE.W #0,SR（ユーザモードへ） */
    p += 2;
    write_word_be(ram, p, 0x0000);
    p += 2;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm,A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x4A50); /* TST.W (A0) */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint16_t ssw = read_word_be(ram, r.a[7] + 0x08);
    mc68k_destroy(cpu);

    assert((ssw & 0x0007) == 1); /* FC=1（ユーザデータ） */
}

/* S-5: 奇数PCへの分岐後のオペコードフェッチ → IF=1, RW=1, FC=6（スーパーバイザ
 * プログラム）。 */
static void test_ssw_opcode_fetch_address_error(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);
    write_word_be(ram, ADDR3_HANDLER, 0x60FE);

    uint32_t odd_target = 0x00003700u + 1u;
    write_word_be(ram, RESET_PC, 0x4EF9); /* JMP abs.L */
    write_long_be(ram, RESET_PC + 2, odd_target);

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint16_t ssw = read_word_be(ram, r.a[7] + 0x08);
    mc68k_destroy(cpu);

    assert((ssw & 0x2000) != 0); /* IF=1 */
    assert((ssw & 0x1000) == 0); /* DF=0 */
    assert((ssw & 0x0100) != 0); /* RW=1（読み出し） */
    assert((ssw & 0x0007) == 6); /* FC=6（スーパーバイザプログラム） */
}

/* S-6: 拡張ワードフェッチのバスエラー → IF=1（fault_access_kind==FAULT_EXTENSION
 * がIFに落ちること）。オペコード自体は正常フェッチさせ、直後の拡張ワードを
 * 未マップ領域に置く。 */
static void test_ssw_extension_fetch_bus_error(void) {
    hole_ctx_t h;
    memset(&h, 0, sizeof(h));
    write_long_be(h.ram, 0, RESET_SSP);
    write_long_be(h.ram, 4, RESET_PC);
    write_long_be(h.ram, 2u * 4u, BUS2_HANDLER);
    write_word_be(h.ram, BUS2_HANDLER, 0x60FE);

    uint32_t opcode_addr = HOLE_RAM_SIZE - 2u;
    write_word_be(h.ram, opcode_addr, 0x207C); /* MOVEA.L #imm,A0（immはopcode直後） */

    mc68k_state_t *cpu = make_cpu_hole(&h, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    cpu->pc = opcode_addr;
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint16_t ssw = read_word_be(h.ram, r.a[7] + 0x08);
    mc68k_destroy(cpu);

    assert(r.pc == BUS2_HANDLER);
    assert((ssw & 0x2000) != 0); /* IF=1（拡張ワードもInstruction Input Buffer扱い） */
    assert((ssw & 0x0100) != 0); /* RW=1（読み出し） */
}

/* S-7: TAS (A0)（A0未マップ）→ RM=1。 */
static void test_ssw_tas_sets_rm(void) {
    hole_ctx_t h;
    memset(&h, 0, sizeof(h));
    write_long_be(h.ram, 0, RESET_SSP);
    write_long_be(h.ram, 4, RESET_PC);
    write_long_be(h.ram, 2u * 4u, BUS2_HANDLER);
    write_word_be(h.ram, BUS2_HANDLER, 0x60FE);

    uint32_t p = RESET_PC;
    write_word_be(h.ram, p, 0x207C); /* MOVEA.L #imm,A0 */
    p += 2;
    write_long_be(h.ram, p, FAR_HOLE_ADDR);
    p += 4;
    write_word_be(h.ram, p, 0x4AD0); /* TAS (A0) */

    mc68k_state_t *cpu = make_cpu_hole(&h, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint16_t ssw = read_word_be(h.ram, r.a[7] + 0x08);
    mc68k_destroy(cpu);

    assert(r.pc == BUS2_HANDLER);
    assert((ssw & 0x0800) != 0); /* RM=1（リード・モディファイ・ライト） */
}

/* S-8: 直前に別サイズ・別FCの正常アクセスを行ってからアドレスエラーを
 * 起こしても、SSWが直前のアクセスの値（BY=1、FC=5）を引きずっていないこと
 * （A-2の回帰テスト。A-2を入れないと落ちる）。 */
static void test_ssw_does_not_leak_previous_access(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 3u * 4u, ADDR3_HANDLER);
    write_word_be(ram, ADDR3_HANDLER, 0x60FE);

    uint32_t scratch_even = 0x00003400u;
    uint32_t fault_addr = 0x00003500u + 1u;

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x227C); /* MOVEA.L #imm,A1 */
    p += 2;
    write_long_be(ram, p, scratch_even);
    p += 4;
    write_word_be(ram, p, 0x4A11); /* TST.B (A1)：直前のsupervisor byteアクセス */
    p += 2;
    write_word_be(ram, p, 0x46FC); /* MOVE.W #0,SR（ユーザモードへ） */
    p += 2;
    write_word_be(ram, p, 0x0000);
    p += 2;
    write_word_be(ram, p, 0x207C); /* MOVEA.L #imm,A0 */
    p += 2;
    write_long_be(ram, p, fault_addr);
    p += 4;
    write_word_be(ram, p, 0x4A50); /* TST.W (A0)：ここでアドレスエラー（word, user） */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint16_t ssw = read_word_be(ram, r.a[7] + 0x08);
    mc68k_destroy(cpu);

    assert((ssw & 0x0200) == 0); /* BY=0：直前のTST.Bの名残(BY=1)を引きずっていない */
    assert((ssw & 0x0007) == 1); /* FC=1：直前のFC=5を引きずっていない */
}

/* ---- C-1〜C-4: RTE Long のサイクル区分（A-4） ---- */

static void write_group0_frame(uint8_t *ram, uint32_t base, uint16_t sr, uint32_t pc,
                               uint16_t fault_vector, uint16_t ssw) {
    write_word_be(ram, base + 0x00, sr);
    write_long_be(ram, base + 0x02, pc);
    write_word_be(ram, base + 0x06, (uint16_t)(0x8000u | (fault_vector * 4u)));
    write_word_be(ram, base + 0x08, ssw);
    /* SP+0x0A以降（フォールトアドレス以下）はサイクル数の判定に無関係なので0のまま。 */
}

/* C-1: SSW.RR=1（ソフトウェア再実行済み）→ CYC_RTE_LONG_NORETRY(110)。 */
static void test_rte_long_cycles_no_retry(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x4E73); /* RTE */

    uint32_t base = RESET_SSP - 58;
    write_group0_frame(ram, base, 0x2700, RESET_PC + 2, 2, 0x8105 /* RR=1,RW=1,FC=5 */);

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    cpu->a[7] = base;
    int consumed = mc68k_execute(cpu, 1);
    mc68k_destroy(cpu);

    assert(consumed == 110);
}

/* C-2: SSW.RR=0,RW=1（読み出しフォルト） → CYC_RTE_LONG_RETRY_READ(112)。 */
static void test_rte_long_cycles_retry_read(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x4E73);

    uint32_t base = RESET_SSP - 58;
    write_group0_frame(ram, base, 0x2700, RESET_PC + 2, 2, 0x0105 /* RR=0,RW=1,FC=5 */);

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    cpu->a[7] = base;
    int consumed = mc68k_execute(cpu, 1);
    mc68k_destroy(cpu);

    assert(consumed == 112);
}

/* C-3: SSW.RR=0,RW=0（書き込みフォルト） → CYC_RTE_LONG_RETRY_WRITE(112)。 */
static void test_rte_long_cycles_retry_write(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x4E73);

    uint32_t base = RESET_SSP - 58;
    write_group0_frame(ram, base, 0x2700, RESET_PC + 2, 2, 0x0005 /* RR=0,RW=0,FC=5 */);

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    cpu->a[7] = base;
    int consumed = mc68k_execute(cpu, 1);
    mc68k_destroy(cpu);

    assert(consumed == 112);
}

/* C-4: フォーマット$0短フレームの回帰（既存CYC_RTE_SHORT=24が変わっていない
 * こと）。 */
static void test_rte_short_cycles_unchanged(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x4E73);

    uint32_t base = RESET_SSP - 8;
    write_word_be(ram, base + 0, 0x2700);
    write_long_be(ram, base + 2, RESET_PC + 2);
    write_word_be(ram, base + 6, 0x0000); /* フォーマット$0 */

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    cpu->a[7] = base;
    int consumed = mc68k_execute(cpu, 1);
    mc68k_destroy(cpu);

    assert(consumed == 24);
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

/* Phase 2a: [DS10 Table 7-4] No Fetch gives (An).L a CEA of two.
 * MOVES.L's 68010 base 20 therefore executes as 22. */
static void test_moves_long_adds_ea_calc_cycles(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC + 0, 0x0E90); /* MOVES.L (A0),D0 */
    write_word_be(ram, RESET_PC + 2, 0x0000); /* D0, memory -> register */
    write_long_be(ram, 0x00002000, 0x12345678);

    mc68k_state_t *cpu = make_cpu(ram, MC68K_MODEL_68010);
    mc68k_reset(cpu);
    cpu->a[0] = 0x00002000;
    int consumed = mc68k_execute(cpu, 1);
    assert(consumed == 22);
    mc68k_destroy(cpu);
}

int main(void) {
    test_move_from_ccr_68000_illegal();
    test_move_from_ccr_68010_ok();
    test_bkpt_68000_illegal();
    test_bkpt_68010_dispatches_with_breakpoint_cycles();
    test_move_from_sr_68000_user_ok();
    test_move_from_sr_68010_user_violation();
    test_movec_vbr_relocation_trap();
    test_movec_vbr_relocation_irq();
    test_movec_privilege_violation();
    test_movec_illegal_creg();
    test_movec_sfc_dfc_mask();
    test_rtd_positive_disp();
    test_rtd_negative_disp();
    test_moves_mem_to_dn_long();
    test_moves_dn_to_mem_byte();
    test_moves_uses_sfc_on_memory_read();
    test_moves_uses_dfc_on_memory_write();
    test_tas_marks_rmw_for_tt();
    test_moves_mem_to_an_sign_extend();
    test_moves_user_mode_privilege_violation();
    test_frame_format_word_full_bytes();
    test_trap_rte_roundtrip_68010();
    test_format_error();
    test_format_error_trace_not_suppressed();
    test_group0_long_frame_basic();
    test_group0_long_frame_rte_resumes();
    test_group0_refault_safety_valve_halts();
    test_group0_refault_guard_clears_after_success();
    test_restart_rollback_postinc_read();
    test_restart_rollback_predec_write();
    test_restart_rollback_movem_bus_error();
    test_restart_rollback_refault_matches_safety_valve();
    test_restart_rollback_not_applied_on_68000();
    test_ssw_read_word_address_error();
    test_ssw_write_word_address_error();
    test_ssw_write_byte_bus_error();
    test_ssw_write_long_bus_error_dob_high_word();
    test_ssw_user_mode_fc();
    test_ssw_opcode_fetch_address_error();
    test_ssw_extension_fetch_bus_error();
    test_ssw_tas_sets_rm();
    test_ssw_does_not_leak_previous_access();
    test_rte_long_cycles_no_retry();
    test_rte_long_cycles_retry_read();
    test_rte_long_cycles_retry_write();
    test_rte_short_cycles_unchanged();
    test_moves_long_adds_ea_calc_cycles();
    return 0;
}
