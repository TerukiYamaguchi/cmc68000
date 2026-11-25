/*
 * test_debug_peek.c — 非侵襲メモリ読み出しフック（mc68k_set_peek_hooks/
 * mc68k_debug_read_bytes）のユニットテスト。
 *
 * peek系フックは mc68k_bus_t とは独立の debug.h 登録APIとして実装されている
 * （x68000側の一部バスコールバック実装が mc68k_bus_t を指示付き初期化子で
 * ゼロ初期化せず個別代入しており、bus_hooks.h に直接オプショナルフィールドを
 * 追加すると不定値になる危険があるとOpus監査で指摘されたための設計判断）。
 *
 * 検証内容:
 *   1. 未登録時は mc68k_debug_read_bytes() が -1 を返す
 *   2. 登録後、RAM内容を正しく読み出せる
 *   3. 未マップアドレスをpeekしても（規約通り mc68k_signal_bus_error() を
 *      呼ばない実装であれば）ゲスト実行に一切影響しないこと
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "mc68000/debug.h"

#define RAM_SIZE  0x10000u
#define RESET_SSP 0x00008000u
#define RESET_PC  0x00001000u

typedef struct {
    uint8_t ram[RAM_SIZE];
} test_ctx_t;

static bool rd_b(void *ctx, uint32_t addr, uint8_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    *out = (addr < RAM_SIZE) ? t->ram[addr] : 0xFF;
    return addr < RAM_SIZE;
}
static bool rd_w(void *ctx, uint32_t addr, uint16_t *out) {
    uint8_t hi = 0, lo = 0;
    bool ok1 = rd_b(ctx, addr, &hi);
    bool ok2 = rd_b(ctx, addr + 1, &lo);
    *out = (uint16_t)((hi << 8) | lo);
    return ok1 && ok2;
}
static bool rd_l(void *ctx, uint32_t addr, uint32_t *out) {
    uint16_t hi = 0, lo = 0;
    bool ok1 = rd_w(ctx, addr, &hi);
    bool ok2 = rd_w(ctx, addr + 2, &lo);
    *out = ((uint32_t)hi << 16) | lo;
    return ok1 && ok2;
}
static bool wr_b(void *ctx, uint32_t addr, uint8_t v) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr < RAM_SIZE)
        t->ram[addr] = v;
    return addr < RAM_SIZE;
}
static bool wr_w(void *ctx, uint32_t addr, uint16_t v) {
    bool ok1 = wr_b(ctx, addr, (uint8_t)(v >> 8));
    bool ok2 = wr_b(ctx, addr + 1, (uint8_t)v);
    return ok1 && ok2;
}
static bool wr_l(void *ctx, uint32_t addr, uint32_t v) {
    bool ok1 = wr_w(ctx, addr, (uint16_t)(v >> 16));
    bool ok2 = wr_w(ctx, addr + 2, (uint16_t)v);
    return ok1 && ok2;
}
static void sync_cb(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}
static void wh_cb(void *ctx, uint32_t addr, uint32_t size) {
    (void)ctx;
    (void)addr;
    (void)size;
}

/* peek: read_byte等と同じ配列を読むが、未マップ領域でも
 * mc68k_signal_bus_error() を一切呼ばない（非侵襲原則）。 */
static uint8_t peek_b(void *ctx, uint32_t addr) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    return (addr < RAM_SIZE) ? t->ram[addr] : 0xFF;
}
static uint16_t peek_w(void *ctx, uint32_t addr) {
    return (uint16_t)((peek_b(ctx, addr) << 8) | peek_b(ctx, addr + 1));
}
static uint32_t peek_l(void *ctx, uint32_t addr) {
    return ((uint32_t)peek_w(ctx, addr) << 16) | peek_w(ctx, addr + 2);
}

static mc68k_state_t *make_cpu(test_ctx_t *t) {
    mc68k_bus_t bus = {
        .read_byte = rd_b,
        .read_word = rd_w,
        .read_long = rd_l,
        .write_byte = wr_b,
        .write_word = wr_w,
        .write_long = wr_l,
        .sync = sync_cb,
        .write_hook = wh_cb,
        .iack = NULL,
        .reset_peripheral = NULL,
        .ctx = t,
    };
    return mc68k_create(&bus);
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t val) {
    ram[addr + 0] = (uint8_t)(val >> 24);
    ram[addr + 1] = (uint8_t)(val >> 16);
    ram[addr + 2] = (uint8_t)(val >> 8);
    ram[addr + 3] = (uint8_t)(val >> 0);
}

/* 未登録時は -1 を返す */
static void test_unregistered_returns_error(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    mc68k_state_t *cpu = make_cpu(&t);

    uint8_t buf[4];
    assert(mc68k_debug_read_bytes(cpu, 0x2000, buf, sizeof(buf)) == -1);
    assert(mc68k_debug_read_physical_bytes(cpu, 0x2000, buf, sizeof(buf)) == -1);

    mc68k_destroy(cpu);
}

/* 登録後、RAM内容を正しく読み出せる */
static void test_read_matches_ram(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    t.ram[0x2000] = 0xDE;
    t.ram[0x2001] = 0xAD;
    t.ram[0x2002] = 0xBE;
    t.ram[0x2003] = 0xEF;

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, &t);

    uint8_t buf[4] = {0};
    assert(mc68k_debug_read_physical_bytes(cpu, 0x2000, buf, sizeof(buf)) == 0);
    assert(buf[0] == 0xDE && buf[1] == 0xAD && buf[2] == 0xBE && buf[3] == 0xEF);
    memset(buf, 0, sizeof(buf));
    assert(mc68k_debug_read_bytes(cpu, 0x2000, buf, sizeof(buf)) == 0);
    assert(buf[0] == 0xDE && buf[1] == 0xAD && buf[2] == 0xBE && buf[3] == 0xEF);

    mc68k_destroy(cpu);
}

/*
 * 未マップアドレスをpeekしても（mc68k_signal_bus_error()を呼ばない規約通りの
 * 実装であれば）ゲスト実行に一切影響しないことを確認する。
 * peek呼び出し後に通常の1命令（NOP）を実行し、バスエラー例外へ誤って
 * ディスパッチされず正常にPCが2進むことを検証する。
 */
static void test_peek_hole_does_not_affect_execution(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    t.ram[RESET_PC] = 0x4E;
    t.ram[RESET_PC + 1] = 0x71; /* NOP */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, &t);
    mc68k_reset(cpu);

    uint8_t buf[8];
    assert(mc68k_debug_read_bytes(cpu, 0x00800000u /* RAM_SIZE外 = 未マップ */, buf,
                                  sizeof(buf)) == 0);
    assert(mc68k_get_exec_state(cpu) == CPU_NORMAL);

    mc68k_execute(cpu, 4);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == RESET_PC + 2);
    assert(mc68k_get_exec_state(cpu) == CPU_NORMAL);

    mc68k_destroy(cpu);
}

int main(void) {
    test_unregistered_returns_error();
    test_read_matches_ram();
    test_peek_hole_does_not_affect_execution();
    return 0;
}
