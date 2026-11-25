/*
 * test_irq_pull.c — pull型IRQ受付（mc68k_set_irq_level_hook）のユニットテスト
 *
 * : CPUコアのIRQ受付をpull型（命令境界でIRQレベルを問い合わせ）へ
 * 移行できるようにする。本ファイルは以下を検証する:
 *
 *   - pullフックがlatch（pending_irq）より優先されること（#123の再現含む）
 *   - レベル7(NMI)がマスクを貫通しつつ、pull型では割り込み嵐を起こさないこと
 *     （nmi_takenエッジ検出）、線が下がれば再アサート可能なこと
 *   - サンプルしたレベルがマスク判定・iack・ベクタへ一貫して伝わること
 *   - STOP待機中の二重サンプルでエッジを失わないこと
 *   - iack空振り→スプリアス割り込み、ベクタドインタラプトがpull経路でも
 *     従来通り動くこと
 *   - 値域外の戻り値がfail-safe（要求なし=0）で無効化され、有効な割り込み
 *     （特にNMI）へ化けないこと
 *   - フックの実行時解除でlatch型へ戻ること
 *   - latch型（フック未登録）はnmi_takenの影響を一切受けず、
 *     mc68k_execute(cpu,1)相当の刻みでも従来通り再受付されること
 *   - iackによるホスト側状態クリアが次のサンプルに反映され、
 *     古い要求を再受付しないこと
 *
 * tests/cpu/test_spurious_irq.c を雛形にする（pullフックとiackを同時に
 * 差し込む必要があるため、bus_make_hooks()ではなく自前バスコールバックを
 * 持つ構成が適切）。既存の test_irq.c / test_spurious_irq.c は無変更のまま
 * 全緑であることが、フック未登録時に非pull利用者への影響が一切ないことの
 * 最強の証拠になる（本ファイルでは重複させない）。
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
#define CUSTOM_VECTOR_NUM  16u
#define SR_READOUT_HANDLER 0x00001700u /* MOVE SR,D2; RTE */
#define COUNT_HANDLER      0x00001720u /* ADDQ.W #1,D3; RTE */

/* level 1-7 のオートベクタハンドラ（すべて自己ループ）。levelごとに
 * アドレスを分けて「どのベクタが取られたか」をPCだけで判別できるようにする。 */
#define AUTOVEC_HANDLER(lv) (0x00001640u + (uint32_t)(lv) * 0x10u)

#define MASK0_LOOP_PC (RESET_PC + 4u) /* prog_mask0() 使用時、マスク0で待機するBRA*の位置 */
#define BRA_LOOP_PC   (RESET_PC)      /* prog_bra() 使用時、マスク7のまま待機するBRA*の位置 */

typedef struct {
    uint8_t ram[RAM_SIZE];
    mc68k_state_t *cpu;

    /* pullフックの戻り値・呼び出し回数 */
    int level;
    int call_count;

    /* iackの記録・挙動切り替え */
    int iack_level_seen;
    bool iack_berr;     /* true: スプリアス割り込みへ倒す */
    bool iack_vectored; /* true: CUSTOM_VECTOR_NUM を返す（falseならオートベクタ=0） */

    /* test_iack_invalidation 専用: ホスト側デバイスの要求状態を模す。
     * iackコールバックが（berrでない限り）これをfalseへ落とす。 */
    bool host_asserted;
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

/* IACK: レベルを記録し、iack_berr/iack_vectored に応じて出し分ける。
 * host_asserted は berr でない限り常にクリアする（test_iack_invalidation
 * 以外のテストでは誰も読まないフィールドなので無害）。 */
static uint8_t tb_iack(void *ctx, int level) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    t->iack_level_seen = level;
    if (t->iack_berr) {
        mc68k_signal_bus_error(t->cpu);
        return 0;
    }
    t->host_asserted = false;
    if (t->iack_vectored)
        return (uint8_t)CUSTOM_VECTOR_NUM;
    return 0; /* オートベクタ */
}

/* pullフック: t->level をそのまま返す（呼び出し回数を記録する） */
static int tb_irq_level(void *ctx) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    t->call_count++;
    return t->level;
}

/* pullフック（ゲート付き）: host_asserted が false ならレベルを一切要求しない。
 * test_iack_invalidation 専用: iackがhost_asserted=falseに落とすと、
 * 次のサンプルから自動的に「要求なし」へ切り替わることを検証する。 */
static int tb_irq_level_gated(void *ctx) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    t->call_count++;
    return t->host_asserted ? t->level : 0;
}

static mc68k_state_t *make_cpu(test_ctx_t *t) {
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

/* 共通セットアップ: リセットベクタ + スプリアス/カスタム/level1-7オートベクタの
 * ハンドラ（すべて自己ループ）。テストごとに必要なベクタ先を個別に上書きする。 */
static void setup_common(uint8_t *ram) {
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 24u * 4u, SPURIOUS_HANDLER);
    write_long_be(ram, CUSTOM_VECTOR_NUM * 4u, VECTORED_HANDLER);
    write_word_be(ram, SPURIOUS_HANDLER, 0x60FE); /* BRA * */
    write_word_be(ram, VECTORED_HANDLER, 0x60FE); /* BRA * */
    for (int lv = 1; lv <= 7; lv++) {
        write_long_be(ram, (uint32_t)(24 + lv) * 4u, AUTOVEC_HANDLER(lv));
        write_word_be(ram, AUTOVEC_HANDLER(lv), 0x60FE); /* BRA * */
    }
}

/* メインプログラム: マスクを0に下げてから待機（level 1-6 を受け付けたい場合） */
static void prog_mask0(uint8_t *ram) {
    write_word_be(ram, RESET_PC + 0, 0x027C); /* ANDI.W #0xF8FF, SR */
    write_word_be(ram, RESET_PC + 2, 0xF8FF);
    write_word_be(ram, RESET_PC + 4, 0x60FE); /* BRA * */
}

/* メインプログラム: マスク7のまま待機（NMI=level7のみ通す想定） */
static void prog_bra(uint8_t *ram) {
    write_word_be(ram, RESET_PC + 0, 0x60FE); /* BRA * */
}

/* ================================================================== */
/* 1. pullのみで基本受付が成立する（mc68k_set_irqを一度も呼ばない）        */
/* ================================================================== */

static void test_pull_basic_accept(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    t.level = 3;
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == AUTOVEC_HANDLER(3));
    assert(t.call_count > 0);
}

/* ================================================================== */
/* 2. pullがlatchの0より優先される                                       */
/* ================================================================== */

static void test_pull_overrides_latch_zero(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    mc68k_set_irq(cpu, 0); /* latchは0のまま */
    t.level = 3;
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == AUTOVEC_HANDLER(3));
}

/* ================================================================== */
/* 3. pullが古いlatch値より優先される（#123の再現: 古いIRQ線を受理しない） */
/* ================================================================== */

static void test_pull_overrides_stale_latch(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    mc68k_set_irq(cpu, 6); /* 古いラッチ値が残っている想定（push型なら受理してしまう） */
    t.level = 0;           /* 実際のIRQ線は既に下がっている */
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == MASK0_LOOP_PC); /* どのハンドラにも入らず主プログラムのループに留まる */
}

/* ================================================================== */
/* 4. レベル7(NMI)はマスク7を貫通する                                    */
/* ================================================================== */

static void test_nmi_bypasses_mask(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_bra(t.ram); /* マスクは7のまま */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    t.level = 7;
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == AUTOVEC_HANDLER(7));
}

/* ================================================================== */
/* 5. レベル7が張り付いていても割り込み嵐にならない（nmi_taken）           */
/* ================================================================== */

static void test_nmi_storm_prevented(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_bra(t.ram);
    /* level7のベクタ先をCOUNT_HANDLER（ADDQ.W #1,D3; RTE）へ差し替える */
    write_long_be(t.ram, (24u + 7u) * 4u, COUNT_HANDLER);
    write_word_be(t.ram, COUNT_HANDLER + 0, 0x5243); /* ADDQ.W #1, D3 */
    write_word_be(t.ram, COUNT_HANDLER + 2, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    t.level = 7; /* 線を7に張り付けたまま変化させない */
    mc68k_execute(cpu, 500);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    /* nmi_takenが無ければハンドラの最初の命令すら実行されずD3は0のまま、
     * かつaccept_irq()が命令境界のたびに空撃ちしてSSPが際限なく減り続ける。 */
    assert((r.d[3] & 0xFFFF) == 1); /* ハンドラは正確に1回だけ完走した */
    assert(r.pc == BRA_LOOP_PC);    /* RTE後、主プログラムのループへ戻っている */
    assert(r.a[7] == RESET_SSP);    /* 1回分のフレームがRTEで正しく後始末されている */
}

/* ================================================================== */
/* 6. NMIは線が7未満に下がれば再アサート可能になる                        */
/* ================================================================== */

static void test_nmi_reassert_after_drop(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_bra(t.ram);
    write_long_be(t.ram, (24u + 7u) * 4u, COUNT_HANDLER);
    write_word_be(t.ram, COUNT_HANDLER + 0, 0x5243); /* ADDQ.W #1, D3 */
    write_word_be(t.ram, COUNT_HANDLER + 2, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);

    t.level = 7;
    mc68k_execute(cpu, 200); /* 1回目の受付: D3 == 1 */

    t.level = 0; /* 線を下げる: nmi_takenがクリアされる */
    mc68k_execute(cpu, 50);

    t.level = 7; /* 再度上げる: 再受付されるはず */
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[3] & 0xFFFF) == 2); /* 2回受付された */
    assert(r.pc == BRA_LOOP_PC);
    assert(r.a[7] == RESET_SSP);
}

/* ================================================================== */
/* 7. サンプルしたレベルがSR/iack/ベクタへ一貫して伝わる                   */
/* ================================================================== */

static void test_sampled_level_consistent(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);
    /* level5のベクタ先をSR_READOUT_HANDLER（MOVE SR,D2; RTE）へ差し替える */
    write_long_be(t.ram, (24u + 5u) * 4u, SR_READOUT_HANDLER);
    write_word_be(t.ram, SR_READOUT_HANDLER + 0, 0x40C2); /* MOVE SR, D2 */
    write_word_be(t.ram, SR_READOUT_HANDLER + 2, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    t.level = 5;
    mc68k_execute(cpu, 500);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(t.iack_level_seen == 5);
    assert((r.d[2] & 0xFF00) == 0x2500); /* ハンドラ内SR: S=1,マスク=5 */
}

/* ================================================================== */
/* 8. STOP待機からの再開（受け付ける側）                                  */
/* ================================================================== */

static void test_stop_release_accept(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    write_word_be(t.ram, RESET_PC + 0, 0x4E72); /* STOP #imm */
    write_word_be(t.ram, RESET_PC + 2, 0x2000); /* S=1, マスク=0 */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);

    t.level = 0;
    mc68k_execute(cpu, 50); /* STOPを実行してCPU_STOPPEDへ */
    assert(mc68k_get_exec_state(cpu) == CPU_STOPPED);

    t.level = 4; /* マスク0 < level4 → 解除されるはず */
    mc68k_execute(cpu, 300);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    cpu_exec_state_t st = mc68k_get_exec_state(cpu);
    mc68k_destroy(cpu);

    assert(st == CPU_NORMAL);
    assert(r.pc == AUTOVEC_HANDLER(4));
}

/* ================================================================== */
/* 9. STOP待機からの再開（受け付けない側 + 二重サンプルでもエッジを失わない） */
/* ================================================================== */

static void test_stop_release_reject_then_nmi(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    write_word_be(t.ram, RESET_PC + 0, 0x4E72); /* STOP #imm */
    write_word_be(t.ram, RESET_PC + 2, 0x2700); /* S=1, マスク=7 */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);

    t.level = 0;
    mc68k_execute(cpu, 50); /* STOPを実行 */
    assert(mc68k_get_exec_state(cpu) == CPU_STOPPED);

    t.level = 6; /* マスク7 < level6ではないので解除されない */
    mc68k_execute(cpu, 50);
    assert(mc68k_get_exec_state(cpu) == CPU_STOPPED);

    t.level = 7; /* NMIはマスク7を貫通するので解除される */
    mc68k_execute(cpu, 300);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    cpu_exec_state_t st = mc68k_get_exec_state(cpu);
    mc68k_destroy(cpu);

    assert(st == CPU_NORMAL);
    assert(r.pc == AUTOVEC_HANDLER(7)); /* 二重サンプル経路でも正しく1回受け付けられている */
}

/* ================================================================== */
/* 10. iack空振り → スプリアス割り込み（pull経路でも従来通り）            */
/* ================================================================== */

static void test_spurious_under_pull(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    t.level = 6;
    t.iack_berr = true;
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == SPURIOUS_HANDLER);
    assert(t.iack_level_seen == 6);
}

/* ================================================================== */
/* 11. ベクタドインタラプト（pull経路でも従来通り）                        */
/* ================================================================== */

static void test_vectored_under_pull(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    t.level = 3;
    t.iack_vectored = true;
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == VECTORED_HANDLER);
    assert(t.iack_level_seen == 3);
}

/* ================================================================== */
/* 12. 値域外はfail-safeで無効化される（有効な割り込みへ化けない）         */
/* ================================================================== */

static void test_out_of_range_high(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    t.level = 9; /* &7なら誤ってlevel1に化ける値。fail-safeなら常に無視される */
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == MASK0_LOOP_PC); /* どのハンドラにも入っていない */
}

static void test_out_of_range_negative(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    t.level = -1; /* &7なら誤ってlevel7(NMI)に化ける値。fail-safeなら常に無視される */
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == MASK0_LOOP_PC);
}

/* ================================================================== */
/* 13. フックの実行時解除でlatch型へ戻る                                  */
/* ================================================================== */

static void test_hook_unregister_falls_back_to_latch(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level, &t);
    t.level = 3;
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == AUTOVEC_HANDLER(3));
    int call_count_before = t.call_count;

    /* フックを解除。以後は呼ばれない・latchのみが有効になるはず */
    mc68k_set_irq_level_hook(cpu, NULL, NULL);
    t.level = 6; /* フックがまだ生きていれば誤って受け付けてしまう値 */
    mc68k_execute(cpu, 100);
    mc68k_get_regs(cpu, &r);
    assert(t.call_count == call_count_before); /* 解除後は一度も呼ばれていない */
    assert(r.pc == AUTOVEC_HANDLER(3));        /* level3の自己ループのまま変化なし */

    /* latchが機能を取り戻していることを確認する */
    mc68k_set_irq(cpu, 5);
    mc68k_execute(cpu, 200);
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.pc == AUTOVEC_HANDLER(5));
}

/* ================================================================== */
/* 14. latch型はnmi_takenの影響を受けない                                */
/*     （mc68k_execute(cpu,1)刻みでも、レビュー指摘の通り従来どおり         */
/*      再受付されることを確認する。フック未登録＝純粋なlatchモード）       */
/* ================================================================== */

static void test_latch_cycles1_nmi_reaccept(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_bra(t.ram); /* マスク7のまま */
    write_long_be(t.ram, (24u + 7u) * 4u, COUNT_HANDLER);
    write_word_be(t.ram, COUNT_HANDLER + 0, 0x5243); /* ADDQ.W #1, D3 */
    write_word_be(t.ram, COUNT_HANDLER + 2, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    /* mc68k_set_irq_level_hook() は一度も呼ばない: 純粋なlatchモード */

    mc68k_set_irq(cpu, 7);
    mc68k_execute(cpu, 1); /* 1サイクル要求でも受付自体は完了する（既存の仕様） */

    mc68k_set_irq(cpu, 7); /* ホストが再度NMIを発行 */
    mc68k_execute(cpu, 1);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    /* nmi_takenがlatchモードに誤って作用していれば2回目が抑止され、
     * SSPは1フレーム分（6バイト）しか減らない。2回受け付けられていれば
     * 12バイト分減る。 */
    assert(r.a[7] == RESET_SSP - 12u);
}

/* ================================================================== */
/* 15. iackによるホスト側状態クリアが次のサンプルに反映される              */
/*     （キャッシュを持たない初期実装の回帰。将来キャッシュ化する際の         */
/*      基準としても機能する）                                            */
/* ================================================================== */

static void test_iack_invalidation_no_stale_reaccept(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    setup_common(t.ram);
    prog_mask0(t.ram);

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_set_irq_level_hook(cpu, tb_irq_level_gated, &t);
    t.level = 6;
    t.host_asserted = true; /* デバイスの要求が立っている */
    mc68k_execute(cpu, 300);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    /* iack (tb_iack) が host_asserted を落とすため、level6のハンドラへ
     * 一度だけ入り、以後は「要求なし」に切り替わって自己ループに留まる。 */
    assert(r.pc == AUTOVEC_HANDLER(6));
    assert(t.iack_level_seen == 6);
    assert(r.a[7] == RESET_SSP - 6u); /* 1回分のフレームのみ、再受付されていない */
}

int main(void) {
    test_pull_basic_accept();
    test_pull_overrides_latch_zero();
    test_pull_overrides_stale_latch();
    test_nmi_bypasses_mask();
    test_nmi_storm_prevented();
    test_nmi_reassert_after_drop();
    test_sampled_level_consistent();
    test_stop_release_accept();
    test_stop_release_reject_then_nmi();
    test_spurious_under_pull();
    test_vectored_under_pull();
    test_out_of_range_high();
    test_out_of_range_negative();
    test_hook_unregister_falls_back_to_latch();
    test_latch_cycles1_nmi_reaccept();
    test_iack_invalidation_no_stale_reaccept();
    return 0;
}
