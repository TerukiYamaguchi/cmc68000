/*
 * MC68040 内蔵 FPU の実装済み命令の実行: 現行仕様〜§5。
 * 演算結果・条件コード・FPIAR・データ転送・条件命令・算術例外の報告時期（保留ラッチ、
 * post-instruction）・未対応データ型を、RAM 上の生バイナリで検証する。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu/cpu_internal.h"
#include "fpu/fpu_exec.h"
#include "mc68000/cpu.h"

enum {
    RAM_SIZE = 0x10000,
    RESET_SP = 0x8000,
    RESET_PC = 0x1000,
    DATA_AREA = 0x3000,
    HANDLER_BASE = 0x400,
    RESET_SR = 0x2700,
    VEC_TRAPCC = 7,
    VEC_FLINE = 11,
    VEC_BSUN = 48,
    VEC_INEX = 49,
    VEC_DZ = 50,
    VEC_UNFL = 51,
    VEC_OPERR = 52,
    VEC_OVFL = 53,
    VEC_SNAN = 54,
    VEC_UNSUPPORTED = 55,
    LAST_VECTOR = 64,
    EXC_INEX2 = MC68K_FPU_EXC_INEX2,
    EXC_DZ = MC68K_FPU_EXC_DZ,
    EXC_UNFL = MC68K_FPU_EXC_UNFL,
    EXC_OVFL = MC68K_FPU_EXC_OVFL,
    EXC_OPERR = MC68K_FPU_EXC_OPERR,
    EXC_SNAN = MC68K_FPU_EXC_SNAN,
    EXC_BSUN = MC68K_FPU_EXC_BSUN,
};

#define HANDLER(v) (HANDLER_BASE + 4u * (v))

/* 拡張語。外部形式の番号は FMT_*（L=0 S=1 X=2 P=3 W=4 D=5 B=6）。 */
enum { FMT_L, FMT_S, FMT_X, FMT_P, FMT_W, FMT_D, FMT_B };
#define EXT_RR(src, dst, op) ((uint16_t)((src) << 10 | (dst) << 7 | (op)))
#define EXT_IN(fmt, dst, op) ((uint16_t)(0x4000 | (fmt) << 10 | (dst) << 7 | (op)))
#define EXT_OUT(fmt, src)    ((uint16_t)(0x6000 | (fmt) << 10 | (src) << 7))

typedef struct {
    uint8_t ram[RAM_SIZE];
} ctx_t;

static uint16_t rw(const ctx_t *c, uint32_t a) {
    return (uint16_t)((uint16_t)c->ram[a] << 8 | c->ram[a + 1]);
}
static uint32_t rl(const ctx_t *c, uint32_t a) {
    return (uint32_t)rw(c, a) << 16 | rw(c, a + 2);
}
static void ww(ctx_t *c, uint32_t a, uint16_t v) {
    c->ram[a] = (uint8_t)(v >> 8);
    c->ram[a + 1] = (uint8_t)v;
}
static void wl(ctx_t *c, uint32_t a, uint32_t v) {
    ww(c, a, (uint16_t)(v >> 16));
    ww(c, a + 2, (uint16_t)v);
}
static bool rb(void *p, uint32_t a, uint8_t *v) {
    ctx_t *c = p;
    if (a >= RAM_SIZE)
        return false;
    *v = c->ram[a];
    return true;
}
static bool rw_cb(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    *v = rw(c, a);
    return true;
}
static bool rl_cb(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    if (a + 3 >= RAM_SIZE)
        return false;
    *v = rl(c, a);
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    if (a >= RAM_SIZE)
        return false;
    c->ram[a] = v;
    return true;
}
static bool ww_cb(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    ww(c, a, v);
    return true;
}
static bool wl_cb(void *p, uint32_t a, uint32_t v) {
    return ww_cb(p, a, (uint16_t)(v >> 16)) && ww_cb(p, a + 2, (uint16_t)v);
}

static mc68k_state_t *make_fpu(ctx_t *c, mc68k_fpu_config_t fpu_config) {
    mc68k_bus_t b = {.read_byte = rb,
                     .read_word = rw_cb,
                     .read_long = rl_cb,
                     .write_byte = wb,
                     .write_word = ww_cb,
                     .write_long = wl_cb,
                     .ctx = c};
    memset(c, 0, sizeof(*c));
    wl(c, 0, RESET_SP);
    wl(c, 4, RESET_PC);
    for (unsigned v = 2; v < LAST_VECTOR; v++) {
        wl(c, v * 4, HANDLER(v));
        ww(c, HANDLER(v), 0x60fe); /* BRA.S * */
    }
    for (uint32_t a = RESET_PC - 0x200; a < RESET_PC; a += 2)
        ww(c, a, 0x60fe);
    for (uint32_t a = RESET_PC + 0x100; a < RESET_PC + 0x400; a += 2)
        ww(c, a, 0x60fe); /* 分岐先はどこでも BRA.S * */
    mc68k_state_t *cpu = mc68k_create_model_with_fpu(&b, MC68K_MODEL_68040, fpu_config);
    assert(cpu);
    mc68k_reset(cpu);
    return cpu;
}

static mc68k_state_t *make(ctx_t *c) {
    return make_fpu(c, MC68K_FPU_68040);
}

/* 命令語列を RESET_PC に置き、末尾に BRA.S * を付けて実行する。戻り値は末尾（BRA.S
 * *）のアドレス。 */
static uint32_t run(ctx_t *c, mc68k_state_t *cpu, const uint16_t *words, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        ww(c, RESET_PC + 2 * i, words[i]);
    ww(c, RESET_PC + 2 * n, 0x60fe);
    cpu->pc = RESET_PC;
    mc68k_execute(cpu, 400);
    return RESET_PC + 2 * n;
}

#define RUN(c, cpu, ...)                             \
    run((c), (cpu), (const uint16_t[]){__VA_ARGS__}, \
        sizeof((const uint16_t[]){__VA_ARGS__}) / 2)

static mc68k_fpu_state_t *fst(mc68k_state_t *cpu) {
    return &cpu->fpu040->state;
}

static mc68k_fp80_t fp(uint16_t sign_exp, uint64_t signif) {
    mc68k_fp80_t v = {sign_exp, signif};
    return v;
}
#define ONE     fp(0x3FFF, 0x8000000000000000ull)
#define TWO     fp(0x4000, 0x8000000000000000ull)
#define THREE   fp(0x4000, 0xC000000000000000ull)
#define FOUR    fp(0x4001, 0x8000000000000000ull)
#define FIVE    fp(0x4001, 0xA000000000000000ull)
#define NEG_ONE fp(0xBFFF, 0x8000000000000000ull)
#define ZERO    fp(0, 0)

static bool same(mc68k_fp80_t a, mc68k_fp80_t b) {
    return a.sign_exp == b.sign_exp && a.signif == b.signif;
}

static void wx(ctx_t *c, uint32_t a, mc68k_fp80_t v) {
    wl(c, a, (uint32_t)v.sign_exp << 16);
    wl(c, a + 4, (uint32_t)(v.signif >> 32));
    wl(c, a + 8, (uint32_t)v.signif);
}
static mc68k_fp80_t rx(const ctx_t *c, uint32_t a) {
    return fp((uint16_t)(rl(c, a) >> 16), (uint64_t)rl(c, a + 4) << 32 | rl(c, a + 8));
}

/* pre-instruction（format $0）: PC = 例外を報告した命令。 */
static void expect_pre(const ctx_t *c, const mc68k_state_t *cpu, unsigned vector, uint32_t pc) {
    const uint32_t sp = RESET_SP - 8;
    assert(cpu->a[7] == sp);
    assert(cpu->pc == HANDLER(vector));
    assert(rw(c, sp) == RESET_SR);
    assert(rl(c, sp + 2) == pc);
    assert(rw(c, sp + 6) == (uint16_t)(vector * 4));
}

/* post-instruction（format $3）: PC = 次命令、+8 = EA。 */
static void expect_post(const ctx_t *c, const mc68k_state_t *cpu, unsigned vector, uint32_t pc,
                        uint32_t ea) {
    const uint32_t sp = RESET_SP - 12;
    assert(cpu->a[7] == sp);
    assert(cpu->pc == HANDLER(vector));
    assert(rw(c, sp) == RESET_SR);
    assert(rl(c, sp + 2) == pc);
    assert(rw(c, sp + 6) == (uint16_t)(0x3000u | vector * 4));
    assert(rl(c, sp + 8) == ea);
}

/* ---- 演算・FMOVE・比較 ---- */

static void test_register_arithmetic(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = TWO;
    /* FADD FP1,FP0 */
    uint32_t end = RUN(&c, cpu, 0xF200, 0x0422);
    assert(cpu->pc == end && same(fst(cpu)->fp[0], THREE) && same(fst(cpu)->fp[1], TWO));
    assert(fst(cpu)->fpiar == RESET_PC && fst(cpu)->fpsr_exc == 0 && cpu->a[7] == RESET_SP);
    mc68k_destroy(cpu);

    static const struct {
        uint16_t ext;
        mc68k_fp80_t dst, src, expect;
    } ops[] = {
        {0x0428,
         {0x4001, 0xA000000000000000ull},
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0xC000000000000000ull}}, /* FSUB: 5-2 */
        {0x0423,
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0xC000000000000000ull},
         {0x4001, 0xC000000000000000ull}}, /* FMUL: 2*3 */
        {0x0420,
         {0x4001, 0x8000000000000000ull},
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0x8000000000000000ull}}, /* FDIV: 4/2 */
        {0x0404,
         {0x3FFF, 0x8000000000000000ull},
         {0x4001, 0x8000000000000000ull},
         {0x4000, 0x8000000000000000ull}}, /* FSQRT FP1,FP0 */
        {0x0418,
         {0x3FFF, 0x8000000000000000ull},
         {0xBFFF, 0x8000000000000000ull},
         {0x3FFF, 0x8000000000000000ull}}, /* FABS */
        {0x041A,
         {0x3FFF, 0x8000000000000000ull},
         {0x3FFF, 0x8000000000000000ull},
         {0xBFFF, 0x8000000000000000ull}}, /* FNEG */
        {0x0400,
         {0x3FFF, 0x8000000000000000ull},
         {0x4001, 0xA000000000000000ull},
         {0x4001, 0xA000000000000000ull}}, /* FMOVE */
        {0x0427,
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0xC000000000000000ull},
         {0x4001, 0xC000000000000000ull}}, /* FSGLMUL */
        {0x0424,
         {0x4001, 0x8000000000000000ull},
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0x8000000000000000ull}}, /* FSGLDIV */
    };
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        cpu = make(&c);
        fst(cpu)->fp[0] = ops[i].dst;
        fst(cpu)->fp[1] = ops[i].src;
        end = RUN(&c, cpu, 0xF200, ops[i].ext);
        assert(cpu->pc == end);
        assert(same(fst(cpu)->fp[0], ops[i].expect));
        assert(same(fst(cpu)->fp[1], ops[i].src));
        assert(fst(cpu)->fpsr_exc == 0);
        mc68k_destroy(cpu);
    }
}

static void test_compare_and_test(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = TWO;
    RUN(&c, cpu, 0xF200, 0x0438); /* FCMP FP1,FP0: 1-2 < 0 */
    assert(same(fst(cpu)->fp[0], ONE) && fst(cpu)->fpsr_cc == MC68K_FPU_CC_N);
    mc68k_destroy(cpu);

    cpu = make(&c);
    fst(cpu)->fp[1] = ZERO;
    RUN(&c, cpu, 0xF200, 0x043A); /* FTST FP1 */
    assert(fst(cpu)->fpsr_cc == MC68K_FPU_CC_Z);
    mc68k_destroy(cpu);
}

static void test_memory_and_register_sources(void) {
    ctx_t c;
    mc68k_state_t *cpu;
    uint32_t end;

    /* FMOVE.L D0,FP0 */
    cpu = make(&c);
    cpu->d[0] = 5;
    end = RUN(&c, cpu, 0xF200, EXT_IN(FMT_L, 0, 0));
    assert(cpu->pc == end && same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);

    /* FMOVE.B D0,FP0 / FMOVE.W D0,FP0（上位は無視）。FMOVE.S D0,FP0。 */
    cpu = make(&c);
    cpu->d[0] = 0xFFFFFF05u;
    RUN(&c, cpu, 0xF200, EXT_IN(FMT_B, 0, 0));
    assert(same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);
    cpu = make(&c);
    cpu->d[0] = 0xFFFF0005u;
    RUN(&c, cpu, 0xF200, EXT_IN(FMT_W, 0, 0));
    assert(same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);
    cpu = make(&c);
    cpu->d[0] = 0x40A00000u; /* 5.0f */
    RUN(&c, cpu, 0xF200, EXT_IN(FMT_S, 0, 0));
    assert(same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);

    /* FADD.X (A0)+,FP1: A0 は +12。 */
    cpu = make(&c);
    wx(&c, DATA_AREA, TWO);
    fst(cpu)->fp[1] = ONE;
    cpu->a[0] = DATA_AREA;
    end = RUN(&c, cpu, 0xF218, EXT_IN(FMT_X, 1, 0x22));
    assert(cpu->pc == end && same(fst(cpu)->fp[1], THREE) && cpu->a[0] == DATA_AREA + 12);
    mc68k_destroy(cpu);

    /* FADD.D -(A0),FP1: A0 は -8。5.0 = 0x4014000000000000。 */
    cpu = make(&c);
    wl(&c, DATA_AREA - 8, 0x40140000u);
    wl(&c, DATA_AREA - 4, 0);
    fst(cpu)->fp[1] = ONE;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF220, EXT_IN(FMT_D, 1, 0x22));
    assert(same(fst(cpu)->fp[1], fp(0x4001, 0xC000000000000000ull)) &&
           cpu->a[0] == DATA_AREA - 8);
    mc68k_destroy(cpu);

    /* FMOVE.W #imm,FP0 / FMOVE.L #imm,FP0 / FMOVE.S #imm,FP0 / FMOVE.D #imm,FP0。 */
    cpu = make(&c);
    end = RUN(&c, cpu, 0xF23C, EXT_IN(FMT_W, 0, 0), 0x0005);
    assert(cpu->pc == end && same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);
    cpu = make(&c);
    end = RUN(&c, cpu, 0xF23C, EXT_IN(FMT_B, 0, 0), 0x0005); /* バイトは語の下位 */
    assert(cpu->pc == end && same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);
    cpu = make(&c);
    end = RUN(&c, cpu, 0xF23C, EXT_IN(FMT_L, 0, 0), 0x0000, 0x0005);
    assert(cpu->pc == end && same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);
    cpu = make(&c);
    end = RUN(&c, cpu, 0xF23C, EXT_IN(FMT_S, 0, 0), 0x40A0, 0x0000);
    assert(cpu->pc == end && same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);
    cpu = make(&c);
    end = RUN(&c, cpu, 0xF23C, EXT_IN(FMT_D, 0, 0), 0x4014, 0x0000, 0x0000, 0x0000);
    assert(cpu->pc == end && same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);

    /* FADD.X (d16,A0),FP0 */
    cpu = make(&c);
    wx(&c, DATA_AREA + 0x20, TWO);
    fst(cpu)->fp[0] = ONE;
    cpu->a[0] = DATA_AREA;
    end = RUN(&c, cpu, 0xF228, EXT_IN(FMT_X, 0, 0x22), 0x0020);
    assert(cpu->pc == end && same(fst(cpu)->fp[0], THREE));
    mc68k_destroy(cpu);
}

static void test_register_destinations(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* FMOVE.X FP0,(A0): 96 bit。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = THREE;
    cpu->a[0] = DATA_AREA;
    uint32_t end = RUN(&c, cpu, 0xF210, EXT_OUT(FMT_X, 0));
    assert(cpu->pc == end && same(rx(&c, DATA_AREA), THREE) && rw(&c, DATA_AREA + 2) == 0);
    assert(fst(cpu)->fpiar == RESET_PC);
    mc68k_destroy(cpu);

    /* FMOVE.S FP0,(A0)+ / FMOVE.D FP0,-(A0) */
    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF218, EXT_OUT(FMT_S, 0));
    assert(rl(&c, DATA_AREA) == 0x40A00000u && cpu->a[0] == DATA_AREA + 4);
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF220, EXT_OUT(FMT_D, 0));
    assert(rl(&c, DATA_AREA - 8) == 0x40140000u && rl(&c, DATA_AREA - 4) == 0 &&
           cpu->a[0] == DATA_AREA - 8);
    mc68k_destroy(cpu);

    /* (An)+/-(An) の更新幅は形式の大きさ（B=1、W=2、L=4、A7 の B は 2）。 */
    static const struct {
        uint16_t opcode;
        unsigned fmt;
        uint32_t a_after;
    } widths[] = {
        {0xF218, FMT_B, DATA_AREA + 1}, {0xF218, FMT_W, DATA_AREA + 2},
        {0xF218, FMT_L, DATA_AREA + 4}, {0xF220, FMT_B, DATA_AREA - 1},
        {0xF220, FMT_W, DATA_AREA - 2}, {0xF220, FMT_L, DATA_AREA - 4},
    };
    for (unsigned i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        cpu = make(&c);
        fst(cpu)->fp[0] = FIVE;
        cpu->a[0] = DATA_AREA;
        RUN(&c, cpu, widths[i].opcode, EXT_OUT(widths[i].fmt, 0));
        assert(cpu->a[0] == widths[i].a_after);
        mc68k_destroy(cpu);
    }
    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    RUN(&c, cpu, 0xF227, EXT_OUT(FMT_B, 0)); /* FMOVE.B FP0,-(A7) */
    assert(cpu->a[7] == RESET_SP - 2 && c.ram[RESET_SP - 2] == 5);
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    RUN(&c, cpu, 0xF21F, EXT_OUT(FMT_B, 0)); /* FMOVE.B FP0,(A7)+ */
    assert(cpu->a[7] == RESET_SP + 2);
    mc68k_destroy(cpu);

    /* FMOVE.L FP0,D1 / FMOVE.W / FMOVE.B（下位だけ更新）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    cpu->d[1] = 0xAAAAAAAAu;
    RUN(&c, cpu, 0xF201, EXT_OUT(FMT_L, 0));
    assert(cpu->d[1] == 5);
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    cpu->d[1] = 0xAAAAAAAAu;
    RUN(&c, cpu, 0xF201, EXT_OUT(FMT_W, 0));
    assert(cpu->d[1] == 0xAAAA0005u);
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    cpu->d[1] = 0xAAAAAAAAu;
    RUN(&c, cpu, 0xF201, EXT_OUT(FMT_B, 0));
    assert(cpu->d[1] == 0xAAAAAA05u);
    mc68k_destroy(cpu);

    /* FMOVE.S FP0,D1（FPSR は変えない。EXC byte は 0）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    RUN(&c, cpu, 0xF201, EXT_OUT(FMT_S, 0));
    assert(cpu->d[1] == 0x40A00000u && fst(cpu)->fpsr_exc == 0);
    mc68k_destroy(cpu);
}

/* ---- control register / FMOVEM ---- */

static void test_control_registers(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* FMOVE.L D0,FPCR: ENABLE=OVFL|OPERR、PREC=double(2)、RND=RM(2)。 */
    cpu = make(&c);
    cpu->d[0] = 0x00003000u | 0x80u | 0x20u;
    RUN(&c, cpu, 0xF200, 0x9000);
    assert(fst(cpu)->fpcr_enable == (EXC_OVFL | EXC_OPERR));
    assert(fst(cpu)->fpcr_round_prec == MC68K_FPU_PREC_DOUBLE);
    assert(fst(cpu)->fpcr_round_mode == MC68K_FPU_ROUND_MINUS_INF);
    assert(fst(cpu)->fpiar == 0); /* FMOVE FPcr は FPIAR を変えない */
    /* FMOVE.L FPCR,D1 */
    RUN(&c, cpu, 0xF201, 0xB000);
    assert(cpu->d[1] == (0x00003000u | 0x80u | 0x20u));
    mc68k_destroy(cpu);

    /* FMOVE.L #imm,FPSR / FMOVE.L FPSR,D0（EXC を書いても例外にならない）。 */
    cpu = make(&c);
    RUN(&c, cpu, 0xF23C, 0x8800, 0x0900, 0x0000);
    assert(fst(cpu)->fpsr_cc == (MC68K_FPU_CC_N | MC68K_FPU_CC_NAN));
    mc68k_destroy(cpu);
    cpu = make(&c);
    RUN(&c, cpu, 0xF23C, 0x8800, 0x0000, 0xFF00);
    assert(fst(cpu)->fpsr_exc == 0xFF && cpu->a[7] == RESET_SP);
    mc68k_destroy(cpu);

    /* FMOVE.L FPIAR,A0 / FMOVE.L A0,FPIAR */
    cpu = make(&c);
    cpu->a[0] = 0x12345678u;
    RUN(&c, cpu, 0xF208, 0x8400);
    assert(fst(cpu)->fpiar == 0x12345678u);
    cpu->a[1] = 0;
    RUN(&c, cpu, 0xF209, 0xA400);
    assert(cpu->a[1] == 0x12345678u);
    mc68k_destroy(cpu);

    /* FMOVEM.L FPCR/FPSR/FPIAR,-(A0): 3 語を FPCR, FPSR, FPIAR の昇順に置く。 */
    cpu = make(&c);
    fst(cpu)->fpcr_enable = EXC_DZ;
    fst(cpu)->fpiar = 0xCAFEBABEu;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF220, 0xBC00);
    assert(cpu->a[0] == DATA_AREA - 12);
    assert(rl(&c, DATA_AREA - 12) == (uint32_t)EXC_DZ << 8);
    assert(rl(&c, DATA_AREA - 8) == 0 && rl(&c, DATA_AREA - 4) == 0xCAFEBABEu);
    /* 逆: (A0)+ から FPCR/FPSR/FPIAR。 */
    wl(&c, DATA_AREA, 0);
    wl(&c, DATA_AREA + 4, 0);
    wl(&c, DATA_AREA + 8, 0x11112222u);
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF218, 0x9C00);
    assert(fst(cpu)->fpcr_enable == 0 && fst(cpu)->fpiar == 0x11112222u &&
           cpu->a[0] == DATA_AREA + 12);
    mc68k_destroy(cpu);
}

static void test_fmovem(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* FMOVEM.X FP0,-(A1)（動的リスト、D0 = 1: predecrement は bit r = FPr）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = TWO;
    fst(cpu)->fp[7] = THREE;
    cpu->d[0] = 0x01;
    cpu->a[1] = DATA_AREA + 12;
    RUN(&c, cpu, 0xF221, 0xE800);
    assert(cpu->a[1] == DATA_AREA && same(rx(&c, DATA_AREA), TWO));
    mc68k_destroy(cpu);

    /* FScc 絶対ロング（FTRAPcc と取り違えない）。 */
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_Z;
    RUN(&c, cpu, 0xF279, 0x0001, 0x0000, DATA_AREA);
    assert(c.ram[DATA_AREA] == 0xFF && cpu->a[7] == RESET_SP);
    mc68k_destroy(cpu);

    /* FMOVEM.X FP0-FP1,-(A0)（predecrement: bit r = FPr）。FP0 が低いアドレス。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = TWO;
    cpu->a[0] = DATA_AREA;
    uint32_t end = RUN(&c, cpu, 0xF220, 0xE003);
    assert(cpu->pc == end && cpu->a[0] == DATA_AREA - 24);
    assert(same(rx(&c, DATA_AREA - 24), ONE) && same(rx(&c, DATA_AREA - 12), TWO));
    assert(fst(cpu)->fpiar == 0);
    mc68k_destroy(cpu);

    /* FMOVEM.X (A0)+,FP2-FP3（postincrement: bit 7-r = FPr、FP2 = bit5、FP3 = bit4）。 */
    cpu = make(&c);
    wx(&c, DATA_AREA, THREE);
    wx(&c, DATA_AREA + 12, FOUR);
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF218, 0xD030);
    assert(same(fst(cpu)->fp[2], THREE) && same(fst(cpu)->fp[3], FOUR));
    assert(cpu->a[0] == DATA_AREA + 24);
    mc68k_destroy(cpu);

    /* FMOVEM.X FP0,(A0)（静的リスト、制御モード）。動的リスト: D1 = FP7 だけ。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF210, 0xF080);
    assert(same(rx(&c, DATA_AREA), FIVE));
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[7] = FIVE;
    cpu->d[1] = 0x01;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF210, 0xF810 | 0x0000); /* 動的 (postinc 形): Dn=D1 → bit0 = FP7 */
    assert(same(rx(&c, DATA_AREA), FIVE));
    mc68k_destroy(cpu);
}

/* ---- 条件命令 ---- */

static void test_conditional(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* FBEQ.W: Z が立っていれば分岐（変位の先頭が基点）。 */
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_Z;
    RUN(&c, cpu, 0xF281, 0x0100);
    assert(cpu->pc == RESET_PC + 2 + 0x100);
    mc68k_destroy(cpu);
    cpu = make(&c);
    uint32_t end = RUN(&c, cpu, 0xF281, 0x0100);
    assert(cpu->pc == end);
    mc68k_destroy(cpu);

    /* FBEQ.L */
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_Z;
    RUN(&c, cpu, 0xF2C1, 0x0000, 0x0200);
    assert(cpu->pc == RESET_PC + 2 + 0x200);
    mc68k_destroy(cpu);

    /* FBEQ.L の負の変位（上位語が非 0）。 */
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_Z;
    RUN(&c, cpu, 0xF2C1, 0xFFFF, 0xFF00);
    assert(cpu->pc == RESET_PC + 2 - 0x100);
    mc68k_destroy(cpu);

    /* FNOP（FBF.W #0）は何もしない。 */
    cpu = make(&c);
    end = RUN(&c, cpu, 0xF280, 0x0000);
    assert(cpu->pc == end && cpu->a[7] == RESET_SP);
    mc68k_destroy(cpu);

    /* FSEQ D0 / FSEQ (A0)+ */
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_Z;
    cpu->d[0] = 0x12345600u;
    RUN(&c, cpu, 0xF240, 0x0001);
    assert(cpu->d[0] == 0x123456FFu);
    mc68k_destroy(cpu);
    cpu = make(&c);
    cpu->a[0] = DATA_AREA;
    wl(&c, DATA_AREA, 0xAAAAAAAAu);
    RUN(&c, cpu, 0xF258, 0x0001);
    assert(c.ram[DATA_AREA] == 0 && cpu->a[0] == DATA_AREA + 1);
    mc68k_destroy(cpu);

    /* FDBEQ D0,<disp>: 偽ならデクリメントして分岐、-1 で抜ける。 */
    cpu = make(&c);
    cpu->d[0] = 0xABCD0002u;
    RUN(&c, cpu, 0xF248, 0x0001, 0x0100);
    assert(cpu->d[0] == 0xABCD0001u && cpu->pc == RESET_PC + 4 + 0x100);
    mc68k_destroy(cpu);
    cpu = make(&c);
    cpu->d[0] = 0x10000;
    end = RUN(&c, cpu, 0xF248, 0x0001, 0x0100);
    assert((cpu->d[0] & 0xFFFF) == 0xFFFF && cpu->pc == end);
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_Z;
    cpu->d[0] = 2;
    end = RUN(&c, cpu, 0xF248, 0x0001, 0x0100);
    assert(cpu->d[0] == 2 && cpu->pc == end);
    mc68k_destroy(cpu);

    /* FTRAPcc: 真なら vector 7、PC は次命令（オペランドの後）。 */
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_Z;
    RUN(&c, cpu, 0xF27A, 0x0001, 0x1234);
    assert(cpu->pc == HANDLER(VEC_TRAPCC));
    assert(cpu->a[7] == RESET_SP - 12 && rw(&c, cpu->a[7]) == RESET_SR);
    assert(rl(&c, cpu->a[7] + 2) == RESET_PC + 6);
    assert(rw(&c, cpu->a[7] + 6) == (uint16_t)(0x2000u | VEC_TRAPCC * 4)); /* format $2 */
    assert(rl(&c, cpu->a[7] + 8) == RESET_PC);                             /* 命令のアドレス */
    mc68k_destroy(cpu);
    cpu = make(&c);
    end = RUN(&c, cpu, 0xF27B, 0x0001, 0x1234, 0x5678);
    assert(cpu->pc == end);
    mc68k_destroy(cpu);
}

static void test_bsun(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* IEEE nonaware 条件（FBGT = $12）+ NAN: BSUN。無効なら aware 版（FBOGT =
     * $02）と同じに評価。 */
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_NAN;
    uint32_t end = RUN(&c, cpu, 0xF292, 0x0100);
    assert(cpu->pc == end && (fst(cpu)->fpsr_exc & EXC_BSUN));
    mc68k_destroy(cpu);

    /* 有効なら pre-instruction（format $0、PC = 命令）。分岐しない。 */
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_NAN;
    fst(cpu)->fpcr_enable = EXC_BSUN;
    RUN(&c, cpu, 0xF292, 0x0100);
    expect_pre(&c, cpu, VEC_BSUN, RESET_PC);
    assert(fst(cpu)->fpsr_exc & EXC_BSUN);
    mc68k_destroy(cpu);

    /* aware 条件は BSUN にならない。 */
    cpu = make(&c);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_NAN;
    fst(cpu)->fpcr_enable = EXC_BSUN;
    end = RUN(&c, cpu, 0xF282, 0x0100);
    assert(cpu->pc == end && !(fst(cpu)->fpsr_exc & EXC_BSUN));
    mc68k_destroy(cpu);
}

/* ---- 算術例外: 保留ラッチと post-instruction ---- */

static void test_pending_dz(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* DZ 無効: 結果は無限大、例外なし。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    uint32_t end = RUN(&c, cpu, 0xF200, 0x0420);
    assert(cpu->pc == end && same(fst(cpu)->fp[0], fp(0x7FFF, 0x8000000000000000ull)));
    assert(fst(cpu)->fpsr_exc == EXC_DZ && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);

    /* DZ 有効: 宛先は変えず保留。次の FP 命令の開始で pre-instruction（PC = その命令）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    RUN(&c, cpu, 0xF200, 0x0420, 0xF200, 0x0422); /* 2 命令目の FADD は実行されない */
    expect_pre(&c, cpu, VEC_DZ, RESET_PC + 4);
    assert(same(fst(cpu)->fp[0], ONE));  /* DZ は宛先を変えない。次の FADD も実行されない */
    assert(fst(cpu)->fpiar == RESET_PC); /* FPIAR は例外を起こした命令 */
    assert(cpu->fpu040->exc.valid && cpu->fpu040->exc.vector == VEC_DZ); /* FSAVE まで残る */
    mc68k_destroy(cpu);

    /* 保留中も FMOVE FPcr / FMOVEM は報告せず通過する。後続の FNOP で報告。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF200, 0x0420, 0xF210, 0xF080, 0xF200, 0xB000, 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_DZ, RESET_PC + 12);
    mc68k_destroy(cpu);

    /* FScc は拡張語の予約ビットが立っていても報告点（opclass と取り違えない）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    RUN(&c, cpu, 0xF200, 0x0420, 0xF240, 0xE001);
    expect_pre(&c, cpu, VEC_DZ, RESET_PC + 4);
    mc68k_destroy(cpu);

    /* FBcc も報告点（FNOP = FBF）。 */
    cpu = make(&c);
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fpcr_enable = EXC_DZ;
    RUN(&c, cpu, 0xF200, 0x0420, 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_DZ, RESET_PC + 4);
    mc68k_destroy(cpu);
}

static void test_nonmaskable_overflow_underflow(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* OVFL は ENABLE に関わらず報告する。宛先は変えない。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x7FFE, 0xFFFFFFFFFFFFFFFFull);
    fst(cpu)->fp[1] = TWO;
    RUN(&c, cpu, 0xF200, 0x0423, 0xF280, 0x0000); /* FMUL FP1,FP0; FNOP */
    expect_pre(&c, cpu, VEC_OVFL, RESET_PC + 4);
    assert(same(fst(cpu)->fp[0], fp(0x7FFE, 0xFFFFFFFFFFFFFFFFull)));
    assert(fst(cpu)->fpsr_exc & EXC_OVFL);
    mc68k_destroy(cpu);

    /* UNFL も同様。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x0001, 0x8000000000000001ull); /* 最小の正規数 + 1 ulp */
    fst(cpu)->fp[1] = fp(0x3FFD, 0x8000000000000000ull); /* 0.25: 結果は不正確な denormal */
    RUN(&c, cpu, 0xF200, 0x0423, 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_UNFL, RESET_PC + 4);
    assert(same(fst(cpu)->fp[0], fp(0x0001, 0x8000000000000001ull)));
    mc68k_destroy(cpu);

    /* OVFL 有効 + INEX 有効でも優先順位は OVFL。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x7FFE, 0xFFFFFFFFFFFFFFFFull);
    fst(cpu)->fp[1] = TWO;
    fst(cpu)->fpcr_enable = EXC_OVFL | EXC_INEX2;
    RUN(&c, cpu, 0xF200, 0x0423, 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_OVFL, RESET_PC + 4);
    mc68k_destroy(cpu);
}

/* 優先順位 BSUN > SNAN > OPERR > OVFL > UNFL > DZ > INEX2 > INEX1（[040UM §9.7]）。
 * 報告する例外の選択関数を、EXC byte と ENABLE byte の全 65536
 * 組で、独立に書いた表と突き合わせる。 INEX2 は OVFL が立つときだけ、ENABLE.INEX2
 * から候補に入る（EXC.INEX2 が無くてもよい）。 */
static void test_exception_priority_exhaustive(void) {
    static const struct {
        uint8_t bit, vector;
    } order[] = {
        {EXC_BSUN, 48}, {EXC_SNAN, 54}, {EXC_OPERR, 52}, {EXC_OVFL, 53},
        {EXC_UNFL, 51}, {EXC_DZ, 50},   {EXC_INEX2, 49}, {MC68K_FPU_EXC_INEX1, 49},
    };
    enum { ORDER_COUNT = sizeof(order) / sizeof(order[0]), BYTE_VALUES = 256 };
    unsigned checked = 0;

    for (unsigned exc = 0; exc < BYTE_VALUES; exc++) {
        for (unsigned en = 0; en < BYTE_VALUES; en++) {
            unsigned active = exc & en;
            uint8_t want_bit = 0, want_vector = 0, got_bit = 0xFF;
            if ((exc & EXC_OVFL) && (en & EXC_INEX2))
                active |= EXC_INEX2;
            for (unsigned i = 0; i < ORDER_COUNT; i++) {
                if (active & order[i].bit) {
                    want_bit = order[i].bit;
                    want_vector = order[i].vector;
                    break;
                }
            }
            uint8_t got = fpu_select_exception((uint8_t)exc, (uint8_t)en, &got_bit);
            assert(got == want_vector && got_bit == want_bit);
            checked++;
        }
    }
    assert(checked == BYTE_VALUES * BYTE_VALUES);
}

/* 実行経路での優先順位: 同時に立った複数の例外のうち、最上位だけが報告される。 */
static void test_exception_priority_instructions(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* SNaN → 整数: ENABLE に下位を全部立てても SNAN。EXC は SNAN だけ（Table 9-11 の FMOVE to
     * B,W,L の OPERR は 非 signaling NaN と +Inf。§9.7 の同時例外の一覧に SNAN と OPERR
     * の組は無い）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x7FFF, 0xA000000000000000ull);
    fst(cpu)->fpcr_enable = EXC_OPERR | EXC_INEX2 | EXC_DZ | EXC_UNFL | EXC_OVFL;
    RUN(&c, cpu, 0xF200, EXT_OUT(FMT_L, 0));
    expect_post(&c, cpu, VEC_SNAN, RESET_PC + 4, 0);
    assert(fst(cpu)->fpsr_exc & EXC_SNAN && !(fst(cpu)->fpsr_exc & EXC_OPERR));
    mc68k_destroy(cpu);

    /* 範囲外の整数変換 + INEX2 有効: OPERR は INEX2 より上。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x4027, 0x8000000000000001ull); /* 2^40 + 端数 */
    fst(cpu)->fpcr_enable = EXC_INEX2;
    RUN(&c, cpu, 0xF200, EXT_OUT(FMT_L, 0));
    expect_post(&c, cpu, VEC_OPERR, RESET_PC + 4, 0);
    mc68k_destroy(cpu);

    /* UNFL（不正確な denormal）+ INEX2 有効: UNFL は INEX2 より上。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x0001, 0x8000000000000001ull);
    fst(cpu)->fp[1] = fp(0x3FFD, 0x8000000000000000ull);
    fst(cpu)->fpcr_enable = EXC_INEX2;
    RUN(&c, cpu, 0xF200, 0x0423, 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_UNFL, RESET_PC + 4);
    assert((fst(cpu)->fpsr_exc & (EXC_UNFL | EXC_INEX2)) == (EXC_UNFL | EXC_INEX2));
    mc68k_destroy(cpu);

    /* OVFL（EXC は OVFL と INEX2）+ DZ 以外の下位を全部有効: OVFL。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x7FFE, 0xFFFFFFFFFFFFFFFFull);
    fst(cpu)->fp[1] = TWO;
    fst(cpu)->fpcr_enable = EXC_UNFL | EXC_DZ | EXC_INEX2;
    RUN(&c, cpu, 0xF200, 0x0423, 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_OVFL, RESET_PC + 4);
    mc68k_destroy(cpu);
}

static void test_inexact_writes_result(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* INEX2 有効: 丸めた結果を書いてから保留（1/3）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = THREE;
    fst(cpu)->fpcr_enable = EXC_INEX2;
    RUN(&c, cpu, 0xF200, 0x0420, 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_INEX, RESET_PC + 4);
    assert(!same(fst(cpu)->fp[0], ONE) && fst(cpu)->fp[0].sign_exp == 0x3FFD);
    assert(fst(cpu)->fpsr_exc & EXC_INEX2);
    mc68k_destroy(cpu);

    /* INEX2 無効: 例外なし。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = THREE;
    uint32_t end = RUN(&c, cpu, 0xF200, 0x0420);
    assert(cpu->pc == end && (fst(cpu)->fpsr_exc & EXC_INEX2) && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);
}

static void test_operr_snan_maskable(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* OPERR 無効: 既定 NaN（仮数全ビット 1）。有効: 宛先は変えず保留。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x7FFF, 0x8000000000000000ull); /* +Inf */
    fst(cpu)->fp[1] = fp(0xFFFF, 0x8000000000000000ull); /* -Inf */
    uint32_t end = RUN(&c, cpu, 0xF200, 0x0422);         /* FADD */
    assert(cpu->pc == end && same(fst(cpu)->fp[0], fp(0x7FFF, 0xFFFFFFFFFFFFFFFFull)));
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x7FFF, 0x8000000000000000ull);
    fst(cpu)->fp[1] = fp(0xFFFF, 0x8000000000000000ull);
    fst(cpu)->fpcr_enable = EXC_OPERR;
    RUN(&c, cpu, 0xF200, 0x0422, 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_OPERR, RESET_PC + 4);
    assert(same(fst(cpu)->fp[0], fp(0x7FFF, 0x8000000000000000ull)));
    mc68k_destroy(cpu);

    /* SNaN 有効: 保留（SNAN）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = fp(0x7FFF, 0xA000000000000000ull); /* SNaN */
    fst(cpu)->fpcr_enable = EXC_SNAN;
    RUN(&c, cpu, 0xF200, 0x0422, 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_SNAN, RESET_PC + 4);
    mc68k_destroy(cpu);
}

static void test_post_instruction_exceptions(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* FMOVE.L FP0,D0 で範囲外: OPERR は nonmaskable で、実行後（format $3）。EA は 0。D0
     * は不変。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x4027, 0x8000000000000000ull); /* 2^40 */
    cpu->d[0] = 0x55555555u;
    RUN(&c, cpu, 0xF200, EXT_OUT(FMT_L, 0));
    expect_post(&c, cpu, VEC_OPERR, RESET_PC + 4, 0);
    assert(cpu->d[0] == 0x55555555u && (fst(cpu)->fpsr_exc & EXC_OPERR));
    assert(fst(cpu)->fpiar == RESET_PC);
    mc68k_destroy(cpu);

    /* FMOVE.B FP0,(A0): -128 ちょうども 040 では OPERR（推論 6）。メモリは書かない。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0xC006, 0x8000000000000000ull); /* -128 */
    cpu->a[0] = DATA_AREA;
    c.ram[DATA_AREA] = 0x77;
    RUN(&c, cpu, 0xF210, EXT_OUT(FMT_B, 0));
    expect_post(&c, cpu, VEC_OPERR, RESET_PC + 4, DATA_AREA);
    assert(c.ram[DATA_AREA] == 0x77);
    mc68k_destroy(cpu);

    /* 範囲内の -127 は例外なし。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0xC005, 0xFE00000000000000ull); /* -127 */
    cpu->a[0] = DATA_AREA;
    uint32_t end = RUN(&c, cpu, 0xF210, EXT_OUT(FMT_B, 0));
    assert(cpu->pc == end && c.ram[DATA_AREA] == 0x81);
    mc68k_destroy(cpu);

    /* SNaN を整数へ: nonmaskable な SNAN（ENABLE に関わらず）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x7FFF, 0xA000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_OUT(FMT_L, 0));
    expect_post(&c, cpu, VEC_SNAN, RESET_PC + 4, 0);
    mc68k_destroy(cpu);

    /* FMOVE.S FP0,(A0)+ で OVFL: nonmaskable、EA は更新前のアドレス、A0
     * は更新済み。メモリ不変。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x4100, 0x8000000000000000ull);
    cpu->a[0] = DATA_AREA;
    wl(&c, DATA_AREA, 0x11223344u);
    RUN(&c, cpu, 0xF218, EXT_OUT(FMT_S, 0));
    expect_post(&c, cpu, VEC_OVFL, RESET_PC + 4, DATA_AREA);
    assert(cpu->a[0] == DATA_AREA + 4 && rl(&c, DATA_AREA) == 0x11223344u);
    mc68k_destroy(cpu);

    /* FMOVE.S FP0,(A0) で INEX2 有効: 結果を書いてから post（INEX）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] =
        fp(0x3FFF, 0x8000000000000001ull); /* 1 + 2^-63: single へ丸めると不正確 */
    fst(cpu)->fpcr_enable = EXC_INEX2;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF210, EXT_OUT(FMT_S, 0));
    expect_post(&c, cpu, VEC_INEX, RESET_PC + 4, DATA_AREA);
    assert(rl(&c, DATA_AREA) == 0x3F800000u);
    mc68k_destroy(cpu);
}

/* ---- 未対応データ型（vector 55） ---- */

static void test_unsupported_data_types(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* FADD.X (A0)+,FP1: ソースが denormal。pre-instruction（format $0）、An は更新済み、FP1
     * 不変。 */
    cpu = make(&c);
    wx(&c, DATA_AREA, fp(0x0000, 0x0000000000000001ull));
    fst(cpu)->fp[1] = ONE;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF218, EXT_IN(FMT_X, 1, 0x22), 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4);
    assert(cpu->a[0] == DATA_AREA + 12 && same(fst(cpu)->fp[1], ONE));
    assert(fst(cpu)->fpsr_exc == 0 && fst(cpu)->fpiar == RESET_PC);
    mc68k_destroy(cpu);

    /* unnormalized（指数 ≠ 0、整数部 0）、単精度・倍精度 denormal、packed。 */
    static const struct {
        uint16_t opcode, ext;
        uint32_t w[3];
    } src[] = {
        {0xF218, EXT_IN(FMT_X, 1, 0x22), {0x3FFF0000u, 0x40000000u, 0}}, /* .X unnormalized */
        {0xF218, EXT_IN(FMT_S, 1, 0x22), {0x00400000u, 0, 0}},           /* .S denormal */
        {0xF218, EXT_IN(FMT_D, 1, 0x22), {0x000FFFFFu, 0xFFFFFFFFu, 0}}, /* .D denormal */
        {0xF218, EXT_IN(FMT_P, 1, 0x22), {0x3FFF0000u, 0x80000000u, 0}}, /* .P（常に未対応） */
    };
    for (unsigned i = 0; i < sizeof(src) / sizeof(src[0]); i++) {
        cpu = make(&c);
        wl(&c, DATA_AREA, src[i].w[0]);
        wl(&c, DATA_AREA + 4, src[i].w[1]);
        wl(&c, DATA_AREA + 8, src[i].w[2]);
        fst(cpu)->fp[1] = ONE;
        cpu->a[0] = DATA_AREA;
        RUN(&c, cpu, src[i].opcode, src[i].ext, 0xF280, 0x0000);
        expect_pre(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4);
        assert(same(fst(cpu)->fp[1], ONE));
        mc68k_destroy(cpu);
    }

    /* 単精度の正規数・零・無限大は未対応ではない。 */
    cpu = make(&c);
    cpu->d[0] = 0x00800000u; /* 最小の正規単精度 */
    uint32_t end = RUN(&c, cpu, 0xF200, EXT_IN(FMT_S, 0, 0));
    assert(cpu->pc == end);
    mc68k_destroy(cpu);

    /* レジスタ間: ソースの denormal、二項演算の宛先の denormal（単項は宛先を読まない）。 */
    cpu = make(&c);
    fst(cpu)->fp[1] = fp(0x0000, 0x0000000000000001ull);
    RUN(&c, cpu, 0xF200, 0x0400, 0xF280, 0x0000); /* FMOVE FP1,FP0 */
    expect_pre(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4);
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x0000, 0x0000000000000001ull);
    fst(cpu)->fp[1] = ONE;
    RUN(&c, cpu, 0xF200, 0x0422, 0xF280, 0x0000); /* FADD FP1,FP0 */
    expect_pre(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4);
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x0000, 0x0000000000000001ull);
    fst(cpu)->fp[1] = FOUR;
    end = RUN(&c, cpu, 0xF200, 0x0400); /* FMOVE FP1,FP0: 宛先の denormal は読まれない */
    assert(cpu->pc == end && same(fst(cpu)->fp[0], FOUR));
    mc68k_destroy(cpu);

    /* FMOVE.P FP0,(A0): 実行後（format $3）、EA = 宛先、メモリは書かない。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    cpu->a[0] = DATA_AREA;
    wl(&c, DATA_AREA, 0x11223344u);
    RUN(&c, cpu, 0xF210, EXT_OUT(FMT_P, 0));
    expect_post(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4, DATA_AREA);
    assert(rl(&c, DATA_AREA) == 0x11223344u);
    mc68k_destroy(cpu);

    /* denormal のレジスタを FMOVE.X で書き出す: 実行後。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x0000, 0x0000000000000001ull);
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF210, EXT_OUT(FMT_X, 0));
    expect_post(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4, DATA_AREA);
    mc68k_destroy(cpu);

    /* FMOVEM は denormal を素通しする。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x0000, 0x0000000000000001ull);
    cpu->a[0] = DATA_AREA;
    end = RUN(&c, cpu, 0xF210, 0xF080);
    assert(cpu->pc == end && same(rx(&c, DATA_AREA), fp(0x0000, 1)));
    mc68k_destroy(cpu);
}

/* 未対応データ型の境界: 指数 0 でも仮数が 0 なら零、指数最大は無限大・NaN。 */
static void test_unsupported_boundaries(void) {
    ctx_t c;
    mc68k_state_t *cpu;
    static const struct {
        uint16_t ext;
        uint32_t w[3];
        bool unsupported;
    } cases[] = {
        {EXT_IN(FMT_S, 1, 0), {0x00000000u, 0, 0}, false}, /* +0 */
        {EXT_IN(FMT_S, 1, 0), {0x80000000u, 0, 0}, false}, /* -0 */
        {EXT_IN(FMT_S, 1, 0), {0x80400000u, 0, 0}, true},  /* 負の denormal */
        {EXT_IN(FMT_S, 1, 0), {0x00000001u, 0, 0}, true},  /* 最小の denormal */
        {EXT_IN(FMT_S, 1, 0), {0x40400000u, 0, 0}, false}, /* 3.0f（指数 $80） */
        {EXT_IN(FMT_S, 1, 0), {0x7F800000u, 0, 0}, false}, /* +Inf */
        {EXT_IN(FMT_D, 1, 0), {0x00000000u, 0, 0}, false}, /* +0 */
        {EXT_IN(FMT_D, 1, 0), {0x00000001u, 0, 0}, true},  /* 仮数の上位側だけ非 0 */
        {EXT_IN(FMT_D, 1, 0), {0x00000000u, 1, 0}, true},  /* 仮数の下位側だけ非 0 */
        {EXT_IN(FMT_D, 1, 0), {0x40080000u, 0, 0}, false}, /* 3.0（指数 $400） */
        {EXT_IN(FMT_D, 1, 0), {0x7FF00000u, 0, 0}, false}, /* +Inf */
        {EXT_IN(FMT_X, 1, 0), {0x00000000u, 0, 0}, false}, /* +0 */
        {EXT_IN(FMT_X, 1, 0),
         {0x7FFF0000u, 0, 0},
         false}, /* 無限大（整数部ビット 0 の符号化） */
        {EXT_IN(FMT_X, 1, 0), {0x7FFF0000u, 0x40000000u, 0}, false}, /* NaN */
        {EXT_IN(FMT_X, 1, 0), {0x3FFF0000u, 0, 0}, true},            /* unnormalized zero */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cpu = make(&c);
        wl(&c, DATA_AREA, cases[i].w[0]);
        wl(&c, DATA_AREA + 4, cases[i].w[1]);
        wl(&c, DATA_AREA + 8, cases[i].w[2]);
        cpu->a[0] = DATA_AREA;
        const uint32_t end = RUN(&c, cpu, 0xF210, cases[i].ext, 0xF280, 0x0000);
        if (cases[i].unsupported)
            expect_pre(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4);
        else
            assert(cpu->pc == end);
        mc68k_destroy(cpu);
    }

    /* 宛先レジスタを読む opmode は denormal で未対応、読まない opmode は素通し。 */
    static const uint8_t reads_dst[] = {0x20, 0x22, 0x23, 0x24, 0x27, 0x28, 0x38};
    static const uint8_t ignores_dst[] = {0x00, 0x04, 0x18, 0x1A};
    for (unsigned i = 0; i < sizeof(reads_dst); i++) {
        cpu = make(&c);
        fst(cpu)->fp[0] = fp(0x0000, 1);
        fst(cpu)->fp[1] = ONE;
        RUN(&c, cpu, 0xF200, EXT_RR(1, 0, reads_dst[i]), 0xF280, 0x0000);
        expect_pre(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4);
        mc68k_destroy(cpu);
    }
    for (unsigned i = 0; i < sizeof(ignores_dst); i++) {
        cpu = make(&c);
        fst(cpu)->fp[0] = fp(0x0000, 1);
        fst(cpu)->fp[1] = FOUR;
        const uint32_t end = RUN(&c, cpu, 0xF200, EXT_RR(1, 0, ignores_dst[i]));
        assert(cpu->pc == end);
        mc68k_destroy(cpu);
    }
    /* FTST は宛先を持たない（FP0 が denormal でも実行する）。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x0000, 1);
    fst(cpu)->fp[1] = ZERO;
    const uint32_t end = RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x3A));
    assert(cpu->pc == end && fst(cpu)->fpsr_cc == MC68K_FPU_CC_Z);
    mc68k_destroy(cpu);
}

/* 整数宛ての FMOVE: 範囲内は例外なし、その形式の最小値と範囲外は OPERR（推論 6）。 */
static void test_integer_destination_limits(void) {
    static const struct {
        unsigned fmt;
        uint16_t se;
        uint64_t sig;
        bool operr;
    } cases[] = {
        {FMT_W, 0xC00E, 0x8000000000000000ull, true},  /* -32768 */
        {FMT_W, 0xC00D, 0xFFFE000000000000ull, false}, /* -32767 */
        {FMT_W, 0x400D, 0xFFFE000000000000ull, false}, /* 32767 */
        {FMT_W, 0x400E, 0x8000000000000000ull, true},  /* 32768 */
        {FMT_L, 0xC01E, 0x8000000000000000ull, true},  /* -2^31 */
        {FMT_L, 0xC01D, 0xFFFFFFFC00000000ull, false}, /* -2^31 + 2 */
        {FMT_L, 0x401D, 0xFFFFFFF800000000ull, false}, /* 2^31 - 4 */
        {FMT_B, 0x4005, 0xFE00000000000000ull, false}, /* 127 */
        {FMT_B, 0x4006, 0x8000000000000000ull, true},  /* 128 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = fp(cases[i].se, cases[i].sig);
        cpu->d[0] = 0x55555555u;
        const uint32_t end = RUN(&c, cpu, 0xF200, EXT_OUT(cases[i].fmt, 0));
        if (cases[i].operr) {
            expect_post(&c, cpu, VEC_OPERR, RESET_PC + 4, 0);
            assert(cpu->d[0] == 0x55555555u);
        } else {
            assert(cpu->pc == end && cpu->d[0] != 0x55555555u);
        }
        mc68k_destroy(cpu);
    }
}

/* 命令の開始で EXC byte を消す（FPCC/AEXC は残す）。 */
static void test_exc_cleared_at_start(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    cpu = make(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ONE;
    fst(cpu)->fpsr_exc = 0xFF & ~EXC_BSUN;
    RUN(&c, cpu, 0xF200, 0x0422);
    assert(fst(cpu)->fpsr_exc == 0);
    mc68k_destroy(cpu);

    cpu = make(&c);
    fst(cpu)->fp[0] = FIVE;
    fst(cpu)->fpsr_exc = 0xFF & ~EXC_BSUN;
    RUN(&c, cpu, 0xF200, EXT_OUT(FMT_L, 0));
    assert(fst(cpu)->fpsr_exc == 0 && cpu->d[0] == 5);
    mc68k_destroy(cpu);

    cpu = make(&c);
    fst(cpu)->fpsr_exc = 0xFF & ~EXC_BSUN;
    RUN(&c, cpu, 0xF280, 0x0000); /* FNOP */
    assert(fst(cpu)->fpsr_exc == 0);
    mc68k_destroy(cpu);

    /* FMOVE FPcr / FMOVEM は EXC を変えない。 */
    cpu = make(&c);
    fst(cpu)->fpsr_exc = EXC_DZ;
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF200, 0xB000, 0xF210, 0xF080);
    assert(fst(cpu)->fpsr_exc == EXC_DZ);
    mc68k_destroy(cpu);
}

/* メモリ転送が失敗したら (An)+/-(An) の更新を戻す。 */
static void test_fault_restores_address_register(void) {
    static const struct {
        uint16_t opcode, ext;
    } cases[] = {
        {0xF218, EXT_IN(FMT_X, 1, 0x22)}, /* FADD.X (A0)+,FP1 */
        {0xF220, EXT_IN(FMT_X, 1, 0x22)}, /* FADD.X -(A0),FP1 */
        {0xF218, EXT_OUT(FMT_X, 0)},      /* FMOVE.X FP0,(A0)+ */
        {0xF220, EXT_OUT(FMT_X, 0)},      /* FMOVE.X FP0,-(A0) */
        {0xF218, 0x9C00},                 /* FMOVE.L (A0)+,FPCR/FPSR/FPIAR */
        {0xF220, 0xBC00},                 /* FMOVE.L FPCR/FPSR/FPIAR,-(A0) */
        {0xF218, 0xD080},                 /* FMOVEM.X (A0)+,FP0 */
        {0xF220, 0xE001},                 /* FMOVEM.X FP0,-(A0) */
        {0xF258, 0x0001},                 /* FScc (A0)+ */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        const uint32_t bad = RAM_SIZE + 0x100; /* バスが応答しない */
        cpu->a[0] = bad;
        cpu->fpu040->state.fpsr_cc = MC68K_FPU_CC_Z;
        RUN(&c, cpu, cases[i].opcode, cases[i].ext);
        assert(cpu->a[0] == bad);
        mc68k_destroy(cpu);
    }
}

/* 保留 → 報告 → handler で RTE → 次に報告した命令から再開（format $0 の往復）。 */
static void test_round_trip(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    ww(&c, HANDLER(VEC_DZ), 0xF310);     /* FSAVE (A0) */
    ww(&c, HANDLER(VEC_DZ) + 2, 0x4E73); /* RTE */
    cpu->a[0] = DATA_AREA;
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    const uint32_t end = RUN(&c, cpu, 0xF200, 0x0420, 0xF280, 0x0000, 0x7005);
    /* FNOP は報告後に再実行され、MOVEQ #5,D0 まで到達する。 */
    assert(cpu->pc == end && cpu->d[0] == 5 && cpu->a[7] == RESET_SP);
    mc68k_destroy(cpu);

    /* format $3 の往復: FMOVE.L FP0,D0 の OPERR → RTE → 次命令から。 */
    cpu = make(&c);
    ww(&c, HANDLER(VEC_OPERR), 0xF310); /* FSAVE (A0) */
    ww(&c, HANDLER(VEC_OPERR) + 2, 0x4E73);
    cpu->a[0] = DATA_AREA;
    fst(cpu)->fp[0] = fp(0x4027, 0x8000000000000000ull);
    const uint32_t end2 = RUN(&c, cpu, 0xF200, EXT_OUT(FMT_L, 0), 0x7007);
    assert(cpu->pc == end2 && cpu->d[0] == 7 && cpu->a[7] == RESET_SP);
    mc68k_destroy(cpu);
}

/* FMOVE <ea>,FPcr / FPcr,<ea> の単一レジスタは -(An) / (An)+ も使える（PRM FMOVE System Control
 * Register）。 */
static void test_control_single_register_modes(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    const uint32_t value = 0x00003000u | 0x80u | 0x20u;

    wl(&c, DATA_AREA, value);
    cpu->a[1] = DATA_AREA + 4;
    RUN(&c, cpu, 0xF221, 0x9000); /* FMOVE.L -(A1),FPCR */
    assert(cpu->a[1] == DATA_AREA && fst(cpu)->fpcr_enable == (EXC_OVFL | EXC_OPERR));
    mc68k_destroy(cpu);

    cpu = make(&c);
    fst(cpu)->fpcr_enable = EXC_OVFL | EXC_OPERR;
    fst(cpu)->fpcr_round_prec = MC68K_FPU_PREC_DOUBLE;
    fst(cpu)->fpcr_round_mode = MC68K_FPU_ROUND_MINUS_INF;
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF219, 0xB000); /* FMOVE.L FPCR,(A1)+ */
    assert(cpu->a[1] == DATA_AREA + 4 && rl(&c, DATA_AREA) == value);
    mc68k_destroy(cpu);
}

/* FMOVE.P FP0,(A1) の P{Dn}（形式 7）は 040 では未対応データ型。メモリは書かない。 */
static void test_packed_dynamic_destination(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fst(cpu)->fp[0] = TWO;
    cpu->a[1] = DATA_AREA;
    wl(&c, DATA_AREA, 0x11223344u);
    RUN(&c, cpu, 0xF211, 0x7C10);
    expect_post(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4, DATA_AREA);
    assert(rl(&c, DATA_AREA) == 0x11223344u && rl(&c, DATA_AREA + 4) == 0);
    mc68k_destroy(cpu);
}

/* 未対応データ型は次の FP 命令で報告する。handler が FSAVE して RTE
 * すれば、同じ命令は再実行されない。 */
static void test_unsupported_is_not_reexecuted(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    const uint32_t src = DATA_AREA + 0x100;

    ww(&c, HANDLER(VEC_UNSUPPORTED), 0xF310);     /* FSAVE (A0) */
    ww(&c, HANDLER(VEC_UNSUPPORTED) + 2, 0x4E73); /* RTE */
    wx(&c, src, fp(0x0000, 0x0000000000000001ull));
    cpu->a[0] = DATA_AREA;
    cpu->a[1] = src;
    fst(cpu)->fp[0] = ONE;
    const uint32_t end = RUN(&c, cpu, 0xF219, EXT_IN(FMT_X, 0, 0x22), 0xF280, 0x0000, 0x7005);
    assert(cpu->pc == end && cpu->d[0] == 5 && cpu->a[7] == RESET_SP);
    assert(cpu->a[1] == src + 12 && same(fst(cpu)->fp[0], ONE)); /* An は 1 回だけ進む */
    mc68k_destroy(cpu);
}

/* T1 のトレースは、実行後（format $3）の FP 例外と FTRAPcc では報告しない（[040UM
 * §8.3]）。通常の FP 命令では報告する。 */
static void test_trace_suppression(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);

    cpu->sr = RESET_SR | 0x8000u;
    fst(cpu)->fp[0] = fp(0x4027, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_OUT(FMT_L, 0));
    assert(cpu->pc == HANDLER(VEC_OPERR) && cpu->a[7] == RESET_SP - 12);
    assert(rw(&c, cpu->a[7]) == (uint16_t)(RESET_SR | 0x8000u) &&
           rl(&c, cpu->a[7] + 2) == RESET_PC + 4);
    assert(cpu->last_exception_vector == VEC_OPERR);
    mc68k_destroy(cpu);

    cpu = make(&c);
    cpu->sr = RESET_SR | 0x8000u;
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_Z;
    RUN(&c, cpu, 0xF27A, 0x0001, 0x1234);
    assert(cpu->pc == HANDLER(VEC_TRAPCC) && cpu->a[7] == RESET_SP - 12);
    assert(cpu->last_exception_vector == VEC_TRAPCC);
    mc68k_destroy(cpu);

    cpu = make(&c);
    cpu->sr = RESET_SR | 0x8000u;
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = TWO;
    RUN(&c, cpu, 0xF200, 0x0422); /* FADD FP1,FP0 は完了してからトレース（vector 9） */
    assert(cpu->last_exception_vector == 9 && cpu->pc == HANDLER(9));
    mc68k_destroy(cpu);

    /* pre-instruction（format $0）の FP 例外は Table 8-4 の priority 2 で、trace（priority
     * 6）より先に処理され、 trace は命令の再実行のときに生じる（設計書 §5）。BSUN（FBcc
     * の評価前）と、保留中の DZ（次の FP 命令の開始時）。 */
    cpu = make(&c);
    cpu->sr = RESET_SR | 0x8000u;
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_NAN;
    fst(cpu)->fpcr_enable = EXC_BSUN;
    RUN(&c, cpu, 0xF292, 0x0100);
    assert(cpu->pc == HANDLER(VEC_BSUN) && cpu->a[7] == RESET_SP - 8);
    assert(cpu->last_exception_vector == VEC_BSUN);
    assert(rw(&c, cpu->a[7]) == (uint16_t)(RESET_SR | 0x8000u) &&
           rl(&c, cpu->a[7] + 2) == RESET_PC);
    mc68k_destroy(cpu);

    cpu = make(&c);
    cpu->sr = RESET_SR | 0x8000u;
    cpu->fpu040->exc.valid = true;
    cpu->fpu040->exc.vector = VEC_DZ;
    RUN(&c, cpu, 0xF280, 0x0000); /* FNOP */
    assert(cpu->pc == HANDLER(VEC_DZ) && cpu->a[7] == RESET_SP - 8);
    assert(cpu->last_exception_vector == VEC_DZ);
    assert(rw(&c, cpu->a[7]) == (uint16_t)(RESET_SR | 0x8000u) &&
           rl(&c, cpu->a[7] + 2) == RESET_PC);
    mc68k_destroy(cpu);

    /* format $2（未実装命令）と $4（FPU なし 040）は Table 8-4 の priority level 3
     * なので、同時のトレースは報告しない。 */
    cpu = make(&c);
    cpu->sr = RESET_SR | 0x8000u;
    fst(cpu)->fp[1] = ONE;
    RUN(&c, cpu, 0xF200, 0x050E); /* FSIN FP1,FP2 */
    assert(cpu->pc == HANDLER(VEC_FLINE) && cpu->a[7] == RESET_SP - 12);
    assert(cpu->last_exception_vector == VEC_FLINE);
    assert(rw(&c, cpu->a[7] + 6) == (uint16_t)(0x2000u | VEC_FLINE * 4));
    mc68k_destroy(cpu);

    cpu = make_fpu(&c, MC68K_FPU_NONE_040);
    cpu->sr = RESET_SR | 0x8000u;
    RUN(&c, cpu, 0xF200, 0x0422); /* FADD FP1,FP0 */
    assert(cpu->pc == HANDLER(VEC_FLINE) && cpu->a[7] == RESET_SP - 16);
    assert(cpu->last_exception_vector == VEC_FLINE);
    assert(rw(&c, cpu->a[7] + 6) == (uint16_t)(0x4000u | VEC_FLINE * 4));
    mc68k_destroy(cpu);
}

/* 未実装命令（format $2）は FPIAR・FPn・FPSR を変えない。 */
static void test_unimplemented_does_not_touch_state(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fst(cpu)->fpiar = 0x1234;
    fst(cpu)->fp[1] = ONE;
    RUN(&c, cpu, 0xF200, 0x050E); /* FSIN FP1,FP2: 未実装 */
    assert(cpu->pc == HANDLER(VEC_FLINE) && fst(cpu)->fpiar == 0x1234);
    assert(same(fst(cpu)->fp[2], fp(0x7FFF, 0xFFFFFFFFFFFFFFFFull)) && fst(cpu)->fpsr_exc == 0);
    mc68k_destroy(cpu);
}

int main(void) {
    test_register_arithmetic();
    test_compare_and_test();
    test_memory_and_register_sources();
    test_register_destinations();
    test_control_registers();
    test_fmovem();
    test_conditional();
    test_bsun();
    test_pending_dz();
    test_nonmaskable_overflow_underflow();
    test_exception_priority_exhaustive();
    test_exception_priority_instructions();
    test_inexact_writes_result();
    test_operr_snan_maskable();
    test_post_instruction_exceptions();
    test_unsupported_data_types();
    test_unsupported_boundaries();
    test_integer_destination_limits();
    test_exc_cleared_at_start();
    test_fault_restores_address_register();
    test_round_trip();
    test_unimplemented_does_not_touch_state();
    test_control_single_register_modes();
    test_packed_dynamic_destination();
    test_unsupported_is_not_reexecuted();
    test_trace_suppression();
    puts("test_fpu040_exec: OK");
    return 0;
}
