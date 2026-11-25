/*
 * test_irq.c — 割り込み処理テスト
 *
 * mc68k_set_irq() → mc68k_execute() 内での受け付け → ハンドラ → RTE の
 * 一連のシーケンスを検証する。
 *
 * オートベクタのベクタアドレス:
 *   level n → vector (24+n) → address (24+n)*4
 *   level 1 → vector 25 → 0x64
 *   level 3 → vector 27 → 0x6C
 *   level 6 → vector 30 → 0x78
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

/* ================================================================== */
/* テスト共通ヘルパー                                                   */
/* ================================================================== */

#define RAM_SIZE  0x10000
#define RESET_SSP 0x00008000
#define RESET_PC  0x00001000

/* IRQ ハンドラを置く領域（プログラム領域と重ならない場所）*/
#define HANDLER_ADDR 0x00001800

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
/* 基本動作 — level 3 IRQ がベクタ経由でジャンプし RTE で復帰する       */
/* ================================================================== */

/*
 * メインプログラムは NOP を実行し続ける。その途中で IRQ level 3 を発行する。
 * ハンドラは D1 に 0x33 をセットして RTE で戻り、メインは BRA * で止まる。
 *
 * level 3 のオートベクタアドレス: (24+3)*4 = 0x6C
 *
 * 期待:
 *   D1 = 0x33（ハンドラが書いた）
 *   A7 = RESET_SSP（スタック正常復帰）
 *   SR のマスクが元の 7 に戻っている（RTE で保存 SR が復元される）
 */
static void test_irq_basic(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    /* level 3 オートベクタ */
    write_long_be(ram, 0x6C, HANDLER_ADDR);

    /*
     * メインプログラム:
     *   ANDI.W #0xF8FF, SR  → 割り込みマスクを 0 に下げる
     *   BRA *               → ここで止まる（IRQ 受付後に RTE で戻る）
     *
     * ANDI.W #imm, SR: 0x027C + imm16
     * 0xF8FF = 1111 1000 1111 1111 → I2/I1/I0 (bits 10-8) をクリア
     */
    write_word_be(ram, RESET_PC + 0, 0x027C); /* ANDI.W #0xF8FF, SR */
    write_word_be(ram, RESET_PC + 2, 0xF8FF);
    write_word_be(ram, RESET_PC + 4, 0x60FE); /* BRA * */

    /* ハンドラ: MOVE.W #0x33, D1; RTE */
    write_word_be(ram, HANDLER_ADDR + 0, 0x323C);
    write_word_be(ram, HANDLER_ADDR + 2, 0x0033);
    write_word_be(ram, HANDLER_ADDR + 4, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, 3);

    /*
     * 100 サイクル実行: ANDI(20) + accept_irq(44) + MOVE.W(8) + RTE(20) = 92 サイクル後、
     * 次のループでも accept_irq が始まる（RTE でマスクが 0 に戻るため）。
     * IRQ をクリアしてから続きを実行し、進行中の accept_irq を RTE で正常終了させる。
     */
    mc68k_execute(cpu, 100);
    mc68k_set_irq(cpu, 0); /* 再トリガーを防ぐ */
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[1] & 0xFFFF) == 0x33);
    assert(r.a[7] == RESET_SSP);
    /* RTE で ANDI 後の SR (0x2000) が復元される */
    assert(r.sr == 0x2000);
}

/* ================================================================== */
/* 割り込み受付時の SR 確認 — マスクレベルが IRQ レベルに更新される      */
/* ================================================================== */

/*
 * ハンドラ内で SR を読み取り、割り込みマスクが level に更新されているか確認する。
 *
 * level 6 → ベクタ (24+6)*4 = 0x78、ハンドラ内 SR マスク = 6
 *
 * ハンドラ内の SR を D2 に保存してから RTE する。
 * MOVE.W (A7), D2 でスタック上の保存 SR ではなく現在の SR を得るには
 * MOVE.W SR, D2 を使う: opcode 0x40C2
 */
static void test_irq_sr_mask(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_long_be(ram, 0x78, HANDLER_ADDR); /* level 6 ベクタ */

    /* メインプログラム: マスクを 0 に下げてから BRA * */
    write_word_be(ram, RESET_PC + 0, 0x027C); /* ANDI.W #0xF8FF, SR */
    write_word_be(ram, RESET_PC + 2, 0xF8FF);
    write_word_be(ram, RESET_PC + 4, 0x60FE); /* BRA * */

    /*
     * ハンドラ: MOVE SR, D2; RTE
     * MOVE SR, Dn: 0x40C0 | dn → MOVE SR, D2 = 0x40C2
     */
    write_word_be(ram, HANDLER_ADDR + 0, 0x40C2); /* MOVE SR, D2 */
    write_word_be(ram, HANDLER_ADDR + 2, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, 6);
    mc68k_execute(cpu, 500);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    /* ハンドラ内 SR: S=1, T=0, マスク=6 → 0x2600 */
    assert((r.d[2] & 0xFF00) == 0x2600);
}

/* ================================================================== */
/* マスク以下の IRQ は無視される                                         */
/* ================================================================== */

/*
 * リセット直後は SR マスク = 7。level 3 の IRQ を発行しても受け付けない。
 * D1 は 0 のまま（ハンドラが実行されない）。
 */
static void test_irq_masked(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_long_be(ram, 0x6C, HANDLER_ADDR); /* level 3 ベクタ（呼ばれないはず）*/

    write_word_be(ram, RESET_PC + 0, 0x60FE); /* BRA * */

    write_word_be(ram, HANDLER_ADDR + 0, 0x323C); /* MOVE.W #0xFF, D1 */
    write_word_be(ram, HANDLER_ADDR + 2, 0x00FF);
    write_word_be(ram, HANDLER_ADDR + 4, 0x4E73);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, 3); /* マスク 7 の下では level 3 は無視 */
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert(r.d[1] == 0); /* ハンドラは実行されていない */
}

/* ================================================================== */
/* IRQ クリア後は再び受け付けない                                        */
/* ================================================================== */

/*
 * マスクを 2 に下げた状態で level 3 を発行 → ハンドラが D1 を 0x11 に設定。
 * その後 mc68k_set_irq(cpu, 0) でクリアし、再度実行しても D1 は変化しない。
 */
static void test_irq_clear(void) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);

    write_long_be(ram, 0x6C, HANDLER_ADDR);

    /*
     * ANDI #0xF8FF, SR でマスクを 0 に下げる（スーパーバイザ命令）。
     * ANDI.W #imm, SR: 0x027C + imm16
     * 0xF8FF = 1111 1000 1111 1111 → I2/I1/I0 を 0 にクリア
     */
    write_word_be(ram, RESET_PC + 0, 0x027C); /* ANDI #0xF8FF, SR */
    write_word_be(ram, RESET_PC + 2, 0xF8FF);
    write_word_be(ram, RESET_PC + 4, 0x60FE); /* BRA * */

    write_word_be(ram, HANDLER_ADDR + 0, 0x323C); /* MOVE.W #0x11, D1 */
    write_word_be(ram, HANDLER_ADDR + 2, 0x0011);
    write_word_be(ram, HANDLER_ADDR + 4, 0x4E73); /* RTE */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, 3);
    mc68k_execute(cpu, 500); /* ANDI → IRQ 受付 → ハンドラ → RTE → BRA * */

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert((r.d[1] & 0xFFFF) == 0x11); /* ハンドラが一度実行された */

    /* IRQ をクリアして再実行: D1 は変化しないはず */
    mc68k_set_irq(cpu, 0);
    mc68k_execute(cpu, 200);
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert((r.d[1] & 0xFFFF) == 0x11); /* 変化なし */
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_irq_basic();
    test_irq_sr_mask();
    test_irq_masked();
    test_irq_clear();
    return 0;
}
