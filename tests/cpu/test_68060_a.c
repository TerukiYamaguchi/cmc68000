/*
 * MC68060-A (#107) T-A1〜T-A8, T-A10。設計書 docs/mc68000-internals/reference/cpu_68060.md §10.1。
 * T-A9 は test_disasm_68060.c。test_68060.c は vector 61 のスタック frame と RTE を補う。
 */
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <string.h>

#define RAM_SIZE      0x20000u
#define HIGH_BASE     0x01000000u
#define HIGH_ALIAS    0x10000u
#define SSP_INIT      0x8000u
#define USP_INIT      0x7000u
#define RESET_PC      0x100u
#define TEST_PC       0x400u
#define DATA          0x1000u
#define HANDLER_BASE  0x3000u
#define FRAME_FORMAT0 8u
#define VEC_ILLEGAL   4
#define VEC_PRIV      8
#define VEC_LINEF     11
#define VEC_UNIMPL    61
#define NO_VECTOR     0xffffu /* last_exception_vector が更新されていない印 */
#define SR_SUP        0x2700
#define SR_USER       0x0000

typedef struct {
    uint8_t ram[RAM_SIZE];
    unsigned data_reads; /* DATA 以上の領域（データ領域）への read 回数 */
    unsigned reset_calls;
    unsigned cs_writes, cs_reads;
    uint32_t cs_value, cs_addr;
    uint8_t cs_size;
    bool cs_ok;
} fixture_t;

static uint32_t phys(uint32_t a) {
    return a >= HIGH_BASE ? a - HIGH_BASE + HIGH_ALIAS : a;
}
static bool rb(void *c, uint32_t a, uint8_t *v) {
    fixture_t *f = c;
    a = phys(a);
    if (a >= DATA && a < HANDLER_BASE)
        f->data_reads++;
    *v = f->ram[a];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    fixture_t *f = c;
    a = phys(a);
    if (a >= DATA && a < HANDLER_BASE)
        f->data_reads++;
    *v = (uint16_t)(f->ram[a] << 8 | f->ram[a + 1]);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    fixture_t *f = c;
    a = phys(a);
    if (a >= DATA && a < HANDLER_BASE)
        f->data_reads++;
    *v = (uint32_t)f->ram[a] << 24 | (uint32_t)f->ram[a + 1] << 16 |
         (uint32_t)f->ram[a + 2] << 8 | f->ram[a + 3];
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    ((fixture_t *)c)->ram[phys(a)] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    fixture_t *f = c;
    a = phys(a);
    f->ram[a] = (uint8_t)(v >> 8);
    f->ram[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    fixture_t *f = c;
    a = phys(a);
    f->ram[a] = (uint8_t)(v >> 24);
    f->ram[a + 1] = (uint8_t)(v >> 16);
    f->ram[a + 2] = (uint8_t)(v >> 8);
    f->ram[a + 3] = (uint8_t)v;
    return true;
}
static void reset_peripheral(void *c) {
    ((fixture_t *)c)->reset_calls++;
}
static bool cs_write(void *c, uint8_t fc, uint32_t a, uint8_t size, uint32_t v) {
    fixture_t *f = c;
    assert(fc == MC68K_FC_CPU_SPACE);
    f->cs_writes++;
    f->cs_addr = a;
    f->cs_size = size;
    f->cs_value = v;
    return f->cs_ok;
}
static bool cs_read(void *c, uint8_t fc, uint32_t a, uint8_t size, uint32_t *v) {
    fixture_t *f = c;
    assert(fc == MC68K_FC_CPU_SPACE);
    f->cs_reads++;
    f->cs_addr = a;
    f->cs_size = size;
    *v = 0;
    return f->cs_ok;
}
static uint8_t pk_b(void *c, uint32_t a) {
    return ((fixture_t *)c)->ram[phys(a)];
}
static uint16_t pk_w(void *c, uint32_t a) {
    return (uint16_t)(pk_b(c, a) << 8 | pk_b(c, a + 1));
}
static uint32_t pk_l(void *c, uint32_t a) {
    return (uint32_t)pk_w(c, a) << 16 | pk_w(c, a + 2);
}

static void word(fixture_t *f, uint32_t a, uint16_t v) {
    f->ram[a] = (uint8_t)(v >> 8);
    f->ram[a + 1] = (uint8_t)v;
}
static void lng(fixture_t *f, uint32_t a, uint32_t v) {
    word(f, a, (uint16_t)(v >> 16));
    word(f, a + 2, (uint16_t)v);
}
static uint16_t ram_w(const fixture_t *f, uint32_t a) {
    return (uint16_t)(f->ram[a] << 8 | f->ram[a + 1]);
}
static uint32_t ram_l(const fixture_t *f, uint32_t a) {
    return (uint32_t)ram_w(f, a) << 16 | ram_w(f, a + 2);
}

static uint32_t handler(int vec) {
    return HANDLER_BASE + (uint32_t)vec * 8u;
}

/* reset 後の状態に戻す。vector 表は固定（vector v → handler(v)）。 */
static void init_state(mc68k_state_t *cpu, fixture_t *f) {
    memset(f->ram, 0, sizeof f->ram);
    lng(f, 0, SSP_INIT);
    lng(f, 4, RESET_PC);
    for (int v = 2; v < 256; ++v)
        lng(f, (uint32_t)v * 4, handler(v));
    f->cs_ok = true;
    f->data_reads = 0;
    mc68k_reset(cpu);
    cpu->usp = USP_INIT;
}
static mc68k_state_t *make_cpu(fixture_t *f, mc68k_model_t model) {
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral,
                       .ctx = f,
                       .cpu_space_read = cs_read,
                       .cpu_space_write = cs_write};
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    assert(cpu);
    mc68k_set_peek_hooks(cpu, pk_b, pk_w, pk_l, f);
    init_state(cpu, f);
    return cpu;
}
/* 命令語列を TEST_PC に置き、指定モードで 1 命令実行する。 */
static void run(mc68k_state_t *cpu, fixture_t *f, bool user, const uint16_t *w, int n) {
    for (int i = 0; i < n; ++i)
        word(f, TEST_PC + (uint32_t)i * 2u, w[i]);
    cpu->pc = TEST_PC;
    cpu_set_sr(cpu, user ? SR_USER : SR_SUP);
    mc68k_execute(cpu, 1);
}
#define RUN(cpu, f, user, ...)                                  \
    do {                                                        \
        const uint16_t w_[] = {__VA_ARGS__};                    \
        run(cpu, f, user, w_, (int)(sizeof w_ / sizeof w_[0])); \
    } while (0)

/* format `$0` frame: SSP-8 に SR / PC / 0000|vec*4。stacked PC を返す。 */
static void expect_trap(const mc68k_state_t *cpu, const fixture_t *f, int vec,
                        uint32_t stacked_pc) {
    assert(cpu->last_exception_vector == vec);
    assert(cpu->pc == handler(vec));
    assert(cpu->a[7] == SSP_INIT - FRAME_FORMAT0);
    assert(ram_l(f, cpu->a[7] + 2) == stacked_pc);
    assert(ram_w(f, cpu->a[7] + 6) == (uint16_t)(vec * 4));
}
static void expect_no_trap(const mc68k_state_t *cpu, uint32_t next_pc) {
    assert(cpu->pc == next_pc);
    assert(cpu->last_exception_vector != VEC_UNIMPL);
}

/* ---- T-A1: decode 表の差分（060 vs 020）。期待集合は EA 分類関数を使わず記述する。 ---- */
static bool ea_is_control_spec(unsigned ea) {
    unsigned m = ea >> 3, r = ea & 7;
    return m == 2 || m == 5 || m == 6 || (m == 7 && r <= 3);
}
static void test_a1(mc68k_state_t *c060, fixture_t *f) {
    mc68k_state_t *c020 = mc68k_create_model(&(mc68k_bus_t){.read_byte = rb,
                                                            .read_word = rw,
                                                            .read_long = rl,
                                                            .write_byte = wb,
                                                            .write_word = ww,
                                                            .write_long = wl,
                                                            .ctx = f},
                                             MC68K_MODEL_68020);
    assert(c020);
    const op_handler_t *t6 = c060->op_table, *t2 = c020->op_table;
    const op_handler_t unimpl = t6[0x0108], line_f = t6[0xffff], illegal = t6[0x4afc];
    assert(unimpl != t2[0x0108] && unimpl != line_f && unimpl != illegal);
    unsigned diffs = 0;
    for (uint32_t op = 0; op < 0x10000; ++op) {
        bool movep = (op & 0xf138) == 0x0108;
        bool chk2 =
            (op & 0xf9c0) == 0x00c0 && ((op >> 9) & 3) != 3 && ea_is_control_spec(op & 0x3f);
        bool cas2 = op == 0x0cfc || op == 0x0efc;
        bool callm_rtm = (op & 0xffc0) == 0x06c0;
        bool atc = (op >= 0xf500 && op <= 0xf51f) || (op >= 0xf588 && op <= 0xf58f) ||
                   (op >= 0xf5c8 && op <= 0xf5cf);
        bool cache = (op >= 0xf400 && op <= 0xf4ff) || (op >= 0xf600 && op <= 0xf627);
        bool fpu = op >= 0xf200 && op <= 0xf3ff; /* cpID 1: FPU 構成で分かれる（060-E） */
        if (movep || chk2 || cas2) {
            assert(t6[op] == unimpl);
        } else if (callm_rtm) {
            assert(t6[op] == illegal);
        } else if (op == 0xf800) {
            assert(t6[op] != line_f && t6[op] != t2[op]);
        } else if (atc || cache || fpu) {
            assert(t6[op] != line_f && t6[op] != illegal);
        } else if (op >= 0xf000) {
            assert(t6[op] == line_f); /* CIR 入口・PTEST・cpID 2〜7 を含め F-line 未実装 */
        } else {
            assert(t6[op] == t2[op]);
        }
        diffs += t6[op] != t2[op];
    }
    assert(diffs > 0);
    mc68k_destroy(c020);
}

/* ---- T-A1 (実行): 登録の差分を user / supervisor の両方で走らせる ---- */
typedef struct {
    const char *name;
    int n;
    uint16_t w[3];
    int user_vec;
    int sup_vec;
} mode_case_t;
static void test_a1_modes(mc68k_state_t *cpu, fixture_t *f) {
    enum { NONE = -1 };
    static const mode_case_t cases[] = {
        {"MOVEP", 2, {0x0108, 0, 0}, VEC_UNIMPL, VEC_UNIMPL},
        {"CHK2.W", 2, {0x02d0, 0x0800, 0}, VEC_UNIMPL, VEC_UNIMPL},
        {"CMP2.L", 2, {0x04d0, 0x0000, 0}, VEC_UNIMPL, VEC_UNIMPL},
        {"CAS2.W", 3, {0x0cfc, 0x8001, 0x8001}, VEC_UNIMPL, VEC_UNIMPL},
        {"MULU.L 64bit", 2, {0x4c02, 0x1401, 0}, VEC_UNIMPL, VEC_UNIMPL},
        {"MULU.L 32bit", 2, {0x4c02, 0x1000, 0}, NONE, NONE},
        {"DIVU.L 32bit", 2, {0x4c42, 0x1003, 0}, NONE, NONE},
        {"CAS.W aligned", 2, {0x0cd0, 0x0081, 0}, NONE, NONE},
        {"CALLM", 2, {0x06d0, 0x0000, 0}, VEC_ILLEGAL, VEC_ILLEGAL},
        {"RTM", 1, {0x06c0, 0, 0}, VEC_ILLEGAL, VEC_ILLEGAL},
        {"PTESTW", 1, {0xf548, 0, 0}, VEC_LINEF, VEC_LINEF},
        {"PTESTR", 1, {0xf568, 0, 0}, VEC_LINEF, VEC_LINEF},
        {"cpID 2 CIR", 2, {0xf400 | 0x1c0, 0, 0}, VEC_LINEF, VEC_LINEF},
        {"PFLUSHA", 1, {0xf518, 0, 0}, VEC_PRIV, NONE},
        {"PFLUSH (A1)", 1, {0xf509, 0, 0}, VEC_PRIV, NONE},
        {"PFLUSHN (A1)", 1, {0xf501, 0, 0}, VEC_PRIV, NONE},
        {"PFLUSHAN", 1, {0xf510, 0, 0}, VEC_PRIV, NONE},
        {"PLPAR (A1)", 1, {0xf5c9, 0, 0}, VEC_PRIV, NONE},
        {"PLPAW (A1)", 1, {0xf589, 0, 0}, VEC_PRIV, NONE},
        {"LPSTOP", 3, {0xf800, 0x01c0, 0x2000}, VEC_PRIV, NONE},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        for (int user = 0; user <= 1; ++user) {
            const mode_case_t *c = &cases[i];
            int want = user ? c->user_vec : c->sup_vec;
            init_state(cpu, f);
            cpu->a[0] = DATA;
            cpu->a[1] = DATA;
            lng(f, DATA, 4);
            cpu->d[1] = 3;
            cpu->d[2] = 5;
            cpu->last_exception_vector = NO_VECTOR;
            run(cpu, f, user, c->w, c->n);
            if (want == NONE) {
                assert(cpu->last_exception_vector == NO_VECTOR);
                assert(!strcmp(c->name, "LPSTOP") || cpu->pc == TEST_PC + (uint32_t)c->n * 2u);
            } else {
                expect_trap(cpu, f, want, TEST_PC);
            }
        }
    }
}

/* ---- T-A2: F-line ---- */
static void test_a2(mc68k_state_t *cpu, fixture_t *f) {
    /* user/supervisor の両方で vector 11 / format $0（cpID 0 は privilege violation
     * にならない）。 CIR アクセス（cpu-space cycle）は出ない。 */
    static const uint16_t ops[] = {0xf000, 0xf1ff, 0xf200 | 0x3f, 0xf3ff, 0xf5ff, 0xf638,
                                   0xf83c, 0xfa00, 0xfc00,        0xfe00, 0xffff};
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; ++i) {
        for (int user = 0; user <= 1; ++user) {
            init_state(cpu, f);
            f->cs_reads = f->cs_writes = 0;
            RUN(cpu, f, user, ops[i], 0, 0);
            expect_trap(cpu, f, VEC_LINEF, TEST_PC);
            assert(f->cs_reads == 0 && f->cs_writes == 0);
        }
    }
}

/* ---- T-A3: MOVEC ---- */
static const uint16_t valid_cr[] = {0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006,
                                    0x007, 0x008, 0x800, 0x801, 0x806, 0x807, 0x808};
static bool is_valid_cr(uint16_t cr) {
    for (unsigned i = 0; i < sizeof valid_cr / sizeof valid_cr[0]; ++i)
        if (valid_cr[i] == cr)
            return true;
    return false;
}
static uint32_t cr_write_mask(uint16_t cr) {
    switch (cr) {
    case 0x000:
    case 0x001:
        return 7;
    case 0x002:
        return 0xf8e0e000u;
    case 0x003:
        return 0x0000fffeu;
    case 0x004:
    case 0x005:
    case 0x006:
    case 0x007:
        return 0xffffe364u;
    case 0x008:
        return 0xf0000000u;
    case 0x806:
    case 0x807:
        return 0xfffffe00u;
    case 0x808:
        return 0x83;
    default:
        return UINT32_MAX;
    }
}
static uint32_t cr_read_mask(uint16_t cr) {
    return cr == 0x002 ? 0xf880e000u : cr_write_mask(cr);
}

static void test_a3(mc68k_state_t *cpu, fixture_t *f) {
    for (uint32_t cr = 0; cr < 0x1000; ++cr) {
        init_state(cpu, f);
        cpu->d[0] = 0;
        RUN(cpu, f, false, 0x4e7a, (uint16_t)cr); /* MOVEC Rc,D0 */
        if (is_valid_cr((uint16_t)cr)) {
            assert(cpu->last_exception_vector != VEC_ILLEGAL && cpu->pc == TEST_PC + 4);
        } else {
            expect_trap(cpu, f, VEC_ILLEGAL, TEST_PC);
        }
        init_state(cpu, f);
        cpu->d[0] = UINT32_MAX;
        RUN(cpu, f, false, 0x4e7b, (uint16_t)cr); /* MOVEC D0,Rc */
        if (is_valid_cr((uint16_t)cr)) {
            assert(cpu->pc == TEST_PC + 4);
        } else {
            expect_trap(cpu, f, VEC_ILLEGAL, TEST_PC);
        }
    }
    for (unsigned i = 0; i < sizeof valid_cr / sizeof valid_cr[0]; ++i) {
        uint16_t cr = valid_cr[i];
        if (cr == 0x003 || cr == 0x801)
            continue; /* TC/VBR は下で個別に扱う */
        init_state(cpu, f);
        cpu->d[0] = UINT32_MAX;
        RUN(cpu, f, false, 0x4e7b, cr);
        cpu->d[1] = 0;
        RUN(cpu, f, false, 0x4e7a | 0, (uint16_t)(0x1000 | cr));
        /* PCR は ID $0430 とリビジョン 7（bit15-8）を固定で返す */
        uint32_t want = cr == 0x808 ? ((cr_write_mask(cr) & cr_read_mask(cr)) | 0x04300700u)
                                    : (cr_write_mask(cr) & cr_read_mask(cr));
        assert(cpu->d[1] == want);
    }
    /* TC / VBR */
    init_state(cpu, f);
    /* TC.E を立てるので、命令・データとも $00 領域を恒等変換する TTR を先に置く */
    cpu->mmu060_itt0 = cpu->mmu060_dtt0 = 0x0000c000u;
    cpu->d[0] = UINT32_MAX;
    RUN(cpu, f, false, 0x4e7b, 0x003);
    RUN(cpu, f, false, 0x4e7a, 0x1003);
    assert(cpu->d[1] == 0x0000fffeu);
    cpu->d[0] = 0;
    RUN(cpu, f, false, 0x4e7b, 0x003);
    init_state(cpu, f);
    cpu->d[0] = 0x1234fe00;
    RUN(cpu, f, false, 0x4e7b, 0x801);
    RUN(cpu, f, false, 0x4e7a, 0x1801);
    assert(cpu->d[1] == 0x1234fe00u);
    /* PCR: ID・revision は不変、bit7/1/0 だけが書ける。未実装ビットは 0 読み。 */
    static const uint32_t pcr_writes[] = {0, UINT32_MAX, 0x80, 0x02, 0x01, 0xffffff00u};
    for (unsigned i = 0; i < sizeof pcr_writes / sizeof pcr_writes[0]; ++i) {
        init_state(cpu, f);
        cpu->d[1] = 0;
        RUN(cpu, f, false, 0x4e7a, 0x1808);
        uint32_t before = cpu->d[1];
        assert((before >> 16) == 0x0430);
        cpu->d[0] = pcr_writes[i];
        RUN(cpu, f, false, 0x4e7b, 0x0808);
        cpu->d[1] = 0;
        RUN(cpu, f, false, 0x4e7a, 0x1808);
        assert((cpu->d[1] & 0xffffff00u) == (before & 0xffffff00u));
        assert((cpu->d[1] & 0x7c) == 0);
        assert((cpu->d[1] & 0x83) == (pcr_writes[i] & 0x83));
    }
}

/* ---- T-A4: vector 61 ---- */
static void expect_unimpl(mc68k_state_t *cpu, fixture_t *f, const uint16_t *w, int n) {
    init_state(cpu, f);
    cpu->d[2] = 7;
    cpu->a[0] = DATA;
    f->data_reads = 0;
    run(cpu, f, false, w, n);
    expect_trap(cpu, f, VEC_UNIMPL, TEST_PC);
    assert(f->data_reads == 0);
}
static void expect_direct(mc68k_state_t *cpu, fixture_t *f, const uint16_t *w, int n) {
    init_state(cpu, f);
    cpu->d[2] = 7;
    cpu->a[0] = DATA;
    lng(f, DATA, 0x00000004);
    run(cpu, f, false, w, n);
    expect_no_trap(cpu, TEST_PC + (uint32_t)n * 2u);
}
static void test_a4(mc68k_state_t *cpu, fixture_t *f) {
    /* MUL: ext = Dl<<12 | sign<<11 | size<<10 | Dh */
    for (unsigned sign = 0; sign <= 1; ++sign) {
        const uint16_t mul64[] = {0x4c02, (uint16_t)(0x0400 | sign << 11 | 1 << 12 | 3)};
        const uint16_t mul32[] = {0x4c02, (uint16_t)(sign << 11 | 1 << 12)};
        const uint16_t div64[] = {0x4c42, (uint16_t)(0x0400 | sign << 11 | 1 << 12 | 3)};
        const uint16_t div32[] = {0x4c42,
                                  (uint16_t)(sign << 11 | 1 << 12 | 3)}; /* DIVUL.L: Dr≠Dq */
        const uint16_t div32q[] = {0x4c42,
                                   (uint16_t)(sign << 11 | 1 << 12 | 1)}; /* DIVU.L: Dr=Dq */
        expect_unimpl(cpu, f, mul64, 2);
        expect_unimpl(cpu, f, div64, 2);
        expect_direct(cpu, f, mul32, 2);
        expect_direct(cpu, f, div32, 2);
        expect_direct(cpu, f, div32q, 2);
    }
    const uint16_t movep[4][2] = {{0x0108, 0}, {0x0148, 0}, {0x0188, 0}, {0x01c8, 0}};
    for (int i = 0; i < 4; ++i)
        expect_unimpl(cpu, f, movep[i], 2);
    for (unsigned sz = 0; sz < 3; ++sz) {
        const uint16_t chk2[] = {(uint16_t)(0x00d0 | sz << 9), 0x0800};
        const uint16_t cmp2[] = {(uint16_t)(0x00d0 | sz << 9), 0x0000};
        expect_unimpl(cpu, f, chk2, 2);
        expect_unimpl(cpu, f, cmp2, 2);
    }
    const uint16_t cas2w[] = {0x0cfc, 0x8001, 0x8001};
    const uint16_t cas2l[] = {0x0efc, 0x8001, 0x8001};
    expect_unimpl(cpu, f, cas2w, 3);
    expect_unimpl(cpu, f, cas2l, 3);
    /* CAS: word は奇数、long は 4 で割り切れないアドレスで trap。byte は常に直接実行。 */
    for (uint32_t off = 0; off < 4; ++off) {
        const uint16_t casb[] = {0x0ad0, 0x0081};
        const uint16_t casw[] = {0x0cd0, 0x0081};
        const uint16_t casl[] = {0x0ed0, 0x0081};
        init_state(cpu, f);
        cpu->a[0] = DATA + off;
        f->data_reads = 0;
        run(cpu, f, false, casb, 2);
        expect_no_trap(cpu, TEST_PC + 4);
        init_state(cpu, f);
        cpu->a[0] = DATA + off;
        f->data_reads = 0;
        run(cpu, f, false, casw, 2);
        if (off & 1) {
            expect_trap(cpu, f, VEC_UNIMPL, TEST_PC);
            assert(f->data_reads == 0);
        } else
            expect_no_trap(cpu, TEST_PC + 4);
        init_state(cpu, f);
        cpu->a[0] = DATA + off;
        f->data_reads = 0;
        run(cpu, f, false, casl, 2);
        if (off) {
            expect_trap(cpu, f, VEC_UNIMPL, TEST_PC);
            assert(f->data_reads == 0);
        } else
            expect_no_trap(cpu, TEST_PC + 4);
    }
    /* (An)+ / -(An) の非整列 CAS は trap し、An は更新しない（ISP が命令を再デコードする） */
    static const struct {
        uint16_t op;
        uint32_t a0;
    } pm[] = {{0x0cd8, DATA + 1}, {0x0ed8, DATA + 2}, {0x0ce0, DATA + 3}, {0x0ee0, DATA + 6}};
    for (unsigned i = 0; i < sizeof pm / sizeof pm[0]; ++i) {
        const uint16_t cas[] = {pm[i].op, 0x0081};
        init_state(cpu, f);
        cpu->a[0] = pm[i].a0;
        f->data_reads = 0;
        run(cpu, f, false, cas, 2);
        expect_trap(cpu, f, VEC_UNIMPL, TEST_PC);
        assert(f->data_reads == 0 && cpu->a[0] == pm[i].a0);
    }
}

/* ---- T-A5: trace ---- */
static void test_a5(mc68k_state_t *cpu, fixture_t *f) {
    static const uint16_t movep[] = {0x0108, 0}, chk2[] = {0x00d0, 0x0800};
    const uint16_t *cases[] = {movep, chk2};
    for (int i = 0; i < 2; ++i) {
        init_state(cpu, f);
        cpu->a[0] = DATA;
        word(f, TEST_PC, cases[i][0]);
        word(f, TEST_PC + 2, cases[i][1]);
        cpu->pc = TEST_PC;
        cpu_set_sr(cpu, SR_SUP | SR_T1);
        mc68k_execute(cpu, 1);
        expect_trap(cpu, f, VEC_UNIMPL, TEST_PC);
        assert(cpu->sr & SR_S);
    }
}

/* ---- T-A6: LPSTOP / BKPT / PTEST / PLPA ---- */
static void test_a6(mc68k_state_t *cpu, fixture_t *f) {
    /* BKPT #0〜#7: cpu-space read をちょうど 1 回（addr 0, size 1）。TA/TEA どちらでも vector
     * 4。 */
    for (int n = 0; n < 8; ++n) {
        for (int ok = 0; ok <= 1; ++ok) {
            init_state(cpu, f);
            f->cs_reads = 0;
            f->cs_ok = ok;
            RUN(cpu, f, false, (uint16_t)(0x4848 | n));
            assert(f->cs_reads == 1 && f->cs_addr == 0 && f->cs_size == 1);
            expect_trap(cpu, f, VEC_ILLEGAL, TEST_PC);
        }
    }
    /* PTEST (An): vector 11 / format $0 */
    for (int an = 0; an < 8; ++an) {
        init_state(cpu, f);
        RUN(cpu, f, false, (uint16_t)(0xf548 | an));
        expect_trap(cpu, f, VEC_LINEF, TEST_PC);
        init_state(cpu, f);
        RUN(cpu, f, true, (uint16_t)(0xf568 | an));
        expect_trap(cpu, f, VEC_LINEF, TEST_PC);
    }
    /* LPSTOP: write ちょうど 1 回、下位 16 bit = imm。TEA でも停止。 */
    for (int ok = 0; ok <= 1; ++ok) {
        init_state(cpu, f);
        f->cs_writes = 0;
        f->cs_ok = ok;
        RUN(cpu, f, false, 0xf800, 0x01c0, 0x2000);
        assert(f->cs_writes == 1 && (f->cs_value & 0xffff) == 0x2000);
        assert(cpu->exec_state == CPU_STOPPED && cpu->pc == TEST_PC + 6);
    }
    /* trace 中の LPSTOP は停止せず、命令後に trace exception（vector 9）になる。 */
    init_state(cpu, f);
    f->cs_writes = 0;
    word(f, TEST_PC, 0xf800);
    word(f, TEST_PC + 2, 0x01c0);
    word(f, TEST_PC + 4, 0xa000);
    cpu->pc = TEST_PC;
    cpu_set_sr(cpu, SR_SUP | SR_T1);
    mc68k_execute(cpu, 1);
    assert(f->cs_writes == 1 && cpu->exec_state == CPU_NORMAL);
    assert(cpu->last_exception_vector == 9 && cpu->pc == handler(9));
    assert(ram_l(f, cpu->a[7] + 2) == TEST_PC + 6);
    /* user は privilege violation で、cycle も停止もしない */
    init_state(cpu, f);
    f->cs_writes = 0;
    RUN(cpu, f, true, 0xf800, 0x01c0, 0x2000);
    expect_trap(cpu, f, VEC_PRIV, TEST_PC);
    assert(f->cs_writes == 0 && cpu->exec_state == CPU_NORMAL);
    /* imm の S クリアは privilege violation */
    init_state(cpu, f);
    RUN(cpu, f, false, 0xf800, 0x01c0, 0x0000);
    assert(cpu->last_exception_vector == VEC_PRIV);
    /* 停止中の割込みで復帰 */
    init_state(cpu, f);
    lng(f, 25 * 4, handler(25));
    RUN(cpu, f, false, 0xf800, 0x01c0, 0x2000);
    assert(cpu->exec_state == CPU_STOPPED);
    mc68k_set_irq(cpu, 1);
    mc68k_execute(cpu, 1);
    assert(cpu->exec_state == CPU_NORMAL && cpu->pc == handler(25) &&
           (cpu->sr & 0x0700) == 0x0100);
    /* hook が NULL でも停止する */
    {
        fixture_t g = {0};
        mc68k_bus_t bus = {.read_byte = rb,
                           .read_word = rw,
                           .read_long = rl,
                           .write_byte = wb,
                           .write_word = ww,
                           .write_long = wl,
                           .ctx = &g};
        mc68k_state_t *c2 = mc68k_create_model(&bus, MC68K_MODEL_68060);
        init_state(c2, &g);
        RUN(c2, &g, false, 0xf800, 0x01c0, 0x2000);
        assert(c2->exec_state == CPU_STOPPED);
        init_state(c2, &g);
        RUN(c2, &g, false, 0x4848);
        expect_trap(c2, &g, VEC_ILLEGAL, TEST_PC);
        mc68k_destroy(c2);
    }
    /* PLPAR/PLPAW: supervisor では decode され（illegal/F-line にならない）、An は MMU
     * 無効で不変 */
    for (int an = 0; an < 8; ++an) {
        uint16_t ops[] = {(uint16_t)(0xf588 | an), (uint16_t)(0xf5c8 | an)};
        for (int i = 0; i < 2; ++i) {
            init_state(cpu, f);
            cpu->a[an] = 0x12345678;
            RUN(cpu, f, false, ops[i]);
            assert(cpu->pc == TEST_PC + 2 && cpu->a[an] == 0x12345678);
        }
    }
}

/* ---- T-A10: 特権命令（060-A で作れる行） ---- */
typedef struct {
    const char *name;
    int n;
    uint16_t w[3];
} priv_case_t;
static const priv_case_t priv_cases[] = {
    {"ANDI to SR", 2, {0x027c, 0x2000, 0}},
    {"ORI to SR", 2, {0x007c, 0x2015, 0}},
    {"EORI to SR", 2, {0x0a7c, 0x0015, 0}},
    {"MOVE from SR Dn", 1, {0x40c1, 0, 0}},
    {"MOVE from SR (A0)+", 1, {0x40d8, 0, 0}},
    {"MOVE from SR -(A0)", 1, {0x40e0, 0, 0}},
    {"MOVE to SR Dn", 1, {0x46c1, 0, 0}},
    {"MOVE to SR (A0)+", 1, {0x46d8, 0, 0}},
    {"MOVE An,USP", 1, {0x4e60, 0, 0}},
    {"MOVE USP,An", 1, {0x4e69, 0, 0}},
    {"MOVES.L D1,(A0)+", 2, {0x0e98, 0x1800, 0}},
    {"MOVES.L (A0)+,D1", 2, {0x0e98, 0x1000, 0}},
    {"MOVES.L D1,-(A0)", 2, {0x0ea0, 0x1800, 0}},
    {"MOVES.L -(A0),D1", 2, {0x0ea0, 0x1000, 0}},
    {"RESET", 1, {0x4e70, 0, 0}},
    {"RTE", 1, {0x4e73, 0, 0}},
    {"STOP", 2, {0x4e72, 0x2000, 0}},
    {"LPSTOP", 3, {0xf800, 0x01c0, 0x2000}},
};
#define PRIV_SENTINEL 0x5a

typedef struct {
    uint32_t d[8], a[7], usp, vbr, cacr, buscr, pcr, tc, sfc, dfc;
    uint8_t mem[0x200];
    unsigned resets, cs_writes;
} snap_t;
static void take(snap_t *s, const mc68k_state_t *cpu, const fixture_t *f) {
    memcpy(s->d, cpu->d, sizeof s->d);
    memcpy(s->a, cpu->a, sizeof s->a);
    s->usp = cpu->usp;
    s->vbr = cpu->vbr;
    s->cacr = cpu->cacr;
    s->buscr = cpu->buscr;
    s->pcr = cpu->pcr;
    s->tc = cpu->mmu040_tc;
    s->sfc = cpu->sfc;
    s->dfc = cpu->dfc;
    memcpy(s->mem, f->ram + DATA, sizeof s->mem);
    s->resets = f->reset_calls;
    s->cs_writes = f->cs_writes;
}
static void prime(mc68k_state_t *cpu, fixture_t *f) {
    init_state(cpu, f);
    memset(f->ram + DATA, PRIV_SENTINEL, 0x200);
    for (int i = 0; i < 8; ++i)
        cpu->d[i] = 0x11111111u * (uint32_t)(i + 1);
    cpu->d[0] = 0xa5a5a5a5u;
    cpu->d[1] = 0x2300; /* MOVE to SR: S 付きで mask も変わる値 */
    for (int i = 0; i < 7; ++i)
        cpu->a[i] = DATA + 0x80 + (uint32_t)i * 4;
    cpu->a[1] = 0xdeadbee0u;
    cpu->usp = USP_INIT;
    lng(f, USP_INIT, 0x00002700); /* user 用 stack 上の RTE frame（SR / PC / format 0） */
    word(f, USP_INIT, 0x2700);
    lng(f, USP_INIT + 2, 0x900);
    word(f, USP_INIT + 6, 0);
    lng(f, SSP_INIT, 0x2700);
    word(f, SSP_INIT, 0x2700);
    lng(f, SSP_INIT + 2, 0x900);
    word(f, SSP_INIT + 6, 0);
}
static void priv_user(mc68k_state_t *cpu, fixture_t *f, const priv_case_t *c, uint16_t cr) {
    snap_t before, after;
    prime(cpu, f);
    (void)cr;
    take(&before, cpu, f);
    run(cpu, f, true, c->w, c->n);
    take(&after, cpu, f);
    expect_trap(cpu, f, VEC_PRIV, TEST_PC);
    assert(ram_w(f, cpu->a[7]) == SR_USER);
    assert(cpu->exec_state == CPU_NORMAL);
    after.a[0] = before.a[0]; /* (A0) 系の更新を検出するため a[0] は下で個別に比較しない */
    assert(!memcmp(before.d, after.d, sizeof before.d));
    assert(!memcmp(&before.a[1], &after.a[1], sizeof before.a - sizeof before.a[0]));
    assert(cpu->a[0] == DATA + 0x80);
    assert(before.usp == after.usp && before.vbr == after.vbr && before.cacr == after.cacr &&
           before.buscr == after.buscr && before.pcr == after.pcr && before.tc == after.tc &&
           before.sfc == after.sfc && before.dfc == after.dfc);
    assert(!memcmp(before.mem, after.mem, sizeof before.mem));
    assert(before.resets == after.resets && before.cs_writes == after.cs_writes);
}
static void priv_sup(mc68k_state_t *cpu, fixture_t *f, const priv_case_t *c) {
    prime(cpu, f);
    cpu->last_exception_vector = NO_VECTOR;
    unsigned resets = f->reset_calls, cs = f->cs_writes;
    run(cpu, f, false, c->w, c->n);
    assert(cpu->last_exception_vector != VEC_PRIV);
    const char *nm = c->name;
    if (!strcmp(nm, "RESET"))
        assert(f->reset_calls == resets + 1);
    else if (!strcmp(nm, "LPSTOP"))
        assert(f->cs_writes == cs + 1 && cpu->exec_state == CPU_STOPPED);
    else if (!strcmp(nm, "STOP"))
        assert(cpu->exec_state == CPU_STOPPED);
    else if (!strcmp(nm, "RTE"))
        assert(cpu->pc == 0x900);
    else
        assert(cpu->pc == TEST_PC + (uint32_t)c->n * 2u);
}
static void test_a10(mc68k_state_t *cpu, fixture_t *f) {
    for (unsigned i = 0; i < sizeof priv_cases / sizeof priv_cases[0]; ++i) {
        priv_user(cpu, f, &priv_cases[i], 0);
        priv_sup(cpu, f, &priv_cases[i]);
    }
    /* MOVEC は有効な 14 Cr の両方向。 */
    for (unsigned i = 0; i < sizeof valid_cr / sizeof valid_cr[0]; ++i) {
        for (int to_cr = 0; to_cr <= 1; ++to_cr) {
            priv_case_t c = {"MOVEC",
                             2,
                             {(uint16_t)(to_cr ? 0x4e7b : 0x4e7a),
                              (uint16_t)(valid_cr[i] | (to_cr ? 0x0000 : 0x1000)), 0}};
            priv_user(cpu, f, &c, valid_cr[i]);
            prime(cpu, f);
            cpu->d[0] = cpu->d[1] = 0xffffffffu & ~0x3u;
            cpu->last_exception_vector = NO_VECTOR;
            run(cpu, f, false, c.w, c.n);
            assert(cpu->last_exception_vector != VEC_PRIV && cpu->pc == TEST_PC + 4);
        }
    }
}

/* ---- T-A7: 020+ 機能の回帰 ---- */
static void test_a7(mc68k_state_t *cpu, fixture_t *f) {
    /* フル EA: MOVE.L (8,A0,D1.L*4),D2。EA = A0 + 8 + D1*4 */
    init_state(cpu, f);
    cpu->a[0] = DATA;
    cpu->d[1] = 3;
    lng(f, DATA + 8 + 12, 0xcafebabe);
    RUN(cpu, f, false, 0x2430, 0x1c08);
    assert(cpu->d[2] == 0xcafebabe && cpu->pc == TEST_PC + 4);
    /* 32bit Bcc / BSR */
    init_state(cpu, f);
    RUN(cpu, f, false, 0x60ff, 0x0000, 0x0100);
    assert(cpu->pc == TEST_PC + 2 + 0x100);
    init_state(cpu, f);
    RUN(cpu, f, false, 0x61ff, 0x0001, 0x0000);
    assert(cpu->pc == TEST_PC + 2 + 0x10000 && ram_l(f, cpu->a[7]) == TEST_PC + 6);
    /* MOVEM.L D0/A0,-(A0): 020+ は A0 の初期値を書く */
    init_state(cpu, f);
    cpu->a[0] = DATA + 0x20;
    cpu->d[0] = 0x11223344;
    RUN(cpu, f, false, 0x48e0, 0x8080);
    assert(cpu->a[0] == DATA + 0x20 - 8);
    assert(ram_l(f, DATA + 0x20 - 8) == 0x11223344);
    { /* A0 に書かれる値（初期値かデクリメント後か）は 020 と同じ */
        static fixture_t g;
        mc68k_state_t *c20 = make_cpu(&g, MC68K_MODEL_68020);
        c20->a[0] = DATA + 0x20;
        c20->d[0] = 0x11223344;
        RUN(c20, &g, false, 0x48e0, 0x8080);
        assert(ram_l(&g, DATA + 0x20 - 4) == ram_l(f, DATA + 0x20 - 4));
        assert(ram_l(f, DATA + 0x20 - 4) == DATA + 0x20 - 4);
        mc68k_destroy(c20);
    }
    /* データの非整列アクセスは例外にならない（word / long） */
    init_state(cpu, f);
    cpu->a[0] = DATA + 1;
    lng(f, DATA + 1, 0x01020304);
    RUN(cpu, f, false, 0x2010); /* MOVE.L (A0),D0 */
    assert(cpu->d[0] == 0x01020304 && cpu->pc == TEST_PC + 2);
    RUN(cpu, f, false, 0x3210); /* MOVE.W (A0),D1 */
    assert((cpu->d[1] & 0xffff) == 0x0102 && cpu->pc == TEST_PC + 2);
    /* debug physical read は 32bit アドレスを 24bit に丸めない */
    for (int i = 0; i < 4; ++i)
        f->ram[HIGH_ALIAS + 0x10 + i] = (uint8_t)(0xa0 + i);
    f->ram[0x10 + 0] = 0x55; /* 24bit に丸められると $000010 を読む */
    uint8_t buf[4] = {0};
    assert(mc68k_debug_read_physical_bytes(cpu, HIGH_BASE + 0x10, buf, 4) == 0);
    assert(buf[0] == 0xa0 && buf[3] == 0xa3);
}

/* ---- T-A8: 述語 ---- */
static void test_a8(mc68k_state_t *cpu) {
    mc68k_state_t probe = {0};
    static const struct {
        mc68k_model_t model;
        bool has_68010_isa;
        bool has_68020_isa;
    } cases[] = {
        {MC68K_MODEL_68000, false, false}, {MC68K_MODEL_68010, true, false},
        {MC68K_MODEL_68020, true, true},   {MC68K_MODEL_68EC030, true, true},
        {MC68K_MODEL_68030, true, true},   {MC68K_MODEL_68040, true, true},
        {MC68K_MODEL_68060, true, true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(model_has_68010_isa(cases[i].model) == cases[i].has_68010_isa);
        assert(model_has_68020_isa(cases[i].model) == cases[i].has_68020_isa);
        probe.model = cases[i].model;
        assert(cpu_is_68000(&probe) == (cases[i].model == MC68K_MODEL_68000));
        assert(cpu_is_68010(&probe) == (cases[i].model == MC68K_MODEL_68010));
        assert(cpu_is_68020(&probe) == (cases[i].model == MC68K_MODEL_68020));
        assert(cpu_is_68ec030(&probe) == (cases[i].model == MC68K_MODEL_68EC030));
        assert(cpu_is_68030(&probe) == (cases[i].model == MC68K_MODEL_68030));
        assert(cpu_is_68040(&probe) == (cases[i].model == MC68K_MODEL_68040));
        assert(cpu_is_68060(&probe) == (cases[i].model == MC68K_MODEL_68060));
    }
    assert(!cpu_is_68040(cpu) && !cpu_has_68040_cache(cpu) && !cpu_has_integrated_fpu(cpu));
    assert(cpu_has_68010_isa(cpu) && cpu_has_68020_isa(cpu));
    assert(!cpu_has_master_stack(cpu) && !cpu_has_t0_trace(cpu) &&
           !cpu_has_cpid0_privilege_check(cpu));
    assert(cpu_is_68060(cpu) && cpu_has_68060_unimpl_int(cpu));
}

int main(void) {
    static fixture_t f;
    mc68k_state_t *cpu = make_cpu(&f, MC68K_MODEL_68060);
    test_a1(cpu, &f);
    test_a1_modes(cpu, &f);
    test_a2(cpu, &f);
    test_a3(cpu, &f);
    test_a4(cpu, &f);
    test_a5(cpu, &f);
    test_a6(cpu, &f);
    test_a7(cpu, &f);
    test_a8(cpu);
    test_a10(cpu, &f);
    mc68k_destroy(cpu);
    return 0;
}
