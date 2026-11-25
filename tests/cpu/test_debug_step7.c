/*
 * test_debug_step7.c — ウォッチポイント・簡易バックトレース・n命令後停止
 * カウンタ・reset-and-haltのユニットテスト。
 *
 * 検証内容:
 *   1. ウォッチポイント: 値変化でヒットする、変化なしではヒットしない、
 *      mc68k_get_watchpoint_hit()で変化前後の値が取れる、
 *      mc68k_debug_run()がMC68K_STOP_WATCHPOINTで停止する
 *   2. mc68k_reset()がウォッチポイントの前回値を再取得する（偽ヒット防止）
 *   3. コールスタックリング: BSR/JSR/RTS実行後にmc68k_dump_call_ring()が
 *      クラッシュしないこと（出力内容はstderr行なので直接検証はしない）
 *   4. n命令後停止カウンタ: 指定命令数だけ進む、BP等が先に成立すれば
 *      早期に打ち切る
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
#define NOP_OP    0x4E71u

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
static uint8_t peek_b(void *ctx, uint32_t addr) {
    uint8_t v = 0;
    rd_b(ctx, addr, &v);
    return v;
}
static uint16_t peek_w(void *ctx, uint32_t addr) {
    uint16_t v = 0;
    rd_w(ctx, addr, &v);
    return v;
}
static uint32_t peek_l(void *ctx, uint32_t addr) {
    uint32_t v = 0;
    rd_l(ctx, addr, &v);
    return v;
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t val) {
    ram[addr + 0] = (uint8_t)(val >> 24);
    ram[addr + 1] = (uint8_t)(val >> 16);
    ram[addr + 2] = (uint8_t)(val >> 8);
    ram[addr + 3] = (uint8_t)(val >> 0);
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
    mc68k_state_t *cpu = mc68k_create(&bus);
    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, t);
    return cpu;
}

/* 1. ウォッチポイント基本動作: CLR.W $2000 で値が変化しヒットする */
static void test_watchpoint_hit_on_change(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    wr_w(&t, 0x2000, 0x1234); /* 監視対象の初期値 */
    wr_w(&t, RESET_PC, NOP_OP);

    /* CLR.W $2000.W : 0x4278, ext=0x2000 */
    wr_w(&t, RESET_PC + 2, 0x4278);
    wr_w(&t, RESET_PC + 4, 0x2000);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);

    int wp = mc68k_add_watchpoint(cpu, 0x2000, 2);
    assert(wp >= 0);
    assert(mc68k_watchpoint_check(cpu) == -1); /* 登録直後はまだ変化なし */

    mc68k_debug_run(cpu, 1000);

    assert(mc68k_get_stop_reason(cpu) == MC68K_STOP_WATCHPOINT);
    uint32_t oldv = 0xFFFFFFFF, newv = 0xFFFFFFFF;
    assert(mc68k_get_watchpoint_hit(cpu, &oldv, &newv) == wp);
    assert(oldv == 0x1234);
    assert(newv == 0x0000);

    mc68k_destroy(cpu);
}

/* 2. 変化がなければヒットしない */
static void test_watchpoint_no_hit_without_change(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    wr_w(&t, 0x2000, 0xABCD);
    for (int i = 0; i < 4; i++)
        wr_w(&t, RESET_PC + (uint32_t)i * 2, NOP_OP);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);

    int wp = mc68k_add_watchpoint(cpu, 0x2000, 2);
    assert(wp >= 0);

    mc68k_debug_run(cpu, 8); /* NOPを数回進めるだけ */

    assert(mc68k_get_stop_reason(cpu) != MC68K_STOP_WATCHPOINT);
    assert(mc68k_get_watchpoint_hit(cpu, NULL, NULL) == -1);

    mc68k_destroy(cpu);
}

/* 3. mc68k_reset()が前回値を再取得し、リセット直後の偽ヒットを防ぐ */
static void test_reset_rearms_watchpoint(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    wr_w(&t, 0x2000, 0x1111);
    wr_w(&t, RESET_PC, NOP_OP);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    int wp = mc68k_add_watchpoint(cpu, 0x2000, 2);
    assert(wp >= 0);

    /* リセット後にメモリ内容が変わる状況を模す（ホストがROM再ロード等） */
    wr_w(&t, 0x2000, 0x9999);
    mc68k_reset(cpu); /* ここで前回値が0x9999へ再取得されるはず */

    assert(mc68k_watchpoint_check(cpu) == -1); /* 偽ヒットしない */

    mc68k_destroy(cpu);
}

/* 4. remove/clear/enable-disable */
static void test_watchpoint_lifecycle(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    wr_b(&t, 0x3000, 0x00);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);

    int wp = mc68k_add_watchpoint(cpu, 0x3000, 1);
    assert(wp >= 0);
    mc68k_enable_watchpoint(cpu, wp, 0);
    wr_b(&t, 0x3000, 0xFF);
    assert(mc68k_watchpoint_check(cpu) == -1); /* 無効化中はヒットしない */

    mc68k_enable_watchpoint(cpu, wp, 1);
    assert(mc68k_watchpoint_check(cpu) == wp); /* 再有効化で検出 */

    mc68k_remove_watchpoint(cpu, wp);
    wr_b(&t, 0x3000, 0x11);
    assert(mc68k_watchpoint_check(cpu) == -1);

    int wp2 = mc68k_add_watchpoint(cpu, 0x3000, 1);
    assert(wp2 >= 0);
    mc68k_clear_watchpoints(cpu);
    wr_b(&t, 0x3000, 0x22);
    assert(mc68k_watchpoint_check(cpu) == -1);

    mc68k_destroy(cpu);
}

/* 5. 不正な引数の拒否 */
static void test_watchpoint_invalid_args(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);

    assert(mc68k_add_watchpoint(cpu, 0x2000, 3) == -1); /* size不正 */
    assert(mc68k_add_watchpoint(cpu, 0x2001, 2) == -1); /* 奇数アドレス+word */
    assert(mc68k_add_watchpoint(cpu, 0x2001, 1) >= 0);  /* byteなら奇数OK */

    mc68k_destroy(cpu);
}

/* peek未登録時はaddが失敗する */
static void test_watchpoint_requires_peek(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);

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
        .ctx = &t,
    };
    mc68k_state_t *cpu = mc68k_create(&bus); /* peek未登録 */
    mc68k_reset(cpu);

    assert(mc68k_add_watchpoint(cpu, 0x2000, 2) == -1);

    mc68k_destroy(cpu);
}

/* 6. 簡易バックトレース: BSR/JSR/RTS実行後、ダンプがクラッシュしない */
static void test_call_ring_dump_does_not_crash(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);

    /* RESET_PC:   BSR.S sub
     * RESET_PC+2: BRA.S * （戻り先。自己ループで安全に停止させる）
     * sub (RESET_PC+4): RTS
     * mc68k_debug_run_n(cpu,2) で「BSR実行→RTS実行」の2命令だけに厳密に
     * 絞る（cycles予算方式のdebug_run()だと自己ループに入った後も予算を
     * 消費し続けてしまうため）。 */
    wr_w(&t, RESET_PC, 0x6102);     /* BSR.S +2 -> sub */
    wr_w(&t, RESET_PC + 2, 0x60FE); /* BRA.S * (自己ループ) */
    wr_w(&t, RESET_PC + 4, 0x4E75); /* sub: RTS */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);

    mc68k_debug_run_n(cpu, 2); /* BSR実行 -> RTS実行 */

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == RESET_PC + 2); /* RTSでBSRの戻り先へ復帰済み */

    mc68k_dump_call_ring(cpu, 0);
    mc68k_dump_call_ring(cpu, 10);

    mc68k_destroy(cpu);
}

/* 7. n命令後停止カウンタ: 指定数だけ進む */
static void test_debug_run_n_basic(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    for (int i = 0; i < 10; i++)
        wr_w(&t, RESET_PC + (uint32_t)i * 2, NOP_OP);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);

    mc68k_debug_run_n(cpu, 3);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == RESET_PC + 6); /* NOP3個分 */
    assert(mc68k_get_stop_reason(cpu) == MC68K_STOP_NONE);

    mc68k_destroy(cpu);
}

/* 8. n命令後停止カウンタ: BPが先に成立すれば打ち切る */
static void test_debug_run_n_stops_at_breakpoint(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    for (int i = 0; i < 10; i++)
        wr_w(&t, RESET_PC + (uint32_t)i * 2, NOP_OP);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_add_breakpoint(cpu, RESET_PC + 4, NULL, NULL);

    mc68k_debug_run_n(cpu, 100); /* BPが先に成立するはず */

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == RESET_PC + 4);
    assert(mc68k_get_stop_reason(cpu) == MC68K_STOP_BREAKPOINT);

    mc68k_destroy(cpu);
}

/* num_instructions<=0 は何もしない */
static void test_debug_run_n_zero_or_negative(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu(&t);
    memset(t.ram, 0, sizeof(t.ram));
    write_long_be(t.ram, 0x000000, RESET_SSP);
    write_long_be(t.ram, 0x000004, RESET_PC);
    wr_w(&t, RESET_PC, NOP_OP);
    mc68k_reset(cpu);

    assert(mc68k_debug_run_n(cpu, 0) == 0);
    assert(mc68k_debug_run_n(cpu, -5) == 0);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == RESET_PC); /* 何も進んでいない */

    mc68k_destroy(cpu);
}

int main(void) {
    test_watchpoint_hit_on_change();
    test_watchpoint_no_hit_without_change();
    test_reset_rearms_watchpoint();
    test_watchpoint_lifecycle();
    test_watchpoint_invalid_args();
    test_watchpoint_requires_peek();
    test_call_ring_dump_does_not_crash();
    test_debug_run_n_basic();
    test_debug_run_n_stops_at_breakpoint();
    test_debug_run_n_zero_or_negative();
    return 0;
}
