/*
 * test_cycle_golden.c — 68000プロファイルの総消費サイクル数のゴールデン回帰
 * (, 現行仕様
 *
 * DBRA の3経路と EA 加算を合わせた命令列（MOVE.W #2,D0 /
 * loop: ADDQ.L #1,(A0) / DBRA D0,loop）を68000モデルで実行し、mc68k_execute()
 * が返す総消費サイクル数を 期待値として固定する。Phase 0
 * の最初のコミットで期待値を採取し、Phase 2a
 * （EAコスト層導入）で1度だけ更新する。それ以外（Phase 1・Phase 3）では
 * 値が変わらないことを保証する回帰テストである。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdlib.h>

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

static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((uint8_t *)p)[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    uint8_t *m = p;
    *v = (uint16_t)((m[a] << 8) | m[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    uint8_t *m = p;
    *v = ((uint32_t)m[a] << 24) | ((uint32_t)m[a + 1] << 16) | ((uint32_t)m[a + 2] << 8) |
         m[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ((uint8_t *)p)[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    uint8_t *m = p;
    m[a] = (uint8_t)(v >> 8);
    m[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    uint8_t *m = p;
    m[a] = (uint8_t)(v >> 24);
    m[a + 1] = (uint8_t)(v >> 16);
    m[a + 2] = (uint8_t)(v >> 8);
    m[a + 3] = (uint8_t)v;
    return true;
}

static mc68k_state_t *make_cpu(uint8_t *ram) {
    mc68k_bus_t bus = {0};
    bus.ctx = ram;
    bus.read_byte = rb;
    bus.read_word = rw;
    bus.read_long = rl;
    bus.write_byte = wb;
    bus.write_word = ww;
    bus.write_long = wl;
    return mc68k_create_model(&bus, MC68K_MODEL_68000);
}

/*
 * MOVE.W #2, D0
 * loop: ADDQ.L #1, (A0)
 *       DBRA D0, loop
 * → *(A0)=3, D0=-1 (0xFFFF)。
 */
static int run_golden_sequence(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x303C); /* MOVE.W #2, D0 */
    p += 2;
    write_word_be(ram, p, 0x0002);
    p += 2;
    uint32_t loop_addr = p;
    write_word_be(ram, p, 0x5290); /* ADDQ.L #1, (A0) */
    p += 2;
    write_word_be(ram, p, 0x51C8); /* DBRA D0, loop */
    p += 2;
    int16_t disp = (int16_t)(loop_addr - p);
    write_word_be(ram, p, (uint16_t)disp);
    p += 2;
    write_word_be(ram, p, 0x4AFC); /* ILLEGAL（停止用） */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    cpu->a[0] = 0x00002000;

    /* ILLEGAL（停止用）の手前、ちょうど7命令（MOVE.W ×1 + (ADDQ.L + DBRA) ×3）
     * だけを1命令ずつ実行して合計する。十分大きな予算を一括で渡すと ILLEGAL
     * 到達後の例外処理サイクルまで total に混入してしまうため、命令数を
     * 数えて正確にそこで止める。 */
    int consumed = 0;
    for (int i = 0; i < 7; i++)
        consumed += mc68k_execute(cpu, 1);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(((uint32_t)ram[0x2000] << 24 | (uint32_t)ram[0x2001] << 16 |
            (uint32_t)ram[0x2002] << 8 | ram[0x2003]) == 3);
    assert((r.d[0] & 0xFFFF) == 0xFFFF);
    return consumed;
}

/* ゴールデン期待値。Phase 2a の値（内訳:
 * MOVE.W #2,D0=8 + (ADDQ.L #1,(A0)=12 + FEA((An).L)=8 + DBRA branch=10)×2 +
 * ADDQ.L #1,(A0)=20 + DBRA expired=14 = 8 + 60 + 20 + 14 = 102。
 * 直前の同じ構造で EA を加算しない値は 78 であり、差分24は [UM00 Table 8-1] の
 * FEA((An).L)=8 が3反復分加算された値と一致する。Phase 2a以外の Phase で
 * この値が変わったら回帰である。 */
#define GOLDEN_TOTAL_CYCLES 102

static void test_golden_regression(void) {
    int consumed = run_golden_sequence();
    assert(consumed == GOLDEN_TOTAL_CYCLES);
}

int main(void) {
    test_golden_regression();
    return 0;
}
