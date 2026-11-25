/*
 * MC68040 内蔵 FPU の state frame と強制精度命令: 現行仕様
 * §6。 FSAVE/FRESTORE の null/idle/busy/unimplemented frame、例外状態の持越し、frame の各 long
 * を、 RAM 上の生バイナリで検証する。frame 内のビット位置は M68040FPSP の fpsp.h による（設計書
 * §6）。
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
/* busy frame の NMNEXC（`$38`）の nmn_unsup_bit（fpsp.h。設計書 推論 25）。未対応データ型の
 * frame が持つ。 */
#define NMN_UNSUP  0x02000000u

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

static mc68k_state_t *make(ctx_t *c) {
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
    mc68k_state_t *cpu = mc68k_create_model_with_fpu(&b, MC68K_MODEL_68040, MC68K_FPU_68040);
    assert(cpu);
    mc68k_reset(cpu);
    return cpu;
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

enum { SAVE_AREA = DATA_AREA + 0x100, VEC_FORMAT_ERROR = 14, VEC_PRIVILEGE = 8 };

static uint32_t at(const ctx_t *c, uint32_t off) {
    return rl(c, SAVE_AREA + off);
}

/* 本命令 + FSAVE (A0)。A0 = SAVE_AREA。 */
static mc68k_state_t *make_a0(ctx_t *c) {
    mc68k_state_t *cpu = make(c);
    cpu->a[0] = SAVE_AREA;
    return cpu;
}

/* ---- null / idle ---- */

static void test_null_and_idle(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    uint32_t end = RUN(&c, cpu, 0xF310);
    assert(cpu->pc == end && at(&c, 0) == 0 && cpu->a[0] == SAVE_AREA);
    mc68k_destroy(cpu);

    /* -(A0): 4 バイト下がる。 */
    cpu = make_a0(&c);
    wl(&c, SAVE_AREA - 4, 0xFFFFFFFFu);
    RUN(&c, cpu, 0xF320);
    assert(cpu->a[0] == SAVE_AREA - 4 && rl(&c, SAVE_AREA - 4) == 0);
    mc68k_destroy(cpu);

    /* 非条件 FP 命令を 1 つ実行すると idle。FMOVE FPcr/FMOVEM も含む。 */
    static const uint16_t first[][2] = {
        {0xF200, 0x0422},
        {0xF200, 0xB000},
        {0xF210, 0xF080},
        {0xF200, 0x6000},
    };
    for (unsigned i = 0; i < sizeof(first) / sizeof(first[0]); i++) {
        cpu = make_a0(&c);
        fst(cpu)->fp[0] = ONE;
        fst(cpu)->fp[1] = ONE;
        RUN(&c, cpu, first[i][0], first[i][1], 0xF310);
        assert(at(&c, 0) == 0x41000000u);
        mc68k_destroy(cpu);
    }

    /* 条件命令（FNOP/FBcc/FDBcc/FScc/FTRAPcc）だけなら null のまま。 */
    static const uint16_t cond[][3] = {
        {0xF280, 0x0000, 0}, {0xF281, 0x0004, 0}, {0xF248, 0x0001, 0x0004},
        {0xF240, 0x0001, 0}, {0xF27C, 0x0000, 0},
    };
    for (unsigned i = 0; i < sizeof(cond) / sizeof(cond[0]); i++) {
        cpu = make_a0(&c);
        const unsigned n = cond[i][0] == 0xF248                           ? 3
                           : cond[i][0] == 0xF281 || cond[i][0] == 0xF280 ? 2
                                                                          : 2;
        uint16_t prog[5] = {cond[i][0], cond[i][1], cond[i][2], 0, 0};
        prog[n] = 0xF310;
        run(&c, cpu, prog, n + 1);
        assert(at(&c, 0) == 0 && cpu->a[7] == RESET_SP);
        mc68k_destroy(cpu);
    }
}

static void test_privilege(void) {
    static const uint16_t op[] = {0xF310, 0xF350};
    for (unsigned i = 0; i < 2; i++) {
        ctx_t c;
        mc68k_state_t *cpu = make_a0(&c);
        cpu->sr = 0x0000; /* ユーザモード */
        cpu->a[7] = RESET_SP;
        cpu->usp = 0x7000;
        wl(&c, SAVE_AREA, 0x41000000u);
        RUN(&c, cpu, op[i]);
        assert(cpu->last_exception_vector == VEC_PRIVILEGE && cpu->a[0] == SAVE_AREA);
        assert(at(&c, 0) == 0x41000000u); /* FSAVE はメモリを書かない */
        mc68k_destroy(cpu);
    }
}

/* ---- FRESTORE ---- */

static void test_restore(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* null: FP レジスタ・FPCR/FPSR/FPIAR を reset、その後の FSAVE は null。 */
    cpu = make_a0(&c);
    fst(cpu)->fp[3] = ONE;
    fst(cpu)->fpiar = 0x1234;
    fst(cpu)->fpcr_enable = EXC_DZ;
    wl(&c, DATA_AREA, 0);
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF359, 0xF310);
    assert(cpu->a[1] == DATA_AREA + 4 && fst(cpu)->fpiar == 0 && fst(cpu)->fpcr_enable == 0);
    assert(same(fst(cpu)->fp[3], fp(0x7FFF, 0xFFFFFFFFFFFFFFFFull)));
    assert(at(&c, 0) == 0);
    mc68k_destroy(cpu);

    /* version 0 は size によらず null frame（4 バイト）として扱う。 */
    cpu = make_a0(&c);
    fst(cpu)->fp[3] = ONE;
    wl(&c, DATA_AREA, 0x00300000u);
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF359, 0xF310);
    assert(cpu->a[1] == DATA_AREA + 4 &&
           same(fst(cpu)->fp[3], fp(0x7FFF, 0xFFFFFFFFFFFFFFFFull)) && at(&c, 0) == 0);
    mc68k_destroy(cpu);

    /* idle: レジスタは変えず、FSAVE は idle。(An)+ は 4。 */
    cpu = make_a0(&c);
    fst(cpu)->fp[3] = ONE;
    wl(&c, DATA_AREA, 0x41000000u);
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF359, 0xF310);
    assert(cpu->a[1] == DATA_AREA + 4 && same(fst(cpu)->fp[3], ONE) &&
           at(&c, 0) == 0x41000000u);
    mc68k_destroy(cpu);

    /* unimplemented（$30）と busy（$60）: 全長を読み、idle に戻る。 */
    static const struct {
        uint32_t head;
        uint32_t bytes;
    } big[] = {
        {0x41300000u, 52},
        {0x41600000u, 100},
    };
    for (unsigned i = 0; i < 2; i++) {
        cpu = make_a0(&c);
        wl(&c, DATA_AREA, big[i].head);
        cpu->a[1] = DATA_AREA;
        RUN(&c, cpu, 0xF359, 0xF310);
        assert(cpu->a[1] == DATA_AREA + big[i].bytes && at(&c, 0) == 0x41000000u);
        mc68k_destroy(cpu);
    }

    /* (A1) モードは An を変えない。 */
    cpu = make_a0(&c);
    wl(&c, DATA_AREA, 0x41000000u);
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF351);
    assert(cpu->a[1] == DATA_AREA);
    mc68k_destroy(cpu);

    /* version / size が不正なら format error。An は変えない。 */
    static const uint32_t bad[] = {0x40000000u, 0x42000000u, 0x41100000u, 0x41310000u,
                                   0x41610000u, 0x41FF0000u, 0x41E00000u};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cpu = make_a0(&c);
        wl(&c, DATA_AREA, bad[i]);
        cpu->a[1] = DATA_AREA;
        RUN(&c, cpu, 0xF359);
        assert(cpu->pc == HANDLER(VEC_FORMAT_ERROR) && cpu->a[1] == DATA_AREA);
        expect_pre(&c, cpu, VEC_FORMAT_ERROR, RESET_PC); /* format $0、PC = FRESTORE */
        mc68k_destroy(cpu);
    }
    /* 有効な size（0・$30・$60）の 1 ビットを反転したものは、すべて不正（size の全 8
     * ビットを見ている）。 */
    static const unsigned valid_sizes[] = {0x00, 0x30, 0x60};
    for (unsigned i = 0; i < 3; i++)
        for (unsigned bit = 0; bit < 8; bit++) {
            cpu = make_a0(&c);
            wl(&c, DATA_AREA, 0x41000000u | (uint32_t)(valid_sizes[i] ^ (1u << bit)) << 16);
            cpu->a[1] = DATA_AREA;
            RUN(&c, cpu, 0xF359);
            assert(cpu->pc == HANDLER(VEC_FORMAT_ERROR) && cpu->a[1] == DATA_AREA);
            mc68k_destroy(cpu);
        }

    /* null frame（version 0）は size が 0 でなくても本文を読まない: RAM の末尾の 4 バイトだけの
     * frame でもバスエラーにならない。 */
    cpu = make_a0(&c);
    wl(&c, RAM_SIZE - 4, 0x00600000u);
    cpu->a[1] = RAM_SIZE - 4;
    const uint32_t null_end = RUN(&c, cpu, 0xF351);
    assert(cpu->pc == null_end && cpu->a[1] == RAM_SIZE - 4);
    mc68k_destroy(cpu);
}

/* ---- 例外状態を載せた frame ---- */

/* busy frame の long。busy のオフセットは設計書 §6。 */
static void expect_busy_head(const ctx_t *c) {
    assert(at(c, 0) == 0x41600000u);
}

static void test_busy_frame_dz(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    /* handler = FSAVE (A0); RTE（A0 は SAVE_AREA）。 */
    ww(&c, HANDLER(VEC_DZ), 0xF310);
    ww(&c, HANDLER(VEC_DZ) + 2, 0x4E73);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    /* FDIV FP1,FP0（DZ を保留）→ FNOP が報告 → handler が FSAVE して RTE → FNOP 再実行。 */
    const uint32_t end = RUN(&c, cpu, 0xF200, 0x0420, 0xF280, 0x0000);
    assert(cpu->pc == end && cpu->a[7] == RESET_SP && !cpu->fpu040->exc.valid);
    expect_busy_head(&c);
    assert(at(&c, 0x28) == RESET_PC);          /* FPIARCU */
    assert(at(&c, 0x34) == 0);                 /* CMDREG3B（E3 は作らない） */
    assert(at(&c, 0x38) == 0);                 /* NMNEXC: 未対応データ型ではない */
    assert(at(&c, 0x3C) == (uint32_t)1 << 29); /* STAG: ソース FP1 = zero */
    assert(at(&c, 0x40) == 0x04200000u);       /* CMDREG1B（上位語） */
    assert(at(&c, 0x44) == 0);                 /* DTAG: 宛先 FP0 = normalized */
    assert(at(&c, 0x48) == 0x04000000u);       /* E1（bit 26）、T = 0 */
    /* FPSP の E_BYTE/T_BYTE/CMDREG1B と同じ位置: $48 の bit 2、$49 の bit 4、$40 の語。 */
    assert((c.ram[SAVE_AREA + 0x48] & 0x04) && !(c.ram[SAVE_AREA + 0x49] & 0x10));
    assert(rw(&c, SAVE_AREA + 0x40) == 0x0420);
    assert(at(&c, 0x4C) == 0x3FFF0000u && at(&c, 0x50) == 0x80000000u &&
           at(&c, 0x54) == 0);                                           /* FPTEMP */
    assert(at(&c, 0x58) == 0 && at(&c, 0x5C) == 0 && at(&c, 0x60) == 0); /* ETEMP */
    mc68k_destroy(cpu);
}

/* FPTEMP = 宛先、ETEMP = ソース（ここでは FDIV FP1,FP0: src = FP1 = 0、dst = FP0 =
 * 1）。取り違えを検出する。 */
static void test_busy_frame_operands(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_DZ), 0xF310);
    ww(&c, HANDLER(VEC_DZ) + 2, 0x60FE);
    fst(cpu)->fp[0] = TWO;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    RUN(&c, cpu, 0xF200, 0x0420, 0xF280, 0x0000);
    assert(at(&c, 0x4C) == 0x40000000u && at(&c, 0x50) == 0x80000000u); /* FPTEMP = 2.0 */
    assert(at(&c, 0x44) == 0);                                          /* DTAG normalized */
    assert(at(&c, 0x58) == 0 && at(&c, 0x5C) == 0);                     /* ETEMP = 0 */
    assert(at(&c, 0x3C) == (uint32_t)1 << 29);                          /* STAG zero */
    mc68k_destroy(cpu);
}

/* FSAVE まで同じ例外を報告し続け、FSAVE の後は報告しない。FMOVEM/FMOVE FPcr は素通り。 */
static void test_exception_state_persists(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    ww(&c, HANDLER(VEC_DZ), 0x4E73); /* handler: RTE のみ（FSAVE しない） */
    RUN(&c, cpu, 0xF200, 0x0420, 0xF280, 0x0000);
    /* FNOP は報告 → RTE → 再実行 → また報告…を繰り返す。a7 は毎回同じ位置へ戻る。 */
    assert(cpu->fpu040->exc.valid && cpu->fpu040->exc.vector == VEC_DZ);
    assert(cpu->last_exception_vector == VEC_DZ);
    mc68k_destroy(cpu);

    cpu = make_a0(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    ww(&c, HANDLER(VEC_DZ), 0xF310);
    ww(&c, HANDLER(VEC_DZ) + 2, 0x4E73);
    /* FSAVE で消える。以後の FADD は報告しない。 */
    const uint32_t end = RUN(&c, cpu, 0xF200, 0x0420, 0xF280, 0x0000, 0xF200, 0x0422);
    assert(cpu->pc == end && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);

    /* FMOVEM と FMOVE FPcr は、4 方向とも保留中の例外を報告せず、FPIAR も変えない。 */
    const struct {
        uint16_t op, ext;
    } passthrough[] = {
        {0xF200, 0x9000}, /* FMOVE.L D0,FPCR */
        {0xF200, 0xB000}, /* FMOVE.L FPCR,D0 */
        {0xF211, 0xD010}, /* FMOVEM.X (A1),FP3 */
        {0xF211, 0xF010}, /* FMOVEM.X FP3,(A1) */
    };
    const unsigned passthrough_count = sizeof(passthrough) / sizeof(passthrough[0]);
    assert(passthrough_count == 4);
    unsigned executed = 0;
    for (unsigned i = 0; i < passthrough_count; i++) {
        cpu = make_a0(&c);
        fst(cpu)->fp[0] = ONE;
        fst(cpu)->fp[1] = ZERO;
        fst(cpu)->fpcr_enable = EXC_DZ;
        cpu->a[1] = DATA_AREA;
        const uint32_t last =
            RUN(&c, cpu, 0xF200, 0x0420, passthrough[i].op, passthrough[i].ext);
        assert(cpu->pc == last && cpu->a[7] == RESET_SP);
        assert(cpu->fpu040->exc.valid && cpu->fpu040->exc.vector == VEC_DZ &&
               fst(cpu)->fpiar == RESET_PC);
        mc68k_destroy(cpu);
        executed++;
    }
    assert(executed == passthrough_count);
}

/* ---- unimplemented frame ---- */

static void test_unimplemented_frame(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0xF310);
    ww(&c, HANDLER(VEC_FLINE) + 2, 0x60FE);
    fst(cpu)->fp[1] = THREE;
    RUN(&c, cpu, 0xF200, 0x050E); /* FSIN FP1,FP2 */
    assert(at(&c, 0) == 0x41300000u);
    assert(at(&c, 0x04) == 0 && at(&c, 0x0C) == 0 && at(&c, 0x10) == 0x050E0000u);
    assert(at(&c, 0x14) == 0 && at(&c, 0x18) == 0); /* E1 = 0: packed ソースでない */
    assert(at(&c, 0x28) == 0x40000000u && at(&c, 0x2C) == 0xC0000000u && at(&c, 0x30) == 0);
    assert(at(&c, 0x1C) == 0 && at(&c, 0x20) == 0 && at(&c, 0x24) == 0); /* 単項: FPTEMP なし */
    mc68k_destroy(cpu);

    /* 二項（FMOD FP1,FP0）: FPTEMP = FP0。FSIN.X (A1)+,FP2: ソースはメモリから ETEMP へ。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0xF310);
    ww(&c, HANDLER(VEC_FLINE) + 2, 0x60FE);
    fst(cpu)->fp[0] = FIVE;
    fst(cpu)->fp[1] = TWO;
    RUN(&c, cpu, 0xF200, 0x0421); /* FMOD */
    assert(at(&c, 0x1C) == 0x40010000u && at(&c, 0x20) == 0xA0000000u);
    assert(at(&c, 0x28) == 0x40000000u && at(&c, 0x2C) == 0x80000000u);
    assert(at(&c, 0x14) == 0);
    mc68k_destroy(cpu);

    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0xF310);
    ww(&c, HANDLER(VEC_FLINE) + 2, 0x60FE);
    wx(&c, DATA_AREA, FOUR);
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF219, 0x490E); /* FSIN.X (A1)+,FP2 */
    assert(at(&c, 0x28) == 0x40010000u && at(&c, 0x2C) == 0x80000000u &&
           cpu->a[1] == DATA_AREA + 12);
    assert(at(&c, 0x10) == 0x490E0000u && at(&c, 0x18) == 0);
    mc68k_destroy(cpu);

    /* FMOVECR: ソースなし。FSAVE は unimplemented frame。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0xF310);
    ww(&c, HANDLER(VEC_FLINE) + 2, 0x60FE);
    RUN(&c, cpu, 0xF200, 0x5C00);
    assert(at(&c, 0) == 0x41300000u && at(&c, 0x28) == 0 && at(&c, 0x0C) == 0);
    mc68k_destroy(cpu);

    /* 例外状態は FSAVE で消え、2 回目の FSAVE は idle。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0xF310);
    ww(&c, HANDLER(VEC_FLINE) + 2, 0xF310);
    ww(&c, HANDLER(VEC_FLINE) + 4, 0x60FE);
    RUN(&c, cpu, 0xF200, 0x050E);
    assert(at(&c, 0) == 0x41000000u && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);
}

/* ---- 未対応データ型の frame ---- */

static void test_unsupported_frame(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* 拡張精度の denormal ソース: STAG = 4、ETEMP は raw のまま。FSQRT は CMDREG1B の opmode が
     * 5。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_UNSUPPORTED), 0xF310);
    ww(&c, HANDLER(VEC_UNSUPPORTED) + 2, 0x60FE);
    wx(&c, DATA_AREA, fp(0x0000, 0x0000000000000005ull));
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_IN(FMT_X, 0, 0x04), 0xF280,
        0x0000); /* FSQRT.X (A1),FP0; FNOP が報告 */
    expect_busy_head(&c);
    assert(at(&c, 0x38) == NMN_UNSUP); /* NMNEXC の unsup bit（推論 25） */
    assert(at(&c, 0x3C) == (uint32_t)4 << 29 && at(&c, 0x40) == (uint32_t)EXT_IN(FMT_X, 0, 0x05)
                                                                    << 16);
    assert(at(&c, 0x58) == 0 && at(&c, 0x5C) == 0 && at(&c, 0x60) == 5 &&
           at(&c, 0x48) == 0x04000000u);
    mc68k_destroy(cpu);

    /* 単精度 denormal: STAG = 5、ETEMP は指数 0・仮数を bit 62 から左詰め（図 9-9）。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_UNSUPPORTED), 0xF310);
    ww(&c, HANDLER(VEC_UNSUPPORTED) + 2, 0x60FE);
    wl(&c, DATA_AREA, 0x80400000u); /* 負、仮数 bit 22 */
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_IN(FMT_S, 0, 0x00), 0xF280, 0x0000);
    assert(at(&c, 0x3C) == (uint32_t)5 << 29);
    assert(at(&c, 0x58) == 0x80000000u && at(&c, 0x5C) == 0x40000000u && at(&c, 0x60) == 0);
    mc68k_destroy(cpu);

    /* 倍精度 denormal。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_UNSUPPORTED), 0xF310);
    ww(&c, HANDLER(VEC_UNSUPPORTED) + 2, 0x60FE);
    wl(&c, DATA_AREA, 0x00080000u);
    wl(&c, DATA_AREA + 4, 0);
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_IN(FMT_D, 0, 0x00), 0xF280, 0x0000);
    assert(at(&c, 0x3C) == (uint32_t)5 << 29);
    assert(at(&c, 0x58) == 0 && at(&c, 0x5C) == 0x40000000u && at(&c, 0x60) == 0);
    mc68k_destroy(cpu);

    /* packed: ETM[63-0] = 下位 64 bit（+4, +8）、FPTM[31-0] = 上位 32
     * bit（+0）。宛先は入れない。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_UNSUPPORTED), 0xF310);
    ww(&c, HANDLER(VEC_UNSUPPORTED) + 2, 0x60FE);
    wl(&c, DATA_AREA, 0x11111111u);
    wl(&c, DATA_AREA + 4, 0x22222222u);
    wl(&c, DATA_AREA + 8, 0x33333333u);
    fst(cpu)->fp[0] = ONE;
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_IN(FMT_P, 0, 0x22), 0xF280, 0x0000);
    assert(at(&c, 0x5C) == 0x22222222u && at(&c, 0x60) == 0x33333333u);
    assert(at(&c, 0x54) == 0x11111111u && at(&c, 0x4C) == 0 && at(&c, 0x50) == 0);
    mc68k_destroy(cpu);

    /* opclass 011（FMOVE.P FP0,(A1)）: T = 1、ETEMP = レジスタ値、STAG。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_UNSUPPORTED), 0xF310);
    ww(&c, HANDLER(VEC_UNSUPPORTED) + 2, 0x60FE);
    fst(cpu)->fp[0] = TWO;
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_OUT(FMT_P, 0));
    assert(at(&c, 0x48) == 0x04100000u && at(&c, 0x40) == (uint32_t)EXT_OUT(FMT_P, 0) << 16);
    assert(at(&c, 0x58) == 0x40000000u && at(&c, 0x5C) == 0x80000000u && at(&c, 0x3C) == 0);
    mc68k_destroy(cpu);
}

/* post-instruction 例外の frame は T = 1。 */
static void test_post_frame(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_OPERR), 0xF310);
    ww(&c, HANDLER(VEC_OPERR) + 2, 0x60FE);
    fst(cpu)->fp[0] = fp(0x4027, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_OUT(FMT_L, 0));
    expect_busy_head(&c);
    assert(at(&c, 0x48) == 0x04100000u && at(&c, 0x58) == 0x40270000u);
    assert((c.ram[SAVE_AREA + 0x48] & 0x04) && (c.ram[SAVE_AREA + 0x49] & 0x10)); /* E1、T */
    mc68k_destroy(cpu);
}

/* 例外状態を載せた frame の細部: タグ、宛先を持たない命令、packed の二項、BSUN、FRESTORE
 * の効果。 */
static void handler_save(ctx_t *c, unsigned vector) {
    ww(c, HANDLER(vector), 0xF310); /* FSAVE (A0) */
    ww(c, HANDLER(vector) + 2, 0x60FE);
}

static void test_frame_details(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* ソースのタグ: Inf = 2、NaN = 3、零 = 1（FSIN FP1,FP2 = 未実装）。 */
    static const struct {
        mc68k_fp80_t v;
        uint32_t tag;
    } tags[] = {
        {{0x7FFF, 0x0000000000000000ull}, 2},
        {{0x7FFF, 0xC000000000000000ull}, 3},
        {{0x0000, 0x0000000000000000ull}, 1},
    };
    for (unsigned i = 0; i < 3; i++) {
        cpu = make_a0(&c);
        handler_save(&c, VEC_FLINE);
        fst(cpu)->fp[1] = tags[i].v;
        RUN(&c, cpu, 0xF200, 0x050E);
        assert(at(&c, 0x0C) == tags[i].tag << 29);
        mc68k_destroy(cpu);
    }

    /* FSCALE FP1,FP0（二項・未実装）は FPTEMP = FP0。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_FLINE);
    fst(cpu)->fp[0] = FIVE;
    fst(cpu)->fp[1] = TWO;
    RUN(&c, cpu, 0xF200, 0x0426);
    assert(at(&c, 0x1C) == 0x40010000u && at(&c, 0x14) == 0);
    mc68k_destroy(cpu);

    /* 二項の未実装命令でも packed ソースなら FPTEMP は packed の上位 32 bit のまま（FMOD.P
     * (A1),FP0）。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_FLINE);
    wl(&c, DATA_AREA, 0x11111111u);
    wl(&c, DATA_AREA + 4, 0x22222222u);
    wl(&c, DATA_AREA + 8, 0x33333333u);
    fst(cpu)->fp[0] = FIVE;
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_IN(FMT_P, 0, 0x21));
    assert(at(&c, 0x1C) == 0 && at(&c, 0x20) == 0 && at(&c, 0x24) == 0x11111111u);
    assert(at(&c, 0x2C) == 0x22222222u && at(&c, 0x30) == 0x33333333u);
    mc68k_destroy(cpu);

    /* 未対応データ型でも、宛先を読まない命令（FSQRT）は FPTEMP/DTAG を空にする。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_UNSUPPORTED);
    wx(&c, DATA_AREA, fp(0x0000, 5));
    fst(cpu)->fp[0] = FIVE;
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_IN(FMT_X, 0, 0x04), 0xF280, 0x0000);
    assert(at(&c, 0x44) == 0 && at(&c, 0x4C) == 0 && at(&c, 0x50) == 0);
    mc68k_destroy(cpu);
    /* 宛先を読む FADD は FPTEMP に宛先。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_UNSUPPORTED);
    wx(&c, DATA_AREA, fp(0x0000, 5));
    fst(cpu)->fp[0] = FIVE;
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_IN(FMT_X, 0, 0x22), 0xF280, 0x0000);
    assert(at(&c, 0x4C) == 0x40010000u && at(&c, 0x50) == 0xA0000000u);
    mc68k_destroy(cpu);

    /* 算術例外（単項の FSQRT -1 + OPERR 有効）は宛先を持たない。二項（FDIV）は持つ。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_OPERR);
    fst(cpu)->fp[0] = FIVE;
    fst(cpu)->fp[1] = NEG_ONE;
    fst(cpu)->fpcr_enable = EXC_OPERR;
    RUN(&c, cpu, 0xF200, 0x0404, 0xF280, 0x0000);
    assert(at(&c, 0x44) == 0 && at(&c, 0x4C) == 0 && at(&c, 0x58) == 0xBFFF0000u);
    assert(at(&c, 0x40) == 0x04050000u);
    mc68k_destroy(cpu);

    /* BSUN: handler が FSAVE すると busy frame（E1）で、例外状態は消える。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_BSUN);
    fst(cpu)->fpsr_cc = MC68K_FPU_CC_NAN;
    fst(cpu)->fpcr_enable = EXC_BSUN;
    RUN(&c, cpu, 0xF292, 0x0004, 0xF280, 0x0000);
    expect_busy_head(&c);
    assert(at(&c, 0x48) == 0x04000000u && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);

    /* FRESTORE は例外状態を消す（idle へ）。以後の FP 命令は報告しない。 */
    cpu = make_a0(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    ww(&c, HANDLER(VEC_DZ), 0xF351); /* FRESTORE (A1) */
    ww(&c, HANDLER(VEC_DZ) + 2, 0x4E73);
    wl(&c, DATA_AREA, 0x41000000u);
    cpu->a[1] = DATA_AREA;
    const uint32_t end = RUN(&c, cpu, 0xF200, 0x0420, 0xF280, 0x0000);
    assert(cpu->pc == end && !cpu->fpu040->exc.valid && cpu->a[7] == RESET_SP);
    mc68k_destroy(cpu);

    /* FRESTORE の本文読出しで失敗したら An は更新しない（frame の頭だけ RAM 内）。 */
    cpu = make_a0(&c);
    wl(&c, RAM_SIZE - 4, 0x41600000u);
    cpu->a[1] = RAM_SIZE - 4;
    RUN(&c, cpu, 0xF359);
    assert(cpu->a[1] == RAM_SIZE - 4);
    mc68k_destroy(cpu);
}

/* unimplemented frame の E1 は packed ソース（FPSP の get_op.sa。UM
 * の本文）。レジスタ・メモリの 非 packed ソースでは 0。 */
static void test_unimplemented_e1_is_packed(void) {
    static const struct {
        uint16_t opcode, ext;
        bool e1;
    } cases[] = {
        {0xF200, 0x050E, false},                 /* FSIN FP1,FP2 */
        {0xF211, EXT_IN(FMT_X, 0, 0x0E), false}, /* FSIN.X (A1),FP0 */
        {0xF211, EXT_IN(FMT_P, 0, 0x0E), true},  /* FSIN.P (A1),FP0 */
        {0xF211, EXT_IN(FMT_P, 0, 0x21), true},  /* FMOD.P (A1),FP0 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make_a0(&c);
        handler_save(&c, VEC_FLINE);
        cpu->a[1] = DATA_AREA;
        RUN(&c, cpu, cases[i].opcode, cases[i].ext);
        assert(((c.ram[SAVE_AREA + 0x18] >> 2) & 1) == cases[i].e1);
        assert(at(&c, 0x18) == (cases[i].e1 ? 0x04000000u : 0));
        mc68k_destroy(cpu);
    }
}

/* 保留中の例外は、未実装命令（format $2）より先に報告される（[040UM §9.6.1]）。 */
static void test_unimplemented_reports_pending(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    RUN(&c, cpu, 0xF200, 0x0420, 0xF200, 0x050E); /* FDIV FP1,FP0（DZ）; FSIN FP1,FP2 */
    expect_pre(&c, cpu, VEC_DZ, RESET_PC + 4);
    assert(cpu->fpu040->exc.valid && cpu->fpu040->exc.vector == VEC_DZ &&
           !cpu->fpu040->exc.unimpl);
    mc68k_destroy(cpu);

    /* handler が FSAVE して RTE すると、FSIN は改めて format $2 になる。 */
    cpu = make_a0(&c);
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    fst(cpu)->fp[0] = ONE;
    ww(&c, HANDLER(VEC_DZ), 0xF310);
    ww(&c, HANDLER(VEC_DZ) + 2, 0x4E73);
    RUN(&c, cpu, 0xF200, 0x0420, 0xF200, 0x050E);
    assert(cpu->pc == HANDLER(VEC_FLINE) && cpu->a[7] == RESET_SP - 12);
    assert(rw(&c, cpu->a[7] + 6) == (uint16_t)(0x2000u | VEC_FLINE * 4));
    assert(rl(&c, cpu->a[7] + 2) == RESET_PC + 8 && at(&c, 0) == 0x41600000u &&
           at(&c, 0x28) == RESET_PC); /* 保留の busy frame を保存済み */
    assert(cpu->fpu040->exc.valid && cpu->fpu040->exc.unimpl);
    mc68k_destroy(cpu);
}

/* 未実装命令自身の状態は保留中の例外ではない。handler が FSAVE せず RTE だけで戻っても、次の FP
 * 命令は format $0 で同じ命令を再実行させず、状態を捨てて実行する（設計書 §5、推論 11）。 */
static void test_unimplemented_state_not_reported(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0x4E73); /* RTE のみ */
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = TWO;
    const uint32_t end =
        RUN(&c, cpu, 0xF200, 0x050E, 0xF200, 0x0422); /* FSIN FP1,FP2; FADD FP1,FP0 */
    assert(cpu->pc == end && cpu->a[7] == RESET_SP);
    assert(same(fst(cpu)->fp[0], THREE) && !cpu->fpu040->exc.valid);
    ww(&c, end, 0xF310); /* FSAVE (A0) */
    ww(&c, end + 2, 0x60fe);
    cpu->pc = end;
    mc68k_execute(cpu, 100);
    assert((at(&c, 0) >> 24) == 0x41 && ((at(&c, 0) >> 16) & 0xFF) == 0); /* idle */
    mc68k_destroy(cpu);

    /* 未実装命令が続けば、2 回目も format $2（1 回目は RTE のみで戻し、2 回目の frame
     * を見る）。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0x4E73);
    fst(cpu)->fp[1] = ONE;
    const uint32_t second = RUN(&c, cpu, 0xF200, 0x050E);
    assert(cpu->pc == second && cpu->a[7] == RESET_SP && cpu->fpu040->exc.unimpl);
    ww(&c, HANDLER(VEC_FLINE), 0x60fe);
    ww(&c, second, 0xF200);
    ww(&c, second + 2, 0x050E);
    ww(&c, second + 4, 0x60fe);
    cpu->pc = second;
    mc68k_execute(cpu, 100);
    assert(cpu->pc == HANDLER(VEC_FLINE) && cpu->a[7] == RESET_SP - 12);
    assert(rw(&c, cpu->a[7] + 6) == (uint16_t)(0x2000u | VEC_FLINE * 4));
    assert(rl(&c, cpu->a[7] + 2) == RESET_PC + 8);
    assert(cpu->fpu040->exc.valid && cpu->fpu040->exc.unimpl);
    mc68k_destroy(cpu);

    /* 条件命令（FNOP）も状態を捨てる。FMOVE FPcr と FMOVEM は捨てず、続く FSAVE は
     * unimplemented frame。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0x4E73);
    fst(cpu)->fp[1] = ONE;
    uint32_t last = RUN(&c, cpu, 0xF200, 0x050E, 0xF280, 0x0000); /* FSIN; FNOP */
    assert(cpu->pc == last && cpu->a[7] == RESET_SP && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);

    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0x4E73);
    fst(cpu)->fp[1] = ONE;
    last = RUN(&c, cpu, 0xF200, 0x050E, 0xF200, 0xB000,
               0xF310); /* FSIN; FMOVE.L FPCR,D0; FSAVE (A0) */
    assert(cpu->pc == last && at(&c, 0) == 0x41300000u);
    mc68k_destroy(cpu);

    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0x4E73);
    fst(cpu)->fp[1] = ONE;
    cpu->a[1] = DATA_AREA;
    last = RUN(&c, cpu, 0xF200, 0x050E, 0xF211, 0xF080,
               0xF310); /* FSIN; FMOVEM.X FP0,(A1); FSAVE (A0) */
    assert(cpu->pc == last && at(&c, 0) == 0x41300000u);
    mc68k_destroy(cpu);

    /* メモリ→レジスタ方向（FMOVE.L D0,FPCR と FMOVEM.X (A1),FP0）も同じ。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0x4E73);
    fst(cpu)->fp[1] = ONE;
    last = RUN(&c, cpu, 0xF200, 0x050E, 0xF200, 0x9000,
               0xF310); /* FSIN; FMOVE.L D0,FPCR; FSAVE (A0) */
    assert(cpu->pc == last && at(&c, 0) == 0x41300000u);
    mc68k_destroy(cpu);

    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0x4E73);
    fst(cpu)->fp[1] = ONE;
    cpu->a[1] = DATA_AREA;
    last = RUN(&c, cpu, 0xF200, 0x050E, 0xF211, 0xD080,
               0xF310); /* FSIN; FMOVEM.X (A1),FP0; FSAVE (A0) */
    assert(cpu->pc == last && at(&c, 0) == 0x41300000u);
    mc68k_destroy(cpu);

    /* 未実装命令自身も開始時に古い状態を捨てる。ソース取得がアクセスエラーになっても残らず、
     * アクセスエラー handler の FSAVE は idle。 */
    cpu = make_a0(&c);
    ww(&c, HANDLER(VEC_FLINE), 0x4E73);
    ww(&c, HANDLER(2), 0xF310); /* FSAVE (A0) */
    ww(&c, HANDLER(2) + 2, 0x60fe);
    fst(cpu)->fp[1] = ONE;
    cpu->a[1] = RAM_SIZE + 0x100;
    RUN(&c, cpu, 0xF200, 0x050E, 0xF211, 0x490E); /* FSIN; FSIN.X (A1),FP2 */
    assert(cpu->pc == HANDLER(2) + 2 && at(&c, 0) == 0x41000000u);
    mc68k_destroy(cpu);
}

/* UNFL/INEX の E1 frame は、FMOVE のレジスタ宛てと FMOVE out で FPTEMP に丸め前の中間結果を持つ
 * （[040UM §9.8 Table 9-16]）。 */
static void test_unrounded_result_in_fptemp(void) {
    ctx_t c;
    mc68k_state_t *cpu;
    const mc68k_fp80_t inexact =
        fp(0x3FFF, 0x8000000200000000ull); /* 1 + 2^-30: single で不正確 */

    /* FMOVE.S FP0,(A1): UNFL（nonmaskable）。FPTEMP = レジスタ値、ETEMP も同じ。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_UNFL);
    fst(cpu)->fp[0] = fp(0x3F00, 0x8000000000000000ull); /* 2^-255 */
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_OUT(FMT_S, 0));
    assert(at(&c, 0x4C) == 0x3F000000u && at(&c, 0x50) == 0x80000000u && at(&c, 0x54) == 0);
    assert(at(&c, 0x58) == 0x3F000000u && at(&c, 0x44) == 0);
    mc68k_destroy(cpu);

    /* FMOVE.S FP0,(A1): INEX2。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_INEX);
    fst(cpu)->fp[0] = inexact;
    fst(cpu)->fpcr_enable = EXC_INEX2;
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_OUT(FMT_S, 0));
    assert(at(&c, 0x4C) == 0x3FFF0000u && at(&c, 0x50) == 0x80000002u && at(&c, 0x54) == 0);
    mc68k_destroy(cpu);

    /* FMOVE FP1,FP0: single に丸まる INEX2。FPTEMP = ソース、DTAG は normalized。FSMOVE
     * も同じ。 */
    static const uint16_t opmodes[] = {0x00, 0x40};
    for (unsigned i = 0; i < 2; i++) {
        cpu = make_a0(&c);
        handler_save(&c, VEC_INEX);
        fst(cpu)->fp[1] = inexact;
        fst(cpu)->fpcr_enable = EXC_INEX2;
        fst(cpu)->fpcr_round_prec = MC68K_FPU_PREC_SINGLE;
        RUN(&c, cpu, 0xF200, EXT_RR(1, 0, opmodes[i]), 0xF280, 0x0000);
        assert(at(&c, 0x4C) == 0x3FFF0000u && at(&c, 0x50) == 0x80000002u && at(&c, 0x54) == 0);
        assert(at(&c, 0x44) == 0);
        mc68k_destroy(cpu);
    }

    /* FADD の INEX2 は E3 frame（FPTEMP には宛先が残る。FMOVE の規則は適用しない）。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_INEX);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = fp(0x3FE1, 0x8000000000000000ull);
    fst(cpu)->fpcr_enable = EXC_INEX2;
    fst(cpu)->fpcr_round_prec = MC68K_FPU_PREC_SINGLE;
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x22), 0xF280, 0x0000);
    assert(at(&c, 0x4C) == 0x3FFF0000u && at(&c, 0x50) == 0x80000000u);
    mc68k_destroy(cpu);
}

/* ---- E3 frame（ Phase C。[040UM] Table 9-16、設計書 §6・推論 17〜20） ---- */

enum {
    E3_ONLY = 0x02000000,
    E1_ONLY = 0x04000000,
    E1_T = 0x04100000,
    GUARD = 0x02000000,
    ROUND_BIT = 0x01000000,
    STICKY = 0x00800000,
    WBTE15_BIT = 0x00100000,
    REPORT_NOP_0 = 0xF280,
    REPORT_NOP_1 = 0x0000,
};

/* E3 の frame の共通部分。WBTEMP（$18〜）、STAG の long、DTAG の long、CMDREG3B、E3/E1/T。 */
static void expect_e3(const ctx_t *c, uint32_t wbts_wbte, uint32_t wbtm_hi, uint32_t wbtm_lo,
                      uint32_t grs, uint32_t wbte15, uint16_t cmdreg3b) {
    assert(at(c, 0x00) == 0x41600000u);
    assert(at(c, 0x18) == wbts_wbte && at(c, 0x1C) == wbtm_hi && at(c, 0x20) == wbtm_lo);
    assert(at(c, 0x34) == (uint32_t)cmdreg3b << 16);
    assert((at(c, 0x3C) & 0x07FFFFFFu) == grs); /* M66 = 0（推論 18）、guard/round/sticky */
    assert((at(c, 0x44) & 0x1FFFFFFFu) == wbte15);
    assert(at(c, 0x48) == E3_ONLY); /* E1 = 0、T = 0（推論 20） */
}

/* OVFL: WBTEMP は仮数を丸めた中間結果、WBTE15 = 0、guard/round/sticky = 0。 */
static void test_e3_overflow(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);

    /* FADD FP1,FP0: 最大値 + 最大値 = 指数 $7FFF（厳密）。 */
    handler_save(&c, VEC_OVFL);
    fst(cpu)->fp[0] = fp(0x7FFE, 0xFFFFFFFFFFFFFFFFull);
    fst(cpu)->fp[1] = fp(0x7FFE, 0xFFFFFFFFFFFFFFFFull);
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x22), REPORT_NOP_0, REPORT_NOP_1);
    expect_e3(&c, 0x7FFF0000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0x12);
    assert(at(&c, 0x40) == 0x04220000u && at(&c, 0x44) == 0); /* CMDREG1B、DTAG normalized */
    mc68k_destroy(cpu);

    /* FSADD: 24 bit 仮数の最大値 + 半 ulp = タイで、保持側の最下位ビットが 1
     * なので繰り上がり、桁上がりで 指数が +1 になる（$407E → $407F）。丸め後の仮数が WBTEMP。
     */
    cpu = make_a0(&c);
    handler_save(&c, VEC_OVFL);
    fst(cpu)->fp[0] = fp(0x407E, 0xFFFFFF0000000000ull);
    fst(cpu)->fp[1] = fp(0x4066, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x62), REPORT_NOP_0, REPORT_NOP_1);
    expect_e3(&c, 0x407F0000u, 0x80000000u, 0, 0, 0, 0x52);
    mc68k_destroy(cpu);

    /* 負数: WBTS = 1。FMUL 2^16383 × -2。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_OVFL);
    fst(cpu)->fp[0] = fp(0x7FFE, 0x8000000000000000ull);
    fst(cpu)->fp[1] = fp(0xC000, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x23), REPORT_NOP_0, REPORT_NOP_1);
    expect_e3(&c, 0xFFFF0000u, 0x80000000u, 0, 0, 0, 0x13);
    mc68k_destroy(cpu);

    /* 指数が $8000 以上（FMUL）でも WBTE15 = 0 で、指数は下位 15 bit（推論 18）。$7FFE × $7FFE
     * = $BFFD。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_OVFL);
    fst(cpu)->fp[0] = fp(0x7FFE, 0x8000000000000000ull);
    fst(cpu)->fp[1] = fp(0x7FFE, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x23), REPORT_NOP_0, REPORT_NOP_1);
    expect_e3(&c, 0x3FFD0000u, 0x80000000u, 0, 0, 0, 0x13);
    mc68k_destroy(cpu);
}

/* UNFL: WBTEMP は丸め前、WBTE15 = 1。 */
static void test_e3_underflow(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);

    /* FMUL: $0001 × -0.25（$BFFD）= 指数 -1 の負数（非正規化域）。WBTE = $7FFF（-1 の下位 15
     * bit）、WBTS = 1。 */
    handler_save(&c, VEC_UNFL);
    fst(cpu)->fp[0] = fp(0x0001, 0x8000000000000000ull);
    fst(cpu)->fp[1] = fp(0xBFFD, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x23), REPORT_NOP_0, REPORT_NOP_1);
    expect_e3(&c, 0xFFFF0000u, 0x80000000u, 0, 0, WBTE15_BIT, 0x13);
    mc68k_destroy(cpu);

    /* FSUB: 1 ulp の差（$0001 の最小差）は指数 -62 に正規化される。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_UNFL);
    fst(cpu)->fp[0] = fp(0x0001, 0x8000000000000001ull);
    fst(cpu)->fp[1] = fp(0x0001, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x28), REPORT_NOP_0, REPORT_NOP_1);
    expect_e3(&c, 0x7FC20000u, 0x80000000u, 0, 0, WBTE15_BIT, 0x14);
    mc68k_destroy(cpu);
}

/* INEX2（FPCR で有効）: 丸め前の中間結果と guard/round/sticky、WBTE15 = 0。結果は宛先へ書く。
 */
static void test_e3_inexact(void) {
    ctx_t c;
    mc68k_state_t *cpu;
    static const struct {
        mc68k_fp80_t small;
        uint32_t grs;
    } adds[] = {
        {{0x3FFF - 64, 0x8000000000000000ull}, GUARD},     /* 2^-64: guard = 1 */
        {{0x3FFF - 65, 0x8000000000000000ull}, ROUND_BIT}, /* 2^-65: round = 1 */
        {{0x3FFF - 65, 0x8000000000000001ull},
         ROUND_BIT | STICKY}, /* 2^-65 (1 + 2^-63): round と sticky */
        {{0x3FFF - 66, 0x8000000000000000ull},
         STICKY}, /* 2^-66: round bit より下なので sticky のみ */
    };
    assert(sizeof(adds) / sizeof(adds[0]) == 4);
    for (unsigned i = 0; i < sizeof(adds) / sizeof(adds[0]); i++) {
        cpu = make_a0(&c);
        handler_save(&c, VEC_INEX);
        fst(cpu)->fp[0] = ONE;
        fst(cpu)->fp[1] = adds[i].small;
        fst(cpu)->fpcr_enable = EXC_INEX2;
        RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x22), REPORT_NOP_0, REPORT_NOP_1);
        expect_e3(&c, 0x3FFF0000u, 0x80000000u, 0, adds[i].grs, 0, 0x12);
        mc68k_destroy(cpu);
    }

    /* 結果が負（WBTS = 1）の INEX2: WBTS|WBTE = 符号と指数（-2.0 は指数 $4000）。round = 1。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_INEX);
    fst(cpu)->fp[0] = fp(0xC000, 0x8000000000000000ull);
    fst(cpu)->fp[1] = fp(0x8000 | (0x4000 - 65), 0x8000000000000000ull); /* -(2^-64) */
    fst(cpu)->fpcr_enable = EXC_INEX2;
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x22), REPORT_NOP_0, REPORT_NOP_1);
    expect_e3(&c, 0xC0000000u, 0x80000000u, 0, ROUND_BIT, 0, 0x12);
    mc68k_destroy(cpu);

    /* FSQRT FP1,FP0（sqrt(2)）: CMDREG1B の opmode は $5、CMDREG3B は $21。guard = 0、round =
     * 1、sticky = 1。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_INEX);
    fst(cpu)->fp[1] = TWO;
    fst(cpu)->fpcr_enable = EXC_INEX2;
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x04), REPORT_NOP_0, REPORT_NOP_1);
    expect_e3(&c, 0x3FFF0000u, 0xB504F333u, 0xF9DE6484u, ROUND_BIT | STICKY, 0, 0x21);
    assert(at(&c, 0x40) == 0x04050000u);
    assert(at(&c, 0x58) == 0x40000000u && at(&c, 0x5C) == 0x80000000u); /* ETEMP = ソース */
    assert(at(&c, 0x4C) == 0 && at(&c, 0x50) == 0 &&
           at(&c, 0x54) == 0); /* 単項演算は FPTEMP を持たない */
    mc68k_destroy(cpu);
}

/* E3 の frame の STAG/DTAG は、guard/round/WBTE15 と同じ long にタグを持つ（`|=`）。0
 * のソース／宛先でも保たれる。 */
static void test_e3_tags(void) {
    ctx_t c;
    mc68k_state_t *cpu;
    const mc68k_fp80_t inexact_single = fp(0x3FFF, 0x8000000200000000ull); /* 1 + 2^-30 */

    cpu = make_a0(&c); /* FSADD FP1,FP0: ソース = 0（STAG = zero）、宛先は single で不正確 */
    handler_save(&c, VEC_INEX);
    fst(cpu)->fp[0] = inexact_single;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_INEX2;
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x62), REPORT_NOP_0, REPORT_NOP_1);
    assert(at(&c, 0x48) == E3_ONLY && (at(&c, 0x3C) >> 29) == 1 && (at(&c, 0x44) >> 29) == 0);
    assert((at(&c, 0x3C) & 0x07FFFFFFu) ==
           0); /* 端数は 64 bit 仮数の中にあり、sig_extra は 0 */
    mc68k_destroy(cpu);

    cpu = make_a0(&c); /* FSADD FP1,FP0: 宛先 = 0（DTAG = zero） */
    handler_save(&c, VEC_INEX);
    fst(cpu)->fp[0] = ZERO;
    fst(cpu)->fp[1] = inexact_single;
    fst(cpu)->fpcr_enable = EXC_INEX2;
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x62), REPORT_NOP_0, REPORT_NOP_1);
    assert(at(&c, 0x48) == E3_ONLY && (at(&c, 0x3C) >> 29) == 0 && (at(&c, 0x44) >> 29) == 1);
    mc68k_destroy(cpu);

    cpu = make_a0(&c); /* UNFL: WBTE15 は DTAG の long の bit 20 で、DTAG と共存する（宛先 = 0
                          では起きないので宛先 normalized） */
    handler_save(&c, VEC_UNFL);
    fst(cpu)->fp[0] = fp(0x0001, 0x8000000000000000ull);
    fst(cpu)->fp[1] = fp(0xBFFD, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x23), REPORT_NOP_0, REPORT_NOP_1);
    assert((at(&c, 0x44) & 0x00100000u) && (at(&c, 0x44) >> 29) == 0);
    mc68k_destroy(cpu);
}

/* CMDREG3B の写像（tests/data/fpu/mc68040_cmdreg3b_map.csv）。強制精度命令と FSGLMUL/FSGLDIV も E3。 */
static void test_e3_cmdreg3b(void) {
    ctx_t c;
    static const struct {
        uint16_t opmode, cmdreg1b_op, cmdreg3b;
    } ops[] = {
        {0x22, 0x22, 0x12}, {0x28, 0x28, 0x14}, {0x23, 0x23, 0x13}, {0x20, 0x20, 0x10},
        {0x04, 0x05, 0x21}, {0x27, 0x27, 0x33}, {0x24, 0x24, 0x30}, {0x62, 0x62, 0x52},
        {0x68, 0x68, 0x54}, {0x63, 0x63, 0x53}, {0x60, 0x60, 0x50}, {0x41, 0x41, 0x41},
        {0x66, 0x66, 0x72}, {0x6C, 0x6C, 0x74}, {0x67, 0x67, 0x73}, {0x64, 0x64, 0x70},
        {0x45, 0x45, 0x61},
    };
    assert(sizeof(ops) / sizeof(ops[0]) == 17);
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        for (unsigned dst = 0; dst < 8; dst++) {
            mc68k_state_t *cpu = make_a0(&c);
            const unsigned op = ops[i].opmode;
            handler_save(&c, VEC_INEX);
            fst(cpu)->fp[dst] = fp(0x3FFF, 0x8000000000000001ull); /* 1 + 2^-63 */
            fst(cpu)->fp[1 + (dst == 1)] = fp(0x4000, 0xC000000000000000ull);
            fst(cpu)->fpcr_enable = EXC_INEX2;
            uint16_t ext = EXT_RR(1 + (dst == 1), dst, op);
            if (op == 0x28 || op == 0x68 ||
                op == 0x6C) /* FSUB 系: dst - (-3) = 4 + 2^-63（正の 3 だと厳密） */
                fst(cpu)->fp[1 + (dst == 1)] = fp(0xC000, 0xC000000000000000ull);
            if (op == 0x20 || op == 0x60 ||
                op == 0x64) /* FDIV 系: (2^63+1)/3 は割り切れるので 2^63+2 を使う */
                fst(cpu)->fp[dst] = fp(0x3FFF, 0x8000000000000002ull);
            if (op == 0x27) { /* FSGLMUL: 24 bit 同士の積は 24 bit に収まらない */
                fst(cpu)->fp[dst] = fp(0x3FFF, 0xFFFFFF0000000000ull);
                fst(cpu)->fp[1 + (dst == 1)] = fp(0x3FFF, 0xFFFFFF0000000000ull);
            } else if (op == 0x04 || op == 0x41 || op == 0x45) {
                fst(cpu)->fp[1 + (dst == 1)] = TWO; /* sqrt(2) */
            }
            RUN(&c, cpu, 0xF200, ext, REPORT_NOP_0, REPORT_NOP_1);
            assert(at(&c, 0x48) == E3_ONLY);
            assert(at(&c, 0x08) == UINT32_C(1) << (16 + dst)); /* 宛先の dirty bit（推論 23） */
            assert(at(&c, 0x38) == 0);
            assert(at(&c, 0x34) == (uint32_t)(ops[i].cmdreg3b | dst << 7) << 16);
            assert(at(&c, 0x40) == (uint32_t)(EXT_RR(1 + (dst == 1), dst, ops[i].cmdreg1b_op))
                                       << 16);
            mc68k_destroy(cpu);
        }
    }
}

/* E3 にならないもの: FMOVE（E1 のまま）、報告されない INEX2、整数宛て。 */
static void test_e1_stays_e1(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);

    handler_save(&c, VEC_INEX);
    fst(cpu)->fp[1] = fp(0x3FFF, 0x8000000200000000ull);
    fst(cpu)->fpcr_enable = EXC_INEX2;
    fst(cpu)->fpcr_round_prec = MC68K_FPU_PREC_SINGLE;
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x00), REPORT_NOP_0, REPORT_NOP_1);
    assert(at(&c, 0x48) == E1_ONLY && at(&c, 0x34) == 0 && at(&c, 0x18) == 0 &&
           at(&c, 0x1C) == 0);
    mc68k_destroy(cpu);

    /* FMOVE out の整数宛て OPERR（nonmaskable）と、FMOVE の SNAN は、丸め前の値を FPTEMP
     * に持たない （Table 9-16: OPERR/SNAN は ETEMP だけ。FPTEMP を持つのは UNFL/INEX）。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_OPERR);
    fst(cpu)->fp[0] = fp(0x401F, 0x8000000000000000ull); /* 2^32: long に収まらない */
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_OUT(FMT_L, 0));
    assert(at(&c, 0x48) == E1_T && at(&c, 0x58) == 0x401F0000u);
    assert(at(&c, 0x4C) == 0 && at(&c, 0x50) == 0 && at(&c, 0x54) == 0);
    mc68k_destroy(cpu);
    cpu = make_a0(&c);
    handler_save(&c, VEC_SNAN);
    fst(cpu)->fp[1] = fp(0x7FFF, 0x8000000000000001ull);
    fst(cpu)->fpcr_enable = EXC_SNAN;
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x00), REPORT_NOP_0, REPORT_NOP_1);
    assert(at(&c, 0x48) == E1_ONLY && at(&c, 0x58) == 0x7FFF0000u);
    assert(at(&c, 0x4C) == 0 && at(&c, 0x50) == 0 && at(&c, 0x54) == 0);
    mc68k_destroy(cpu);

    /* INEX2 が有効でなければ、E3 になる演算でも例外を報告しない。 */
    cpu = make_a0(&c);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = fp(0x3FFF - 65, 0x8000000000000000ull);
    const uint32_t end = RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x22), REPORT_NOP_0, REPORT_NOP_1);
    assert(cpu->pc == end && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);
}

/* OVFL の E1（FMOVE out）: 仮数を丸めた中間結果を ETEMP に置き、UM どおり FPTEMP にも置く（推論
 * 21）。STAG は normalized。 */
static void test_e1_overflow_operand(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* FMOVE.S FP0,(A1): 2^129 は single で溢れる。post-instruction（format $3）、T = 1。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_OVFL);
    fst(cpu)->fp[0] = fp(0x4080, 0x8000000000000000ull);
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_OUT(FMT_S, 0));
    assert(at(&c, 0x48) == E1_T && at(&c, 0x3C) == 0);
    assert(at(&c, 0x4C) == 0x40800000u && at(&c, 0x50) == 0x80000000u && at(&c, 0x54) == 0);
    assert(at(&c, 0x58) == 0x40800000u && at(&c, 0x5C) == 0x80000000u && at(&c, 0x60) == 0);
    mc68k_destroy(cpu);

    /* 24 bit への丸めで桁上がりする値: 仮数が丸められ、指数が +1。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_OVFL);
    fst(cpu)->fp[0] = fp(0x4080, 0xFFFFFFFFFFFFFFFFull);
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_OUT(FMT_S, 0));
    assert(at(&c, 0x4C) == 0x40810000u && at(&c, 0x50) == 0x80000000u && at(&c, 0x54) == 0);
    assert(at(&c, 0x58) == 0x40810000u && at(&c, 0x5C) == 0x80000000u && at(&c, 0x60) == 0);
    mc68k_destroy(cpu);

    /* FMOVE FP1,FP0（レジスタ宛て）は、共通コアが OVFL を常にクリアする（68881 の規則。fpu.h の
     * mc68k_fpu_move）ので OVFL を報告しない。 */
    cpu = make_a0(&c);
    fst(cpu)->fp[1] = fp(0x4080, 0xFFFFFFFFFFFFFFFFull);
    fst(cpu)->fpcr_round_prec = MC68K_FPU_PREC_SINGLE;
    const uint32_t end = RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x00), REPORT_NOP_0, REPORT_NOP_1);
    assert(cpu->pc == end && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);
}

/* ---- FRESTORE の busy frame（ Phase D。設計書 §6・推論 10・23・24） ---- */

enum {
    FRESTORE_A0 = 0xF350,
    FSAVE_A1 = 0xF311,
    FLAG_E1 = 0x04000000,
    FLAG_E3 = 0x02000000,
    FLAG_T = 0x00100000,
    CU_RESUME = 0xFE,
    TAG_E15 = 0x10000000,
};

/* busy frame を base に置く。フィールドの位置は設計書 §6。cu は CU_SAVEPC（$08 の byte 0）。 */
static void put_busy(ctx_t *c, uint32_t base, uint16_t cmdreg1b, uint32_t flags, uint8_t stag,
                     uint8_t dtag, mc68k_fp80_t etemp, mc68k_fp80_t fptemp, uint8_t cu) {
    memset(&c->ram[base], 0, 100);
    wl(c, base, 0x41600000u);
    wl(c, base + 0x08, (uint32_t)cu << 24);
    wl(c, base + 0x3C, (uint32_t)stag << 29);
    wl(c, base + 0x40, (uint32_t)cmdreg1b << 16);
    wl(c, base + 0x44, (uint32_t)dtag << 29);
    wl(c, base + 0x48, flags);
    wx(c, base + 0x4C, fptemp);
    wx(c, base + 0x58, etemp);
}

/* handler_save() の handler（FSAVE (A0); BRA.S *）に入った後の pre-instruction frame。 */
static void expect_pre_saved(const ctx_t *c, const mc68k_state_t *cpu, unsigned vector,
                             uint32_t pc) {
    const uint32_t sp = RESET_SP - 8;
    assert(cpu->a[7] == sp && cpu->pc == HANDLER(vector) + 2);
    assert(rl(c, sp + 2) == pc && rw(c, sp + 6) == (uint16_t)(vector * 4));
}

static uint32_t at1(const ctx_t *c, uint32_t off) {
    return rl(c, DATA_AREA + off);
}

static void roundtrip(ctx_t *c, const uint8_t *saved, uint8_t fpsr_exc, uint8_t enable,
                      unsigned vector);

/* FPSP が frame を書き直して frestore する経路（CU_SAVEPC != 0、E1/E3/T = 0）:
 * 命令を完了させる。 */
static void test_restore_completes_instruction(void) {
    ctx_t c;
    mc68k_state_t *cpu;
    uint8_t saved[100];

    cpu = make_a0(&c);
    cpu->a[1] = DATA_AREA;
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), 0, 0, 0, TWO, ONE, CU_RESUME);
    fst(cpu)->fp[0] = FIVE; /* レジスタの値でなく frame の FPTEMP を使う */
    RUN(&c, cpu, FRESTORE_A0, FSAVE_A1);
    assert(same(fst(cpu)->fp[0], THREE) && !cpu->fpu040->exc.valid);
    assert(at1(&c, 0) == 0x41000000u); /* idle */
    mc68k_destroy(cpu);

    /* FSQRT（CMDREG1B の opmode は $5）、宛先は FP2。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, (uint16_t)(EXT_RR(1, 2, 0x05)), 0, 0, 0, FOUR, ZERO, CU_RESUME);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[2], TWO));
    mc68k_destroy(cpu);

    /* 強制精度（FSADD）: single に丸まって INEX2 が FPSR に立つ。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x62), 0, 0, 0, fp(0x3FFF, 0x8000000200000000ull), ONE,
             CU_RESUME);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[0], TWO) && (fst(cpu)->fpsr_exc & EXC_INEX2));
    mc68k_destroy(cpu);

    /* FSMOVE: PREC = 拡張でも single に丸める。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x40), 0, 0, 0, fp(0x3FFF, 0x8000000000000001ull),
             ZERO, CU_RESUME);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[0], ONE));
    mc68k_destroy(cpu);

    /* 前の EXC は消える（命令の開始で EXC byte をクリアする）。厳密な FADD で INEX2
     * も立たない。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), 0, 0, 0, TWO, ONE, CU_RESUME);
    fst(cpu)->fpsr_exc = EXC_DZ | EXC_INEX2;
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[0], THREE) && fst(cpu)->fpsr_exc == 0);
    mc68k_destroy(cpu);

    /* 完了時の例外は保留に積み、次の FP 命令が報告する（宛先は書かない）。FDIV 1/0 の DZ。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x20), 0, 1, 0, ZERO, ONE, CU_RESUME);
    fst(cpu)->fpcr_enable = EXC_DZ;
    fst(cpu)->fp[0] = FIVE;
    RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    expect_pre(&c, cpu, VEC_DZ, RESET_PC + 2);
    assert(same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);

    /* opclass 010 の frame（ソースは ETEMP）も完了させる。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_IN(FMT_X, 0, 0x22), 0, 0, 0, TWO, ONE, CU_RESUME);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[0], THREE));
    mc68k_destroy(cpu);

    /* unimplemented frame（$30）は本文を busy frame として解釈しない。本文の `$08` を CU_SAVEPC
     * に見立てても、CMDREG1B = 0 の FMOVE FP0,FP0 として実行されない。 */
    cpu = make_a0(&c);
    wl(&c, SAVE_AREA, 0x41300000u);
    wl(&c, SAVE_AREA + 0x08, 0x01000000u);
    fst(cpu)->fp[0] = FIVE;
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[0], FIVE) && !cpu->fpu040->exc.valid && cpu->fpu040->idle);
    mc68k_destroy(cpu);

    /* CU_SAVEPC は 0 かどうかだけを見る（奇数・上位 bit だけ・$FE 以外のどれでも完了する）。 */
    static const uint8_t cu_values[] = {0x01, 0x02, 0x40, 0x80, 0xFF};
    for (unsigned i = 0; i < sizeof(cu_values) / sizeof(cu_values[0]); i++) {
        cpu = make_a0(&c);
        put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), 0, 0, 0, TWO, ONE, cu_values[i]);
        RUN(&c, cpu, FRESTORE_A0);
        assert(same(fst(cpu)->fp[0], THREE));
        mc68k_destroy(cpu);
    }
    assert(sizeof(cu_values) / sizeof(cu_values[0]) == 5);

    /* ETE15 でも仮数が 0 のオペランドは 0（符号つき）として実行する。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), 0, 0, 0, fp(0x7FC1, 0), ONE, CU_RESUME);
    wl(&c, SAVE_AREA + 0x3C, TAG_E15);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[0], ONE) && fst(cpu)->fpsr_exc == 0);
    mc68k_destroy(cpu);

    /* ソースレジスタは 0〜7 のどれでも（FP3/FP7 が packed や FMOVECR
     * と取り違えられない）。CU_SAVEPC は $FE 以外でも完了させる。 */
    for (unsigned src = 0; src < 8; src++) {
        cpu = make_a0(&c);
        put_busy(&c, SAVE_AREA, EXT_RR(src, 0, 0x22), 0, 0, 0, TWO, ONE,
                 src == 6 ? 0x06 : CU_RESUME);
        RUN(&c, cpu, FRESTORE_A0);
        assert(same(fst(cpu)->fp[0], THREE) && !cpu->fpu040->exc.valid);
        mc68k_destroy(cpu);
    }

    /* FPSP の標準の完了 frame は S/X flag（sx_mask $01800000）を E_BYTE に持つ（gen_except.sa
     * の do_check）。例外状態と取り違えない。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), 0x01800000u, 0, 0, TWO, ONE, CU_RESUME);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[0], THREE) && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);

    /* 強制精度の FDSQRT（CMDREG1B の opmode $45）は double に丸める。FSQRT
     * の写像（$5）と取り違えない。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 2, 0x45), 0, 0, 0, TWO, ZERO, CU_RESUME);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[2], fp(0x3FFF, 0xB504F333F9DE6800ull)));
    mc68k_destroy(cpu);

    /* FSQRT は仮数の下位まで正しい（sqrt(2)）。INEX2 も FPSR に立つ。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 2, 0x05), 0, 0, 0, TWO, ZERO, CU_RESUME);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[2], fp(0x3FFF, 0xB504F333F9DE6484ull)) &&
           (fst(cpu)->fpsr_exc & EXC_INEX2));
    mc68k_destroy(cpu);

    /* 宛先を読まない命令（FSQRT）は、FPTEMP が未対応データ型の値でも検査しない。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 2, 0x05), 0, 0, 0, FOUR, fp(0x0000, 1), CU_RESUME);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[2], TWO) && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);

    /* 宛先ごとに FPTEMP から宛先の operand を取り、CMDREG1B の bits 9-7 へ書く。 */
    for (unsigned dst = 0; dst < 8; dst++) {
        cpu = make_a0(&c);
        put_busy(&c, SAVE_AREA, EXT_RR(1, dst, 0x22), 0, 0, 0, TWO, ONE, CU_RESUME);
        for (unsigned r = 0; r < 8; r++)
            fst(cpu)->fp[r] = FIVE;
        RUN(&c, cpu, FRESTORE_A0);
        for (unsigned r = 0; r < 8; r++)
            assert(same(fst(cpu)->fp[r], r == dst ? THREE : FIVE));
        mc68k_destroy(cpu);
    }

    /* 何もしない frame: CU_SAVEPC = 0（CU に命令が無い。E3 を消した frame
     * もここ。D-B1）、未実装の演算（FSIN、FINT）、 FMOVECR（opclass 010 の source specifier
     * 7）、opclass 011、命令を持たない E1/T の frame（FPSR に EXC が無い）。 */
    static const struct {
        uint16_t cmd;
        uint32_t flags;
        uint8_t cu;
    } nop[] = {
        {EXT_RR(1, 0, 0x22), 0, 0},
        {EXT_RR(1, 0, 0x0E), 0, CU_RESUME},
        {EXT_RR(1, 0, 0x01), 0, CU_RESUME},
        {EXT_IN(7, 0, 0x00), 0, CU_RESUME},
        {EXT_OUT(FMT_S, 1), 0, CU_RESUME},
        {(uint16_t)(0x2000 | 1 << 10 | 0x22), 0, CU_RESUME},
        {EXT_RR(1, 0, 0x22), FLAG_E1, CU_RESUME},
        {EXT_RR(1, 0, 0x00), FLAG_E1 | FLAG_T, CU_RESUME},
        {0, 0, 0},
        {0, FLAG_E1, 0},
    };
    for (unsigned i = 0; i < sizeof(nop) / sizeof(nop[0]); i++) {
        cpu = make_a0(&c);
        put_busy(&c, SAVE_AREA, nop[i].cmd, nop[i].flags, 0, 0, TWO, THREE, nop[i].cu);
        wl(&c, SAVE_AREA + 0x08,
           rl(&c, SAVE_AREA + 0x08) | 0x00800000u); /* FP7 の dirty bit は CU_SAVEPC ではない */
        for (unsigned r = 0; r < 8; r++)
            fst(cpu)->fp[r] = FIVE;
        fst(cpu)->fpsr_cc =
            MC68K_FPU_CC_Z; /* 命令を実行すると（FTST 相当でも）条件コードが変わる */
        RUN(&c, cpu, FRESTORE_A0);
        for (unsigned r = 0; r < 8; r++)
            assert(same(fst(cpu)->fp[r], FIVE));
        assert(fst(cpu)->fpsr_cc == MC68K_FPU_CC_Z);
        assert(!cpu->fpu040->exc.valid && cpu->fpu040->idle);
        mc68k_destroy(cpu);
    }
    assert(sizeof(nop) / sizeof(nop[0]) == 10);

    /* D-B3: FPSP が正規化して負の指数にしたオペランド（ETE15/FPTE15 = 1 かつ指数 bit 14 =
     * 1）。7FC1 は 16 bit で -63（$7FC1 - $8000）。巨大な値（2^16321
     * 倍）として足してはいけない。丸めモード RP で、消える端数の有無 （非 0 なら 1 ulp
     * 上がる）も確かめる。 */
    static const uint16_t tiny_exps[] = {0x7FFF, 0x7FC1,
                                         0x4000}; /* 右シフト 1、63、16384（>= 64） */
    assert(sizeof(tiny_exps) / sizeof(tiny_exps[0]) == 3);
    for (unsigned i = 0; i < sizeof(tiny_exps) / sizeof(tiny_exps[0]); i++) {
        for (unsigned side = 0; side < 2; side++) { /* ソースが小さい／宛先が小さい */
            cpu = make_a0(&c);
            const mc68k_fp80_t tiny = fp(tiny_exps[i], 0x8000000000000000ull);
            put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), 0, 0, 0, side ? ONE : tiny,
                     side ? tiny : ONE, CU_RESUME);
            wl(&c, SAVE_AREA + (side ? 0x44 : 0x3C), TAG_E15);
            fst(cpu)->fpcr_round_mode = MC68K_FPU_ROUND_PLUS_INF;
            RUN(&c, cpu, FRESTORE_A0);
            assert(same(fst(cpu)->fp[0], fp(0x3FFF, 0x8000000000000001ull)) &&
                   (fst(cpu)->fpsr_exc & EXC_INEX2));
            mc68k_destroy(cpu);
        }
    }
    /* 正の指数（bit 14 = 0）は ETE15 があっても通常の値。FADD 1.0 + 1.0。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), 0, 0, 0, ONE, ONE, CU_RESUME);
    wl(&c, SAVE_AREA + 0x3C, TAG_E15);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[0], TWO));
    mc68k_destroy(cpu);

    /* 右シフト量・符号・消える端数の扱い: 非正規化数どうしを FCMP して、同じ値なら Z が立つ。
     * ソース（ETE15）= {exp, J|extra} を、宛先（ETE15 なし。指数 0 の非正規化数）と比べる。 */
    static const struct {
        uint16_t exp;
        uint64_t sig;
        mc68k_fp80_t dst;
    } cmp[] = {
        {0x7FFF, 0x8000000000000000ull, {0x0000, 0x4000000000000000ull}}, /* 右へ 1 */
        {0x7FFE, 0x8000000000000000ull, {0x0000, 0x2000000000000000ull}}, /* 右へ 2 */
        {0x7FC1, 0x8000000000000000ull, {0x0000, 0x0000000000000001ull}}, /* 右へ 63 */
        {0x7FC0,
         0x8000000000000000ull,
         {0x0000, 0x0000000000000001ull}}, /* 右へ 64: 全部消えて非 0 → 最小値 */
        {0x4000, 0xFFFFFFFFFFFFFFFFull, {0x0000, 0x0000000000000001ull}}, /* 右へ 16384 */
        {0x7FFF,
         0x8000000000000001ull,
         {0x0000, 0x4000000000000000ull}}, /* 右へ 1: 消えた端数は仮数を変えない */
        {0xFFFF,
         0x8000000000000000ull,
         {0x8000, 0x4000000000000000ull}}, /* 負数: 符号が保たれる */
        {0x0000,
         0x0000000000000001ull,
         {0x0000, 0x0000000000000001ull}}, /* 指数 bit 14 = 0: 値のまま、検査もしない */
        /* 整数部が 0 の仮数が全部消えるとき: 非 0 なら最小値、0 なら 0 のまま */
        {0x7FFF,
         0x0000000000000001ull,
         {0x0000, 0x0000000000000001ull}}, /* 右へ 1 で 0 になる非 0 */
        {0x7FFE, 0x0000000000000002ull, {0x0000, 0x0000000000000001ull}}, /* 右へ 2（偶数） */
        {0x7FC1,
         0x7FFFFFFFFFFFFFFFull,
         {0x0000, 0x0000000000000001ull}}, /* 右へ 63 で 0 になる */
        {0x4000,
         0x0000000000000000ull,
         {0x0000, 0x0000000000000000ull}}, /* 右へ 16384: 0 は 0 のまま */
        {0x7FFF,
         0x0000000000000000ull,
         {0x0000, 0x0000000000000000ull}}, /* 右へ 1: 0 は 0 のまま */
    };
    for (unsigned i = 0; i < sizeof(cmp) / sizeof(cmp[0]); i++) {
        cpu = make_a0(&c);
        put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x38), 0, 0, 0, fp(cmp[i].exp, cmp[i].sig),
                 cmp[i].dst, CU_RESUME);
        wl(&c, SAVE_AREA + 0x3C, TAG_E15);
        wl(&c, SAVE_AREA + 0x44,
           TAG_E15); /* 宛先は指数 bit 14 = 0
                        なので通常の値のまま（未対応データ型の検査もしない） */
        fst(cpu)->fpsr_cc = 0;
        RUN(&c, cpu, FRESTORE_A0);
        assert(fst(cpu)->fpsr_cc & MC68K_FPU_CC_Z);
        assert(!cpu->fpu040->exc.valid);
        mc68k_destroy(cpu);
    }
    assert(sizeof(cmp) / sizeof(cmp[0]) == 13);

    /* 完了の再実行でオペランドがまだ未対応データ型（タグは normalized のまま）:
     * 未対応データ型を保留に積み直す（E1 = 1）。 packed のソース形式も同じ。 */
    static const struct {
        uint16_t cmd;
        mc68k_fp80_t etemp, fptemp;
    } still_unsupported[] = {
        {EXT_RR(1, 0, 0x22), {0x0000, 1}, {0x3FFF, 0x8000000000000000ull}},
        {EXT_RR(1, 0, 0x22), {0x3FFF, 0x8000000000000000ull}, {0x0000, 1}},
        {EXT_IN(FMT_P, 0, 0x22),
         {0x3FFF, 0x8000000000000000ull},
         {0x3FFF, 0x8000000000000000ull}},
        {EXT_RR(1, 0, 0x62),
         {0x3FFF, 0x8000000000000000ull},
         {0x0000, 1}}, /* 強制精度の FSADD も宛先を検査する */
    };
    for (unsigned i = 0; i < sizeof(still_unsupported) / sizeof(still_unsupported[0]); i++) {
        cpu = make_a0(&c);
        handler_save(&c, VEC_UNSUPPORTED);
        put_busy(&c, SAVE_AREA, still_unsupported[i].cmd, 0, 0, 0, still_unsupported[i].etemp,
                 still_unsupported[i].fptemp, CU_RESUME);
        fst(cpu)->fp[0] = FIVE;
        RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
        expect_pre_saved(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 2);
        assert(at(&c, 0x48) == FLAG_E1 && at(&c, 0x38) == NMN_UNSUP &&
               same(fst(cpu)->fp[0], FIVE));
        memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
        mc68k_destroy(cpu);
        roundtrip(&c, saved, 0, 0,
                  VEC_UNSUPPORTED); /* E-B3: 積み直した状態も、もう一度の往復で失われない */
    }
    assert(sizeof(still_unsupported) / sizeof(still_unsupported[0]) == 4);

    cpu = make_a0(&c); /* ETE15 が無ければ 2^(0x7FC1-0x3FFF) の巨大な値 */
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), 0, 0, 0, fp(0x7FC1, 0x8000000000000000ull), ONE,
             CU_RESUME);
    RUN(&c, cpu, FRESTORE_A0);
    assert(same(fst(cpu)->fp[0], fp(0x7FC1, 0x8000000000000000ull)));
    mc68k_destroy(cpu);
}

/* D-B1: [040UM] §9.7.4/9.7.5/9.7.7 の手順（handler が FSAVE し、frame の E3 を消して FRESTORE
 * する）で、 同じ例外を繰り返さない。E3 = 0・CU_SAVEPC = 0 の frame は何もしない。 */
static void test_restore_after_clearing_e3(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    const uint16_t handler[] = {
        0xF310, 0x08A8,      0x0001,
        0x0048, FRESTORE_A0, 0x4E73}; /* FSAVE (A0); BCLR #1,$48(A0); FRESTORE (A0); RTE */

    for (unsigned i = 0; i < sizeof(handler) / sizeof(handler[0]); i++)
        ww(&c, HANDLER(VEC_OVFL) + 2 * i, handler[i]);
    fst(cpu)->fp[0] = fp(0x7FFE, 0xFFFFFFFFFFFFFFFFull);
    fst(cpu)->fp[1] = fp(0x7FFE, 0xFFFFFFFFFFFFFFFFull);
    const uint32_t end = RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x22), REPORT_NOP_0, REPORT_NOP_1);
    assert(cpu->pc == end && cpu->a[7] == RESET_SP && !cpu->fpu040->exc.valid);
    assert(
        same(fst(cpu)->fp[0], fp(0x7FFE, 0xFFFFFFFFFFFFFFFFull))); /* OVFL は宛先を書かない */
    mc68k_destroy(cpu);
}

/* 未対応データ型・算術例外・post-instruction の frame を FSAVE → FRESTORE
 * で往復して、保留が失われず frame も 同じになる（D-N7、D-B2）。vector は frame
 * に無いので、FPSR の EXC と FPCR の ENABLE から決める。 */
static void roundtrip(ctx_t *c, const uint8_t *saved, uint8_t fpsr_exc, uint8_t enable,
                      unsigned vector) {
    mc68k_state_t *cpu = make_a0(c);

    handler_save(c, vector);
    memcpy(&c->ram[SAVE_AREA], saved, 100);
    fst(cpu)->fp[0] = FIVE;
    fst(cpu)->fpsr_exc = fpsr_exc;
    fst(cpu)->fpcr_enable = enable;
    /* FRESTORE は FPIAR を復元しない（OS が FMOVE で戻す。推論 23）。FSAVE が書く FPIARCU を
     * frame に揃える */
    fst(cpu)->fpiar = (uint32_t)saved[0x28] << 24 | (uint32_t)saved[0x29] << 16 |
                      (uint32_t)saved[0x2A] << 8 | saved[0x2B];
    RUN(c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    expect_pre_saved(c, cpu, vector, RESET_PC + 2);
    assert(memcmp(&c->ram[SAVE_AREA], saved, 100) == 0); /* 再び FSAVE した frame が元と同じ */
    assert(same(fst(cpu)->fp[0], FIVE));                 /* 宛先は書かない */
    mc68k_destroy(cpu);
}

static void test_restore_exception_state(void) {
    ctx_t c;
    mc68k_state_t *cpu;
    uint8_t saved[100];
    const mc68k_fp80_t denorm = {0x0000, 0x0000000000000001ull}, unnorm = {0x3FFF, 1};
    const struct {
        uint16_t ext;
        mc68k_fp80_t dst, src;
        bool memory_single_denorm; /* FADD.S (A1) の single denormal（STAG = 5）で作る */
    } cases[] = {
        {EXT_RR(1, 0, 0x22), ONE, denorm, false},  /* ソースが extended の denormal */
        {EXT_RR(1, 0, 0x22), ONE, unnorm, false},  /* ソースが unnormalized */
        {EXT_RR(1, 0, 0x22), denorm, ONE, false},  /* 宛先が denormal（DTAG = 4） */
        {EXT_RR(1, 0, 0x04), ZERO, denorm, false}, /* FSQRT: CMDREG1B の opmode は $5 */
        {EXT_RR(1, 0, 0x62), denorm, ONE, false},  /* 強制精度の FSADD */
        {EXT_IN(FMT_S, 0, 0x22), ONE, ZERO,
         true}, /* single の denormal（STAG = 5、図 9-9 の形） */
        {EXT_RR(1, 0, 0x00), ZERO, denorm,
         false}, /* CMDREG1B = 0（FMOVE FP1,FP0）。BSUN の frame と見分けがつく */
        {EXT_RR(1, 0, 0x00), ZERO, unnorm, false},
    };
    assert(sizeof(cases) / sizeof(cases[0]) == 8);

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cpu = make_a0(&c);
        handler_save(&c, VEC_UNSUPPORTED);
        fst(cpu)->fp[0] = cases[i].dst;
        fst(cpu)->fp[1] = cases[i].src;
        if (cases[i].memory_single_denorm) {
            wl(&c, DATA_AREA, 0x00400000u);
            cpu->a[1] = DATA_AREA;
            RUN(&c, cpu, 0xF211, cases[i].ext, REPORT_NOP_0, REPORT_NOP_1);
            assert(at(&c, 0x3C) == (uint32_t)5 << 29);
        } else {
            RUN(&c, cpu, 0xF200, cases[i].ext, REPORT_NOP_0, REPORT_NOP_1);
        }
        memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
        assert(at(&c, 0) == 0x41600000u);
        mc68k_destroy(cpu);
        roundtrip(&c, saved, 0, 0, VEC_UNSUPPORTED);
    }

    /* packed のソース: ETM[63-0]・FPTM[31-0] を含めて保たれる。ETEMP 自体は正常な値で、packed
     * かどうかは CMDREG1B のソース形式から決まる。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_IN(FMT_P, 0, 0x22), FLAG_E1, 0, 0,
             fp(0x4000, 0x8000000055667788ull), fp(0x4000, 0x80000000AABBCCDDull), 0);
    wl(&c, SAVE_AREA + 0x38,
       NMN_UNSUP); /* 未対応データ型の frame は NMNEXC の unsup bit を持つ（推論 25） */
    handler_save(&c, VEC_UNSUPPORTED);
    RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    expect_pre_saved(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 2);
    assert(at(&c, 0x5C) == 0x80000000u && at(&c, 0x60) == 0x55667788u &&
           at(&c, 0x54) == 0xAABBCCDDu);
    assert(at(&c, 0x4C) == 0x40000000u && at(&c, 0x50) == 0x80000000u);
    assert(at(&c, 0x40) == ((uint32_t)EXT_IN(FMT_P, 0, 0x22) << 16));
    mc68k_destroy(cpu);

    /* E3 の frame（OVFL、UNFL、INEX2）: FSAVE の frame を FRESTORE
     * すると、WBTEMP・CMDREG3B・guard/round/sticky・ dirty bit も同じ frame
     * として戻り、宛先は書かない（D-B2）。FPSR の EXC は FPSP が frestore の前に戻す。 */
    static const struct {
        unsigned vector;
        uint16_t ext;
        mc68k_fp80_t dst, src;
        uint8_t enable, exc;
    } e3[] = {
        {VEC_OVFL,
         EXT_RR(1, 0, 0x22),
         {0x7FFE, 0xFFFFFFFFFFFFFFFFull},
         {0x7FFE, 0xFFFFFFFFFFFFFFFFull},
         0,
         EXC_OVFL},
        {VEC_UNFL,
         EXT_RR(1, 0, 0x23),
         {0x0001, 0x8000000000000000ull},
         {0xBFFD, 0x8000000000000000ull},
         0,
         EXC_UNFL},
        {VEC_INEX,
         EXT_RR(1, 0, 0x22),
         {0x3FFF, 0x8000000000000000ull},
         {0x3FFF - 65, 0x8000000000000001ull},
         EXC_INEX2,
         EXC_INEX2},
        {VEC_INEX,
         EXT_RR(1, 0, 0x04),
         {0, 0},
         {0x4000, 0x8000000000000000ull},
         EXC_INEX2,
         EXC_INEX2},
        {VEC_INEX,
         EXT_RR(1, 0, 0x22),
         {0x3FFF, 0x8000000000000000ull},
         {0x3FFF - 64, 0x8000000000000000ull},
         EXC_INEX2,
         EXC_INEX2}, /* guard = 1 */
        {VEC_INEX,
         EXT_RR(1, 0, 0x22),
         {0x3FFF, 0x8000000000000000ull},
         {0x3FFF - 66, 0x8000000000000000ull},
         EXC_INEX2,
         EXC_INEX2}, /* round = 0、sticky = 1 */
        {VEC_INEX,
         EXT_RR(1, 0, 0x22),
         {0x3FFF, 0x8000000000000000ull},
         {0x3FFF - 65, 0x8000000000000000ull},
         EXC_INEX2,
         EXC_INEX2}, /* round = 1、sticky = 0 */
        /* 結果が負（WBTS = 1）。WBTE の指数が偶数・奇数の両方 */
        {VEC_OVFL,
         EXT_RR(1, 0, 0x22),
         {0xFFFE, 0xFFFFFFFFFFFFFFFFull},
         {0xFFFE, 0xFFFFFFFFFFFFFFFFull},
         0,
         EXC_OVFL},
        {VEC_UNFL,
         EXT_RR(1, 0, 0x23),
         {0x0001, 0x8000000000000000ull},
         {0xBFFD, 0x8000000000000000ull},
         0,
         EXC_UNFL},
        {VEC_INEX,
         EXT_RR(1, 0, 0x22),
         {0xC000, 0x8000000000000000ull},
         {0x4000 - 65, 0x8000000000000001ull},
         EXC_INEX2,
         EXC_INEX2}, /* -2.0 に微小な値を足す（指数が偶数） */
        {VEC_INEX,
         EXT_RR(1, 0, 0x22),
         {0xBFFF, 0x8000000000000000ull},
         {0x3FFF - 65, 0x8000000000000001ull},
         EXC_INEX2,
         EXC_INEX2}, /* -1.0 に微小な値を足す（指数が奇数） */
    };
    for (unsigned i = 0; i < sizeof(e3) / sizeof(e3[0]); i++) {
        cpu = make_a0(&c);
        handler_save(&c, e3[i].vector);
        fst(cpu)->fp[0] = e3[i].dst;
        fst(cpu)->fp[1] = e3[i].src;
        fst(cpu)->fpcr_enable = e3[i].enable;
        RUN(&c, cpu, 0xF200, e3[i].ext, REPORT_NOP_0, REPORT_NOP_1);
        assert(at(&c, 0x48) == E3_ONLY);
        memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
        mc68k_destroy(cpu);
        roundtrip(&c, saved, e3[i].exc, e3[i].enable, e3[i].vector);
    }
    assert(sizeof(e3) / sizeof(e3[0]) == 11);

    /* E1 の算術例外（DZ）と、post-instruction（T = 1）の OPERR。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_DZ);
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x20), REPORT_NOP_0, REPORT_NOP_1);
    memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
    mc68k_destroy(cpu);
    roundtrip(&c, saved, EXC_DZ, EXC_DZ, VEC_DZ);

    cpu = make_a0(&c);
    handler_save(&c, VEC_OPERR);
    fst(cpu)->fp[0] = fp(0x401F, 0x8000000000000000ull);
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, EXT_OUT(FMT_L, 0));
    assert(at(&c, 0x48) == E1_T);
    memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
    mc68k_destroy(cpu);
    roundtrip(&c, saved, EXC_OPERR, 0, VEC_OPERR); /* 整数宛ての OPERR は nonmaskable */

    /* post-instruction の未対応データ型（T = 1）: packed と P{Dn} の FMOVE out。 */
    static const uint16_t out_fmts[] = {FMT_P, 7};
    assert(sizeof(out_fmts) / sizeof(out_fmts[0]) == 2);
    for (unsigned i = 0; i < sizeof(out_fmts) / sizeof(out_fmts[0]); i++) {
        cpu = make_a0(&c);
        handler_save(&c, VEC_UNSUPPORTED);
        fst(cpu)->fp[0] = ONE;
        cpu->a[1] = DATA_AREA;
        RUN(&c, cpu, 0xF211, EXT_OUT(out_fmts[i], 0));
        assert(at(&c, 0x48) == E1_T);
        memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
        mc68k_destroy(cpu);
        roundtrip(&c, saved, 0, 0, VEC_UNSUPPORTED);
    }

    /* post-instruction（T = 1）の frame でも、整数宛てでない形式（S/D/X）の SNAN は nonmaskable
     * ではないので、 FPCR の enable が無ければ FRESTORE
     * は例外状態を戻さない。整数宛て（L/W/B）は戻す。 */
    for (unsigned fmt = 0; fmt < 7; fmt++) {
        if (fmt == FMT_P)
            continue;
        cpu = make_a0(&c);
        put_busy(&c, SAVE_AREA, EXT_OUT(fmt, 0), FLAG_T, 0, 0, ONE, ONE, 0);
        fst(cpu)->fpsr_exc = EXC_SNAN;
        RUN(&c, cpu, FRESTORE_A0);
        assert(cpu->fpu040->exc.valid == (fmt == FMT_L || fmt == FMT_W || fmt == FMT_B));
        mc68k_destroy(cpu);
    }

    /* 整数宛て（L/W/B）の OPERR（nonmaskable）の post-instruction frame。 */
    static const uint16_t int_fmts[] = {FMT_L, FMT_W, FMT_B};
    assert(sizeof(int_fmts) / sizeof(int_fmts[0]) == 3);
    for (unsigned i = 0; i < sizeof(int_fmts) / sizeof(int_fmts[0]); i++) {
        cpu = make_a0(&c);
        handler_save(&c, VEC_OPERR);
        fst(cpu)->fp[0] = fp(0x401F, 0x8000000000000000ull);
        cpu->a[1] = DATA_AREA;
        RUN(&c, cpu, 0xF211, EXT_OUT(int_fmts[i], 0));
        assert(at(&c, 0x48) == E1_T);
        memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
        mc68k_destroy(cpu);
        roundtrip(&c, saved, EXC_OPERR, 0, VEC_OPERR);
    }

    /* T = 0 の frame は、整数形式のソースでも SNAN/OPERR を nonmaskable にしない（FPSR に OPERR
     * があっても ENABLE が無ければ戻さない）。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_IN(FMT_L, 0, 0x22), FLAG_E1, 0, 0, TWO, ONE, 0);
    fst(cpu)->fpsr_exc = EXC_OPERR;
    const uint32_t end_l = RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    assert(cpu->pc == end_l && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);

    /* E-B1: ETE15 で完了させた命令の UNFL を往復しても vector は UNFL のまま（タグは 4
     * になるが、未対応データ型ではない）。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_UNFL);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x23), 0, 0, 0, fp(0x7FC2, 0xC000000000000000ull),
             fp(0x3FFE, 0x8000000000000000ull), CU_RESUME);
    wl(&c, SAVE_AREA + 0x3C, TAG_E15);
    RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    expect_pre_saved(&c, cpu, VEC_UNFL, RESET_PC + 2);
    assert(at(&c, 0x38) == 0 && at(&c, 0x48) == E3_ONLY);
    memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
    mc68k_destroy(cpu);
    roundtrip(&c, saved, EXC_UNFL, 0, VEC_UNFL);

    /* E-B2: FPSP が frestore する frame は、タグが denormal のまま UNFL
     * などの例外になる（gen_except.sa の CU_ONLY 経路、 unsE3_1、commonE1）。NMNEXC の unsup
     * bit が無ければ、タグでなく FPSR の EXC と ENABLE で決める。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x00), FLAG_E1, 4, 0, fp(0, 1), fp(0, 1), 0);
    memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
    mc68k_destroy(cpu);
    roundtrip(&c, saved, EXC_UNFL, 0, VEC_UNFL);
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_OUT(FMT_S, 0), FLAG_E3 | FLAG_T, 4, 0, fp(0, 1), fp(0, 1), 0);
    wl(&c, SAVE_AREA + 0x08, 0x00010000u); /* E3 の宛先 FP0 の dirty bit */
    memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
    mc68k_destroy(cpu);
    roundtrip(&c, saved, EXC_UNFL, 0, VEC_UNFL);

    /* 同じ frame に NMNEXC の unsup bit を立てれば、未対応データ型になる。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x00), FLAG_E1, 4, 0, fp(0, 1), fp(0, 1), 0);
    wl(&c, SAVE_AREA + 0x38, NMN_UNSUP);
    memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
    mc68k_destroy(cpu);
    roundtrip(&c, saved, EXC_UNFL, 0, VEC_UNSUPPORTED);

    /* F-B1: `gen_except.sa` の do_build は、unimp→busy 変換の E1 frame の NMCEXC の bits 3-0 に
     * FPSR の SNAN/OPERR/OVFL/UNFL を 写す。これらは未対応データ型ではない（vector は FPSR と
     * ENABLE から決める）。 */
    static const struct {
        uint32_t nmcexc;
        unsigned vector;
        uint8_t exc;
    } nmc_bits[] = {
        {0x00010000u, VEC_UNFL, EXC_UNFL},
        {0x00020000u, VEC_OVFL, EXC_OVFL},
        {0x00040000u, VEC_OPERR, EXC_OPERR},
        {0x00080000u, VEC_SNAN, EXC_SNAN},
    };
    assert(sizeof(nmc_bits) / sizeof(nmc_bits[0]) == 4);
    for (unsigned i = 0; i < sizeof(nmc_bits) / sizeof(nmc_bits[0]); i++) {
        cpu = make_a0(&c);
        put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), FLAG_E1, 0, 0, TWO, ONE, 0);
        wl(&c, SAVE_AREA + 0x38, nmc_bits[i].nmcexc);
        handler_save(&c, nmc_bits[i].vector);
        fst(cpu)->fpsr_exc = nmc_bits[i].exc;
        fst(cpu)->fpcr_enable = nmc_bits[i].exc;
        RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
        expect_pre_saved(&c, cpu, nmc_bits[i].vector, RESET_PC + 2);
        assert(at(&c, 0x38) == 0);
        mc68k_destroy(cpu);
    }

    /* F-B2: 宛先が FPSP に正規化された（FPTE15 = 1）FDIV が完了で UNFL になった frame（DTAG =
     * 4、NMNEXC = 0）は、往復しても UNFL のまま。 */
    cpu = make_a0(&c);
    handler_save(&c, VEC_UNFL);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x20), 0, 0, 0, TWO, fp(0x7FC2, 0xC000000000000000ull),
             CU_RESUME);
    wl(&c, SAVE_AREA + 0x44, TAG_E15);
    RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    expect_pre_saved(&c, cpu, VEC_UNFL, RESET_PC + 2);
    assert(at(&c, 0x38) == 0 && (at(&c, 0x44) >> 29) == 4);
    memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
    mc68k_destroy(cpu);
    roundtrip(&c, saved, EXC_UNFL, 0, VEC_UNFL);

    /* T = 1 で S/D/X 形式の frame に SNAN/OPERR があっても、整数宛てでなければ nonmaskable
     * にしない（ENABLE が無ければ戻さない）。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_OUT(FMT_S, 0), FLAG_E1 | FLAG_T, 0, 0, TWO, ZERO, 0);
    fst(cpu)->fpsr_exc = EXC_SNAN | EXC_OPERR;
    const uint32_t end_s = RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    assert(cpu->pc == end_s && !cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);

    /* NMCEXC の nmc_unsup_bit（`$39` の bit 4）だけでも未対応データ型。再度の FSAVE は NMNEXC
     * の unsup bit で書く。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x00), FLAG_E1, 4, 0, fp(0, 1), fp(0, 1), 0);
    wl(&c, SAVE_AREA + 0x38, 0x00100000u);
    handler_save(&c, VEC_UNSUPPORTED);
    RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    expect_pre_saved(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 2);
    assert(at(&c, 0x38) == NMN_UNSUP);
    mc68k_destroy(cpu);

    /* ソースレジスタが FP1 以外（FP2〜FP7）の DZ の frame。 */
    for (unsigned src = 2; src < 8; src++) {
        cpu = make_a0(&c);
        handler_save(&c, VEC_DZ);
        fst(cpu)->fp[0] = ONE;
        fst(cpu)->fp[src] = ZERO;
        fst(cpu)->fpcr_enable = EXC_DZ;
        RUN(&c, cpu, 0xF200, EXT_RR(src, 0, 0x20), REPORT_NOP_0, REPORT_NOP_1);
        memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
        mc68k_destroy(cpu);
        roundtrip(&c, saved, EXC_DZ, EXC_DZ, VEC_DZ);
    }

    /* T だけが立つ frame（E1 なし）の整数宛て OPERR は nonmaskable。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_OUT(FMT_L, 0), FLAG_T, 0, 0, TWO, ZERO, 0);
    fst(cpu)->fpsr_exc = EXC_OPERR;
    RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    expect_pre(&c, cpu, VEC_OPERR, RESET_PC + 2);
    mc68k_destroy(cpu);

    /* BSUN の frame（CMDREG1B = 0、ETEMP = 0）: FPSR の BSUN と ENABLE から戻す。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, 0, FLAG_E1, 0, 0, ZERO, ZERO, 0);
    memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
    mc68k_destroy(cpu);
    roundtrip(&c, saved, EXC_BSUN, EXC_BSUN, VEC_BSUN);

    /* CMDREG1B = 0 の算術例外（FMOVE FP0,FP0 の INEX2）も BSUN と取り違えない。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, 0, FLAG_E1, 0, 0, fp(0x3FFF, 0x8000000200000000ull), ZERO, 0);
    memcpy(saved, &c.ram[SAVE_AREA], sizeof(saved));
    mc68k_destroy(cpu);
    roundtrip(&c, saved, EXC_INEX2, EXC_INEX2, VEC_INEX);

    /* 例外状態を決められない frame（FPSR に報告できる EXC が無い）は何も戻さない。E3 だけの
     * frame も同じ。 */
    cpu = make_a0(&c);
    put_busy(&c, SAVE_AREA, EXT_RR(1, 0, 0x22), FLAG_E3, 0, 0, TWO, ONE, 0);
    fst(cpu)->fp[0] = FIVE;
    const uint32_t end = RUN(&c, cpu, FRESTORE_A0, REPORT_NOP_0, REPORT_NOP_1);
    assert(cpu->pc == end && !cpu->fpu040->exc.valid && same(fst(cpu)->fp[0], FIVE));
    mc68k_destroy(cpu);
}

/* FMOVE.P FP0,(A1) の P{Dn}（形式 7）の frame: T = 1、CMDREG1B = 命令の拡張語。 */
static void test_packed_dynamic_frame(void) {
    ctx_t c;
    mc68k_state_t *cpu = make_a0(&c);
    handler_save(&c, VEC_UNSUPPORTED);
    fst(cpu)->fp[0] = TWO;
    cpu->a[1] = DATA_AREA;
    RUN(&c, cpu, 0xF211, 0x7C10);
    assert(at(&c, 0x48) == 0x04100000u && at(&c, 0x40) == 0x7C100000u);
    assert(at(&c, 0x58) == 0x40000000u && at(&c, 0x5C) == 0x80000000u);
    mc68k_destroy(cpu);
}

/* FMOVE.P FP0,(A1){#k}（形式 3）の frame の CMDREG1B は拡張語のまま。k-factor = 4 が FSQRT の
 * opmode と 同じ値でも $5 に写らない（#84）。 */
static void test_packed_static_kfactor_frame(void) {
    for (unsigned k = 0; k < 0x80; k++) {
        ctx_t c;
        mc68k_state_t *cpu = make_a0(&c);
        const uint16_t ext = (uint16_t)(0x6C00u | k);
        handler_save(&c, VEC_UNSUPPORTED);
        fst(cpu)->fp[0] = TWO;
        cpu->a[1] = DATA_AREA;
        RUN(&c, cpu, 0xF211, ext);
        assert(at(&c, 0x40) == (uint32_t)ext << 16);
        mc68k_destroy(cpu);
    }
}

/* FSAVE/FRESTORE のアドレッシングモード: busy/unimplemented frame の
 * -(An)、(d16,An)、(d16,PC)、 書込み失敗時の An と例外状態。 */
static void test_save_restore_addressing(void) {
    ctx_t c;
    mc68k_state_t *cpu;

    /* handler = FSAVE -(A0); BRA.S *: busy frame（先頭 long を含めて 100 バイト）。 */
    cpu = make(&c);
    cpu->a[0] = SAVE_AREA + 0x200;
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = ZERO;
    fst(cpu)->fpcr_enable = EXC_DZ;
    ww(&c, HANDLER(VEC_DZ), 0xF320);
    ww(&c, HANDLER(VEC_DZ) + 2, 0x60FE);
    RUN(&c, cpu, 0xF200, 0x0420, 0xF280, 0x0000);
    assert(cpu->a[0] == SAVE_AREA + 0x200 - 100 && rl(&c, cpu->a[0]) == 0x41600000u);
    assert(rl(&c, cpu->a[0] + 0x3C) == (uint32_t)1 << 29); /* STAG */
    mc68k_destroy(cpu);

    /* unimplemented frame（52 バイト）。 */
    cpu = make(&c);
    cpu->a[0] = SAVE_AREA + 0x200;
    ww(&c, HANDLER(VEC_FLINE), 0xF320);
    ww(&c, HANDLER(VEC_FLINE) + 2, 0x60FE);
    RUN(&c, cpu, 0xF200, 0x050E);
    assert(cpu->a[0] == SAVE_AREA + 0x200 - 52 && rl(&c, cpu->a[0]) == 0x41300000u);
    mc68k_destroy(cpu);

    /* FSAVE (d16,A0) と FRESTORE (d16,PC)。 */
    cpu = make_a0(&c);
    ww(&c, RESET_PC + 0x200, 0x4100);
    ww(&c, RESET_PC + 0x202, 0x0000);
    RUN(&c, cpu, 0xF37A, 0x01FE, 0xF328, 0x0010); /* FRESTORE (d16,PC) → idle; FSAVE 0x10(A0) */
    assert(cpu->a[0] == SAVE_AREA && at(&c, 0x10) == 0x41000000u);
    mc68k_destroy(cpu);

    /* 書込み失敗: A0 は戻り、例外状態は残る。 */
    cpu = make(&c);
    cpu->a[0] = RAM_SIZE + 0x20;
    cpu->fpu040->exc.valid = true;
    cpu->fpu040->exc.vector = VEC_DZ;
    RUN(&c, cpu, 0xF320);
    assert(cpu->a[0] == RAM_SIZE + 0x20 && cpu->fpu040->exc.valid);
    mc68k_destroy(cpu);
}

/* ---- 強制精度命令 ---- */

#define THIRD_EXT 0x3FFD
static void test_forced_precision(void) {
    static const struct {
        uint16_t opmode;
        uint16_t se;
        uint64_t sig;
    } div[] = {
        {0x20, THIRD_EXT, 0xAAAAAAAAAAAAAAABull}, /* FDIV: 拡張 */
        {0x60, THIRD_EXT, 0xAAAAAB0000000000ull}, /* FSDIV: single */
        {0x64, THIRD_EXT, 0xAAAAAAAAAAAAA800ull}, /* FDDIV: double */
    };
    for (unsigned i = 0; i < 3; i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = ONE;
        fst(cpu)->fp[1] = THREE;
        const uint32_t end = RUN(&c, cpu, 0xF200, EXT_RR(1, 0, div[i].opmode));
        assert(cpu->pc == end && same(fst(cpu)->fp[0], fp(div[i].se, div[i].sig)));
        assert(fst(cpu)->fpcr_round_prec == MC68K_FPU_PREC_EXTENDED); /* FPCR は変わらない */
        mc68k_destroy(cpu);
    }

    /* PREC=single の FPCR でも、FDADD は double で丸める（命令が精度を決める）。1 + 2^-30 は
     * single では 1 に丸まり、double では残る。 */
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fst(cpu)->fpcr_round_prec = MC68K_FPU_PREC_SINGLE;
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = fp(0x3FE1, 0x8000000000000000ull); /* 2^-30 */
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x66));
    assert(same(fst(cpu)->fp[0], fp(0x3FFF, 0x8000000200000000ull)) &&
           fst(cpu)->fpcr_round_prec == MC68K_FPU_PREC_SINGLE);
    mc68k_destroy(cpu);
    /* 対照: 強制精度なしの FADD は FPCR の single で丸めるので 1 のまま。 */
    cpu = make(&c);
    fst(cpu)->fpcr_round_prec = MC68K_FPU_PREC_SINGLE;
    fst(cpu)->fp[0] = ONE;
    fst(cpu)->fp[1] = fp(0x3FE1, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x22));
    assert(same(fst(cpu)->fp[0], ONE));
    mc68k_destroy(cpu);

    /* 基本命令の対応: 各強制精度 opmode
     * が基本演算を行う（FSMOVE/FDMOVE/FSSQRT/FDSQRT/FSABS/FDNEG/FSADD/FDMUL/FSSUB）。 */
    static const struct {
        uint16_t opmode;
        mc68k_fp80_t dst, src, expect;
    } ops[] = {
        {0x40, {0, 0}, {0x4001, 0xA000000000000000ull}, {0x4001, 0xA000000000000000ull}},
        {0x44, {0, 0}, {0x4001, 0xA000000000000000ull}, {0x4001, 0xA000000000000000ull}},
        {0x41, {0, 0}, {0x4001, 0x8000000000000000ull}, {0x4000, 0x8000000000000000ull}},
        {0x45, {0, 0}, {0x4001, 0x8000000000000000ull}, {0x4000, 0x8000000000000000ull}},
        {0x58, {0, 0}, {0xBFFF, 0x8000000000000000ull}, {0x3FFF, 0x8000000000000000ull}},
        {0x58, {0, 0}, {0x3FFF, 0x8000000000000000ull}, {0x3FFF, 0x8000000000000000ull}},
        {0x5C, {0, 0}, {0xBFFF, 0x8000000000000000ull}, {0x3FFF, 0x8000000000000000ull}},
        {0x5C, {0, 0}, {0x3FFF, 0x8000000000000000ull}, {0x3FFF, 0x8000000000000000ull}},
        /* √2 は single と double で丸めた仮数が異なる（拡張は B504F333F9DE6484）。 */
        {0x41, {0, 0}, {0x4000, 0x8000000000000000ull}, {0x3FFF, 0xB504F30000000000ull}},
        {0x45, {0, 0}, {0x4000, 0x8000000000000000ull}, {0x3FFF, 0xB504F333F9DE6800ull}},
        {0x5A, {0, 0}, {0x3FFF, 0x8000000000000000ull}, {0xBFFF, 0x8000000000000000ull}},
        {0x5E, {0, 0}, {0x3FFF, 0x8000000000000000ull}, {0xBFFF, 0x8000000000000000ull}},
        {0x62,
         {0x3FFF, 0x8000000000000000ull},
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0xC000000000000000ull}},
        {0x66,
         {0x3FFF, 0x8000000000000000ull},
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0xC000000000000000ull}},
        {0x63,
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0xC000000000000000ull},
         {0x4001, 0xC000000000000000ull}},
        {0x67,
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0xC000000000000000ull},
         {0x4001, 0xC000000000000000ull}},
        {0x68,
         {0x4001, 0xA000000000000000ull},
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0xC000000000000000ull}},
        {0x6C,
         {0x4001, 0xA000000000000000ull},
         {0x4000, 0x8000000000000000ull},
         {0x4000, 0xC000000000000000ull}},
    };
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        cpu = make(&c);
        fst(cpu)->fp[0] = ops[i].dst;
        fst(cpu)->fp[1] = ops[i].src;
        RUN(&c, cpu, 0xF200, EXT_RR(1, 0, ops[i].opmode));
        assert(same(fst(cpu)->fp[0], ops[i].expect));
        mc68k_destroy(cpu);
    }

    /* 強制精度の結果が single の範囲を超えると OVFL（nonmaskable）。拡張精度の FADD
     * なら溢れない。 */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x407E, 0x8000000000000000ull); /* 2^127 */
    fst(cpu)->fp[1] = fp(0x407E, 0x8000000000000000ull);
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x62), 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_OVFL, RESET_PC + 4);
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0x407E, 0x8000000000000000ull);
    fst(cpu)->fp[1] = fp(0x407E, 0x8000000000000000ull);
    const uint32_t end = RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x22), 0xF280, 0x0000);
    assert(cpu->pc == end);
    mc68k_destroy(cpu);

    /* FSMOVE/FDMOVE は PREC = 拡張でも single/double に丸める（FPCR を使わない）。1 + 2^-63
     * は両方で 1.0。 */
    for (unsigned i = 0; i < 2; i++) {
        cpu = make(&c);
        fst(cpu)->fp[1] = fp(0x3FFF, 0x8000000000000001ull);
        RUN(&c, cpu, 0xF200, EXT_RR(1, 0, i ? 0x44 : 0x40));
        assert(same(fst(cpu)->fp[0], ONE));
        mc68k_destroy(cpu);
    }

    /* 宛先を読む強制精度命令は、宛先が denormal なら未対応データ型。FSMOVE は宛先を読まない。
     */
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0, 1);
    fst(cpu)->fp[1] = ONE;
    RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x62), 0xF280, 0x0000);
    expect_pre(&c, cpu, VEC_UNSUPPORTED, RESET_PC + 4);
    mc68k_destroy(cpu);
    cpu = make(&c);
    fst(cpu)->fp[0] = fp(0, 1);
    fst(cpu)->fp[1] = ONE;
    const uint32_t end2 = RUN(&c, cpu, 0xF200, EXT_RR(1, 0, 0x40));
    assert(cpu->pc == end2 && same(fst(cpu)->fp[0], ONE));
    mc68k_destroy(cpu);
}

int main(void) {
    test_null_and_idle();
    test_privilege();
    test_restore();
    test_busy_frame_dz();
    test_busy_frame_operands();
    test_exception_state_persists();
    test_unimplemented_frame();
    test_unsupported_frame();
    test_post_frame();
    test_frame_details();
    test_unimplemented_e1_is_packed();
    test_unimplemented_reports_pending();
    test_unimplemented_state_not_reported();
    test_unrounded_result_in_fptemp();
    test_e3_overflow();
    test_e3_underflow();
    test_e3_inexact();
    test_e3_tags();
    test_e3_cmdreg3b();
    test_e1_stays_e1();
    test_e1_overflow_operand();
    test_restore_completes_instruction();
    test_restore_after_clearing_e3();
    test_restore_exception_state();
    test_packed_dynamic_frame();
    test_packed_static_kfactor_frame();
    test_save_restore_addressing();
    test_forced_precision();
    puts("test_fpu040_frame: OK");
    return 0;
}
