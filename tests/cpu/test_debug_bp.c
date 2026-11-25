/*
 * test_debug_bp.c — アドレス比較型ブレークポイント
 * （mc68k_add_breakpoint/remove/enable/clear/hit）のユニットテスト
 * 。
 *
 * 再開セマンティクス（同一PCへの無限再ヒット回避）は「一時無効化→1命令→
 * 再有効化」のような特別な手順を使わず、「まず mc68k_execute(cpu,1) で
 * 1命令実行し、その後で mc68k_breakpoint_hit() を呼ぶ」という順序規約
 * だけで実現される設計（debug.h参照、Opus監査で命令パッチ方式の遺構と
 * 判断され簡素化）。テストもこの順序を守って書く。
 *
 * 検証内容:
 *   1. 未登録時は常にヒットなし（-1）
 *   2. 登録アドレスへPCが到達したときだけヒットする
 *   3. 無効化（enable=0）中はヒットしない、再有効化で再びヒットする
 *   4. 条件付きブレークポイント（condが0を返す間はヒットしない）
 *   5. MC68K_MAX_BREAKPOINTS を超える登録は -1
 *   6. remove後はヒットしない、clearで全消去できる
 *   7. 奇数アドレスは登録拒否
 *
 * 各テストは exec_state が CPU_NORMAL のまま留まる単純な NOP 列のみを使い、
 * STOP/例外分岐（mc68k_execute(cpu,1) が特殊な挙動をする経路）には
 * 触れない。
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
    return mc68k_create(&bus);
}

/* RESET_PC から N 個の NOP を敷き詰めた最小限のプログラムを用意する。 */
static mc68k_state_t *make_cpu_with_nops(test_ctx_t *t, int n) {
    memset(t->ram, 0, sizeof(t->ram));
    write_long_be(t->ram, 0x000000, RESET_SSP);
    write_long_be(t->ram, 0x000004, RESET_PC);
    for (int i = 0; i < n; i++)
        wr_w(t, RESET_PC + (uint32_t)i * 2, NOP_OP);

    mc68k_state_t *cpu = make_cpu(t);
    mc68k_reset(cpu);
    return cpu;
}

/* 未登録時は常にヒットなし */
static void test_no_hit_without_registration(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu_with_nops(&t, 4);

    assert(mc68k_breakpoint_hit(cpu) == -1);
    mc68k_execute(cpu, 1);
    assert(mc68k_get_exec_state(cpu) == CPU_NORMAL);
    assert(mc68k_breakpoint_hit(cpu) == -1);

    mc68k_destroy(cpu);
}

/*
 * 登録アドレスへPCが到達したときだけヒットする。
 * 3命令目（RESET_PC+4）にBPを置き、1命令ずつ実行しながら判定する。
 */
static void test_hit_at_registered_address(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu_with_nops(&t, 4);

    int id = mc68k_add_breakpoint(cpu, RESET_PC + 4, NULL, NULL);
    assert(id >= 0);

    mc68k_execute(cpu, 1); /* PC: RESET_PC -> RESET_PC+2 */
    assert(mc68k_breakpoint_hit(cpu) == -1);

    mc68k_execute(cpu, 1); /* PC: RESET_PC+2 -> RESET_PC+4 (BPアドレス) */
    assert(mc68k_breakpoint_hit(cpu) == id);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == RESET_PC + 4);

    mc68k_destroy(cpu);
}

/* 無効化中はヒットしない、再有効化で再びヒットする */
static void test_enable_disable(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu_with_nops(&t, 4);

    int id = mc68k_add_breakpoint(cpu, RESET_PC + 2, NULL, NULL);
    mc68k_enable_breakpoint(cpu, id, 0);

    mc68k_execute(cpu, 1);                   /* PC -> RESET_PC+2 */
    assert(mc68k_breakpoint_hit(cpu) == -1); /* 無効化中なのでヒットしない */

    mc68k_enable_breakpoint(cpu, id, 1);
    assert(mc68k_breakpoint_hit(cpu) == id); /* 再有効化後、同じPCで再びヒット */

    mc68k_destroy(cpu);
}

/* 条件付きブレークポイント: condが0を返す間はヒットしない */
static int cond_false_until(mc68k_state_t *cpu, void *ctx) {
    (void)cpu;
    int *counter = (int *)ctx;
    (*counter)++;
    return *counter >= 3; /* 3回目の評価で初めてヒット扱いにする */
}

static void test_conditional_breakpoint(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu_with_nops(&t, 2);
    int counter = 0;

    int id = mc68k_add_breakpoint(cpu, RESET_PC, cond_false_until, &counter);
    assert(id >= 0);

    assert(mc68k_breakpoint_hit(cpu) == -1); /* counter=1 */
    assert(mc68k_breakpoint_hit(cpu) == -1); /* counter=2 */
    assert(mc68k_breakpoint_hit(cpu) == id); /* counter=3 でヒット */

    mc68k_destroy(cpu);
}

/* MC68K_MAX_BREAKPOINTS を超える登録は -1 */
static void test_max_breakpoints(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu_with_nops(&t, 1);

    for (int i = 0; i < MC68K_MAX_BREAKPOINTS; i++) {
        int id = mc68k_add_breakpoint(cpu, RESET_PC + (uint32_t)i * 2, NULL, NULL);
        assert(id == i);
    }
    assert(mc68k_add_breakpoint(cpu, RESET_PC + 999 * 2, NULL, NULL) == -1);

    mc68k_destroy(cpu);
}

/* remove後はヒットしない、clearで全消去できる */
static void test_remove_and_clear(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu_with_nops(&t, 4);

    int id_a = mc68k_add_breakpoint(cpu, RESET_PC, NULL, NULL);
    int id_b = mc68k_add_breakpoint(cpu, RESET_PC + 2, NULL, NULL);
    (void)id_b;

    assert(mc68k_breakpoint_hit(cpu) == id_a);
    mc68k_remove_breakpoint(cpu, id_a);
    assert(mc68k_breakpoint_hit(cpu) == -1);

    mc68k_execute(cpu, 1); /* PC -> RESET_PC+2 */
    assert(mc68k_breakpoint_hit(cpu) == id_b);

    mc68k_clear_breakpoints(cpu);
    assert(mc68k_breakpoint_hit(cpu) == -1);

    /* clear後、スロットが再利用できることも確認する */
    int id_c = mc68k_add_breakpoint(cpu, RESET_PC + 2, NULL, NULL);
    assert(id_c >= 0);
    assert(mc68k_breakpoint_hit(cpu) == id_c);

    mc68k_destroy(cpu);
}

/* 奇数アドレスは登録拒否 */
static void test_odd_address_rejected(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu_with_nops(&t, 1);

    assert(mc68k_add_breakpoint(cpu, RESET_PC + 1, NULL, NULL) == -1);

    mc68k_destroy(cpu);
}

int main(void) {
    test_no_hit_without_registration();
    test_hit_at_registered_address();
    test_enable_disable();
    test_conditional_breakpoint();
    test_max_breakpoints();
    test_remove_and_clear();
    test_odd_address_rejected();
    return 0;
}
