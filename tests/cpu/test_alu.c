/*
 * test_alu.c — ADD / SUB / CMP / ADDI / SUBI / CMPI / ADDQ / SUBQ テスト
 *
 * MC68000 の算術演算命令と CCR フラグ更新を検証する。
 * Z / N / V / C / X フラグの境界値ケースを重点的にカバーする。
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

/* ビッグエンディアンで RAM にワードを書く */
static void write_word_be(uint8_t *ram, uint32_t addr, uint16_t val) {
    ram[addr + 0] = (uint8_t)(val >> 8);
    ram[addr + 1] = (uint8_t)(val & 0xFF);
}

/* ビッグエンディアンで RAM にロングワードを書く */
static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t val) {
    ram[addr + 0] = (uint8_t)(val >> 24);
    ram[addr + 1] = (uint8_t)(val >> 16);
    ram[addr + 2] = (uint8_t)(val >> 8);
    ram[addr + 3] = (uint8_t)(val >> 0);
}

/*
 * テスト用 CPU を生成する。
 * ram は呼び出し元が確保・初期化する（テストごとに命令を書き込む）。
 */
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

static mc68k_state_t *make_cpu(uint8_t *ram) {
    return make_cpu_model(ram, MC68K_MODEL_68000);
}

/*
 * CPU をリセットし、指定サイクル数だけ実行してレジスタスナップショットを返す。
 */
static mc68k_regs_t run(mc68k_state_t *cpu, int cycles) {
    mc68k_reset(cpu);
    mc68k_execute(cpu, cycles);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    return r;
}

/* CCR 下位 5bit（X N Z V C）だけ取り出す */
static uint16_t ccr(const mc68k_regs_t *r) {
    return r->sr & 0x1F;
}

/* ================================================================== */
/* ADD.W EA, Dn テスト（ライン D、op-mode 001）                        */
/* ================================================================== */

/*
 * MOVE.W #0x0001, D0  →  D0 = 0x0001
 * MOVE.W #0x0002, D1  →  D1 = 0x0002
 * ADD.W  D1, D0       →  D0 = 0x0003、CCR: Z=0, N=0, V=0, C=0, X=0
 *
 * ADD.W Dn, Dn のオペコード: 0xD040 | (src<<0)
 *   0xD041 = ADD.W D1, D0
 */
static void test_add_word_dn_dn(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVE.W #1, D0 = 0x303C 0x0001 */
    write_word_be(ram, RESET_PC + 0, 0x303C);
    write_word_be(ram, RESET_PC + 2, 0x0001);
    /* MOVE.W #2, D1 = 0x323C 0x0002 */
    write_word_be(ram, RESET_PC + 4, 0x323C);
    write_word_be(ram, RESET_PC + 6, 0x0002);
    /* ADD.W D1, D0 = 0xD041 */
    write_word_be(ram, RESET_PC + 8, 0xD041);
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x0003);
    assert(ccr(&r) == 0x00); /* 全フラグ 0 */
}

/* ================================================================== */
/* ADD.W — ゼロフラグ                                                   */
/* ================================================================== */

/*
 * 0x7FFF + 0x8001 = 0x10000 → ワードで 0x0000
 * C=1（桁あふれ）、Z=1（結果ゼロ）、V=0、N=0
 *
 * X も C と同時に 1 になる。
 */
static void test_add_carry_zero(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0x7FFF, D0 */
    write_word_be(ram, RESET_PC + 2, 0x7FFF);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #0x8001, D1 */
    write_word_be(ram, RESET_PC + 6, 0x8001);
    write_word_be(ram, RESET_PC + 8, 0xD041);  /* ADD.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0000);
    /* C=1, X=1, Z=1, N=0, V=0 → CCR bits: X=0x10, Z=0x04, C=0x01 = 0x15 */
    assert(ccr(&r) == 0x15);
}

/* ================================================================== */
/* ADD.W — オーバーフローフラグ（正 + 正 → 負）                         */
/* ================================================================== */

/*
 * 0x4000 + 0x4000 = 0x8000 → 符号付きでは +16384 + 16384 = -32768（溢れ）
 * V=1, N=1（MSB が 1）
 */
static void test_add_overflow(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0x4000, D0 */
    write_word_be(ram, RESET_PC + 2, 0x4000);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #0x4000, D1 */
    write_word_be(ram, RESET_PC + 6, 0x4000);
    write_word_be(ram, RESET_PC + 8, 0xD041);  /* ADD.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x8000);
    /* V=1, N=1 → CCR bits: N=0x08, V=0x02 = 0x0A */
    assert(ccr(&r) == 0x0A);
}

/* ================================================================== */
/* SUB.W EA, Dn — 基本減算                                              */
/* ================================================================== */

/*
 * MOVE.W #5, D0
 * MOVE.W #3, D1
 * SUB.W D1, D0   →  D0 = 2、CCR: Z=0, N=0, V=0, C=0
 *
 * SUB.W Dn, Dn のオペコード: 0x9040 | (src<<0)
 *   0x9041 = SUB.W D1, D0
 */
static void test_sub_basic(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #5, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0005);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #3, D1 */
    write_word_be(ram, RESET_PC + 6, 0x0003);
    write_word_be(ram, RESET_PC + 8, 0x9041);  /* SUB.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0002);
    assert(ccr(&r) == 0x00);
}

/* ================================================================== */
/* SUB.W — ボローフラグ（小さい値から大きい値を引く）                    */
/* ================================================================== */

/*
 * 1 - 2 = -1 → ワードで 0xFFFF
 * C=1（ボロー）、N=1、X=1
 */
static void test_sub_borrow(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #1, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0001);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #2, D1 */
    write_word_be(ram, RESET_PC + 6, 0x0002);
    write_word_be(ram, RESET_PC + 8, 0x9041);  /* SUB.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0xFFFF);
    /* C=1, X=1, N=1 → 0x19 */
    assert(ccr(&r) == 0x19);
}

/* ================================================================== */
/* SUB.W — オーバーフロー（負 - 正 → 正）                               */
/* ================================================================== */

/*
 * -32768（0x8000）- 1（0x0001）= -32769 → ワードで 0x7FFF（オーバーフロー）
 * V=1, C=1（ボロー）
 */
static void test_sub_overflow(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0x8000, D0 */
    write_word_be(ram, RESET_PC + 2, 0x8000);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #1, D1 */
    write_word_be(ram, RESET_PC + 6, 0x0001);
    write_word_be(ram, RESET_PC + 8, 0x9041);  /* SUB.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x7FFF);
    /* V=1 のみ。1 < 0x8000（符号なし）なのでボローは発生しない。
     * 符号付きでは -32768 - 1 = -32769 → 16bit に収まらない → V=1 */
    assert(ccr(&r) == 0x02);
}

/* ================================================================== */
/* CMP.W — 等値比較（Z フラグ）                                          */
/* ================================================================== */

/*
 * CMP は結果を格納しない。D0 の値は変化しない。
 * 5 - 5 = 0 → Z=1
 *
 * CMP.W Dn, Dn: 0xB041 = CMP.W D1, D0（D0 - D1 を計算、D0 は不変）
 */
static void test_cmp_equal(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #5, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0005);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #5, D1 */
    write_word_be(ram, RESET_PC + 6, 0x0005);
    write_word_be(ram, RESET_PC + 8, 0xB041);  /* CMP.W D1, D0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0005); /* D0 は変化しない */
    assert(ccr(&r) == 0x04);             /* Z=1 のみ */
}

/* ================================================================== */
/* CMP.W — X フラグ不変確認                                              */
/* ================================================================== */

/*
 * CMP は X フラグを更新しない。
 * 先に ADD で X=1 にしてから CMP しても X は 1 のまま。
 */
static void test_cmp_x_unchanged(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* ADD で X=1 にする（0x8000 + 0x8000 → C=X=1） */
    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0x8000, D0 */
    write_word_be(ram, RESET_PC + 2, 0x8000);
    write_word_be(ram, RESET_PC + 4, 0x323C); /* MOVE.W #0x8000, D1 */
    write_word_be(ram, RESET_PC + 6, 0x8000);
    write_word_be(ram, RESET_PC + 8, 0xD041); /* ADD.W D1, D0 → C=X=1 */
    /* CMP で Z=1 になっても X は 1 のまま */
    write_word_be(ram, RESET_PC + 10, 0x303C); /* MOVE.W #3, D0 */
    write_word_be(ram, RESET_PC + 12, 0x0003);
    write_word_be(ram, RESET_PC + 14, 0x323C); /* MOVE.W #3, D1 */
    write_word_be(ram, RESET_PC + 16, 0x0003);
    write_word_be(ram, RESET_PC + 18, 0xB041); /* CMP.W D1, D0 → Z=1, X 不変 */
    write_word_be(ram, RESET_PC + 20, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    /* X=1（ADD で立てた）、Z=1（CMP で立てた）、N/V/C=0 → 0x14 */
    assert(ccr(&r) == 0x14);
}

/* ================================================================== */
/* ADDI.W — イミディエイト加算                                           */
/* ================================================================== */

/*
 * ADDI.W #0x0010, D0
 *   opcode: 0x0640、続けて即値ワード 0x0010
 *
 * D0 初期値 0x0005 → 結果 0x0015
 */
static void test_addi_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #5, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0005);
    write_word_be(ram, RESET_PC + 4, 0x0640); /* ADDI.W #0x0010, D0 */
    write_word_be(ram, RESET_PC + 6, 0x0010);
    write_word_be(ram, RESET_PC + 8, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0015);
    assert(ccr(&r) == 0x00);
}

/* ================================================================== */
/* SUBI.W — イミディエイト減算                                           */
/* ================================================================== */

/*
 * SUBI.W #0x0001, D0（D0 = 1）→ D0 = 0, Z=1
 *   opcode: 0x0440、続けて即値 0x0001
 */
static void test_subi_zero(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #1, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0001);
    write_word_be(ram, RESET_PC + 4, 0x0440); /* SUBI.W #1, D0 */
    write_word_be(ram, RESET_PC + 6, 0x0001);
    write_word_be(ram, RESET_PC + 8, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0000);
    assert(ccr(&r) == 0x04); /* Z=1 */
}

/* ================================================================== */
/* CMPI.W — イミディエイト比較                                           */
/* ================================================================== */

/*
 * CMPI.W #0x000A, D0（D0 = 5）→ 5 - 10 → ボロー C=1, N=1
 *   opcode: 0x0C40、続けて即値 0x000A
 */
static void test_cmpi_less(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #5, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0005);
    write_word_be(ram, RESET_PC + 4, 0x0C40); /* CMPI.W #10, D0 */
    write_word_be(ram, RESET_PC + 6, 0x000A);
    write_word_be(ram, RESET_PC + 8, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0005); /* D0 変化なし */
    /* CMP/CMPI は X を変化させない。リセット直後 X=0 のため X は 0 のまま。
     * C=1（ボロー: 10 > 5）、N=1 → 0x09 */
    assert(ccr(&r) == 0x09);
}

/* ================================================================== */
/* ADDQ.W #4, Dn                                                        */
/* ================================================================== */

/*
 * ADDQ.W #4, D0（D0 = 10）→ D0 = 14
 *   ADDQ.W: 0101 | data | 0 | 01 | 000 | 000 = 0x5840（data=4、size=01=word、Dn=D0）
 *   bits 11-9 = 100 (data=4), bit 8 = 0 (ADDQ), bits 7-6 = 01 (word), bits 5-0 = 000000 (D0)
 *   = 0101 100 0 01 000000 = 0x5840
 */
static void test_addq_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #10, D0 */
    write_word_be(ram, RESET_PC + 2, 0x000A);
    write_word_be(ram, RESET_PC + 4, 0x5840); /* ADDQ.W #4, D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x000E);
    assert(ccr(&r) == 0x00);
}

/* ================================================================== */
/* SUBQ.W #1, Dn — ループカウンタの典型パターン                         */
/* ================================================================== */

/*
 * SUBQ.W #1, D0（D0 = 1）→ D0 = 0, Z=1
 *   SUBQ.W #1: 0101 001 1 01 000000 = 0x5340
 *   bits 11-9 = 001 (data=1), bit 8 = 1 (SUBQ), bits 7-6 = 01 (word)
 *
 * X68000 のループ本体末尾でよく見かけるパターン。
 */
static void test_subq_to_zero(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #1, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0001);
    write_word_be(ram, RESET_PC + 4, 0x5340); /* SUBQ.W #1, D0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0000);
    assert(ccr(&r) == 0x04); /* Z=1 */
}

/* ================================================================== */
/* ADDQ.W #8, An — ADDA 相当（CCR 更新なし）                           */
/* ================================================================== */

/*
 * ADDQ の宛先が An のときは CCR を更新しない。
 * ADDQ.W #8, A0: bits 11-9 = 000 (data=8), bit 8 = 0 (ADDQ), bits 7-6 = 01 (word)
 *   bits 5-3 = 001 (An), bits 2-0 = 000 (A0)
 *   = 0101 000 0 01 001 000 = 0x5048
 */
static void test_addq_an_no_ccr(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* まず ADD で Z=1 にしておく */
    write_word_be(ram, RESET_PC + 0, 0x303C); /* MOVE.W #0, D0 */
    write_word_be(ram, RESET_PC + 2, 0x0000);
    write_word_be(ram, RESET_PC + 4, 0x0C40); /* CMPI.W #0, D0 → Z=1 */
    write_word_be(ram, RESET_PC + 6, 0x0000);
    /* ADDQ.W #8, A0 → CCR は変化しないはず */
    write_word_be(ram, RESET_PC + 8, 0x5048);  /* ADDQ.W #8, A0 */
    write_word_be(ram, RESET_PC + 10, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    /* リセット後 A7=SSP だが A0 はゼロ初期化。ADDQ.W #8, A0 → A0 = 0+8 = 8 */
    assert(r.a[0] == 8);
    /* Z=1 は ADDQ.W An 後も維持されるはず */
    assert(ccr(&r) == 0x04);
}

/* ================================================================== */
/* ADD.L — 32bit 加算のキャリー                                          */
/* ================================================================== */

/*
 * 0x80000000 + 0x80000000 = 0x100000000 → ロングで 0x00000000
 * C=1, X=1, Z=1
 *
 * ADD.L D1, D0 = 0xD081
 */
static void test_add_long_carry(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVE.L #0x80000000, D0 = MOVEQ では範囲外なので MOVE.L #imm */
    /* MOVE.L #imm, Dn: 0x203C + longword */
    write_word_be(ram, RESET_PC + 0, 0x203C);
    write_long_be(ram, RESET_PC + 2, 0x80000000);
    write_word_be(ram, RESET_PC + 6, 0x223C);
    write_long_be(ram, RESET_PC + 8, 0x80000000);
    write_word_be(ram, RESET_PC + 12, 0xD081); /* ADD.L D1, D0 */
    write_word_be(ram, RESET_PC + 14, 0x60FE); /* BRA * — ゼロメモリへの暴走を防ぐ */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t r = run(cpu, 200);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x00000000);
    /* C=1, X=1, Z=1, V=1: -2147483648 + -2147483648 = 0（ゼロへのラップ）で
     * 両入力が負・結果がゼロ（正）→ 符号付きオーバーフロー発生 → 0x17 */
    assert(ccr(&r) == 0x17);
}

/* 040UM Table 10-6 (PDF 304, printed 10-13): a 040 must not fall back
 * to the 68000 literal timing for EA,Dn arithmetic.  A budget of one makes
 * mc68k_execute run exactly this single instruction and return its cost. */
/* #90 の実行時網羅検査で見つかった未配線を 040UM の表値で固定する。 */
static int run_040_one(uint8_t *ram, void (*setup)(mc68k_state_t *), uint32_t *d0_out) {
    mc68k_state_t *cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    setup(cpu);
    int cycles = mc68k_execute(cpu, 1);
    if (d0_out)
        *d0_out = cpu->d[0];
    mc68k_destroy(cpu);
    return cycles;
}
static void setup_d0_1(mc68k_state_t *cpu) {
    cpu->d[0] = 1;
}
static void setup_d0_5(mc68k_state_t *cpu) {
    cpu->d[0] = 5;
}

static void test_040_runtime_coverage_gaps(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t d0;

    /* OR.B D0,(A0): Table 10-6 ADD/AND/EOR/OR column, (An): 1 + 1 + fetch 1 = 3. */
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x8110);
    ram[0x0200] = 0x02;
    assert(run_040_one(ram, setup_d0_1, &d0) == 3);
    assert(ram[0x0200] == 0x03);

    /* AND.B D0,(A0): same column. */
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0xC110);
    ram[0x0200] = 0x03;
    assert(run_040_one(ram, setup_d0_1, &d0) == 3);
    assert(ram[0x0200] == 0x01);

    /* MOVEC D0,SFC: Table 10-5 Rn,Rc = calculate 7 + execute base 6 = 13. */
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x4E7B);
    write_word_be(ram, RESET_PC + 2, 0x0000);
    assert(run_040_one(ram, setup_d0_5, &d0) == 13);

    /* CHK.W (BR,D1.W),D0 in bounds: calculate 12 + execute 11 + fetch 1 = 24. */
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x41B0);
    write_word_be(ram, RESET_PC + 2, 0x1110); /* full, BS=0, IS=0, null bd, D1.W */
    write_word_be(ram, 0x0200, 0x0002);
    assert(run_040_one(ram, setup_d0_1, &d0) == 24);

    /* CHK.W ([bd,A0],IS=1),D0 pre-null od: calculate 16 + execute 15 + fetch 2 = 33. */
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_word_be(ram, RESET_PC, 0x41B0);
    write_word_be(ram, RESET_PC + 2, 0x0171);
    write_long_be(ram, RESET_PC + 4, 0x00000010);
    write_long_be(ram, 0x0210, 0x00000300);
    write_word_be(ram, 0x0300, 0x0002);
    assert(run_040_one(ram, setup_d0_1, &d0) == 33);
}

static void test_add_sub_68040_table10_6(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* ADD.W (A0),D0: calculate 1 + execute 1 + fetch 1 = 3. */
    write_word_be(ram, RESET_PC, 0xD050);
    write_word_be(ram, 0x0200, 0x0002);
    mc68k_state_t *cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 1;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(cpu->d[0] == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* ADD.W ([bd,A0],IS),D0: §10.1 equates IS=1 postindexed memory
     * indirect with the matching preindexed row: 21 cycles. */
    write_word_be(ram, RESET_PC, 0xD070);
    write_word_be(ram, RESET_PC + 2, 0x0171); /* full, IS=1, long bd, preindexed */
    write_long_be(ram, RESET_PC + 4, 0x00000010);
    write_long_be(ram, 0x0210, 0x00000300);
    write_word_be(ram, 0x0300, 0x0002);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 3;
    assert(mc68k_execute(cpu, 1) == 21);
    assert(cpu->d[0] == 5);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* ADDQ.W #1,(A0): calculate 1 + execute 1 + fetch 1 = 3. */
    write_word_be(ram, RESET_PC, 0x5250);
    write_word_be(ram, 0x0200, 0x0002);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(ram[0x0201] == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* ADD.W D0,(A0): standard Table 10-6 EA column is also 3 cycles. */
    write_word_be(ram, RESET_PC, 0xD150);
    write_word_be(ram, 0x0200, 2);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 1;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(ram[0x0201] == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* CMP.W (A0),D0: 1 calculate + 1 execute + one read = 3. */
    write_word_be(ram, RESET_PC, 0xB050);
    write_word_be(ram, 0x0200, 2);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 3;
    assert(mc68k_execute(cpu, 1) == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* EOR.W D0,(A0): standard Table 10-6 EA column is 3 cycles. */
    write_word_be(ram, RESET_PC, 0xB150);
    write_word_be(ram, 0x0200, 3);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 1;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(ram[0x0201] == 2);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* CMPI.W #2,(A0): Table 10-6 CMPI: 1 + 1 + one read = 3. */
    write_word_be(ram, RESET_PC, 0x0C50);
    write_word_be(ram, RESET_PC + 2, 2);
    write_word_be(ram, 0x0200, 3);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* CMPA.L (A0),A1 uses the CMPI/CMPA.L column: 1 + 1 + one read = 3. */
    write_word_be(ram, RESET_PC, 0xB3D0);
    write_long_be(ram, 0x0200, 3);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->a[1] = 4;
    assert(mc68k_execute(cpu, 1) == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* CLR.W (A0): Table 10-6 gives calculate 1 + execute 1 = 2. */
    write_word_be(ram, RESET_PC, 0x4250);
    write_word_be(ram, 0x0200, 7);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 2);
    assert(ram[0x0201] == 0);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* NEG.W (A0): 1 calculate + 1 execute + one old-value fetch = 3. */
    write_word_be(ram, RESET_PC, 0x4450);
    write_word_be(ram, 0x0200, 1);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(ram[0x0201] == 0xFF);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* MULU.W (A0),D0: 1 calculate + 14 execute + one fetch = 16. */
    write_word_be(ram, RESET_PC, 0xC0D0);
    write_word_be(ram, 0x0200, 7);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 3;
    assert(mc68k_execute(cpu, 1) == 16);
    assert(cpu->d[0] == 21);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* DIVU.W (A0),D0: calculate 8 + execute 27 + one fetch = 36. */
    write_word_be(ram, RESET_PC, 0x80D0);
    write_word_be(ram, 0x0200, 3);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 21;
    assert(mc68k_execute(cpu, 1) == 36);
    assert(cpu->d[0] == 7);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* NBCD (A0): 1 calculate + 2 execute + one old-value fetch = 4. */
    write_word_be(ram, RESET_PC, 0x4810);
    ram[0x0200] = 0x07;
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 4);
    assert(ram[0x0200] == 0x93);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* LEA (A0),A1: 1 calculate + 1 execute = 2. */
    write_word_be(ram, RESET_PC, 0x43D0);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 2);
    assert(cpu->a[1] == 0x0200);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* PEA (A0): 2 calculate + execute base 1 = 3. */
    write_word_be(ram, RESET_PC, 0x4850);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(cpu->a[7] == RESET_SSP - 4);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* MOVE.W (A0),CCR: 1 calculate + 2 execute + one fetch = 4. */
    write_word_be(ram, RESET_PC, 0x44D0);
    write_word_be(ram, 0x0200, 0x001F);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 4);
    assert((cpu->sr & 0x1F) == 0x1F);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* MOVE.W CCR,(A0): 1 calculate + 2 execute = 3. */
    write_word_be(ram, RESET_PC, 0x42D0);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(ram[0x0200] == 0);
    assert(ram[0x0201] == 0);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* CLR.W (bd,A0,D1.W): 7 calculate + execute base 6 = 13. */
    write_word_be(ram, RESET_PC, 0x4270);
    write_word_be(ram, RESET_PC + 2, 0x1120); /* full, D1.W, word bd, direct */
    write_word_be(ram, RESET_PC + 4, 0x0000);
    write_word_be(ram, 0x0200, 0x0007);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[1] = 0;
    assert(mc68k_execute(cpu, 1) == 13);
    assert(ram[0x0201] == 0);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* ANDI.W #3,(A0) uses the same immediate-RMW 040 row: 3 cycles. */
    write_word_be(ram, RESET_PC, 0x0250);
    write_word_be(ram, RESET_PC + 2, 3);
    write_word_be(ram, 0x0200, 7);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(ram[0x0201] == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* SUBQ.W #1,(d8,A0,D1.W): calculate 3 + execute 3 + fetch 1 = 7. */
    write_word_be(ram, RESET_PC, 0x5370);
    write_word_be(ram, RESET_PC + 2, 0x1000);
    write_word_be(ram, 0x0200, 0x0002);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[1] = 0;
    assert(mc68k_execute(cpu, 1) == 7);
    assert(ram[0x0201] == 1);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* CHK.W (A0),D0 in bounds: calculate 9 + execute base 7 + fetch 1 = 17. */
    write_word_be(ram, RESET_PC, 0x4190);
    write_word_be(ram, 0x0200, 0x0007);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 3;
    assert(mc68k_execute(cpu, 1) == 17);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* ADDI.W #1,(A0): calculate 1 + execute 1 + destination fetch 1 = 3. */
    write_word_be(ram, RESET_PC, 0x0650);
    write_word_be(ram, RESET_PC + 2, 1);
    write_word_be(ram, 0x0200, 2);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(ram[0x0201] == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* SUB.W (d8,A0,D1.W),D0: calculate 3 + execute 3 + fetch 1 = 7. */
    write_word_be(ram, RESET_PC, 0x9070);
    write_word_be(ram, RESET_PC + 2, 0x1000); /* D1.W, displacement 0 */
    write_word_be(ram, 0x0200, 0x0002);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 5;
    cpu->d[1] = 0;
    assert(mc68k_execute(cpu, 1) == 7);
    assert(cpu->d[0] == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* ADDA.W (A0),A1: calculate 1 + execute 2 + fetch 1 = 4. */
    write_word_be(ram, RESET_PC, 0xD2D0);
    write_word_be(ram, 0x0200, 0x0002);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->a[1] = 5;
    assert(mc68k_execute(cpu, 1) == 4);
    assert(cpu->a[1] == 7);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* SUBA.W (d8,A0,D1.W),A1: calculate 4 + execute 5 + fetch 1 = 10. */
    write_word_be(ram, RESET_PC, 0x92F0);
    write_word_be(ram, RESET_PC + 2, 0x1000); /* D1.W, displacement 0 */
    write_word_be(ram, 0x0200, 0x0002);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->a[1] = 5;
    cpu->d[1] = 0;
    assert(mc68k_execute(cpu, 1) == 10);
    assert(cpu->a[1] == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* AND.W (A0),D0: calculate 1 + execute 1 + fetch 1 = 3. */
    write_word_be(ram, RESET_PC, 0xC050);
    write_word_be(ram, 0x0200, 0x0003);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[0] = 7;
    assert(mc68k_execute(cpu, 1) == 3);
    assert(cpu->d[0] == 3);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* TST.W (d8,A0,D1.W): calculate 3 + execute 3 + fetch 1 = 7. */
    write_word_be(ram, RESET_PC, 0x4A70);
    write_word_be(ram, RESET_PC + 2, 0x1000); /* D1.W, displacement 0 */
    write_word_be(ram, 0x0200, 0x0003);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x0200;
    cpu->d[1] = 0;
    assert(mc68k_execute(cpu, 1) == 7);
    mc68k_destroy(cpu);
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_add_word_dn_dn();
    test_add_carry_zero();
    test_add_overflow();
    test_sub_basic();
    test_sub_borrow();
    test_sub_overflow();
    test_cmp_equal();
    test_cmp_x_unchanged();
    test_addi_word();
    test_subi_zero();
    test_cmpi_less();
    test_addq_word();
    test_subq_to_zero();
    test_addq_an_no_ccr();
    test_add_long_carry();
    test_add_sub_68040_table10_6();
    test_040_runtime_coverage_gaps();
    return 0;
}
