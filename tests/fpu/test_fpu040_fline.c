/*
 * MC68040 内蔵 FPU の F-line 入口: 現行仕様。
 * 命令の分類（不正 / 未実装 / 実装済み）、format $0/$2/$4 のフレーム、RTE の受理を、
 * RAM 上の生バイナリで検証する。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu/cpu_internal.h"
#include "mc68000/cpu.h"

enum {
    RAM_SIZE = 0x10000,
    RESET_SP = 0x8000,
    RESET_PC = 0x1000,
    FLINE_HANDLER = 0x400,
    FORMAT_HANDLER = 0x440,
    DATA_AREA = 0x3000,
    VEC_FORMAT_ERROR = 14,
    VEC_FLINE = 11,
    RESET_SR = 0x2700,
};

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

static mc68k_state_t *make(ctx_t *c, mc68k_fpu_config_t fpu) {
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
    wl(c, VEC_FLINE * 4, FLINE_HANDLER);
    wl(c, VEC_FORMAT_ERROR * 4, FORMAT_HANDLER);
    ww(c, FLINE_HANDLER, 0x60fe); /* BRA.S * */
    ww(c, FORMAT_HANDLER, 0x60fe);
    mc68k_state_t *cpu = mc68k_create_model_with_fpu(&b, MC68K_MODEL_68040, fpu);
    assert(cpu);
    mc68k_reset(cpu);
    return cpu;
}

/* 命令語列を RESET_PC に置き、ハンドラへ到達するまで実行する。 */
static void run(ctx_t *c, mc68k_state_t *cpu, const uint16_t *words, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        ww(c, RESET_PC + 2 * i, words[i]);
    mc68k_execute(cpu, 100);
}

#define RUN(c, cpu, ...)                                 \
    do {                                                 \
        const uint16_t w_[] = {__VA_ARGS__};             \
        run((c), (cpu), w_, sizeof(w_) / sizeof(w_[0])); \
    } while (0)

static void expect_frame(const ctx_t *c, const mc68k_state_t *cpu, unsigned format, uint32_t pc,
                         uint32_t ea, uint32_t faulted_pc) {
    const uint32_t bytes = format == 0 ? 8 : format == 2 ? 12 : 16;
    const uint32_t sp = RESET_SP - bytes;
    assert(cpu->a[7] == sp);
    assert(cpu->pc == FLINE_HANDLER);
    assert(rw(c, sp) == RESET_SR);
    assert(rl(c, sp + 2) == pc);
    assert(rw(c, sp + 6) == (uint16_t)(format << 12 | VEC_FLINE * 4));
    if (format != 0)
        assert(rl(c, sp + 8) == ea);
    if (format == 4)
        assert(rl(c, sp + 12) == faulted_pc);
}

/* ---- 分類 ---- */

static bool in_list(const uint8_t *list, unsigned n, unsigned v) {
    for (unsigned i = 0; i < n; i++)
        if (list[i] == v)
            return true;
    return false;
}

static void test_classify_opmodes(void) {
    /* 実装済み: 共通 FPU 演算 + 040 の強制精度（設計書 §3、opmode は推論）。 */
    static const uint8_t impl[] = {0x00, 0x04, 0x18, 0x1A, 0x20, 0x22, 0x23, 0x24, 0x27, 0x28,
                                   0x38, 0x3A, 0x40, 0x41, 0x44, 0x45, 0x58, 0x5A, 0x5C, 0x5E,
                                   0x60, 0x62, 0x63, 0x64, 0x66, 0x67, 0x68, 0x6C};
    /* 未実装: [040UM Table 9-10] + FLOG2（Table E-2）。FSINCOS は 0x30-0x37。 */
    static const uint8_t unimpl[] = {0x01, 0x02, 0x03, 0x06, 0x08, 0x09, 0x0A, 0x0C, 0x0D,
                                     0x0E, 0x0F, 0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x19,
                                     0x1C, 0x1D, 0x1E, 0x1F, 0x21, 0x25, 0x26, 0x30, 0x31,
                                     0x32, 0x33, 0x34, 0x35, 0x36, 0x37};
    unsigned n_impl = 0, n_unimpl = 0, n_illegal = 0;

    for (unsigned op = 0; op < 128; op++) {
        const fpu040_class_t expect = in_list(impl, sizeof(impl), op)       ? FPU040_IMPL
                                      : in_list(unimpl, sizeof(unimpl), op) ? FPU040_UNIMPL
                                                                            : FPU040_ILLEGAL;
        /* reg-reg（opclass 000）と memory-to-reg の Dn.X ではなく (A0)（opclass 010、.L） */
        assert(fpu040_classify(0xF200, (uint16_t)(0x0400 | op)) == expect);
        assert(fpu040_classify(0xF210, (uint16_t)(0x4000 | op)) == expect);
        n_impl += expect == FPU040_IMPL;
        n_unimpl += expect == FPU040_UNIMPL;
        n_illegal += expect == FPU040_ILLEGAL;
    }
    assert(n_impl == 28 && n_unimpl == 33 && n_illegal == 67);
}

static void test_classify_forms(void) {
    /* FMOVECR（opclass 010、source 7）は未実装。EA 欄が 0 以外なら不正。 */
    assert(fpu040_classify(0xF200, 0x5C00) == FPU040_UNIMPL);
    assert(fpu040_classify(0xF210, 0x5C00) == FPU040_ILLEGAL);
    /* opclass 000 は EA 欄 0 が必須、opclass 001 は存在しない。 */
    assert(fpu040_classify(0xF208, 0x0522) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF200, 0x2522) == FPU040_ILLEGAL);
    /* memory-to-reg: An は不可、Dn は B/W/L/S のみ。 */
    assert(fpu040_classify(0xF208, 0x4022) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF200, 0x4022) == FPU040_IMPL);    /* FADD.L D0,FP0 */
    assert(fpu040_classify(0xF200, 0x4822) == FPU040_ILLEGAL); /* .X */
    assert(fpu040_classify(0xF200, 0x4C22) == FPU040_ILLEGAL); /* .P */
    assert(fpu040_classify(0xF200, 0x5422) == FPU040_ILLEGAL); /* .D */
    assert(fpu040_classify(0xF200, 0x5822) == FPU040_IMPL);    /* .B */
    /* reg-to-memory は data alterable。(d16,PC) と即値は不可。 */
    assert(fpu040_classify(0xF210, 0x6800) == FPU040_IMPL);
    assert(fpu040_classify(0xF23A, 0x6800) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF23C, 0x6800) == FPU040_ILLEGAL);
    /* type 6/7 は不正。FBcc/FSAVE/FRESTORE/FDBcc/FTRAPcc は分類上は実装済み。 */
    assert(fpu040_classify(0xF380, 0) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF3C0, 0) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF281, 0) == FPU040_IMPL);
    assert(fpu040_classify(0xF2C1, 0) == FPU040_IMPL);
    assert(fpu040_classify(0xF248, 0) == FPU040_IMPL);
    assert(fpu040_classify(0xF27A, 0) == FPU040_IMPL);
    assert(fpu040_classify(0xF310, 0) == FPU040_IMPL);
    assert(fpu040_classify(0xF318, 0) == FPU040_ILLEGAL); /* FSAVE (An)+ は不可 */
    assert(fpu040_classify(0xF320, 0) == FPU040_IMPL);    /* FSAVE -(An) */
    assert(fpu040_classify(0xF358, 0) == FPU040_IMPL);    /* FRESTORE (An)+ */
    assert(fpu040_classify(0xF360, 0) == FPU040_ILLEGAL); /* FRESTORE -(An) は不可 */

    /* opclass 100/101（FMOVE <ea>,FPcr / FPcr,<ea>）: 単一レジスタなら -(An) / (An)+ も可。
     * 複数レジスタなら不可。Dn は 1 本まで、An は FPIAR のみ。 */
    assert(fpu040_classify(0xF221, 0x9000) == FPU040_IMPL);    /* FMOVE.L -(A1),FPCR */
    assert(fpu040_classify(0xF221, 0x8400) == FPU040_IMPL);    /* ... FPIAR */
    assert(fpu040_classify(0xF221, 0x9800) == FPU040_ILLEGAL); /* FPCR+FPSR */
    assert(fpu040_classify(0xF221, 0x9C00) == FPU040_ILLEGAL); /* 3 本 */
    assert(fpu040_classify(0xF219, 0x9000) == FPU040_IMPL);    /* (A1)+,FPCR: 常に可 */
    assert(fpu040_classify(0xF219, 0xB000) == FPU040_IMPL);    /* FMOVE.L FPCR,(A1)+ */
    assert(fpu040_classify(0xF219, 0xA400) == FPU040_IMPL);    /* FPIAR 単独 */
    assert(fpu040_classify(0xF219, 0xB400) == FPU040_ILLEGAL); /* FPCR+FPIAR */
    assert(fpu040_classify(0xF219, 0xB800) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF219, 0xBC00) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF221, 0xB000) == FPU040_IMPL);    /* FPcr,-(An) は常に可 */
    assert(fpu040_classify(0xF200, 0x9800) == FPU040_ILLEGAL); /* Dn に 2 本 */
    assert(fpu040_classify(0xF208, 0x8400) == FPU040_IMPL);    /* An は FPIAR のみ */
    assert(fpu040_classify(0xF208, 0x9000) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF23C, 0x9C00) == FPU040_IMPL);    /* 即値 */
    assert(fpu040_classify(0xF23C, 0xBC00) == FPU040_ILLEGAL); /* 即値へは書けない */

    /* TYPE_COND の EA 欄: FTRAPcc は 072〜074、FScc は data alterable（絶対ロングは可、075
     * は不可）。 */
    assert(fpu040_classify(0xF279, 0x0001) == FPU040_IMPL); /* FScc abs.L */
    assert(fpu040_classify(0xF27D, 0x0001) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF27A, 0x0001) == FPU040_IMPL); /* FTRAPcc.W */
    assert(fpu040_classify(0xF27C, 0x0001) == FPU040_IMPL); /* FTRAPcc 無引数 */

    /* FMOVEM（opclass 111）: -(An) は predecrement 形式（static 00 / dynamic 01）だけ。 */
    assert(fpu040_classify(0xF221, 0xE001) == FPU040_IMPL);
    assert(fpu040_classify(0xF221, 0xE801) == FPU040_IMPL);
    assert(fpu040_classify(0xF221, 0xF001) == FPU040_ILLEGAL);
    assert(fpu040_classify(0xF221, 0xF801) == FPU040_ILLEGAL);
}

/* ---- 68040 内蔵 FPU: 未実装命令は format $2 ---- */

static void test_unimplemented_format2(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* FSIN FP1,FP2: EA を持たないので 0。PC は次命令。 */
    cpu = make(&c, MC68K_FPU_68040);
    RUN(&c, cpu, 0xF200, 0x050E);
    expect_frame(&c, cpu, 2, RESET_PC + 4, 0, 0);
    mc68k_destroy(cpu);

    /* FMOVECR #0,FP0 */
    cpu = make(&c, MC68K_FPU_68040);
    RUN(&c, cpu, 0xF200, 0x5C00);
    expect_frame(&c, cpu, 2, RESET_PC + 4, 0, 0);
    mc68k_destroy(cpu);

    /* FSIN.X (A0)+,FP2: EA は更新前、A0 は +12。 */
    cpu = make(&c, MC68K_FPU_68040);
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF218, 0x490E);
    expect_frame(&c, cpu, 2, RESET_PC + 4, DATA_AREA, 0);
    assert(cpu->a[0] == DATA_AREA + 12);
    mc68k_destroy(cpu);

    /* FSIN.X -(A0),FP0: A0 は -12、EA は更新後。 */
    cpu = make(&c, MC68K_FPU_68040);
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF220, 0x480E);
    expect_frame(&c, cpu, 2, RESET_PC + 4, DATA_AREA - 12, 0);
    assert(cpu->a[0] == DATA_AREA - 12);
    mc68k_destroy(cpu);

    /* FSIN.B (A7)+,FP0: A7 の byte 更新は 2。フレームは更新後の SP の下に積まれるので、
     * 更新前の A7（RESET_SP）が EA。 */
    cpu = make(&c, MC68K_FPU_68040);
    RUN(&c, cpu, 0xF21F, 0x580E);
    assert(cpu->pc == FLINE_HANDLER);
    assert(rl(&c, cpu->a[7] + 8) == RESET_SP);
    assert(cpu->a[7] == RESET_SP + 2 - 12);
    mc68k_destroy(cpu);

    /* FSIN.X (d16,A0),FP0 */
    cpu = make(&c, MC68K_FPU_68040);
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF228, 0x480E, 0x0123);
    expect_frame(&c, cpu, 2, RESET_PC + 6, DATA_AREA + 0x123, 0);
    mc68k_destroy(cpu);

    /* FSIN.X #imm,FP0: 即値 12 バイトを読み飛ばし、EA は 0。 */
    cpu = make(&c, MC68K_FPU_68040);
    RUN(&c, cpu, 0xF23C, 0x480E, 0, 0, 0, 0, 0, 0);
    expect_frame(&c, cpu, 2, RESET_PC + 4 + 12, 0, 0);
    mc68k_destroy(cpu);

    /* FSIN.D #imm,FP0: 8 バイト。FSIN.B #imm: 1 語。 */
    cpu = make(&c, MC68K_FPU_68040);
    RUN(&c, cpu, 0xF23C, 0x540E, 0, 0, 0, 0);
    expect_frame(&c, cpu, 2, RESET_PC + 4 + 8, 0, 0);
    mc68k_destroy(cpu);
    cpu = make(&c, MC68K_FPU_68040);
    RUN(&c, cpu, 0xF23C, 0x580E, 0);
    expect_frame(&c, cpu, 2, RESET_PC + 4 + 2, 0, 0);
    mc68k_destroy(cpu);

    /* FSIN.L D0,FP0（Dn）: EA 0。 */
    cpu = make(&c, MC68K_FPU_68040);
    RUN(&c, cpu, 0xF200, 0x400E);
    expect_frame(&c, cpu, 2, RESET_PC + 4, 0, 0);
    mc68k_destroy(cpu);
}

/* 不正な F-line は format $0（An も更新しない）。 */
static void test_illegal_format0(void) {
    static const uint16_t cases[][2] = {
        {0xF208, 0x050E}, /* opclass 000 の EA 欄が非 0 */
        {0xF200, 0x0505}, /* 未定義 opmode */
        {0xF200, 0x2522}, /* opclass 001 */
        {0xF380, 0x0000}, /* type 6 */
        {0xF3C0, 0x0000}, /* type 7 */
        {0xF208, 0x4022}, /* An をソースにできない */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c, MC68K_FPU_68040);
        cpu->a[0] = DATA_AREA;
        RUN(&c, cpu, cases[i][0], cases[i][1]);
        expect_frame(&c, cpu, 0, RESET_PC, 0, 0);
        assert(cpu->a[0] == DATA_AREA); /* 不正な命令は An を更新しない */
        mc68k_destroy(cpu);
    }
}

/* FPU を構成しない 040（NONE）では cpID 1 も通常の F-line。 */
static void test_generic_040_has_no_fpu(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c, MC68K_FPU_NONE);
    cpu->a[0] = DATA_AREA;
    RUN(&c, cpu, 0xF218, 0x490E);
    expect_frame(&c, cpu, 0, RESET_PC, 0, 0);
    assert(cpu->a[0] == DATA_AREA);
    mc68k_destroy(cpu);
}

/* ---- FPU 非搭載 040: 正当な FP 命令は format $4 ---- */

typedef struct {
    uint16_t w[6];
    unsigned n;
    uint32_t next; /* 次命令までの長さ */
    uint32_t ea;
    uint32_t a0_after;
} nofpu_case_t;

static void test_no_fpu_format4(void) {
    static const nofpu_case_t cases[] = {
        {{0xF200, 0x0522}, 2, 4, 0, DATA_AREA},                        /* FADD FP1,FP2 */
        {{0xF200, 0x050E}, 2, 4, 0, DATA_AREA},                        /* FSIN FP1,FP2 */
        {{0xF218, 0x4922}, 2, 4, DATA_AREA, DATA_AREA},                /* FADD.X (A0)+,FP2 */
        {{0xF220, 0x4822}, 2, 4, DATA_AREA - 12, DATA_AREA},           /* FADD.X -(A0),FP0 */
        {{0xF228, 0x4822, 0x0010}, 3, 6, DATA_AREA + 0x10, DATA_AREA}, /* (d16,A0) */
        {{0xF23C, 0x4022, 0, 0}, 4, 8, 0, DATA_AREA},                  /* FADD.L #imm,FP0 */
        {{0xF200, 0xB000}, 2, 4, 0, DATA_AREA},                        /* FMOVE.L FPCR,D0 */
        {{0xF220, 0xE003}, 2, 4, DATA_AREA - 24, DATA_AREA},           /* FMOVEM FP0-1,-(A0) */
        {{0xF218, 0xD003}, 2, 4, DATA_AREA, DATA_AREA},         /* FMOVEM.X (A0)+,FP0-1 */
        {{0xF281, 0x0010}, 2, 4, 0, DATA_AREA},                 /* FBF.W */
        {{0xF2C1, 0x0000, 0x0010}, 3, 6, 0, DATA_AREA},         /* FBcc.L */
        {{0xF248, 0x0001, 0x0010}, 3, 6, 0, DATA_AREA},         /* FDBcc D0 */
        {{0xF27A, 0x0001, 0x1234}, 3, 6, 0, DATA_AREA},         /* FTRAPcc.W */
        {{0xF27B, 0x0001, 0x1234, 0x5678}, 4, 8, 0, DATA_AREA}, /* FTRAPcc.L */
        {{0xF27C, 0x0001}, 2, 4, 0, DATA_AREA},                 /* FTRAPcc */
        {{0xF240, 0x0001}, 2, 4, 0, DATA_AREA},                 /* FScc D0 */
        {{0xF258, 0x0001}, 2, 4, DATA_AREA, DATA_AREA},         /* FScc (A0)+ */
        {{0xF310}, 1, 2, DATA_AREA, DATA_AREA},                 /* FSAVE (A0) */
        {{0xF350}, 1, 2, DATA_AREA, DATA_AREA},                 /* FRESTORE (A0) */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c, MC68K_FPU_NONE_040);
        cpu->a[0] = DATA_AREA;
        run(&c, cpu, cases[i].w, cases[i].n);
        assert(cpu->a[0] == cases[i].a0_after);
        expect_frame(&c, cpu, 4, RESET_PC + cases[i].next, cases[i].ea, RESET_PC);
        mc68k_destroy(cpu);
    }

    /* 不正な形式は NONE_040 でも format $0。 */
    ctx_t c;
    mc68k_state_t *cpu = make(&c, MC68K_FPU_NONE_040);
    RUN(&c, cpu, 0xF200, 0x0505);
    expect_frame(&c, cpu, 0, RESET_PC, 0, 0);
    mc68k_destroy(cpu);
}

/* ---- RTE ---- */

static void push_frame(ctx_t *c, mc68k_state_t *cpu, unsigned format, uint32_t bytes,
                       uint32_t pc) {
    cpu->a[7] = RESET_SP - bytes;
    ww(c, cpu->a[7], 0x2004);
    wl(c, cpu->a[7] + 2, pc);
    ww(c, cpu->a[7] + 6, (uint16_t)(format << 12 | VEC_FLINE * 4));
    ww(c, pc, 0x60fe); /* 復帰先で BRA.S * */
}

static void test_rte(void) {
    static const struct {
        unsigned format, bytes;
    } valid[] = {{2, 12}, {3, 12}, {0, 8}};
    ctx_t c;
    mc68k_state_t *cpu;

    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        cpu = make(&c, MC68K_FPU_68040);
        push_frame(&c, cpu, valid[i].format, valid[i].bytes, 0x2000);
        RUN(&c, cpu, 0x4E73);
        assert(cpu->pc == 0x2000 && cpu->a[7] == RESET_SP && cpu->sr == 0x2004);
        mc68k_destroy(cpu);
    }
    /* format $4 は NONE_040 だけが読む（16 バイト）。 */
    cpu = make(&c, MC68K_FPU_NONE_040);
    push_frame(&c, cpu, 4, 16, 0x2000);
    RUN(&c, cpu, 0x4E73);
    assert(cpu->pc == 0x2000 && cpu->a[7] == RESET_SP && cpu->sr == 0x2004);
    mc68k_destroy(cpu);

    cpu = make(&c, MC68K_FPU_68040);
    push_frame(&c, cpu, 4, 16, 0x2000);
    RUN(&c, cpu, 0x4E73);
    assert(cpu->pc == FORMAT_HANDLER && cpu->last_exception_vector == VEC_FORMAT_ERROR);
    mc68k_destroy(cpu);

    cpu = make(&c, MC68K_FPU_NONE);
    push_frame(&c, cpu, 4, 16, 0x2000);
    RUN(&c, cpu, 0x4E73);
    assert(cpu->pc == FORMAT_HANDLER && cpu->last_exception_vector == VEC_FORMAT_ERROR);
    mc68k_destroy(cpu);
}

/* 例外 → handler で RTE → 次命令から再開（format $2/$4 の往復）。 */
static void test_round_trip(void) {
    static const mc68k_fpu_config_t cfg[] = {MC68K_FPU_68040, MC68K_FPU_NONE_040};
    for (unsigned i = 0; i < 2; i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c, cfg[i]);
        ww(&c, FLINE_HANDLER, 0x4E73); /* RTE */
        ww(&c, RESET_PC + 4, 0x7005);  /* MOVEQ #5,D0 */
        ww(&c, RESET_PC + 6, 0x60fe);
        RUN(&c, cpu, 0xF200, i == 0 ? 0x050E : 0x0522);
        assert(cpu->d[0] == 5);
        assert(cpu->a[7] == RESET_SP);
        mc68k_destroy(cpu);
    }
}

int main(void) {
    test_classify_opmodes();
    test_classify_forms();
    test_unimplemented_format2();
    test_illegal_format0();
    test_generic_040_has_no_fpu();
    test_no_fpu_format4();
    test_rte();
    test_round_trip();
    puts("test_fpu040_fline: OK");
    return 0;
}
