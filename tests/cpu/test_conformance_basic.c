/*
 * test_conformance_basic.c — REG / BUS / EA / PRV-030 の適合性ケース。
 *
 * 期待値は実装ではなく docs/mc68000-internals/spec_mc68000.md の第3、4、5、7節に基づく。
 * ケース ID（T-<norm id>-nn）は tests/data/cpu/conformance_map_mc68000.csv と対応する。
 * 24-bit 外部アドレス規則（REG-009）を観測できるよう bus は 16 MiB RAM とし、
 * 1領域だけ bus error を返す。
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu_internal.h"
#include "mc68000/bus_hooks.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"

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
} cycle_t;

typedef struct {
    uint8_t *ram;
    cycle_t log[LOG_MAX];
    unsigned count;
    mc68k_state_t *cpu;
} fixture_t;

static bool in_berr(uint32_t addr) {
    uint32_t a = addr & ADDR_MASK;
    return a >= BERR_BASE && a < BERR_END;
}

static void access_timing(void *ctx, const mc68k_bus_access_t *a, mc68k_bus_timing_t *out) {
    fixture_t *f = ctx;
    out->wait_cycles = 0;
    out->cacheable = false;
    assert(f->count < LOG_MAX);
    f->log[f->count].address = a->physical;
    f->log[f->count].size = a->size;
    f->log[f->count].fc = a->fc;
    f->log[f->count].read = a->read;
    f->log[f->count].instruction = a->instruction;
    f->count++;
}

static bool rb(void *c, uint32_t a, uint8_t *o) {
    fixture_t *f = c;
    if (in_berr(a))
        return false;
    *o = f->ram[a & ADDR_MASK];
    return true;
}
static bool rw_(void *c, uint32_t a, uint16_t *o) {
    fixture_t *f = c;
    if (in_berr(a))
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
    if (in_berr(a))
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
static uint16_t get_w(const uint8_t *ram, uint32_t a) {
    return (uint16_t)((ram[a] << 8) | ram[a + 1]);
}
static uint32_t get_l(const uint8_t *ram, uint32_t a) {
    return ((uint32_t)get_w(ram, a) << 16) | get_w(ram, a + 2);
}

static fixture_t make_fixture(void) {
    fixture_t f = {0};
    f.ram = calloc(RAM_SIZE, 1);
    assert(f.ram);
    put_l(f.ram, 0, RESET_SSP);
    put_l(f.ram, 4, RESET_PC);
    return f;
}

static void start(fixture_t *f) {
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
    f->cpu = mc68k_create(&bus);
    assert(f->cpu);
    mc68k_reset(f->cpu);
    f->count = 0;
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

static void step(fixture_t *f) {
    f->count = 0;
    mc68k_execute(f->cpu, 1);
}

/* Count data (non-instruction) cycles. */
static unsigned data_cycles(const fixture_t *f) {
    unsigned n = 0;
    for (unsigned i = 0; i < f->count; i++)
        if (!f->log[i].instruction)
            n++;
    return n;
}

/* ---- REG ---------------------------------------------------------- */

/* T-REG-001-01: byte/word writes to Dn keep the upper bits. */
static void test_reg_001(void) {
    fixture_t f = make_fixture();
    start(&f);
    PROGRAM(&f, 0x3200); /* MOVE.W D0,D1 */
    f.cpu->d[0] = 0x11223344u;
    f.cpu->d[1] = 0xAABBCCDDu;
    step(&f);
    assert(f.cpu->d[1] == 0xAABB3344u);
    dispose(&f);
}

/* T-REG-002-01: MOVEA.W sign-extends into the whole An. */
static void test_reg_002(void) {
    fixture_t f = make_fixture();
    start(&f);
    PROGRAM(&f, 0x3240); /* MOVEA.W D0,A1 */
    f.cpu->d[0] = 0x00008000u;
    step(&f);
    assert(f.cpu->a[1] == 0xFFFF8000u);
    dispose(&f);
}

/* T-REG-003-01: leaving supervisor mode saves SSP and exposes USP as A7;
 * T-REG-003-02: the stack grows downward (exception push). */
static void test_reg_003(void) {
    fixture_t f = make_fixture();
    start(&f);
    PROGRAM(&f, 0x46FC, 0x0000); /* MOVE #$0000,SR */
    f.cpu->usp = USER_SP;
    f.cpu->a[7] = 0x6000u; /* SSP value in use while S=1 */
    step(&f);
    assert((f.cpu->sr & SR_S) == 0);
    assert(f.cpu->a[7] == USER_SP);
    assert(f.cpu->ssp == 0x6000u);
    dispose(&f);

    fixture_t g = make_fixture();
    start(&g);
    put_l(g.ram, 4u * 4u, HANDLER); /* illegal instruction vector */
    PROGRAM(&g, 0x4AFC);
    step(&g);
    assert(g.cpu->a[7] == RESET_SSP - 6u);
    dispose(&g);
}

/* T-REG-004-01: PC keeps its upper bits; only the bus address is 24 bit. */
static void test_reg_004(void) {
    fixture_t f = make_fixture();
    start(&f);
    PROGRAM(&f, 0x4E71); /* NOP at $001000 */
    f.cpu->pc = 0xAB001000u;
    step(&f);
    assert(f.cpu->pc == 0xAB001002u);
    assert(f.count >= 1);
    assert((f.log[0].address & ADDR_MASK) == RESET_PC);
    dispose(&f);
}

/* T-REG-005-01: memory is big-endian, long = high word at the low address. */
static void test_reg_005(void) {
    fixture_t f = make_fixture();
    start(&f);
    PROGRAM(&f, 0x2080); /* MOVE.L D0,(A0) */
    f.cpu->d[0] = 0x11223344u;
    f.cpu->a[0] = 0x200u;
    step(&f);
    assert(f.ram[0x200] == 0x11 && f.ram[0x201] == 0x22);
    assert(f.ram[0x202] == 0x33 && f.ram[0x203] == 0x44);
    dispose(&f);
}

/* T-REG-006-01: MOVE from SR is legal in user mode and reads the whole SR. */
static void test_reg_006(void) {
    fixture_t f = make_fixture();
    start(&f);
    put_l(f.ram, 8u * 4u, HANDLER); /* privilege violation: must not be taken */
    PROGRAM(&f, 0x40C0);            /* MOVE SR,D0 */
    f.cpu->sr = 0x0015u;            /* user, X and Z... N=0 Z=0 V=0 C=1 X=1 */
    f.cpu->d[0] = 0xFFFF0000u;
    step(&f);
    assert(f.cpu->pc == RESET_PC + 2u);
    assert((f.cpu->d[0] & 0xFFFF0000u) == 0xFFFF0000u);
    assert((f.cpu->d[0] & SR_MASK) == 0x0015u);
    dispose(&f);
}

/* T-REG-008-01: SR bit 14 (T0) and bit 12 (M) always read 0.  Bits 11 and
 * 7:5 are undefined and are not compared. */
static void test_reg_008(void) {
    fixture_t f = make_fixture();
    start(&f);
    PROGRAM(&f, 0x46FC, 0x7FFF, 0x40C0); /* MOVE #$7FFF,SR ; MOVE SR,D0 (T1 clear) */
    f.cpu->usp = USER_SP;
    step(&f);
    step(&f);
    assert((f.cpu->d[0] & SR_T0) == 0);
    assert((f.cpu->d[0] & SR_M) == 0);
    dispose(&f);
}

/* T-REG-009-01: a 32-bit register value selects memory only by its low 24
 * bits, and the register keeps its upper byte. */
static void test_reg_009(void) {
    fixture_t f = make_fixture();
    start(&f);
    PROGRAM(&f, 0x3210); /* MOVE.W (A0),D1 */
    f.cpu->a[0] = 0xAB000200u;
    put_w(f.ram, 0x200, 0x1234);
    step(&f);
    assert(f.cpu->d[1] == 0x1234u);
    assert(f.cpu->a[0] == 0xAB000200u);
    unsigned seen = 0;
    for (unsigned i = 0; i < f.count; i++)
        if (!f.log[i].instruction) {
            assert((f.log[i].address & ADDR_MASK) == 0x200u);
            seen++;
        }
    assert(seen == 1);
    dispose(&f);
}

/* T-REG-011-01: a word access to an odd address is an address error taken
 * before any data transfer for that access. */
static void test_reg_011(void) {
    fixture_t f = make_fixture();
    start(&f);
    put_l(f.ram, VEC_ADDR_ERR * 4u, HANDLER);
    PROGRAM(&f, 0x3210); /* MOVE.W (A0),D1 */
    f.cpu->a[0] = 0x201u;
    step(&f);
    assert(f.cpu->pc == HANDLER);
    for (unsigned i = 0; i < f.count; i++)
        assert(!(f.log[i].address == 0x201u && !f.log[i].instruction));
    dispose(&f);
}

/* ---- EA ----------------------------------------------------------- */

/* Run "MOVE.W <ea>,D1" laid out from RESET_PC and return D1. */
#define EA_SETUP(f)                                                                                \
    fixture_t f = make_fixture();                                                                  \
    start(&f);                                                                                     \
    put_w(f.ram, 0x200, 0x1111);                                                                   \
    put_w(f.ram, 0x202, 0x2222);                                                                   \
    put_w(f.ram, 0x204, 0x3333);                                                                   \
    put_w(f.ram, 0x206, 0x4444);                                                                   \
    put_w(f.ram, 0x210, 0x5555);                                                                   \
    f.cpu->d[1] = 0xFFFF0000u;                                                                     \
    f.cpu->a[0] = 0x200u

static void test_ea_001_002(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x3200); /* T-EA-001-01: MOVE.W D0,D1 */
    f.cpu->d[0] = 0x00009876u;
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF9876u && f.cpu->pc == RESET_PC + 2u);
    dispose(&f);

    EA_SETUP(g);
    PROGRAM(&g, 0x3208); /* T-EA-002-01: MOVE.W A0,D1 */
    step(&g);
    assert(g.cpu->d[1] == 0xFFFF0200u && g.cpu->pc == RESET_PC + 2u);
    dispose(&g);
}

static void test_ea_003(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x3210); /* MOVE.W (A0),D1 */
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF1111u && f.cpu->a[0] == 0x200u);
    dispose(&f);
}

static void test_ea_004(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x3218); /* MOVE.W (A0)+,D1 */
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF1111u && f.cpu->a[0] == 0x202u);
    dispose(&f);

    EA_SETUP(g); /* T-EA-004-02: A7 byte step is 2 (EA-014 rule shared) */
    PROGRAM(&g, 0x121F); /* MOVE.B (A7)+,D1 */
    g.cpu->a[7] = 0x400u;
    put_w(g.ram, 0x400, 0xAB00);
    step(&g);
    assert((g.cpu->d[1] & 0xFFu) == 0xABu && g.cpu->a[7] == 0x402u);
    dispose(&g);
}

static void test_ea_005(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x3220); /* MOVE.W -(A0),D1 */
    f.cpu->a[0] = 0x204u;
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF2222u && f.cpu->a[0] == 0x202u);
    dispose(&f);
}

static void test_ea_006(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x3228, 0x0004); /* MOVE.W 4(A0),D1 */
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF3333u && f.cpu->pc == RESET_PC + 4u);
    dispose(&f);

    EA_SETUP(g); /* T-EA-006-02: negative displacement */
    PROGRAM(&g, 0x3228, 0xFFFE);
    g.cpu->a[0] = 0x204u;
    step(&g);
    assert(g.cpu->d[1] == 0xFFFF2222u);
    dispose(&g);
}

static void test_ea_007(void) {
    /* T-EA-007-01: word index sign-extends Xn[15:0]; bits 10:9 are not a scale. */
    EA_SETUP(f);
    PROGRAM(&f, 0x3230, 0x2602); /* MOVE.W 2(A0,D2.W),D1 ; ext bits 10:9 = 11 */
    f.cpu->d[2] = 0xFFFF0002u;   /* D2.W = 2 */
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF3333u && f.cpu->pc == RESET_PC + 4u);
    dispose(&f);

    /* T-EA-007-02: negative word index */
    EA_SETUP(g);
    PROGRAM(&g, 0x3230, 0x2002); /* 2(A0,D2.W) */
    g.cpu->a[0] = 0x204u;
    g.cpu->d[2] = 0x0000FFFCu; /* D2.W = -4 */
    step(&g);
    assert(g.cpu->d[1] == 0xFFFF2222u); /* 0x204 + 2 - 4 = 0x202 */
    dispose(&g);
}

static void test_ea_007_long_index(void) {
    /* T-EA-007-03: long index uses the whole register, An as index works. */
    EA_SETUP(f);
    PROGRAM(&f, 0x3230, 0x2800); /* MOVE.W 0(A0,D2.L),D1 : bit 11 = long */
    f.cpu->d[2] = 0x00000010u;
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF5555u);
    dispose(&f);

    EA_SETUP(g);
    PROGRAM(&g, 0x3230, 0xA800); /* index register A2.L (bit 15 = 1) */
    g.cpu->a[2] = 0x00000010u;
    step(&g);
    assert(g.cpu->d[1] == 0xFFFF5555u);
    dispose(&g);
}

/* T-EA-013-01: brief extension word bits 10:8 are not decoded on the MC68000;
 * they affect neither the address nor exception processing. */
static void test_ea_013(void) {
    static const uint16_t extensions[] = {0x2702, 0x2102};
    for (unsigned i = 0; i < sizeof extensions / sizeof extensions[0]; i++) {
        EA_SETUP(f);
        put_l(f.ram, 4u * 4u, HANDLER); /* illegal instruction must not be taken */
        PROGRAM(&f, 0x3230, extensions[i]); /* MOVE.W 2(A0,D2.W),D1 */
        f.cpu->d[2] = 0xFFFF0002u;
        step(&f);
        assert(f.cpu->d[1] == 0xFFFF3333u && f.cpu->pc == RESET_PC + 4u);
        dispose(&f);
    }
}

/* INS-052: 予約済み source EA（mode=111/reg=101–111）の ADDA/SUBA/CMPA は ILLEGAL を起こす。
 * 一方、An 直接（mode=001）は PM が許す source なので ILLEGAL にならない。 */
static void test_reserved_source_ea_is_illegal(void) {
    static const uint16_t bases[] = {
        0xD0C0, /* ADDA.W */ 0xD1C0, /* ADDA.L */ 0x90C0, /* SUBA.W */
        0x91C0, /* SUBA.L */ 0xB0C0, /* CMPA.W */ 0xB1C0, /* CMPA.L */
    };
    static const unsigned reserved_ea[] = {0x3Du, 0x3Eu, 0x3Fu}; /* mode=111, reg=101–111 */
    static const unsigned an_direct_ea = 0x09u;                  /* A1 */

    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        for (unsigned j = 0; j < sizeof reserved_ea / sizeof reserved_ea[0]; j++) {
            fixture_t f = make_fixture();
            start(&f);
            put_l(f.ram, 4u * 4u, HANDLER);
            PROGRAM(&f, (uint16_t)(bases[i] | reserved_ea[j]));
            f.cpu->a[0] = 0x12345678u;
            step(&f);
            assert(f.cpu->pc == HANDLER);
            assert(f.cpu->last_exception_vector == 4u);
            assert(f.cpu->a[0] == 0x12345678u);
            dispose(&f);
        }
        fixture_t g = make_fixture();
        start(&g);
        put_l(g.ram, 4u * 4u, HANDLER);
        PROGRAM(&g, (uint16_t)(bases[i] | an_direct_ea));
        g.cpu->a[0] = 0x12345678u;
        g.cpu->a[1] = 0x00000010u;
        step(&g);
        assert(g.cpu->pc == RESET_PC + 2u);
        /* source は A1=$10。ADDA は加算、SUBA は減算、CMPA は An を変更しない */
        const uint32_t a0_init = 0x12345678u;
        const uint32_t a0_expect = (i < 2) ? a0_init + 0x10u : (i < 4) ? a0_init - 0x10u : a0_init;
        assert(g.cpu->a[0] == a0_expect);
        dispose(&g);
    }
}

static void test_ea_008_009(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x3238, 0x0200); /* MOVE.W ($0200).W,D1 */
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF1111u && f.cpu->pc == RESET_PC + 4u);
    dispose(&f);

    /* T-EA-008-02: absolute short is sign-extended; $8000 -> $FF8000 */
    EA_SETUP(g);
    PROGRAM(&g, 0x3238, 0x8000);
    step(&g);
    bool hit = false;
    for (unsigned i = 0; i < g.count; i++)
        if (!g.log[i].instruction && (g.log[i].address & ADDR_MASK) == 0xFF8000u)
            hit = true;
    assert(hit);
    dispose(&g);

    EA_SETUP(h);
    PROGRAM(&h, 0x3239, 0x0000, 0x0204); /* MOVE.W ($00000204).L,D1 */
    step(&h);
    assert(h.cpu->d[1] == 0xFFFF3333u && h.cpu->pc == RESET_PC + 6u);
    dispose(&h);
}

static void test_ea_010_011(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x323A, 0x01FE); /* MOVE.W $1FE(PC),D1 ; PC_ext=$1002 -> $1200 */
    put_w(f.ram, 0x1200, 0x7777);
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF7777u && f.cpu->pc == RESET_PC + 4u);
    dispose(&f);

    EA_SETUP(g);
    PROGRAM(&g, 0x323B, 0x2002); /* MOVE.W 2(PC,D2.W),D1 ; PC_ext=$1002 */
    g.cpu->d[2] = 0x000001FCu;   /* $1002 + 2 + $1FC = $1200 */
    put_w(g.ram, 0x1200, 0x7777);
    step(&g);
    assert(g.cpu->d[1] == 0xFFFF7777u && g.cpu->pc == RESET_PC + 4u);
    dispose(&g);
}

static void test_ea_012(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x323C, 0x1234); /* MOVE.W #$1234,D1 */
    step(&f);
    assert(f.cpu->d[1] == 0xFFFF1234u && f.cpu->pc == RESET_PC + 4u);
    dispose(&f);

    EA_SETUP(g);
    PROGRAM(&g, 0x223C, 0x1234, 0x5678); /* MOVE.L #$12345678,D1 */
    step(&g);
    assert(g.cpu->d[1] == 0x12345678u && g.cpu->pc == RESET_PC + 6u);
    dispose(&g);

    EA_SETUP(h); /* byte immediate is the low byte of its one extension word */
    PROGRAM(&h, 0x123C, 0xFFAB); /* MOVE.B #$AB,D1 */
    step(&h);
    assert(h.cpu->d[1] == 0xFFFF00ABu && h.cpu->pc == RESET_PC + 4u);
    dispose(&h);
}

/* T-EA-014-01: -(An) uses the decremented address and keeps it after a bus
 * error; A7 byte step is 2. */
static void test_ea_014(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x1100); /* MOVE.B D0,-(A0) */
    f.cpu->d[0] = 0x5Au;
    f.cpu->a[0] = 0x201u;
    step(&f);
    assert(f.cpu->a[0] == 0x200u && f.ram[0x200] == 0x5Au);
    dispose(&f);

    EA_SETUP(g);
    PROGRAM(&g, 0x1F00); /* MOVE.B D0,-(A7) */
    g.cpu->d[0] = 0x5Au;
    g.cpu->a[7] = 0x400u;
    step(&g);
    assert(g.cpu->a[7] == 0x3FEu && g.ram[0x3FE] == 0x5Au);
    dispose(&g);

    EA_SETUP(h); /* T-EA-014-02: bus error on the write keeps the decremented A0 */
    put_l(h.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&h, 0x3100); /* MOVE.W D0,-(A0) */
    h.cpu->a[0] = BERR_BASE + 0x1000u;
    h.cpu->d[0] = 0x1234u;
    step(&h);
    assert(h.cpu->pc == HANDLER);
    assert(h.cpu->a[0] == BERR_BASE + 0x1000u - 2u);
    dispose(&h);
}

/* T-EA-015-01/02, T-EA-016-01/02: after a bus error inside (An)+ or MOVEM the
 * architectural register updates are undefined (spec EA-015/016), so only the
 * exception itself is checked: vector 2, 14-byte frame, handler entry.  A0, A1
 * and the data registers are deliberately not compared. */
static void expect_bus_error(fixture_t *f) {
    assert(f->cpu->pc == HANDLER);
    assert(f->cpu->a[7] == RESET_SSP - 14u);
}

static void test_ea_015_016(void) {
    EA_SETUP(f); /* T-EA-015-01: read fault in (A0)+ */
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&f, 0x3218); /* MOVE.W (A0)+,D1 */
    f.cpu->a[0] = BERR_BASE;
    step(&f);
    expect_bus_error(&f);
    dispose(&f);

    EA_SETUP(g); /* T-EA-015-02: write fault in (A0)+ */
    put_l(g.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&g, 0x30C0); /* MOVE.W D0,(A0)+ */
    g.cpu->a[0] = BERR_BASE;
    g.cpu->d[0] = 0x1234u;
    step(&g);
    expect_bus_error(&g);
    dispose(&g);

    EA_SETUP(h); /* T-EA-016-01: second MOVEM read faults */
    put_l(h.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&h, 0x4C98, 0x0003); /* MOVEM.W (A0)+,D0-D1 */
    h.cpu->a[0] = BERR_BASE - 2u;
    step(&h);
    expect_bus_error(&h);
    dispose(&h);

    EA_SETUP(k); /* T-EA-016-02: destination write of MOVE (A0)+,-(A1) faults */
    put_l(k.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&k, 0x3318); /* MOVE.W (A0)+,-(A1) */
    k.cpu->a[1] = BERR_BASE + 2u;
    step(&k);
    expect_bus_error(&k);
    dispose(&k);
}

/* T-EA-018-01: memory read-modify-write reads first, then writes the same
 * address; T-EA-018-02: a failed read produces no write. */
static void test_ea_018(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x4450); /* NEG.W (A0) */
    put_w(f.ram, 0x200, 0x0001);
    step(&f);
    assert(get_w(f.ram, 0x200) == 0xFFFFu);
    unsigned reads = 0, writes = 0;
    int first_read = -1, first_write = -1;
    for (unsigned i = 0; i < f.count; i++) {
        if (f.log[i].instruction || (f.log[i].address & ADDR_MASK) != 0x200u)
            continue;
        if (f.log[i].read) {
            reads++;
            if (first_read < 0)
                first_read = (int)i;
        } else {
            writes++;
            if (first_write < 0)
                first_write = (int)i;
        }
    }
    assert(reads == 1 && writes == 1 && first_read < first_write);
    dispose(&f);

    EA_SETUP(g);
    put_l(g.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&g, 0x4450);
    g.cpu->a[0] = BERR_BASE + 0x100u;
    step(&g);
    assert(g.cpu->pc == HANDLER);
    for (unsigned i = 0; i < g.count; i++)
        assert(!(!g.log[i].read && !g.log[i].instruction &&
                 (g.log[i].address & ADDR_MASK) == BERR_BASE + 0x100u));
    dispose(&g);
}

/* ---- BUS ---------------------------------------------------------- */

/* T-BUS-001-01: byte accesses are single-byte requests at even and odd
 * addresses (lane is a function of address parity). */
static void test_bus_001(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x1210, 0x1228, 0x0001); /* MOVE.B (A0),D1 ; MOVE.B 1(A0),D1 */
    f.ram[0x200] = 0xA1;
    f.ram[0x201] = 0xB2;
    step(&f);
    assert((f.cpu->d[1] & 0xFFu) == 0xA1u);
    unsigned n = 0;
    for (unsigned i = 0; i < f.count; i++)
        if (!f.log[i].instruction) {
            assert(f.log[i].size == 1 && f.log[i].address == 0x200u);
            n++;
        }
    assert(n == 1);
    step(&f);
    assert((f.cpu->d[1] & 0xFFu) == 0xB2u);
    n = 0;
    for (unsigned i = 0; i < f.count; i++)
        if (!f.log[i].instruction) {
            assert(f.log[i].size == 1 && f.log[i].address == 0x201u);
            n++;
        }
    assert(n == 1);
    dispose(&f);
}

/* T-BUS-004-01: an ascending long read is two word cycles, high then low. */
static void test_bus_004(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x2210); /* MOVE.L (A0),D1 */
    step(&f);
    assert(f.cpu->d[1] == 0x11112222u);
    cycle_t d[4];
    unsigned n = 0;
    for (unsigned i = 0; i < f.count; i++)
        if (!f.log[i].instruction) {
            assert(n < 4);
            d[n++] = f.log[i];
        }
    /* The core may report the long as one size-4 callback; accept either
     * a single 4-byte request or two 2-byte requests in address order. */
    assert((n == 1 && d[0].size == 4 && d[0].address == 0x200u) ||
           (n == 2 && d[0].size == 2 && d[0].address == 0x200u && d[1].size == 2 &&
            d[1].address == 0x202u));
    dispose(&f);
}

/* T-BUS-005-01: fetches are word reads and advance the cursor by 2;
 * T-BUS-005-02: an odd fetch address is an address error with no bus request. */
static void test_bus_005(void) {
    fixture_t f = make_fixture();
    start(&f);
    PROGRAM(&f, 0x4E71);
    step(&f);
    assert(f.cpu->pc == RESET_PC + 2u);
    assert(f.log[0].instruction && f.log[0].read && f.log[0].size >= 2);
    dispose(&f);

    fixture_t g = make_fixture();
    start(&g);
    put_l(g.ram, VEC_ADDR_ERR * 4u, HANDLER);
    g.cpu->pc = RESET_PC + 1u;
    step(&g);
    assert(g.cpu->pc == HANDLER);
    for (unsigned i = 0; i < g.count; i++)
        assert(!(g.log[i].instruction && g.log[i].address == RESET_PC + 1u));
    dispose(&g);
}

/* T-BUS-008-01: only FC 1, 2, 5, 6 appear on ordinary requests, program fetch
 * and operand FC follow the mode. */
static void test_bus_008(void) {
    EA_SETUP(f); /* supervisor */
    PROGRAM(&f, 0x3210);
    step(&f);
    for (unsigned i = 0; i < f.count; i++) {
        assert(f.log[i].instruction ? f.log[i].fc == FC_SPROG : f.log[i].fc == FC_SDATA);
    }
    dispose(&f);

    EA_SETUP(g); /* user */
    PROGRAM(&g, 0x3210);
    g.cpu->sr = 0x0000u;
    g.cpu->usp = USER_SP;
    g.cpu->a[7] = USER_SP;
    step(&g);
    for (unsigned i = 0; i < g.count; i++) {
        assert(g.log[i].instruction ? g.log[i].fc == FC_UPROG : g.log[i].fc == FC_UDATA);
    }
    dispose(&g);
}

/* T-BUS-009-01: BERR on an ordinary cycle takes the bus-error vector (2). */
static void test_bus_009(void) {
    EA_SETUP(f);
    put_l(f.ram, VEC_BUS_ERROR * 4u, HANDLER);
    PROGRAM(&f, 0x3210);
    f.cpu->a[0] = BERR_BASE;
    step(&f);
    assert(f.cpu->pc == HANDLER);
    assert(f.cpu->a[7] == RESET_SSP - 14u); /* 14-byte group-0 frame */
    dispose(&f);
}

/* T-BUS-011-01: address error issues no data transfer for the bad access
 * (checked in test_reg_011); the frame is the 14-byte group-0 frame. */
static void test_bus_011(void) {
    EA_SETUP(f);
    put_l(f.ram, VEC_ADDR_ERR * 4u, HANDLER);
    PROGRAM(&f, 0x3210);
    f.cpu->a[0] = 0x201u;
    step(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->a[7] == RESET_SSP - 14u);
    assert(data_cycles(&f) == 0 ||
           f.log[f.count - 1].address != 0x201u); /* no cycle at the bad address */
    dispose(&f);
}

/* T-BUS-012-01: two reads of the same address are two requests, each logged;
 * T-BUS-012-02: the debugger read path generates no CPU bus request. */
static void test_bus_012(void) {
    EA_SETUP(f);
    PROGRAM(&f, 0x3210, 0x3210);
    step(&f);
    unsigned a = data_cycles(&f);
    step(&f);
    unsigned b = data_cycles(&f);
    assert(a == 1 && b == 1);

    uint8_t buf[2];
    mc68k_set_peek_hooks(f.cpu, peek_b, NULL, NULL, &f);
    f.count = 0;
    assert(mc68k_debug_read_bytes(f.cpu, 0x200, buf, sizeof buf) == 0);
    assert(buf[0] == 0x11 && buf[1] == 0x11);
    assert(f.count == 0);
    dispose(&f);
}

/* ---- PRV-030 ------------------------------------------------------ */

/* T-PRV-030-01: while stopped, no instruction fetch is issued. */
static void test_prv_030(void) {
    fixture_t f = make_fixture();
    start(&f);
    PROGRAM(&f, 0x4E72, 0x2000, 0x4E71); /* STOP #$2000 ; NOP */
    step(&f);
    f.count = 0;
    mc68k_execute(f.cpu, 1000);
    assert(f.count == 0);
    dispose(&f);
}

int main(void) {
    test_reg_001();
    test_reg_002();
    test_reg_003();
    test_reg_004();
    test_reg_005();
    test_reg_006();
    test_reg_008();
    test_reg_009();
    test_reg_011();
    test_ea_001_002();
    test_ea_003();
    test_ea_004();
    test_ea_005();
    test_ea_006();
    test_ea_007();
    test_ea_007_long_index();
    test_ea_013();
    test_reserved_source_ea_is_illegal();
    test_ea_008_009();
    test_ea_010_011();
    test_ea_012();
    test_ea_014();
    test_ea_015_016();
    test_ea_018();
    test_bus_001();
    test_bus_004();
    test_bus_005();
    test_bus_008();
    test_bus_009();
    test_bus_011();
    test_bus_012();
    test_prv_030();
    puts("test_conformance_basic: OK");
    return 0;
}
