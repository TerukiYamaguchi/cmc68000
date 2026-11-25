/*
 * test_extra.c — 追加命令テスト
 *
 * BTST/BSET/BCLR/BCHG, Scc, DBcc, DIVU/DIVS,
 * ADDX/SUBX, CMPM, RTR を検証する。
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
/* BTST / BSET / BCLR / BCHG                                          */
/* ================================================================== */

/*
 * BSET #0, D0 (D0=0): D0=1, Z=1 (bit was 0)
 * BSET #2, D0: D0=5, Z=1 (bit was 0)
 * BCLR #0, D0: D0=4, Z=0 (bit was 1)
 * BCHG #2, D0: D0=0, Z=0 (bit was 1 → toggled to 0)
 * BTST #0, D0: D0=0, Z=1 (bit 0 = 0)
 */
static void test_bit_static(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    /* MOVEQ #0, D0 */
    write_word_be(ram, p, 0x7000);
    p += 2;
    /* BSET #0, D0 → D0=0x01, Z=1 */
    write_word_be(ram, p, 0x08C0);
    p += 2;
    write_word_be(ram, p, 0x0000);
    p += 2;
    /* BSET #2, D0 → D0=0x05, Z=1 */
    write_word_be(ram, p, 0x08C0);
    p += 2;
    write_word_be(ram, p, 0x0002);
    p += 2;
    /* BCLR #0, D0 → D0=0x04, Z=0 */
    write_word_be(ram, p, 0x0880);
    p += 2;
    write_word_be(ram, p, 0x0000);
    p += 2;
    /* BCHG #2, D0 → D0=0x00, Z=0 (bit was 1) */
    write_word_be(ram, p, 0x0840);
    p += 2;
    write_word_be(ram, p, 0x0002);
    p += 2;
    /* BTST #0, D0 → D0=0x00, Z=1 (bit 0 = 0) */
    write_word_be(ram, p, 0x0800);
    p += 2;
    write_word_be(ram, p, 0x0000);
    p += 2;
    write_word_be(ram, p, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x00000000);
    assert(r.sr & 0x0004); /* Z=1: BTST で bit0=0 を確認 */
}

/*
 * BTST Dn, EA の動的形式: D1 のビット番号で D0 をテスト
 */
static void test_bit_dynamic(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    /* MOVE.L #0xFF00, D0 */
    write_word_be(ram, p, 0x203C);
    p += 2;
    write_long_be(ram, p, 0x0000FF00);
    p += 4;
    /* MOVEQ #8, D1 */
    write_word_be(ram, p, 0x7208);
    p += 2; /* MOVEQ #8, D1 */
    /* BTST D1, D0: bit 8 of 0xFF00 → bit 8 = 1 → Z=0 */
    write_word_be(ram, p, 0x0300);
    p += 2; /* BTST D1, D0: 0000 001 100 000 000 */
    /* BSET D1, D0: bit 8 is already 1, set again → D0=0xFF00, Z=0 */
    write_word_be(ram, p, 0x03C0);
    p += 2; /* BSET D1, D0 */
    /* MOVEQ #7, D1 */
    write_word_be(ram, p, 0x720F);
    p += 2; /* MOVEQ #15, D1 */
    /* BCLR D1, D0: bit 15 of 0xFF00 → bit15=1 → D0=0x7F00, Z=0 */
    write_word_be(ram, p, 0x0380);
    p += 2;                        /* BCLR D1, D0 */
    write_word_be(ram, p, 0x60FE); /* BRA *（フラグ保持して停止）*/

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x00007F00);
    assert(!(r.sr & 0x0004)); /* Z=0 after BCLR (bit was 1) */
}

/*
 * BTST Dn,#imm（動的ビットテスト、ソースEAが即値モード mode=111,reg=100）。
 * ea_rmw_read()がmode=7の全サブモードを無条件にea_get_addr()（JMP/JSR/LEA/
 * PEA/MOVEM専用の制御EA、即値を正しく非サポート）へ委譲していたバグの回帰
 * テスト（x68000#82、mint.xがこの命令でクラッシュしていた）。
 * $90 = 0b10010000: bit4=1（BTST後Z=0）、bit0=0（BTST後Z=1）。
 */
static void test_bit_dynamic_immediate(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    /* MOVEQ #4, D0 */
    write_word_be(ram, p, 0x7004);
    p += 2;
    /* BTST D0,#$90 (bit4=1 → Z=0) */
    write_word_be(ram, p, 0x013C);
    p += 2;
    write_word_be(ram, p, 0x0090);
    p += 2;
    uint32_t loop_pc = p;
    write_word_be(ram, p, 0x60FE); /* BRA *（フラグ保持して自己ループで停止）*/

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    /* バグ有りだと exec_state=CPU_EXCEPTION がfault_vector未設定のまま
     * dispatch_group0_exception()へ渡り、ゼロ埋めベクタテーブルから
     * 読んだ不定PCへ飛ぶ（BRA*の自己ループへ到達しない）。 */
    assert(r.pc == loop_pc);
    assert(r.d[0] == 4);      /* BTSTはソースDnを変更しない */
    assert(!(r.sr & 0x0004)); /* Z=0（bit4がセットされていた） */
}

/* ================================================================== */
/* Scc / DBcc                                                           */
/* ================================================================== */

/*
 * Scc: ST (always 0xFF) と SF (always 0x00)
 */
static void test_scc(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    /* ST D0: 0x50C0 | dn=0 = 0x50C0 */
    write_word_be(ram, p, 0x50C0);
    p += 2;
    /* SF D1: 0x51C0 | dn=1 = 0x51C1 */
    write_word_be(ram, p, 0x51C1);
    p += 2;
    /* CMP.W D0, D0 → Z=1 */
    write_word_be(ram, p, 0xB040);
    p += 2;
    /* SEQ D2: 0x57C2 (Z=1 → 0xFF) */
    write_word_be(ram, p, 0x57C2);
    p += 2;
    write_word_be(ram, p, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFF) == 0xFF); /* ST */
    assert((r.d[1] & 0xFF) == 0x00); /* SF */
    assert((r.d[2] & 0xFF) == 0xFF); /* SEQ (Z=1) */
}

/*
 * DBcc: DBRA でループを 3 回まわす
 *   MOVE.W #2, D0
 *   loop: ADDQ.W #1, D1
 *         DBRA D0, loop
 *   → D1=3, D0=-1 (0xFFFF)
 */
static void test_dbcc(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    /* MOVE.W #2, D0: 0x303C 0x0002 */
    write_word_be(ram, p, 0x303C);
    p += 2;
    write_word_be(ram, p, 0x0002);
    p += 2;
    /* loop: ADDQ.W #1, D1 (0x5241) */
    uint32_t loop_addr = p;
    write_word_be(ram, p, 0x5241);
    p += 2;
    /* DBRA D0, loop: 0x51C8 + disp */
    write_word_be(ram, p, 0x51C8);
    p += 2;
    int16_t disp = (int16_t)(loop_addr - p); /* loop_addr - (p) */
    write_word_be(ram, p, (uint16_t)disp);
    p += 2;
    write_word_be(ram, p, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[1] & 0xFFFF) == 3);      /* ループ3回 */
    assert((r.d[0] & 0xFFFF) == 0xFFFF); /* カウンタ -1 */
}

/* ================================================================== */
/* DIVU / DIVS                                                          */
/* ================================================================== */

/*
 * DIVU.W #5, D0: 13 ÷ 5 = 商 2、余り 3
 * DIVU: D0 = (余り<<16) | 商 = 0x00030002
 */
static void test_divu(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    /* MOVE.L #13, D0 */
    write_word_be(ram, p, 0x203C);
    p += 2;
    write_long_be(ram, p, 13);
    p += 4;
    /* DIVU.W #5, D0: opcode = 0x80FC + imm */
    /* DIVU EA, D0: 0x80C0 | (0<<9) | ea
     * ea for #imm: mode=7, reg=4 → ea=0x3C
     * opcode = 0x80FC */
    write_word_be(ram, p, 0x80FC);
    p += 2;
    write_word_be(ram, p, 0x0005);
    p += 2;
    write_word_be(ram, p, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x00030002); /* 余り=3, 商=2 */
    assert(!(r.sr & 0x0002));     /* V=0 (オーバーフローなし) */
}

/*
 * DIVS.W #-3, D0: -10 ÷ -3 = 商 3、余り -1
 */
static void test_divs(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    /* MOVE.L #-10, D0 */
    write_word_be(ram, p, 0x203C);
    p += 2;
    write_long_be(ram, p, (uint32_t)-10);
    p += 4;
    /* DIVS.W #-3, D0: 0x81FC + imm */
    write_word_be(ram, p, 0x81FC);
    p += 2;
    write_word_be(ram, p, (uint16_t)-3);
    p += 2;
    write_word_be(ram, p, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    /* 商=3 (0x0003), 余り=-1 (0xFFFF) → D0=0xFFFF0003 */
    assert(r.d[0] == 0xFFFF0003);
}

/* ================================================================== */
/* ADDX / SUBX                                                          */
/* ================================================================== */

/*
 * ADDX.L D1, D0: D0=0xFFFFFF00, D1=0x100, X=0
 * → D0 = 0xFFFFFF00 + 0x100 + 0 = 0x100000000 → D0=0, X=1 (carry)
 * また Z 保持規則: 直前の Z=1 (SUBQ で D2=0 → Z=1) も ADDX の Z に反映される
 * ADDX 結果が 0 → Z を保持 (Z=1)
 */
static void test_addx(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    /* MOVE.L #0xFFFFFF00, D0 */
    write_word_be(ram, p, 0x203C);
    p += 2;
    write_long_be(ram, p, 0xFFFFFF00);
    p += 4;
    /* MOVE.L #0x100, D1 */
    write_word_be(ram, p, 0x223C);
    p += 2;
    write_long_be(ram, p, 0x00000100);
    p += 4;
    /* SUBQ.W #1, D2 を2回: D2=0→-1, D2=-1→-2... ではなく
     * ADDQ.W #1, D2 → SUBQ.W #1, D2 → Z=1, X=0 を確保 */
    write_word_be(ram, p, 0x5442);
    p += 2; /* ADDQ.W #1, D2 */
    write_word_be(ram, p, 0x5542);
    p += 2; /* SUBQ.W #1, D2 → Z=1, X=0 */
    /* ADDX.L D1, D0: opcode = 0xD100 | (0<<9) | (2<<6) | 1 = 0xD181 */
    write_word_be(ram, p, 0xD181);
    p += 2;
    write_word_be(ram, p, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.d[0] == 0x00000000); /* overflow: lower 32bit = 0 */
    assert(r.sr & 0x0010);        /* X=1: キャリー発生 */
    assert(r.sr & 0x0004);        /* Z=1: 結果0、Z保持規則 */
}

/*
 * SUBX.W D1, D0: D0 = D0 - D1 - X
 * D0=0x0010, D1=0x0008, X=0 → D0 = 0x0008
 */
static void test_subx(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    /* MOVEQ #0x10, D0 */
    write_word_be(ram, p, 0x7010);
    p += 2;
    /* MOVEQ #0x08, D1 */
    write_word_be(ram, p, 0x7208);
    p += 2;
    /* MOVEQ #0, D2 → Z=1, X=0 (MOVEQ sets X as well as Z? No, MOVEQ uses ccr_logic which clears
     * X... wait) */
    /* Actually MOVEQ does ccr_logic which clears C and V but X is "unchanged". Let me use a
     * different approach. */
    /* Use ADDQ.W #1, D2 then SUBQ.W #1, D2 to clear X */
    write_word_be(ram, p, 0x5442);
    p += 2; /* ADDQ.W #1, D2 */
    write_word_be(ram, p, 0x5542);
    p += 2; /* SUBQ.W #1, D2: Z=1, X=0 */
    /* SUBX.W D1, D0: 0x9140 | (0<<9) | (1<<6) | 1 = 0x9141 */
    write_word_be(ram, p, 0x9141);
    p += 2;
    write_word_be(ram, p, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[0] & 0xFFFF) == 0x0008);
}

/* ================================================================== */
/* CMPM                                                                  */
/* ================================================================== */

/*
 * CMPM.W (A1)+, (A0)+: (A0) - (A1) でフラグ更新
 * A0 → 0x2000 (値0x1234), A1 → 0x2100 (値0x1234)
 * → 等しい: Z=1
 */
static void test_cmpm(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* メモリにデータを配置 */
    write_word_be(ram, 0x2000, 0x1234);
    write_word_be(ram, 0x2100, 0x1234);

    uint32_t p = RESET_PC;
    /* LEA 0x2000, A0 */
    write_word_be(ram, p, 0x41F8);
    p += 2; /* LEA (abs.W), A0 */
    write_word_be(ram, p, 0x2000);
    p += 2;
    /* LEA 0x2100, A1 */
    write_word_be(ram, p, 0x43F8);
    p += 2;
    write_word_be(ram, p, 0x2100);
    p += 2;
    /* CMPM.W (A1)+, (A0)+: 0xB149 (ax=0, sz=1, ay=1) */
    /* opcode = 0xB100 | (ax<<9) | (sz<<6) | 0x08 | ay
     *        = 0xB100 | (0<<9) | (1<<6) | 0x08 | 1
     *        = 0xB100 | 0x0040 | 0x0009 = 0xB149 */
    write_word_be(ram, p, 0xB149);
    p += 2;
    write_word_be(ram, p, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.sr & 0x0004);    /* Z=1: 値が等しい */
    assert(r.a[0] == 0x2002); /* postincrement */
    assert(r.a[1] == 0x2102);
}

/* ================================================================== */
/* RTR                                                                   */
/* ================================================================== */

/*
 * RTR: スタックから CCR と PC を復元する。
 * スタックに CCR=0x001F (全フラグセット), PC=BRA* を積んでから RTR。
 * → CCR=0x1F, SR 上位バイトは変化しない。
 */
static void test_rtr(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t target = 0x00002000;
    write_word_be(ram, target + 0, 0x60FE); /* target: BRA *（PC を保持して停止） */

    uint32_t p = RESET_PC;
    /* MOVE.W #target_hi, -(SP): PC 上位 → push */
    /* より簡単: LEA target, A0; MOVE.L A0, -(SP) してから MOVE.W #0x001F, -(SP) */
    /* MOVE.L #target, -(A7) */
    write_word_be(ram, p, 0x2F3C);
    p += 2; /* MOVE.L #imm, -(A7) */
    write_long_be(ram, p, target);
    p += 4;
    /* MOVE.W #0x001F, -(A7) (CCR 値) */
    write_word_be(ram, p, 0x3F3C);
    p += 2; /* MOVE.W #imm, -(A7) */
    write_word_be(ram, p, 0x001F);
    p += 2;
    /* RTR */
    write_word_be(ram, p, 0x4E77);
    p += 2;
    write_word_be(ram, p, 0x4AFC); /* ここには来ない */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    /* RTR 後 PC=target に飛び BRA * で停止（自己分岐なので PC=target のまま） */
    assert(r.pc == target);
    /* CCR: 全フラグ (0x1F = XNZVC) がセット */
    assert((r.sr & 0x001F) == 0x001F);
    /* SR 上位バイト: 元のスーパーバイザビット等は保持 */
    assert(r.sr & (1 << 13)); /* S ビット維持 */
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_bit_static();
    test_bit_dynamic();
    test_bit_dynamic_immediate();
    test_scc();
    test_dbcc();
    test_divu();
    test_divs();
    test_addx();
    test_subx();
    test_cmpm();
    test_rtr();
    return 0;
}
