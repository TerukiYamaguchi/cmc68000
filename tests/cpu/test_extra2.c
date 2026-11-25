/*
 * test_extra2.c — 追加命令テスト (第2弾)
 *
 * MOVEP, TAS, CHK, ABCD/SBCD/NBCD を検証する。
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

#define RAM_SIZE  0x10000
#define RESET_SSP 0x00008000u
#define RESET_PC  0x00001000u

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
    /* テスト終端の ILLEGAL(0x4AFC)/未割当 opcode は実機通りベクタ4 経由で例外
     * ディスパッチされる。個別設定が無ければ、CCR を壊さず停止する自己ループ
     * (BRA *) へ向けて、テストが意図通り停止できるようにする。 */
    if (ram[0x10] == 0 && ram[0x11] == 0 && ram[0x12] == 0 && ram[0x13] == 0) {
        ram[0x12] = 0x04; /* ベクタ4 → 0x00000400 */
        ram[0x400] = 0x60;
        ram[0x401] = 0xFE; /* 0x400: BRA *（停止） */
    }
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = ram;
    r->base = 0x000000;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);
    mc68k_bus_t bus = bus_make_hooks(mem);
    return mc68k_create(&bus);
}

/* MOVE from CCR（0x42C0）は MC68010 以降の命令。
 * test_tas() はこれを「TAS 実行後の CCR を読み出す」ユーティリティとして
 * 使っているだけ（MOVE from CCR 自体の検証が目的ではない）ため、68000 モデルでは
 * ILLEGAL になってしまう。68010 モデルの CPU を使うことでテストの本来の意図
 * （TAS の Z/N フラグ検証）を保ったまま観測手段を維持する。 */
static mc68k_state_t *make_cpu_68010(uint8_t *ram) {
    if (ram[0x10] == 0 && ram[0x11] == 0 && ram[0x12] == 0 && ram[0x13] == 0) {
        ram[0x12] = 0x04;
        ram[0x400] = 0x60;
        ram[0x401] = 0xFE;
    }
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = ram;
    r->base = 0x000000;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);
    mc68k_bus_t bus = bus_make_hooks(mem);
    return mc68k_create_model(&bus, MC68K_MODEL_68010);
}

/* ================================================================== */
/* MOVEP                                                                */
/* ================================================================== */

/*
 * MOVEP.W Dn, (d16,An):  D0=0x1234 を A0+0x10 の偶数バイトに書く
 * 結果: ram[0x10]=0x12, ram[0x12]=0x34
 *
 * MOVEP.W (d16,An), D1:  同じ場所から読んで D1 下位ワードに格納
 * 結果: D1 = 0x00001234
 */
static void test_movep_word(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    /* ベクタテーブル */
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /*
     * MOVEQ #0x34, D0       ; D0 = 0x34 (8bit sign-extend)
     * ORI.L #0x1200, D0     ; D0 = 0x00001234  ← 上位バイトを足す
     *   実際は MOVEQ #0, D0; ADD.W #0x1234, D0 の方が短いが
     *   ここでは MOVEQ + MOVEP の確認が目的なので直書き
     *
     * 最短手順: MOVEQ #0x12, D0; LSL.L #8, D0; ADDI.W #0x34, D0
     *
     * 簡単のため: MOVEQ #0, D0; MOVE.W #0x1234, D0
     */

    /* MOVEQ #0, D0 : 0x7000 */
    write_word_be(ram, p, 0x7000);
    p += 2;

    /* MOVE.W #0x1234, D0 : 0x303C 0x1234 */
    write_word_be(ram, p, 0x303C);
    p += 2;
    write_word_be(ram, p, 0x1234);
    p += 2;

    /* MOVEP.W D0, (0x0010,A0) : 0x01C8 d16=0x0010
     * opcode: 0x0000 | (D0=0)<<9 | opmode=6<<6 | 0x08 | A0=0
     *       = 0x01C8, then d16=0x0010
     * bit breakdown: 0b 0000 0001 1100 1000 = 0x01C8
     *   bits 11-9 = 000 (D0)
     *   bits 8-6  = 111... wait: opmode=6 (110), so bits 8-6=110
     *   0x0000 | (0<<9) | (6<<6) | 0x08 | 0
     *   = 0x0000 | 0x0000 | 0x0180 | 0x0008 | 0
     *   = 0x0188
     */
    write_word_be(ram, p, 0x0188);
    p += 2; /* MOVEP.W D0, (d16,A0) */
    write_word_be(ram, p, 0x0010);
    p += 2; /* d16=0x0010 */

    /* MOVEP.W (0x0010,A0), D1 : opmode=4
     * 0x0000 | (D1=1)<<9 | (4<<6) | 0x08 | A0=0
     * = 0x0000 | 0x0200 | 0x0100 | 0x08 | 0
     * = 0x0308
     */
    write_word_be(ram, p, 0x0308);
    p += 2; /* MOVEP.W (d16,A0), D1 */
    write_word_be(ram, p, 0x0010);
    p += 2; /* d16=0x0010 */

    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    /* 書き込み確認: 交互バイト配置 */
    assert(ram[0x10] == 0x12);
    assert(ram[0x12] == 0x34);
    /* ram[0x11]/[0x13] は変更なし */
    assert(ram[0x11] == 0x00);
    assert(ram[0x13] == 0x00);

    /* 読み込み確認: D1 下位ワード */
    assert((r.d[1] & 0xFFFF) == 0x1234);

    mc68k_destroy(cpu);
}

/*
 * MOVEP.L テスト:
 * D2=0x12345678 を A1+0x00 の偶数バイトに書く (opmode=7)
 * → ram[0]=0x12, ram[2]=0x34, ram[4]=0x56, ram[6]=0x78
 * MOVEP.L (0,A1), D3 で読み戻して D3=0x12345678 を確認 (opmode=5)
 */
static void test_movep_long(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVE.L #0x12345678, D2 : 0x243C 0x1234 0x5678 */
    write_word_be(ram, p, 0x243C);
    p += 2;
    write_word_be(ram, p, 0x1234);
    p += 2;
    write_word_be(ram, p, 0x5678);
    p += 2;

    /* MOVEA.L #0x2000, A1 : A1 = 0x2000 (ターゲット RAM 領域) */
    write_word_be(ram, p, 0x227C);
    p += 2;
    write_word_be(ram, p, 0x0000);
    p += 2;
    write_word_be(ram, p, 0x2000);
    p += 2;

    /* MOVEP.L D2, (0x0000,A1) : opmode=7
     * 0x0000 | (D2=2)<<9 | (7<<6) | 0x08 | A1=1
     * = 0x0000 | 0x0400 | 0x01C0 | 0x08 | 0x01
     * = 0x05C9
     */
    write_word_be(ram, p, 0x05C9);
    p += 2;
    write_word_be(ram, p, 0x0000);
    p += 2; /* d16=0 */

    /* MOVEP.L (0x0000,A1), D3 : opmode=5
     * 0x0000 | (D3=3)<<9 | (5<<6) | 0x08 | A1=1
     * = 0x0000 | 0x0600 | 0x0140 | 0x08 | 0x01
     * = 0x0749
     */
    write_word_be(ram, p, 0x0749);
    p += 2;
    write_word_be(ram, p, 0x0000);
    p += 2; /* d16=0 */

    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 300);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    /* 書き込み確認 */
    assert(ram[0x2000] == 0x12);
    assert(ram[0x2002] == 0x34);
    assert(ram[0x2004] == 0x56);
    assert(ram[0x2006] == 0x78);

    /* 読み込み確認 */
    assert(r.d[3] == 0x12345678u);

    mc68k_destroy(cpu);
}

/* ================================================================== */
/* TAS (Test and Set)                                                   */
/* ================================================================== */

/*
 * D0=0x00: TAS D0 → Z=1 (byte=0), N=0, bit7 セット → D0=0x80
 * D1=0x80: TAS D1 → Z=0 (byte≠0), N=1, bit7 は既にセット → D1=0x80
 */
static void test_tas(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEQ #0, D0 */
    write_word_be(ram, p, 0x7000);
    p += 2;

    /* MOVEQ #-128, D1 → D1=0xFFFFFF80 (符号拡張) */
    write_word_be(ram, p, 0x7280);
    p += 2;

    /* TAS D0 : 0x4AC0 | (mode=0, reg=0) = 0x4AC0 */
    write_word_be(ram, p, 0x4AC0);
    p += 2;

    /* MOVE.W CCR, D4 : 0x42C0 | (D4=4) = 0x42C4 */
    write_word_be(ram, p, 0x42C4);
    p += 2;

    /* TAS D1 : 0x4AC0 | (mode=0, reg=1) = 0x4AC1 */
    write_word_be(ram, p, 0x4AC1);
    p += 2;

    /* MOVE.W CCR, D5 : 0x42C5 */
    write_word_be(ram, p, 0x42C5);
    p += 2;

    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu_68010(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    /* TAS D0 (byte=0): Z=1, N=0, D0.B=0x80 */
    assert((r.d[0] & 0xFF) == 0x80);
    assert(r.d[4] & (1 << 2));    /* Z=1 */
    assert(!(r.d[4] & (1 << 3))); /* N=0 */

    /* TAS D1 (byte=0x80): Z=0, N=1, D1.B=0x80 */
    assert((r.d[1] & 0xFF) == 0x80);
    assert(!(r.d[5] & (1 << 2))); /* Z=0 */
    assert(r.d[5] & (1 << 3));    /* N=1 */

    mc68k_destroy(cpu);
}

/* ================================================================== */
/* CHK                                                                  */
/* ================================================================== */

/*
 * CHK.W #10, D0 で D0 が範囲内(0≤D0≤10)なら例外なし。
 * 範囲外(D0<0 または D0>10)なら CHK 例外(ベクタ6)へ。
 *
 * ベクタ6=0x18 → アドレス 0x0018 に例外ルーチン先頭を書く。
 * ルーチンは D7=0xDEAD をセットして ILLEGAL で止まる。
 *
 * テスト1: D0=5 (範囲内) → D7=0 (例外なし)
 * テスト2: D0=0xFFFF = -1 (範囲外: 負) → D7=0xDEAD (例外発生)
 */
static void test_chk(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    /* CHK 例外ルーチン先頭アドレス */
    uint32_t chk_handler = 0x00002000;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    /* ベクタ 6 = 0x0018 */
    write_long_be(ram, 0x18, chk_handler);

    /* 例外ルーチン: MOVE.W #0xDEAD, D7; ILLEGAL */
    write_word_be(ram, chk_handler, 0x3E3C);
    write_word_be(ram, chk_handler + 2, 0xDEAD);
    write_word_be(ram, chk_handler + 4, 0x4AFC);

    /* --- テスト1: D0=5 (範囲内) --- */
    p = RESET_PC;

    /* MOVEQ #5, D0 */
    write_word_be(ram, p, 0x7005);
    p += 2;

    /* MOVEQ #0, D7 */
    write_word_be(ram, p, 0x7E00);
    p += 2;

    /*
     * CHK.W #10, D0:
     * CHK の即値形式は EA=mode7/reg4 (即値)。
     * opcode = 0x4180 | (D0=0)<<9 | ea(mode=7,reg=4=0x3C)
     *        = 0x4180 | 0x003C = 0x41BC
     * bound は次のワード
     */
    write_word_be(ram, p, 0x41BC);
    p += 2;
    write_word_be(ram, p, 0x000A);
    p += 2; /* bound=10 */

    /* ILLEGAL (正常終了点) */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.d[7] == 0); /* CHK 例外は発生しない */
    mc68k_destroy(cpu);

    /* --- テスト2: D0=-1 (範囲外: 負) --- */
    memset(ram, 0, sizeof(ram));
    p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x18, chk_handler);

    write_word_be(ram, chk_handler, 0x3E3C);
    write_word_be(ram, chk_handler + 2, 0xDEAD);
    write_word_be(ram, chk_handler + 4, 0x4AFC);

    /* MOVE.W #0xFFFF, D0 → D0.W = -1 */
    write_word_be(ram, p, 0x303C);
    p += 2;
    write_word_be(ram, p, 0xFFFF);
    p += 2;

    write_word_be(ram, p, 0x7E00);
    p += 2; /* MOVEQ #0, D7 */

    write_word_be(ram, p, 0x41BC);
    p += 2; /* CHK.W #10, D0 */
    write_word_be(ram, p, 0x000A);
    p += 2;

    write_word_be(ram, p, 0x4AFC);
    p += 2;

    cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 300);

    mc68k_get_regs(cpu, &r);
    assert(r.d[7] == 0xDEAD); /* CHK 例外 → ルーチン実行 */
    mc68k_destroy(cpu);
}

/* ================================================================== */
/* ABCD / SBCD / NBCD                                                   */
/* ================================================================== */

/*
 * ABCD: BCD加算 05 + 09 + X=0 = 14 (BCD)
 *   D0.B=0x05, D1.B=0x09 → ABCD D1, D0 → D0.B=0x14, C=0
 */
static void test_abcd(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEQ #5, D0 → D0=0x05 */
    write_word_be(ram, p, 0x7005);
    p += 2;
    /* MOVEQ #9, D1 → D1=0x09 */
    write_word_be(ram, p, 0x7209);
    p += 2;

    /* ANDI.W #0xFFEF, CCR : X=0 クリア (ANDI.B #0xEF, CCR = 0x023C 0x00EF) */
    write_word_be(ram, p, 0x023C);
    p += 2;
    write_word_be(ram, p, 0x00EF);
    p += 2;

    /*
     * ABCD D1, D0 (register form):
     * opcode = 0xC100 | (D0=0)<<9 | D1=1 = 0xC101
     */
    write_word_be(ram, p, 0xC101);
    p += 2;

    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    /* 0x05 + 0x09 = 14 → BCD 0x14 */
    assert((r.d[0] & 0xFF) == 0x14);
    assert(!(r.sr & (1 << 0))); /* C=0 (桁上がりなし) */

    mc68k_destroy(cpu);
}

/*
 * ABCD 桁上がりテスト: 99 + 01 + X=0 = 100 → BCD=00, C=1
 */
static void test_abcd_carry(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, p, 0x7099);
    p += 2; /* MOVEQ #0x99, D0 = BCD 99 (byte=0x99) */
    write_word_be(ram, p, 0x7201);
    p += 2; /* MOVEQ #1, D1 */

    write_word_be(ram, p, 0x023C);
    p += 2; /* ANDI.B #0xEF, CCR → X=0 */
    write_word_be(ram, p, 0x00EF);
    p += 2;

    write_word_be(ram, p, 0xC101);
    p += 2; /* ABCD D1, D0 */
    write_word_be(ram, p, 0x60FE);
    p += 2; /* BRA *（フラグ保持して停止）*/

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    assert((r.d[0] & 0xFF) == 0x00); /* 99+01=100 → BCD=00 */
    assert(r.sr & (1 << 0));         /* C=1 */
    assert(r.sr & (1 << 4));         /* X=1 */

    mc68k_destroy(cpu);
}

/*
 * SBCD: BCD減算 23 - 09 - X=0 = 14 (BCD)
 *   D0.B=0x23, D1.B=0x09 → SBCD D1, D0 → D0.B=0x14, C=0
 */
static void test_sbcd(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, p, 0x7023);
    p += 2; /* MOVEQ #0x23, D0 = 23 BCD */
    write_word_be(ram, p, 0x7209);
    p += 2; /* MOVEQ #9, D1 */

    write_word_be(ram, p, 0x023C);
    p += 2; /* ANDI.B #0xEF, CCR → X=0 */
    write_word_be(ram, p, 0x00EF);
    p += 2;

    /*
     * SBCD D1, D0 (register form):
     * opcode = 0x8100 | (D0=0)<<9 | D1=1 = 0x8101
     */
    write_word_be(ram, p, 0x8101);
    p += 2;
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    assert((r.d[0] & 0xFF) == 0x14); /* 23-09=14 BCD */
    assert(!(r.sr & (1 << 0)));      /* C=0 */

    mc68k_destroy(cpu);
}

/*
 * NBCD: 0 - D0 - X = 0 - 0x07 - 0 = -7 → BCD 93, C=1
 *   (BCD補数: 100-7=93)
 */
static void test_nbcd(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_word_be(ram, p, 0x7007);
    p += 2; /* MOVEQ #7, D0 = 07 BCD */

    write_word_be(ram, p, 0x023C);
    p += 2; /* ANDI.B #0xEF, CCR → X=0 */
    write_word_be(ram, p, 0x00EF);
    p += 2;

    /*
     * NBCD D0 : 0x4800 | (mode=0, reg=0) = 0x4800
     */
    write_word_be(ram, p, 0x4800);
    p += 2;
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    assert((r.d[0] & 0xFF) == 0x93); /* 0-07=93 BCD (10の補数) */
    assert(r.sr & (1 << 0));         /* C=1 */

    mc68k_destroy(cpu);
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_movep_word();
    test_movep_long();
    test_tas();
    test_chk();
    test_abcd();
    test_abcd_carry();
    test_sbcd();
    test_nbcd();
    return 0;
}
