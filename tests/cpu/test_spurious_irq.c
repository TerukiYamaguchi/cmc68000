/*
 * test_spurious_irq.c — スプリアス割り込み（ベクタ24）のユニットテスト
 *
 * IACK（割り込み確認）サイクル中にホスト側バスコールバックが
 * mc68k_signal_bus_error() を呼んだ場合（＝どのデバイスも応答しなかった場合）、
 * オートベクタではなくベクタ24固定でディスパッチされることを検証する。
 * あわせて、通常のベクタドインタラプト・オートベクタが回帰していないことも確認する。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"

#define RAM_SIZE           0x10000u
#define RESET_SSP          0x00008000u
#define RESET_PC           0x00001000u
#define SPURIOUS_HANDLER   0x00001600u /* ベクタ24 (0x60) */
#define VECTORED_HANDLER   0x00001620u /* カスタムベクタ (0x40 = ベクタ番号16) */
#define AUTOVECTOR_HANDLER 0x00001640u /* level3 オートベクタ (0x6C) */
#define CUSTOM_VECTOR_NUM  16u         /* アドレス 16*4 = 0x40 */
#define IRQ_LEVEL          3

typedef enum {
    IACK_AUTOVECTOR,   /* 0 を返す（バスエラーなし）→ オートベクタへフォールバック */
    IACK_VECTORED,     /* 非0のベクタ番号を返す（バスエラーなし） */
    IACK_SPURIOUS,     /* バスエラーを通知（どのデバイスも応答せず） */
    IACK_BERR_PRIORITY /* 非0を返しつつバスエラーも通知 → スプリアス優先 */
} iack_mode_t;

typedef struct {
    uint8_t ram[RAM_SIZE];
    mc68k_state_t *cpu;
    iack_mode_t mode;
} test_ctx_t;

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

/* --- バスコールバック: RAM_SIZE の素朴なメモリ実装（バスエラーなし） --- */

static bool tb_read_byte(void *ctx, uint32_t addr, uint8_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    *out = t->ram[addr % RAM_SIZE];
    return true;
}

static bool tb_read_word(void *ctx, uint32_t addr, uint16_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    uint32_t a = addr % RAM_SIZE;
    *out = (uint16_t)((t->ram[a] << 8) | t->ram[a + 1]);
    return true;
}

static bool tb_read_long(void *ctx, uint32_t addr, uint32_t *out) {
    uint16_t hi = 0, lo = 0;
    tb_read_word(ctx, addr, &hi);
    tb_read_word(ctx, addr + 2, &lo);
    *out = ((uint32_t)hi << 16) | lo;
    return true;
}

static bool tb_write_byte(void *ctx, uint32_t addr, uint8_t val) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    t->ram[addr % RAM_SIZE] = val;
    return true;
}

static bool tb_write_word(void *ctx, uint32_t addr, uint16_t val) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    uint32_t a = addr % RAM_SIZE;
    t->ram[a] = (uint8_t)(val >> 8);
    t->ram[a + 1] = (uint8_t)(val & 0xFF);
    return true;
}

static bool tb_write_long(void *ctx, uint32_t addr, uint32_t val) {
    tb_write_word(ctx, addr, (uint16_t)(val >> 16));
    tb_write_word(ctx, addr + 2, (uint16_t)(val & 0xFFFF));
    return true;
}

static void tb_sync(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}
static void tb_write_hook(void *ctx, uint32_t addr, uint32_t size) {
    (void)ctx;
    (void)addr;
    (void)size;
}

/* IACK コールバック: モードに応じてベクタ番号・バスエラー通知を出し分ける */
static uint8_t tb_iack(void *ctx, int level) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    (void)level;
    switch (t->mode) {
    case IACK_AUTOVECTOR:
        return 0;
    case IACK_VECTORED:
        return (uint8_t)CUSTOM_VECTOR_NUM;
    case IACK_SPURIOUS:
        mc68k_signal_bus_error(t->cpu);
        return 0;
    case IACK_BERR_PRIORITY:
        mc68k_signal_bus_error(t->cpu);
        return (uint8_t)CUSTOM_VECTOR_NUM;
    default:
        return 0;
    }
}

static mc68k_state_t *make_cpu(test_ctx_t *t, iack_mode_t mode) {
    t->mode = mode;
    mc68k_bus_t bus = {
        .read_byte = tb_read_byte,
        .read_word = tb_read_word,
        .read_long = tb_read_long,
        .write_byte = tb_write_byte,
        .write_word = tb_write_word,
        .write_long = tb_write_long,
        .sync = tb_sync,
        .write_hook = tb_write_hook,
        .iack = tb_iack,
        .reset_peripheral = NULL,
        .ctx = t,
    };
    t->cpu = mc68k_create(&bus);
    return t->cpu;
}

/* 共通セットアップ: リセットベクタ + 3種のハンドラ（すべて自己ループ）+
 * メインプログラム（割り込みマスクを0に下げて BRA * で待機）。 */
static void setup_common(uint8_t *ram) {
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 24u * 4u, SPURIOUS_HANDLER);
    write_long_be(ram, CUSTOM_VECTOR_NUM * 4u, VECTORED_HANDLER);
    write_long_be(ram, (24u + IRQ_LEVEL) * 4u, AUTOVECTOR_HANDLER);

    write_word_be(ram, SPURIOUS_HANDLER, 0x60FE);   /* BRA * */
    write_word_be(ram, VECTORED_HANDLER, 0x60FE);   /* BRA * */
    write_word_be(ram, AUTOVECTOR_HANDLER, 0x60FE); /* BRA * */

    /* ANDI.W #0xF8FF, SR ; BRA * — マスクを0に下げて IRQ を受け付ける */
    write_word_be(ram, RESET_PC + 0, 0x027C);
    write_word_be(ram, RESET_PC + 2, 0xF8FF);
    write_word_be(ram, RESET_PC + 4, 0x60FE);
}

/* ================================================================== */
/* スプリアス割り込み: IACK中にバスエラー → ベクタ24固定                 */
/* ================================================================== */

static void test_spurious(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);

    mc68k_state_t *cpu = make_cpu(&t, IACK_SPURIOUS);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, IRQ_LEVEL);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == SPURIOUS_HANDLER);
    assert(r.a[7] == RESET_SSP - 6); /* 標準割り込みフレーム(PC+SR、6バイト) */
}

/* ================================================================== */
/* 回帰: ベクタドインタラプト（バスエラーなし）                          */
/* ================================================================== */

static void test_iack_vectored_ok(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);

    mc68k_state_t *cpu = make_cpu(&t, IACK_VECTORED);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, IRQ_LEVEL);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VECTORED_HANDLER);
}

/* ================================================================== */
/* 回帰: オートベクタ（iack が 0 を返す、バスエラーなし）                 */
/* ================================================================== */

static void test_iack_autovector_ok(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);

    mc68k_state_t *cpu = make_cpu(&t, IACK_AUTOVECTOR);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, IRQ_LEVEL);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == AUTOVECTOR_HANDLER);
}

/* ================================================================== */
/* バスエラー優先: 非0ベクタを返してもバスエラー通知があればスプリアス優先 */
/* ================================================================== */

static void test_iack_berr_priority(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);

    mc68k_state_t *cpu = make_cpu(&t, IACK_BERR_PRIORITY);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, IRQ_LEVEL);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == SPURIOUS_HANDLER); /* CUSTOM_VECTOR_NUM のハンドラではない */
}

int main(void) {
    test_spurious();
    test_iack_vectored_ok();
    test_iack_autovector_ok();
    test_iack_berr_priority();
    return 0;
}
