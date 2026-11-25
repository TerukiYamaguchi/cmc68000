/*
 * MC68040 内蔵 FPU の全数・全ビット試験: scripts/fpcp_auto_mutants.py --target
 * fpu4 の生き残りのうち、 レジスタ番号・マスクの各ビット・オペランド幅・EA
 * 分類・バスエラー経路を 1 つずつ突く試験だけを集める。 期待値は production
 * の式の写しでなく、命令の定義（EA の分類、外部形式のバイト数、レジスタ番号）から別に書く。
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
    VEC_BUS_ERROR = 2,
    VEC_FLINE = 11,
    VEC_UNSUPPORTED = 55,
    LAST_VECTOR = 64,
    EA_IMMEDIATE = 074,
    BAD_ADDR = RAM_SIZE + 0x100, /* バスが応答しない */
};

#define HANDLER(v) (HANDLER_BASE + 4u * (v))

enum { FMT_L, FMT_S, FMT_X, FMT_P, FMT_W, FMT_D, FMT_B };
#define EXT_IN(fmt, dst, op) ((uint16_t)(0x4000 | (fmt) << 10 | (dst) << 7 | (op)))
#define EXT_OUT(fmt, src)    ((uint16_t)(0x6000 | (fmt) << 10 | (src) << 7))

typedef struct {
    uint8_t ram[RAM_SIZE];
    unsigned failed_reads; /* バスが応答しなかった読出しの回数（host から観測できる） */
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
    if (a >= RAM_SIZE) {
        c->failed_reads++;
        return false;
    }
    *v = c->ram[a];
    return true;
}
static bool rw_cb(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE) {
        c->failed_reads++;
        return false;
    }
    *v = rw(c, a);
    return true;
}
static bool rl_cb(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    if (a + 3 >= RAM_SIZE) {
        c->failed_reads++;
        return false;
    }
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
static uint32_t run_at(ctx_t *c, mc68k_state_t *cpu, uint32_t pc, const uint16_t *words,
                       unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (pc + 2 * i + 2 <=
            RAM_SIZE) /* RAM の端に置く試験では、RAM の外の語は置けない（取得が失敗する） */
            ww(c, pc + 2 * i, words[i]);
    if (pc + 2 * n + 2 <= RAM_SIZE)
        ww(c, pc + 2 * n, 0x60fe);
    cpu->pc = pc;
    mc68k_execute(cpu, 400);
    return pc + 2 * n;
}

#define RUN(c, cpu, ...)                                          \
    run_at((c), (cpu), RESET_PC, (const uint16_t[]){__VA_ARGS__}, \
           sizeof((const uint16_t[]){__VA_ARGS__}) / 2)

static mc68k_fpu_state_t *fst(mc68k_state_t *cpu) {
    return &cpu->fpu040->state;
}

static mc68k_fp80_t fp(uint16_t sign_exp, uint64_t signif) {
    mc68k_fp80_t v = {sign_exp, signif};
    return v;
}
#define ONE   fp(0x3FFF, 0x8000000000000000ull)
#define TWO   fp(0x4000, 0x8000000000000000ull)
#define THREE fp(0x4000, 0xC000000000000000ull)

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

/* ---- EA の分類（UM の定義から。production の ea_is_*() とは別に書く） ---- */

static bool ea_valid(unsigned ea) {
    return (ea >> 3) < 7 || (ea & 7u) <= 4;
}
static bool ea_data(unsigned ea) {
    return ea_valid(ea) && (ea >> 3) != 1;
}
static bool ea_alterable(unsigned ea) {
    return ea_valid(ea) && !((ea >> 3) == 7 && (ea & 7u) >= 2);
}
static bool ea_data_alterable(unsigned ea) {
    return ea_data(ea) && ea_alterable(ea);
}
static bool ea_control(unsigned ea) {
    const unsigned mode = ea >> 3;
    return mode == 2 || mode == 5 || mode == 6 || (mode == 7 && (ea & 7u) <= 3);
}
static bool ea_control_alterable(unsigned ea) {
    return ea_control(ea) && ea_alterable(ea);
}

static unsigned bits_set(unsigned v) {
    unsigned n = 0;
    for (; v; v >>= 1)
        n += v & 1u;
    return n;
}

/* 拡張語の bits 15-10（opclass と、source/dest format・レジスタ選択・FMOVEM
 * のモード）を全部ふって、 全 EA（0〜63）と全 type の分類が定義どおりか。opmode は実装済みの
 * FADD（0x22）。 */
static fpu040_class_t expected_class(unsigned type, unsigned ea, unsigned top6) {
    const unsigned mode = ea >> 3, opclass = top6 >> 3, fmt = top6 & 7u, regsel = fmt,
                   multi_mode = (top6 >> 1) & 3u; /* 拡張語の bits 12-11 */
    const bool fmt_dn = fmt == FMT_L || fmt == FMT_S || fmt == FMT_W || fmt == FMT_B;
    const unsigned n = bits_set(regsel);
    const bool trapcc = mode == 7 && (ea & 7u) >= 2 && (ea & 7u) <= 4;
#define CLS(b) ((b) ? FPU040_IMPL : FPU040_ILLEGAL)
    switch (type) {
    case 0:
        switch (opclass) {
        case 0:
            return CLS(ea == 0);
        case 2:
            return fmt == 7 ? (ea == 0 ? FPU040_UNIMPL : FPU040_ILLEGAL)
                            : CLS(ea_data(ea) && !(mode == 0 && !fmt_dn));
        case 3:
            return CLS(ea_data_alterable(ea) && !(mode == 0 && !fmt_dn));
        case 4:
            return CLS((mode == 0 && n <= 1) || (mode == 1 && regsel == 1) || ea_control(ea) ||
                       mode == 3 || ea == EA_IMMEDIATE || (mode == 4 && n == 1));
        case 5:
            return CLS((mode == 0 && n <= 1) || (mode == 1 && regsel == 1) ||
                       ea_control_alterable(ea) || mode == 4 || (mode == 3 && n == 1));
        case 6:
            return CLS(multi_mode >= 2 && (ea_control(ea) || mode == 3));
        case 7:
            return CLS(mode == 4 ? multi_mode <= 1
                                 : (multi_mode >= 2 && ea_control_alterable(ea)));
        default:
            return FPU040_ILLEGAL;
        }
    case 1:
        return CLS(mode == 1 || trapcc || ea_data_alterable(ea));
    case 2:
    case 3:
        return FPU040_IMPL;
    case 4:
        return CLS(ea_control_alterable(ea) || mode == 4);
    case 5:
        return CLS(ea_control(ea) || mode == 3);
    default:
        return FPU040_ILLEGAL;
    }
#undef CLS
}

static void test_classify_all_forms(void) {
    unsigned impl = 0;
    for (unsigned type = 0; type < 8; type++)
        for (unsigned ea = 0; ea < 64; ea++)
            for (unsigned top6 = 0; top6 < 64; top6++) {
                const fpu040_class_t got = fpu040_classify((uint16_t)(0xF200u | type << 6 | ea),
                                                           (uint16_t)(top6 << 10 | 0x22u));
                const fpu040_class_t expect = expected_class(type, ea, top6);
                assert(got == expect);
                impl += got == FPU040_IMPL;
            }
    assert(impl != 0);
}

/* ---- 外部形式の未対応データ型（single/double の denormal）の判定を全ビットで ---- */

static void run_fmove_imm(ctx_t *c, mc68k_state_t **cpu, unsigned fmt, const uint16_t *imm,
                          unsigned n) {
    uint16_t w[8] = {0xF23C, EXT_IN(fmt, 0, 0)};
    memcpy(&w[2], imm, n * sizeof(uint16_t));
    *cpu = make(c);
    run_at(c, *cpu, RESET_PC, w, 2 + n);
}

/* single: 指数 0 で仮数が非 0 のときだけ未対応データ型。指数の各 bit・仮数の各 bit を 1
 * 本ずつ立てる。 */
static void test_single_denormal_bits(void) {
    for (unsigned k = 0; k < 8; k++) { /* 指数の bit k だけ → 正規化数 */
        const uint32_t v = (1u << k) << 23 | 1u;
        const uint16_t imm[2] = {(uint16_t)(v >> 16), (uint16_t)v};
        mc68k_state_t *cpu;
        ctx_t c;
        run_fmove_imm(&c, &cpu, FMT_S, imm, 2);
        assert(!cpu->fpu040->exc.valid);
        mc68k_destroy(cpu);
    }
    for (unsigned k = 0; k < 23;
         k++) { /* 仮数の bit k だけ → denormal。ETEMP は左へ 40 寄せる */
        const uint32_t frac = 1u << k;
        for (unsigned sign = 0; sign < 2; sign++) {
            const uint32_t v = (sign ? 0x80000000u : 0u) | frac;
            const uint16_t imm[2] = {(uint16_t)(v >> 16), (uint16_t)v};
            mc68k_state_t *cpu;
            ctx_t c;
            run_fmove_imm(&c, &cpu, FMT_S, imm, 2);
            const mc68k_fpu040_exc_t *e = &cpu->fpu040->exc;
            assert(e->valid && e->vector == VEC_UNSUPPORTED && !e->unimpl);
            assert(e->etemp.signif == (uint64_t)frac << 40);
            assert(e->etemp.sign_exp == (sign ? 0x8000u : 0u));
            mc68k_destroy(cpu);
        }
    }
}

static void test_double_denormal_bits(void) {
    for (unsigned k = 0; k < 11;
         k++) { /* 指数の bit k だけ → 正規化数（符号 bit も含めて 12 bit 目まで） */
        const uint32_t hi = (1u << k) << 20;
        const uint16_t imm[4] = {(uint16_t)(hi >> 16), (uint16_t)hi, 0, 1};
        mc68k_state_t *cpu;
        ctx_t c;
        run_fmove_imm(&c, &cpu, FMT_D, imm, 4);
        assert(!cpu->fpu040->exc.valid);
        mc68k_destroy(cpu);
    }
    for (unsigned k = 0; k < 52;
         k++) { /* 仮数の bit k だけ → denormal。ETEMP は左へ 11 寄せる */
        const uint64_t frac = UINT64_C(1) << k;
        for (unsigned sign = 0; sign < 2; sign++) {
            const uint64_t v = (sign ? UINT64_C(0x8000000000000000) : 0) | frac;
            const uint16_t imm[4] = {(uint16_t)(v >> 48), (uint16_t)(v >> 32),
                                     (uint16_t)(v >> 16), (uint16_t)v};
            mc68k_state_t *cpu;
            ctx_t c;
            run_fmove_imm(&c, &cpu, FMT_D, imm, 4);
            const mc68k_fpu040_exc_t *e = &cpu->fpu040->exc;
            assert(e->valid && e->vector == VEC_UNSUPPORTED);
            assert(e->etemp.signif == frac << 11);
            assert(e->etemp.sign_exp == (sign ? 0x8000u : 0u));
            mc68k_destroy(cpu);
        }
    }
}

/* ---- 条件命令（FScc / FDBcc / FBcc / FTRAPcc）: レジスタ番号・マスクの各ビット ---- */

enum { PRED_EQ = 0x01, CC_Z = MC68K_FPU_CC_Z };

/* FScc: EA 欄の全レジスタ（D0-D7 は下位バイトだけ書き換える、(An) は A0-A7、(An)+ / -(An) は A7
 * のバイトが 2）。 */
static void test_fscc_all_registers(void) {
    for (unsigned r = 0; r < 8; r++) {
        for (unsigned tf = 0; tf < 2; tf++) {
            ctx_t c;
            mc68k_state_t *cpu = make(&c);
            const uint32_t before = tf ? 0xFFFFFF00u : 0x000000FFu;
            fst(cpu)->fpsr_cc = tf ? CC_Z : 0;
            cpu->d[r] =
                before; /* 下位バイトだけが反転する（残りの bit は 0x00 / 0xFF のまま） */
            RUN(&c, cpu, (uint16_t)(0xF240u | r), PRED_EQ);
            assert(cpu->d[r] == (tf ? 0xFFFFFFFFu : 0x00000000u));
            assert(rl(&c, 0) == RESET_SP); /* Dn への FScc はメモリへ書かない */
            for (unsigned o = 0; o < 8; o++)
                assert(o == r || cpu->d[o] == 0);
            mc68k_destroy(cpu);
        }
        for (unsigned mode = 2; mode <= 4; mode++) { /* (An) / (An)+ / -(An) */
            for (unsigned tf = 0; tf < 2; tf++) {
                ctx_t c;
                mc68k_state_t *cpu = make(&c);
                fst(cpu)->fpsr_cc = tf ? CC_Z : 0;
                const uint32_t base = DATA_AREA + 0x100;
                cpu->a[r] = r == 7 ? RESET_SP - 0x200 : base;
                const uint32_t start = cpu->a[r];
                const uint32_t step = r == 7 ? 2 : 1;
                RUN(&c, cpu, (uint16_t)(0xF240u | mode << 3 | r), PRED_EQ);
                const uint32_t addr = mode == 4 ? start - step : start;
                assert(c.ram[addr] == (tf ? 0xFF : 0x00));
                assert(cpu->a[r] == (mode == 2   ? start
                                     : mode == 3 ? start + step
                                                 : start - step));
                assert(cpu->a[7] == (r == 7 ? cpu->a[r] : RESET_SP));
                mc68k_destroy(cpu);
            }
        }
    }
}

/* FDBcc: カウンタは下位 16 bit だけ（上位は保つ）。-1 になったら分岐しない。 */
static void test_fdbcc_counter_bits(void) {
    for (unsigned r = 0; r < 8; r++) {
        static const struct {
            uint32_t d, expect;
            bool branch;
        } cases[] = {
            {0x00000000u, 0x0000FFFFu, false}, {0x12340000u, 0x1234FFFFu, false},
            {0xFFFF0000u, 0xFFFFFFFFu, false}, {0x00000001u, 0x00000000u, true},
            {0x0000FFFFu, 0x0000FFFEu, true},  {0xABCD8000u, 0xABCD7FFFu, true},
            {0x55550001u, 0x55550000u, true},  {0x0000AAAAu, 0x0000AAA9u, true},
            {0x00005555u, 0x00005554u, true},  {0xFFFF0100u, 0xFFFF00FFu, true},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            ctx_t c;
            mc68k_state_t *cpu = make(&c);
            cpu->d[r] = cases[i].d;
            const uint32_t end = RUN(&c, cpu, (uint16_t)(0xF248u | r), PRED_EQ, 0x0100);
            assert(cpu->d[r] == cases[i].expect);
            assert(cpu->pc == (cases[i].branch ? RESET_PC + 4 + 0x100 : end));
            mc68k_destroy(cpu);
        }
    }
}

/* FDBcc/FBcc/FTRAPcc を、EA 欄が mode 0〜6 でも FTRAPcc 扱いにならないことで確かめる（FScc の
 * EA 欄 reg 2〜4）。 */
static void test_fscc_is_not_ftrapcc(void) {
    for (unsigned mode = 0; mode < 7; mode++)
        for (unsigned r = 2; r <= 4; r++) {
            if (mode == 1)
                continue; /* FDBcc */
            ctx_t c;
            mc68k_state_t *cpu = make(&c);
            fst(cpu)->fpsr_cc = CC_Z;
            cpu->a[r] = DATA_AREA + 0x100;
            uint16_t ea_words[2] = {0, 0};
            const unsigned n_ea = mode == 5 ? 1 : mode == 6 ? 1 : 0;
            if (mode == 5)
                ea_words[0] = 0x0010;
            if (mode == 6)
                ea_words[0] = 0x0010; /* (d8,An,D0.W) */
            uint16_t w[4] = {(uint16_t)(0xF240u | mode << 3 | r), PRED_EQ, ea_words[0],
                             ea_words[1]};
            const uint32_t end = run_at(&c, cpu, RESET_PC, w, 2 + n_ea);
            assert(cpu->pc == end); /* FTRAPcc なら vector 7 の handler へ行く */
            mc68k_destroy(cpu);
        }
}

/* FBcc の変位の取得（W / L、符号拡張）。 */
static void test_fbcc_displacements(void) {
    static const struct {
        uint16_t opcode;
        uint16_t d0, d1;
        int32_t disp;
    } cases[] = {
        {0xF281, 0x7FFE, 0, 0x7FFE},
        {0xF281, 0x8000, 0, -0x8000},
        {0xF281, 0xFFF0, 0, -0x10},
        {0xF2C1, 0x0001, 0x0000, 0x10000},
        {0xF2C1, 0x0000, 0x8000, 0x8000},
        {0xF2C1, 0x00FF, 0xFFFF, 0xFFFFFF},
        {0xF2C1, 0x8000, 0x0000, (int32_t)0x80000000},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fpsr_cc = CC_Z;
        const bool is_long = cases[i].opcode == 0xF2C1;
        /* 分岐先に命令を置けない（RAM の外）ので、PC だけを見る。実行は 1 命令で止める。 */
        ww(&c, RESET_PC, cases[i].opcode);
        ww(&c, RESET_PC + 2, cases[i].d0);
        if (is_long)
            ww(&c, RESET_PC + 4, cases[i].d1);
        cpu->pc = RESET_PC;
        mc68k_execute(cpu, 1);
        assert(cpu->pc == RESET_PC + 2 + (uint32_t)cases[i].disp);
        mc68k_destroy(cpu);
    }
}

/* ---- FMOVE FPn,Dm / FMOVE Dm,FPn / FADD FPs,FPd: 全レジスタ番号 ---- */

static mc68k_fp80_t int_fp(unsigned n) { /* 1.0 〜 8.0 */
    static const mc68k_fp80_t table[8] = {
        {0x3FFF, 0x8000000000000000ull}, {0x4000, 0x8000000000000000ull},
        {0x4000, 0xC000000000000000ull}, {0x4001, 0x8000000000000000ull},
        {0x4001, 0xA000000000000000ull}, {0x4001, 0xC000000000000000ull},
        {0x4001, 0xE000000000000000ull}, {0x4002, 0x8000000000000000ull}};
    return table[n];
}

static void test_register_numbers_in_arithmetic(void) {
    for (unsigned s = 0; s < 8; s++)
        for (unsigned d = 0; d < 8; d++) {
            ctx_t c;
            mc68k_state_t *cpu = make(&c);
            for (unsigned i = 0; i < 8; i++)
                fst(cpu)->fp[i] = int_fp(i);
            RUN(&c, cpu, 0xF200, (uint16_t)(s << 10 | d << 7 | 0x22)); /* FADD FPs,FPd */
            /* 期待値は FMOVE.L で作る整数（s + 1 + d + 1）。FADD の経路とは別 */
            ctx_t c2;
            mc68k_state_t *ref = make(&c2);
            ref->d[0] = s + 1 + d + 1;
            RUN(&c2, ref, 0xF200, EXT_IN(FMT_L, 0, 0));
            for (unsigned i = 0; i < 8; i++)
                assert(same(fst(cpu)->fp[i], i == d ? fst(ref)->fp[0] : int_fp(i)));
            mc68k_destroy(ref);
            mc68k_destroy(cpu);
        }
}

/* FMOVE.L Dm,FPn と FMOVE.{L,W,B} FPn,Dm（B/W は下位だけを書き、残りの bit を保つ）。 */
static void test_register_numbers_in_moves(void) {
    static const struct {
        unsigned fmt;
        uint32_t keep_mask; /* 書かれずに残る bit */
    } widths[] = {{FMT_L, 0}, {FMT_W, 0xFFFF0000u}, {FMT_B, 0xFFFFFF00u}};

    for (unsigned n = 0; n < 8; n++)
        for (unsigned m = 0; m < 8; m++) {
            ctx_t c;
            mc68k_state_t *cpu = make(&c);
            cpu->d[m] = n + 1;
            RUN(&c, cpu, (uint16_t)(0xF200u | m), EXT_IN(FMT_L, n, 0));
            assert(same(fst(cpu)->fp[n], int_fp(n)));
            mc68k_destroy(cpu);

            for (unsigned w = 0; w < 3; w++)
                for (unsigned neg = 0; neg < 2;
                     neg++) { /* -1 を書く（全 bit 1）と 0 を書く（全 bit 0） */
                    cpu = make(&c);
                    fst(cpu)->fp[n] = neg ? fp(0xBFFF, 0x8000000000000000ull) : fp(0, 0);
                    cpu->d[m] = neg ? 0x00000000u : 0xFFFFFFFFu;
                    const uint32_t before = cpu->d[m];
                    RUN(&c, cpu, (uint16_t)(0xF200u | m), EXT_OUT(widths[w].fmt, n));
                    const uint32_t written = neg ? ~widths[w].keep_mask : 0u;
                    assert(cpu->d[m] == ((before & widths[w].keep_mask) | written));
                    mc68k_destroy(cpu);
                }
        }
}

/* FMOVE <ea>,FPcr / FPcr,<ea>: Dn・An の全レジスタ、空のレジスタ選択、即値。 */
static void test_control_register_numbers(void) {
    for (unsigned r = 0; r < 8; r++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        cpu->d[r] = 0x12345678u;
        RUN(&c, cpu, (uint16_t)(0xF200u | r), 0x8400); /* FMOVE.L Dr,FPIAR */
        assert(fst(cpu)->fpiar == 0x12345678u);
        mc68k_destroy(cpu);

        cpu = make(&c);
        cpu->a[r] = 0x9ABCDEF0u;
        RUN(&c, cpu, (uint16_t)(0xF208u | r), 0x8400); /* FMOVE.L Ar,FPIAR */
        assert(fst(cpu)->fpiar == 0x9ABCDEF0u);
        mc68k_destroy(cpu);

        cpu = make(&c);
        fst(cpu)->fpiar = 0x0BADF00Du;
        RUN(&c, cpu, (uint16_t)(0xF200u | r), 0xA400); /* FMOVE.L FPIAR,Dr */
        assert(cpu->d[r] == 0x0BADF00Du);
        mc68k_destroy(cpu);

        cpu = make(&c);
        fst(cpu)->fpiar = 0x0BADF00Du;
        RUN(&c, cpu, (uint16_t)(0xF208u | r), 0xA400); /* FMOVE.L FPIAR,Ar */
        assert(cpu->a[r] == 0x0BADF00Du);
        mc68k_destroy(cpu);

        /* レジスタ選択が空: Dn も An も（-(An)/(An)+ の更新も）変わらず、即値は読まない。 */
        cpu = make(&c);
        cpu->d[r] = 0x1234u;
        const uint32_t end = RUN(&c, cpu, (uint16_t)(0xF200u | r), 0xA000);
        assert(cpu->d[r] == 0x1234u && cpu->pc == end);
        mc68k_destroy(cpu);
    }
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    const uint32_t end = RUN(&c, cpu, 0xF23C, 0x8000); /* FMOVE.L #imm,（空） */
    assert(cpu->pc == end);
    mc68k_destroy(cpu);
}

/* FMOVEM: 静的リストの各 bit（FP0〜FP7）と、動的リストのレジスタ番号（D0〜D7）、リストは下位 8
 * bit だけ。 */
static void test_fmovem_lists(void) {
    for (unsigned r = 0; r < 8; r++) {
        for (unsigned form = 0; form < 4; form++) { /* 0: predecrement 静的 1: predecrement 動的
                                                       2: postincrement 静的 3: 同 動的 */
            const bool predec = form < 2, dynamic = form & 1u;
            for (unsigned fpn = 0; fpn < 8; fpn++) {
                const unsigned list_bit = predec ? fpn : 7 - fpn;
                ctx_t c;
                mc68k_state_t *cpu = make(&c);
                for (unsigned i = 0; i < 8; i++)
                    fst(cpu)->fp[i] = int_fp(i);
                cpu->d[r] =
                    0xFFFFFF00u | (1u << list_bit); /* 動的リストの Dn: 上位 24 bit は無視 */
                const unsigned dn = (r + 3) & 7u;
                cpu->d[dn] = 0xFFFFFF00u | (1u << list_bit);
                cpu->a[0] = DATA_AREA + 0x100;
                if (predec) {
                    const uint16_t ext = dynamic ? (uint16_t)(0xE800u | dn << 4)
                                                 : (uint16_t)(0xE000u | (1u << list_bit));
                    RUN(&c, cpu, 0xF220, ext);
                    assert(cpu->a[0] == DATA_AREA + 0x100 - 12);
                    assert(same(rx(&c, DATA_AREA + 0x100 - 12), int_fp(fpn)));
                } else {
                    wx(&c, DATA_AREA + 0x100, fp(0x4003, 0x8000000000000000ull));
                    const uint16_t ext = dynamic ? (uint16_t)(0xD800u | dn << 4)
                                                 : (uint16_t)(0xD000u | (1u << list_bit));
                    RUN(&c, cpu, 0xF218, ext);
                    assert(cpu->a[0] == DATA_AREA + 0x100 + 12);
                    for (unsigned i = 0; i < 8; i++)
                        assert(same(fst(cpu)->fp[i],
                                    i == fpn ? fp(0x4003, 0x8000000000000000ull) : int_fp(i)));
                }
                mc68k_destroy(cpu);
            }
        }
        /* An のレジスタ番号（制御モード (An)）。FP0 を書き出し、FP1 へ読み戻す。 */
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = int_fp(2);
        cpu->a[r] = r == 7 ? DATA_AREA + 0x100 : DATA_AREA + 0x100;
        RUN(&c, cpu, (uint16_t)(0xF210u | r), 0xF080); /* FMOVEM.X FP0,(Ar) */
        assert(same(rx(&c, DATA_AREA + 0x100), int_fp(2)));
        fst(cpu)->fp[5] = ONE;
        RUN(&c, cpu, (uint16_t)(0xF210u | r), 0xD004); /* FMOVEM.X (Ar),FP5 */
        assert(same(fst(cpu)->fp[5], int_fp(2)));
        mc68k_destroy(cpu);
    }
}

/* FSAVE / FRESTORE の An の番号（A0〜A7）。 */
static void test_save_restore_register_numbers(void) {
    for (unsigned r = 0; r < 8; r++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = ONE;
        RUN(&c, cpu, 0xF200, EXT_IN(FMT_L, 0, 0)); /* 実行済みにして idle frame にする */
        cpu->a[r] = r == 7 ? DATA_AREA + 0x100 : DATA_AREA + 0x100;
        RUN(&c, cpu, (uint16_t)(0xF310u | r)); /* FSAVE (Ar) */
        assert(rl(&c, DATA_AREA + 0x100) >> 24 == 0x41);
        assert(cpu->a[r] == DATA_AREA + 0x100);
        mc68k_destroy(cpu);

        cpu = make(&c);
        cpu->a[r] = DATA_AREA + 0x100 + 4;
        RUN(&c, cpu, (uint16_t)(0xF320u | r)); /* FSAVE -(Ar) */
        assert(cpu->a[r] == DATA_AREA + 0x100);
        mc68k_destroy(cpu);

        cpu = make(&c);
        wl(&c, DATA_AREA + 0x100, 0x41000000u); /* idle frame（本文なし） */
        cpu->a[r] = DATA_AREA + 0x100;
        RUN(&c, cpu, (uint16_t)(0xF358u | r)); /* FRESTORE (Ar)+ */
        assert(cpu->a[r] == DATA_AREA + 0x100 + 4);
        assert(cpu->fpu040->idle);
        mc68k_destroy(cpu);
    }
}

/* レジスタ間の未対応データ型（unnormalized）は、ソースのレジスタ番号が何であっても ETEMP
 * にそのレジスタの値を取る。 */
static void test_unsupported_register_sources(void) {
    for (unsigned s = 0; s < 8; s++)
        for (unsigned d = 0; d < 8; d++) {
            ctx_t c;
            mc68k_state_t *cpu = make(&c);
            for (unsigned i = 0; i < 8; i++)
                fst(cpu)->fp[i] = int_fp(i);
            const mc68k_fp80_t unnorm =
                fp(0x3FFF, 0x4000000000000000ull + s); /* 整数部ビットが 0 */
            fst(cpu)->fp[s] = unnorm;
            RUN(&c, cpu, 0xF200, (uint16_t)(s << 10 | d << 7 | 0x22)); /* FADD FPs,FPd */
            const mc68k_fpu040_exc_t *e = &cpu->fpu040->exc;
            assert(e->valid && e->vector == VEC_UNSUPPORTED);
            assert(same(e->etemp, unnorm));
            if (s != d)
                assert(same(e->fptemp, int_fp(d)));
            mc68k_destroy(cpu);
        }
}

/* 未実装の二項演算（FMOD・FREM・FSCALE）は、ソース・宛先のレジスタ番号どおりに ETEMP/FPTEMP
 * を取る。 */
static void test_unimplemented_register_capture(void) {
    static const unsigned dyadic[] = {0x21, 0x25, 0x26}; /* FMOD FREM FSCALE */
    for (unsigned k = 0; k < sizeof(dyadic) / sizeof(dyadic[0]); k++)
        for (unsigned s = 0; s < 8; s++)
            for (unsigned d = 0; d < 8; d++) {
                ctx_t c;
                mc68k_state_t *cpu = make(&c);
                for (unsigned i = 0; i < 8; i++)
                    fst(cpu)->fp[i] = int_fp(i);
                RUN(&c, cpu, 0xF200, (uint16_t)(s << 10 | d << 7 | dyadic[k])); /* FPs,FPd */
                const mc68k_fpu040_exc_t *e = &cpu->fpu040->exc;
                assert(e->valid && e->unimpl);
                assert(same(e->etemp, int_fp(s)));
                assert(same(e->fptemp, int_fp(d)));
                mc68k_destroy(cpu);
            }
}

/* ---- FPU 非搭載 040: EA を計算して format $4。An は更新しない。-(An) の EA
 * に出る幅（外部形式・レジスタ選択・FMOVEM リスト）と即値の長さ ---- */

/* 1 命令を実行し、format $4 の frame（EA = +8、faulted PC = +12）と A0 の不変、PC
 * の位置を確かめる。 */
static void expect_nofpu(const uint16_t *words, unsigned n, uint32_t a0, uint32_t ea,
                         uint32_t d_regs) {
    ctx_t c;
    mc68k_state_t *cpu = make_fpu(&c, MC68K_FPU_NONE_040);
    cpu->a[0] = a0;
    for (unsigned i = 1; i < 8; i++)
        cpu->d[i] = d_regs; /* D1〜D7（動的リストはどれを指しても同じ値。D0 は 0） */
    run_at(&c, cpu, RESET_PC, words, n);
    assert(cpu->pc == HANDLER(VEC_FLINE));
    assert(cpu->a[7] == RESET_SP - 16);
    assert(rw(&c, cpu->a[7] + 6) == (uint16_t)(0x4000u | VEC_FLINE * 4));
    assert(rl(&c, cpu->a[7] + 2) == RESET_PC + 2 * n); /* 次命令（拡張語・即値の後） */
    assert(rl(&c, cpu->a[7] + 8) == ea);
    assert(rl(&c, cpu->a[7] + 12) == RESET_PC);
    assert(cpu->a[0] == a0); /* FPU 非搭載 040 は An を更新しない */
    mc68k_destroy(cpu);
}

static void test_nofpu_operand_bytes(void) {
    static const unsigned fmt_bytes[7] = {4, 4, 12, 12, 2, 8, 1}; /* L S X P W D B */
    const uint32_t a = DATA_AREA + 0x100;

    for (unsigned fmt = 0; fmt < 7; fmt++) {
        const unsigned bytes = fmt_bytes[fmt];
        const uint16_t in[] = {0xF218, EXT_IN(fmt, 0, 0x22)}; /* FADD.<fmt> (A0)+,FP0 */
        expect_nofpu(in, 2, a, a, 0);
        const uint16_t pre[] = {0xF220, EXT_IN(fmt, 0, 0x22)}; /* FADD.<fmt> -(A0),FP0 */
        expect_nofpu(pre, 2, a, a - bytes, 0);
        const uint16_t out[] = {0xF218, EXT_OUT(fmt, 0)}; /* FMOVE.<fmt> FP0,(A0)+ */
        expect_nofpu(out, 2, a, a, 0);
        const uint16_t outp[] = {0xF220, EXT_OUT(fmt, 0)}; /* FMOVE.<fmt> FP0,-(A0) */
        expect_nofpu(outp, 2, a, a - bytes, 0);
    }
    { /* P{Dn}（動的な k-factor）は 12 バイト */
        const uint16_t w[] = {0xF218, (uint16_t)(0x6000 | 7 << 10 | 0x1000)};
        expect_nofpu(w, 2, a, a, 0);
        const uint16_t pre[] = {
            0xF220, (uint16_t)(0x6000 | 7 << 10 | 0x1000)}; /* -(A0) は 12 バイト引く */
        expect_nofpu(pre, 2, a, a - 12, 0);
    }
    { /* FMOVECR #0,FP0 は EA を持たない（Dn 形、EA = 0） */
        const uint16_t w[] = {0xF200, 0x5C00};
        expect_nofpu(w, 2, a, 0, 0);
    }
    /* 即値: 語数だけ読み飛ばし、EA は 0 */
    for (unsigned fmt = 0; fmt < 7; fmt++) {
        const unsigned words = fmt_bytes[fmt] < 2 ? 1 : fmt_bytes[fmt] / 2;
        uint16_t w[8] = {0xF23C, EXT_IN(fmt, 0, 0x22)};
        expect_nofpu(w, 2 + words, a, 0, 0);
    }
    /* FMOVE <ea>,FPcr / FPcr,<ea>: 4 バイト × レジスタ数（(An)+ は取り込み、-(An) は書き出し）
     */
    for (unsigned regsel = 1; regsel < 8; regsel++) {
        const unsigned bytes = 4 * bits_set(regsel);
        const uint16_t in[] = {0xF218, (uint16_t)(0x8000u | regsel << 10)};
        expect_nofpu(in, 2, a, a, 0);
        const uint16_t out[] = {0xF220, (uint16_t)(0xA000u | regsel << 10)};
        expect_nofpu(out, 2, a, a - bytes, 0);
        uint16_t imm[8] = {0xF23C, (uint16_t)(0x8000u | regsel << 10)};
        expect_nofpu(imm, 2 + 2 * bits_set(regsel), a, 0, 0); /* 即値（FPcr のみ） */
    }
    /* FMOVEM: 12 バイト × リストの bit 数。動的リストは Dn の下位 8 bit */
    for (unsigned list = 1; list < 256; list += list < 16 ? 1 : 37) {
        const unsigned bytes = 12 * bits_set(list);
        const uint16_t in[] = {0xF218, (uint16_t)(0xD000u | list)};
        expect_nofpu(in, 2, a, a, 0);
        const uint16_t out[] = {0xF220, (uint16_t)(0xE000u | list)};
        expect_nofpu(out, 2, a, a - bytes, 0);
        for (unsigned dn = 1; dn < 8; dn++) { /* 動的リストの Dn（D1〜D7 は同じ値、D0 は 0） */
            const uint16_t din[] = {0xF218, (uint16_t)(0xD800u | dn << 4)};
            expect_nofpu(din, 2, a, a, 0xFFFFFF00u | list);
            const uint16_t dout[] = {0xF220, (uint16_t)(0xE800u | dn << 4)};
            expect_nofpu(dout, 2, a, a - bytes, 0xFFFFFF00u | list);
        }
    }
    /* FScc は 1 バイト、FSAVE/FRESTORE と FBcc・FDBcc・FTRAPcc は EA
     * を計算しない（FSAVE/FRESTORE は An を変えず EA = An） */
    {
        const uint16_t scc[] = {0xF258, 0x0001};
        expect_nofpu(scc, 2, a, a, 0);
        const uint16_t sccp[] = {0xF260, 0x0001};
        expect_nofpu(sccp, 2, a, a - 1, 0);
        const uint16_t save[] = {0xF320}; /* FSAVE -(A0) */
        expect_nofpu(save, 1, a, a, 0);
        const uint16_t restore[] = {0xF358}; /* FRESTORE (A0)+ */
        expect_nofpu(restore, 1, a, a, 0);
        const uint16_t dbcc[] = {0xF248, 0x0001, 0x0100}; /* FDBcc D0（EA 欄は Dn） */
        expect_nofpu(dbcc, 3, a, 0, 0);
        const uint16_t trapw[] = {0xF27A, 0x0001, 0x1234}; /* FTRAPcc.W */
        expect_nofpu(trapw, 3, a, 0, 0);
        const uint16_t trapl[] = {0xF27B, 0x0001, 0x1234, 0x5678};
        expect_nofpu(trapl, 4, a, 0, 0);
        const uint16_t trap0[] = {0xF27C, 0x0001};
        expect_nofpu(trap0, 2, a, 0, 0);
        const uint16_t bccw[] = {0xF281, 0x0100}; /* FBcc.W: 変位を読み飛ばす */
        expect_nofpu(bccw, 2, a, 0, 0);
        const uint16_t bccl[] = {0xF2C1, 0x0000, 0x0100};
        expect_nofpu(bccl, 3, a, 0, 0);
    }
    /* PC 相対の EA（FTRAPcc の 072/073 と同じ EA 欄）は、FTRAPcc
     * 以外では変位の後ろを読み飛ばさない */
    {
        const uint16_t fadd_d16[] = {0xF23A, EXT_IN(FMT_L, 0, 0x22),
                                     0x0100}; /* FADD.L (d16,PC),FP0 */
        expect_nofpu(fadd_d16, 3, a, RESET_PC + 4 + 0x100, 0);
        const uint16_t fadd_d8[] = {0xF23B, EXT_IN(FMT_L, 0, 0x22),
                                    0x0010}; /* FADD.L (d8,PC,D0.W),FP0 */
        expect_nofpu(fadd_d8, 3, a, RESET_PC + 4 + 0x10, 0);
        const uint16_t restore_d16[] = {0xF37A, 0x0100}; /* FRESTORE (d16,PC) */
        expect_nofpu(restore_d16, 2, a, RESET_PC + 2 + 0x100, 0);
        const uint16_t restore_d8[] = {0xF37B, 0x0010}; /* FRESTORE (d8,PC,D0.W) */
        expect_nofpu(restore_d8, 2, a, RESET_PC + 2 + 0x10, 0);
        const uint16_t movea[] = {0xF208, 0x8400}; /* FMOVE.L A0,FPIAR（EA 欄は An） */
        expect_nofpu(movea, 2, a, 0, 0);
    }
    { /* A7 の byte 更新は 2 */
        ctx_t c;
        mc68k_state_t *cpu = make_fpu(&c, MC68K_FPU_NONE_040);
        run_at(&c, cpu, RESET_PC, (const uint16_t[]){0xF21F, EXT_IN(FMT_B, 0, 0x22)}, 2);
        assert(rl(&c, cpu->a[7] + 8) == RESET_SP && cpu->a[7] == RESET_SP - 16);
        mc68k_destroy(cpu);
    }
}

/* ---- バスエラー経路: 失敗した命令は FPU の状態も An も変えず、例外状態を作らない ---- */

typedef struct {
    uint16_t w[6];
    unsigned n;
    uint32_t pc; /* 既定は RESET_PC。RAM の端に置くと拡張語・変位・即値の取得が失敗する */
    bool none_040;
    bool pending; /* 先に保留中の例外（DZ）を作る: FSAVE/FRESTORE が壊さないことを見る */
    uint32_t a0;
    uint32_t hdr; /* 0 以外: RAM の末尾 4 バイトに置く frame の先頭 long（本文は RAM の外） */
} fault_case_t;

static void run_fault_case(const fault_case_t *fc) {
    ctx_t c;
    mc68k_state_t *cpu = make_fpu(&c, fc->none_040 ? MC68K_FPU_NONE_040 : MC68K_FPU_68040);
    mc68k_fpu_state_t *st = fc->none_040 ? NULL : fst(cpu);
    if (st) {
        for (unsigned i = 0; i < 8; i++)
            st->fp[i] = int_fp(i);
        st->fpcr_enable = MC68K_FPU_EXC_INEX2 | MC68K_FPU_EXC_DZ;
        st->fpsr_cc = CC_Z;
    }
    cpu->a[0] = fc->a0 ? fc->a0 : (uint32_t)BAD_ADDR;
    for (unsigned i = 1; i < 8; i++)
        cpu->d[i] = 0x11110000u * i + 0x0101u;
    const uint32_t d_before[8] = {cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3],
                                  cpu->d[4], cpu->d[5], cpu->d[6], cpu->d[7]};
    const uint32_t a0_before = cpu->a[0];
    if (fc->hdr)
        wl(&c, RAM_SIZE - 4, fc->hdr);
    if (fc->pending && st) {
        cpu->fpu040->exc.valid = true;
        cpu->fpu040->exc.vector = 50;
    }
    run_at(&c, cpu, fc->pc ? fc->pc : RESET_PC, fc->w, fc->n);
    assert(cpu->last_exception_vector == VEC_BUS_ERROR);
    assert(cpu->pc == HANDLER(VEC_BUS_ERROR));
    assert(cpu->a[0] == a0_before);
    for (unsigned i = 0; i < 8; i++)
        assert(cpu->d[i] == d_before[i]);
    if (st) {
        for (unsigned i = 0; i < 8; i++)
            assert(same(st->fp[i], int_fp(i)));
        assert(st->fpcr_enable == (MC68K_FPU_EXC_INEX2 | MC68K_FPU_EXC_DZ));
        assert(st->fpsr_cc == CC_Z);
        assert(cpu->fpu040->exc.valid ==
               fc->pending); /* 失敗した命令は例外状態を作らず、保留も消さない */
        if (fc->hdr)
            assert(!cpu->fpu040
                        ->idle); /* FRESTORE の本文の読出しが失敗したら、frame を取り込まない */
    }
    mc68k_destroy(cpu);
}

static void test_bus_error_paths(void) {
    enum { END = RAM_SIZE };
    static const fault_case_t cases[] = {
        /* 実行済み命令のメモリ読み書き（A0 = BAD_ADDR） */
        {{0xF210, EXT_IN(FMT_X, 1, 0x00)}, 2}, /* FMOVE.X (A0),FP1 */
        {{0xF218, EXT_IN(FMT_X, 1, 0x00)}, 2}, /* FMOVE.X (A0)+,FP1 */
        {{0xF220, EXT_IN(FMT_X, 1, 0x00)}, 2}, /* FMOVE.X -(A0),FP1 */
        {{0xF210, EXT_OUT(FMT_X, 1)}, 2},      /* FMOVE.X FP1,(A0) */
        {{0xF210, EXT_OUT(FMT_S, 0)}, 2}, /* FMOVE.S FP0,(A0): INEX2 は報告しない（書けない） */
        {{0xF210, 0x9000}, 2},            /* FMOVE.L (A0),FPCR */
        {{0xF210, 0x9C00}, 2},            /* FMOVE.L (A0),FPCR/FPSR/FPIAR */
        {{0xF218, 0x9C00}, 2},
        {{0xF210, 0xBC00}, 2}, /* FMOVE.L FPCR/FPSR/FPIAR,(A0) */
        {{0xF220, 0xBC00}, 2},
        {{0xF210, 0xD080}, 2}, /* FMOVEM.X (A0),FP0 */
        {{0xF218, 0xD080}, 2},
        {{0xF210, 0xF080}, 2}, /* FMOVEM.X FP0,(A0) */
        {{0xF220, 0xE001}, 2},
        {{0xF250, PRED_EQ}, 2}, /* FScc (A0) */
        {{0xF258, PRED_EQ}, 2},
        {{0xF260, PRED_EQ}, 2},
        {{0xF310}, 1, .pending = true}, /* FSAVE (A0) */
        {{0xF320}, 1, .pending = true}, /* FSAVE -(A0) */
        {{0xF350}, 1, .pending = true}, /* FRESTORE (A0) */
        {{0xF358}, 1, .pending = true}, /* FRESTORE (A0)+ */
        {{0xF350},
         1,
         .pending = true,
         .a0 = RAM_SIZE - 4,
         .hdr = 0x41600000u}, /* FRESTORE: 本文が RAM の外 */
        {{0xF350},
         1,
         .pending = true,
         .a0 = RAM_SIZE - 4,
         .hdr = 0x41300000u}, /* 同 unimplemented frame */
        /* 実行できない命令（format $2 のためのソース取得） */
        {{0xF210, EXT_IN(FMT_X, 1, 0x0E)}, 2}, /* FSIN.X (A0),FP1 */
        {{0xF218, EXT_IN(FMT_X, 1, 0x0E)}, 2},
        /* 拡張語・変位・即値の取得失敗（命令を RAM の端に置く） */
        {{0xF200}, 1, .pc = END - 2}, /* 拡張語 */
        {{0xF23C, EXT_IN(FMT_L, 1, 0x00)},
         2,
         .pc = END - 4}, /* FMOVE.L #imm,FP1: 即値が RAM の外 */
        {{0xF23C, EXT_IN(FMT_X, 1, 0x00), 0, 0, 0, 0},
         6,
         .pc = END - 8},                               /* FMOVE.X #imm,FP1: 途中で外へ出る */
        {{0xF23C, 0x8000 | 0x1000}, 2, .pc = END - 4}, /* FMOVE.L #imm,FPCR */
        {{0xF281, 0x0100}, 2, .pc = END - 2},          /* FBcc.W: 変位が RAM の外 */
        {{0xF2C1, 0x0000, 0x0100}, 3, .pc = END - 4},  /* FBcc.L: 下位の変位が RAM の外 */
        {{0xF248, PRED_EQ, 0x0100}, 3, .pc = END - 4}, /* FDBcc: 変位が RAM の外 */
        {{0xF27A, PRED_EQ, 0x1234}, 3, .pc = END - 4}, /* FTRAPcc.W: オペランドが RAM の外 */
        {{0xF27B, PRED_EQ, 0x1234, 0x5678}, 4, .pc = END - 6}, /* FTRAPcc.L */
        {{0xF228, EXT_IN(FMT_X, 1, 0x22), 0x0010},
         3,
         .pc = END - 4,
         .a0 = DATA_AREA}, /* (d16,A0): 変位語が RAM の外 */
        /* FPU 非搭載 040 */
        {{0xF200}, 1, .pc = END - 2, .none_040 = true},
        {{0xF23C, EXT_IN(FMT_X, 0, 0x22)},
         2,
         .pc = END - 4,
         .none_040 = true}, /* 即値の読み飛ばしが RAM の外 */
        {{0xF281, 0x0100}, 2, .pc = END - 2, .none_040 = true}, /* FBcc の変位の読み飛ばし */
        {{0xF27A, PRED_EQ, 0x1234}, 3, .pc = END - 4, .none_040 = true},
        {{0xF228, EXT_IN(FMT_X, 1, 0x22), 0x0010},
         3,
         .pc = END - 4,
         .none_040 = true,
         .a0 = DATA_AREA},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_fault_case(&cases[i]);
}

/* FBcc.L の上位語の取得が失敗したら、下位語を取りにいかない（host の bus
 * に余分な読出しを出さない。）。 */
static void test_fbcc_long_stops_after_high_word_fault(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    const uint16_t words[] = {0xF2C1, 0x0000, 0x0100};

    run_at(&c, cpu, RAM_SIZE - 2, words, 3); /* 上位語が RAM の外 */
    assert(cpu->last_exception_vector == VEC_BUS_ERROR);
    assert(cpu->addr_err_addr == RAM_SIZE);
    assert(c.failed_reads == 1);
    mc68k_destroy(cpu);
}

/* ---- 条件命令（FBcc/FDBcc/FScc/FTRAPcc）の補足と、フォルト時の An の復元 ---- */

/* 1 命令だけ実行する（実行後のトレース例外までは含む）。 */
static void exec_one(ctx_t *c, mc68k_state_t *cpu, const uint16_t *words, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        ww(c, RESET_PC + 2 * i, words[i]);
    cpu->pc = RESET_PC;
    mc68k_execute(cpu, 1);
}

/* FBcc は EA 欄の位置に条件を持つ。条件が 0〜63 のどれでも、FScc
 * の経路（Dn・メモリへの書込み）へ落ちない。 */
static void test_fbcc_does_not_fall_into_fscc(void) {
    for (unsigned pred = 0; pred < 64; pred++)
        for (unsigned is_long = 0; is_long < 2; is_long++)
            for (unsigned tf = 0; tf < 2; tf++) {
                ctx_t c;
                mc68k_state_t *cpu = make(&c);
                static ctx_t before;
                fst(cpu)->fpsr_cc = tf ? CC_Z : 0;
                for (unsigned i = 0; i < 8; i++) {
                    cpu->d[i] = 0x11111111u * (i + 1);
                    cpu->a[i] = i == 7 ? cpu->a[7] : DATA_AREA + 0x100 + 0x20 * i;
                }
                const uint32_t a_before[8] = {cpu->a[0], cpu->a[1], cpu->a[2], cpu->a[3],
                                              cpu->a[4], cpu->a[5], cpu->a[6], cpu->a[7]};
                before = c;
                const uint16_t w[3] = {(uint16_t)((is_long ? 0xF2C0u : 0xF280u) | pred), 0,
                                       0x0100};
                const uint16_t w_w[2] = {w[0], 0x0100};
                exec_one(&c, cpu, is_long ? w : w_w, is_long ? 3 : 2);
                for (unsigned i = 0; i < 8; i++) {
                    assert(cpu->d[i] == 0x11111111u * (i + 1));
                    assert(cpu->a[i] == a_before[i]);
                }
                for (unsigned i = 0; i < 6; i++)
                    before.ram[RESET_PC + i] = c.ram[RESET_PC + i]; /* 命令語は試験が置いた */
                assert(memcmp(before.ram, c.ram, sizeof(c.ram)) == 0);
                mc68k_destroy(cpu);
            }
}

/* FDBcc のカウンタ: 下位 16 bit が 1 ビットだけ立つ値から 1
 * を引く（立っていたビットが落ち、下位が全部立つ）。 */
static void test_fdbcc_single_bit_counters(void) {
    for (unsigned k = 0; k < 16; k++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        cpu->d[3] = 0xA5A50000u | (1u << k);
        const uint32_t end = RUN(&c, cpu, 0xF24B, PRED_EQ, 0x0100);
        (void)end;
        assert(cpu->d[3] == (0xA5A50000u | ((1u << k) - 1u)));
        assert(cpu->pc == RESET_PC + 4 + 0x100);
        mc68k_destroy(cpu);
    }
}

/* 変位の取得に失敗した FDBcc は、カウンタを減らさず、分岐もしない。 */
static void test_fdbcc_fault_keeps_counter(void) {
    for (unsigned r = 0; r < 8; r++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        cpu->d[r] = 0x00010005u;
        const uint16_t w[3] = {(uint16_t)(0xF248u | r), PRED_EQ, 0x0100};
        run_at(&c, cpu, RAM_SIZE - 4, w,
               2); /* 変位語が RAM の外（条件は偽: 分岐するならカウンタを減らす） */
        assert(cpu->last_exception_vector == VEC_BUS_ERROR);
        assert(cpu->d[r] == 0x00010005u);
        mc68k_destroy(cpu);
    }
}

/* FScc の EA の拡張語が取れないときは、メモリへ書かない。 */
static void test_fscc_fault_does_not_write(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fst(cpu)->fpsr_cc = CC_Z;
    cpu->a[0] = DATA_AREA;
    const uint16_t w[2] = {0xF268, PRED_EQ}; /* FScc (d16,A0): 変位語が RAM の外 */
    run_at(&c, cpu, RAM_SIZE - 4, w, 2);
    assert(cpu->last_exception_vector == VEC_BUS_ERROR);
    assert(rl(&c, 0) == RESET_SP);
    for (unsigned i = 0; i < 0x40; i++)
        assert(c.ram[DATA_AREA + i] == 0);
    mc68k_destroy(cpu);
}

/* T0 トレース: 分岐が成立した FBcc/FDBcc の後だけトレース例外（ベクタ 9）。FTRAPcc は FTRAPcc
 * 例外のみ。 */
static void test_t0_trace_on_taken_branches(void) {
    static const struct {
        uint16_t w[3];
        unsigned n;
        uint32_t d0;
        bool cc_z, trace;
    } cases[] = {
        {{0xF281, 0x0100}, 2, 0, true, true},         /* FBcc.W 成立 */
        {{0xF281, 0x0100}, 2, 0, false, false},       /* 不成立 */
        {{0xF2C1, 0x0000, 0x0100}, 3, 0, true, true}, /* FBcc.L 成立 */
        {{0xF2C1, 0x0000, 0x0100}, 3, 0, false, false},
        {{0xF248, PRED_EQ, 0x0100},
         3,
         5,
         false,
         true}, /* FDBcc: 条件が偽でカウンタが残る → 分岐 */
        {{0xF248, PRED_EQ, 0x0100},
         3,
         0,
         false,
         false}, /* カウンタが尽きる（0 → -1）→ 分岐しない */
        {{0xF248, PRED_EQ, 0x0100}, 3, 5, true, false}, /* 条件が真 → 分岐しない */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fpsr_cc = cases[i].cc_z ? CC_Z : 0;
        cpu->d[0] = cases[i].d0;
        cpu->sr = RESET_SR | SR_T0;
        cpu->last_exception_vector = 0;
        exec_one(&c, cpu, cases[i].w, cases[i].n);
        assert((cpu->last_exception_vector == 9) == cases[i].trace);
        mc68k_destroy(cpu);
    }
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fst(cpu)->fpsr_cc = CC_Z;
    cpu->sr = RESET_SR | SR_T0;
    cpu->last_exception_vector = 0;
    const uint16_t w[2] = {0xF27C, PRED_EQ}; /* FTRAPcc 成立 */
    exec_one(&c, cpu, w, 2);
    assert(cpu->last_exception_vector == 7); /* 同時のトレースは報告しない */
    mc68k_destroy(cpu);
}

/* フォルトで An の更新を戻すとき、戻す先のレジスタ番号が正しい（A1〜A6。A0 は他の試験、A7 は
 * SP）。 */
static void test_an_restore_register_numbers(void) {
    static const struct {
        uint16_t opcode, ext;
        unsigned n; /* 命令語の数 */
    } cases[] = {
        {0xF218, 0x9000, 2},                 /* FMOVE.L (An)+,FPCR */
        {0xF220, 0xB000, 2},                 /* FMOVE.L FPCR,-(An) */
        {0xF258, PRED_EQ, 2},                /* FScc (An)+ */
        {0xF260, PRED_EQ, 2},                /* FScc -(An) */
        {0xF218, 0xD080, 2},                 /* FMOVEM.X (An)+,FP0 */
        {0xF220, 0xE001, 2},                 /* FMOVEM.X FP0,-(An) */
        {0xF218, EXT_IN(FMT_X, 1, 0x00), 2}, /* FMOVE.X (An)+,FP1 */
        {0xF220, EXT_OUT(FMT_X, 1), 2},      /* FMOVE.X FP1,-(An) */
        {0xF218, EXT_IN(FMT_L, 1, 0x00), 2}, /* FMOVE.L (An)+,FP1 */
        {0xF218, EXT_IN(FMT_B, 1, 0x00), 2}, /* FMOVE.B (An)+,FP1 */
        {0xF358, 0, 1},                      /* FRESTORE (An)+ */
        {0xF320, 0, 1},                      /* FSAVE -(An) */
    };
    for (unsigned r = 1; r <= 6; r++)
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            ctx_t c;
            mc68k_state_t *cpu = make(&c);
            for (unsigned a = 0; a < 7; a++)
                cpu->a[a] = DATA_AREA + 0x100 + 0x20 * a;
            cpu->a[r] = BAD_ADDR + 0x10 * r;
            uint32_t a_before[7];
            for (unsigned a = 0; a < 7; a++)
                a_before[a] = cpu->a[a];
            const uint16_t w[2] = {(uint16_t)(cases[i].opcode | r), cases[i].ext};
            run_at(&c, cpu, RESET_PC, w, cases[i].n);
            assert(cpu->last_exception_vector == VEC_BUS_ERROR);
            for (unsigned a = 0; a < 7; a++)
                assert(cpu->a[a] == a_before[a]);
            mc68k_destroy(cpu);
        }
}

/* ---- メモリオペランドの幅と書込み範囲 ---- */

/* n（0 以上、2^63 未満）を正確に表す拡張精度値。 */
static mc68k_fp80_t fp_from_uint(uint64_t n) {
    assert(n != 0 && n < (UINT64_C(1) << 63));
    unsigned top = 0;
    for (unsigned b = 0; b < 64; b++)
        if (n >> b)
            top = b;
    return fp((uint16_t)(0x3FFF + top), n << (63 - top));
}

enum { SENTINEL = 0xA5, GUARD_BYTES = 16, BUF = DATA_AREA + 0x100 };

static void fill_sentinel(ctx_t *c) {
    memset(&c->ram[BUF - GUARD_BYTES], SENTINEL, 3 * GUARD_BYTES);
}

/* BUF の前の GUARD_BYTES バイトと、BUF + len から GUARD_BYTES
 * バイトは、書込み前の値（SENTINEL）のまま。 */
static void assert_guard(const ctx_t *c, unsigned len) {
    for (unsigned i = 0; i < GUARD_BYTES; i++) {
        assert(c->ram[BUF - GUARD_BYTES + i] == SENTINEL);
        assert(c->ram[BUF + len + i] == SENTINEL);
    }
}

/* FMOVE FP1,<ea> の各形式の書込みバイト数と値、FMOVE <ea>,FP1 の読込み。 */
static void test_memory_operand_widths(void) {
    static const struct {
        unsigned fmt, len;
        uint64_t value;   /* FP1 に置く整数 */
        uint8_t bytes[4]; /* 先頭の最大 4 バイト */
    } cases[] = {
        {FMT_B, 1, 0x7B, {0x7B}},
        {FMT_W, 2, 0x1234, {0x12, 0x34}},
        {FMT_L, 4, 0x12345678, {0x12, 0x34, 0x56, 0x78}},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fill_sentinel(&c);
        fst(cpu)->fp[1] = fp_from_uint(cases[i].value);
        cpu->a[0] = BUF;
        RUN(&c, cpu, 0xF210, EXT_OUT(cases[i].fmt, 1)); /* FMOVE.<fmt> FP1,(A0) */
        for (unsigned b = 0; b < cases[i].len; b++)
            assert(c.ram[BUF + b] == cases[i].bytes[b]);
        assert_guard(&c, cases[i].len);
        mc68k_destroy(cpu);

        cpu = make(&c);
        fill_sentinel(&c);
        for (unsigned b = 0; b < cases[i].len; b++)
            c.ram[BUF + b] = cases[i].bytes[b];
        cpu->a[0] = BUF;
        RUN(&c, cpu, 0xF210, EXT_IN(cases[i].fmt, 1, 0x00)); /* FMOVE.<fmt> (A0),FP1 */
        assert(same(fst(cpu)->fp[1], fp_from_uint(cases[i].value)));
        mc68k_destroy(cpu);
    }

    static const struct {
        uint16_t ext;
        unsigned len;
    } ctl[] = {{0xB000, 4}, {0xA800, 4}, {0xA400, 4}, {0xBC00, 12}, {0xB800, 8}};
    for (unsigned i = 0; i < sizeof(ctl) / sizeof(ctl[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fill_sentinel(&c);
        fst(cpu)->fpiar = 0x01020304u;
        cpu->a[0] = BUF;
        RUN(&c, cpu, 0xF210, ctl[i].ext); /* FMOVE.L FPcr,(A0) */
        assert_guard(&c, ctl[i].len);
        mc68k_destroy(cpu);
    }

    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fill_sentinel(&c);
    fst(cpu)->fp[0] = ONE;
    cpu->a[0] = BUF;
    RUN(&c, cpu, 0xF210, 0xF080); /* FMOVEM.X FP0,(A0) */
    assert(same(rx(&c, BUF), ONE));
    assert_guard(&c, 12);
    mc68k_destroy(cpu);
}

/* FMOVE.B #imm,FPn: 拡張語の下位バイトが値（上位バイトは無視）。全ビットを 1 つずつ。 */
static void test_byte_immediate_bits(void) {
    for (unsigned k = 0; k < 8; k++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        const unsigned imm = 1u << k;
        RUN(&c, cpu, 0xF23C, EXT_IN(FMT_B, 1, 0x00),
            (uint16_t)(0xA500u | imm)); /* FMOVE.B #imm,FP1 */
        const mc68k_fp80_t expect = k == 7 ? fp(0xC006, 0x8000000000000000ull)
                                           : fp_from_uint(imm); /* -128 または 2^k */
        assert(same(fst(cpu)->fp[1], expect));
        mc68k_destroy(cpu);
    }
}

/* FMOVEM は FP0〜FP7 の 8 本（96 バイト）を書き出して読み戻せる。 */
static void test_fmovem_all_registers(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    for (unsigned i = 0; i < 8; i++)
        fst(cpu)->fp[i] = int_fp(i);
    cpu->a[0] = BUF + 96;
    RUN(&c, cpu, 0xF220, 0xE0FF); /* FMOVEM.X FP0-FP7,-(A0) */
    assert(cpu->a[0] == BUF);
    for (unsigned i = 0; i < 8; i++)
        assert(same(rx(&c, BUF + 12 * i), int_fp(i)));
    for (unsigned i = 0; i < 8; i++)
        fst(cpu)->fp[i] = ONE;
    RUN(&c, cpu, 0xF218, 0xD0FF); /* FMOVEM.X (A0)+,FP0-FP7 */
    assert(cpu->a[0] == BUF + 96);
    for (unsigned i = 0; i < 8; i++)
        assert(same(fst(cpu)->fp[i], int_fp(i)));
    mc68k_destroy(cpu);
}

/* 先頭の書込みが失敗した後は、続きの書込みを行わない（アドレスが 32 bit で一周して 0
 * 番地に当たる場合）。 */
static void test_write_stops_after_fault(void) {
    static const struct {
        uint16_t ext;
    } cases[] = {{EXT_OUT(FMT_X, 1)}, {0xF080}, {0xBC00}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = ONE;
        fst(cpu)->fp[1] = ONE;
        fst(cpu)->fpiar = 0x01020304u;
        cpu->a[0] = 0xFFFFFFFCu;
        RUN(&c, cpu, 0xF210, cases[i].ext);
        assert(cpu->last_exception_vector == VEC_BUS_ERROR);
        assert(rl(&c, 0) == RESET_SP);
        mc68k_destroy(cpu);
    }
}

/*
 * 先頭の読出しが失敗した後は、続きの読出しを行わない（P-N15）。68040 の MMU
 * は表の探索で記述子の U ビットを立てるので、 続けて読むと、次のページの記述子が変わる。1
 * つ目の long が失敗し、2 つ目が別ページに当たる配置で確かめる。
 */
enum {
    MMU_ROOT = 0x2000,
    MMU_POINTER = 0x2400,
    MMU_PAGE = 0x2800,
    MMU_PAGE_SIZE = 0x1000,
    MMU_LOGICAL = 0x02000000u,
    MMU_GOOD_PHYS = 0x4000,
    MMU_NEXT_PHYS = 0x5000,
    MMU_TC_ENABLE = 0x8000,
    MMU_TTR_LOW_16M = 0x0000C000, /* E=1、S=2（FC 無視）、0x00xxxxxx だけ透過 */
    DESC_RESIDENT = 1,
    DESC_USED = 8,
    TABLE_DESC = 2,
};

static void test_read_stops_after_fault(void) {
    static const struct {
        uint16_t ext;
    } cases[] = {{EXT_IN(FMT_X, 1, 0)}, {0xD080}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (unsigned first_ok = 0; first_ok < 2; first_ok++) {
            ctx_t c;
            mc68k_state_t *cpu = make(&c);
            const uint32_t leaf0 = MMU_PAGE, leaf1 = MMU_PAGE + 4;
            wl(&c, MMU_ROOT + (MMU_LOGICAL >> 25) * 4u, MMU_POINTER | TABLE_DESC);
            wl(&c, MMU_POINTER + ((MMU_LOGICAL >> 18) & 0x7f) * 4u, MMU_PAGE | TABLE_DESC);
            /* 1 ページ目は RAM の外（読出しがバスエラー）、または RAM 内（対照）。2
             * ページ目は常に RAM 内。 */
            wl(&c, leaf0, (first_ok ? MMU_GOOD_PHYS : RAM_SIZE * 2u) | DESC_RESIDENT);
            wl(&c, leaf1, MMU_NEXT_PHYS | DESC_RESIDENT);
            cpu->mmu040_tc = MMU_TC_ENABLE;
            cpu->mmu040_srp = cpu->mmu040_urp = MMU_ROOT;
            cpu->mmu040_itt0 = cpu->mmu040_dtt0 = MMU_TTR_LOW_16M;
            cpu->a[0] = MMU_LOGICAL + MMU_PAGE_SIZE -
                        4; /* 先頭の long は 1 ページ目、続きは 2 ページ目 */
            RUN(&c, cpu, 0xF210, cases[i].ext);
            if (first_ok) { /* 対照: 読出しが続けば 2 ページ目の U ビットが立つ（この試験が U
                               ビットを見られる） */
                assert(cpu->last_exception_vector != VEC_BUS_ERROR);
                assert(rl(&c, leaf1) & DESC_USED);
            } else {
                assert(cpu->last_exception_vector == VEC_BUS_ERROR);
                assert(rl(&c, leaf1) == (MMU_NEXT_PHYS | DESC_RESIDENT));
            }
            mc68k_destroy(cpu);
        }
    }
}

/* オペランドの取得に失敗した FTRAPcc は、FTRAPcc
 * の例外フレームを積まない（バスエラーのフレームだけ）。 */
static void test_ftrapcc_fault_pushes_only_bus_error(void) {
    static const struct {
        uint16_t w[4];
        unsigned n;
        uint32_t pc;
    } cases[] = {
        {{0xF281, 0x0100}, 2, RAM_SIZE - 2},          /* 基準: FBcc.W の変位が RAM の外 */
        {{0xF27A, PRED_EQ, 0x1234}, 3, RAM_SIZE - 4}, /* FTRAPcc.W（条件は真） */
        {{0xF27B, PRED_EQ, 0x1234, 0x5678}, 4, RAM_SIZE - 6}, /* FTRAPcc.L */
    };
    uint32_t base = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fpsr_cc = CC_Z;
        run_at(&c, cpu, cases[i].pc, cases[i].w, cases[i].n);
        assert(cpu->last_exception_vector == VEC_BUS_ERROR);
        if (i == 0)
            base = cpu->a[7];
        else
            assert(cpu->a[7] == base);
        mc68k_destroy(cpu);
    }
}

/* ---- 演算の opmode:
 * 結果が宛先・ソースと違う値で、基本命令と強制精度命令（FSxxx・FDxxx）を突く ---- */

#define XVAL                                                \
    fp(0x3FFF, 0x8000000200000100ull) /* 1 + 2^-30 + 2^-55: \
                                         単精度・倍精度・拡張精度で丸めが分かれる */
#define NEG_XVAL fp(0xBFFF, 0x8000000200000100ull)
#define HALF     fp(0x3FFE, 0x8000000000000000ull)

typedef struct {
    unsigned base, single, dbl;
    mc68k_fp80_t dst, src;
} opmode_case_t;

static mc68k_fp80_t run_opmode_case(unsigned opmode, const opmode_case_t *oc,
                                    unsigned fpcr_prec, unsigned *prec_after) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fst(cpu)->fp[0] = oc->dst;
    fst(cpu)->fp[1] = oc->src;
    fst(cpu)->fpcr_round_prec = (uint8_t)fpcr_prec;
    RUN(&c, cpu, 0xF200, (uint16_t)(0x0400u | opmode)); /* <op> FP1,FP0 */
    assert(cpu->fpu040->exc.valid == false);
    const mc68k_fp80_t r = fst(cpu)->fp[0];
    *prec_after = fst(cpu)->fpcr_round_prec;
    mc68k_destroy(cpu);
    return r;
}

static void test_forced_precision_opmodes(void) {
    static const opmode_case_t cases[] = {
        {0x00,
         0x40,
         0x44,
         {0x4000, 0x8000000000000000ull},
         {0x3FFF, 0x8000000200000100ull}}, /* FMOVE */
        {0x04,
         0x41,
         0x45,
         {0x4000, 0x8000000000000000ull},
         {0x3FFF, 0x8000000200000100ull}}, /* FSQRT */
        {0x18,
         0x58,
         0x5C,
         {0x4000, 0x8000000000000000ull},
         {0xBFFF, 0x8000000200000100ull}}, /* FABS */
        {0x1A,
         0x5A,
         0x5E,
         {0x4000, 0x8000000000000000ull},
         {0x3FFF, 0x8000000200000100ull}}, /* FNEG */
        {0x20,
         0x60,
         0x64,
         {0x3FFF, 0x8000000200000100ull},
         {0x3FFF, 0x8000000000000000ull}}, /* FDIV */
        {0x22,
         0x62,
         0x66,
         {0x3FFF, 0x8000000200000100ull},
         {0x3FFF, 0x8000000000000000ull}}, /* FADD */
        {0x23,
         0x63,
         0x67,
         {0x3FFF, 0x8000000200000100ull},
         {0x3FFF, 0x8000000000000000ull}}, /* FMUL */
        {0x28,
         0x68,
         0x6C,
         {0x3FFF, 0x8000000200000100ull},
         {0x3FFE, 0x8000000000000000ull}}, /* FSUB */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const opmode_case_t *oc = &cases[i];
        unsigned pa;
        /* 基本命令を FPCR の精度で実行したものが、強制精度命令の期待値（FPCR
         * の精度は使わない・変えない）。 */
        const mc68k_fp80_t ext_r = run_opmode_case(oc->base, oc, MC68K_FPU_PREC_EXTENDED, &pa);
        const mc68k_fp80_t sgl_r = run_opmode_case(oc->base, oc, MC68K_FPU_PREC_SINGLE, &pa);
        const mc68k_fp80_t dbl_r = run_opmode_case(oc->base, oc, MC68K_FPU_PREC_DOUBLE, &pa);
        assert(!same(sgl_r, dbl_r) && !same(dbl_r, ext_r)); /* 値が精度で分かれていること */
        for (unsigned prec = 0; prec < 3; prec++) {
            assert(same(run_opmode_case(oc->single, oc, prec, &pa), sgl_r) && pa == prec);
            assert(same(run_opmode_case(oc->dbl, oc, prec, &pa), dbl_r) && pa == prec);
        }
    }
}

/* FTST・FCMP は宛先を書かない（条件コードだけ）。 */
static void test_compare_does_not_write_destination(void) {
    static const unsigned ops[] = {0x38, 0x3A}; /* FCMP FTST */
    for (unsigned i = 0; i < 2; i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = int_fp(6);
        fst(cpu)->fp[1] = int_fp(2);
        RUN(&c, cpu, 0xF200, (uint16_t)(0x0400u | ops[i]));
        assert(same(fst(cpu)->fp[0], int_fp(6)) && same(fst(cpu)->fp[1], int_fp(2)));
        mc68k_destroy(cpu);
    }
}

/* ---- 未実装命令の例外フレーム（format $2 / $4）の積み方 ---- */

enum {
    EXT_FSIN_FP1_FP0 = 0x040E,
    FRAME_BYTES_UNIMPL = 12,
    FRAME_BYTES_NOFPU = 16,
    MOVE_0_TO_SR = 0x46FC
};

/* ユーザーモードから起きても、ハンドラはスーパーバイザで動き、積んだ SR は元の値のまま。 */
static void test_unimplemented_from_user_mode(void) {
    for (unsigned nofpu = 0; nofpu < 2; nofpu++) {
        ctx_t c;
        mc68k_state_t *cpu = make_fpu(&c, nofpu ? MC68K_FPU_NONE_040 : MC68K_FPU_68040);
        RUN(&c, cpu, MOVE_0_TO_SR, 0x0000, 0xF200, EXT_FSIN_FP1_FP0);
        const uint32_t sp = RESET_SP - (nofpu ? FRAME_BYTES_NOFPU : FRAME_BYTES_UNIMPL);
        assert(cpu->pc == HANDLER(VEC_FLINE));
        assert((cpu->sr & 0x2000u) != 0);
        assert(cpu->a[7] == sp);
        assert(rw(&c, sp) == 0x0000); /* 積んだ SR（ユーザーモード） */
        mc68k_destroy(cpu);
    }
}

/* format $2 のフレームは 12 バイトで、その上のメモリを書かない。成功した例外は frame
 * の記録を残さない。 */
static void test_unimplemented_frame_extent(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    memset(&c.ram[RESET_SP], SENTINEL, 16);
    RUN(&c, cpu, 0xF200, EXT_FSIN_FP1_FP0);
    assert(cpu->pc == HANDLER(VEC_FLINE));
    for (unsigned i = 0; i < 16; i++)
        assert(c.ram[RESET_SP + i] == SENTINEL);
    assert(cpu->exception_frame_origins == NULL);
    assert(rl(&c, RESET_SP - FRAME_BYTES_UNIMPL + 8) ==
           0); /* レジスタ間の命令は EA を持たない */
    mc68k_destroy(cpu);
}

/* 最初の書込みに失敗したら、続きを積まない（スタックのアドレスが 32 bit で一周して 0
 * 番地に当たる場合）。 */
static void test_unimplemented_frame_stops_at_first_fault(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    cpu->a[7] = 0x0Au; /* 12 バイトのフレームの先頭が 0xFFFFFFFE（最初の word が RAM
                          の外、続きは 0 番地以降） */
    RUN(&c, cpu, 0xF200, EXT_FSIN_FP1_FP0);
    assert(cpu->last_exception_vector != VEC_FLINE);
    assert(rl(&c, 0) == RESET_SP && rl(&c, 4) == RESET_PC);
    mc68k_destroy(cpu);
}

/* ---- FMOVE FPn,<ea>（opclass 011）の補足 ---- */

#define SNAN_X \
    fp(0x7FFF, 0x8000000000000001ull) /* 非数（仮数の最上位の次が 0）= signaling NaN */

/* -0.0 は整数の最小値と同じビット列（0x80000000）になるが、整数宛てではないので OPERR
 * にならない。 */
static void test_negative_zero_to_float_formats(void) {
    static const unsigned fmts[] = {FMT_S, FMT_D, FMT_X};
    for (unsigned i = 0; i < 3; i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = fp(0x8000, 0);
        fst(cpu)->fpcr_enable = MC68K_FPU_EXC_OPERR;
        cpu->a[0] = BUF;
        RUN(&c, cpu, 0xF210, EXT_OUT(fmts[i], 0)); /* FMOVE.<fmt> FP0,(A0) */
        assert(fst(cpu)->fpsr_exc == 0 && !cpu->fpu040->exc.valid);
        assert(rl(&c, BUF) == 0x80000000u);
        mc68k_destroy(cpu);
    }
}

/* signaling NaN の FMOVE out: 整数宛てだけが無条件に SNAN を報告し、S/D/X 宛ては FPCR の enable
 * に従う。 */
static void test_snan_to_memory(void) {
    static const unsigned fmts[] = {FMT_S, FMT_D, FMT_X, FMT_L, FMT_W, FMT_B};
    for (unsigned i = 0; i < 6; i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = SNAN_X;
        cpu->a[0] = BUF;
        RUN(&c, cpu, 0xF210, EXT_OUT(fmts[i], 0));
        const bool int_dest = fmts[i] == FMT_L || fmts[i] == FMT_W || fmts[i] == FMT_B;
        assert(cpu->fpu040->exc.valid == int_dest);
        mc68k_destroy(cpu);
    }
}

/* 丸め前の中間結果は、直前の命令の記録（FADD の INEX2）を引きずらず、この命令の OVFL
 * のものを取る。 */
static void test_overflow_e1_ignores_previous_unrounded(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    fst(cpu)->fp[0] = XVAL;
    fst(cpu)->fp[1] = ONE;
    RUN(&c, cpu, 0xF200,
        0x0022); /* FADD FP0,FP0: 丸め前の記録を残す（INEX2 は無効なので例外にならない） */
    assert(!cpu->fpu040->exc.valid);
    fst(cpu)->fp[2] = fp(0x40C7, 0x8000000000000000ull); /* 2^200 */
    fst(cpu)->fpcr_enable = MC68K_FPU_EXC_OVFL;
    cpu->a[0] = BUF;
    RUN(&c, cpu, 0xF210, EXT_OUT(FMT_S, 2)); /* FMOVE.S FP2,(A0): 単精度で溢れる */
    const mc68k_fpu040_exc_t *e = &cpu->fpu040->exc;
    assert(e->valid && e->vector != 0);
    assert(same(e->etemp, fp(0x40C7, 0x8000000000000000ull)));
    mc68k_destroy(cpu);
}

/* 書込みに失敗したら、結果の INEX2 を報告しない。EA の拡張語が取れない B/W
 * 書込みは、メモリへ書かず FPSR にも触れない。 */
static void test_to_ea_fault_keeps_state(void) {
    for (unsigned form = 0; form < 3; form++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = XVAL;
        fst(cpu)->fpcr_enable = MC68K_FPU_EXC_INEX2;
        fst(cpu)->fpsr_exc = MC68K_FPU_EXC_DZ;
        cpu->a[0] = form == 0 ? (uint32_t)BAD_ADDR : (uint32_t)DATA_AREA;
        if (form == 0) { /* FMOVE.S FP0,(A0): 不正確（INEX2 有効）で、書込みが失敗する */
            RUN(&c, cpu, 0xF210, EXT_OUT(FMT_S, 0));
        } else { /* FMOVE.B / .W FP0,(d16,A0): d16 が RAM の外 */
            const uint16_t w[2] = {0xF228, EXT_OUT(form == 1 ? FMT_B : FMT_W, 0)};
            run_at(&c, cpu, RAM_SIZE - 4, w, 2);
            assert(rl(&c, 0) == RESET_SP);
            for (unsigned i = 0; i < 0x40; i++)
                assert(c.ram[DATA_AREA + i] == 0);
        }
        assert(cpu->last_exception_vector == VEC_BUS_ERROR);
        assert(!cpu->fpu040->exc.valid);
        if (form != 0) /* 拡張語の取得で失敗した命令は FPSR に触れない（変換の後で失敗する form
                          0 は INEX2 が残る） */
            assert(fst(cpu)->fpsr_exc == MC68K_FPU_EXC_DZ);
        mc68k_destroy(cpu);
    }
}

/* ---- FPU 非搭載 040 の補足と、op_table の範囲 ---- */

/* FBcc は EA 欄の位置に条件を持つ。条件が何であっても、EA
 * を計算せず変位だけを読み飛ばす（An・メモリも変えない）。 */
static void test_nofpu_fbcc_all_predicates(void) {
    for (unsigned pred = 0; pred < 64; pred++) {
        const uint16_t w[] = {(uint16_t)(0xF280u | pred), 0x0100};
        expect_nofpu(w, 2, DATA_AREA + 0x100, 0, 0);
        const uint16_t l[] = {(uint16_t)(0xF2C0u | pred), 0x0000, 0x0100};
        expect_nofpu(l, 3, DATA_AREA + 0x100, 0, 0);
    }
}

/* EA・変位の取得に失敗した命令は、format $4
 * のフレームを積まず、バスエラーのフレームだけを積む（FBcc の失敗を基準にする）。 */
static void test_nofpu_fault_pushes_only_bus_error(void) {
    static const struct {
        uint16_t w[4];
        unsigned n;
        uint32_t pc;
    } cases[] = {
        {{0xF281, 0x0100}, 2, RAM_SIZE - 2}, /* 基準: FBcc.W の変位が RAM の外 */
        {{0xF228, EXT_IN(FMT_X, 1, 0x22), 0x0010},
         3,
         RAM_SIZE - 4},                               /* EA の変位語が RAM の外 */
        {{0xF27A, PRED_EQ, 0x1234}, 3, RAM_SIZE - 4}, /* FTRAPcc.W のオペランドが RAM の外 */
    };
    uint32_t base = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c;
        mc68k_state_t *cpu = make_fpu(&c, MC68K_FPU_NONE_040);
        cpu->a[0] = DATA_AREA;
        run_at(&c, cpu, cases[i].pc, cases[i].w, cases[i].n);
        assert(cpu->last_exception_vector == VEC_BUS_ERROR);
        if (i == 0)
            base = cpu->a[7];
        else
            assert(cpu->a[7] == base);
        mc68k_destroy(cpu);
    }
}

/* cpID 1（0xF200〜0xF3FF）だけが FPU の handler。前後の
 * opcode（MMU・キャッシュ命令）には触れない。 */
static void test_fpu_opcode_range(void) {
    ctx_t c;
    mc68k_state_t *cpu = make(&c);
    for (uint32_t op = 0xF200; op < 0xF400; op++)
        assert(cpu->op_table[op] == cpu->op_table[0xF200]);
    assert(cpu->op_table[0xF1FF] != cpu->op_table[0xF200]);
    assert(cpu->op_table[0xF400] != cpu->op_table[0xF200]);
    mc68k_destroy(cpu);
}

/* 単精度・倍精度の入出力: 全バイトを検査する（倍精度は下位語も）。 */
static void test_single_double_memory_round_trip(void) {
    static const struct {
        unsigned fmt, len;
        mc68k_fp80_t value;
        uint8_t bytes[8];
    } cases[] = {
        {FMT_S, 4, {0x3FFF, 0x8123450000000000ull}, {0x3F, 0x81, 0x23, 0x45}},
        {FMT_D,
         8,
         {0x3FFF, 0x8000091A2B3C4800ull},
         {0x3F, 0xF0, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89}},
    };
    for (unsigned i = 0; i < 2; i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fill_sentinel(&c);
        fst(cpu)->fp[1] = cases[i].value;
        cpu->a[0] = BUF;
        RUN(&c, cpu, 0xF210, EXT_OUT(cases[i].fmt, 1)); /* FMOVE.<fmt> FP1,(A0) */
        for (unsigned b = 0; b < cases[i].len; b++)
            assert(c.ram[BUF + b] == cases[i].bytes[b]);
        assert_guard(&c, cases[i].len);
        assert(!cpu->fpu040->exc.valid);
        mc68k_destroy(cpu);

        cpu = make(&c);
        for (unsigned b = 0; b < cases[i].len; b++)
            c.ram[BUF + b] = cases[i].bytes[b];
        cpu->a[0] = BUF;
        RUN(&c, cpu, 0xF210, EXT_IN(cases[i].fmt, 1, 0x00)); /* FMOVE.<fmt> (A0),FP1 */
        assert(same(fst(cpu)->fp[1], cases[i].value));
        mc68k_destroy(cpu);
    }
}

/* 宛先が未対応データ型で例外になるとき、正常なソース（L/S/W/D/B）はそのまま ETEMP
 * に取り、タグは normalized。 */
static void test_unsupported_destination_captures_normal_source(void) {
    static const struct {
        unsigned fmt;
        uint32_t w[2];
        mc68k_fp80_t etemp;
    } cases[] = {
        {FMT_L, {0x00000005u, 0}, {0x4001, 0xA000000000000000ull}},
        {FMT_S, {0x40A00000u, 0}, {0x4001, 0xA000000000000000ull}},
        {FMT_W, {0x00050000u, 0}, {0x4001, 0xA000000000000000ull}},
        {FMT_D, {0x40140000u, 0x00000000u}, {0x4001, 0xA000000000000000ull}},
        {FMT_B, {0x05000000u, 0}, {0x4001, 0xA000000000000000ull}},
    };
    for (unsigned i = 0; i < 5; i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = fp(0x0000, 1); /* 宛先は extended の denormal（未対応データ型） */
        cpu->a[0] = BUF;
        const unsigned n = cases[i].fmt == FMT_D ? 2 : 1;
        for (unsigned k = 0; k < n; k++)
            wl(&c, BUF + 4 * k, cases[i].w[k]);
        RUN(&c, cpu, 0xF210, EXT_IN(cases[i].fmt, 0, 0x22)); /* FADD.<fmt> (A0),FP0 */
        const mc68k_fpu040_exc_t *e = &cpu->fpu040->exc;
        assert(e->valid && e->vector == VEC_UNSUPPORTED);
        assert(same(e->etemp, cases[i].etemp) && e->stag == 0);
        mc68k_destroy(cpu);
    }
}

/* 例外のフレームのタグ: 符号つきの無限大・NaN も無限大・NaN のタグ。 */
static void test_exception_tags_for_signed_specials(void) {
    static const struct {
        mc68k_fp80_t src;
        uint16_t ext;
        uint8_t enable, tag;
    } cases[] = {
        {{0x7FFF, 0x8000000000000000ull},
         0x0404,
         MC68K_FPU_EXC_OPERR,
         0}, /* FSQRT(+Inf) は例外なし（下で除く） */
        {{0xFFFF, 0x8000000000000000ull},
         0x0404,
         MC68K_FPU_EXC_OPERR,
         2}, /* FSQRT(-Inf): OPERR、タグは無限大 */
        {{0xFFFF, 0x8000000000000001ull},
         0x0422,
         MC68K_FPU_EXC_SNAN,
         3}, /* 負の signaling NaN */
        {{0x7FFF, 0x8000000000000001ull},
         0x0422,
         MC68K_FPU_EXC_SNAN,
         3}, /* 正の signaling NaN */
    };
    for (unsigned i = 0; i < 4; i++) {
        ctx_t c;
        mc68k_state_t *cpu = make(&c);
        fst(cpu)->fp[0] = ONE;
        fst(cpu)->fp[1] = cases[i].src;
        fst(cpu)->fpcr_enable = cases[i].enable;
        RUN(&c, cpu, 0xF200, cases[i].ext); /* <op> FP1,FP0 */
        const mc68k_fpu040_exc_t *e = &cpu->fpu040->exc;
        if (i == 0) { /* +Inf の平方根は +Inf（例外なし） */
            assert(!e->valid);
        } else {
            assert(e->valid && e->stag == cases[i].tag);
        }
        mc68k_destroy(cpu);
    }
}

int main(void) {
    test_classify_all_forms();
    test_single_denormal_bits();
    test_double_denormal_bits();
    test_fscc_all_registers();
    test_fdbcc_counter_bits();
    test_fscc_is_not_ftrapcc();
    test_fbcc_displacements();
    test_register_numbers_in_arithmetic();
    test_register_numbers_in_moves();
    test_control_register_numbers();
    test_fmovem_lists();
    test_save_restore_register_numbers();
    test_unsupported_register_sources();
    test_unimplemented_register_capture();
    test_nofpu_operand_bytes();
    test_bus_error_paths();
    test_fbcc_long_stops_after_high_word_fault();
    test_fbcc_does_not_fall_into_fscc();
    test_fdbcc_single_bit_counters();
    test_fdbcc_fault_keeps_counter();
    test_fscc_fault_does_not_write();
    test_t0_trace_on_taken_branches();
    test_an_restore_register_numbers();
    test_memory_operand_widths();
    test_fmovem_all_registers();
    test_byte_immediate_bits();
    test_write_stops_after_fault();
    test_read_stops_after_fault();
    test_ftrapcc_fault_pushes_only_bus_error();
    test_forced_precision_opmodes();
    test_compare_does_not_write_destination();
    test_negative_zero_to_float_formats();
    test_snan_to_memory();
    test_overflow_e1_ignores_previous_unrounded();
    test_to_ea_fault_keeps_state();
    test_nofpu_fbcc_all_predicates();
    test_nofpu_fault_pushes_only_bus_error();
    test_fpu_opcode_range();
    test_single_double_memory_round_trip();
    test_unsupported_destination_captures_normal_source();
    test_exception_tags_for_signed_specials();
    test_unimplemented_from_user_mode();
    test_unimplemented_frame_extent();
    test_unimplemented_frame_stops_at_first_fault();
    puts("test_fpu040_gaps: OK");
    return 0;
}
