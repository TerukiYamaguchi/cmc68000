/*
 * test_trace.c — トレース例外（ベクタ9）のユニットテスト
 *
 * 命令実行前に SR の T ビットが立っていた場合、その命令が正常完了した後に
 * トレース例外（ベクタ9）が発生することを検証する。
 *
 * 検証項目:
 *   - 基本動作: T=1でNOP実行後にベクタ9へ、フレーム内容（saved SR に T=1、
 *     pushed PC=次命令アドレス）が正しいこと
 *   - 複数命令: T=1のまま単一ステップが継続し、各命令ごとにトレースが
 *     発生すること（ハンドラ自身は T=0 で実行されるため再帰しない）
 *   - Group1抑止: ユーザモードでの特権命令違反、ILLEGAL命令はそれぞれ
 *     ベクタ8/4のみが発生し、ベクタ9は発生しないこと
 *   - Group2追随+優先順位: TRAP実行時はTRAPハンドラより先にトレース
 *     ハンドラが実行されること（トレースフレームがTRAPフレームの上に
 *     積まれるため）
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

#define RAM_SIZE      0x10000
#define RESET_SSP     0x00008000
#define RESET_PC      0x00001000
#define VEC9_HANDLER  0x00001500 /* トレースハンドラ */
#define VEC8_HANDLER  0x00001540 /* 特権違反ハンドラ */
#define VEC4_HANDLER  0x00001560 /* ILLEGAL命令ハンドラ */
#define TRAP0_HANDLER 0x00001580 /* TRAP #0 ハンドラ */

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

static uint16_t read_word_be(const uint8_t *ram, uint32_t addr) {
    return (uint16_t)((ram[addr] << 8) | ram[addr + 1]);
}

static uint32_t read_long_be(const uint8_t *ram, uint32_t addr) {
    return ((uint32_t)ram[addr] << 24) | ((uint32_t)ram[addr + 1] << 16) |
           ((uint32_t)ram[addr + 2] << 8) | (uint32_t)ram[addr + 3];
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

static void write_vector_table(uint8_t *ram) {
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x20, VEC8_HANDLER);  /* ベクタ8: 特権違反 */
    write_long_be(ram, 0x10, VEC4_HANDLER);  /* ベクタ4: ILLEGAL */
    write_long_be(ram, 0x24, VEC9_HANDLER);  /* ベクタ9: トレース */
    write_long_be(ram, 0x80, TRAP0_HANDLER); /* ベクタ32: TRAP #0 */
}

/* ================================================================== */
/* 基本動作: T=1 で NOP を実行 → ベクタ9へ、フレーム内容を検証           */
/* ================================================================== */

/*
 * 0x1000: ORI.W #0x8000, SR   ; T=1 にする（この命令自体は旧T=0でトレースされない）
 * 0x1004: NOP                 ; T=1 でトレース対象
 * 0x1006: BRA *               ; トレースされれば到達しないはず
 *
 * ハンドラ (VEC9_HANDLER): ADDQ.W #1,D5; BRA *（フレームを残したまま自己ループ）
 */
static void test_trace_basic(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_vector_table(ram);

    write_word_be(ram, RESET_PC + 0, 0x007C); /* ORI.W #0x8000, SR */
    write_word_be(ram, RESET_PC + 2, 0x8000);
    write_word_be(ram, RESET_PC + 4, 0x4E71); /* NOP */
    write_word_be(ram, RESET_PC + 6, 0x60FE); /* BRA * */

    write_word_be(ram, VEC9_HANDLER + 0, 0x5245); /* ADDQ.W #1, D5 */
    write_word_be(ram, VEC9_HANDLER + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[5] & 0xFFFF) == 1);   /* トレースハンドラが1回だけ実行された */
    assert(r.pc == VEC9_HANDLER + 2); /* ハンドラ内 BRA * の自己ループ */
    assert(r.a[7] == RESET_SSP - 6);  /* シンプルフレーム（6バイト）、未RTEで残存 */

    uint16_t saved_sr = read_word_be(ram, r.a[7] + 0);
    uint32_t saved_pc = read_long_be(ram, r.a[7] + 2);
    assert(saved_sr & 0x8000);        /* 保存 SR に T=1 が残っている */
    assert(saved_pc == RESET_PC + 6); /* pushed PC = 次命令（BRA *）のアドレス */
}

/* ================================================================== */
/* 複数命令: T=1 のまま単一ステップが継続する                            */
/* ================================================================== */

/*
 * 0x1000: ORI.W #0x8000, SR
 * 0x1004-0x1008: NOP x3        ; それぞれトレースされる（計3回)
 * 0x100A: STOP #0x2700         ; 実行開始時のTによりトレースされ、停止しない
 *
 * ハンドラ: ADDQ.W #1,D5; RTE（T をクリアしないが STOP が SR を丸ごと
 * 上書きするので問題ない）
 */
static void test_trace_each_instruction(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_vector_table(ram);

    write_word_be(ram, RESET_PC + 0, 0x007C); /* ORI.W #0x8000, SR */
    write_word_be(ram, RESET_PC + 2, 0x8000);
    write_word_be(ram, RESET_PC + 4, 0x4E71);  /* NOP */
    write_word_be(ram, RESET_PC + 6, 0x4E71);  /* NOP */
    write_word_be(ram, RESET_PC + 8, 0x4E71);  /* NOP */
    write_word_be(ram, RESET_PC + 10, 0x4E72); /* STOP #0x2700 */
    write_word_be(ram, RESET_PC + 12, 0x2700);

    write_word_be(ram, VEC9_HANDLER + 0, 0x5245); /* ADDQ.W #1, D5 */
    write_word_be(ram, VEC9_HANDLER + 2, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[5] & 0xFFFF) == 4); /* NOP 3回と traced STOP でトレースが発生 */
}

/* ================================================================== */
/* Group1抑止: 特権違反ではベクタ9が発生しない                           */
/* ================================================================== */

/*
 * 0x1000: MOVE.W #0x8000, SR  ; T=1,S=0（ユーザモード）にする。
 *                               旧T=0なのでこの命令自体はトレースされない
 * 0x1004: STOP #0x2700        ; ユーザモードなので特権違反（ベクタ8）
 * 0x1008: BRA *               ; 違反すれば到達しない
 *
 * ハンドラ:
 *   VEC8: ADDQ.W #1,D6; BRA *
 *   VEC9: ADDQ.W #1,D5; BRA *（呼ばれてはいけない）
 */
static void test_trace_suppressed_on_privilege(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_vector_table(ram);

    write_word_be(ram, RESET_PC + 0, 0x46FC); /* MOVE.W #0x8000, SR */
    write_word_be(ram, RESET_PC + 2, 0x8000);
    write_word_be(ram, RESET_PC + 4, 0x4E72); /* STOP #0x2700（ユーザモードで特権違反） */
    write_word_be(ram, RESET_PC + 6, 0x2700);
    write_word_be(ram, RESET_PC + 8, 0x60FE); /* BRA * */

    write_word_be(ram, VEC8_HANDLER + 0, 0x5246); /* ADDQ.W #1, D6 */
    write_word_be(ram, VEC8_HANDLER + 2, 0x60FE); /* BRA * */
    write_word_be(ram, VEC9_HANDLER + 0, 0x5245); /* ADDQ.W #1, D5 */
    write_word_be(ram, VEC9_HANDLER + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[6] & 0xFFFF) == 1); /* 特権違反ハンドラは実行された */
    assert((r.d[5] & 0xFFFF) == 0); /* トレースハンドラは実行されていない */
    assert(r.pc == VEC8_HANDLER + 2);
}

/* ================================================================== */
/* Group1抑止: ILLEGAL命令ではベクタ9が発生しない                        */
/* ================================================================== */

/*
 * 0x1000: ORI.W #0x8000, SR   ; T=1（旧T=0なのでこの命令自体は未トレース）
 * 0x1004: ILLEGAL (0x4AFC)    ; ベクタ4
 *
 * ハンドラ:
 *   VEC4: ADDQ.W #1,D7; BRA *
 *   VEC9: ADDQ.W #1,D5; BRA *（呼ばれてはいけない）
 */
static void test_trace_suppressed_on_illegal(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_vector_table(ram);

    write_word_be(ram, RESET_PC + 0, 0x007C); /* ORI.W #0x8000, SR */
    write_word_be(ram, RESET_PC + 2, 0x8000);
    write_word_be(ram, RESET_PC + 4, 0x4AFC); /* ILLEGAL */

    write_word_be(ram, VEC4_HANDLER + 0, 0x5247); /* ADDQ.W #1, D7 */
    write_word_be(ram, VEC4_HANDLER + 2, 0x60FE); /* BRA * */
    write_word_be(ram, VEC9_HANDLER + 0, 0x5245); /* ADDQ.W #1, D5 */
    write_word_be(ram, VEC9_HANDLER + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[7] & 0xFFFF) == 1); /* ILLEGALハンドラは実行された */
    assert((r.d[5] & 0xFFFF) == 0); /* トレースハンドラは実行されていない */
    assert(r.pc == VEC4_HANDLER + 2);
}

/* ================================================================== */
/* Group2追随+優先順位: TRAP実行時はトレースハンドラが先に走る            */
/* ================================================================== */

/*
 * 0x1000: ORI.W #0x8000, SR   ; T=1（旧T=0なのでこの命令自体は未トレース）
 * 0x1004: TRAP #0             ; T=1 でトレース対象。Group2なので抑止されない
 * 0x1006: BRA *                ; 両ハンドラのRTE後にここへ戻る
 *
 * D5 を共有ステップカウンタとして両ハンドラでインクリメントし、
 * 自分の番号を D6(トレース)/D7(TRAP) に記録する。
 * 期待: トレースハンドラが先に走るので D6=1、TRAPハンドラは後なので D7=2。
 * 各ハンドラは RTE 前にスタック上の保存SRのTビットをクリアし、
 * トレースの連鎖（BRA * 自己ループの再トレース）を止める。
 *
 * ハンドラ共通の「Tビットクリア」部分:
 *   MOVE.W (A7),D0       ; 0x3017  保存SRを読む
 *   ANDI.W #0x7FFF,D0    ; 0x0240 0x7FFF  Tビットを落とす
 *   MOVE.W D0,(A7)       ; 0x3E80  書き戻す
 */
static void test_trace_on_trap_order(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_vector_table(ram);

    write_word_be(ram, RESET_PC + 0, 0x007C); /* ORI.W #0x8000, SR */
    write_word_be(ram, RESET_PC + 2, 0x8000);
    write_word_be(ram, RESET_PC + 4, 0x4E40); /* TRAP #0 */
    write_word_be(ram, RESET_PC + 6, 0x60FE); /* BRA * */

    /* VEC9_HANDLER（トレース）: D5++; D6=D5; Tクリア; RTE */
    uint32_t p = VEC9_HANDLER;
    write_word_be(ram, p, 0x5245);
    p += 2; /* ADDQ.W #1, D5 */
    write_word_be(ram, p, 0x3C05);
    p += 2; /* MOVE.W D5, D6 */
    write_word_be(ram, p, 0x3017);
    p += 2; /* MOVE.W (A7), D0 */
    write_word_be(ram, p, 0x0240);
    p += 2; /* ANDI.W #0x7FFF, D0 */
    write_word_be(ram, p, 0x7FFF);
    p += 2;
    write_word_be(ram, p, 0x3E80);
    p += 2; /* MOVE.W D0, (A7) */
    write_word_be(ram, p, 0x4E73);
    p += 2; /* RTE */

    /* TRAP0_HANDLER: D5++; D7=D5; Tクリア; RTE */
    p = TRAP0_HANDLER;
    write_word_be(ram, p, 0x5245);
    p += 2; /* ADDQ.W #1, D5 */
    write_word_be(ram, p, 0x3E05);
    p += 2; /* MOVE.W D5, D7 */
    write_word_be(ram, p, 0x3017);
    p += 2; /* MOVE.W (A7), D0 */
    write_word_be(ram, p, 0x0240);
    p += 2; /* ANDI.W #0x7FFF, D0 */
    write_word_be(ram, p, 0x7FFF);
    p += 2;
    write_word_be(ram, p, 0x3E80);
    p += 2; /* MOVE.W D0, (A7) */
    write_word_be(ram, p, 0x4E73);
    p += 2; /* RTE */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 500);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[6] & 0xFFFF) == 1); /* トレースハンドラが先に実行された(step=1) */
    assert((r.d[7] & 0xFFFF) == 2); /* TRAPハンドラは後に実行された(step=2) */
    assert(r.pc == RESET_PC + 6);   /* 両方RTE後、元のBRA * に正しく復帰 */
    assert(r.a[7] == RESET_SSP);    /* スタックは完全に巻き戻っている */
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_trace_basic();
    test_trace_each_instruction();
    test_trace_suppressed_on_privilege();
    test_trace_suppressed_on_illegal();
    test_trace_on_trap_order();
    return 0;
}
