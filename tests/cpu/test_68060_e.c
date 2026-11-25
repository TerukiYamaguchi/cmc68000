/*
 * MC68060-E (#111) T-E1〜T-E6。設計書 docs/mc68000-internals/reference/cpu_68060.md §8、§8.1、§8.2、§10.1。
 * FPU 構成 `MC68K_FPU_68060` の F-line（cpID 1）の分類、FSAVE/FRESTORE の 3 long word frame、
 * vector 11 の 3 種別（$2/$4/$0）、vector 60、特権、trace 抑止を固定する。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE         0x80000u
#define SSP_INIT         0x8000u
#define USP_INIT         0x7000u
#define RESET_PC         0x100u
#define TEST_PC          0x400u
#define OPERAND          0x6000u /* メモリオペランドの置き場 */
#define HANDLER          0x3000u
#define SR_SUP           0x2700
#define SR_USER          0x0000
#define SR_TRACE         0xa700 /* T1 + S */
#define NO_VECTOR        0xffffu
#define RTE_OPCODE       0x4e73
#define VEC_FORMAT_ERROR 14
#define VEC_PRIV         8
#define VEC_FPU          11
#define VEC_BSUN         48
#define VEC_DZ           50
#define VEC_UNSUPP       55
#define VEC_UNIMPL_EA    60
#define PCR_DFP          0x2u
#define FPIAR_SENTINEL   0xdeadbee0u
#define PATTERN          0xa5a5a5a5u

#define FMT_0 0x0
#define FMT_2 0x2
#define FMT_3 0x3
#define FMT_4 0x4

static uint8_t ram[RAM_SIZE];
static mc68k_state_t *cpu_g;

static bool rb(void *c, uint32_t a, uint8_t *v) {
    (void)c;
    if (a >= RAM_SIZE)
        return false;
    *v = ram[a];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    (void)c;
    if (a + 2 > RAM_SIZE)
        return false;
    *v = (uint16_t)(ram[a] << 8 | ram[a + 1]);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    (void)c;
    if (a + 4 > RAM_SIZE)
        return false;
    *v = (uint32_t)ram[a] << 24 | (uint32_t)ram[a + 1] << 16 | (uint32_t)ram[a + 2] << 8 |
         ram[a + 3];
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    (void)c;
    if (a >= RAM_SIZE)
        return false;
    ram[a] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    (void)c;
    if (a + 2 > RAM_SIZE)
        return false;
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    (void)c;
    if (a + 4 > RAM_SIZE)
        return false;
    ram[a] = (uint8_t)(v >> 24);
    ram[a + 1] = (uint8_t)(v >> 16);
    ram[a + 2] = (uint8_t)(v >> 8);
    ram[a + 3] = (uint8_t)v;
    return true;
}
static void reset_peripheral(void *c) {
    (void)c;
}

static void h_word(uint32_t a, uint16_t v) {
    if (!ww(NULL, a, v))
        abort();
}
static void h_long(uint32_t a, uint32_t v) {
    if (!wl(NULL, a, v))
        abort();
}
static uint16_t ram_w(uint32_t a) {
    uint16_t v;
    if (!rw(NULL, a, &v))
        abort();
    return v;
}
static uint32_t ram_l(uint32_t a) {
    uint32_t v;
    if (!rl(NULL, a, &v))
        abort();
    return v;
}

static uint32_t handler(int vec) {
    return HANDLER + (uint32_t)vec * 8u;
}

static mc68k_fpu040_t *fpu(void) {
    return cpu_g->fpu040;
}

static void init_state(void) {
    memset(ram, 0, sizeof ram);
    h_long(0, SSP_INIT);
    h_long(4, RESET_PC);
    for (int v = 2; v < 256; ++v) {
        h_long((uint32_t)v * 4u, handler(v));
        h_word(handler(v), RTE_OPCODE);
    }
    mc68k_reset(cpu_g);
    cpu_g->usp = USP_INIT;
    mc68k_set_irq(cpu_g, 0);
    cpu_g->irq_defer = false;
    cpu_g->last_exception_vector = NO_VECTOR;
    cpu_g->pcr = 0;
    fpu()->state.fpiar = FPIAR_SENTINEL;
}

static void place(uint16_t sr, const uint16_t *w, int n) {
    for (int i = 0; i < n; ++i)
        h_word(TEST_PC + (uint32_t)i * 2u, w[i]);
    cpu_g->a[7] = SSP_INIT;
    cpu_g->pc = TEST_PC;
    cpu_set_sr(cpu_g, sr);
    cpu_g->last_exception_vector = NO_VECTOR;
}
#define PLACE(sr, ...)                                  \
    do {                                                \
        const uint16_t w_[] = {__VA_ARGS__};            \
        place(sr, w_, (int)(sizeof w_ / sizeof w_[0])); \
    } while (0)
#define STEP() mc68k_execute(cpu_g, 1)
#define RUN(sr, ...)            \
    do {                        \
        PLACE(sr, __VA_ARGS__); \
        STEP();                 \
    } while (0)

static mc68k_fp80_t fp(uint16_t se, uint64_t sig) {
    mc68k_fp80_t v = {se, sig};
    return v;
}
static mc68k_fp80_t fp_1_5(void) {
    return fp(0x3fff, 0xc000000000000000ull);
}
static mc68k_fp80_t fp_2_5(void) {
    return fp(0x4000, 0xa000000000000000ull);
}
static mc68k_fp80_t fp_2_9(void) {
    return fp(0x4000, 0xb99999999999999aull);
}
static mc68k_fp80_t fp_2(void) {
    return fp(0x4000, 0x8000000000000000ull);
}
static mc68k_fp80_t fp_5(void) {
    return fp(0x4001, 0xa000000000000000ull);
}
static bool fp_eq(mc68k_fp80_t a, mc68k_fp80_t b) {
    return a.sign_exp == b.sign_exp && a.signif == b.signif;
}

/* 例外 frame の検査。base は frame の先頭（= 例外後の SSP）。 */
static void expect_frame(uint32_t base, int fmt, int vec, uint32_t pc, uint16_t sr) {
    assert(cpu_g->last_exception_vector == vec);
    assert(cpu_g->pc == handler(vec));
    assert(cpu_g->a[7] == base);
    assert(ram_w(base) == sr);
    assert(ram_l(base + 2) == pc);
    assert(ram_w(base + 6) == (uint16_t)((unsigned)fmt << 12 | (unsigned)vec * 4u));
}
static uint32_t frame_bytes(int fmt) {
    return fmt == FMT_0 ? 8 : fmt == FMT_4 ? 16 : 12;
}
#define FRAME_BASE(fmt) (SSP_INIT - frame_bytes(fmt))

/* ---- T-E1: 構成値と F-line ---- */
static void t_e1(void) {
    assert(cpu_has_060_fpu(cpu_g));
    assert(cpu_g->fpu_config == MC68K_FPU_68060 && fpu() != NULL);

    /* FPU なしの 060（NONE）は汎用 CPU。cpID 1 も通常の F-line（vector 11 / $0、PC = 命令）。
     */
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral};
    mc68k_state_t *none = mc68k_create_model(&bus, MC68K_MODEL_68060);
    assert(none && !cpu_has_060_fpu(none) && none->fpu040 == NULL);
    mc68k_state_t *saved = cpu_g;
    cpu_g = none;
    static const uint16_t fp_insns[][2] = {
        {0xf200, 0x0422}, {0xf200, 0x040e}, {0xf280, 0x0000}, {0xf310, 0}};
    for (unsigned i = 0; i < sizeof fp_insns / sizeof fp_insns[0]; ++i) {
        memset(ram, 0, sizeof ram);
        h_long(0, SSP_INIT);
        h_long(4, RESET_PC);
        h_long(VEC_FPU * 4u, handler(VEC_FPU));
        h_word(handler(VEC_FPU), RTE_OPCODE);
        mc68k_reset(cpu_g);
        PLACE(SR_SUP, fp_insns[i][0], fp_insns[i][1]);
        STEP();
        expect_frame(FRAME_BASE(FMT_0), FMT_0, VEC_FPU, TEST_PC, SR_SUP);
    }
    cpu_g = saved;
    mc68k_destroy(none);
}

/* ---- T-E2: FSAVE / FRESTORE ---- */
static void fill_operand(void) {
    for (uint32_t i = 0; i < 9; ++i)
        h_long(OPERAND - 12 + i * 4, PATTERN);
}

static void t_e2(void) {
    /* NULL（reset 直後）。3 long 全部を書く（NULL も 12 byte） */
    init_state();
    fill_operand();
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf310); /* FSAVE (A0) */
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    assert(ram_l(OPERAND) == 0 && ram_l(OPERAND + 4) == 0 && ram_l(OPERAND + 8) == 0);
    assert(ram_l(OPERAND + 12) == PATTERN && ram_l(OPERAND - 4) == PATTERN);

    /* IDLE: FP 命令を実行した後。format は long 0 の bits 15-8 */
    init_state();
    fill_operand();
    fpu()->state.fp[1] = fp_1_5();
    RUN(SR_SUP, 0xf200, 0x0500); /* FMOVE FP1,FP2 */
    assert(fpu()->idle);
    cpu_g->a[0] = OPERAND + 12;
    RUN(SR_SUP, 0xf320); /* FSAVE -(A0) */
    assert(cpu_g->a[0] == OPERAND);
    assert(ram_l(OPERAND) == 0x00006000u && ram_l(OPERAND + 4) == 0 && ram_l(OPERAND + 8) == 0);
    assert(ram_l(OPERAND + 12) == PATTERN && ram_l(OPERAND - 4) == PATTERN);

    /* EXCP: 保留中の FP 例外（DZ）がある状態。FSAVE が例外状態を消す */
    init_state();
    fill_operand();
    fpu()->state.fpcr_enable = MC68K_FPU_EXC_DZ;
    fpu()->state.fp[0] = fp_1_5();
    fpu()->state.fp[1] = fp(0, 0);
    RUN(SR_SUP, 0xf200, 0x0420); /* FDIV FP1,FP0 */
    assert(fpu()->exc.valid && fpu()->exc.vector == VEC_DZ);
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf310);
    assert(ram_l(OPERAND) == 0x0000e002u && ram_l(OPERAND + 4) == 0 &&
           ram_l(OPERAND + 8) == 0); /* V2-V0 = DZ */
    assert(!fpu()->exc.valid);

    /* FRESTORE: IDLE は idle へ、(A0)+ は 12 byte 進む */
    init_state();
    h_long(OPERAND, 0x00006000u);
    h_long(OPERAND + 4, PATTERN);
    h_long(OPERAND + 8, PATTERN);
    fpu()->state.fp[3] = fp_5();
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf358); /* FRESTORE (A0)+ */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->a[0] == OPERAND + 12);
    assert(fpu()->idle && !fpu()->exc.valid &&
           fp_eq(fpu()->state.fp[3], fp_5())); /* IDLE は exception 状態にしない */

    /* FRESTORE: EXCP は FPU を exception 状態へ戻し、V2-V0 と exception operand を保持する。
     * 次の FSAVE が同じ frame を書き、状態を消す（[060UM §6.7]） */
    for (unsigned v = 0; v < 8; ++v) {
        init_state();
        h_long(OPERAND, 0x4001e000u | v); /* operand 指数 $4001・EXCP・V */
        h_long(OPERAND + 4, 0xa5a5a5a5u);
        h_long(OPERAND + 8, 0x5a5a5a5au);
        cpu_g->a[0] = OPERAND;
        RUN(SR_SUP, 0xf358); /* FRESTORE (A0)+ */
        assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->a[0] == OPERAND + 12);
        assert(fpu()->idle && fpu()->exc.valid);
        h_long(OPERAND + 16, 0xffffffffu);
        h_long(OPERAND + 20, 0xffffffffu);
        h_long(OPERAND + 24, 0xffffffffu);
        cpu_g->a[0] = OPERAND + 16;
        RUN(SR_SUP, 0xf310 | 0); /* FSAVE (A0) */
        assert(ram_l(OPERAND + 16) == (0x4001e000u | v));
        assert(ram_l(OPERAND + 20) == 0xa5a5a5a5u && ram_l(OPERAND + 24) == 0x5a5a5a5au);
        assert(!fpu()->exc.valid);
    }

    /* FRESTORE: NULL は reset（FPn は NaN） */
    fpu()->state.fp[3] = fp_5();
    fpu()->idle = true;
    h_long(OPERAND, 0);
    h_long(OPERAND + 4, 0);
    h_long(OPERAND + 8, 0);
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf350); /* FRESTORE (A0) */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->a[0] == OPERAND);
    assert(!fpu()->idle && fpu()->state.fp[3].sign_exp == 0x7fff);

    /* format（long 0 の bits 15-8）が 00/60/E0 以外は vector 14。bits 31-24 の 60 は format
     * ではない */
    static const uint32_t bad[] = {0x00000100u, 0x00004100u, 0x00006100u,
                                   0x0000e100u, 0x0000f000u, 0x00001000u};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        init_state();
        h_long(OPERAND, bad[i]);
        fpu()->state.fp[3] = fp_5();
        fpu()->idle = false;
        cpu_g->a[0] = OPERAND;
        RUN(SR_SUP, 0xf358);
        expect_frame(FRAME_BASE(FMT_0), FMT_0, VEC_FORMAT_ERROR, TEST_PC, SR_SUP);
        assert(cpu_g->a[0] == OPERAND && !fpu()->idle && fp_eq(fpu()->state.fp[3], fp_5()));
    }
    init_state();
    h_long(OPERAND, 0x60000000u); /* bits 15-8 = 00: NULL として reset */
    fpu()->state.fp[3] = fp_5();
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf350);
    assert(cpu_g->last_exception_vector == NO_VECTOR && fpu()->state.fp[3].sign_exp == 0x7fff);
}

/* ---- T-E6: user の FSAVE / FRESTORE は privilege violation ---- */
static void t_e6(void) {
    /* FSAVE -(A0): 書込先に FSAVE が書かない値を置く。FPU は idle（FSAVE すれば NULL/IDLE
     * が書かれる） */
    init_state();
    fill_operand();
    fpu()->state.fp[1] = fp_1_5();
    RUN(SR_SUP, 0xf200, 0x0500);
    assert(fpu()->idle);
    cpu_g->a[0] = OPERAND + 12;
    RUN(SR_USER, 0xf320);
    expect_frame(SSP_INIT - 8, FMT_0, VEC_PRIV, TEST_PC, SR_USER);
    assert(cpu_g->a[0] == OPERAND + 12);
    for (uint32_t i = 0; i < 9; ++i)
        assert(ram_l(OPERAND - 12 + i * 4) == PATTERN);
    assert(fpu()->idle && fp_eq(fpu()->state.fp[2], fp_1_5()));

    /* FSAVE (A0) の副作用なし版（-(A0) の An 更新は上で確認済み）。EXCP 状態も消さない */
    init_state();
    fill_operand();
    fpu()->state.fpcr_enable = MC68K_FPU_EXC_DZ;
    fpu()->state.fp[0] = fp_1_5();
    fpu()->state.fp[1] = fp(0, 0);
    RUN(SR_SUP, 0xf200, 0x0420);
    assert(fpu()->exc.valid);
    cpu_g->a[0] = OPERAND;
    RUN(SR_USER, 0xf310);
    expect_frame(SSP_INIT - 8, FMT_0, VEC_PRIV, TEST_PC, SR_USER);
    assert(fpu()->exc.valid && ram_l(OPERAND) == PATTERN);

    /* FRESTORE (A0)+: FPU は FPn・FPCR/FPSR/FPIAR が非 0 の状態。NULL frame を読ませる */
    init_state();
    h_long(OPERAND, 0);
    h_long(OPERAND + 4, 0);
    h_long(OPERAND + 8, 0);
    fpu()->state.fp[3] = fp_5();
    fpu()->state.fpcr_enable = MC68K_FPU_EXC_DZ;
    fpu()->state.fpsr_exc = MC68K_FPU_EXC_INEX2;
    fpu()->idle = true;
    cpu_g->a[0] = OPERAND;
    RUN(SR_USER, 0xf358);
    expect_frame(SSP_INIT - 8, FMT_0, VEC_PRIV, TEST_PC, SR_USER);
    assert(cpu_g->a[0] == OPERAND && fpu()->idle && fp_eq(fpu()->state.fp[3], fp_5()));
    assert(fpu()->state.fpcr_enable == MC68K_FPU_EXC_DZ &&
           fpu()->state.fpsr_exc == MC68K_FPU_EXC_INEX2);
    assert(fpu()->state.fpiar == FPIAR_SENTINEL);
    RUN(SR_SUP, 0xf350); /* 同じ命令は supervisor では実行される */
    assert(cpu_g->last_exception_vector == NO_VECTOR && !fpu()->idle);
}

/* ---- T-E3: trace 中の FP 例外は trace frame を出さない ---- */
static void expect_no_trace(int vec) {
    assert(cpu_g->last_exception_vector == vec); /* trace(9) が後から入っていない */
    assert(cpu_g->pc == handler(vec));
}

static void t_e3(void) {
    /* FP Unimplemented（11 / $2） */
    init_state();
    fpu()->state.fp[1] = fp_1_5();
    RUN(SR_TRACE, 0xf200, 0x040e); /* FSIN FP1,FP0 */
    expect_no_trace(VEC_FPU);

    /* BSUN（48）: FPSR の NAN を立て、BSUN を許可して FBGT */
    init_state();
    RUN(SR_SUP, 0xf23c, 0x8800, 0x0100, 0x0000); /* FMOVE.L #$01000000,FPSR */
    RUN(SR_SUP, 0xf23c, 0x9000, 0x0000, 0x8000); /* FMOVE.L #$8000,FPCR */
    assert(fpu()->state.fpcr_enable == 0x80);
    RUN(SR_TRACE, 0xf292, 0x0010); /* FBGT .+$12 */
    expect_no_trace(VEC_BSUN);

    /* 算術例外（DZ = 50）: 保留中の例外を次の FP 命令（FNOP）が pre-instruction で報告する */
    init_state();
    fpu()->state.fpcr_enable = MC68K_FPU_EXC_DZ;
    fpu()->state.fp[0] = fp_1_5();
    fpu()->state.fp[1] = fp(0, 0);
    RUN(SR_SUP, 0xf200, 0x0420);   /* FDIV FP1,FP0 */
    RUN(SR_TRACE, 0xf280, 0x0000); /* FNOP */
    expect_no_trace(VEC_DZ);

    /* Unsupported Data Type（55）: FMOVE.P FPn,<ea> */
    init_state();
    fpu()->state.fp[0] = fp_1_5();
    cpu_g->a[0] = OPERAND;
    PLACE(SR_TRACE, 0xf210, 0x6c00); /* FMOVE.P FP0,(A0){#0} */
    STEP();
    expect_no_trace(VEC_UNSUPP);
}

/* ---- T-E4: 未実装 EA（vector 60） ---- */
static void expect_ea_trap(uint16_t sr, bool fp_prefix_tracing) {
    (void)fp_prefix_tracing;
    expect_frame(SSP_INIT - 8, FMT_0, VEC_UNIMPL_EA, TEST_PC, sr);
    assert(fpu()->state.fpiar == FPIAR_SENTINEL); /* FPIAR 不変 */
}

static void t_e4(void) {
    struct {
        const char *name;
        uint16_t w[8];
        int n;
    } traps[] = {
        {"FADD.X #imm", {0xf23c, 0x4822, 0, 0, 0, 0, 0, 0}, 8},
        {"FADD.P #imm", {0xf23c, 0x4c22, 0, 0, 0, 0, 0, 0}, 8},
        {"FMOVE.X #imm", {0xf23c, 0x4800, 0, 0, 0, 0, 0, 0}, 8},
        {"FSIN.X #imm (Table 6-11 より優先)", {0xf23c, 0x480e, 0, 0, 0, 0, 0, 0}, 8},
        {"FMOVEM.L #imm,FPCR/FPSR", {0xf23c, 0x9800, 0, 0, 0, 0, 0, 0}, 6},
        {"FMOVEM.L #imm,FPCR/FPIAR", {0xf23c, 0x9400, 0, 0, 0, 0, 0, 0}, 6},
        {"FMOVEM.L #imm,FPSR/FPIAR", {0xf23c, 0x8c00, 0, 0, 0, 0, 0, 0}, 6},
        {"FMOVEM.L #imm,FPCR/FPSR/FPIAR", {0xf23c, 0x9c00, 0, 0, 0, 0, 0, 0}, 8},
        {"FMOVEM.X D0,(A0)", {0xf210, 0xf800, 0, 0, 0, 0, 0, 0}, 2},
        {"FMOVEM.X D0,-(A0)", {0xf220, 0xe800, 0, 0, 0, 0, 0, 0}, 2},
        {"FMOVEM.X (A0),D0", {0xf210, 0xd800, 0, 0, 0, 0, 0, 0}, 2},
        {"FMOVEM.X (A0)+,D0", {0xf218, 0xd800, 0, 0, 0, 0, 0, 0}, 2},
    };
    for (unsigned i = 0; i < sizeof traps / sizeof traps[0]; ++i) {
        for (int tracing = 0; tracing <= 1; ++tracing) {
            init_state();
            cpu_g->a[0] = OPERAND;
            place(tracing ? SR_TRACE : SR_SUP, traps[i].w, traps[i].n);
            STEP();
            expect_ea_trap(tracing ? SR_TRACE : SR_SUP, tracing);
            assert(cpu_g->a[0] == OPERAND); /* An 不変 */
        }
    }

    /* 境界: 直接実行 */
    init_state();
    RUN(SR_SUP, 0xf23c, 0x4000, 0x0000, 0x0005); /* FMOVE.L #5,FP0 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 8);
    assert(fp_eq(fpu()->state.fp[0], fp_5()));
    init_state();
    RUN(SR_SUP, 0xf23c, 0x5000, 0x0005); /* FMOVE.W #5,FP0 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 6);
    assert(fp_eq(fpu()->state.fp[0], fp_5()));
    init_state();
    RUN(SR_SUP, 0xf23c, 0x5800, 0x0005); /* FMOVE.B #5,FP0 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 6);
    assert(fp_eq(fpu()->state.fp[0], fp_5()));
    init_state();
    RUN(SR_SUP, 0xf23c, 0x4400, 0x40a0, 0x0000); /* FMOVE.S #5.0,FP0 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 8);
    assert(fp_eq(fpu()->state.fp[0], fp_5()));
    init_state();
    RUN(SR_SUP, 0xf23c, 0x5400, 0x4014, 0, 0); /* FMOVE.D #5.0,FP0 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 12);
    assert(fp_eq(fpu()->state.fp[0], fp_5()));
    static const struct {
        uint16_t ext;
        uint32_t enable;
    } single[] = {
        {0x9000, 0x10}, /* FPCR（enable = bits 15-8） */
    };
    for (unsigned i = 0; i < sizeof single / sizeof single[0]; ++i) {
        init_state();
        RUN(SR_SUP, 0xf23c, single[i].ext, 0x0000, 0x1000);
        assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 8);
        assert(fpu()->state.fpcr_enable == single[i].enable);
    }
    init_state();
    RUN(SR_SUP, 0xf23c, 0x8400, 0x1234, 0x5678); /* FMOVE.L #imm,FPIAR */
    assert(cpu_g->last_exception_vector == NO_VECTOR && fpu()->state.fpiar == 0x12345678u);
    init_state();
    RUN(SR_SUP, 0xf23c, 0x8800, 0x0000, 0x0000); /* FMOVE.L #imm,FPSR */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 8);
    /* メモリの X は直接実行、静的リストの FMOVEM.X も直接実行 */
    init_state();
    h_long(OPERAND, 0x40010000u);
    h_long(OPERAND + 4, 0xa0000000u);
    h_long(OPERAND + 8, 0);
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf210, 0x4800); /* FMOVE.X (A0),FP0 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && fp_eq(fpu()->state.fp[0], fp_5()));
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf210, 0xd080); /* FMOVEM.X (A0),FP0（静的リスト） */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 4);
}

/* ---- T-E5: 未実装 FP 命令（$2）、FP 無効（$4）、F-line（$0） ---- */
static void expect_unimpl(uint32_t pc_next, uint32_t ea) {
    expect_frame(FRAME_BASE(FMT_2), FMT_2, VEC_FPU, pc_next, SR_SUP);
    assert(ram_l(FRAME_BASE(FMT_2) + 8) == ea);
    assert(fpu()->state.fpiar == TEST_PC); /* FPIAR = FP 命令のアドレス */
}

static void t_e5_unimpl(void) {
    static const uint16_t ops[] = {
        0x1c, 0x0c, 0x0a, 0x0d, 0x1d, 0x19, 0x10, 0x08, 0x1e, 0x1f,
        0x15, 0x16, 0x14, 0x06, 0x0e, 0x02, 0x0f, 0x09, 0x12, 0x11, /* 超越関数 */
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,             /* FSINCOS */
        0x21, 0x25, 0x26,                                           /* FMOD FREM FSCALE */
    };
    unsigned count = 0;
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; ++i) {
        init_state();
        fpu()->state.fp[0] = fp_2();
        fpu()->state.fp[1] = fp_1_5();
        RUN(SR_SUP, 0xf200, (uint16_t)(0x0400 | ops[i])); /* F<op> FP1,FP0 */
        expect_unimpl(TEST_PC + 4, 0);
        count++;
    }
    assert(count == 31);
    /* FMOVECR（ROM offset が FINT・FINTRZ の opmode と同じ値でも未実装） */
    for (unsigned dst = 0; dst < 8; ++dst)
        for (unsigned off = 0; off < 0x80; ++off) {
            init_state();
            RUN(SR_SUP, 0xf200, (uint16_t)(0x5c00 | (dst << 7) | off));
            expect_unimpl(TEST_PC + 4, 0);
        }
    /* メモリオペランド: EA は SP+8 */
    static const struct {
        uint16_t w[4];
        int n;
        uint32_t next;
    } mem[] = {
        {{0xf210, 0x480e}, 2, TEST_PC + 4},                  /* FSIN.X (A0),FP0 */
        {{0xf228, 0x480e, 0x0010}, 3, TEST_PC + 6},          /* FSIN.X (16,A0),FP0 */
        {{0xf239, 0x480e, 0x0000, OPERAND}, 4, TEST_PC + 8}, /* FSIN.X (abs.L),FP0 */
    };
    static const uint32_t mem_ea[] = {OPERAND, OPERAND + 0x10, OPERAND};
    for (unsigned i = 0; i < 3; ++i) {
        init_state();
        cpu_g->a[0] = OPERAND;
        place(SR_SUP, mem[i].w, mem[i].n);
        STEP();
        expect_unimpl(mem[i].next, mem_ea[i]);
        assert(cpu_g->a[0] == OPERAND);
    }
    /* FDBcc / FScc / FTRAPcc は 060 で未実装 */
    init_state();
    RUN(SR_SUP, 0xf248, 0x000f, 0x0010); /* FDBT D0,.+ */
    expect_unimpl(TEST_PC + 6, 0);
    init_state();
    RUN(SR_SUP, 0xf240, 0x000f); /* FST D0 */
    expect_unimpl(TEST_PC + 4, 0);
    init_state();
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf250, 0x000f); /* FST (A0) */
    expect_unimpl(TEST_PC + 4, OPERAND);
    init_state();
    RUN(SR_SUP, 0xf27c, 0x000f); /* FTRAPT */
    expect_unimpl(TEST_PC + 4, 0);
    init_state();
    RUN(SR_SUP, 0xf27a, 0x000f, 0x1234); /* FTRAPT.W #imm */
    expect_unimpl(TEST_PC + 6, 0);
    init_state();
    RUN(SR_SUP, 0xf27b, 0x000f, 0x1234, 0x5678); /* FTRAPT.L #imm */
    expect_unimpl(TEST_PC + 8, 0);
    /* trace 中でも trace frame が出ない */
    init_state();
    RUN(SR_TRACE, 0xf200, 0x041c);
    assert(cpu_g->last_exception_vector == VEC_FPU && cpu_g->pc == handler(VEC_FPU));
    /* FBcc は直接実行 */
    init_state();
    RUN(SR_SUP, 0xf281, 0x0010); /* FBEQ（Z = 0 なので不成立） */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 4);
}

static void t_e5_int(void) {
    /* FINT（FPCR の丸め = 最近接偶数）と FINTRZ は 060 では直接実行 */
    init_state();
    fpu()->state.fp[1] = fp_2_5();
    RUN(SR_SUP, 0xf200, 0x0401); /* FINT FP1,FP0 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 4);
    assert(fp_eq(fpu()->state.fp[0], fp_2()));
    init_state();
    fpu()->state.fp[1] = fp_2_9();
    RUN(SR_SUP, 0xf200, 0x0403); /* FINTRZ FP1,FP0 */
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    assert(fp_eq(fpu()->state.fp[0], fp_2()));
    init_state();
    h_long(OPERAND, 0x40000000u | 0);
    h_long(OPERAND + 4, 0xa0000000u);
    h_long(OPERAND + 8, 0);
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf210, 0x4803); /* FINTRZ.X (A0),FP0 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && fp_eq(fpu()->state.fp[0], fp_2()));
}

static void expect_disabled(uint32_t pc_next, uint32_t ea) {
    expect_frame(FRAME_BASE(FMT_4), FMT_4, VEC_FPU, pc_next, SR_SUP);
    assert(ram_l(FRAME_BASE(FMT_4) + 8) == ea);
    assert(ram_l(FRAME_BASE(FMT_4) + 12) == TEST_PC); /* FP 命令の PC */
    assert(fpu()->state.fpiar == FPIAR_SENTINEL);     /* FP 無効では FPIAR を更新しない */
}

static void t_e5_disabled(void) {
    /* DFP=1: Table 6-11 の命令も、実装済みの命令も $4。未実装の判定を DFP より先にしない */
    static const uint16_t regs[][2] = {{0xf200, 0x040e},
                                       {0xf200, 0x0422},
                                       {0xf200, 0x0401},
                                       {0xf200, 0x5c00},
                                       {0xf280, 0x0000}};
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; ++i) {
        init_state();
        cpu_g->pcr = PCR_DFP;
        RUN(SR_SUP, regs[i][0], regs[i][1]);
        expect_disabled(
            TEST_PC + 4,
            0); /* レジスタオペランド・オペランドなしの EA は規定なし → 0 は実装値 */
    }
    /* メモリオペランド: (An)・(d16,An)・abs.L は EA、(An)+ は命令前の An（An 不変） */
    init_state();
    cpu_g->pcr = PCR_DFP;
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf210, 0x480e);
    expect_disabled(TEST_PC + 4, OPERAND);
    init_state();
    cpu_g->pcr = PCR_DFP;
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf228, 0x480e, 0x0010);
    expect_disabled(TEST_PC + 6, OPERAND + 0x10);
    init_state();
    cpu_g->pcr = PCR_DFP;
    RUN(SR_SUP, 0xf239, 0x480e, 0x0000, OPERAND);
    expect_disabled(TEST_PC + 8, OPERAND);
    init_state();
    cpu_g->pcr = PCR_DFP;
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf218, 0x480e); /* FSIN.X (A0)+,FP0 */
    expect_disabled(TEST_PC + 4, OPERAND);
    assert(cpu_g->a[0] == OPERAND);
    /* -(An): 拡張精度は An-4、S/L/D は An-size。An は更新しない */
    static const struct {
        uint16_t ext;
        uint32_t ea;
    } pre[] = {
        {0x480e, OPERAND - 4}, /* .X */
        {0x440e, OPERAND - 4}, /* .S */
        {0x400e, OPERAND - 4}, /* .L */
        {0x540e, OPERAND - 8}, /* .D */
    };
    for (unsigned i = 0; i < sizeof pre / sizeof pre[0]; ++i) {
        init_state();
        cpu_g->pcr = PCR_DFP;
        cpu_g->a[0] = OPERAND;
        RUN(SR_SUP, 0xf220, pre[i].ext);
        expect_disabled(TEST_PC + 4, pre[i].ea);
        assert(cpu_g->a[0] == OPERAND);
    }
    /* DFP=0 なら同じ命令は $2 */
    init_state();
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf210, 0x480e);
    expect_unimpl(TEST_PC + 4, OPERAND);
    /* DFP=1 で trace 中でも trace frame は出ない */
    init_state();
    cpu_g->pcr = PCR_DFP;
    RUN(SR_TRACE, 0xf200, 0x040e);
    assert(cpu_g->last_exception_vector == VEC_FPU && cpu_g->pc == handler(VEC_FPU));
}

static void t_e5_fline(void) {
    /* cpID 1 で FPU が認識しないパターンは vector 11 / $0、PC = F-line 語、DFP に依らない */
    static const uint16_t bad[][2] = {
        {0xf380, 0}, {0xf3ff, 0}, {0xf200, 0x2000}, {0xf200, 0x0450}, {0xf200, 0x0400 | 0x7f}};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        for (unsigned dfp = 0; dfp <= 1; ++dfp) {
            init_state();
            cpu_g->pcr = dfp ? PCR_DFP : 0;
            RUN(SR_SUP, bad[i][0], bad[i][1]);
            expect_frame(FRAME_BASE(FMT_0), FMT_0, VEC_FPU, TEST_PC, SR_SUP);
        }
    }
}

/* ---- T-E7: 変異試験で見つかった検出漏れの補強 ---- */
static void expect_bus_error(void) {
    assert(cpu_g->last_exception_vector == 2);
    assert(cpu_g->pc == handler(2));
    assert(cpu_g->a[7] == SSP_INIT - 16);
    assert(ram_w(SSP_INIT - 16 + 6) == 0x4008);
}
/* RAM の末尾に命令を置き、後続の語（拡張語・変位・即値）の fetch を失敗させる */
static void run_at_end(unsigned words_before_end, const uint16_t *w, int n) {
    const uint32_t pc = RAM_SIZE - words_before_end * 2u;
    for (int i = 0; i < n && pc + (uint32_t)i * 2u < RAM_SIZE; ++i)
        h_word(pc + (uint32_t)i * 2u, w[i]);
    cpu_g->a[7] = SSP_INIT;
    cpu_g->pc = pc;
    cpu_set_sr(cpu_g, SR_SUP);
    cpu_g->last_exception_vector = NO_VECTOR;
    STEP();
}

static void t_e7_branch_and_control(void) {
    /* FBcc は 060 でも直接実行する。成立（FBT）と不成立（FBF）、W と L */
    init_state();
    RUN(SR_SUP, 0xf28f, 0x0010); /* FBT.W .+$12 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 2 + 0x10);
    init_state();
    RUN(SR_SUP, 0xf2cf, 0x0000, 0x0020); /* FBT.L .+$22 */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 2 + 0x20);
    init_state();
    RUN(SR_SUP, 0xf280, 0x0010); /* FBF.W */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 4);
    init_state();
    RUN(SR_SUP, 0xf2c0, 0x0000, 0x0020); /* FBF.L */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 6);

    /* 制御レジスタの FMOVEM.L をメモリから: 2 本・3 本でも未実装 EA にならない（即値だけが
     * vector 60） */
    init_state();
    h_long(OPERAND, 0x00001000u);
    h_long(OPERAND + 4, 0x08000000u);
    h_long(OPERAND + 8, 0x12345678u);
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf210, 0x9800); /* FMOVEM.L (A0),FPCR/FPSR */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 4);
    assert(fpu()->state.fpcr_enable == 0x10);
    init_state();
    h_long(OPERAND, 0x00001000u);
    h_long(OPERAND + 4, 0x08000000u);
    h_long(OPERAND + 8, 0x12345678u);
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf210, 0x9c00); /* FMOVEM.L (A0),FPCR/FPSR/FPIAR */
    assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 4);
    assert(fpu()->state.fpcr_enable == 0x10 && fpu()->state.fpiar == 0x12345678u);

    /* 静的リストの FMOVEM は、予約ビット（bits 10-8）と list のどのビットが立っても直接実行 */
    for (unsigned b = 0; b < 11; ++b) {
        if (b >= 8)
            continue; /* bits 10-8 は予約。1 を指定した場合の動作は規定が無く固定しない */
        init_state();
        h_long(OPERAND, 0x40010000u);
        h_long(OPERAND + 4, 0xa0000000u);
        h_long(OPERAND + 8, 0);
        cpu_g->a[0] = OPERAND;
        RUN(SR_SUP, 0xf210, (uint16_t)(0xd000u | (1u << b))); /* FMOVEM.X (A0),<list> 静的 */
        assert(cpu_g->last_exception_vector == NO_VECTOR && cpu_g->pc == TEST_PC + 4);
    }
    /* 動的リスト（bit 11）だけが未実装 EA。静的リストは list のビットに依らず実行する */
    static const uint16_t classes[] = {0xd000, 0xf000}; /* mem→FPn の (An)、FPn→mem の (An) */
    for (unsigned c = 0; c < 2; ++c) {
        for (unsigned dyn = 0; dyn < 2; ++dyn) {
            init_state();
            cpu_g->a[0] = OPERAND;
            RUN(SR_SUP, 0xf210, (uint16_t)(classes[c] | (dyn ? 0x0800u : 0u) | 0x80u));
            if (dyn)
                expect_frame(SSP_INIT - 8, FMT_0, VEC_UNIMPL_EA, TEST_PC, SR_SUP);
            else
                assert(cpu_g->last_exception_vector == NO_VECTOR);
        }
    }
}

static void t_e7_disabled_ea(void) {
    /* DFP=1 の FSAVE / FRESTORE / FBcc。SP+8 は FSAVE/FRESTORE のメモリ EA、FBcc は 0 */
    init_state();
    cpu_g->pcr = PCR_DFP;
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf310); /* FSAVE (A0) */
    expect_disabled(TEST_PC + 2, OPERAND);
    init_state();
    cpu_g->pcr = PCR_DFP;
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf350); /* FRESTORE (A0) */
    expect_disabled(TEST_PC + 2, OPERAND);
    /* FRESTORE の PC 相対（FTRAPcc の EA 形式と同じ mode 7 の reg 2・3）も EA を計算する */
    init_state();
    cpu_g->pcr = PCR_DFP;
    RUN(SR_SUP, 0xf37a, 0x0100); /* FRESTORE (d16,PC) */
    expect_disabled(TEST_PC + 4, TEST_PC + 2 + 0x100);
    init_state();
    cpu_g->pcr = PCR_DFP;
    RUN(SR_SUP, 0xf37b, 0x0010); /* FRESTORE (d8,PC,D0.W) */
    expect_disabled(TEST_PC + 4, TEST_PC + 2 + 0x10);
    for (unsigned cond = 0; cond < 0x20; ++cond) {
        init_state();
        cpu_g->pcr = PCR_DFP;
        cpu_g->a[0] = OPERAND;
        cpu_g->a[1] = OPERAND;
        RUN(SR_SUP, (uint16_t)(0xf280 | cond), 0x0010); /* FBcc.W */
        expect_disabled(TEST_PC + 4, 0);
    }
    /* -(An) の拡張精度は An-4（register to EA でも）。他の形式は An-size */
    static const struct {
        uint16_t ext;
        uint32_t sub;
    } rte[] = {{0x6800, 4}, {0x6400, 4}, {0x7400, 8}, {0x7000, 2}, {0x7800, 1}, {0x6c00, 12}};
    for (unsigned r = 0; r < 8; ++r) {
        if (r == 7)
            continue; /* A7 は frame の push 先なので別に確認する */
        for (unsigned i = 0; i < sizeof rte / sizeof rte[0]; ++i) {
            init_state();
            cpu_g->pcr = PCR_DFP;
            cpu_g->a[r] = OPERAND;
            RUN(SR_SUP, (uint16_t)(0xf220 | r), rte[i].ext); /* FMOVE.<fmt> FP0,-(An) */
            expect_disabled(TEST_PC + 4, OPERAND - rte[i].sub);
            assert(cpu_g->a[r] == OPERAND);
        }
        static const struct {
            uint16_t ext;
            uint32_t sub;
        } etr[] = {{0x480e, 4}, {0x500e, 2}, {0x580e, 1}, {0x4c0e, 12}};
        for (unsigned i = 0; i < sizeof etr / sizeof etr[0]; ++i) {
            init_state();
            cpu_g->pcr = PCR_DFP;
            cpu_g->a[r] = OPERAND;
            RUN(SR_SUP, (uint16_t)(0xf220 | r), etr[i].ext); /* FSIN.<fmt> -(An),FP0 */
            expect_disabled(TEST_PC + 4, OPERAND - etr[i].sub);
            assert(cpu_g->a[r] == OPERAND);
        }
    }
    /* FMOVE.L FPSR,-(A0) / FMOVEM.L FPSR/FPIAR,-(A0): X と同じ select 値でも An-size */
    init_state();
    cpu_g->pcr = PCR_DFP;
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf220, 0xa800);
    expect_disabled(TEST_PC + 4, OPERAND - 4);
    init_state();
    cpu_g->pcr = PCR_DFP;
    cpu_g->a[0] = OPERAND;
    RUN(SR_SUP, 0xf220, 0xac00);
    expect_disabled(TEST_PC + 4, OPERAND - 8);
}

static void t_e7_an_registers(void) {
    /* 未実装命令（$2）は An を更新しない。A0〜A7 のどの An でも */
    for (unsigned r = 0; r < 8; ++r) {
        for (unsigned mode = 3; mode <= 4; ++mode) {
            init_state();
            const uint32_t op = 0xf200u | (mode << 3) | r;
            uint32_t before = r == 7 ? SSP_INIT : OPERAND;
            cpu_g->a[r] = before;
            PLACE(SR_SUP, (uint16_t)op, 0x480e); /* FSIN.X (An)+ / -(An),FP0 */
            if (r == 7)
                cpu_g->a[7] = SSP_INIT;
            STEP();
            assert(cpu_g->last_exception_vector == VEC_FPU);
            const uint32_t sp_after = r == 7 ? FRAME_BASE(FMT_2) : before;
            assert(cpu_g->a[r] == sp_after);
            assert(ram_l(FRAME_BASE(FMT_2) + 8) == (mode == 3 ? before : before - 12));
            for (unsigned o = 0; o < 7; ++o)
                if (o != r)
                    assert(cpu_g->a[o] == 0);
        }
    }
    /* FRESTORE (An)+ は 12 byte 進める */
    for (unsigned r = 0; r < 8; ++r) {
        init_state();
        h_long(OPERAND, 0x00006000u);
        h_long(OPERAND + 4, 0);
        h_long(OPERAND + 8, 0);
        cpu_g->a[r] = OPERAND;
        PLACE(SR_SUP, (uint16_t)(0xf358 | r));
        if (r == 7)
            cpu_g->a[7] = OPERAND;
        STEP();
        assert(cpu_g->last_exception_vector == NO_VECTOR);
        assert(cpu_g->a[r] == OPERAND + 12);
        for (unsigned o = 0; o < 7; ++o)
            if (o != r)
                assert(cpu_g->a[o] == 0);
    }
}

static void t_e7_faults(void) {
    /* 拡張語の fetch fault（op_fpu060 の入口） */
    init_state();
    fpu()->state.fp[0] = fp(0xbfff, 0x8000000000000000ull); /* -1。実行されれば N が立つ */
    fpu()->state.fpsr_cc = 0;
    {
        const uint16_t w[] = {0xf200};
        run_at_end(1, w, 1);
    }
    expect_bus_error();
    assert(fpu()->state.fpsr_cc == 0 &&
           fp_eq(fpu()->state.fp[0], fp(0xbfff, 0x8000000000000000ull)));
    /* 未実装 FP 命令の EA 計算（変位）の fault */
    init_state();
    cpu_g->a[0] = OPERAND;
    {
        const uint16_t w[] = {0xf228, 0x480e};
        run_at_end(2, w, 2);
    }
    expect_bus_error();
    /* FTRAPcc.W の末尾の語（skip_trailing_words）の fault */
    init_state();
    {
        const uint16_t w[] = {0xf27a, 0x000f};
        run_at_end(2, w, 2);
    }
    expect_bus_error();
    /* FSAVE の EA 計算の fault: operand にも vector 表にも書かない */
    init_state();
    cpu_g->a[0] = OPERAND;
    {
        const uint16_t w[] = {0xf328};
        run_at_end(1, w, 1);
    }
    expect_bus_error();
    assert(ram_l(0) == SSP_INIT && ram_l(4) == RESET_PC);
    /* FSAVE の書込み fault: 保留中の FP 例外は消えない */
    init_state();
    fill_operand();
    fpu()->state.fpcr_enable = MC68K_FPU_EXC_DZ;
    fpu()->state.fp[0] = fp_1_5();
    fpu()->state.fp[1] = fp(0, 0);
    RUN(SR_SUP, 0xf200, 0x0420); /* FDIV FP1,FP0 → DZ を保留 */
    assert(fpu()->exc.valid);
    cpu_g->a[0] = RAM_SIZE - 4;
    RUN(SR_SUP, 0xf310); /* FSAVE (A0): 2 本目の long が書けない */
    expect_bus_error();
    assert(fpu()->exc.valid);
    /* FRESTORE の EA 計算の fault と読出し fault: FPU の状態は変わらない */
    init_state();
    cpu_g->a[0] = OPERAND;
    fpu()->state.fp[3] = fp_5();
    fpu()->idle = false;
    {
        const uint16_t w[] = {0xf368};
        run_at_end(1, w, 1);
    } /* FRESTORE (d16,A0) */
    expect_bus_error();
    assert(!fpu()->idle && fp_eq(fpu()->state.fp[3], fp_5()));
    init_state();
    fpu()->state.fp[3] = fp_5();
    fpu()->idle = false;
    cpu_g->a[0] = RAM_SIZE;
    RUN(SR_SUP, 0xf350); /* FRESTORE (A0): 読めない */
    expect_bus_error();
    assert(!fpu()->idle && fp_eq(fpu()->state.fp[3], fp_5()));
    /* scope = 00 の $F400 は CINV/CPUSH の illegal（FPU の命令として拡張語を取りに行かない） */
    init_state();
    {
        const uint16_t w[] = {0xf400};
        run_at_end(1, w, 1);
    }
    assert(cpu_g->last_exception_vector == 4);
}

/* ---- T-E8: FP の例外 frame（cpu_dispatch_fpu040_exception）も vector fetch・stacking で miss
 * に allocate せず、hit は通常どおり（[060UM §7.7.1、§7.7.3、§5.4.3]） ---- */
static void e8_cache(void) {
    cpu_g->mmu060_dtt0 = UINT32_C(0x00ffc020); /* 全領域 copyback cacheable */
    cpu_g->cacr = UINT32_C(0x80000000);        /* EDC */
}
static void t_e8_cache_no_allocate(void) {
    const uint32_t vaddr = VEC_FPU * 4u, base = FRAME_BASE(FMT_2);
    const uint32_t H2 = handler(VEC_FPU) + 0x100u;
    for (unsigned hit = 0; hit < 2; ++hit) {
        init_state();
        e8_cache();
        h_word(H2, 0x4e73);
        fpu()->state.fp[1] = fp_1_5();
        if (hit) {
            assert(cpu_read_long(cpu_g, vaddr) ==
                   handler(VEC_FPU)); /* line を載せてから RAM だけ書き換える */
            assert(cpu_read_long(cpu_g, base) == 0);
            h_long(vaddr, H2);
        }
        RUN(SR_SUP, 0xf200, 0x040e); /* FSIN FP1,FP0（未実装） */
        assert(cpu_g->last_exception_vector == VEC_FPU);
        if (hit) {
            assert(cpu_g->pc == handler(VEC_FPU)); /* vector は cache の値 */
            assert(ram_l(base) == 0);              /* stacking は line に書かれ RAM は不変 */
            assert(cpu_read_long(cpu_g, base) != 0);
        } else {
            expect_unimpl(TEST_PC + 4, 0); /* stacking は RAM へ出る */
            h_long(vaddr, H2);
            RUN(SR_SUP, 0xf200, 0x040e);
            assert(cpu_g->pc == H2); /* vector は allocate されていない */
        }
    }
}

int main(void) {
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral};
    cpu_g = mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68060, MC68K_FPU_68060);
    assert(cpu_g);
    t_e1();
    t_e2();
    t_e3();
    t_e4();
    t_e5_unimpl();
    t_e5_int();
    t_e5_disabled();
    t_e5_fline();
    t_e6();
    t_e7_branch_and_control();
    t_e7_disabled_ea();
    t_e7_an_registers();
    t_e7_faults();
    t_e8_cache_no_allocate();
    mc68k_destroy(cpu_g);
    return 0;
}
