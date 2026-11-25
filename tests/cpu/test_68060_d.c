/*
 * MC68060-D (#110) T-D1〜T-D5（+ locked RMW の bypass）。設計書 docs/mc68000-internals/reference/cpu_68060.md
 * §6、§10.1。 cache の有無は「ホストが RAM
 * を直接書き換えた後に旧値（stale）が見えるか」で観測する。 領域: 0x00xxxxxx =
 * 制御コード・stack・vector（cache-inhibited）、0x01xxxxxx = 観測対象。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define LO_SIZE     0x80000u
#define HI_BASE     0x01000000u
#define HI_SIZE     0x40000u
#define SSP_INIT    0x8000u
#define USP_INIT    0x7000u
#define RESET_PC    0x100u
#define TEST_PC     0x400u
#define HANDLER     0x3000u
#define SR_SUP      0x2700
#define SR_USER     0x0000
#define NO_VECTOR   0xffffu
#define RTE_OPCODE  0x4e73
#define NOP_OPCODE  0x4e71
#define JSR_A0      0x4e90
#define RTS_OPCODE  0x4e75
#define MOVEQ_D0    0x7000
#define VEC_ILLEGAL 4
#define VEC_PRIV    8

#define CACR_EDC   UINT32_C(0x80000000)
#define CACR_DPI   UINT32_C(0x10000000)
#define CACR_EIC   UINT32_C(0x00008000)
#define TC_P_BIT   UINT32_C(0x00004000)
#define TC_E_BIT   UINT32_C(0x00008000)
/* TTR: base[31:24] mask[23:16] E[15] S[14:13]=10(FC2 無視) CM[6:5]（060UM §4.1.3） */
#define TTR_LOW_CI UINT32_C(0x0000c040)
#define TTR_HI_WT  UINT32_C(0x0100c000)
#define TTR_HI_CB  UINT32_C(0x0100c020)
#define TTR_E_BIT  UINT32_C(0x00008000)

#define OPC_CINV(cc, ss)  ((uint16_t)(0xf400u | ((cc) << 6) | ((ss) << 3)))
#define OPC_CPUSH(cc, ss) ((uint16_t)(0xf420u | ((cc) << 6) | ((ss) << 3)))
#define CC_D              1u
#define CC_I              2u
#define CC_BOTH           3u
#define SS_LINE           1u
#define SS_PAGE           2u
#define SS_ALL            3u

#define V_MEM0 0x0e0e0e0eu
#define V_HOST 0x4057a1d5u
#define OLD_K  0x11u
#define NEW_K  0x22u

typedef struct {
    uint8_t lo[LO_SIZE];
    uint8_t hi[HI_SIZE];
    bool fail_w_on; /* fail_w_addr を含む long への write を bus error にする（持続） */
    uint32_t fail_w_addr;
} fixture_t;

static uint8_t *mem(fixture_t *f, uint32_t a, uint32_t n) {
    if ((uint64_t)a + n <= LO_SIZE)
        return f->lo + a;
    if (a >= HI_BASE && (uint64_t)(a - HI_BASE) + n <= HI_SIZE)
        return f->hi + (a - HI_BASE);
    return NULL;
}
static bool rb(void *c, uint32_t a, uint8_t *v) {
    uint8_t *p = mem(c, a, 1);
    if (!p)
        return false;
    *v = p[0];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    uint8_t *p = mem(c, a, 2);
    if (!p)
        return false;
    *v = (uint16_t)(p[0] << 8 | p[1]);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    uint8_t *p = mem(c, a, 4);
    if (!p)
        return false;
    *v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return true;
}
static bool write_blocked(const fixture_t *f, uint32_t a, uint32_t n) {
    return f->fail_w_on && a < f->fail_w_addr + 4u && a + n > f->fail_w_addr;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    uint8_t *p = mem(c, a, 1);
    if (!p || write_blocked(c, a, 1))
        return false;
    p[0] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    uint8_t *p = mem(c, a, 2);
    if (!p || write_blocked(c, a, 2))
        return false;
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return true;
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    uint8_t *p = mem(c, a, 4);
    if (!p || write_blocked(c, a, 4))
        return false;
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return true;
}
static void reset_peripheral(void *c) {
    (void)c;
}

static mc68k_state_t *cpu_g;
static fixture_t *f_g;

/* ホストが RAM を直接読み書きする。 */
static void h_word(uint32_t a, uint16_t v) {
    if (!ww(f_g, a, v))
        abort();
}
static void h_long(uint32_t a, uint32_t v) {
    if (!wl(f_g, a, v))
        abort();
}
static uint32_t h_get(uint32_t a) {
    uint32_t v;
    if (!rl(f_g, a, &v))
        abort();
    return v;
}

static uint32_t handler(int vec) {
    return HANDLER + (uint32_t)vec * 8u;
}

static void init_state(void) {
    memset(f_g, 0, sizeof *f_g);
    h_long(0, SSP_INIT);
    h_long(4, RESET_PC);
    for (int v = 2; v < 256; ++v) {
        h_long((uint32_t)v * 4u, handler(v));
        h_word(handler(v), RTE_OPCODE);
    }
    mc68k_reset(cpu_g);
    cpu_040_cache_reset(
        cpu_g); /* 060 の reset は cache を保持する（T-D5）ので試験が明示的に空にする */
    cpu_g->usp = USP_INIT;
    mc68k_set_irq(cpu_g, 0);
    cpu_g->irq_defer = false;
    cpu_g->last_exception_vector = NO_VECTOR;
    cpu_g->mmu060_tc = 0;
}

/* 制御コードの領域は cache-inhibited、観測領域は I が WT、D が dtt1。 */
static void setup_cache(uint32_t cacr_extra, uint32_t dtt1) {
    cpu_g->mmu060_itt0 = cpu_g->mmu060_dtt0 = TTR_LOW_CI;
    cpu_g->mmu060_itt1 = TTR_HI_WT;
    cpu_g->mmu060_dtt1 = dtt1;
    cpu_g->cacr = CACR_EDC | CACR_EIC | cacr_extra;
}

static void place(bool user, const uint16_t *w, int n) {
    for (int i = 0; i < n; ++i)
        h_word(TEST_PC + (uint32_t)i * 2u, w[i]);
    cpu_g->a[7] = SSP_INIT;
    cpu_g->pc = TEST_PC;
    cpu_set_sr(cpu_g, user ? SR_USER : SR_SUP);
    cpu_g->last_exception_vector = NO_VECTOR;
}
#define PLACE(user, ...)                                  \
    do {                                                  \
        const uint16_t w_[] = {__VA_ARGS__};              \
        place(user, w_, (int)(sizeof w_ / sizeof w_[0])); \
    } while (0)
#define STEP() mc68k_execute(cpu_g, 1)
#define RUN(user, ...)            \
    do {                          \
        PLACE(user, __VA_ARGS__); \
        STEP();                   \
    } while (0)

static uint16_t ram_w(uint32_t a) {
    uint16_t v;
    if (!rw(f_g, a, &v))
        abort();
    return v;
}
static uint32_t ram_l(uint32_t a) {
    return h_get(a);
}

static void expect_four_word(uint32_t base, int vec, uint16_t sr, uint32_t pc) {
    assert(cpu_g->last_exception_vector == vec);
    assert(cpu_g->pc == handler(vec));
    assert(cpu_g->a[7] == base);
    assert(ram_w(base) == sr);
    assert(ram_l(base + 2) == pc);
    assert(ram_w(base + 6) == (uint16_t)(vec * 4));
}

/* ---- 観測: D は long の read、I は MOVEQ #k,D0; RTS を JSR で呼んで D0 を見る ---- */
typedef enum { KIND_D, KIND_I } kind_t;

static uint32_t probe_i(uint32_t addr) {
    h_word(TEST_PC, JSR_A0);
    h_word(TEST_PC + 2, NOP_OPCODE);
    cpu_g->a[0] = addr;
    cpu_g->d[0] = 0xdeadu;
    cpu_g->a[7] = SSP_INIT;
    cpu_g->pc = TEST_PC;
    cpu_set_sr(cpu_g, SR_SUP);
    cpu_g->last_exception_vector = NO_VECTOR;
    STEP();
    STEP();
    STEP();
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    return cpu_g->d[0];
}
static uint32_t probe(kind_t k, uint32_t addr) {
    return k == KIND_D ? cpu_read_long(cpu_g, addr) : probe_i(addr);
}
static void prepare(kind_t k, uint32_t addr, uint32_t v) {
    if (k == KIND_D) {
        h_long(addr, v);
        return;
    }
    h_word(addr, (uint16_t)(MOVEQ_D0 | v));
    h_word(addr + 2, RTS_OPCODE);
}

static void exec_op(uint16_t op, uint32_t a0) {
    PLACE(false, op);
    cpu_g->a[0] = a0;
    STEP();
}

/* ---- T-D1: CACR 読み戻し、scope=00、容量・連想度・line サイズ ---- */
static void t_d1_cacr(void) {
    init_state();
    setup_cache(0, TTR_HI_WT); /* 制御コードの領域を cache-inhibited にしておく */
    cpu_g->d[0] = 0xffffffffu;
    RUN(false, 0x4e7b, 0x0002); /* MOVEC D0,CACR */
    RUN(false, 0x4e7a, 0x1002); /* MOVEC CACR,D1 */
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    assert(cpu_g->d[1] == 0xf880e000u); /* CABC/CUBC は 0 読み */
    cpu_g->d[0] = CACR_EDC | CACR_EIC | CACR_DPI | (1u << 22) | (1u << 21);
    RUN(false, 0x4e7b, 0x0002);
    RUN(false, 0x4e7a, 0x1002);
    assert(cpu_g->d[1] == (CACR_EDC | CACR_EIC | CACR_DPI));
}

static void t_d1_scope0(void) {
    for (unsigned push = 0; push < 2; ++push)
        for (unsigned cc = 1; cc <= 3; ++cc) {
            init_state();
            RUN(false, push ? OPC_CPUSH(cc, 0) : OPC_CINV(cc, 0));
            expect_four_word(SSP_INIT - 8, VEC_ILLEGAL, SR_SUP, TEST_PC);
        }
}

static void t_d1_geometry(kind_t k) {
    const uint32_t A = HI_BASE + 0x10000u;
    static const uint32_t intervals[] = {0x800u, 0x2000u};
    /* (i) 同じ set の 4 アドレスは全部 stale */
    for (unsigned n = 0; n < 2; ++n) {
        init_state();
        setup_cache(0, TTR_HI_WT);
        for (unsigned i = 0; i < 4; ++i) {
            prepare(k, A + i * intervals[n], OLD_K + i);
            (void)probe(k, A + i * intervals[n]);
        }
        for (unsigned i = 0; i < 4; ++i)
            prepare(k, A + i * intervals[n], NEW_K + i);
        for (unsigned i = 0; i < 4; ++i)
            assert(probe(k, A + i * intervals[n]) == OLD_K + i);
    }
    /* (ii) 5 アドレスのうち 1 本以上が追い出される */
    {
        init_state();
        setup_cache(0, TTR_HI_WT);
        for (unsigned i = 0; i < 5; ++i) {
            prepare(k, A + i * 0x800u, OLD_K + i);
            (void)probe(k, A + i * 0x800u);
        }
        for (unsigned i = 0; i < 5; ++i)
            prepare(k, A + i * 0x800u, NEW_K + i);
        unsigned fresh = 0;
        for (unsigned i = 0; i < 5; ++i)
            fresh += probe(k, A + i * 0x800u) == NEW_K + i;
        assert(fresh >= 1);
    }
    /* (iii) 連続する 8KB（512 line）が全部 stale */
    {
        init_state();
        setup_cache(0, TTR_HI_WT);
        for (unsigned i = 0; i < 512; ++i) {
            prepare(k, A + i * 16u, OLD_K + (i & 7u));
            (void)probe(k, A + i * 16u);
        }
        for (unsigned i = 0; i < 512; ++i)
            prepare(k, A + i * 16u, NEW_K + (i & 7u));
        for (unsigned i = 0; i < 512; ++i)
            assert(probe(k, A + i * 16u) == OLD_K + (i & 7u));
    }
}

/* (iv) line サイズ。A は 64 byte 境界。 */
static void t_d1_line_d(void) {
    const uint32_t A = HI_BASE + 0x10fc0u;
    init_state();
    setup_cache(0, TTR_HI_WT);
    h_long(A + 0x8, 0x11);
    h_long(A + 0xc, 0x22);
    h_long(A + 0x10, 0x33);
    h_long(A + 0x20, 0x44);
    h_long(A + 0x30, 0x55);
    (void)cpu_read_long(cpu_g, A);
    h_long(A + 0x8, 0x66);
    h_long(A + 0xc, 0x77);
    h_long(A + 0x10, 0x88);
    h_long(A + 0x20, 0x99);
    h_long(A + 0x30, 0xaa);
    assert(cpu_read_long(cpu_g, A + 0x8) == 0x11);
    assert(cpu_read_byte(cpu_g, A + 0xf) == 0x22);
    assert(cpu_read_long(cpu_g, A + 0x10) == 0x88);
    assert(cpu_read_long(cpu_g, A + 0x20) == 0x99);
    assert(cpu_read_long(cpu_g, A + 0x30) == 0xaa);
}
static void t_d1_line_i(void) {
    const uint32_t A = HI_BASE + 0x10fc0u;
    init_state();
    setup_cache(0, TTR_HI_WT);
    prepare(KIND_I, A + 0x8, 0x11);
    prepare(KIND_I, A + 0xc, 0x22);
    prepare(KIND_I, A + 0x10, 0x33);
    prepare(KIND_I, A + 0x20, 0x44);
    prepare(KIND_I, A + 0x30, 0x55);
    assert(probe_i(A + 0x8) == 0x11);
    assert(probe_i(A + 0xc) == 0x22);
    prepare(KIND_I, A + 0x8, 0x66);
    prepare(KIND_I, A + 0xc, 0x77);
    h_word(A + 0xe, (uint16_t)(MOVEQ_D0 | 0x7f)); /* A+0xe の RTS を MOVEQ に */
    prepare(KIND_I, A + 0x10, 0x58);
    prepare(KIND_I, A + 0x20, 0x59);
    prepare(KIND_I, A + 0x30, 0x2a);
    assert(probe_i(A + 0x8) == 0x11);
    assert(probe_i(A + 0xc) == 0x22); /* A+0xe も stale（RTS のまま） */
    assert(probe_i(A + 0x10) == 0x58);
    assert(probe_i(A + 0x20) == 0x59);
    assert(probe_i(A + 0x30) == 0x2a);
}

/* ---- 共通: 1 本の line に D の dirty と I の valid を置く ---- */
#define V_DIRTY(i) (0xd1000000u + (uint32_t)(i))
static void load_line(uint32_t line, unsigned idx) {
    h_long(line, V_MEM0);
    prepare(KIND_I, line + 8, OLD_K);
    assert(probe_i(line + 8) == OLD_K);
    cpu_write_long(cpu_g, line, V_DIRTY(idx));
    assert(h_get(line) == V_MEM0);
}
static void host_change(uint32_t line) {
    h_long(line, V_HOST);
    prepare(KIND_I, line + 8, NEW_K);
}

/* ---- T-D3: user の CINV/CPUSH は cc=00 を含め vector 8、何にも触れない ---- */
static void t_d3(void) {
    const uint32_t B = HI_BASE + 0x10000u, line = B + 0x40u;
    for (unsigned push = 0; push < 2; ++push)
        for (unsigned dpi = 0; dpi < 2; ++dpi)
            for (unsigned cc = 0; cc < 4; ++cc)
                for (unsigned ss = 1; ss < 4; ++ss)
                    for (unsigned user = 0; user < 2; ++user) {
                        if (user == 0 && cc != 0)
                            continue; /* supervisor の cc!=0 は T-D1/T-D4 */
                        init_state();
                        setup_cache(dpi ? CACR_DPI : 0, TTR_HI_CB);
                        load_line(line, 0);
                        uint16_t op = push ? OPC_CPUSH(cc, ss) : OPC_CINV(cc, ss);
                        PLACE(user, op);
                        cpu_g->a[0] = line + 4;
                        STEP();
                        if (user)
                            expect_four_word(SSP_INIT - 8, VEC_PRIV, SR_USER, TEST_PC);
                        else
                            assert(cpu_g->last_exception_vector == NO_VECTOR);
                        assert(cpu_g->a[0] == line + 4);
                        assert(h_get(line) == V_MEM0);
                        host_change(line);
                        assert(probe_i(line + 8) == OLD_K);
                        assert(cpu_read_long(cpu_g, line) == V_DIRTY(0));
                    }
}

/* ---- T-D4: supervisor の CPUSH の DPI と対象範囲 ---- */
enum { OP_L, OP_P, OP_A };
static bool in_range(unsigned op, bool p8k, uint32_t an, uint32_t line) {
    if (op == OP_A)
        return true;
    uint32_t mask = op == OP_L ? 0xfu : (p8k ? 0x1fffu : 0xfffu);
    return (line & ~mask) == (an & ~mask);
}

static void t_d4_case(unsigned op, unsigned cc, bool dpi, bool p8k, uint32_t an_off) {
    const uint32_t B = HI_BASE + 0x10000u;
    const uint32_t lines[] = {B + 0x40u, B + 0x100u, B + 0x1040u, B + 0x1100u, B + 0x2040u};
    const unsigned n = sizeof lines / sizeof lines[0];
    const uint32_t an = B + an_off;
    init_state();
    setup_cache(dpi ? CACR_DPI : 0, TTR_HI_CB);
    cpu_g->mmu060_tc = p8k ? TC_P_BIT : 0;
    for (unsigned i = 0; i < n; ++i)
        load_line(lines[i], i);
    exec_op(OPC_CPUSH(cc, op + 1), an);
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    assert(cpu_g->a[0] == an);
    bool d_cleaned[5];
    for (unsigned i = 0; i < n; ++i) {
        bool in = in_range(op, p8k, an, lines[i]);
        bool pushed = (cc & CC_D) && in;
        assert(h_get(lines[i]) == (pushed ? V_DIRTY(i) : V_MEM0));
        d_cleaned[i] = pushed && !dpi; /* DPI=0 の push は line を無効化する */
        host_change(lines[i]);
    }
    for (unsigned i = 0; i < n; ++i) {
        bool in = in_range(op, p8k, an, lines[i]);
        assert(probe_i(lines[i] + 8) == (((cc & CC_I) && in) ? NEW_K : OLD_K));
        assert(cpu_read_long(cpu_g, lines[i]) == (d_cleaned[i] ? V_HOST : V_DIRTY(i)));
    }
    /* DPI=0 の CPUSHA で dirty のまま残った line だけが再び RAM に書かれる */
    cpu_g->cacr &= ~CACR_DPI;
    exec_op(OPC_CPUSH(CC_D, SS_ALL), an);
    for (unsigned i = 0; i < n; ++i)
        assert(h_get(lines[i]) == (d_cleaned[i] ? V_HOST : V_DIRTY(i)));
}
static void t_d4(void) {
    for (unsigned op = OP_L; op <= OP_A; ++op)
        for (unsigned cc = 1; cc <= 3; ++cc)
            for (unsigned dpi = 0; dpi < 2; ++dpi)
                for (unsigned p = 0; p < 2; ++p) {
                    t_d4_case(op, cc, dpi, p, 0x44u);
                    t_d4_case(op, cc, dpi, p, 0x1044u);
                }
}

/* ---- T-D6: CINV は dirty を push しない。An の 8 レジスタすべてと、CINV/CPUSH の全組合せ ----
 */
static void exec_op_reg(uint16_t op, unsigned reg, uint32_t an) {
    PLACE(false, (uint16_t)(op | reg));
    cpu_g->a[reg] = an;
    STEP();
}
static void t_d6_case(bool push, unsigned reg, unsigned op, unsigned cc, bool dpi, bool p8k,
                      uint32_t an_off) {
    const uint32_t B = HI_BASE + 0x10000u;
    const uint32_t lines[] = {B + 0x40u, B + 0x100u, B + 0x1040u, B + 0x1100u, B + 0x2040u};
    const unsigned n = sizeof lines / sizeof lines[0];
    const uint32_t an = B + an_off;
    init_state();
    setup_cache(dpi ? CACR_DPI : 0, TTR_HI_CB);
    cpu_g->mmu060_tc = p8k ? TC_P_BIT : 0;
    for (unsigned i = 0; i < n; ++i)
        load_line(lines[i], i);
    const uint16_t opcode = push ? OPC_CPUSH(cc, op + 1) : OPC_CINV(cc, op + 1);
    PLACE(false, (uint16_t)(opcode | reg));
    cpu_g->a[reg] = an;
    if (reg == 7)
        cpu_g->usp = USP_INIT; /* A7 は SSP。supervisor で実行する */
    uint32_t sp_before = cpu_g->a[7];
    STEP();
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    assert(cpu_g->a[reg] == (reg == 7 ? sp_before : an));
    bool d_gone[5];
    for (unsigned i = 0; i < n; ++i) {
        bool in = in_range(op, p8k, an, lines[i]);
        bool pushed = push && (cc & CC_D) && in; /* CINV は push しない */
        assert(h_get(lines[i]) == (pushed ? V_DIRTY(i) : V_MEM0));
        d_gone[i] = (cc & CC_D) && in && !(push && dpi);
        host_change(lines[i]);
    }
    for (unsigned i = 0; i < n; ++i) {
        bool in = in_range(op, p8k, an, lines[i]);
        assert(probe_i(lines[i] + 8) == (((cc & CC_I) && in) ? NEW_K : OLD_K));
        assert(cpu_read_long(cpu_g, lines[i]) == (d_gone[i] ? V_HOST : V_DIRTY(i)));
    }
}
static void t_d6(void) {
    for (unsigned push = 0; push < 2; ++push)
        for (unsigned reg = 0; reg < 8; ++reg)
            for (unsigned op = OP_L; op <= OP_A; ++op)
                for (unsigned cc = 1; cc <= 3; ++cc)
                    for (unsigned dpi = 0; dpi < 2; ++dpi)
                        for (unsigned p = 0; p < 2; ++p) {
                            t_d6_case(push, reg, op, cc, dpi, p, 0x44u);
                            t_d6_case(push, reg, op, cc, dpi, p, 0x1044u);
                        }
    /* cache = 00・scope != 00 は何もしない */
    for (unsigned push = 0; push < 2; ++push)
        for (unsigned ss = 1; ss < 4; ++ss) {
            const uint32_t line = HI_BASE + 0x10040u;
            init_state();
            setup_cache(0, TTR_HI_CB);
            load_line(line, 0);
            exec_op_reg((uint16_t)((push ? OPC_CPUSH(0, ss) : OPC_CINV(0, ss))), 0, line);
            assert(cpu_g->last_exception_vector == NO_VECTOR);
            assert(h_get(line) == V_MEM0);
            host_change(line);
            assert(probe_i(line + 8) == OLD_K);
            assert(cpu_read_long(cpu_g, line) == V_DIRTY(0));
        }
}

/* ---- T-D6b: locked RMW（TAS）の bypass、060 以外の cache の geometry ---- */
static void t_d6_rmw(void) {
    const uint32_t line = HI_BASE + 0x10000u;
    /* line が無いアドレス: TAS は RAM を読み、書く */
    init_state();
    setup_cache(0, TTR_HI_CB);
    h_long(line, 0x12000000u);
    PLACE(false, 0x4ad0);
    cpu_g->a[0] = line;
    STEP();
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    assert(h_get(line) == 0x92000000u);
    assert(!(cpu_g->sr & 0x0004) && !(cpu_g->sr & 0x0008)); /* Z=0、N=0（読んだ値 $12） */
    /* clean な line に hit: TAS は RAM 上で実行され、line は無効になる */
    init_state();
    setup_cache(0, TTR_HI_CB);
    h_long(line, 0x12000000u);
    assert(cpu_read_long(cpu_g, line) == 0x12000000u);
    h_long(line, 0x34000000u); /* cache の外で書換え */
    PLACE(false, 0x4ad0);
    cpu_g->a[0] = line;
    STEP();
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    assert(h_get(line) == 0xb4000000u); /* RAM の $34 を読んで $B4 */
    h_long(line, V_HOST);
    assert(cpu_read_long(cpu_g, line) == V_HOST);
    /* dirty な line の push が失敗: access error。RAM の TAS 対象は触らない */
    init_state();
    setup_cache(0, TTR_HI_CB);
    h_long(line, V_MEM0);
    h_long(line + 4, 0x11111111u);
    h_long(line + 8, 0x22222222u);
    cpu_write_long(cpu_g, line, V_DIRTY(1));
    cpu_write_long(cpu_g, line + 4, V_DIRTY(2));
    f_g->fail_w_on = true;
    f_g->fail_w_addr = line + 4; /* push の 2 本目（dirty）で失敗 */
    PLACE(false, 0x4ad0);
    cpu_g->a[0] = line + 9;
    STEP();
    assert(cpu_g->last_exception_vector == 2);
    assert(((h_get(line + 8) >> 16) & 0xff) == 0x22); /* line + 9 は TAS されていない */
    f_g->fail_w_on = false;
    /* CPUSH の push が失敗: access error。失敗した line は dirty のまま残る */
    init_state();
    setup_cache(0, TTR_HI_CB);
    h_long(line, V_MEM0);
    h_long(line + 4, 0x11111111u);
    cpu_write_long(cpu_g, line, V_DIRTY(1));
    cpu_write_long(cpu_g, line + 4, V_DIRTY(2));
    f_g->fail_w_on = true;
    f_g->fail_w_addr = line + 4;
    exec_op(OPC_CPUSH(CC_D, SS_LINE), line);
    assert(cpu_g->last_exception_vector == 2);
    f_g->fail_w_on = false;
    host_change(line);
    assert(cpu_read_long(cpu_g, line) == V_DIRTY(1)); /* 無効化されていない */
}

static void t_d6_040_geometry(void) {
    /* 040 の cache は 64 set × 4 way。0x400 間隔の 5 アドレスは同じ set に入り、1
     * 本以上が追い出される */
    const uint32_t A = HI_BASE + 0x10000u;
    init_state();
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral,
                       .ctx = f_g};
    mc68k_state_t *c40 = mc68k_create_model(&bus, MC68K_MODEL_68040);
    assert(c40);
    c40->cacr = MC68K_CACR_040_DE;
    for (unsigned i = 0; i < 5; ++i) {
        h_long(A + i * 0x400u, OLD_K + i);
        (void)cpu_read_long(c40, A + i * 0x400u);
    }
    for (unsigned i = 0; i < 5; ++i)
        h_long(A + i * 0x400u, NEW_K + i);
    unsigned fresh = 0;
    for (unsigned i = 0; i < 5; ++i)
        fresh += cpu_read_long(c40, A + i * 0x400u) == NEW_K + i;
    assert(fresh >= 1);
    mc68k_destroy(c40);
}

/* CINV/CPUSH の An は物理アドレス。論理≠物理にマップして試す。 */
#define ROOT_S   0x10000u
#define PTR_S    0x10200u
#define PAGE_S   0x10400u
#define LOGICAL  0x02000000u
#define PHYSICAL (HI_BASE + 0x30000u)
#define DESC_RES 1u
#define DESC_TBL 3u
#define DESC_CB  0x20u
static void t_d4_physical(void) {
    init_state();
    setup_cache(0, TTR_HI_CB);
    h_long(ROOT_S + 4u * (LOGICAL >> 25), PTR_S | DESC_TBL);
    h_long(PTR_S + 4u * ((LOGICAL >> 18) & 0x7f), PAGE_S | DESC_TBL);
    h_long(PAGE_S + 4u * ((LOGICAL >> 12) & 0x3f), PHYSICAL | DESC_CB | DESC_RES);
    cpu_g->mmu060_srp = ROOT_S;
    cpu_g->mmu060_tc = TC_E_BIT;
    h_long(PHYSICAL, V_MEM0);
    cpu_write_long(cpu_g, LOGICAL, V_DIRTY(1));
    assert(h_get(PHYSICAL) == V_MEM0);
    exec_op(OPC_CPUSH(CC_D, SS_LINE), LOGICAL); /* 論理アドレスは一致する line がない */
    assert(h_get(PHYSICAL) == V_MEM0);
    exec_op(OPC_CINV(CC_D, SS_LINE), LOGICAL);
    h_long(PHYSICAL, V_HOST);
    assert(cpu_read_long(cpu_g, LOGICAL) == V_DIRTY(1)); /* line は残っている */
    exec_op(OPC_CPUSH(CC_D, SS_LINE), PHYSICAL);
    assert(h_get(PHYSICAL) == V_DIRTY(1));
    cpu_write_long(cpu_g, LOGICAL, V_DIRTY(2));
    exec_op(OPC_CINV(CC_D, SS_LINE), PHYSICAL); /* dirty は捨てる */
    h_long(PHYSICAL, V_HOST);
    assert(cpu_read_long(cpu_g, LOGICAL) == V_HOST);
}

/* ---- T-D2: MOVE16 ---- */
static void fill16(uint32_t a, uint32_t seed) {
    for (unsigned i = 0; i < 4; ++i)
        h_long(a + i * 4u, seed + i);
}
static void expect16(uint32_t a, uint32_t seed) {
    for (unsigned i = 0; i < 4; ++i)
        assert(h_get(a + i * 4u) == seed + i);
}
static void t_d2(void) {
    const uint32_t S = HI_BASE + 0x10100u, D = HI_BASE + 0x10200u;
    /* (Ax)+,(Ay)+ */
    init_state();
    setup_cache(0, TTR_HI_CB);
    fill16(S, 0x100);
    fill16(D, 0x900);
    PLACE(false, 0xf620, 0x9000);
    cpu_g->a[0] = S;
    cpu_g->a[1] = D;
    STEP();
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    expect16(D, 0x100);
    assert(cpu_g->a[0] == S + 16 && cpu_g->a[1] == D + 16);
    /* CACR.EDC=0（data cache 無効）の MOVE16 は line に触れない（[060UM §5.2]）: source の古い
     * line を使わず、 dirty な dest line を push も無効化もしない */
    init_state();
    setup_cache(0, TTR_HI_CB);
    fill16(S, 0x100);
    fill16(D, 0x900);
    assert(cpu_read_long(cpu_g, S) == 0x100u); /* source の clean な line */
    cpu_write_long(cpu_g, D, 0xd1d1d1d1u);     /* dest の dirty な line */
    fill16(S + 0, 0x200);                      /* RAM だけ書き換える */
    cpu_g->cacr &= ~CACR_EDC;
    PLACE(false, 0xf620, 0x9000);
    cpu_g->a[0] = S;
    cpu_g->a[1] = D;
    STEP();
    expect16(D, 0x200); /* RAM の source を読み、dest に書く */
    cpu_g->cacr |= CACR_EDC;
    assert(cpu_read_long(cpu_g, D) == 0xd1d1d1d1u); /* dirty な line は残っている */
    /* Ax = Ay: 増分は 1 回、line は自分自身にコピー */
    init_state();
    setup_cache(0, TTR_HI_CB);
    fill16(S, 0x100);
    PLACE(false, 0xf620, 0x8000);
    cpu_g->a[0] = S;
    STEP();
    expect16(S, 0x100);
    assert(cpu_g->a[0] == S + 16);
    /* 拡張語 bit15 = 0 は illegal */
    init_state();
    setup_cache(0, TTR_HI_CB);
    PLACE(false, 0xf620, 0x1000);
    cpu_g->a[0] = S;
    STEP();
    assert(cpu_g->last_exception_vector == VEC_ILLEGAL);
    /* 絶対番地の 4 構文。下位 4 bit は無視される */
    static const struct {
        uint16_t op;
        bool src_abs;
        bool inc;
    } forms[] = {{0xf601, false, true},
                 {0xf609, true, true},
                 {0xf611, false, false},
                 {0xf619, true, false}};
    for (unsigned i = 0; i < 4; ++i) {
        init_state();
        setup_cache(0, TTR_HI_CB);
        fill16(S, 0x100);
        fill16(D, 0x900);
        uint32_t abs_a = forms[i].src_abs ? S + 5u : D + 7u;
        PLACE(false, forms[i].op, (uint16_t)(abs_a >> 16), (uint16_t)abs_a);
        cpu_g->a[1] = forms[i].src_abs ? D + 3u : S + 9u;
        uint32_t a1 = cpu_g->a[1];
        STEP();
        assert(cpu_g->last_exception_vector == NO_VECTOR);
        expect16(D, 0x100);
        assert(cpu_g->a[1] == (forms[i].inc ? a1 + 16u : a1));
    }
    /* D-cache に allocate しない: source / dest とも、書換え後の読みが新値 */
    init_state();
    setup_cache(0, TTR_HI_CB);
    fill16(S, 0x100);
    fill16(D, 0x900);
    PLACE(false, 0xf620, 0x9000);
    cpu_g->a[0] = S;
    cpu_g->a[1] = D;
    STEP();
    h_long(S, V_HOST);
    h_long(D, V_HOST + 1);
    assert(cpu_read_long(cpu_g, S) == V_HOST);
    assert(cpu_read_long(cpu_g, D) == V_HOST + 1);
    /* source が cache に hit していれば cache（dirty）から読む */
    init_state();
    setup_cache(0, TTR_HI_CB);
    fill16(S, 0x100);
    fill16(D, 0x900);
    cpu_write_long(cpu_g, S, V_DIRTY(3));
    PLACE(false, 0xf620, 0x9000);
    cpu_g->a[0] = S;
    cpu_g->a[1] = D;
    STEP();
    assert(h_get(D) == V_DIRTY(3));
    assert(h_get(D + 4) == 0x101);
    /* dest が clean に hit: 無効化して外部書込（in-place 更新ではない） */
    init_state();
    setup_cache(0, TTR_HI_CB);
    fill16(S, 0x100);
    fill16(D, 0x900);
    (void)cpu_read_long(cpu_g, D);
    PLACE(false, 0xf620, 0x9000);
    cpu_g->a[0] = S;
    cpu_g->a[1] = D;
    STEP();
    h_long(D, V_HOST);
    assert(cpu_read_long(cpu_g, D) == V_HOST);
    /* dest が dirty に hit: push してから無効化し、MOVE16 のデータが RAM に残る */
    init_state();
    setup_cache(0, TTR_HI_CB);
    fill16(S, 0x100);
    fill16(D, 0x900);
    cpu_write_long(cpu_g, D + 4, V_DIRTY(4));
    PLACE(false, 0xf620, 0x9000);
    cpu_g->a[0] = S;
    cpu_g->a[1] = D;
    STEP();
    expect16(D, 0x100);
    assert(cpu_read_long(cpu_g, D + 4) == 0x101);
}

/* ---- T-D5: reset は I/D cache を保持する ---- */
static void t_d5(void) {
    const uint32_t LINE_D = HI_BASE + 0x10000u, LINE_I = HI_BASE + 0x10100u;
    init_state();
    setup_cache(0, TTR_HI_WT);
    h_long(LINE_D, 0x1234);
    prepare(KIND_I, LINE_I, 1);
    assert(cpu_read_long(cpu_g, LINE_D) == 0x1234);
    assert(probe_i(LINE_I) == 1);
    h_long(LINE_D, 0x5678);
    prepare(KIND_I, LINE_I, 2);
    mc68k_reset(cpu_g);
    assert(cpu_g->cacr == 0);
    assert(!(cpu_g->mmu060_itt0 & TTR_E_BIT) && !(cpu_g->mmu060_dtt1 & TTR_E_BIT));
    cpu_g->mmu060_itt0 = cpu_g->mmu060_dtt0 = TTR_LOW_CI;
    cpu_g->cacr = CACR_EDC | CACR_EIC;
    assert(cpu_read_long(cpu_g, LINE_D) == 0x1234);
    assert(probe_i(LINE_I) == 1);
}

/* ---- locked RMW（TAS）は D-cache に allocate せず、hit した dirty line は push して無効化する
 * ---- */
static void t_rmw(void) {
    const uint32_t line = HI_BASE + 0x10000u;
    init_state();
    setup_cache(0, TTR_HI_CB);
    h_long(line, 0);
    cpu_write_long(cpu_g, line, 0x12345678u);
    PLACE(false, 0x4ad0); /* TAS (A0) */
    cpu_g->a[0] = line;
    STEP();
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    assert(h_get(line) == 0x92345678u); /* dirty は push され、TAS は RAM 上で実行される */
    h_long(line, V_HOST);
    assert(cpu_read_long(cpu_g, line) == V_HOST); /* line は残っていない */
}

/* ---- T-D7: SFC/DFC=1・2・5・6 の MOVES は変換して通常の data アクセス、0・3・4・7 は変換せず
 * noncachable（[060UM §4.2.5、§7.7.1]）。 例外の vector fetch・RTE の stack 解放・例外の
 * stacking は miss で allocate しない（§7.7.1、§7.7.3）---- */
#define MOVES_L_READ_D0  0x0e90, 0x0000 /* MOVES.L (A0),D0 */
#define MOVES_L_WRITE_D0 0x0e90, 0x0800 /* MOVES.L D0,(A0) */
static void t_d7_moves(void) {
    const uint32_t L2 = HI_BASE + 0x20000u, P2 = HI_BASE + 0x28000u;
    for (unsigned fc = 0; fc < 8; ++fc) {
        const bool translated = fc == 1 || fc == 2 || fc == 5 || fc == 6;
        for (unsigned write = 0; write < 2; ++write) {
            init_state();
            setup_cache(0,
                        0); /* TTR なし。D-cache は有効（既定属性は writethrough cacheable） */
            h_long(ROOT_S + 4u * (L2 >> 25), PTR_S | DESC_TBL);
            h_long(PTR_S + 4u * ((L2 >> 18) & 0x7f), PAGE_S | DESC_TBL);
            h_long(PAGE_S + 4u * ((L2 >> 12) & 0x3f), P2 | DESC_CB | DESC_RES);
            cpu_g->mmu060_srp = cpu_g->mmu060_urp = ROOT_S;
            cpu_g->mmu060_tc = TC_E_BIT;
            h_long(L2, 0x11111111u); /* 変換しない場合の物理アドレス */
            h_long(P2, 0x22222222u); /* 変換する場合の物理アドレス */
            cpu_g->sfc = cpu_g->dfc = (uint8_t)fc;
            cpu_g->d[0] = 0x33333333u;
            if (!translated) { /* 物理 L2 の line を載せておく（MOVES が push/無効化する） */
                cpu_g->mmu060_tc = 0;
                assert(cpu_read_long(cpu_g, L2) == 0x11111111u);
                cpu_g->mmu060_tc = TC_E_BIT;
            }
            if (write) {
                PLACE(false, MOVES_L_WRITE_D0);
            } else {
                PLACE(false, MOVES_L_READ_D0);
            }
            cpu_g->a[0] = L2;
            STEP();
            assert(cpu_g->last_exception_vector == NO_VECTOR);
            if (translated) {
                /* 通常の data アクセス（TT=00）: cache を通り、通常の read/write と一貫する */
                cpu_g->sfc = cpu_g->dfc = 5;
                if (write) {
                    assert(cpu_read_long(cpu_g, L2) == 0x33333333u);
                } else {
                    assert(cpu_g->d[0] == 0x22222222u);
                    cpu_write_long(cpu_g, L2, 0x44444444u); /* dirty line */
                    PLACE(false, MOVES_L_READ_D0);
                    cpu_g->a[0] = L2;
                    STEP();
                    assert(cpu_g->d[0] == 0x44444444u);
                }
                continue;
            }
            /* alternate な空間（TT=10）: 変換せず、cache を通らない */
            if (write) {
                assert(h_get(L2) == 0x33333333u); /* 書込みは即 RAM */
                assert(h_get(P2) == 0x22222222u);
            } else {
                assert(cpu_g->d[0] == 0x11111111u);
            }
            h_long(L2, V_HOST);
            cpu_g->mmu060_tc = 0;
            assert(cpu_read_long(cpu_g, L2) == V_HOST); /* line を allocate していない */
        }
    }
}

/* 例外処理（vector fetch・stacking）と RTE の stack 解放は miss で allocate しない。hit
 * は通常どおり （[060UM §7.7.1、§7.7.3、§5.4.3]）。TRAP・割込み・access error の各経路で見る。
 */
typedef enum { EV_TRAP, EV_IRQ, EV_ACCESS_ERROR } ev_t;
static void ev_trigger(ev_t ev) {
    switch (ev) {
    case EV_TRAP:
        PLACE(false, 0x4e40);
        break; /* TRAP #0 */
    case EV_IRQ:
        PLACE(false, NOP_OPCODE);
        cpu_set_sr(cpu_g, 0x2000);
        mc68k_set_irq(cpu_g, 3);
        break;
    case EV_ACCESS_ERROR:
        PLACE(false, 0x2010);
        cpu_g->a[0] = 0x30000000u;
        break; /* MOVE.L (A0),D0 */
    }
    cpu_g->a[7] = HI_BASE + 0x20000u;
    cpu_g->irq_defer = false;
    STEP();
    mc68k_set_irq(cpu_g, 0);
}
/* 変換も TTR もない（恒等経路）でも、alternate な空間の MOVES は noncachable */
static void t_d7_moves_identity(void) {
    const uint32_t A = HI_BASE + 0x20000u;
    static const uint8_t alt[] = {0, 3, 4, 7};
    for (unsigned i = 0; i < sizeof alt; ++i)
        for (unsigned write = 0; write < 2; ++write) {
            init_state();
            setup_cache(0, 0);
            cpu_g->mmu060_itt0 = cpu_g->mmu060_itt1 = cpu_g->mmu060_dtt0 = cpu_g->mmu060_dtt1 =
                0;
            cpu_g->mmu060_tc = 0;
            h_long(A, 0x11111111u);
            assert(cpu_read_long(cpu_g, A) == 0x11111111u); /* 通常の read は line を載せる */
            cpu_g->sfc = cpu_g->dfc = alt[i];
            cpu_g->d[0] = 0x33333333u;
            if (write) {
                PLACE(false, MOVES_L_WRITE_D0);
            } else {
                PLACE(false, MOVES_L_READ_D0);
            }
            cpu_g->a[0] = A;
            h_long(A, V_HOST); /* RAM だけ書き換える */
            STEP();
            assert(cpu_g->last_exception_vector == NO_VECTOR);
            if (write) {
                assert(h_get(A) == 0x33333333u); /* 書込みは即 RAM */
            } else {
                assert(cpu_g->d[0] == V_HOST); /* cache の古い line を使わない */
            }
            h_long(A, V_HOST + 1u); /* noncachable に一致した line は無効化される */
            assert(cpu_read_long(cpu_g, A) == V_HOST + 1u);
        }
}

/* noncachable アクセスに一致した有効な line は、dirty なら push して無効化、clean
 * なら無効化する （[060UM §5.1 p.5-4、§5.4.2 p.5-8: CM=10・11 は同じ]）。data
 * アクセスだけ・CACR.DE=1 のときだけ 触るのは推論（設計書 §11） */
static void t_d7_noncachable_hit(void) {
    const uint32_t A = HI_BASE + 0x20000u;
    static const uint32_t ci[] = {UINT32_C(0x0100c040), UINT32_C(0x0100c060)}; /* CM=10・11 */
    for (unsigned c = 0; c < 2; ++c)
        for (unsigned write = 0; write < 2; ++write)
            for (unsigned dirty = 0; dirty < 2; ++dirty) {
                init_state();
                setup_cache(0, TTR_HI_CB);
                h_long(A, 0x11111111u);
                h_long(A + 8, 0x22222222u);
                if (dirty)
                    cpu_write_long(cpu_g, A,
                                   0x55555555u); /* copyback の dirty line（+0 だけ dirty） */
                else
                    assert(cpu_read_long(cpu_g, A) == 0x11111111u);
                cpu_g->mmu060_dtt1 = ci[c]; /* ここから noncachable */
                if (write) {
                    cpu_write_long(cpu_g, A + 8, 0x66666666u);
                    assert(h_get(A + 8) == 0x66666666u);
                } else {
                    assert(cpu_read_long(cpu_g, A + 8) == 0x22222222u);
                }
                assert(h_get(A) ==
                       (dirty ? 0x55555555u : 0x11111111u)); /* dirty は push 済み */
                cpu_g->mmu060_dtt1 = TTR_HI_CB;              /* 戻すと line は残っていない */
                h_long(A, V_HOST);
                h_long(A + 8, V_HOST + 1u);
                assert(cpu_read_long(cpu_g, A) == V_HOST);
                assert(cpu_read_long(cpu_g, A + 8) == V_HOST + 1u);
            }
    /* push が失敗したら access error で、外部アクセス（write）はしない。line は dirty のまま */
    for (unsigned write = 0; write < 2; ++write) {
        init_state();
        setup_cache(0, TTR_HI_CB);
        h_long(A + 4, 0x11111111u);
        h_long(A + 8, 0x22222222u);
        cpu_write_long(cpu_g, A + 4, 0x55555555u);
        cpu_g->mmu060_dtt1 = ci[0];
        f_g->fail_w_on = true;
        f_g->fail_w_addr = A + 4;
        cpu_g->d[0] = 0x66666666u;
        if (write) {
            PLACE(false, 0x2080);
        } else {
            PLACE(false, 0x2010);
        } /* MOVE.L D0,(A0) / MOVE.L (A0),D0 */
        cpu_g->a[0] = A + 8;
        STEP();
        assert(cpu_g->last_exception_vector == 2);
        f_g->fail_w_on = false;
        assert(h_get(A + 8) == 0x22222222u); /* 外部 write をしていない */
        cpu_g->mmu060_dtt1 = TTR_HI_CB;
        assert(cpu_read_long(cpu_g, A + 4) == 0x55555555u); /* line は dirty のまま */
    }
    /* CACR.DE=0 では line に触れない（推論）。命令 fetch は D-cache の line に触れない（推論）
     */
    init_state();
    setup_cache(0, TTR_HI_CB);
    h_long(A, 0x11111111u);
    cpu_write_long(cpu_g, A, 0x55555555u);
    cpu_g->mmu060_dtt1 = ci[0];
    cpu_g->cacr &= ~CACR_EDC;
    assert(cpu_read_long(cpu_g, A) == 0x11111111u);
    assert(h_get(A) == 0x11111111u);
    cpu_write_long(cpu_g, A + 8,
                   0x66666666u); /* write も line に触れない（dirty な A を push しない） */
    assert(h_get(A + 8) == 0x66666666u && h_get(A) == 0x11111111u);
    init_state();
    setup_cache(0, TTR_HI_CB);
    cpu_g->mmu060_itt1 = ci[0]; /* 命令側の HI 領域は cache-inhibited */
    h_word(A, NOP_OPCODE);
    h_long(A + 8, 0x22222222u);
    cpu_write_long(cpu_g, A + 8, 0x77777777u); /* 同じ line を data 側で dirty にする */
    cpu_g->a[7] = SSP_INIT;
    cpu_g->pc = A;
    cpu_set_sr(cpu_g, SR_SUP);
    STEP();
    assert(cpu_g->pc == A + 2 && h_get(A + 8) == 0x22222222u);
}

/* 040 の noncachable アクセスは line に触れない（060 だけの規則） */
static void t_d7_040_noncachable(void) {
    const uint32_t A = HI_BASE + 0x20000u;
    init_state();
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral,
                       .ctx = f_g};
    mc68k_state_t *c40 = mc68k_create_model(&bus, MC68K_MODEL_68040);
    assert(c40);
    c40->cacr = MC68K_CACR_040_DE;
    c40->mmu040_dtt1 = UINT32_C(0x0100c020); /* copyback */
    h_long(A, 0x11111111u);
    cpu_write_long(c40, A, 0x55555555u);
    c40->mmu040_dtt1 = UINT32_C(0x0100c040); /* cache-inhibited */
    assert(cpu_read_long(c40, A) == 0x11111111u);
    assert(h_get(A) == 0x11111111u);         /* 040 は push しない */
    cpu_write_long(c40, A + 8, 0x66666666u); /* write でも dirty な line を push しない */
    assert(h_get(A + 8) == 0x66666666u && h_get(A) == 0x11111111u);
    mc68k_destroy(c40);
}

static void t_d7_exception_no_allocate(void) {
    const uint32_t VBR_HI = HI_BASE + 0x30000u, STACK_HI = HI_BASE + 0x20000u;
    const uint32_t H1 = handler(40), H2 = handler(41);
    static const struct {
        ev_t ev;
        uint32_t vec, frame;
    } cases[] = {{EV_TRAP, 32, 8}, {EV_IRQ, 27, 8}, {EV_ACCESS_ERROR, 2, 16}};
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const uint32_t vaddr = VBR_HI + cases[c].vec * 4u, base = STACK_HI - cases[c].frame;
        for (unsigned hit = 0; hit < 2; ++hit) {
            init_state();
            setup_cache(0, TTR_HI_CB); /* HI 領域は copyback cacheable */
            cpu_g->vbr = VBR_HI;
            h_long(vaddr, H1);
            h_long(base, V_MEM0);
            h_word(H1, RTE_OPCODE);
            h_word(H2, RTE_OPCODE);
            if (hit) {
                assert(cpu_read_long(cpu_g, vaddr) ==
                       H1); /* vector と stack の line を cache に載せる */
                assert(cpu_read_long(cpu_g, base) == V_MEM0);
                h_long(vaddr, H2); /* RAM だけ書き換える */
            }
            ev_trigger(cases[c].ev);
            assert(cpu_g->pc == H1);
            if (hit) {
                /* hit は通常どおり: vector は cache の値、stacking は line に書いて RAM は不変
                 */
                assert(h_get(base) == V_MEM0);
                assert(cpu_read_long(cpu_g, base) != V_MEM0);
            } else {
                /* miss は allocate しない: stacking は RAM へ出て、書き換えた vector
                 * が次に見える */
                assert(h_get(base) != V_MEM0);
                h_long(vaddr, H2);
                ev_trigger(cases[c].ev);
                assert(cpu_g->pc == H2);
            }
        }
    }
    /* RTE の stack 解放の read は allocate しない */
    init_state();
    setup_cache(0, TTR_HI_CB);
    cpu_g->vbr = VBR_HI;
    h_long(VBR_HI + 32u * 4u, H1);
    h_word(H1, RTE_OPCODE);
    ev_trigger(EV_TRAP);
    STEP(); /* RTE */
    assert(cpu_g->pc == TEST_PC + 2 && cpu_g->a[7] == STACK_HI);
    h_long(STACK_HI - 8u, V_HOST);
    assert(cpu_read_long(cpu_g, STACK_HI - 8u) == V_HOST);
}

int main(void) {
    fixture_t *f = calloc(1, sizeof *f);
    assert(f);
    f_g = f;
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral,
                       .ctx = f};
    cpu_g = mc68k_create_model(&bus, MC68K_MODEL_68060);
    assert(cpu_g);
    t_d1_cacr();
    t_d1_scope0();
    t_d1_geometry(KIND_D);
    t_d1_geometry(KIND_I);
    t_d1_line_d();
    t_d1_line_i();
    t_d2();
    t_d3();
    t_d4();
    t_d6();
    t_d6_rmw();
    t_d7_moves();
    t_d7_moves_identity();
    t_d7_noncachable_hit();
    t_d7_040_noncachable();
    t_d7_exception_no_allocate();
    t_d6_040_geometry();
    t_d4_physical();
    t_d5();
    t_rmw();
    mc68k_destroy(cpu_g);
    free(f);
    return 0;
}
