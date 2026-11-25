/*
 * test_missing.c — 未実装だった命令のテスト
 *
 * NEGX / STOP / RESET / TRAPV / MOVE USP を検証する。
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

/* ================================================================== */
/* NEGX                                                                 */
/* ================================================================== */

/*
 * NEGX.L D0, X=0 の基本動作
 *   D0=1, X=0  → result = -1 = 0xFFFFFFFF, C=1, X=1, N=1, Z=0, V=0
 */
static void test_negx_basic(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEQ #1, D0        ; D0=1, CCR: Z=0,N=0,V=0,C=0,X=0 (MOVEQ は X を変えない) */
    write_word_be(ram, p, 0x7001);
    p += 2;
    /* ANDI #0xEF, CCR     ; X ビットを明示的にクリア */
    write_word_be(ram, p, 0x023C);
    p += 2;
    write_word_be(ram, p, 0x00EF);
    p += 2;
    /* NEGX.L D0           ; opcode: 0100 0000 10 000 000 = 0x4080 */
    write_word_be(ram, p, 0x4080);
    p += 2;
    /* BRA *（フラグを変えずに停止。0x4AFC は ILLEGAL 例外で PC=0 へ飛び CCR を破壊する）*/
    write_word_be(ram, p, 0x60FE);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 100);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.d[0] == 0xFFFFFFFFu); /* 0 - 1 - 0 = -1 */
    assert(r.sr & (1 << 4));       /* X=1 */
    assert(r.sr & (1 << 3));       /* N=1 */
    assert(!(r.sr & (1 << 2)));    /* Z=0 */
    assert(!(r.sr & (1 << 1)));    /* V=0 */
    assert(r.sr & (1 << 0));       /* C=1 */
    mc68k_destroy(cpu);
}

/*
 * NEGX.B D0, X=0, D0=0x80 → オーバーフロー
 *   0 - 0x80 - 0 = 0x80, V=1, C=1, N=1, Z=0
 */
static void test_negx_overflow(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEQ #0x7F, D0 → D0=0x7F (MOVEQ は符号拡張なので 0x7F → 0x0000007F) */
    write_word_be(ram, p, 0x707F);
    p += 2;
    /* ADDI.B #1, D0 → D0.byte = 0x80, V=1 (0x7F+1 オーバーフロー) */
    write_word_be(ram, p, 0x0600);
    p += 2;
    write_word_be(ram, p, 0x0001);
    p += 2;
    /* ANDI #0xEF, CCR  ; X をクリア */
    write_word_be(ram, p, 0x023C);
    p += 2;
    write_word_be(ram, p, 0x00EF);
    p += 2;
    /* NEGX.B D0  ; 0x4000 */
    write_word_be(ram, p, 0x4000);
    p += 2;
    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 100);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert((r.d[0] & 0xFF) == 0x80u); /* 0 - 0x80 = 0x80 */
    assert(r.sr & (1 << 1));          /* V=1 オーバーフロー */
    assert(r.sr & (1 << 3));          /* N=1 */
    assert(r.sr & (1 << 0));          /* C=1 */
    mc68k_destroy(cpu);
}

/*
 * NEGX.L D0 の Z フラグ保持テスト
 *   D0=0, X=0 → result=0, Z は変化なし（1 のままであること）
 */
static void test_negx_z_unchanged(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEQ #0, D0  ; D0=0, Z=1 */
    write_word_be(ram, p, 0x7000);
    p += 2;
    /* ANDI #0xEF, CCR  ; X=0 */
    write_word_be(ram, p, 0x023C);
    p += 2;
    write_word_be(ram, p, 0x00EF);
    p += 2;
    /* NEGX.L D0  ; 0x4080  result=0-0-0=0, Z は 1 のまま */
    write_word_be(ram, p, 0x4080);
    p += 2;
    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 100);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.d[0] == 0);
    assert(r.sr & (1 << 2));    /* Z=1 (変化なし) */
    assert(!(r.sr & (1 << 0))); /* C=0 (ボローなし) */
    mc68k_destroy(cpu);
}

/*
 * NEGX.W D0, X=1: D0=0x0000, X=1 → result = 0xFFFF, C=1
 *   多倍長演算のキャリーチェーン確認
 */
static void test_negx_with_extend(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEQ #0, D0  ; D0=0, Z=1 */
    write_word_be(ram, p, 0x7000);
    p += 2;
    /* ORI #0x10, CCR  ; X=1 をセット */
    write_word_be(ram, p, 0x003C);
    p += 2;
    write_word_be(ram, p, 0x0010);
    p += 2;
    /* NEGX.W D0  ; 0x4040  result = 0 - 0 - 1 = 0xFFFF, C=1 */
    write_word_be(ram, p, 0x4040);
    p += 2;
    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 100);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert((r.d[0] & 0xFFFF) == 0xFFFFu);
    assert(r.sr & (1 << 0));    /* C=1 */
    assert(r.sr & (1 << 4));    /* X=1 */
    assert(r.sr & (1 << 3));    /* N=1 */
    assert(!(r.sr & (1 << 2))); /* Z=0 (結果非ゼロ) */
    mc68k_destroy(cpu);
}

/* ================================================================== */
/* TRAPV                                                                */
/* ================================================================== */

/*
 * TRAPV with V=0: 何も起きず次命令へ
 */
static void test_trapv_no_trap(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* ANDI #0xFD, CCR  ; V=0 (bit 1 をクリア) */
    write_word_be(ram, p, 0x023C);
    p += 2;
    write_word_be(ram, p, 0x00FD);
    p += 2;
    /* MOVEQ #0x42, D0  ; D0=0x42 */
    write_word_be(ram, p, 0x7042);
    p += 2;
    /* TRAPV  ; 0x4E76 */
    write_word_be(ram, p, 0x4E76);
    p += 2;
    /* MOVEQ #0x55, D1  ; D1=0x55 (トラップが起きなければ実行される) */
    write_word_be(ram, p, 0x7255);
    p += 2;
    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 100);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.d[0] == 0x42u);
    assert(r.d[1] == 0x55u); /* TRAPV 後の命令が実行された */
    mc68k_destroy(cpu);
}

/*
 * TRAPV with V=1: ベクタ 7 へジャンプしてハンドラを実行
 */
static void test_trapv_takes_trap(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* ベクタ 7 (0x1C) → ハンドラアドレス 0x2000 */
    write_long_be(ram, 0x001C, 0x2000);

    /* ハンドラ: MOVEQ #0x55, D7 (0x55 は bit7=0 なので sign-extend で 0x55 のまま); ILLEGAL */
    write_word_be(ram, 0x2000, 0x7E55); /* MOVEQ #0x55, D7 */
    write_word_be(ram, 0x2002, 0x4AFC); /* ILLEGAL */

    /* ORI #0x02, CCR  ; V=1 */
    write_word_be(ram, p, 0x003C);
    p += 2;
    write_word_be(ram, p, 0x0002);
    p += 2;
    /* TRAPV  ; V=1 なのでベクタ 7 へ */
    write_word_be(ram, p, 0x4E76);
    p += 2;
    /* MOVEQ #0x00, D0  ; トラップが起きていれば実行されない */
    write_word_be(ram, p, 0x7000);
    p += 2;
    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.d[7] == 0x55u); /* ハンドラが実行された */
    mc68k_destroy(cpu);
}

/* ================================================================== */
/* MOVE USP                                                             */
/* ================================================================== */

/*
 * MOVE A0, USP / MOVE USP, A1 で USP を経由してレジスタ転送
 */
static void test_move_usp(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEA.L #0x12345678, A0  (使いやすい値をセット) */
    /* MOVE.L #imm, A0 = MOVEA.L: opcode 0x207C + long imm */
    write_word_be(ram, p, 0x207C);
    p += 2;
    write_long_be(ram, p, 0x12345678);
    p += 4;
    /* MOVE A0, USP  ; 0x4E60 (bits 2-0=0 → A0) */
    write_word_be(ram, p, 0x4E60);
    p += 2;
    /* MOVE USP, A1  ; 0x4E69 (bit 3=1, bits 2-0=1 → A1) */
    write_word_be(ram, p, 0x4E69);
    p += 2;
    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 100);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.usp == 0x12345678u);  /* USP に A0 の値が格納された */
    assert(r.a[1] == 0x12345678u); /* A1 に USP から値が読み出された */
    mc68k_destroy(cpu);
}

/* ================================================================== */
/* RESET                                                                */
/* ================================================================== */

/*
 * RESET 命令が実行されてもレジスタが壊れないことを確認
 */
static void test_reset_instr(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEQ #0x7E, D0 */
    write_word_be(ram, p, 0x707E);
    p += 2;
    /* RESET  ; 0x4E70 */
    write_word_be(ram, p, 0x4E70);
    p += 2;
    /* MOVEQ #0x01, D1 */
    write_word_be(ram, p, 0x7201);
    p += 2;
    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.d[0] == 0x7Eu);
    assert(r.d[1] == 0x01u); /* RESET 後も続きが実行された */
    mc68k_destroy(cpu);
}

/* ================================================================== */
/* STOP                                                                 */
/* ================================================================== */

/*
 * STOP #0x2700 で CPU が CPU_STOPPED になることを確認
 */
static void test_stop_halts(void) {
    uint8_t ram[RAM_SIZE] = {0};
    uint32_t p = RESET_PC;

    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* MOVEQ #1, D0 */
    write_word_be(ram, p, 0x7001);
    p += 2;
    /* STOP #0x2700  ; 0x4E72 + imm */
    write_word_be(ram, p, 0x4E72);
    p += 2;
    write_word_be(ram, p, 0x2700);
    p += 2;
    /* MOVEQ #2, D0  ; STOP 後は実行されないはず */
    write_word_be(ram, p, 0x7002);
    p += 2;
    /* ILLEGAL */
    write_word_be(ram, p, 0x4AFC);
    p += 2;

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.d[0] == 0x01u);                          /* STOP 後の命令は実行されていない */
    assert(mc68k_get_exec_state(cpu) == CPU_STOPPED); /* 状態が STOPPED */
    assert(r.sr == 0x2700u);                          /* SR が即値で更新された */

    /* 2 度目の execute も 0 サイクルで返る（IRQ なし） */
    int consumed = mc68k_execute(cpu, 100);
    assert(consumed == 0);
    assert(mc68k_get_exec_state(cpu) == CPU_STOPPED);

    mc68k_destroy(cpu);
}

/* ================================================================== */
/* main                                                                 */
/* ================================================================== */

int main(void) {
    test_negx_basic();
    test_negx_overflow();
    test_negx_z_unchanged();
    test_negx_with_extend();
    test_trapv_no_trap();
    test_trapv_takes_trap();
    test_move_usp();
    test_reset_instr();
    test_stop_halts();
    return 0;
}
