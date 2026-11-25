/*
 * test_exception_t0.c — 例外処理が SR の T1 と T0 の両方を消すこと
 *
 * MC68020 UM §6.1 は例外処理の手順で T1 と T0 を消すと定める。
 * 68020/68030 で SR.T0=1 のまま各種例外に入り、ハンドラ内の SR に T0 が残らないこと、
 * 退避フレームには例外前の SR（T0 付き）が積まれることを確認する。
 * 残っていると、ハンドラ先頭の BRA で分岐時トレース（ベクタ9）が誤って発火する。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mc68000/bus_hooks.h"
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"

#define RAM_SIZE      0x10000u
#define RESET_SSP     0x00008000u
#define RESET_PC      0x00001000u
#define HANDLER       0x00000300u
#define VECTOR_FIRST  2u
#define VECTOR_LAST   47u
#define VECTOR_IRQ3   27u
#define UNMAPPED_ADDR 0x00020000u

#define OP_MOVE_IMM_SR  0x46FCu
#define OP_BRA_SELF     0x60FEu
#define OP_MOVE_SR_D0   0x40C0u /* MOVE SR,D0（分岐ではないのでトレースされない） */
#define OP_NOP          0x4E71u
#define OP_ILLEGAL      0x4AFCu
#define OP_TRAP0        0x4E40u
#define OP_LINE_A       0xA000u
#define OP_LINE_F       0xFFFFu
#define OP_ORI_SR       0x007Cu /* 特権命令（ユーザモードで特権違反） */
#define OP_MOVEL_ABS_D0 0x2039u /* MOVE.L abs.L,D0 */

#define SR_T1_BIT 0x8000u
#define SR_T0_BIT 0x4000u
#define SR_S_BIT  0x2000u
#define SR_I_MASK 0x0700u

#define TRACE_HANDLER 0x00000340u /* ベクタ9専用。D0 を触らない */
#define VECTOR_TRACE  9u

#define EXEC_CYCLES 400

typedef struct {
    uint8_t ram[RAM_SIZE];
} test_ctx_t;

static void put_word(uint8_t *ram, uint32_t a, uint16_t v) {
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1] = (uint8_t)v;
}

static void put_long(uint8_t *ram, uint32_t a, uint32_t v) {
    put_word(ram, a, (uint16_t)(v >> 16));
    put_word(ram, a + 2, (uint16_t)v);
}

static bool tb_read_byte(void *c, uint32_t a, uint8_t *o) {
    test_ctx_t *t = c;
    if (a >= RAM_SIZE) {
        *o = 0xFF;
        return false;
    }
    *o = t->ram[a];
    return true;
}

static bool tb_read_word(void *c, uint32_t a, uint16_t *o) {
    test_ctx_t *t = c;
    if (a + 1 >= RAM_SIZE) {
        *o = 0xFFFF;
        return false;
    }
    *o = (uint16_t)((t->ram[a] << 8) | t->ram[a + 1]);
    return true;
}

static bool tb_read_long(void *c, uint32_t a, uint32_t *o) {
    uint16_t hi = 0, lo = 0;
    if (!tb_read_word(c, a, &hi) || !tb_read_word(c, a + 2, &lo)) {
        *o = 0xFFFFFFFFu;
        return false;
    }
    *o = ((uint32_t)hi << 16) | lo;
    return true;
}

static bool tb_write_byte(void *c, uint32_t a, uint8_t v) {
    test_ctx_t *t = c;
    if (a >= RAM_SIZE)
        return false;
    t->ram[a] = v;
    return true;
}

static bool tb_write_word(void *c, uint32_t a, uint16_t v) {
    test_ctx_t *t = c;
    if (a + 1 >= RAM_SIZE)
        return false;
    put_word(t->ram, a, v);
    return true;
}

static bool tb_write_long(void *c, uint32_t a, uint32_t v) {
    bool ok1 = tb_write_word(c, a, (uint16_t)(v >> 16));
    bool ok2 = tb_write_word(c, a + 2, (uint16_t)v);
    return ok1 && ok2;
}

static void tb_sync(void *c, int cycles) {
    (void)c;
    (void)cycles;
}

static mc68k_state_t *make_cpu(test_ctx_t *t, mc68k_model_t model) {
    mc68k_bus_t bus = {
        .read_byte = tb_read_byte,
        .read_word = tb_read_word,
        .read_long = tb_read_long,
        .write_byte = tb_write_byte,
        .write_word = tb_write_word,
        .write_long = tb_write_long,
        .sync = tb_sync,
        .ctx = t,
    };
    return mc68k_create_model(&bus, model);
}

/* 全例外ベクタを HANDLER（BRA *）へ向け、RESET_PC に
 * MOVE #pre_sr,SR / trigger[0..n_trigger) を置く。 */
static void setup(test_ctx_t *t, uint16_t pre_sr, const uint16_t *trigger, int n_trigger) {
    memset(t, 0, sizeof(*t));
    put_long(t->ram, 0, RESET_SSP);
    put_long(t->ram, 4, RESET_PC);
    for (uint32_t v = VECTOR_FIRST; v <= VECTOR_LAST; v++)
        put_long(t->ram, v * 4, HANDLER);
    /* ハンドラ入口の SR を D0 へ捕捉してから自己ループする。BRA 後は残った T0 が
     * ベクタ9を起こして最終 SR を上書きするため、最終 SR だけでは欠陥が見えない。 */
    put_word(t->ram, HANDLER, OP_MOVE_SR_D0);
    put_word(t->ram, HANDLER + 2, OP_BRA_SELF);
    put_long(t->ram, VECTOR_TRACE * 4, TRACE_HANDLER);
    put_word(t->ram, TRACE_HANDLER, OP_BRA_SELF);

    uint32_t p = RESET_PC;
    put_word(t->ram, p, OP_MOVE_IMM_SR);
    put_word(t->ram, p + 2, pre_sr);
    p += 4;
    for (int i = 0; i < n_trigger; i++, p += 2)
        put_word(t->ram, p, trigger[i]);
}

typedef struct {
    const char *name;
    uint16_t pre_sr;     /* MOVE #,SR で設定する例外前の SR */
    uint16_t trigger[3]; /* 例外を起こす命令語 */
    int n_trigger;
    bool irq;             /* true: 命令ではなく IRQ3 で入る */
    bool entry_sr;        /* ハンドラ入口の SR（D0）を検査する。TRAP は Group2 でトレースが
                           * ハンドラ先頭より先に入るため検査できない（退避フレームで見る） */
    uint32_t frame_bytes; /* 最初に積まれるフレームの大きさ。0 = 退避 SR の検査を省略 */
} case_t;

/* 68020/68030 の format 0 フレーム（SR, PC, フォーマットワード）。
 * TRAP は Group2 でトレースに追随し 2 つ目のフレームが下に積まれるため、
 * 最初のフレームは最終 A7 でなく RESET_SSP からの固定位置で読む。 */
#define FRAME_FORMAT0_BYTES 8u

static const case_t CASES[] = {
    {"TRAP #0",
     SR_T0_BIT | SR_S_BIT | SR_I_MASK,
     {OP_TRAP0},
     1,
     false,
     false,
     FRAME_FORMAT0_BYTES},
    {"TRAP #0 with T1+T0",
     SR_T1_BIT | SR_T0_BIT | SR_S_BIT | SR_I_MASK,
     {OP_TRAP0},
     1,
     false,
     false,
     FRAME_FORMAT0_BYTES},
    {"ILLEGAL",
     SR_T0_BIT | SR_S_BIT | SR_I_MASK,
     {OP_ILLEGAL},
     1,
     false,
     true,
     FRAME_FORMAT0_BYTES},
    {"Line-A",
     SR_T0_BIT | SR_S_BIT | SR_I_MASK,
     {OP_LINE_A},
     1,
     false,
     true,
     FRAME_FORMAT0_BYTES},
    {"Line-F",
     SR_T0_BIT | SR_S_BIT | SR_I_MASK,
     {OP_LINE_F},
     1,
     false,
     true,
     FRAME_FORMAT0_BYTES},
    {"privilege violation",
     SR_T0_BIT,
     {OP_ORI_SR, 0x0000},
     2,
     false,
     true,
     FRAME_FORMAT0_BYTES},
    {"bus error (group 0)",
     SR_T0_BIT | SR_S_BIT | SR_I_MASK,
     {OP_MOVEL_ABS_D0, 0x0002, 0x0000},
     3,
     false,
     true,
     0},
    {"IRQ3 accept", SR_T0_BIT | SR_S_BIT, {OP_NOP, OP_NOP}, 2, true, true, FRAME_FORMAT0_BYTES},
};
#define NUM_CASES (sizeof(CASES) / sizeof(CASES[0]))

static void run_case(mc68k_model_t model, const case_t *c) {
    test_ctx_t t;
    setup(&t, c->pre_sr, c->trigger, c->n_trigger);
    (void)UNMAPPED_ADDR; /* 命令語中の 0x0002:0x0000 がこのアドレス */
    if (c->irq)
        put_long(t.ram, VECTOR_IRQ3 * 4, HANDLER);

    mc68k_state_t *cpu = make_cpu(&t, model);
    mc68k_reset(cpu);
    if (c->irq) {
        mc68k_execute(cpu, 1); /* MOVE #,SR だけ実行 */
        mc68k_set_irq(cpu, 3);
    }
    mc68k_execute(cpu, EXEC_CYCLES);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    if (c->entry_sr)
        assert(r.pc == HANDLER + 2); /* MOVE SR,D0 を実行済みで BRA * に居る */
    else
        assert(r.pc == TRACE_HANDLER); /* Group2: トレースが後続してベクタ9で停止 */
    uint16_t entry_sr = (uint16_t)(c->pre_sr | SR_S_BIT);
    uint16_t expect_sr = (uint16_t)(entry_sr & ~(SR_T1_BIT | SR_T0_BIT));
    if (c->irq)
        expect_sr = (uint16_t)((expect_sr & ~SR_I_MASK) | (3u << 8));

    if (c->entry_sr && r.d[0] != expect_sr) {
        fprintf(stderr, "FAIL model=%d %s: handler-entry SR=%04X (expect %04X)\n", (int)model,
                c->name, (unsigned)r.d[0], expect_sr);
        assert(0);
    }

    if (c->frame_bytes != 0) {
        uint32_t frame = RESET_SSP - c->frame_bytes;
        uint16_t stacked = (uint16_t)((t.ram[frame] << 8) | t.ram[frame + 1]);
        if (stacked != c->pre_sr) {
            fprintf(stderr, "FAIL model=%d %s: stacked SR=%04X (expect %04X)\n", (int)model,
                    c->name, stacked, c->pre_sr);
            assert(0);
        }
    }
    mc68k_destroy(cpu);
}

int main(void) {
    static const mc68k_model_t MODELS[] = {MC68K_MODEL_68020, MC68K_MODEL_68030};
    for (size_t m = 0; m < sizeof(MODELS) / sizeof(MODELS[0]); m++)
        for (size_t i = 0; i < NUM_CASES; i++)
            run_case(MODELS[m], &CASES[i]);
    printf("test_exception_t0: OK (%zu models x %zu cases)\n",
           sizeof(MODELS) / sizeof(MODELS[0]), (size_t)NUM_CASES);
    return 0;
}
