/* pc / reg / call ring の構造化取得 API・有効件数・試行番号 seq・dump の件数 clamp
 * 。
 * 公開 API だけで検証する（内部ヘッダは include しない）。 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mc68000/bus_hooks.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"

#define RAM_SIZE  0x10000u
#define RESET_SSP 0x00008000u
#define RESET_PC  0x00001000u
#define NOP_OP    0x4E71u
#define RTS_OP    0x4E75u
#define BRA_SELF  0x60FEu

/* BSR.S の宛先。0x1000 と 0x1002 に置いた BSR.S がここへ飛ぶ。 */
#define SUB_ADDR     0x00001010u
#define BSR_1000_SUB 0x610Eu /* 0x1000 + 2 + 0x0E = SUB_ADDR */
#define BSR_1002_SUB 0x610Cu /* 0x1002 + 2 + 0x0C = SUB_ADDR */

#define HANDLER_ADDR   0x00003000u
#define BUS_ERR_VEC    0x00000008u /* ベクタ2 */
#define AUTOVEC1_VEC   0x00000064u /* ベクタ25（レベル1 オートベクタ） */
#define OUT_OF_RAM     0x00020000u
#define OUT_OF_RAM_SSP 0x00030000u

typedef struct {
    uint8_t ram[RAM_SIZE];
} test_ctx_t;

typedef struct {
    char text[8192];
    size_t len;
} log_capture_t;

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

static bool wr_b(void *ctx, uint32_t addr, uint8_t value) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr < RAM_SIZE)
        t->ram[addr] = value;
    return addr < RAM_SIZE;
}

static bool wr_w(void *ctx, uint32_t addr, uint16_t value) {
    return wr_b(ctx, addr, (uint8_t)(value >> 8)) && wr_b(ctx, addr + 1, (uint8_t)value);
}

static bool wr_l(void *ctx, uint32_t addr, uint32_t value) {
    return wr_w(ctx, addr, (uint16_t)(value >> 16)) && wr_w(ctx, addr + 2, (uint16_t)value);
}

static void sync_cb(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}

static mc68k_state_t *make_cpu_model(test_ctx_t *t, mc68k_model_t model) {
    mc68k_bus_t bus = {
        .read_byte = rd_b,
        .read_word = rd_w,
        .read_long = rd_l,
        .write_byte = wr_b,
        .write_word = wr_w,
        .write_long = wr_l,
        .sync = sync_cb,
        .ctx = t,
    };
    return mc68k_create_model(&bus, model);
}

static void capture_log(void *ctx, const char *msg) {
    log_capture_t *capture = (log_capture_t *)ctx;
    size_t remaining = sizeof(capture->text) - capture->len - 1;
    size_t len = strlen(msg);
    if (len > remaining)
        len = remaining;
    memcpy(capture->text + capture->len, msg, len);
    capture->len += len;
    capture->text[capture->len] = '\0';
}

static void clear_log(log_capture_t *capture) {
    capture->text[0] = '\0';
    capture->len = 0;
}

static void init_reset_vectors(test_ctx_t *t, uint32_t ssp, uint32_t pc) {
    wr_l(t, 0, ssp);
    wr_l(t, 4, pc);
}

/* RESET_PC から count 個の NOP を置く。 */
static void put_nops(test_ctx_t *t, int count) {
    for (int i = 0; i < count; i++)
        wr_w(t, RESET_PC + 2u * (uint32_t)i, NOP_OP);
}

/* BSR(0x1000) → RTS(SUB_ADDR) → BSR(0x1002) の3試行で、pc / reg / call の各リングに
 * ちょうど3件ずつ記録されるプログラム。 */
static void put_bsr_rts_bsr(test_ctx_t *t) {
    wr_w(t, RESET_PC, BSR_1000_SUB);
    wr_w(t, RESET_PC + 2, BSR_1002_SUB);
    wr_w(t, SUB_ADDR, RTS_OP);
}

static void step(mc68k_state_t *cpu, int n) {
    for (int i = 0; i < n; i++)
        mc68k_execute(cpu, 1);
}

static bool all_bytes(const void *p, size_t n, uint8_t v) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != v)
            return false;
    }
    return true;
}

/* A1: read の境界規則（pc / reg / call の3種）。 */
static void test_a1_read_bounds(void) {
    mc68k_pc_ring_entry_t pbuf[8];
    mc68k_reg_ring_entry_t rbuf[8];
    mc68k_call_ring_entry_t cbuf[8];
    assert(mc68k_read_pc_ring(NULL, pbuf, 8) == 0);
    assert(mc68k_read_reg_ring(NULL, rbuf, 8) == 0);
    assert(mc68k_read_call_ring(NULL, cbuf, 8) == 0);

    test_ctx_t t = {0};
    init_reset_vectors(&t, RESET_SSP, RESET_PC);
    put_bsr_rts_bsr(&t);
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_pc_ring(cpu, 1);
    mc68k_set_reg_ring(cpu, 1);

    assert(mc68k_read_pc_ring(cpu, NULL, 0) == 0);
    assert(mc68k_read_reg_ring(cpu, NULL, 0) == 0);
    assert(mc68k_read_call_ring(cpu, NULL, 0) == 0);

    step(cpu, 3);
    assert(mc68k_read_pc_ring(cpu, NULL, 1) == 3);
    assert(mc68k_read_reg_ring(cpu, NULL, 1) == 3);
    assert(mc68k_read_call_ring(cpu, NULL, 1) == 3);

    memset(pbuf, 0xA5, sizeof(pbuf));
    memset(rbuf, 0xA5, sizeof(rbuf));
    memset(cbuf, 0xA5, sizeof(cbuf));
    assert(mc68k_read_pc_ring(cpu, pbuf, 0) == 0);
    assert(mc68k_read_reg_ring(cpu, rbuf, 0) == 0);
    assert(mc68k_read_call_ring(cpu, cbuf, 0) == 0);
    assert(all_bytes(pbuf, sizeof(pbuf), 0xA5));
    assert(all_bytes(rbuf, sizeof(rbuf), 0xA5));
    assert(all_bytes(cbuf, sizeof(cbuf), 0xA5));
    mc68k_destroy(cpu);
}

/* A2: pc ring の内容と順序、cap による最新 cap 件の取得。 */
static void test_a2_pc_order(void) {
    test_ctx_t t = {0};
    init_reset_vectors(&t, RESET_SSP, RESET_PC);
    put_nops(&t, 3);
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_pc_ring(cpu, 1);
    step(cpu, 3);

    mc68k_pc_ring_entry_t buf[8];
    assert(mc68k_read_pc_ring(cpu, buf, 8) == 3);
    assert(buf[0].pc == 0x1000u);
    assert(buf[1].pc == 0x1002u);
    assert(buf[2].pc == 0x1004u);
    uint64_t s = buf[0].seq;
    assert(s >= 1);
    assert(buf[1].seq == s + 1);
    assert(buf[2].seq == s + 2);

    assert(mc68k_read_pc_ring(cpu, buf, 2) == 2);
    assert(buf[0].pc == 0x1002u && buf[0].seq == s + 1);
    assert(buf[1].pc == 0x1004u && buf[1].seq == s + 2);
    mc68k_destroy(cpu);
}

/* A3: 折返し。容量 + 5 試行で、最古は6個目の NOP、seq は全件で +1 ずつ。 */
static mc68k_pc_ring_entry_t pc_all[MC68K_PC_RING_SIZE];
static mc68k_reg_ring_entry_t reg_all[MC68K_REG_RING_SIZE];

static void test_a3_wrap(void) {
    const int pc_steps = MC68K_PC_RING_SIZE + 5;
    const int reg_steps = MC68K_REG_RING_SIZE + 5;
    const uint32_t sixth_nop = RESET_PC + 2u * 5u;

    test_ctx_t t = {0};
    init_reset_vectors(&t, RESET_SSP, RESET_PC);
    put_nops(&t, pc_steps);
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_pc_ring(cpu, 1);
    step(cpu, pc_steps);
    assert(mc68k_read_pc_ring(cpu, NULL, 0) == MC68K_PC_RING_SIZE);
    assert(mc68k_read_pc_ring(cpu, pc_all, MC68K_PC_RING_SIZE) == MC68K_PC_RING_SIZE);
    assert(pc_all[0].pc == sixth_nop);
    for (int i = 1; i < MC68K_PC_RING_SIZE; i++) {
        assert(pc_all[i].pc == pc_all[i - 1].pc + 2);
        assert(pc_all[i].seq == pc_all[i - 1].seq + 1);
    }
    mc68k_destroy(cpu);

    test_ctx_t t2 = {0};
    init_reset_vectors(&t2, RESET_SSP, RESET_PC);
    put_nops(&t2, reg_steps);
    cpu = make_cpu_model(&t2, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_reg_ring(cpu, 1);
    step(cpu, reg_steps);
    assert(mc68k_read_reg_ring(cpu, NULL, 0) == MC68K_REG_RING_SIZE);
    assert(mc68k_read_reg_ring(cpu, reg_all, MC68K_REG_RING_SIZE) == MC68K_REG_RING_SIZE);
    assert(reg_all[0].pc == sixth_nop);
    for (int i = 1; i < MC68K_REG_RING_SIZE; i++) {
        assert(reg_all[i].pc == reg_all[i - 1].pc + 2);
        assert(reg_all[i].seq == reg_all[i - 1].seq + 1);
        assert(reg_all[i].opcode_valid == 1 && reg_all[i].opcode == NOP_OP);
    }
    mc68k_destroy(cpu);
}

/* A4: OFF 中は記録しないが seq は進み、ON で続きから追記する。 */
static void test_a4_off_on(void) {
    test_ctx_t t = {0};
    init_reset_vectors(&t, RESET_SSP, RESET_PC);
    put_nops(&t, 5);
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_pc_ring(cpu, 1);
    step(cpu, 2);

    mc68k_pc_ring_entry_t before[4], after[4];
    assert(mc68k_read_pc_ring(cpu, before, 4) == 2);
    mc68k_set_pc_ring(cpu, 0);
    step(cpu, 2);
    assert(mc68k_read_pc_ring(cpu, after, 4) == 2);
    assert(memcmp(before, after, 2 * sizeof(before[0])) == 0);

    mc68k_set_pc_ring(cpu, 1);
    step(cpu, 1);
    assert(mc68k_read_pc_ring(cpu, after, 4) == 3);
    assert(after[2].pc == 0x1008u);
    assert(after[2].seq == after[1].seq + 3);
    mc68k_destroy(cpu);
}

/* A5: reset は ON/OFF・履歴・有効件数・seq を変えない。 */
static void test_a5_reset(void) {
    test_ctx_t t = {0};
    init_reset_vectors(&t, RESET_SSP, RESET_PC);
    put_nops(&t, 3);
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_pc_ring(cpu, 1);
    step(cpu, 2);

    mc68k_reset(cpu);
    assert(mc68k_get_pc_ring(cpu) == 1);
    assert(mc68k_read_pc_ring(cpu, NULL, 0) == 2);
    step(cpu, 1);

    mc68k_pc_ring_entry_t buf[4];
    assert(mc68k_read_pc_ring(cpu, buf, 4) == 3);
    assert(buf[0].pc == 0x1000u && buf[1].pc == 0x1002u);
    assert(buf[2].pc == 0x1000u); /* reset 後の最初の試行 */
    assert(buf[2].seq > buf[1].seq);
    mc68k_destroy(cpu);
}

/* A6 で D0-D7 / A0-A6 に入れる値。レジスタごとに異なる非ゼロ値にして、
 * reg ring の d[] / a[] が取り違えなくコピーされていることを検証する。 */
static uint32_t a6_dreg_value(int n) {
    return 0xD0D0D000u + (uint32_t)n;
}

static uint32_t a6_areg_value(int n) {
    return 0x0000A000u + 0x10u * (uint32_t)n;
}

/* A6: opcode fetch 自体がフォールトした試行は opcode_valid == 0。 */
static void test_a6_opcode_valid(void) {
    const int movea_count = 7;    /* MOVEA.L #imm,A0〜A6 */
    const uint32_t movea_len = 6; /* opcode + imm32 */
    const uint32_t jmp_pc = RESET_PC + movea_len * (uint32_t)movea_count;

    test_ctx_t t = {0};
    init_reset_vectors(&t, RESET_SSP, RESET_PC);
    wr_l(&t, BUS_ERR_VEC, HANDLER_ADDR);
    for (int n = 0; n < movea_count; n++) {
        uint32_t pc = RESET_PC + movea_len * (uint32_t)n;
        wr_w(&t, pc, (uint16_t)(0x207Cu | ((uint32_t)n << 9))); /* MOVEA.L #imm,An */
        wr_l(&t, pc + 2, a6_areg_value(n));
    }
    wr_w(&t, jmp_pc, 0x4EF9u); /* JMP $00020000.L（RAM 外） */
    wr_l(&t, jmp_pc + 2, OUT_OF_RAM);
    wr_w(&t, HANDLER_ADDR, BRA_SELF);
    /* 68000 は JMP 完了時に分岐先を先読みして bus error になるため、次命令の取得を観測する
     * この試験は先読みキューを持たない 68010 で行う。 */
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68010);
    assert(cpu);
    mc68k_reset(cpu);
    for (int n = 0; n < 8; n++)
        mc68k_set_dreg(cpu, n, a6_dreg_value(n));
    step(cpu, movea_count); /* reg ring OFF のまま A0-A6 を設定する */
    assert(mc68k_read_reg_ring(cpu, NULL, 0) == 0);
    mc68k_set_reg_ring(cpu, 1);
    step(cpu, 2);

    mc68k_reg_ring_entry_t buf[4];
    assert(mc68k_read_reg_ring(cpu, buf, 4) == 2);
    assert(buf[0].pc == jmp_pc);
    assert(buf[0].opcode_valid == 1 && buf[0].opcode == 0x4EF9u);
    assert(buf[1].pc == OUT_OF_RAM);
    assert(buf[1].opcode_valid == 0);
    assert(buf[1].seq == buf[0].seq + 1);
    int checked = 0;
    for (int i = 0; i < 2; i++) {
        assert(buf[i].sr == 0x2700u); /* reset 直後の S=1・マスク7 */
        for (int n = 0; n < 8; n++)
            assert(buf[i].d[n] == a6_dreg_value(n));
        for (int n = 0; n < 7; n++)
            assert(buf[i].a[n] == a6_areg_value(n));
        assert(buf[i].a[7] == RESET_SSP);
        checked++;
    }
    assert(checked == 2);
    mc68k_destroy(cpu);
}

/* A7: call ring の内容と、pc ring 上の試行との seq 一致。 */
static void test_a7_call(void) {
    test_ctx_t t = {0};
    init_reset_vectors(&t, RESET_SSP, RESET_PC);
    put_bsr_rts_bsr(&t);
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_pc_ring(cpu, 1);
    step(cpu, 2); /* BSR → RTS */

    mc68k_call_ring_entry_t calls[4];
    mc68k_pc_ring_entry_t pcs[4];
    assert(mc68k_read_call_ring(cpu, calls, 4) == 2);
    assert(mc68k_read_pc_ring(cpu, pcs, 4) == 2);
    assert(pcs[0].pc == RESET_PC && pcs[1].pc == SUB_ADDR);

    assert(calls[0].is_return == 0);
    assert(calls[0].from_pc == RESET_PC);
    assert(calls[0].to_pc == SUB_ADDR);
    assert(calls[0].seq == pcs[0].seq);

    assert(calls[1].is_return == 1);
    assert(calls[1].from_pc == SUB_ADDR);
    assert(calls[1].to_pc == RESET_PC + 2);
    assert(calls[1].seq == pcs[1].seq);
    mc68k_destroy(cpu);
}

/* A8: push がバスエラーになった JSR は call ring に載らないが、pc ring には載る。 */
static void test_a8_failed_call(void) {
    test_ctx_t t = {0};
    init_reset_vectors(&t, OUT_OF_RAM_SSP, RESET_PC);
    wr_w(&t, RESET_PC, 0x4EB9u); /* JSR SUB_ADDR.L */
    wr_l(&t, RESET_PC + 2, SUB_ADDR);
    wr_w(&t, SUB_ADDR, RTS_OP);
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_pc_ring(cpu, 1);
    step(cpu, 1);

    assert(mc68k_read_call_ring(cpu, NULL, 0) == 0);
    mc68k_pc_ring_entry_t buf[4];
    assert(mc68k_read_pc_ring(cpu, buf, 4) == 1); /* JSR の試行だけ（その後 HALT） */
    assert(buf[0].pc == RESET_PC);
    assert(buf[0].seq == 1); /* 新規 CPU の最初の試行 */
    mc68k_destroy(cpu);
}

/* A9: IRQ 受付は採番しない。ハンドラ先頭の試行は直前の NOP の seq + 1。 */
static void test_a9_irq_no_seq(void) {
    test_ctx_t t = {0};
    init_reset_vectors(&t, RESET_SSP, RESET_PC);
    wr_l(&t, AUTOVEC1_VEC, HANDLER_ADDR);
    wr_w(&t, RESET_PC, 0x46FCu); /* MOVE #$2000,SR */
    wr_w(&t, RESET_PC + 4, NOP_OP);
    wr_w(&t, RESET_PC + 6, NOP_OP);
    for (uint32_t i = 0; i < 4; i++)
        wr_w(&t, HANDLER_ADDR + 2 * i, NOP_OP);
    wr_w(&t, RESET_PC + 2, 0x2000u);
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_pc_ring(cpu, 1);
    step(cpu, 2); /* MOVE → NOP(0x1004) */

    mc68k_pc_ring_entry_t buf[8];
    size_t n = mc68k_read_pc_ring(cpu, buf, 8);
    assert(n == 2 && buf[1].pc == RESET_PC + 4);
    uint64_t s = buf[1].seq;

    mc68k_set_irq(cpu, 1);
    int found = -1;
    for (int tries = 0; tries < 4 && found < 0; tries++) {
        mc68k_execute(cpu, 1);
        n = mc68k_read_pc_ring(cpu, buf, 8);
        for (size_t i = 0; i < n; i++) {
            if (buf[i].pc == HANDLER_ADDR)
                found = (int)i;
        }
    }
    assert(found == 2); /* NOP(0x1004) の次の試行がハンドラ先頭 */
    assert(buf[found].seq == s + 1);
    mc68k_destroy(cpu);
}

/* A10: dump は有効件数で clamp し、見出しの件数は実出力件数。 */
static void test_a10_dump_clamp(void) {
    test_ctx_t t = {0};
    log_capture_t log = {0};
    init_reset_vectors(&t, RESET_SSP, RESET_PC);
    put_bsr_rts_bsr(&t);
    mc68k_state_t *cpu = make_cpu_model(&t, MC68K_MODEL_68000);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_log_handler(cpu, capture_log, &log);
    mc68k_set_pc_ring(cpu, 1);
    mc68k_set_reg_ring(cpu, 1);

    mc68k_dump_pc_ring(cpu, 0);
    assert(strstr(log.text, "last 0 instrs") != NULL);
    assert(strstr(log.text, " 000000") == NULL);
    clear_log(&log);
    mc68k_dump_reg_ring(cpu, 0);
    assert(strstr(log.text, "last 0 instrs") != NULL);
    assert(strstr(log.text, "pc=") == NULL);
    clear_log(&log);
    mc68k_dump_call_ring(cpu, 0);
    assert(strstr(log.text, "last 0 events") != NULL);
    assert(strstr(log.text, "CALL") == NULL);

    step(cpu, 3);
    static const struct {
        int arg;
        const char *instrs;
        const char *events;
    } cases[] = {
        {0, "last 3 instrs", "last 3 events"},
        {2, "last 2 instrs", "last 2 events"},
        {5000, "last 3 instrs", "last 3 events"},
    };
    int checked = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        clear_log(&log);
        mc68k_dump_pc_ring(cpu, cases[i].arg);
        assert(strstr(log.text, cases[i].instrs) != NULL);
        clear_log(&log);
        mc68k_dump_reg_ring(cpu, cases[i].arg);
        assert(strstr(log.text, cases[i].instrs) != NULL);
        clear_log(&log);
        mc68k_dump_call_ring(cpu, cases[i].arg);
        assert(strstr(log.text, cases[i].events) != NULL);
        checked++;
    }
    assert(checked == 3);
    mc68k_destroy(cpu);
}

int main(void) {
    test_a1_read_bounds();
    test_a2_pc_order();
    test_a3_wrap();
    test_a4_off_on();
    test_a5_reset();
    test_a6_opcode_valid();
    test_a7_call();
    test_a8_failed_call();
    test_a9_irq_no_seq();
    test_a10_dump_clamp();
    printf("test_ring_read: all passed\n");
    return 0;
}
