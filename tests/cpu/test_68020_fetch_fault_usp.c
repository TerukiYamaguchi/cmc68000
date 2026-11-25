/*
 * test_68020_fetch_fault_usp.c — オペコードフェッチのバスエラーで USP が壊れないこと
 *
 * 68020+ の dispatch_group0_exception() は「命令を再実行する」方式のため、
 * 例外フレームを積む前に restart_d/restart_a からレジスタを巻き戻す。
 * この退避値は**命令境界**で取らなければならない。
 *
 * オペコードフェッチ成功後にしか退避を取らないと、フェッチ自体がバスエラーに
 * なったときに「1つ前の命令の実行開始時点」の値が復元される。直前が RTE
 * （スーパーバイザ→ユーザ復帰）だと、復元される a[7] はカーネルの SSP であり、
 * 続く cpu_set_sr(saved_sr | SR_S) の S=0→1 スワップがユーザ SP をその値で
 * 上書きしてしまう。
 *
 * 実害: NetBSD/x68k の init が RTE でユーザモードへ戻った直後、デマンド
 * ページング初回の命令フェッチでフォルトした際に USP がカーネルスタックの
 * アドレスへ化け、SIGSEGV の無限ループに陥っていた。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mc68000/bus_hooks.h"
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"

#define RAM_SIZE        0x10000u
#define RESET_SSP       0x00008000u
#define RESET_PC        0x00001000u
#define BUS_ERR_HANDLER 0x00000300u
#define USER_SP         0x00007000u /* RTE で復帰するユーザモードのスタック */
#define USER_PC         0x00002000u /* 復帰先。最初のフェッチだけバスエラーにする */
#define RTE_FRAME       0x00006000u /* RTE 用のフォーマット0フレームを置く場所 */

typedef struct {
    uint8_t ram[RAM_SIZE];
    uint32_t fail_fetch_at;   /* このアドレスのワード読みを */
    unsigned fail_fetch_left; /* この回数だけバスエラーにする */
} test_ctx_t;

static void write_word_be(uint8_t *ram, uint32_t addr, uint16_t val) {
    ram[addr + 0] = (uint8_t)(val >> 8);
    ram[addr + 1] = (uint8_t)val;
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t val) {
    ram[addr + 0] = (uint8_t)(val >> 24);
    ram[addr + 1] = (uint8_t)(val >> 16);
    ram[addr + 2] = (uint8_t)(val >> 8);
    ram[addr + 3] = (uint8_t)val;
}

static bool tb_read_byte(void *ctx, uint32_t addr, uint8_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr >= RAM_SIZE) {
        *out = 0xFF;
        return false;
    }
    *out = t->ram[addr];
    return true;
}

static bool tb_read_word(void *ctx, uint32_t addr, uint16_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr == t->fail_fetch_at && t->fail_fetch_left > 0) {
        t->fail_fetch_left--;
        *out = 0xFFFF;
        return false;
    }
    if (addr + 1 >= RAM_SIZE) {
        *out = 0xFFFF;
        return false;
    }
    *out = (uint16_t)((t->ram[addr] << 8) | t->ram[addr + 1]);
    return true;
}

static bool tb_read_long(void *ctx, uint32_t addr, uint32_t *out) {
    uint16_t hi = 0, lo = 0;
    if (!tb_read_word(ctx, addr, &hi) || !tb_read_word(ctx, addr + 2, &lo)) {
        *out = 0xFFFFFFFFu;
        return false;
    }
    *out = ((uint32_t)hi << 16) | lo;
    return true;
}

static bool tb_write_byte(void *ctx, uint32_t addr, uint8_t val) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr >= RAM_SIZE)
        return false;
    t->ram[addr] = val;
    return true;
}

static bool tb_write_word(void *ctx, uint32_t addr, uint16_t val) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr + 1 >= RAM_SIZE)
        return false;
    write_word_be(t->ram, addr, val);
    return true;
}

static bool tb_write_long(void *ctx, uint32_t addr, uint32_t val) {
    bool ok1 = tb_write_word(ctx, addr, (uint16_t)(val >> 16));
    bool ok2 = tb_write_word(ctx, addr + 2, (uint16_t)val);
    return ok1 && ok2;
}

static void tb_sync(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
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
        .write_hook = NULL,
        .iack = NULL,
        .reset_peripheral = NULL,
        .ctx = t,
    };
    return mc68k_create_model(&bus, MC68K_MODEL_68020);
}

/* スーパーバイザ側: USP を USER_SP に設定し、RTE でユーザモードの USER_PC へ戻る。
 *   MOVEA.L #USER_SP,A0 : 0x207C + imm32
 *   MOVE.L A0,USP       : 0x4E60
 *   MOVEA.L #RTE_FRAME,A7: 0x2E7C + imm32
 *   RTE                 : 0x4E73
 * RTE フレーム(フォーマット0、8バイト): SR=0x0000 / PC=USER_PC / フォーマットワード=0x0000 */
static void setup(test_ctx_t *t, uint16_t handler_opcode) {
    memset(t, 0, sizeof(*t));
    write_long_be(t->ram, 0x000000, RESET_SSP);
    write_long_be(t->ram, 0x000004, RESET_PC);
    write_long_be(t->ram, 0x000008, BUS_ERR_HANDLER); /* ベクタ2 */
    write_word_be(t->ram, BUS_ERR_HANDLER, handler_opcode);

    uint32_t p = RESET_PC;
    write_word_be(t->ram, p, 0x207C);
    p += 2;
    write_long_be(t->ram, p, USER_SP);
    p += 4;
    write_word_be(t->ram, p, 0x4E60);
    p += 2;
    write_word_be(t->ram, p, 0x2E7C);
    p += 2;
    write_long_be(t->ram, p, RTE_FRAME);
    p += 4;
    write_word_be(t->ram, p, 0x4E73);

    write_word_be(t->ram, RTE_FRAME + 0, 0x0000); /* SR: ユーザモード */
    write_long_be(t->ram, RTE_FRAME + 2, USER_PC);
    write_word_be(t->ram, RTE_FRAME + 6, 0x0000); /* フォーマット0 */

    write_word_be(t->ram, USER_PC, 0x60FE); /* BRA *（フェッチ成功時の落とし所） */

    t->fail_fetch_at = USER_PC;
}

/* ベクタ2到達時点で USP がユーザ SP を保持していること。
 * 修正前は RTE 命令開始時の SSP（= RTE_FRAME）で上書きされていた。 */
static void test_usp_survives_fetch_fault(void) {
    test_ctx_t t;
    setup(&t, 0x60FE); /* ハンドラ = BRA *（自己ループで停止） */
    t.fail_fetch_left = 1000;

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 300);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    assert(r.pc == BUS_ERR_HANDLER); /* バスエラーでベクタ2へ来ている */
    assert(r.sr & 0x2000);           /* スーパーバイザモード */
    assert(r.usp == USER_SP);        /* ★ USP が壊れていない */
    assert(r.usp != RTE_FRAME);      /* 修正前に化けていた値そのもの */

    mc68k_destroy(cpu);
    printf("test_usp_survives_fetch_fault: OK\n");
}

/* ハンドラが RTE で復帰し、フォルトした命令を再実行できること。
 * 再実行後もユーザ SP が保たれていること。 */
static void test_usp_survives_handler_rte(void) {
    test_ctx_t t;
    setup(&t, 0x4E73);     /* ハンドラ = RTE（フォーマットAフレームから復帰） */
    t.fail_fetch_left = 1; /* 最初のフェッチだけ失敗させ、再実行は成功させる */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 500);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    assert(t.fail_fetch_left == 0); /* フォルトが実際に起きた */
    assert((r.sr & 0x2000) == 0);   /* ユーザモードへ戻っている */
    assert(r.pc == USER_PC);        /* フォルトした命令を再実行できた */
    assert(r.a[7] == USER_SP);      /* ★ ユーザ SP が保たれている */

    mc68k_destroy(cpu);
    printf("test_usp_survives_handler_rte: OK\n");
}

int main(void) {
    test_usp_survives_fetch_fault();
    test_usp_survives_handler_rte();
    printf("test_68020_fetch_fault_usp: all tests passed\n");
    return 0;
}
