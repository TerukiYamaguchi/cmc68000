/*
 * test_conformance_timing.c — TIM の適合性ケース（仕様書第12.6節）。
 *
 * 期待値は実装ではなく docs/mc68000-internals/spec_mc68000.md 第12.6節
 * （UM 第8節の cycle table）に基づく。cycle 数は mc68k_execute(cpu, 1) の戻り値、
 * (r/w) 数は記録した bus cycle 数である。
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu_internal.h"
#include "mc68000/bus_hooks.h"
#include "mc68000/cpu.h"

#define ADDR_MASK     0x00FFFFFFu
#define RAM_SIZE      0x01000000u
#define RESET_SSP     0x8000u
#define RESET_PC      0x1000u
#define USER_SP       0x7000u
#define BERR_BASE     0x00F00000u /* reads and writes here answer BERR */
#define BERR_END      0x00F10000u
#define LOG_MAX       64u
#define SR_MASK       0xF71Fu /* bits whose value is architecturally defined */
#define SR_X          0x0010u
#define SR_N          0x0008u
#define VEC_BUS_ERROR 2u
#define VEC_ADDR_ERR  3u
#define HANDLER       0x1800u
#define RESERVED_STACK 0x6000u /* RTE 試験用の例外フレームの置き場所 */
#define FC_UDATA      1u
#define FC_UPROG      2u
#define FC_SDATA      5u
#define FC_SPROG      6u

typedef struct {
    uint32_t address; /* as received from the core */
    uint8_t size;
    uint8_t fc;
    bool read;
    bool instruction;
    bool rmw;
} cycle_t;

typedef struct {
    uint8_t *ram;
    cycle_t log[LOG_MAX];
    unsigned count;
    unsigned data_read_wait; /* extra clocks on each operand read */
    uint32_t berr_lo, berr_hi; /* 追加の BERR 領域 [lo, hi)。両方0なら無効 */
    unsigned irq_samples;      /* irq_after_first_sample の標本化回数 */
    mc68k_state_t *cpu;
} fixture_t;

static bool in_berr(const fixture_t *f, uint32_t addr) {
    uint32_t a = addr & ADDR_MASK;
    if (f->berr_hi != 0 && a >= f->berr_lo && a < f->berr_hi)
        return true;
    return a >= BERR_BASE && a < BERR_END;
}

static void access_timing(void *ctx, const mc68k_bus_access_t *a, mc68k_bus_timing_t *out) {
    fixture_t *f = ctx;
    out->wait_cycles = (!a->instruction && a->read) ? (uint16_t)f->data_read_wait : 0;
    out->cacheable = false;
    assert(f->count < LOG_MAX);
    f->log[f->count].address = a->physical;
    f->log[f->count].size = a->size;
    f->log[f->count].fc = a->fc;
    f->log[f->count].read = a->read;
    f->log[f->count].instruction = a->instruction;
    f->log[f->count].rmw = a->rmw;
    f->count++;
}

static bool rb(void *c, uint32_t a, uint8_t *o) {
    fixture_t *f = c;
    if (in_berr(f, a))
        return false;
    *o = f->ram[a & ADDR_MASK];
    return true;
}
static bool rw_(void *c, uint32_t a, uint16_t *o) {
    fixture_t *f = c;
    if (in_berr(f, a))
        return false;
    uint32_t m = a & ADDR_MASK;
    *o = (uint16_t)((f->ram[m] << 8) | f->ram[m + 1]);
    return true;
}
static bool rl_(void *c, uint32_t a, uint32_t *o) {
    uint16_t hi, lo;
    if (!rw_(c, a, &hi) || !rw_(c, a + 2, &lo))
        return false;
    *o = ((uint32_t)hi << 16) | lo;
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    fixture_t *f = c;
    if (in_berr(f, a))
        return false;
    f->ram[a & ADDR_MASK] = v;
    return true;
}
static bool ww_(void *c, uint32_t a, uint16_t v) {
    return wb(c, a, (uint8_t)(v >> 8)) && wb(c, a + 1, (uint8_t)v);
}
static bool wl_(void *c, uint32_t a, uint32_t v) {
    return ww_(c, a, (uint16_t)(v >> 16)) && ww_(c, a + 2, (uint16_t)v);
}
static uint8_t peek_b(void *c, uint32_t a) {
    fixture_t *f = c;
    return f->ram[a & ADDR_MASK];
}
static void sync_(void *c, int n) {
    (void)c;
    (void)n;
}
static void wh_(void *c, uint32_t a, uint32_t n) {
    (void)c;
    (void)a;
    (void)n;
}

static void put_w(uint8_t *ram, uint32_t a, uint16_t v) {
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1] = (uint8_t)v;
}
static void put_l(uint8_t *ram, uint32_t a, uint32_t v) {
    put_w(ram, a, (uint16_t)(v >> 16));
    put_w(ram, a + 2, (uint16_t)v);
}

static fixture_t make_fixture(void) {
    fixture_t f = {0};
    f.ram = calloc(RAM_SIZE, 1);
    assert(f.ram);
    put_l(f.ram, 0, RESET_SSP);
    put_l(f.ram, 4, RESET_PC);
    return f;
}

static void start_model(fixture_t *f, mc68k_model_t model) {
    mc68k_bus_t bus = {0};
    bus.read_byte = rb;
    bus.read_word = rw_;
    bus.read_long = rl_;
    bus.write_byte = wb;
    bus.write_word = ww_;
    bus.write_long = wl_;
    bus.sync = sync_;
    bus.write_hook = wh_;
    bus.ctx = f;
    bus.access_timing = access_timing;
    f->cpu = mc68k_create_model(&bus, model);
    assert(f->cpu);
    mc68k_reset(f->cpu);
    f->count = 0;
}

static void start(fixture_t *f) {
    start_model(f, MC68K_MODEL_68000);
}

static void dispose(fixture_t *f) {
    mc68k_destroy(f->cpu);
    free(f->ram);
}

/* Lay out instruction words at RESET_PC, with a clean user-visible CPU. */
static void program(fixture_t *f, const uint16_t *words, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        put_w(f->ram, RESET_PC + 2 * i, words[i]);
}
#define PROGRAM(f, ...)                                                                            \
    do {                                                                                           \
        const uint16_t w_[] = {__VA_ARGS__};                                                       \
        program((f), w_, sizeof w_ / sizeof w_[0]);                                                \
    } while (0)



/* Execute one instruction and return the clocks it consumed. */
static int step_cycles(fixture_t *f) {
    /* 仕様 §12.6: ケースはキューが満たされた命令先頭から始まる。初期2語は計測外にする。 */
    cpu_prefetch_prime(f->cpu);
    f->count = 0;
    return mc68k_execute(f->cpu, 1);
}

static unsigned reads(const fixture_t *f) {
    unsigned n = 0;
    for (unsigned i = 0; i < f->count; i++)
        n += f->log[i].read ? (f->log[i].size == 4 ? 2u : 1u) : 0u; /* long は word 2回 */
    return n;
}

static unsigned writes(const fixture_t *f) {
    unsigned n = 0;
    for (unsigned i = 0; i < f->count; i++)
        n += f->log[i].read ? 0u : (f->log[i].size == 4 ? 2u : 1u);
    return n;
}

/* Common initial state of spec section 12.6: PC=$1000, A0=$200, A1=$300. */
static fixture_t timing_fixture(void) {
    fixture_t f = make_fixture();
    start(&f);
    f.cpu->a[0] = 0x200u;
    f.cpu->a[1] = 0x300u;
    return f;
}

static void expect_timing(const char *id, uint16_t opcode, int clocks, unsigned r, unsigned w) {
    fixture_t f = timing_fixture();
    put_w(f.ram, RESET_PC, opcode);
    int got = step_cycles(&f);
    if (got != clocks || reads(&f) != r || writes(&f) != w) {
        fprintf(stderr, "%s: %d(%u/%u), expected %d(%u/%u)\n", id, got, reads(&f), writes(&f),
                clocks, r, w);
        abort();
    }
    dispose(&f);
}

/* T-TIM-010-01 .. T-TIM-011-06, T-TIM-020-01: nominal clocks and (r/w). */
static void test_nominal_cycles(void) {
    expect_timing("T-TIM-010-01", 0x4E71, 4, 1, 0);  /* NOP */
    expect_timing("T-TIM-011-01", 0x3200, 4, 1, 0);  /* MOVE.W D0,D1 */
    expect_timing("T-TIM-011-02", 0x3210, 8, 2, 0);  /* MOVE.W (A0),D1 */
    expect_timing("T-TIM-011-03", 0x3281, 8, 1, 1);  /* MOVE.W D1,(A1) */
    expect_timing("T-TIM-011-04", 0x3290, 12, 2, 1); /* MOVE.W (A0),(A1) */
    expect_timing("T-TIM-011-05", 0x50C0, 6, 1, 0);  /* ST D0   (condition true) */
    expect_timing("T-TIM-011-06", 0x51C0, 4, 1, 0);  /* SF D0   (condition false) */
    expect_timing("T-TIM-020-01", 0x4280, 6, 1, 0);  /* CLR.L D0 */
}

/* T-TIM-013-02: 分岐・ジャンプ・SR/CCR 即値・STOP の公称クロックと (r/w)。
 * 期待値は UM Table 8-9, 8-10, 8-12 の値。EA 時間は表の脚注どおり加算済み。 */
typedef struct {
    const char *id;
    uint16_t words[3];
    unsigned nwords;
    uint16_t sr;        /* 0 のとき既定の SR を使う */
    uint32_t d0;
    int clocks;
    unsigned r, w;
} flow_timing_t;

static void test_flow_timing(void) {
    static const flow_timing_t rows[] = {
        {"T-TIM-013-02.01 BRA.B", {0x6006}, 1, 0, 0, 10, 2, 0},
        {"T-TIM-013-02.02 BRA.W", {0x6000, 0x0010}, 2, 0, 0, 10, 2, 0},
        {"T-TIM-013-02.03 BNE.B taken", {0x6606}, 1, 0, 0, 10, 2, 0},
        {"T-TIM-013-02.04 BEQ.B not taken", {0x6706}, 1, 0, 0, 8, 1, 0},
        {"T-TIM-013-02.05 BNE.W taken", {0x6600, 0x0010}, 2, 0, 0, 10, 2, 0},
        {"T-TIM-013-02.06 BEQ.W not taken", {0x6700, 0x0010}, 2, 0, 0, 12, 2, 0},
        {"T-TIM-013-02.07 BSR.B", {0x6106}, 1, 0, 0, 18, 2, 2},
        {"T-TIM-013-02.08 BSR.W", {0x6100, 0x0010}, 2, 0, 0, 18, 2, 2},
        {"T-TIM-013-02.09 DBRA not expired", {0x51C8, 0x0010}, 2, 0, 1, 10, 2, 0},
        {"T-TIM-013-02.10 DBRA expired", {0x51C8, 0x0010}, 2, 0, 0, 14, 3, 0},
        {"T-TIM-013-02.11 DBT cc true", {0x50C8, 0x0010}, 2, 0, 1, 12, 2, 0},
        {"T-TIM-013-02.12 JMP (A0)", {0x4ED0}, 1, 0, 0, 8, 2, 0},
        {"T-TIM-013-02.13 JMP (d16,A0)", {0x4EE8, 0x0010}, 2, 0, 0, 10, 2, 0},
        {"T-TIM-013-02.14 JMP (xxx).W", {0x4EF8, 0x1100}, 2, 0, 0, 10, 2, 0},
        {"T-TIM-013-02.15 JMP (xxx).L", {0x4EF9, 0x0000, 0x1100}, 3, 0, 0, 12, 3, 0},
        {"T-TIM-013-02.16 JSR (A0)", {0x4E90}, 1, 0, 0, 16, 2, 2},
        {"T-TIM-013-02.17 JSR (xxx).L", {0x4EB9, 0x0000, 0x1100}, 3, 0, 0, 20, 3, 2},
        {"T-TIM-013-02.18 LEA (A0),A1", {0x43D0}, 1, 0, 0, 4, 1, 0},
        {"T-TIM-013-02.19 PEA (A0)", {0x4850}, 1, 0, 0, 12, 1, 2},
        {"T-TIM-013-02.20 MOVE #,SR", {0x46FC, 0x2700}, 2, 0, 0, 16, 3, 0},
        {"T-TIM-013-02.21 ORI #,SR", {0x007C, 0x2000}, 2, 0, 0, 20, 3, 0},
        {"T-TIM-013-02.22 ANDI #,SR", {0x027C, 0x2700}, 2, 0, 0, 20, 3, 0},
        {"T-TIM-013-02.23 EORI #,SR", {0x0A7C, 0x0000}, 2, 0, 0, 20, 3, 0},
        {"T-TIM-013-02.24 ANDI #,CCR", {0x023C, 0x00FF}, 2, 0, 0, 20, 3, 0},
        {"T-TIM-013-02.25 ORI #,CCR", {0x003C, 0x0000}, 2, 0, 0, 20, 3, 0},
        {"T-TIM-013-02.26 EORI #,CCR", {0x0A3C, 0x0000}, 2, 0, 0, 20, 3, 0},
        {"T-TIM-013-02.27 MOVE D0,CCR", {0x44C0}, 1, 0, 0, 12, 1, 0},
        {"T-TIM-013-02.28 MOVE D0,SR", {0x46C0}, 1, 0, 0x2700, 12, 2, 0},
        {"T-TIM-013-02.29 STOP", {0x4E72, 0x2700}, 2, 0, 0, 4, 0, 0},
        {"T-TIM-013-02.30 JMP (d8,An,Xn)", {0x4EF0, 0x0000}, 2, 0, 0, 14, 3, 0},
        {"T-TIM-013-02.31 JMP (d8,PC,Xn)", {0x4EFB, 0x0000}, 2, 0, 0, 14, 3, 0},
        {"T-TIM-013-02.32 JSR (d8,An,Xn)", {0x4EB0, 0x0000}, 2, 0, 0, 22, 2, 2},
        {"T-TIM-013-02.33 LEA (d8,An,Xn),A1", {0x43F0, 0x0000}, 2, 0, 0, 12, 2, 0},
        {"T-TIM-013-02.34 PEA (d8,An,Xn)", {0x4870, 0x0000}, 2, 0, 0, 20, 2, 2},
        {"T-TIM-013-02.35 CLR.W (A1)", {0x4251}, 1, 0, 0, 12, 2, 1},
        {"T-TIM-013-02.36 CLR.L (A1)", {0x4291}, 1, 0, 0, 20, 3, 2},
        {"T-TIM-013-02.37 ST (A1)", {0x50D1}, 1, 0, 0, 12, 2, 1},
        {"T-TIM-013-02.38 SF (A1)", {0x51D1}, 1, 0, 0, 12, 2, 1},
        {"T-TIM-013-02.39 MOVE SR,(A1)", {0x40D1}, 1, 0, 0, 12, 2, 1},
        {"T-TIM-013-02.40 MOVEM.W (A0),D0-D1", {0x4C90, 0x0003}, 2, 0, 0, 20, 5, 0},
        {"T-TIM-013-02.41 MOVEM.L (A0),D0-D1", {0x4CD0, 0x0003}, 2, 0, 0, 28, 7, 0},
        {"T-TIM-013-02.42 MOVEM.W (A0)+,D0-D1", {0x4C98, 0x0003}, 2, 0, 0, 20, 5, 0},
        {"T-TIM-013-02.43 MOVEM.L (A0)+,D0-D1", {0x4CD8, 0x0003}, 2, 0, 0, 28, 7, 0},
        {"T-TIM-013-02.44 MOVEM.W (xxx).W,D0-D1", {0x4CB8, 0x0003, 0x0200}, 3, 0, 0, 24, 6, 0},
        {"T-TIM-013-02.45 TRAP #0", {0x4E40}, 1, 0, 0, 34, 4, 3},
        {"T-TIM-013-02.46 CHK.W D1,D0 (trap)", {0x4181}, 1, 0, 0xFFFFFFFFu, 40, 4, 3},
        {"T-TIM-013-02.47 ILLEGAL", {0x4AFC}, 1, 0, 0, 34, 4, 3},
    };
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const flow_timing_t *row = &rows[i];
        fixture_t f = timing_fixture();
        program(&f, row->words, row->nwords);
        f.cpu->d[0] = row->d0;
        if (row->sr)
            f.cpu->sr = row->sr;
        int got = step_cycles(&f);
        if (got != row->clocks || reads(&f) != row->r || writes(&f) != row->w) {
            fprintf(stderr, "%s: %d(%u/%u), expected %d(%u/%u)\n", row->id, got, reads(&f),
                    writes(&f), row->clocks, row->r, row->w);
            bad++;
        }
        dispose(&f);
    }
    assert(bad == 0);
}

/* T-TIM-014: ADD／ADDA／SUB／SUBA／AND／OR／EOR／ANDI／ORI／EORI／CHK／MULU／MULS のクロック数（UM Table 8-4, 8-6 と脚注）。
 * 期待値は UM の基本時間に Table 8-1 の EA 時間を加算した値。A0=$200（RAM は 0）、A1=$300。
 * MULU は 38+2n（n は元オペランドの 1 のビット数）、MULS は 38+2n（n は末尾に 0 を足した
 * オペランドの 01／10 の組の数）。 */
typedef struct {
    const char *id;
    uint16_t words[3];
    unsigned nwords;
    uint32_t d0, d1;
    int clocks;
    unsigned r, w;
} alu_timing_t;

static void test_alu_timing(void) {
    static const alu_timing_t rows[] = {
        {"T-TIM-014-01 ADD.W (A0),D1", {0xD250}, 1, 0, 0, 8, 2, 0},
        {"T-TIM-014-02 ADD.L (A0),D1", {0xD290}, 1, 0, 0, 14, 3, 0},
        {"T-TIM-014-03 ADD.L D0,D1", {0xD280}, 1, 0, 0, 8, 1, 0},
        {"T-TIM-014-04 ADD.L #imm,D1", {0xD2BC, 0x0000}, 2, 0, 0, 16, 3, 0},
        {"T-TIM-014-05 ADD.W (d16,A0),D1", {0xD268, 0x0010}, 2, 0, 0, 12, 3, 0},
        {"T-TIM-014-06 ADD.L (d16,A0),D1", {0xD2A8, 0x0010}, 2, 0, 0, 18, 4, 0},
        {"T-TIM-014-07 ADD.W D1,(A1)", {0xD351}, 1, 0, 0, 12, 2, 1},
        {"T-TIM-014-08 ADD.L D1,(A1)", {0xD391}, 1, 0, 0, 20, 3, 2},
        {"T-TIM-014-09 ADDA.W (A0),A2", {0xD4D0}, 1, 0, 0, 12, 2, 0},
        {"T-TIM-014-10 ADDA.L (A0),A2", {0xD5D0}, 1, 0, 0, 14, 3, 0},
        {"T-TIM-014-11 ADDA.L D0,A2", {0xD5C0}, 1, 0, 0, 8, 1, 0},
        {"T-TIM-014-12 ADDA.L #imm,A2", {0xD5FC, 0x0000}, 2, 0, 0, 16, 3, 0},
        {"T-TIM-014-13 CHK.W D1,D0", {0x4181}, 1, 0, 0x10, 10, 1, 0},
        {"T-TIM-014-14 CHK.W (A0),D0", {0x4190}, 1, 0, 0, 14, 2, 0},
        {"T-TIM-014-15 CHK.W (d16,A0),D0", {0x41A8, 0x0010}, 2, 0, 0, 18, 3, 0},
        {"T-TIM-014-16 MULU.W D1,D0 n=0", {0xC0C1}, 1, 0, 0x0000, 38, 1, 0},
        {"T-TIM-014-17 MULU.W D1,D0 n=16", {0xC0C1}, 1, 0, 0xFFFF, 70, 1, 0},
        {"T-TIM-014-18 MULU.W D1,D0 n=3", {0xC0C1}, 1, 0, 0x8401, 44, 1, 0},
        {"T-TIM-014-19 MULU.W D1,D0 上位語は数えない", {0xC0C1}, 1, 0, 0xFFFF0000u, 38, 1, 0},
        {"T-TIM-014-20 MULU.W (A0),D0 n=0", {0xC0D0}, 1, 0, 0, 42, 2, 0},
        {"T-TIM-014-21 MULU.W #imm,D0 n=1", {0xC0FC, 0x0001}, 2, 0, 0, 44, 2, 0},
        {"T-TIM-014-22 SUB.W (A0),D1", {0x9250}, 1, 0, 0, 8, 2, 0},
        {"T-TIM-014-23 SUB.L (A0),D1", {0x9290}, 1, 0, 0, 14, 3, 0},
        {"T-TIM-014-24 SUB.L D0,D1", {0x9280}, 1, 0, 0, 8, 1, 0},
        {"T-TIM-014-25 SUB.L #imm,D1", {0x92BC, 0x0000}, 2, 0, 0, 16, 3, 0},
        {"T-TIM-014-26 SUB.W (d16,A0),D1", {0x9268, 0x0010}, 2, 0, 0, 12, 3, 0},
        {"T-TIM-014-27 SUB.W D1,(A1)", {0x9351}, 1, 0, 0, 12, 2, 1},
        {"T-TIM-014-28 SUB.L D1,(A1)", {0x9391}, 1, 0, 0, 20, 3, 2},
        {"T-TIM-014-29 SUBA.W (A0),A2", {0x94D0}, 1, 0, 0, 12, 2, 0},
        {"T-TIM-014-30 SUBA.L (A0),A2", {0x95D0}, 1, 0, 0, 14, 3, 0},
        {"T-TIM-014-31 SUBA.L D0,A2", {0x95C0}, 1, 0, 0, 8, 1, 0},
        {"T-TIM-014-32 SUBA.L #imm,A2", {0x95FC, 0x0000}, 2, 0, 0, 16, 3, 0},
        {"T-TIM-014-33 MULS.W D1,D0 n=0", {0xC1C1}, 1, 0, 0x0000, 38, 1, 0},
        {"T-TIM-014-34 MULS.W D1,D0 n=1 (0x0001)", {0xC1C1}, 1, 0, 0x0001, 42, 1, 0},
        {"T-TIM-014-35 MULS.W D1,D0 n=16 (0x5555)", {0xC1C1}, 1, 0, 0x5555, 70, 1, 0},
        {"T-TIM-014-36 MULS.W D1,D0 n=1 (0xFFFF)", {0xC1C1}, 1, 0, 0xFFFF, 40, 1, 0},
        {"T-TIM-014-37 MULS.W D1,D0 上位語は数えない", {0xC1C1}, 1, 0, 0xFFFF0000u, 38, 1, 0},
        {"T-TIM-014-38 MULS.W (A0),D0 n=0", {0xC1D0}, 1, 0, 0, 42, 2, 0},
        {"T-TIM-014-39 AND.W (A0),D1", {0xC250}, 1, 0, 0, 8, 2, 0},
        {"T-TIM-014-40 AND.L (A0),D1", {0xC290}, 1, 0, 0, 14, 3, 0},
        {"T-TIM-014-41 AND.L D0,D1", {0xC280}, 1, 0, 0, 8, 1, 0},
        {"T-TIM-014-42 AND.L #imm,D1", {0xC2BC, 0x0000}, 2, 0, 0, 16, 3, 0},
        {"T-TIM-014-43 AND.W (d16,A0),D1", {0xC268, 0x0010}, 2, 0, 0, 12, 3, 0},
        {"T-TIM-014-44 AND.W D1,(A1)", {0xC351}, 1, 0, 0, 12, 2, 1},
        {"T-TIM-014-45 AND.L D1,(A1)", {0xC391}, 1, 0, 0, 20, 3, 2},
        {"T-TIM-014-46 OR.W (A0),D1", {0x8250}, 1, 0, 0, 8, 2, 0},
        {"T-TIM-014-47 OR.L (A0),D1", {0x8290}, 1, 0, 0, 14, 3, 0},
        {"T-TIM-014-48 OR.L D0,D1", {0x8280}, 1, 0, 0, 8, 1, 0},
        {"T-TIM-014-49 OR.L #imm,D1", {0x82BC, 0x0000}, 2, 0, 0, 16, 3, 0},
        {"T-TIM-014-50 OR.W D1,(A1)", {0x8351}, 1, 0, 0, 12, 2, 1},
        {"T-TIM-014-51 OR.L D1,(A1)", {0x8391}, 1, 0, 0, 20, 3, 2},
        {"T-TIM-014-52 EOR.W D1,D0", {0xB340}, 1, 0, 0, 4, 1, 0},
        {"T-TIM-014-53 EOR.L D1,D0", {0xB380}, 1, 0, 0, 8, 1, 0},
        {"T-TIM-014-54 EOR.W D1,(A1)", {0xB351}, 1, 0, 0, 12, 2, 1},
        {"T-TIM-014-55 EOR.L D1,(A1)", {0xB391}, 1, 0, 0, 20, 3, 2},
        {"T-TIM-014-56 EOR.W D1,(d16,A1)", {0xB369, 0x0010}, 2, 0, 0, 16, 3, 1},
        {"T-TIM-014-57 ANDI.W #,D1", {0x0241, 0x0001}, 2, 0, 0, 8, 2, 0},
        {"T-TIM-014-58 ANDI.L #,D1", {0x0281, 0x0000, 0x0001}, 3, 0, 0, 14, 3, 0},
        {"T-TIM-014-59 ANDI.W #,(A1)", {0x0251, 0x0001}, 2, 0, 0, 16, 3, 1},
        {"T-TIM-014-60 ANDI.L #,(A1)", {0x0291, 0x0000, 0x0001}, 3, 0, 0, 28, 5, 2},
        {"T-TIM-014-61 ANDI.W #,(d16,A1)", {0x0269, 0x0001, 0x0010}, 3, 0, 0, 20, 4, 1},
        {"T-TIM-014-62 ORI.W #,D1", {0x0041, 0x0001}, 2, 0, 0, 8, 2, 0},
        {"T-TIM-014-63 ORI.L #,D1", {0x0081, 0x0000, 0x0001}, 3, 0, 0, 16, 3, 0},
        {"T-TIM-014-64 ORI.W #,(A1)", {0x0051, 0x0001}, 2, 0, 0, 16, 3, 1},
        {"T-TIM-014-65 ORI.L #,(A1)", {0x0091, 0x0000, 0x0001}, 3, 0, 0, 28, 5, 2},
        {"T-TIM-014-66 ORI.W #,(d16,A1)", {0x0069, 0x0001, 0x0010}, 3, 0, 0, 20, 4, 1},
        {"T-TIM-014-67 EORI.W #,D1", {0x0A41, 0x0001}, 2, 0, 0, 8, 2, 0},
        {"T-TIM-014-68 EORI.L #,D1", {0x0A81, 0x0000, 0x0001}, 3, 0, 0, 16, 3, 0},
        {"T-TIM-014-69 EORI.W #,(A1)", {0x0A51, 0x0001}, 2, 0, 0, 16, 3, 1},
        {"T-TIM-014-70 EORI.L #,(A1)", {0x0A91, 0x0000, 0x0001}, 3, 0, 0, 28, 5, 2},
        {"T-TIM-014-71 EORI.W #,(d16,A1)", {0x0A69, 0x0001, 0x0010}, 3, 0, 0, 20, 4, 1},
    };
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const alu_timing_t *row = &rows[i];
        fixture_t f = timing_fixture();
        program(&f, row->words, row->nwords);
        f.cpu->d[0] = row->d0;
        f.cpu->d[1] = row->d1;
        int got = step_cycles(&f);
        if (got != row->clocks || reads(&f) != row->r || writes(&f) != row->w) {
            fprintf(stderr, "%s: %d(%u/%u), expected %d(%u/%u)\n", row->id, got, reads(&f),
                    writes(&f), row->clocks, row->r, row->w);
            bad++;
        }
        dispose(&f);
    }
    assert(bad == 0);
}

/* T-TIM-012-01: wait states add clocks but not bus cycles. */
static void test_wait_states(void) {
    fixture_t f = timing_fixture();
    f.data_read_wait = 2;
    put_w(f.ram, RESET_PC, 0x3210); /* MOVE.W (A0),D1 */
    int got = step_cycles(&f);
    assert(got == 10);
    assert(reads(&f) == 2 && writes(&f) == 0);
    dispose(&f);
}

/* T-TIM-013-01: MOVE.W D1,(A1) の後、次命令語を補充する。 */
static void test_bus_trace_order(void) {
    fixture_t f = timing_fixture();
    put_w(f.ram, RESET_PC, 0x3281);
    step_cycles(&f);
    assert(f.count == 2);
    assert(!f.log[0].instruction && !f.log[0].read);
    assert(f.log[0].address == 0x300u && f.log[0].size == 2);
    assert(f.log[1].instruction && f.log[1].read);
    dispose(&f);
}

/* Index of the operand read of TAS (A0), or -1. */
static int operand_cycle(const fixture_t *f, bool read) {
    for (unsigned i = 0; i < f->count; i++)
        if (!f->log[i].instruction && f->log[i].read == read && f->log[i].address == 0x200u)
            return (int)i;
    return -1;
}

/* T-TIM-030-01 and T-TIM-032-01: TAS (A0) is read8 then write8(old|$80), with
 * no other bus cycle between them. T-TIM-030-01 additionally requires the RMW
 * attribute on both cycles. */
static void test_tas_sequence(void) {
    fixture_t f = timing_fixture();
    f.ram[0x200] = 0x12;
    put_w(f.ram, RESET_PC, 0x4AD0); /* TAS (A0) */
    step_cycles(&f);
    int r = operand_cycle(&f, true), w = operand_cycle(&f, false);
    assert(r >= 0 && w >= 0);
    assert(f.log[r].size == 1 && f.log[w].size == 1);
    assert(w == r + 1); /* T-TIM-032-01: contiguous, nothing in between */
    assert(f.ram[0x200] == 0x92);
    assert(f.log[r].rmw && f.log[w].rmw); /* T-TIM-030-01 */
    assert(!(f.cpu->sr & SR_N));            /* TAS sets N from the old value ($12: N=0) */
    dispose(&f);
}

/* T-TIM-031-01: a bus error in the TAS read is not retried; bus-error frame. */
static void test_tas_bus_error(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    f.cpu->a[0] = BERR_BASE + 0x100u;
    put_w(f.ram, RESET_PC, 0x4AD0);
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER);
    assert(f.cpu->a[7] == RESET_SSP - 14u);
    unsigned operand_reads = 0, operand_writes = 0;
    for (unsigned i = 0; i < f.count; i++)
        if (!f.log[i].instruction && (f.log[i].address & ADDR_MASK) == BERR_BASE + 0x100u) {
            if (f.log[i].read)
                operand_reads++;
            else
                operand_writes++;
        }
    assert(operand_reads == 1 && operand_writes == 0);
    dispose(&f);
}

/* T-TIM-041-01: instruction fetch FC is %110 in supervisor and %010 in user. */
static void test_fetch_fc(void) {
    fixture_t f = timing_fixture();
    put_w(f.ram, RESET_PC, 0x4E71);
    step_cycles(&f);
    assert(f.count >= 1 && f.log[0].instruction && f.log[0].fc == FC_SPROG);
    dispose(&f);

    fixture_t g = timing_fixture();
    put_w(g.ram, RESET_PC, 0x4E71);
    g.cpu->sr = 0x0000u;
    g.cpu->usp = USER_SP;
    g.cpu->a[7] = USER_SP;
    step_cycles(&g);
    assert(g.count >= 1 && g.log[0].instruction && g.log[0].fc == FC_UPROG);
    dispose(&g);
}

/* バスエラー frame の saved PC は PC0+2 .. PC0+10 の偶数値（UM 6.3.9.1）。 */
static void expect_saved_pc_in_range(const fixture_t *f, const char *id) {
    uint32_t saved = ((uint32_t)f->ram[RESET_SSP - 14u + 10u] << 24) |
                     ((uint32_t)f->ram[RESET_SSP - 14u + 11u] << 16) |
                     ((uint32_t)f->ram[RESET_SSP - 14u + 12u] << 8) | f->ram[RESET_SSP - 14u + 13u];
    if (saved < RESET_PC + 2u || saved > RESET_PC + 10u || (saved & 1u)) {
        fprintf(stderr, "%s: saved PC = %08x, expected %04x..%04x\n", id, saved, RESET_PC + 2u,
                RESET_PC + 10u);
        abort();
    }
}

/* T-TIM-040-01: データアクセス bus error の saved PC は PC0+2 .. PC0+10。 */
static void test_saved_pc_range(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    f.cpu->a[0] = BERR_BASE;
    put_w(f.ram, RESET_PC, 0x3210); /* MOVE.W (A0),D1 */
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->a[7] == RESET_SSP - 14u);
    expect_saved_pc_in_range(&f, "T-TIM-040-01 (read)");
    dispose(&f);

    fixture_t g = timing_fixture();
    put_l(g.ram, VEC_BUS_ERROR * 4u, HANDLER);
    g.cpu->a[0] = BERR_BASE;
    put_w(g.ram, RESET_PC, 0x3080); /* MOVE.W D0,(A0) */
    step_cycles(&g);
    assert(g.cpu->pc == HANDLER && g.cpu->a[7] == RESET_SSP - 14u);
    expect_saved_pc_in_range(&g, "T-TIM-040-01 (write)");
    dispose(&g);
}

/* T-TIM-040-02: 書込み先が直後の命令語でも、既に先読み済みの旧語を実行する。 */
static void test_self_modifying_next_word_uses_prefetch(void) {
    fixture_t f = timing_fixture();
    PROGRAM(&f, 0x31C0, 0x1004, 0x4E71); /* MOVE.W D0,($1004).W ; NOP */
    f.cpu->d[0] = 0x00004AFCu;            /* メモリ上の次語を ILLEGAL へ書換える */
    step_cycles(&f);
    assert(((uint16_t)f.ram[RESET_PC + 4] << 8 | f.ram[RESET_PC + 5]) == 0x4AFCu);
    step_cycles(&f);
    assert(f.cpu->pc == RESET_PC + 6u); /* 先読み済み NOP を実行した */
    dispose(&f);
}

/* T-TIM-040-04: 最後の補充は命令のデータアクセスより後に行う。書込み先が補充される
 * 2語目なら、書換え後の語を取得して実行する。 */
static void test_last_refill_follows_data_access(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, 4u * 4u, HANDLER);
    PROGRAM(&f, 0x31C0, 0x1006, 0x4E71, 0x4E71); /* MOVE.W D0,($1006).W ; NOP ; NOP */
    f.cpu->d[0] = 0x00004AFCu;                     /* $1006 を ILLEGAL へ書換える */
    step_cycles(&f);
    assert(f.count == 3);
    assert(f.log[0].read && f.log[0].instruction && (f.log[0].address & ADDR_MASK) == 0x1004u);
    assert(!f.log[1].read && (f.log[1].address & ADDR_MASK) == 0x1006u);
    assert(f.log[2].read && f.log[2].instruction && (f.log[2].address & ADDR_MASK) == 0x1006u);
    step_cycles(&f);
    assert(f.cpu->pc == RESET_PC + 6u); /* $1004 の NOP */
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER);       /* 書換え後の語（ILLEGAL）を実行した */
    dispose(&f);
}

/* T-TIM-040-05: 分岐先の次命令フェッチが失敗した場合、saved PC は分岐命令の近傍
 * （PC0+2 .. PC0+10 の偶数）になる（UM §6.3.9.1）。bus error と address error の両方。 */
static void test_branch_target_fetch_fault_saved_pc(void) {
    static const struct {
        const char *id;
        uint32_t target;
        uint32_t vector;
    } rows[] = {
        {"T-TIM-040-05 (bus error)", BERR_BASE, VEC_BUS_ERROR},
        {"T-TIM-040-05 (address error)", 0x3001u, VEC_ADDR_ERR},
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        fixture_t f = timing_fixture();
        put_l(f.ram, rows[i].vector * 4u, HANDLER);
        PROGRAM(&f, 0x4EF9, (uint16_t)(rows[i].target >> 16), (uint16_t)rows[i].target);
        step_cycles(&f); /* JMP abs.L。分岐先の先読みの失敗は JMP の中で起きる */
        assert(f.cpu->pc == HANDLER && f.cpu->a[7] == RESET_SSP - 14u);
        expect_saved_pc_in_range(&f, rows[i].id);
        dispose(&f);
    }
}

/* ---- 例外処理中のフォルト（UM §6.3.9.1, §5.4.4） ---- */

static uint32_t stacked_pc(const fixture_t *f, uint32_t sp) {
    return ((uint32_t)f->ram[sp + 10u] << 24) | ((uint32_t)f->ram[sp + 11u] << 16) |
           ((uint32_t)f->ram[sp + 12u] << 8) | f->ram[sp + 13u];
}

/* T-EXC-030-05: Group 1/2 例外のベクタ読出しが失敗すると、ベクタのアドレスを saved PC とする
 * bus error 例外を取る（HALT しない）。 */
static void test_vector_read_fault_takes_bus_error(void) {
    fixture_t f = timing_fixture();
    f.berr_lo = 0x80u; /* TRAP #0 のベクタ（vector 32） */
    f.berr_hi = 0x84u;
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&f, 0x4E40); /* TRAP #0 */
    step_cycles(&f);
    assert(f.cpu->exec_state != CPU_HALTED);
    assert(f.cpu->pc == HANDLER && f.cpu->a[7] == RESET_SSP - 6u - 14u);
    assert(stacked_pc(&f, f.cpu->a[7]) == 0x80u);
    dispose(&f);
}

/* T-EXC-030-06: Group 1/2 例外のハンドラ先頭の命令フェッチが失敗すると、ベクタのアドレスを
 * saved PC とする bus error 例外を取る。 */
static void test_handler_fetch_fault_takes_bus_error(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, 32u * 4u, BERR_BASE);
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&f, 0x4E40); /* TRAP #0 */
    step_cycles(&f); /* TRAP の処理の最終段（handler 先頭の命令語の取得）で失敗する */
    assert(f.cpu->exec_state != CPU_HALTED);
    assert(f.cpu->pc == HANDLER && f.cpu->a[7] == RESET_SSP - 6u - 14u);
    assert(stacked_pc(&f, f.cpu->a[7]) == 32u * 4u);
    dispose(&f);
}

/* T-EXC-030-03: bus error 例外のベクタ読出しの失敗は二重フォルトで HALT する。 */
static void test_group0_vector_read_fault_halts(void) {
    fixture_t f = timing_fixture();
    f.berr_lo = VEC_BUS_ERROR * 4u;
    f.berr_hi = VEC_BUS_ERROR * 4u + 4u;
    f.cpu->a[0] = BERR_BASE;
    PROGRAM(&f, 0x3210); /* MOVE.W (A0),D1 */
    step_cycles(&f);
    assert(f.cpu->exec_state == CPU_HALTED);
    dispose(&f);
}

/* T-EXC-030-04: bus error 例外のハンドラ先頭の命令フェッチの失敗も二重フォルトで HALT する。
 * 1命令目の step の中で起きる。 */
static void test_group0_handler_fetch_fault_halts(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_BUS_ERROR * 4u, BERR_BASE);
    f.cpu->a[0] = BERR_BASE;
    PROGRAM(&f, 0x3210); /* MOVE.W (A0),D1 */
    step_cycles(&f); /* bus error 例外の処理の最終段（handler 先頭の取得）で失敗する */
    assert(f.cpu->exec_state == CPU_HALTED);
    dispose(&f);
}

static uint16_t stacked_ssw(const fixture_t *f, uint32_t sp) {
    return (uint16_t)(((uint16_t)f->ram[sp] << 8) | f->ram[sp + 1u]);
}

#define SSW_NOT_INSTRUCTION 0x0008u
#define IRQ_LEVEL3          3
#define VEC_IRQ3            27u
#define VEC_TRACE           9u
#define VEC_ILLEGAL         4u

/* T-EXC-030-07: 分岐先の先読みの失敗は分岐命令の中で起きる。T=1 でも trace 例外にならず
 * bus error 例外になる（命令が bus error で中断されるとトレースは起きない）。 */
static void test_branch_fetch_fault_beats_trace(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    put_l(f.ram, VEC_TRACE * 4u, HANDLER + 0x40u);
    f.cpu->sr = 0xA000u; /* T=1, S=1 */
    PROGRAM(&f, 0x4EF9, 0x00F0, 0x0000); /* JMP $F00000 */
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == VEC_BUS_ERROR);
    dispose(&f);
}

/* T-EXC-030-08: 割り込みのマスクを解除する命令の中で先読みが失敗した場合、保留中の割り込みより
 * 先に bus error が処理される。MOVE #$2000,SR の直後が BERR 領域にある。 */
static void test_prefetch_fault_beats_pending_irq(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    put_l(f.ram, VEC_IRQ3 * 4u, HANDLER + 0x80u);
    f.cpu->sr = 0x2300u; /* mask=3 */
    mc68k_set_irq(f.cpu, IRQ_LEVEL3);
    f.cpu->pc = BERR_BASE - 4u;
    put_w(f.ram, BERR_BASE - 4u, 0x46FC); /* MOVE #$2000,SR */
    put_w(f.ram, BERR_BASE - 2u, 0x2000);
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == VEC_BUS_ERROR);
    dispose(&f);
}

/* T-EXC-030-09: 使われない語の先読みの失敗も bus error になる。NOP が BERR 領域の直前にある。 */
static void test_unused_prefetch_fault_raises_bus_error(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    f.cpu->pc = BERR_BASE - 2u;
    put_w(f.ram, BERR_BASE - 2u, 0x4E71); /* NOP。次の語（BERR_BASE）の先読みが失敗する */
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == VEC_BUS_ERROR);
    dispose(&f);
}

/* IRQ 入力を、最初の標本化（命令境界）では0、以降は3にするフック。命令の途中で割り込みが
 * 保留になった状況を作る。 */
static int irq_after_first_sample(void *ctx) {
    fixture_t *f = ctx;
    return f->irq_samples++ == 0 ? 0 : IRQ_LEVEL3;
}

/* T-EXC-030-10: 命令の中で割り込みが保留になっても、Group 0 例外の処理の最終段の失敗
 * （二重フォルト）は HALT になる。TRAP の最終段の失敗は、割り込みより先に bus error 例外に
 * なる [UM §6.3.9.1]。 */
static void test_exception_entry_fault_beats_irq(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_BUS_ERROR * 4u, BERR_BASE);
    put_l(f.ram, VEC_IRQ3 * 4u, HANDLER + 0x80u);
    f.cpu->sr = 0x2000u;
    f.cpu->a[0] = BERR_BASE;
    mc68k_set_irq_level_hook(f.cpu, irq_after_first_sample, &f);
    PROGRAM(&f, 0x3210); /* MOVE.W (A0),D1 */
    step_cycles(&f);
    assert(f.cpu->exec_state == CPU_HALTED);
    dispose(&f);

    fixture_t g = timing_fixture();
    put_l(g.ram, 32u * 4u, BERR_BASE);
    put_l(g.ram, VEC_BUS_ERROR * 4u, HANDLER);
    put_l(g.ram, VEC_IRQ3 * 4u, HANDLER + 0x80u);
    g.cpu->sr = 0x2000u;
    mc68k_set_irq_level_hook(g.cpu, irq_after_first_sample, &g);
    PROGRAM(&g, 0x4E40); /* TRAP #0 */
    step_cycles(&g);
    assert(g.cpu->pc == HANDLER && g.cpu->last_exception_vector == VEC_BUS_ERROR);
    assert(stacked_pc(&g, g.cpu->a[7]) == 32u * 4u);
    dispose(&g);
}

/* T-EXC-030-11: Group 0／Group 1 例外の処理中の bus error は SSW の I/N=1（命令処理中でない）、
 * Group 2（TRAP）は I/N=0 [UM §6.3.9.1]。 */
static void test_exception_entry_fault_ssw_in(void) {
    fixture_t f = timing_fixture();
    f.berr_lo = VEC_ILLEGAL * 4u;
    f.berr_hi = VEC_ILLEGAL * 4u + 4u;
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&f, 0x4AFC); /* ILLEGAL（Group 1）。vector 4 の読出しが失敗する */
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER);
    assert((stacked_ssw(&f, f.cpu->a[7]) & SSW_NOT_INSTRUCTION) != 0);
    dispose(&f);

    fixture_t g = timing_fixture();
    g.berr_lo = VEC_IRQ3 * 4u;
    g.berr_hi = VEC_IRQ3 * 4u + 4u;
    put_l(g.ram, VEC_BUS_ERROR * 4u, HANDLER);
    g.cpu->sr = 0x2000u;
    mc68k_set_irq(g.cpu, IRQ_LEVEL3);
    PROGRAM(&g, 0x4E71);
    step_cycles(&g); /* 割り込みの vector 読出しが失敗する（Group 1） */
    assert(g.cpu->pc == HANDLER);
    assert((stacked_ssw(&g, g.cpu->a[7]) & SSW_NOT_INSTRUCTION) != 0);
    dispose(&g);

    fixture_t h = timing_fixture();
    h.berr_lo = 32u * 4u;
    h.berr_hi = 32u * 4u + 4u;
    put_l(h.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&h, 0x4E40); /* TRAP #0（Group 2） */
    step_cycles(&h);
    assert(h.cpu->pc == HANDLER);
    assert((stacked_ssw(&h, h.cpu->a[7]) & SSW_NOT_INSTRUCTION) == 0);
    dispose(&h);
}

/* T-EXC-030-12: 例外ベクタごとの SSW I/N。Group 0／1（illegal、privilege、trace、Line-A／F、
 * 割り込み）は命令処理中でない（I/N=1）、Group 2（TRAP、TRAPV、CHK、ゼロ除算）は 0。
 * ベクタの読出しが BERR になる構成で、スタック上の SSW bit3 を比較する。 */
static void test_vector_group_ssw_in(void) {
    static const struct {
        const char *id;
        uint16_t words[2];
        unsigned nwords;
        uint16_t sr;
        uint32_t d0;
        uint32_t vector;
        bool not_instruction;
    } rows[] = {
        {"illegal", {0x4AFC}, 1, 0x2000, 0, 4, true},
        {"privilege", {0x4E72, 0x2700}, 2, 0x0000, 0, 8, true},
        {"trace", {0x4E71}, 1, 0xA000, 0, 9, true},
        {"line-A", {0xA000}, 1, 0x2000, 0, 10, true},
        {"line-F", {0xF000}, 1, 0x2000, 0, 11, true},
        {"CHK", {0x4181}, 1, 0x2000, 0xFFFFFFFFu, 6, false},
        {"TRAPV", {0x4E76}, 1, 0x2002, 0, 7, false},
        {"DIVU #0", {0x80FC, 0x0000}, 2, 0x2000, 1, 5, false},
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        fixture_t f = timing_fixture();
        f.berr_lo = rows[i].vector * 4u;
        f.berr_hi = rows[i].vector * 4u + 4u;
        put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
        program(&f, rows[i].words, rows[i].nwords);
        f.cpu->sr = rows[i].sr;
        f.cpu->d[0] = rows[i].d0;
        step_cycles(&f);
        if (f.cpu->pc != HANDLER || f.cpu->last_exception_vector != VEC_BUS_ERROR ||
            f.cpu->d[0] != rows[i].d0 || /* CHK／DIVU の本来の処理は行われない */
            ((stacked_ssw(&f, f.cpu->a[7]) & SSW_NOT_INSTRUCTION) != 0) != rows[i].not_instruction ||
            stacked_pc(&f, f.cpu->a[7]) != rows[i].vector * 4u) {
            fprintf(stderr, "T-EXC-030-12 %s: pc=%X ssw=%04X saved pc=%X\n", rows[i].id, f.cpu->pc,
                    stacked_ssw(&f, f.cpu->a[7]), stacked_pc(&f, f.cpu->a[7]));
            abort();
        }
        dispose(&f);
    }
    /* 割り込み（vector 25〜31、level 1〜7）はすべて Group 1 */
    for (uint32_t vec = 25; vec <= 31; vec++) {
        fixture_t f = timing_fixture();
        f.berr_lo = vec * 4u;
        f.berr_hi = vec * 4u + 4u;
        put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
        f.cpu->sr = 0x2000u;
        mc68k_set_irq(f.cpu, (int)(vec - 24u));
        PROGRAM(&f, 0x4E71);
        step_cycles(&f);
        assert(f.cpu->pc == HANDLER);
        assert((stacked_ssw(&f, f.cpu->a[7]) & SSW_NOT_INSTRUCTION) != 0);
        assert(stacked_pc(&f, f.cpu->a[7]) == vec * 4u);
        dispose(&f);
    }
}

/* T-EXC-030-13: handler 先頭の取得の失敗（Group 1）は I/N=1、saved PC=vector。IRQ の場合、
 * handler 先頭の2語は handler のアドレスから読み、割り込まれた側の PC を読まない。
 * 失敗の後の通常命令の bus error は I/N=0 に戻る。 */
static void test_handler_fetch_fault_in_and_irq_order(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_ILLEGAL * 4u, BERR_BASE);
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    f.cpu->a[0] = BERR_BASE;
    put_w(f.ram, HANDLER, 0x3210); /* MOVE.W (A0),D1（bus error を起こす） */
    PROGRAM(&f, 0x4AFC);           /* ILLEGAL */
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER);
    assert((stacked_ssw(&f, f.cpu->a[7]) & SSW_NOT_INSTRUCTION) != 0);
    assert(stacked_pc(&f, f.cpu->a[7]) == VEC_ILLEGAL * 4u);
    step_cycles(&f); /* handler の MOVE.W (A0),D1 が通常の bus error になる */
    assert(f.cpu->pc == HANDLER);
    assert((stacked_ssw(&f, f.cpu->a[7]) & SSW_NOT_INSTRUCTION) == 0);
    dispose(&f);

    /* IRQ: handler 先頭の2語を handler から読む（旧 PC の $1000 を読まない） */
    fixture_t g = timing_fixture();
    put_l(g.ram, VEC_IRQ3 * 4u, 0x2000u);
    g.cpu->sr = 0x2000u;
    mc68k_set_irq(g.cpu, IRQ_LEVEL3);
    PROGRAM(&g, 0x4E71);
    cpu_prefetch_prime(g.cpu);
    g.count = 0;
    mc68k_execute(g.cpu, 1);
    assert(g.cpu->pc == 0x2000u);
    for (unsigned i = 0; i < g.count; i++)
        assert(!(g.log[i].instruction && (g.log[i].address & ADDR_MASK) < 0x2000u));
    bool read_2000 = false, read_2002 = false;
    for (unsigned i = 0; i < g.count; i++) {
        read_2000 |= g.log[i].instruction && (g.log[i].address & ADDR_MASK) == 0x2000u;
        read_2002 |= g.log[i].instruction && (g.log[i].address & ADDR_MASK) == 0x2002u;
    }
    assert(read_2000 && read_2002);
    dispose(&g);

    /* handler が BERR 領域にある IRQ: saved PC=vector アドレス、I/N=1 */
    fixture_t h = timing_fixture();
    put_l(h.ram, VEC_IRQ3 * 4u, BERR_BASE);
    put_l(h.ram, VEC_BUS_ERROR * 4u, HANDLER);
    h.cpu->sr = 0x2000u;
    mc68k_set_irq(h.cpu, IRQ_LEVEL3);
    PROGRAM(&h, 0x4E71);
    cpu_prefetch_prime(h.cpu);
    h.count = 0;
    mc68k_execute(h.cpu, 200); /* 同じ呼出し内で bus error の handler の実行に進む */
    assert(h.cpu->pc > HANDLER && h.cpu->exec_state == CPU_NORMAL);
    assert(h.cpu->last_exception_vector == VEC_BUS_ERROR);
    uint32_t sp = h.cpu->a[7];
    assert((stacked_ssw(&h, sp) & SSW_NOT_INSTRUCTION) != 0);
    assert(stacked_pc(&h, sp) == VEC_IRQ3 * 4u);
    dispose(&h);
}

/* T-EXC-030-14: 拡張語の消費時の補充の失敗は、命令のデータアクセスより前に報告される。
 * 書込み・更新は行われず、分岐やゼロ除算より bus error が優先される。 */
static void test_extension_refill_fault_is_immediate(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    f.cpu->a[1] = 0x300u;
    f.cpu->d[0] = 0x1234u;
    f.cpu->pc = BERR_BASE - 4u;
    put_w(f.ram, BERR_BASE - 4u, 0x3340); /* MOVE.W D0,(d16,A1) */
    put_w(f.ram, BERR_BASE - 2u, 0x0000);
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == VEC_BUS_ERROR);
    assert(f.ram[0x300] == 0 && f.ram[0x301] == 0); /* データ書込みは行われない */
    dispose(&f);

    fixture_t g = timing_fixture();
    put_l(g.ram, VEC_BUS_ERROR * 4u, HANDLER);
    g.cpu->pc = BERR_BASE - 4u;
    put_w(g.ram, BERR_BASE - 4u, 0x80FC); /* DIVU #0,D0 */
    put_w(g.ram, BERR_BASE - 2u, 0x0000);
    step_cycles(&g);
    assert(g.cpu->last_exception_vector == VEC_BUS_ERROR);
    dispose(&g);

    fixture_t h = timing_fixture();
    put_l(h.ram, VEC_BUS_ERROR * 4u, HANDLER);
    h.cpu->pc = BERR_BASE - 4u;
    put_w(h.ram, BERR_BASE - 4u, 0x4EF0); /* JMP (d8,A0,D0) */
    put_w(h.ram, BERR_BASE - 2u, 0x0000);
    step_cycles(&h);
    assert(h.cpu->pc == HANDLER && h.cpu->last_exception_vector == VEC_BUS_ERROR);
    dispose(&h);
}

/* T-EXC-030-15: CLR の書込み前 read が失敗した場合は write を出さず、SSW は read（R=1）。
 * 68010 以降は書込み前 read を行わない。 */
static void test_read_before_write_fault_and_model(void) {
    fixture_t f = timing_fixture();
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    f.berr_lo = 0x300u;
    f.berr_hi = 0x302u;
    f.ram[0x300] = 0x55;
    PROGRAM(&f, 0x4251); /* CLR.W (A1) */
    step_cycles(&f);
    assert(f.cpu->pc == HANDLER);
    assert((stacked_ssw(&f, f.cpu->a[7]) & 0x0010u) != 0); /* read（R=1） */
    assert(f.ram[0x300] == 0x55);
    for (unsigned i = 0; i < f.count; i++) /* CLR 自身の write（$300）は出ない */
        assert(f.log[i].read || (f.log[i].address & ADDR_MASK) != 0x300u);
    dispose(&f);

    fixture_t g = make_fixture();
    start_model(&g, MC68K_MODEL_68010);
    g.cpu->a[1] = 0x300u;
    PROGRAM(&g, 0x4251);
    step_cycles(&g);
    for (unsigned i = 0; i < g.count; i++)
        assert(g.log[i].instruction || !g.log[i].read); /* データ read はない */
    dispose(&g);
}

/* T-TIM-013-02.48: RTE は 20(5/0)（UM Table 8-12）。復帰先の取得が失敗した場合は、分岐先の取得の
 * 失敗として RTE の次のアドレスを saved PC とする bus error 例外になる。 */
static void test_rte_timing_and_target_fault(void) {
    fixture_t f = timing_fixture();
    put_w(f.ram, RESERVED_STACK, 0x2000);    /* 復帰する SR（スーパバイザ） */
    put_l(f.ram, RESERVED_STACK + 2u, 0x2000u); /* 復帰先 PC */
    f.cpu->a[7] = RESERVED_STACK;
    PROGRAM(&f, 0x4E73); /* RTE */
    int got = step_cycles(&f);
    if (got != 20 || reads(&f) != 5 || writes(&f) != 0) {
        fprintf(stderr, "T-TIM-013-02.48 RTE: %d(%u/%u), expected 20(5/0)\n", got, reads(&f),
                writes(&f));
        abort();
    }
    assert(f.cpu->pc == 0x2000u);
    dispose(&f);

    fixture_t g = timing_fixture();
    put_l(g.ram, VEC_BUS_ERROR * 4u, HANDLER);
    put_w(g.ram, RESERVED_STACK, 0x2000);
    put_l(g.ram, RESERVED_STACK + 2u, BERR_BASE);
    g.cpu->a[7] = RESERVED_STACK;
    PROGRAM(&g, 0x4E73);
    step_cycles(&g);
    assert(g.cpu->pc == HANDLER && g.cpu->last_exception_vector == VEC_BUS_ERROR);
    assert(stacked_pc(&g, g.cpu->a[7]) == RESET_PC + 2u);
    dispose(&g);
}

/* T-EXC-030-16: 拡張語の消費時の補充が失敗した場合、その位置より後の副作用
 * （データ read・An の更新・SR/CCR の更新）は出ない。スタックに積まれる SR は命令実行前の値。 */
static void test_refill_fault_has_no_later_side_effects(void) {
    static const struct {
        const char *id;
        uint16_t opcode;
        uint16_t imm;
    } rows[] = {
        {"ORI.W #,(A0)", 0x0050, 1},    {"ORI.W #,(A0)+", 0x0058, 1},
        {"ORI.W #,-(A0)", 0x0060, 1},   {"ANDI.W #,(A0)", 0x0250, 0},
        {"ANDI.W #,(A0)+", 0x0258, 0},  {"ANDI.W #,-(A0)", 0x0260, 0},
        {"SUBI.W #,(A0)", 0x0450, 1},   {"SUBI.W #,(A0)+", 0x0458, 1},
        {"SUBI.W #,-(A0)", 0x0460, 1},  {"ADDI.W #,(A0)", 0x0650, 1},
        {"ADDI.W #,(A0)+", 0x0658, 1},  {"ADDI.W #,-(A0)", 0x0660, 1},
        {"EORI.W #,(A0)", 0x0A50, 1},   {"EORI.W #,(A0)+", 0x0A58, 1},
        {"EORI.W #,-(A0)", 0x0A60, 1},  {"CMPI.W #,(A0)", 0x0C50, 1},
        {"CMPI.W #,(A0)+", 0x0C58, 1},  {"CMPI.W #,-(A0)", 0x0C60, 1},
        {"CMPI.B #,(A0)", 0x0C10, 1},   {"ORI.B #,(A0)+", 0x0018, 1},
        {"ORI #,SR", 0x007C, 0x0002},   {"ANDI #,SR", 0x027C, 0x0000},
        {"EORI #,SR", 0x0A7C, 0x0001},  {"ORI #,CCR", 0x003C, 0x0002},
        {"ANDI #,CCR", 0x023C, 0x0000}, {"EORI #,CCR", 0x0A3C, 0x0001},
    };
    const uint16_t sr0 = 0x2715u;
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        fixture_t f = timing_fixture();
        put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
        f.cpu->sr = sr0;
        f.cpu->pc = BERR_BASE - 4u;
        put_w(f.ram, BERR_BASE - 4u, rows[i].opcode);
        put_w(f.ram, BERR_BASE - 2u, rows[i].imm);
        step_cycles(&f);
        bool data_access = false;
        for (unsigned k = 0; k < f.count; k++)
            data_access |= !f.log[k].instruction && (f.log[k].address & ADDR_MASK) >= 0x1F0u &&
                           (f.log[k].address & ADDR_MASK) < 0x210u;
        uint16_t saved_sr = (uint16_t)(((uint16_t)f.ram[f.cpu->a[7] + 8u] << 8) |
                                       f.ram[f.cpu->a[7] + 9u]);
        if (f.cpu->pc != HANDLER || f.cpu->last_exception_vector != VEC_BUS_ERROR || data_access ||
            f.cpu->a[0] != 0x200u || saved_sr != sr0) {
            fprintf(stderr, "T-EXC-030-16 %s: pc=%X vec=%u data=%d a0=%X saved sr=%04X\n",
                    rows[i].id, f.cpu->pc, (unsigned)f.cpu->last_exception_vector, data_access,
                    f.cpu->a[0], saved_sr);
            abort();
        }
        dispose(&f);
    }
}

/* T-EXC-030-17: trace 例外の vector 読出しの失敗を同じ実行呼出しの中で即時に処理するのは
 * MC68000 と 68010。68020 では例外処理が保留（CPU_EXCEPTION）で戻り、次の実行呼出しで vector 2
 * の bus error 例外になる。68010 の挙動も T-EXC-030-17 で固定する（下の
 * test_68010_vector_read_fault）。 */
static void test_trace_entry_fault_model_limited(void) {
    fixture_t f = make_fixture();
    start_model(&f, MC68K_MODEL_68020);
    f.berr_lo = VEC_TRACE * 4u;
    f.berr_hi = VEC_TRACE * 4u + 4u;
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    f.cpu->sr = 0xA000u;
    PROGRAM(&f, 0x4E71);
    step_cycles(&f);
    assert(f.cpu->exec_state == CPU_EXCEPTION && f.cpu->pc != HANDLER);
    mc68k_execute(f.cpu, 1);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == VEC_BUS_ERROR);
    dispose(&f);
}

#define M10_VBR        0x4000u
#define M10_GROUP0_LEN 58u     /* format $8 */
#define M10_NORMAL_LEN 8u      /* format $0（途中まで積まれる） */
#define M10_SSW_VECTOR 0x1105u /* DF|RW|FC=5（supervisor データ読出し） */

typedef enum { M10_TRACE, M10_TRAP, M10_ILLEGAL, M10_IRQ } m10_kind_t;

/* 68010 で exception_vector の読出しを BERR にして、1 命令（または 1 IRQ）実行した状態を作る。
 */
static void m10_setup(fixture_t *f, m10_kind_t kind, uint32_t vbr, bool user, uint32_t vec_no) {
    start_model(f, MC68K_MODEL_68010);
    f->cpu->vbr = vbr;
    put_l(f->ram, vbr + VEC_BUS_ERROR * 4u, HANDLER);
    f->berr_lo = vbr + vec_no * 4u;
    f->berr_hi = vbr + vec_no * 4u + 4u;
    uint16_t sr = kind == M10_TRACE ? 0xA000u : 0x2000u;
    if (user)
        sr = (uint16_t)(sr & ~0x2000u);
    f->cpu->a[7] = RESET_SSP;
    cpu_set_sr(f->cpu, sr);
    if (user)
        f->cpu->a[7] = USER_SP;
    switch (kind) {
    case M10_TRACE:
        PROGRAM(f, 0x7001); /* MOVEQ #1,D0 */
        break;
    case M10_TRAP:
        PROGRAM(f, 0x4E40); /* TRAP #0 */
        break;
    case M10_ILLEGAL:
        PROGRAM(f, 0x4AFC);
        break;
    case M10_IRQ:
        PROGRAM(f, 0x7001);
        mc68k_set_irq(f->cpu, IRQ_LEVEL3);
        break;
    }
}

static uint16_t m10_w(const fixture_t *f, uint32_t a) {
    return (uint16_t)((f->ram[a] << 8) | f->ram[a + 1]);
}
static uint32_t m10_l(const fixture_t *f, uint32_t a) {
    return ((uint32_t)m10_w(f, a) << 16) | m10_w(f, a + 2);
}

/* T-EXC-030-17: 68010 の vector 読出しの失敗は HALT でなく、同じ実行呼出しの中で bus error
 * 例外になる。積むのは例外が起きた時点の SR／PC で、命令は巻き戻さない。 */
static void check_m10_vector_fault(m10_kind_t kind, uint32_t vbr, bool user, uint32_t vec_no,
                                   uint16_t saved_sr, uint32_t saved_pc, bool d0_set) {
    fixture_t f = make_fixture();
    m10_setup(&f, kind, vbr, user, vec_no);
    const uint32_t user_sp_before = f.cpu->a[7];
    step_cycles(&f);
    const char *what = kind == M10_TRACE     ? "trace"
                       : kind == M10_TRAP    ? "trap"
                       : kind == M10_ILLEGAL ? "illegal"
                                             : "irq";
    if (f.cpu->exec_state != CPU_NORMAL || f.cpu->pc != HANDLER ||
        f.cpu->last_exception_vector != VEC_BUS_ERROR) {
        fprintf(stderr, "T-EXC-030-17 %s: state=%d pc=%X vec=%u\n", what, f.cpu->exec_state,
                f.cpu->pc, f.cpu->last_exception_vector);
        abort();
    }
    assert((f.cpu->d[0] == 1u) == d0_set); /* 命令を巻き戻さない */
    const uint32_t sp = f.cpu->a[7];
    const uint32_t expect_sp = RESET_SSP - M10_NORMAL_LEN - M10_GROUP0_LEN;
    if (sp != expect_sp || m10_w(&f, sp) != saved_sr || m10_l(&f, sp + 2) != saved_pc ||
        m10_w(&f, sp + 6) != (uint16_t)(0x8000u | VEC_BUS_ERROR * 4u) ||
        m10_w(&f, sp + 8) != M10_SSW_VECTOR || m10_l(&f, sp + 0xA) != vbr + vec_no * 4u) {
        fprintf(stderr, "T-EXC-030-17 %s: sp=%X sr=%04X pc=%X fmt=%04X ssw=%04X addr=%X\n",
                what, sp, m10_w(&f, sp), m10_l(&f, sp + 2), m10_w(&f, sp + 6),
                m10_w(&f, sp + 8), m10_l(&f, sp + 0xA));
        abort();
    }
    /* handler が vector を直して RTE する。例外処理の途中まで積んだフレームごと戻る。 */
    f.berr_lo = f.berr_hi = 0;
    mc68k_set_irq(f.cpu, 0);       /* 同じ IRQ を再び受け付けないようにする */
    put_w(f.ram, HANDLER, 0x4E73); /* RTE */
    /* T=1 のまま戻ると RTE 直後に trace 例外になるので、積まれた SR の T を落としておく。 */
    const uint16_t return_sr = (uint16_t)(saved_sr & ~0x8000u);
    put_w(f.ram, sp, return_sr);
    cpu_prefetch_prime(f.cpu);
    mc68k_execute(f.cpu, 1);
    if (f.cpu->pc != saved_pc || f.cpu->sr != return_sr) {
        fprintf(stderr, "T-EXC-030-17 %s RTE: pc=%X sr=%04X state=%d vec=%u\n", what, f.cpu->pc,
                f.cpu->sr, f.cpu->exec_state, f.cpu->last_exception_vector);
        abort();
    }
    assert(f.cpu->exception_frame_origins == NULL);
    if (user) {
        assert(f.cpu->a[7] == user_sp_before && f.cpu->ssp == RESET_SSP);
    } else {
        assert(f.cpu->a[7] == RESET_SSP);
    }
    dispose(&f);
}

static void test_68010_vector_read_fault(void) {
    check_m10_vector_fault(M10_TRACE, 0, false, VEC_TRACE, 0xA000u, RESET_PC + 2u, true);
    check_m10_vector_fault(M10_TRACE, M10_VBR, false, VEC_TRACE, 0xA000u, RESET_PC + 2u, true);
    check_m10_vector_fault(M10_TRACE, 0, true, VEC_TRACE, 0x8000u, RESET_PC + 2u, true);
    check_m10_vector_fault(M10_TRAP, 0, false, 32u, 0x2000u, RESET_PC + 2u, false);
    check_m10_vector_fault(M10_TRAP, M10_VBR, true, 32u, 0x0000u, RESET_PC + 2u, false);
    check_m10_vector_fault(M10_ILLEGAL, 0, false, 4u, 0x2000u, RESET_PC, false);
    check_m10_vector_fault(M10_IRQ, 0, false, VEC_IRQ3, 0x2000u, RESET_PC, false);
    check_m10_vector_fault(M10_IRQ, M10_VBR, true, VEC_IRQ3, 0x0000u, RESET_PC, false);
}

/* T-EXC-030-18: 変位語の取得が失敗した BSR.W／BRA.W／Bcc.W は、戻りアドレスの push も
 * PC の書換えも行わない（スタックは bus error フレームの14 byte だけ）。 */
static void test_branch_word_refill_fault_no_side_effects(void) {
    static const uint16_t opcodes[] = {0x6100, 0x6000, 0x6700}; /* BSR.W, BRA.W, BEQ.W（Z=1） */
    for (unsigned i = 0; i < sizeof opcodes / sizeof opcodes[0]; i++) {
        fixture_t f = timing_fixture();
        put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
        f.cpu->sr = 0x2704u;
        f.cpu->pc = BERR_BASE - 2u;
        put_w(f.ram, BERR_BASE - 2u, opcodes[i]); /* 変位語（$F00000）自体の取得が失敗する */
        step_cycles(&f);
        assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == VEC_BUS_ERROR);
        assert(f.cpu->a[7] == RESET_SSP - 14u);
        dispose(&f);
    }
}

/* T-TIM-040-03: 分岐では逐次streamの先読みを破棄し、分岐先を2語充填する。 */
static void test_branch_refills_prefetch(void) {
    fixture_t f = timing_fixture();
    PROGRAM(&f, 0x6004, 0x4AFC, 0x4AFC, 0x4E71, 0x4E71); /* BRA.S $1006 */
    step_cycles(&f);
    assert(f.cpu->pc == RESET_PC + 6u);
    assert(f.count == 2 && f.log[0].instruction && f.log[1].instruction);
    assert(f.log[0].address == RESET_PC + 6u && f.log[1].address == RESET_PC + 8u);
    step_cycles(&f);
    assert(f.cpu->pc == RESET_PC + 8u);
    dispose(&f);
}

int main(void) {
    test_nominal_cycles();
    test_wait_states();
    test_bus_trace_order();
    test_tas_sequence();
    test_tas_bus_error();
    test_fetch_fc();
    test_saved_pc_range();
    test_self_modifying_next_word_uses_prefetch();
    test_flow_timing();
    test_alu_timing();
    test_branch_refills_prefetch();
    test_last_refill_follows_data_access();
    test_branch_target_fetch_fault_saved_pc();
    test_vector_read_fault_takes_bus_error();
    test_handler_fetch_fault_takes_bus_error();
    test_group0_vector_read_fault_halts();
    test_group0_handler_fetch_fault_halts();
    test_branch_fetch_fault_beats_trace();
    test_prefetch_fault_beats_pending_irq();
    test_unused_prefetch_fault_raises_bus_error();
    test_exception_entry_fault_beats_irq();
    test_exception_entry_fault_ssw_in();
    test_vector_group_ssw_in();
    test_handler_fetch_fault_in_and_irq_order();
    test_extension_refill_fault_is_immediate();
    test_read_before_write_fault_and_model();
    test_rte_timing_and_target_fault();
    test_refill_fault_has_no_later_side_effects();
    test_trace_entry_fault_model_limited();
    test_68010_vector_read_fault();
    test_branch_word_refill_fault_no_side_effects();
    puts("test_conformance_timing: OK");
    return 0;
}
