/*
 * MC68060-C (#109) T-C1〜T-C10。設計書 docs/mc68000-internals/reference/cpu_68060.md §7、§10.1。
 * FSLW・frame の期待値は 060UM §8.4.4.3 の図から手で組み立て、実装の定数は使わない。
 * 通常 frame を積む途中の fault の最終挙動（HALT か access error か）は §11
 * の未確定なので固定しない。
 */
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <string.h>

#define RAM_SIZE   0x80000u
#define SSP_INIT   0x8000u
#define USP_INIT   0x7000u
#define RESET_PC   0x100u
#define TEST_PC    0x400u
#define DATA       0x1000u
#define HANDLER    0x3000u
#define TARGET     0x2000u
#define BAD_BASE   0x60000u /* ホスト bus error の領域 */
#define BAD_SIZE   0x1000u
#define SR_SUP     0x2700
#define SR_USER    0x0000
#define SR_T1_BIT  0x8000
#define SR_T0_BIT  0x4000
#define SR_M_BIT   0x1000
#define NO_VECTOR  0xffffu
#define RTE_OPCODE 0x4e73
#define NOP_OPCODE 0x4e71

#define VEC_BUS        2
#define VEC_ADDR       3
#define VEC_TRACE      9
#define VEC_RTE_FORMAT 14

/* 変換テーブル（物理）。user 用と supervisor 用の 2 本の木 */
#define ROOT_U           0x10000u
#define PTR_U            0x10200u
#define PAGE_U           0x10400u
#define ROOT_S           0x11000u
#define PTR_S            0x11200u
#define PAGE_S           0x11400u
#define INDIRECT_A       0x12000u
#define INDIRECT_B       0x12010u
#define PHYS_BASE        0x20000u
#define PHYS_PAGE        0x1000u
#define LOG_BASE         0x01000000u
#define LOG_PAGE         0x1000u
#define TTR_IDENTITY_LOW 0x0000c000u
#define TTR_LOG_WP       0x0100c004u /* LOG_BASE の上位 byte、E=1・S=any・W=1 */
#define NOT_IN_RAM       0x00100000u
#define PTR_INDEX        64u
#define DESC_RESIDENT    1u
#define DESC_INDIRECT    2u
#define DESC_TABLE       3u
#define DESC_W           0x04u
#define DESC_S           0x80u
#define TC_E             0x8000u
#define CODE_OFFSET      0x10u

/* FSLW（060UM Figure 8-6）の欄。実装とは独立に書く。 */
#define F_MA     (1u << 27)
#define F_LK     (1u << 25)
#define F_IO     (1u << 15)
#define F_PTA    (1u << 12)
#define F_PTB    (1u << 11)
#define F_IL     (1u << 10)
#define F_PF     (1u << 9)
#define F_SP     (1u << 8)
#define F_WP     (1u << 7)
#define F_TWE    (1u << 6)
#define F_RE     (1u << 5)
#define F_WE     (1u << 4)
#define F_TTR    (1u << 3)
#define RW_WRITE 1u
#define RW_READ  2u
#define RW_RMW   3u
#define SZ_BYTE  0u
#define SZ_WORD  1u
#define SZ_LONG  2u
#define FS(rw, size, tt, tm, flags)                                               \
    (((uint32_t)(rw) << 23) | ((uint32_t)(size) << 21) | ((uint32_t)(tt) << 19) | \
     ((uint32_t)(tm) << 16) | (uint32_t)(flags))

typedef struct {
    uint8_t ram[RAM_SIZE];
    bool bad_on;       /* BAD 領域の read/write を bus error にする */
    bool fail_w_armed; /* fail_w_addr への最初の write だけ bus error にする */
    uint32_t fail_w_addr;
    bool fail_r_on; /* fail_r_addr からの read を bus error にする（持続） */
    uint32_t fail_r_addr;
    bool fail_ws_on; /* fail_ws_addr への write を bus error にする（持続） */
    uint32_t fail_ws_addr;
} fixture_t;

static bool bad(const fixture_t *f, uint32_t a) {
    return f->bad_on && a >= BAD_BASE && a < BAD_BASE + BAD_SIZE;
}
static bool rd_ok(const fixture_t *f, uint32_t a, uint32_t n) {
    if (f->fail_r_on && a == f->fail_r_addr)
        return false;
    return a + n <= RAM_SIZE && !bad(f, a);
}
static bool wr_ok(fixture_t *f, uint32_t a, uint32_t n) {
    if (f->fail_ws_on && a == f->fail_ws_addr)
        return false;
    if (f->fail_w_armed && a == f->fail_w_addr) {
        f->fail_w_armed = false;
        return false;
    }
    return rd_ok(f, a, n);
}
static bool rb(void *c, uint32_t a, uint8_t *v) {
    fixture_t *f = c;
    if (!rd_ok(f, a, 1))
        return false;
    *v = f->ram[a];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    fixture_t *f = c;
    if (!rd_ok(f, a, 2))
        return false;
    *v = (uint16_t)(f->ram[a] << 8 | f->ram[a + 1]);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    fixture_t *f = c;
    if (!rd_ok(f, a, 4))
        return false;
    *v = (uint32_t)f->ram[a] << 24 | (uint32_t)f->ram[a + 1] << 16 |
         (uint32_t)f->ram[a + 2] << 8 | f->ram[a + 3];
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    fixture_t *f = c;
    if (!wr_ok(f, a, 1))
        return false;
    f->ram[a] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    fixture_t *f = c;
    if (!wr_ok(f, a, 2))
        return false;
    f->ram[a] = (uint8_t)(v >> 8);
    f->ram[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    fixture_t *f = c;
    if (!wr_ok(f, a, 4))
        return false;
    f->ram[a] = (uint8_t)(v >> 24);
    f->ram[a + 1] = (uint8_t)(v >> 16);
    f->ram[a + 2] = (uint8_t)(v >> 8);
    f->ram[a + 3] = (uint8_t)v;
    return true;
}
static void reset_peripheral(void *c) {
    (void)c;
}
static uint8_t pk_b(void *c, uint32_t a) {
    return a < RAM_SIZE ? ((fixture_t *)c)->ram[a] : 0xff;
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

static mc68k_state_t *cpu_g;
static fixture_t *f_g;

static uint32_t handler(int vec) {
    return HANDLER + (uint32_t)vec * 8u;
}

/* reset 後の状態へ。vector v → handler(v)、各 handler は RTE 1 命令。 */
static void init_state(mc68k_state_t *cpu, fixture_t *f) {
    memset(f->ram, 0, sizeof f->ram);
    f->bad_on = false;
    f->fail_w_armed = false;
    f->fail_r_on = false;
    f->fail_ws_on = false;
    lng(f, 0, SSP_INIT);
    lng(f, 4, RESET_PC);
    for (int v = 2; v < 256; ++v) {
        lng(f, (uint32_t)v * 4, handler(v));
        word(f, handler(v), RTE_OPCODE);
    }
    mc68k_reset(cpu);
    cpu->usp = USP_INIT;
    mc68k_set_irq(cpu, 0);
    cpu->irq_defer = false;
    cpu->last_exception_vector = NO_VECTOR;
}
static mc68k_state_t *make_cpu(fixture_t *f, mc68k_model_t model) {
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral,
                       .ctx = f};
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    assert(cpu);
    mc68k_set_peek_hooks(cpu, pk_b, pk_w, pk_l, f);
    init_state(cpu, f);
    return cpu;
}

/* 命令語列を TEST_PC に置き、SR を設定して PC・A7 を合わせる（実行はしない）。 */
static void place(bool user, uint16_t sr, const uint16_t *w, int n) {
    for (int i = 0; i < n; ++i)
        word(f_g, TEST_PC + (uint32_t)i * 2u, w[i]);
    cpu_g->a[7] = SSP_INIT;
    cpu_g->pc = TEST_PC;
    cpu_set_sr(cpu_g, user ? (uint16_t)(sr & ~(0x2000 | 0x1000 | 0x0700)) : sr);
    cpu_g->last_exception_vector = NO_VECTOR;
}
#define PLACE(user, sr, ...)                                  \
    do {                                                      \
        const uint16_t w_[] = {__VA_ARGS__};                  \
        place(user, sr, w_, (int)(sizeof w_ / sizeof w_[0])); \
    } while (0)
#define STEP() mc68k_execute(cpu_g, 1)
#define RUN(user, sr, ...)            \
    do {                              \
        PLACE(user, sr, __VA_ARGS__); \
        STEP();                       \
    } while (0)

/* format $4 frame: SSP から 16 byte。 */
static void expect_access_frame(uint32_t base, uint16_t sr, uint32_t pc, uint32_t addr,
                                uint32_t fslw) {
    assert(cpu_g->last_exception_vector == VEC_BUS);
    assert(cpu_g->pc == handler(VEC_BUS));
    assert(cpu_g->a[7] == base);
    assert(ram_w(f_g, base) == sr);
    assert(ram_l(f_g, base + 2) == pc);
    assert(ram_w(f_g, base + 6) == 0x4008);
    assert(ram_l(f_g, base + 8) == addr);
    assert(ram_l(f_g, base + 12) == fslw);
}
/* format $2 frame: 12 byte。 */
static void expect_six_word(uint32_t base, int vec, uint16_t sr, uint32_t pc, uint32_t addr) {
    assert(cpu_g->last_exception_vector == vec);
    assert(cpu_g->pc == handler(vec));
    assert(cpu_g->a[7] == base);
    assert(ram_w(f_g, base) == sr);
    assert(ram_l(f_g, base + 2) == pc);
    assert(ram_w(f_g, base + 6) == (uint16_t)(0x2000 | vec * 4));
    assert(ram_l(f_g, base + 8) == addr);
}
static void expect_four_word(uint32_t base, int vec, uint16_t sr, uint32_t pc) {
    assert(cpu_g->last_exception_vector == vec);
    assert(cpu_g->pc == handler(vec));
    assert(cpu_g->a[7] == base);
    assert(ram_w(f_g, base) == sr);
    assert(ram_l(f_g, base + 2) == pc);
    assert(ram_w(f_g, base + 6) == (uint16_t)(vec * 4));
}

/* ---- MMU 木 ---- */
typedef enum { SPACE_USER = 0, SPACE_SUP = 1 } space_t;
static uint32_t root_of(space_t s) {
    return s == SPACE_USER ? ROOT_U : ROOT_S;
}
static uint32_t ptr_of(space_t s) {
    return s == SPACE_USER ? PTR_U : PTR_S;
}
static uint32_t page_of(space_t s) {
    return s == SPACE_USER ? PAGE_U : PAGE_S;
}
static uint32_t log_addr(unsigned n) {
    return LOG_BASE + n * LOG_PAGE;
}
static uint32_t phys_of(unsigned slot) {
    return PHYS_BASE + slot * PHYS_PAGE;
}
static void build_tree(space_t s) {
    lng(f_g, root_of(s), ptr_of(s) | DESC_TABLE);
    lng(f_g, ptr_of(s) + PTR_INDEX * 4u, page_of(s) | DESC_TABLE);
}
static void set_page_desc(space_t s, unsigned n, uint32_t desc) {
    lng(f_g, page_of(s) + n * 4u, desc);
}
static void map_page(space_t s, unsigned n, unsigned slot, uint32_t flags) {
    set_page_desc(s, n, phys_of(slot) | flags | DESC_RESIDENT);
}
static void mmu_on(void) {
    memset(cpu_g->atc060_i, 0, sizeof cpu_g->atc060_i);
    memset(cpu_g->atc060_d, 0, sizeof cpu_g->atc060_d);
    cpu_g->mmu060_urp = ROOT_U;
    cpu_g->mmu060_srp = ROOT_S;
    cpu_g->mmu060_itt0 = cpu_g->mmu060_dtt0 = TTR_IDENTITY_LOW;
    cpu_g->mmu060_tc = TC_E;
}

static const mc68k_fault_ring_entry_t *last_ring(mc68k_fault_ring_entry_t *buf) {
    size_t n = mc68k_read_fault_ring(cpu_g, buf, 8);
    assert(n >= 1);
    return &buf[n - 1];
}

/* ---- T-C1: host bus error の FSLW と frame、restart、fault ring ---- */
static void host_case(bool user, uint16_t opcode, uint32_t a0, uint32_t fslw,
                      uint16_t stacked_sr) {
    init_state(cpu_g, f_g);
    f_g->bad_on = true;
    cpu_g->a[0] = a0;
    mc68k_set_fault_ring(cpu_g, 1);
    RUN(user, SR_SUP, opcode);
    expect_access_frame(SSP_INIT - 16, stacked_sr, TEST_PC, a0, fslw);
    mc68k_fault_ring_entry_t buf[8];
    const mc68k_fault_ring_entry_t *e = last_ring(buf);
    assert(e->vector == VEC_BUS && e->frame_format == 4);
    assert(e->ssw == (uint16_t)fslw && e->fault_addr == a0);
    assert(e->outcome == MC68K_FAULT_OUTCOME_VECTORED);
}

static void test_c1_host(void) {
    const uint32_t a = BAD_BASE + 0x10;
    host_case(false, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 5, F_RE), SR_SUP); /* MOVE.L (A0),D0 */
    host_case(true, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 1, F_RE), SR_USER);
    host_case(false, 0x3080, a, FS(RW_WRITE, SZ_WORD, 0, 5, F_WE), SR_SUP); /* MOVE.W D0,(A0) */
    host_case(true, 0x1080, a, FS(RW_WRITE, SZ_BYTE, 0, 1, F_WE), SR_USER); /* MOVE.B D0,(A0) */
    host_case(false, 0x4ad0, a, FS(RW_RMW, SZ_BYTE, 0, 5, F_RE | F_LK), SR_SUP); /* TAS (A0) */
    /* 非整列 long の 2 回目の cycle（BAD の 1 byte 手前から）で fault: MA、fault address
     * は先頭、SIZE は long */
    host_case(false, 0x2010, BAD_BASE - 1, FS(RW_READ, SZ_LONG, 0, 5, F_MA | F_RE), SR_SUP);
    host_case(false, 0x2080, BAD_BASE - 2, FS(RW_WRITE, SZ_LONG, 0, 5, F_MA | F_WE),
              SR_SUP); /* MOVE.L D0,(A0) */

    /* MOVES: [060UM Table 4-2]。DFC=1・2 は TT=0・TM=1、5・6 は TT=0・TM=5（命令空間は data
     * 空間へ変換）、 0・3・4・7 は TT=2・TM は DFC */
    static const struct {
        uint8_t dfc, tt, tm;
    } moves_fc[] = {{0, 2, 0}, {1, 0, 1}, {2, 0, 1}, {3, 2, 3},
                    {4, 2, 4}, {5, 0, 5}, {6, 0, 5}, {7, 2, 7}};
    for (unsigned i = 0; i < sizeof moves_fc / sizeof moves_fc[0]; ++i) {
        init_state(cpu_g, f_g);
        f_g->bad_on = true;
        cpu_g->a[0] = a;
        cpu_g->dfc = moves_fc[i].dfc;
        RUN(false, SR_SUP, 0x0e90, 0x0800); /* MOVES.L D0,(A0) */
        expect_access_frame(SSP_INIT - 16, SR_SUP, TEST_PC, a,
                            FS(RW_WRITE, SZ_LONG, moves_fc[i].tt, moves_fc[i].tm, F_WE));
    }

    /* TTR が一致したアクセスの host bus error は FSLW.TTR（TTR hit は translation fault
     * だけではない） */
    for (unsigned write = 0; write < 2; ++write) {
        init_state(cpu_g, f_g);
        f_g->bad_on = true;
        cpu_g->mmu060_dtt0 = UINT32_C(0x00ffc000); /* 全アドレス・E=1・S=10（FC2 無視） */
        cpu_g->a[0] = a;
        RUN(false, SR_SUP, write ? 0x2080 : 0x2010); /* MOVE.L D0,(A0) / MOVE.L (A0),D0 */
        expect_access_frame(
            SSP_INIT - 16, SR_SUP, TEST_PC, a,
            FS(write ? RW_WRITE : RW_READ, SZ_LONG, 0, 5, (write ? F_WE : F_RE) | F_TTR));
    }

    /* BUSCR.L=1 の間の fault は LK */
    init_state(cpu_g, f_g);
    f_g->bad_on = true;
    cpu_g->a[0] = a;
    cpu_g->buscr = 0x80000000u;
    RUN(false, SR_SUP, 0x2010);
    expect_access_frame(SSP_INIT - 16, SR_SUP, TEST_PC, a,
                        FS(RW_READ, SZ_LONG, 0, 5, F_RE | F_LK));
    /* 命令 prefetch の bus error: IO、TM=6 */
    init_state(cpu_g, f_g);
    f_g->bad_on = true;
    cpu_g->a[0] = BAD_BASE + 0x20;
    PLACE(false, SR_SUP, 0x4ed0); /* JMP (A0) */
    STEP();
    STEP();
    assert(cpu_g->last_exception_vector == VEC_BUS);
    expect_access_frame(SSP_INIT - 16, SR_SUP, BAD_BASE + 0x20, BAD_BASE + 0x20,
                        FS(RW_READ, SZ_WORD, 0, 6, F_IO | F_RE));
}

static void test_c1_restart(void) {
    /* MOVE.L D1,-(A0): predecrement を巻き戻して命令の先頭 PC を積み、RTE
     * で同じ命令を再実行する */
    init_state(cpu_g, f_g);
    f_g->bad_on = true;
    cpu_g->a[0] = BAD_BASE + 0x20;
    cpu_g->d[1] = 0xcafef00du;
    PLACE(false, SR_SUP, 0x2101);
    STEP();
    expect_access_frame(SSP_INIT - 16, SR_SUP, TEST_PC, BAD_BASE + 0x1c,
                        FS(RW_WRITE, SZ_LONG, 0, 5, F_WE));
    assert(cpu_g->a[0] == BAD_BASE + 0x20); /* 巻き戻し */
    f_g->bad_on = false;
    STEP(); /* handler の RTE */
    assert(cpu_g->pc == TEST_PC && cpu_g->a[7] == SSP_INIT);
    STEP();
    assert(cpu_g->pc == TEST_PC + 2 && cpu_g->a[0] == BAD_BASE + 0x1c);
    assert(ram_l(f_g, BAD_BASE + 0x1c) == 0xcafef00du);
}

static void mmu_case(bool user, uint16_t opcode, uint32_t a0, uint32_t fslw,
                     uint16_t stacked_sr) {
    cpu_g->a[0] = a0;
    cpu_g->last_exception_vector = NO_VECTOR;
    PLACE(user, SR_SUP, opcode);
    STEP();
    expect_access_frame(SSP_INIT - 16, stacked_sr, TEST_PC, a0, fslw);
}

static void test_c1_mmu(void) {
    const uint32_t a = log_addr(0) + 0x10;
    /* PTA: root descriptor 無効 */
    init_state(cpu_g, f_g);
    mmu_on();
    mmu_case(false, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 5, F_PTA), SR_SUP);
    /* PTB: pointer descriptor 無効 */
    init_state(cpu_g, f_g);
    mmu_on();
    lng(f_g, ROOT_S, PTR_S | DESC_TABLE);
    mmu_case(false, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 5, F_PTB), SR_SUP);
    /* PF: page descriptor 無効 */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    mmu_case(false, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 5, F_PF), SR_SUP);
    /* IL: indirect が indirect を指す */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    set_page_desc(SPACE_SUP, 0, INDIRECT_A | DESC_INDIRECT);
    lng(f_g, INDIRECT_A, INDIRECT_B | DESC_INDIRECT);
    mmu_case(false, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 5, F_IL), SR_SUP);
    /* indirect の先の page descriptor が無効なら PF（IL ではない） */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    set_page_desc(SPACE_SUP, 0, INDIRECT_A | DESC_INDIRECT);
    lng(f_g, INDIRECT_A, 0);
    mmu_case(false, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 5, F_PF), SR_SUP);
    /* SP: user が supervisor-only page を読む */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_USER);
    map_page(SPACE_USER, 0, 0, DESC_S);
    mmu_case(true, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 1, F_SP), SR_USER);
    /* WP: write-protected page への write（TTR は関与しない） */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    map_page(SPACE_SUP, 0, 0, DESC_W);
    mmu_case(false, 0x2080, a, FS(RW_WRITE, SZ_LONG, 0, 5, F_WP), SR_SUP);
    /* TWE: table search 中の bus error（LK も立つ） */
    init_state(cpu_g, f_g);
    mmu_on();
    cpu_g->mmu060_srp = NOT_IN_RAM;
    mmu_case(false, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 5, F_TWE | F_LK), SR_SUP);
    /* TTR: TTR が write protect の領域への write は WP と TTR */
    init_state(cpu_g, f_g);
    mmu_on();
    cpu_g->mmu060_dtt1 = TTR_LOG_WP;
    mmu_case(false, 0x2080, a, FS(RW_WRITE, SZ_LONG, 0, 5, F_WP | F_TTR), SR_SUP);
    /* RMW（TAS）の ATC fault は RW=11 で LK、RE/WE は立たない */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    mmu_case(false, 0x4ad0, a, FS(RW_RMW, SZ_BYTE, 0, 5, F_PF | F_LK), SR_SUP);

    /* PLPAR / PLPAW の失敗: DFC の空間・read/write の data access error として原因を FSLW
     * に載せる （直前の opcode fetch の属性ではない） */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    cpu_g->dfc = 5;
    mmu_case(false, 0xf5c8, a, FS(RW_READ, SZ_BYTE, 0, 5, F_PF), SR_SUP); /* PLPAR (A0) */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    map_page(SPACE_SUP, 0, 0, DESC_W);
    cpu_g->dfc = 5;
    mmu_case(false, 0xf588, a, FS(RW_WRITE, SZ_BYTE, 0, 5, F_WP), SR_SUP); /* PLPAW (A0) */
    init_state(cpu_g, f_g);
    mmu_on();
    cpu_g->dfc = 1;
    mmu_case(false, 0xf5c8, a, FS(RW_READ, SZ_BYTE, 0, 1, F_PTA), SR_SUP);

    /* 命令 opword の fault: IO、TM=6、stacked PC と fault address は分岐先 */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    cpu_g->a[0] = log_addr(3);
    PLACE(false, SR_SUP, 0x4ed0); /* JMP (A0) */
    STEP();
    STEP();
    expect_access_frame(SSP_INIT - 16, SR_SUP, log_addr(3), log_addr(3),
                        FS(RW_READ, SZ_WORD, 0, 6, F_IO | F_PF));

    /* 拡張語の fault: IO と MA。fault address は拡張語でなく opword（page の最後の語） */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    map_page(SPACE_SUP, 0, 0, 0);
    word(f_g, phys_of(0) + 0xffc, 0x203c); /* MOVE.L #imm,D0 */
    word(f_g, phys_of(0) + 0xffe, 0x1234);
    cpu_g->a[0] = log_addr(0) + 0xffc;
    PLACE(false, SR_SUP, 0x4ed0);
    STEP();
    STEP();
    expect_access_frame(SSP_INIT - 16, SR_SUP, log_addr(0) + 0xffc, log_addr(0) + 0xffc,
                        FS(RW_READ, SZ_WORD, 0, 6, F_IO | F_MA | F_PF));

    /* 非整列 long の 2 回目以降（page をまたぐ）: MA、fault address は先頭、SIZE は long */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    map_page(SPACE_SUP, 0, 0, 0);
    mmu_case(false, 0x2010, log_addr(0) + 0xffe, FS(RW_READ, SZ_LONG, 0, 5, F_MA | F_PF),
             SR_SUP);
    /* 最初の cycle で fault する場合は MA=0 */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    map_page(SPACE_SUP, 1, 1, 0);
    mmu_case(false, 0x2010, log_addr(0) + 0xffe, FS(RW_READ, SZ_LONG, 0, 5, F_PF), SR_SUP);

    /* fault ring の mmu 欄は 060 でも埋まる */
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    mc68k_set_fault_ring(cpu_g, 1);
    mmu_case(false, 0x2010, a, FS(RW_READ, SZ_LONG, 0, 5, F_PF), SR_SUP);
    mc68k_fault_ring_entry_t buf[8];
    const mc68k_fault_ring_entry_t *e = last_ring(buf);
    assert(e->mmu_valid == 1 && e->mmu_fault == MC68K_DEBUG_MMU_INVALID &&
           e->frame_format == 4);
    assert(e->tc == 0 && e->crp == 0 && e->srp == 0); /* 030 の欄は 060 では 0 */
}

/* ---- T-C2: address error は format $2 ---- */
static void test_c2(void) {
    mc68k_fault_ring_entry_t buf[8];
    /* JMP (A0) の奇数先。stacked PC は JMP、address は分岐先で A0=0 */
    init_state(cpu_g, f_g);
    mc68k_set_fault_ring(cpu_g, 1);
    cpu_g->a[0] = TARGET + 1;
    RUN(false, SR_SUP, 0x4ed0);
    expect_six_word(SSP_INIT - 12, VEC_ADDR, SR_SUP, TEST_PC, TARGET);
    const mc68k_fault_ring_entry_t *e = last_ring(buf);
    assert(e->vector == VEC_ADDR && e->frame_format == 2 && e->ssw == 0 &&
           e->fault_addr == TARGET);
    assert(e->outcome == MC68K_FAULT_OUTCOME_VECTORED);

    /* RTS の奇数先: pop は完了済み */
    init_state(cpu_g, f_g);
    lng(f_g, SSP_INIT - 4, TARGET + 3);
    PLACE(false, SR_SUP, 0x4e75);
    cpu_g->a[7] = SSP_INIT - 4;
    STEP();
    expect_six_word(SSP_INIT - 12, VEC_ADDR, SR_SUP, TEST_PC, TARGET + 2);

    /* user でも同じ（stacked SR は user のまま） */
    init_state(cpu_g, f_g);
    cpu_g->a[0] = TARGET + 1;
    RUN(true, SR_SUP, 0x4ed0);
    expect_six_word(SSP_INIT - 12, VEC_ADDR, SR_USER, TEST_PC, TARGET);

    /* RTE の PC が奇数: 新しい frame の SR は RTE 実行前、PC は RTE 命令、RTE の stack は pop
     * されない */
    init_state(cpu_g, f_g);
    const uint32_t base = SSP_INIT - 0x100;
    word(f_g, base, 0x2004);
    lng(f_g, base + 2, TARGET + 1);
    word(f_g, base + 6, 0x0000);
    PLACE(false, SR_SUP, RTE_OPCODE);
    cpu_g->a[7] = base;
    STEP();
    expect_six_word(base - 12, VEC_ADDR, SR_SUP, TEST_PC, TARGET);

    /* データの非整列は例外にならない */
    init_state(cpu_g, f_g);
    cpu_g->a[0] = DATA + 1;
    lng(f_g, DATA + 1, 0x01020304);
    RUN(false, SR_SUP, 0x2010);
    assert(cpu_g->d[0] == 0x01020304 && cpu_g->last_exception_vector == NO_VECTOR);
}

/* ---- T-C3: RTE の format 16 通り ---- */
static void test_c3(void) {
    for (unsigned fmt = 0; fmt < 16; ++fmt) {
        init_state(cpu_g, f_g);
        const uint32_t base = SSP_INIT - 0x40;
        word(f_g, base, 0x2004);
        lng(f_g, base + 2, TARGET);
        word(f_g, base + 6, (uint16_t)(fmt << 12 | 0x008));
        PLACE(false, SR_SUP, RTE_OPCODE);
        cpu_g->a[7] = base;
        STEP();
        if (fmt == 0 || fmt == 2 || fmt == 3 || fmt == 4) {
            const uint32_t size = fmt == 0 ? 8u : fmt == 4 ? 16u : 12u;
            assert(cpu_g->pc == TARGET && cpu_g->sr == 0x2004 && cpu_g->a[7] == base + size);
            assert(cpu_g->last_exception_vector == NO_VECTOR);
        } else {
            expect_four_word(base - 8, VEC_RTE_FORMAT, SR_SUP, TEST_PC);
        }
    }
}

/* ---- T-C4: SSP 1 本 ---- */
static void test_c4(void) {
    init_state(cpu_g, f_g);
    word(f_g, TEST_PC + 4, NOP_OPCODE);
    RUN(false, SR_SUP, 0x46fc, 0x3000);                     /* MOVE #$3000,SR */
    assert(cpu_g->sr == 0x3000 && cpu_g->a[7] == SSP_INIT); /* M 0→1 で A7 不変 */
    mc68k_set_irq(cpu_g, 5);
    STEP();
    assert(cpu_g->pc == handler(24 + 5));
    assert(cpu_g->a[7] == SSP_INIT - 8);        /* long_frame だけ */
    assert(ram_w(f_g, SSP_INIT - 8) == 0x3000); /* 積んだ SR に M=1 */
    assert(ram_l(f_g, SSP_INIT - 6) == TEST_PC + 4);
    assert(ram_w(f_g, SSP_INIT - 2) == (uint16_t)((24 + 5) * 4)); /* throwaway $1 なし */
    assert(cpu_g->sr == 0x2500);                                  /* S、M=0、mask 5 */
    STEP();                                                       /* handler の RTE */
    assert(cpu_g->sr == 0x3000 && cpu_g->a[7] == SSP_INIT && cpu_g->pc == TEST_PC + 4);
    /* M=1 のまま割込みを受けて RTE した後も A7 は SSP のまま、MOVE #$2000,SR で M=0
     * に戻しても不変 */
    PLACE(false, 0x3000, 0x46fc, 0x2000);
    cpu_g->pc = TEST_PC;
    STEP();
    assert(cpu_g->sr == 0x2000 && cpu_g->a[7] == SSP_INIT);

    /* M=1 の割込み受付で frame の write が fault しても access error は $4（throwaway $1 なし）
     */
    init_state(cpu_g, f_g);
    PLACE(false, 0x3000, NOP_OPCODE, NOP_OPCODE);
    mc68k_set_irq(cpu_g, 4);
    f_g->fail_w_armed = true;
    f_g->fail_w_addr = SSP_INIT - 6; /* PC 長語の書込み */
    STEP();                          /* IRQ 受付で fault が保留される */
    STEP();                          /* 次の実行で配送 */
    assert(cpu_g->last_exception_vector == VEC_BUS && cpu_g->pc == handler(VEC_BUS));
    assert(cpu_g->a[7] == SSP_INIT - 6 - 16);
    assert(ram_w(f_g, cpu_g->a[7] + 6) == 0x4008);
    assert(ram_w(f_g, cpu_g->a[7]) == 0x3000);      /* 受付前の SR */
    assert(ram_l(f_g, cpu_g->a[7] + 2) == TEST_PC); /* 次に実行する命令 */
}

/* ---- T-C5: trace 抑止 ---- */
typedef struct {
    const char *name;
    uint16_t w[4];
    int n;
    int vec;
    bool user;
    uint16_t sr_extra; /* CCR など */
    uint32_t d0, d1;
} trace_case_t;

static void test_c5(void) {
    static const trace_case_t cases[] = {
        {"TRAP", {0x4e40}, 1, 32, false, 0, 0, 0},
        {"CHK", {0x4380}, 1, 6, false, 0, 5, 0x10},
        {"DIVU.W #0", {0x80fc, 0x0000}, 2, 5, false, 0, 1, 0},
        {"TRAPV", {0x4e76}, 1, 7, false, 0x0002, 0, 0},
        {"TRAPT", {0x50fc}, 1, 7, false, 0, 0, 0},
        {"TRAPT.W", {0x50fa, 0x0000}, 2, 7, false, 0, 0, 0},
        {"TRAPT.L", {0x50fb, 0x0000, 0x0000}, 3, 7, false, 0, 0, 0},
        {"ILLEGAL", {0x4afc}, 1, 4, false, 0, 0, 0},
        {"privilege", {0x46fc, 0x0000}, 2, 8, true, 0, 0, 0},
        {"A-line", {0xa000}, 1, 10, false, 0, 0, 0},
        {"F-line", {0xf000, 0, 0}, 3, 11, false, 0, 0, 0},
        {"MOVEP", {0x0108, 0}, 2, 61, false, 0, 0, 0},
        {"CHK2", {0x00d0, 0x0800}, 2, 61, false, 0, 0, 0},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const trace_case_t *c = &cases[i];
        init_state(cpu_g, f_g);
        cpu_g->a[0] = DATA;
        cpu_g->d[0] = c->d0;
        cpu_g->d[1] = c->d1;
        place(c->user, (uint16_t)(SR_SUP | SR_T1_BIT | c->sr_extra), c->w, c->n);
        STEP();
        assert(cpu_g->last_exception_vector == c->vec);
        assert(cpu_g->pc == handler(c->vec));
    }
    /* RTE format error（group 3）: 不正な format の frame */
    init_state(cpu_g, f_g);
    word(f_g, SSP_INIT - 0x40, 0x2000);
    lng(f_g, SSP_INIT - 0x3e, TARGET);
    word(f_g, SSP_INIT - 0x3a, 0x7008);
    PLACE(false, SR_SUP | SR_T1_BIT, RTE_OPCODE);
    cpu_g->a[7] = SSP_INIT - 0x40;
    STEP();
    assert(cpu_g->last_exception_vector == VEC_RTE_FORMAT &&
           cpu_g->pc == handler(VEC_RTE_FORMAT));

    /* 対照: 例外のない命令は trace される */
    init_state(cpu_g, f_g);
    RUN(false, SR_SUP | SR_T1_BIT, NOP_OPCODE);
    assert(cpu_g->last_exception_vector == VEC_TRACE && cpu_g->pc == handler(VEC_TRACE));

    /* T0 のみ（T1=0）の分岐は trace されない。020 では trace される（対照） */
    init_state(cpu_g, f_g);
    RUN(false, SR_SUP | SR_T0_BIT, 0x6002); /* BRA.B +2 */
    assert(cpu_g->pc == TEST_PC + 4 && cpu_g->last_exception_vector == NO_VECTOR);
    {
        static fixture_t g;
        mc68k_state_t *c20 = make_cpu(&g, MC68K_MODEL_68020);
        word(&g, TEST_PC, 0x6002);
        c20->pc = TEST_PC;
        cpu_set_sr(c20, SR_SUP | SR_T0_BIT);
        mc68k_execute(c20, 1);
        assert(c20->last_exception_vector == VEC_TRACE);
        mc68k_destroy(c20);
    }
}

/* ---- T-C6: handler 先頭 1 命令の間は割込みを受け付けない ---- */
static void irq_defer_case(mc68k_state_t *cpu, fixture_t *f, bool expect_deferred) {
    cpu_g = cpu;
    f_g = f;
    init_state(cpu, f);
    word(f, handler(32), 0x46fc);
    word(f, handler(32) + 2, 0x2700); /* MOVE #$2700,SR */
    word(f, handler(32) + 4, NOP_OPCODE);
    PLACE(false, 0x2000, 0x4e40); /* TRAP #0 (mask 0) */
    STEP();
    assert(cpu->last_exception_vector == 32 && cpu->pc == handler(32));
    mc68k_set_irq(cpu, 3);
    STEP();
    if (expect_deferred) {
        assert(cpu->pc == handler(32) + 4 && cpu->last_exception_vector == 32);
        STEP(); /* mask 7 なので受け付けない */
        assert(cpu->pc == handler(32) + 6 && cpu->last_exception_vector == 32);
    } else {
        assert(cpu->pc == handler(24 + 3)); /* 対照 */
    }
}

/* access error の handler 先頭でも同じ */
static void irq_defer_after_access_error(void) {
    init_state(cpu_g, f_g);
    word(f_g, handler(VEC_BUS), 0x46fc);
    word(f_g, handler(VEC_BUS) + 2, 0x2700);
    word(f_g, handler(VEC_BUS) + 4, NOP_OPCODE);
    f_g->bad_on = true;
    cpu_g->a[0] = BAD_BASE + 0x10;
    RUN(false, 0x2000, 0x2010);
    assert(cpu_g->pc == handler(VEC_BUS));
    mc68k_set_irq(cpu_g, 3);
    STEP();
    assert(cpu_g->pc == handler(VEC_BUS) + 4 && cpu_g->last_exception_vector == VEC_BUS);
    STEP();
    assert(cpu_g->pc == handler(VEC_BUS) + 6 && cpu_g->last_exception_vector == VEC_BUS);
}

/* 先頭 1 命令が済めば割込みを受け付ける（遅延は 1 命令だけ） */
static void irq_defer_release(void) {
    init_state(cpu_g, f_g);
    word(f_g, handler(32), NOP_OPCODE);
    word(f_g, handler(32) + 2, NOP_OPCODE);
    PLACE(false, 0x2000, 0x4e40); /* TRAP #0 (mask 0) */
    STEP();
    assert(cpu_g->last_exception_vector == 32 && cpu_g->pc == handler(32));
    mc68k_set_irq(cpu_g, 3);
    STEP(); /* 先頭の NOP。割込みは待つ */
    assert(cpu_g->pc == handler(32) + 2 && cpu_g->last_exception_vector == 32);
    STEP();
    assert(cpu_g->pc == handler(24 + 3));
    mc68k_set_irq(cpu_g, 0);
}

static void test_c6(void) {
    mc68k_state_t *c60 = cpu_g;
    fixture_t *f60 = f_g;
    irq_defer_case(c60, f60, true);
    irq_defer_after_access_error();
    irq_defer_release();
    static fixture_t g;
    mc68k_state_t *c20 = make_cpu(&g, MC68K_MODEL_68020);
    irq_defer_case(c20, &g, false);
    mc68k_destroy(c20);
    cpu_g = c60;
    f_g = f60;
}

/* ---- T-C7: reset ---- */
static void test_c7(void) {
    init_state(cpu_g, f_g);
    cpu_g->mmu060_tc = 0x0000fffeu;
    cpu_g->mmu060_itt0 = cpu_g->mmu060_itt1 = cpu_g->mmu060_dtt0 = cpu_g->mmu060_dtt1 =
        0xffffe364u;
    cpu_g->cacr = 0xf8e0e000u;
    cpu_g->vbr = 0x1000;
    cpu_g->pcr = 0x83;
    cpu_g->buscr = 0xf0000000u;
    cpu_g->mmu060_urp = ROOT_U;
    cpu_g->mmu060_srp = ROOT_S;
    cpu_set_sr(cpu_g, 0x8000 | 0x2000 | 0x0100); /* T1・mask 1 */
    mc68k_reset(cpu_g);
    assert(cpu_g->mmu060_tc == 0);
    assert(cpu_g->mmu060_itt0 == 0xffff6364u && cpu_g->mmu060_itt1 == 0xffff6364u);
    assert(cpu_g->mmu060_dtt0 == 0xffff6364u && cpu_g->mmu060_dtt1 == 0xffff6364u);
    assert(cpu_g->cacr == 0 && cpu_g->vbr == 0 && cpu_g->pcr == 0 && cpu_g->buscr == 0);
    assert(cpu_g->sr == SR_SUP);
    assert(cpu_g->mmu060_urp == ROOT_U && cpu_g->mmu060_srp == ROOT_S); /* URP/SRP は保持 */
    assert(cpu_g->a[7] == SSP_INIT && cpu_g->pc == RESET_PC);
}

/* ---- T-C8: vector 5/6/7/9 は format $2、vector 13 は生成しない ---- */
static void test_c8(void) {
    init_state(cpu_g, f_g);
    cpu_g->d[0] = 1;
    RUN(false, SR_SUP, 0x80fc, 0x0000); /* DIVU.W #0,D0 */
    expect_six_word(SSP_INIT - 12, 5, SR_SUP, TEST_PC + 4, TEST_PC);
    init_state(cpu_g, f_g);
    cpu_g->d[0] = 5;
    cpu_g->d[1] = 0x10;
    RUN(false, SR_SUP, 0x4380); /* CHK.W D0,D1 */
    expect_six_word(SSP_INIT - 12, 6, SR_SUP, TEST_PC + 2, TEST_PC);
    init_state(cpu_g, f_g);
    RUN(false, SR_SUP | 0x0002, 0x4e76); /* TRAPV */
    expect_six_word(SSP_INIT - 12, 7, SR_SUP | 0x0002, TEST_PC + 2, TEST_PC);
    init_state(cpu_g, f_g);
    RUN(false, SR_SUP | SR_T1_BIT, NOP_OPCODE);
    expect_six_word(SSP_INIT - 12, VEC_TRACE, SR_SUP | SR_T1_BIT, TEST_PC + 2, TEST_PC);

    /* 通常 frame の push 中の fault は origin を EXCEPTION_FRAME として記録する。
     * その後 HALT か access error かは §11 の未確定なので固定しない。 */
    init_state(cpu_g, f_g);
    mc68k_set_fault_ring(cpu_g, 1);
    f_g->fail_w_armed = true;
    f_g->fail_w_addr = SSP_INIT - 2;
    RUN(false, SR_SUP, 0x4e40); /* TRAP #0 */
    mc68k_fault_ring_entry_t buf[8];
    const mc68k_fault_ring_entry_t *e = last_ring(buf);
    assert(e->origin == MC68K_FAULT_ORIGIN_EXCEPTION_FRAME && e->vector == VEC_BUS &&
           e->frame_format == 4);

    /* MMU 有効・TTR なし・table なしでは命令 fetch も frame の push も fault
     * する。再帰せずに止まる （HALT か否かは固定しない）。 */
    init_state(cpu_g, f_g);
    cpu_g->mmu060_tc = TC_E;
    cpu_g->mmu060_urp = ROOT_U;
    cpu_g->mmu060_srp = ROOT_S;
    memset(cpu_g->atc060_i, 0, sizeof cpu_g->atc060_i);
    memset(cpu_g->atc060_d, 0, sizeof cpu_g->atc060_d);
    cpu_g->pc = TEST_PC;
    cpu_g->a[7] = SSP_INIT;
    STEP();
    STEP();
    assert(cpu_g->exec_state == CPU_HALTED || cpu_g->exec_state == CPU_NORMAL);

    /* 全 opcode を supervisor で 1 回ずつ実行し、vector 12/13 は出ない */
    init_state(cpu_g, f_g);
    for (uint32_t op = 0; op < 0x10000u; ++op) {
        for (int i = 0; i < 8; ++i) {
            cpu_g->d[i] = 0;
            cpu_g->a[i] = DATA;
        }
        cpu_g->a[7] = SSP_INIT;
        cpu_g->exec_state = CPU_NORMAL;
        word(f_g, TEST_PC, (uint16_t)op);
        cpu_g->pc = TEST_PC;
        cpu_set_sr(cpu_g, SR_SUP);
        cpu_g->a[7] = SSP_INIT;
        cpu_g->last_exception_vector = NO_VECTOR;
        STEP();
        assert(cpu_g->last_exception_vector != 12 && cpu_g->last_exception_vector != 13);
    }
}

/* ---- T-C9: MOVEM は restart（040 の継続を使わない） ---- */
static void test_c9(void) {
    init_state(cpu_g, f_g);
    mmu_on();
    build_tree(SPACE_SUP);
    map_page(SPACE_SUP, 0, 0, 0); /* EA の先頭 2 語はここ */
    map_page(SPACE_SUP, 2, 2, 0); /* 書換え後の EA */
    cpu_g->a[0] = LOG_BASE;
    cpu_g->d[3] = 0xff8;
    cpu_g->d[0] = 0x11111111;
    cpu_g->d[1] = 0x22222222;
    cpu_g->d[2] = 0x33333333;
    PLACE(false, SR_SUP, 0x48f0, 0x0007, 0x3800); /* MOVEM.L D0-D2,(0,A0,D3.L) */
    STEP();
    /* 3 語目（page 1）が write fault */
    expect_access_frame(SSP_INIT - 16, SR_SUP, TEST_PC, LOG_BASE + 0xff8 + 8,
                        FS(RW_WRITE, SZ_LONG, 0, 5, F_PF));
    assert(cpu_g->d[3] == 0xff8 && cpu_g->d[2] == 0x33333333);
    /* handler が Xn を別の値へ書換えてから RTE。restart なら新しい Xn で EA を計算し直す */
    cpu_g->d[3] = 0x2000;
    STEP(); /* RTE */
    assert(cpu_g->pc == TEST_PC);
    STEP();
    assert(cpu_g->pc == TEST_PC + 6 && cpu_g->last_exception_vector == VEC_BUS);
    assert(ram_l(f_g, phys_of(2) + 0) == 0x11111111); /* 先頭語も新 EA */
    assert(ram_l(f_g, phys_of(2) + 4) == 0x22222222);
    assert(ram_l(f_g, phys_of(2) + 8) == 0x33333333);
}

/* ---- T-C10: BUSCR ---- */
static void test_c10(void) {
    /* 例外で L→SL、LE→SLE、L/LE をクリア */
    const struct {
        uint32_t before, after;
    } cases[] = {
        {0x80000000u, 0x40000000u}, {0x20000000u, 0x10000000u}, {0xa0000000u, 0x50000000u},
        {0x00000000u, 0x00000000u}, {0x40000000u, 0x00000000u}, {0x50000000u, 0x00000000u},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        init_state(cpu_g, f_g);
        cpu_g->d[0] = cases[i].before;
        RUN(false, SR_SUP, 0x4e7b, 0x0008); /* MOVEC D0,BUSCR */
        assert(cpu_g->buscr == cases[i].before);
        cpu_g->pc = TEST_PC;
        word(f_g, TEST_PC, 0x4e40);
        STEP(); /* TRAP #0 */
        assert(cpu_g->last_exception_vector == 32);
        assert(cpu_g->buscr == cases[i].after);
    }
    /* 割込み受付でも同じ */
    init_state(cpu_g, f_g);
    cpu_g->buscr = 0xa0000000u;
    PLACE(false, 0x2000, NOP_OPCODE);
    mc68k_set_irq(cpu_g, 3);
    STEP();
    assert(cpu_g->pc == handler(27) && cpu_g->buscr == 0x50000000u);
    /* ロック中の access error は FSLW.LK=1 で、BUSCR は退避される */
    init_state(cpu_g, f_g);
    f_g->bad_on = true;
    cpu_g->a[0] = BAD_BASE + 0x10;
    cpu_g->buscr = 0x80000000u;
    RUN(false, SR_SUP, 0x2010);
    expect_access_frame(SSP_INIT - 16, SR_SUP, TEST_PC, BAD_BASE + 0x10,
                        FS(RW_READ, SZ_LONG, 0, 5, F_RE | F_LK));
    assert(cpu_g->buscr == 0x40000000u);
    /* ロックしていなければ LK=0 */
    init_state(cpu_g, f_g);
    f_g->bad_on = true;
    cpu_g->a[0] = BAD_BASE + 0x10;
    RUN(false, SR_SUP, 0x2010);
    assert((ram_l(f_g, SSP_INIT - 4) & F_LK) == 0 && cpu_g->buscr == 0);
}

/* ---- T-C11: 変異試験で見つかった検出漏れの補強（reset の保持範囲、LPSTOP・MOVEC・CAS・RTE の
 * fault、 通常 frame の各語の書込み fault と vector 読出し fault） ---- */
static void expect_bus_error_frame(void) {
    assert(cpu_g->last_exception_vector == VEC_BUS);
    assert(cpu_g->pc == handler(VEC_BUS));
    assert(cpu_g->a[7] == SSP_INIT - 16 || cpu_g->a[7] < SSP_INIT);
    assert(ram_w(f_g, cpu_g->a[7] + 6) == 0x4008);
}

static void test_c11_reset(void) {
    /* reset が変えるのは TTR の E だけ。他のビットは（MOVEC では立たない位置も）保持する */
    init_state(cpu_g, f_g);
    cpu_g->mmu060_tc = 0xffffffffu;
    cpu_g->mmu060_itt0 = cpu_g->mmu060_itt1 = cpu_g->mmu060_dtt0 = cpu_g->mmu060_dtt1 =
        0xffffffffu;
    cpu_g->pcr = 0xffffffffu;
    cpu_g->buscr = 0xffffffffu;
    cpu_g->g0_no_rewind = true;
    cpu_g->irq_defer = true;
    mc68k_reset(cpu_g);
    assert(cpu_g->mmu060_tc == 0 && cpu_g->pcr == 0 && cpu_g->buscr == 0);
    assert(cpu_g->mmu060_itt0 == 0xffff7fffu && cpu_g->mmu060_itt1 == 0xffff7fffu);
    assert(cpu_g->mmu060_dtt0 == 0xffff7fffu && cpu_g->mmu060_dtt1 == 0xffff7fffu);
    assert(!cpu_g->g0_no_rewind && !cpu_g->irq_defer);

    /* 060 以外の I/D cache は reset で空になる（060 だけが保持する） */
    static fixture_t g;
    mc68k_state_t *c40 = make_cpu(&g, MC68K_MODEL_68040);
    c40->icache040[0][0].valid = true;
    c40->dcache040[1][2].valid = true;
    mc68k_reset(c40);
    assert(!c40->icache040[0][0].valid && !c40->dcache040[1][2].valid);
    mc68k_destroy(c40);
}

static void test_c11_lpstop(void) {
    /* 拡張語が $01C0 でなければ F-line（vector 11・format $0・PC = 命令先頭）。1
     * ビットずつ崩して全部 */
    for (unsigned b = 0; b < 16; ++b) {
        init_state(cpu_g, f_g);
        RUN(false, SR_SUP, 0xf800, (uint16_t)(0x01c0u ^ (1u << b)), 0x2000);
        expect_four_word(SSP_INIT - 8, 11, SR_SUP, TEST_PC);
        /* user でも F-line が privilege violation より先（Table 8-3 の 2.1 > 2.3）。$01C0 なら
         * vector 8 */
        init_state(cpu_g, f_g);
        RUN(true, SR_SUP, 0xf800, (uint16_t)(0x01c0u ^ (1u << b)), 0x2000);
        expect_four_word(SSP_INIT - 8, 11, SR_USER, TEST_PC);
    }
    init_state(cpu_g, f_g);
    RUN(true, SR_SUP, 0xf800, 0x01c0, 0x2000);
    expect_four_word(SSP_INIT - 8, 8, SR_USER, TEST_PC);
    /* 拡張語 / 即値の fetch fault は access error（F-line や LPSTOP の完了にならない） */
    for (unsigned k = 0; k < 2; ++k) {
        init_state(cpu_g, f_g);
        f_g->fail_r_on = true;
        f_g->fail_r_addr = TEST_PC + 2 + 2 * k;
        RUN(false, SR_SUP, 0xf800, 0x01c0, 0x2000);
        expect_bus_error_frame();
        assert(cpu_g->exec_state == CPU_NORMAL);
        assert(cpu_g->sr == SR_SUP);
    }
}

static void test_c11_movec(void) {
    init_state(cpu_g, f_g);
    cpu_g->sfc = 1;
    cpu_g->dfc = 5;
    RUN(false, SR_SUP, 0x4e7a, 0x0000); /* MOVEC SFC,D0 */
    assert(cpu_g->d[0] == 1);
    RUN(false, SR_SUP, 0x4e7a, 0x1001); /* MOVEC DFC,D1 */
    assert(cpu_g->d[1] == 5);
    cpu_g->d[0] = 0xfffffffbu;          /* 下位 3 bit だけ有効 */
    RUN(false, SR_SUP, 0x4e7b, 0x0000); /* MOVEC D0,SFC */
    assert(cpu_g->sfc == 3 && cpu_g->dfc == 5);
    cpu_g->d[0] = 0xfffffffeu;
    RUN(false, SR_SUP, 0x4e7b, 0x0001); /* MOVEC D0,DFC */
    assert(cpu_g->dfc == 6 && cpu_g->sfc == 3);
    /* CACR: 書込みで保持するビットと、読出しで見えるビットが違う */
    cpu_g->cacr = 0;
    cpu_g->d[0] = 0xffffffffu;
    RUN(false, SR_SUP, 0x4e7b, 0x0002); /* MOVEC D0,CACR */
    assert(cpu_g->cacr == 0xf8e0e000u);
    RUN(false, SR_SUP, 0x4e7a, 0x1002); /* MOVEC CACR,D1 */
    assert(cpu_g->d[1] == 0xf880e000u);
}

static void test_c11_cas(void) {
    /* 060: 非整列の word/long は operand access より前に vector 61。byte は対象外 */
    for (unsigned sz = 0; sz < 3; ++sz) {
        static const uint16_t op[] = {0x0ad0, 0x0cd0, 0x0ed0}; /* CAS.B/W/L D0,D1,(A0) */
        for (unsigned off = 0; off < 4; ++off) {
            init_state(cpu_g, f_g);
            cpu_g->a[0] = DATA + off;
            cpu_g->d[0] = 0;
            cpu_g->d[1] = 0x55;
            lng(f_g, DATA, 0x11223344);
            RUN(false, SR_SUP, op[sz], 0x0040);
            const bool misaligned = (sz == 1 && (off & 1)) || (sz == 2 && off != 0);
            if (misaligned) {
                expect_four_word(SSP_INIT - 8, 61, SR_SUP, TEST_PC);
                assert(ram_l(f_g, DATA) == 0x11223344);
            } else {
                assert(cpu_g->last_exception_vector == NO_VECTOR);
            }
        }
    }
    /* 020 には vector 61 の非整列検査がない */
    {
        static fixture_t g;
        mc68k_state_t *c20 = make_cpu(&g, MC68K_MODEL_68020);
        lng(&g, DATA, 0x11223344);
        c20->a[0] = DATA + 2;
        c20->d[0] = 0;
        c20->d[1] = 0x55;
        word(&g, TEST_PC, 0x0ed0);
        word(&g, TEST_PC + 2, 0x0040);
        c20->pc = TEST_PC;
        cpu_set_sr(c20, SR_SUP);
        mc68k_execute(c20, 1);
        assert(c20->last_exception_vector != 61);
        mc68k_destroy(c20);
    }
    /* 拡張語（displacement）の fetch fault は access error。operand にも CAS の結果にも触れない
     */
    init_state(cpu_g, f_g);
    cpu_g->d[0] = ram_l(f_g, 0);
    cpu_g->d[1] = 0xdeadbeefu;
    cpu_g->a[0] = DATA;
    f_g->fail_r_on = true;
    f_g->fail_r_addr = TEST_PC + 4;
    RUN(false, SR_SUP, 0x0ee8, 0x0040, 0x0000); /* CAS.L D0,D1,(0,A0) */
    expect_bus_error_frame();
    assert(ram_l(f_g, 0) == SSP_INIT && cpu_g->d[0] == SSP_INIT && cpu_g->d[1] == 0xdeadbeefu);
}

static void test_c11_rte_faults(void) {
    const uint32_t base = SSP_INIT - 0x40;
    /* SR の read fault、PC の read fault（format は読めている） */
    for (unsigned k = 0; k < 2; ++k) {
        init_state(cpu_g, f_g);
        word(f_g, base, 0x2004);
        lng(f_g, base + 2, TARGET);
        word(f_g, base + 6, 0x0000);
        PLACE(false, SR_SUP, RTE_OPCODE);
        cpu_g->a[7] = base;
        f_g->fail_r_on = true;
        f_g->fail_r_addr = base + 2 * k;
        STEP();
        assert(cpu_g->last_exception_vector == VEC_BUS && cpu_g->pc == handler(VEC_BUS));
        assert(cpu_g->a[7] == base - 16);
        assert(ram_w(f_g, base - 16 + 6) == 0x4008 && ram_l(f_g, base - 16 + 2) == TEST_PC);
        assert(cpu_g->sr == SR_SUP);
    }
}

/* 通常 frame の各語の書込み fault は HALT（以降の語は書かない）。vector の読出し fault も HALT
 */
static void frame_fault_case(bool address_error, unsigned slot_offset, bool vector_read) {
    init_state(cpu_g, f_g);
    mc68k_set_fault_ring(cpu_g, 1);
    const uint32_t frame_bytes = address_error ? 12u : 16u;
    const uint32_t base = SSP_INIT - frame_bytes;
    if (address_error) {
        cpu_g->a[0] = TARGET + 1;
        PLACE(false, SR_SUP, 0x4ed0); /* JMP (A0) */
    } else {
        f_g->bad_on = true;
        cpu_g->a[0] = BAD_BASE + 0x10;
        PLACE(false, SR_SUP, 0x2010); /* MOVE.L (A0),D0 */
    }
    if (vector_read) {
        f_g->fail_r_on = true;
        f_g->fail_r_addr = (uint32_t)(address_error ? VEC_ADDR : VEC_BUS) * 4u;
    } else {
        f_g->fail_ws_on = true;
        f_g->fail_ws_addr = base + slot_offset;
    }
    STEP();
    assert(cpu_g->exec_state == CPU_HALTED);
    assert(cpu_g->last_exception_vector == NO_VECTOR);
    mc68k_fault_ring_entry_t buf[8];
    const mc68k_fault_ring_entry_t *e = last_ring(buf);
    assert(e->outcome == (vector_read ? MC68K_FAULT_OUTCOME_VECTOR_READ_FAULT
                                      : MC68K_FAULT_OUTCOME_FRAME_WRITE_FAULT));
    if (!vector_read) {
        /* 失敗した語より後ろ（次の語以降）には何も書かない */
        static const unsigned slot_end[] = {2, 6, 8, 12, 16};
        unsigned next = 0;
        while (slot_end[next] <= slot_offset)
            ++next;
        ++next;
        for (uint32_t o = next < 5 ? slot_end[next - 1] : frame_bytes; o < frame_bytes; ++o)
            assert(f_g->ram[base + o] == 0);
    }
}

static void test_c11_frame_faults(void) {
    static const unsigned access_slots[] = {0, 2, 6, 8, 12};
    static const unsigned address_slots[] = {0, 2, 6, 8};
    for (unsigned i = 0; i < 5; ++i)
        frame_fault_case(false, access_slots[i], false);
    for (unsigned i = 0; i < 4; ++i)
        frame_fault_case(true, address_slots[i], false);
    frame_fault_case(false, 0, true);
    frame_fault_case(true, 0, true);
}

static void test_c11_misc_delivery(void) {
    /* access error は T1/T0 をクリアして handler に入る（stacked SR は元のまま）。user からでも
     * S=1 */
    static const struct {
        bool user;
        uint16_t sr;
    } trace_cases[] = {{false, SR_SUP | SR_T1_BIT},
                       {false, SR_SUP | SR_T0_BIT},
                       {false, SR_SUP | SR_T1_BIT | SR_T0_BIT},
                       {true, SR_T1_BIT},
                       {true, SR_T0_BIT}};
    for (unsigned i = 0; i < sizeof trace_cases / sizeof trace_cases[0]; ++i) {
        init_state(cpu_g, f_g);
        f_g->bad_on = true;
        cpu_g->a[0] = BAD_BASE + 0x10;
        PLACE(false, trace_cases[i].sr, 0x2010);
        if (trace_cases[i].user)
            cpu_set_sr(cpu_g, trace_cases[i].sr);
        STEP();
        assert(cpu_g->last_exception_vector == VEC_BUS);
        assert(ram_w(f_g, cpu_g->a[7]) == trace_cases[i].sr);
        assert((cpu_g->sr & (SR_T1_BIT | SR_T0_BIT)) == 0 && (cpu_g->sr & 0x2000));
    }
    /* 例外 frame の push が fault して bus error frame に入った後、その handler 内の別の fault
     * は 元の例外ではなく、いま実行中の命令から frame を作る（origin は bind 済み） */
    init_state(cpu_g, f_g);
    word(f_g, handler(VEC_BUS), 0x2010); /* handler: MOVE.L (A0),D0 */
    PLACE(false, SR_SUP, 0x4e40);        /* TRAP #0 */
    f_g->fail_w_armed = true;
    f_g->fail_w_addr = SSP_INIT - 2;
    STEP(); /* TRAP の frame 書込みが fault → bus error frame */
    assert(cpu_g->last_exception_vector == VEC_BUS && cpu_g->pc == handler(VEC_BUS));
    f_g->bad_on = true;
    cpu_g->a[0] = BAD_BASE + 0x20;
    const uint32_t sp = cpu_g->a[7];
    STEP(); /* handler の MOVE.L (A0),D0 が fault */
    assert(cpu_g->last_exception_vector == VEC_BUS && cpu_g->a[7] == sp - 16);
    assert(ram_l(f_g, sp - 16 + 2) == handler(VEC_BUS)); /* 実行中の命令の PC */
    assert(ram_l(f_g, sp - 16 + 8) == BAD_BASE + 0x20);

    /* その bus error frame から RTE で戻ると、作りかけだった例外 frame は破棄され SP
     * は例外前に戻る */
    init_state(cpu_g, f_g);
    word(f_g, handler(VEC_BUS), 0x4e73); /* handler: RTE */
    PLACE(false, SR_SUP, 0x4e40);        /* TRAP #0 */
    f_g->fail_w_armed = true;
    f_g->fail_w_addr = SSP_INIT - 2;
    STEP();
    assert(cpu_g->last_exception_vector == VEC_BUS && cpu_g->pc == handler(VEC_BUS));
    STEP();
    assert(cpu_g->a[7] == SSP_INIT && cpu_g->pc == TEST_PC + 2 && cpu_g->sr == SR_SUP);

    /* 奇数の handler アドレスへの例外は、その場では address error にならず、次の fetch
     * で検出する */
    init_state(cpu_g, f_g);
    lng(f_g, 32 * 4, HANDLER + 0x1001);
    RUN(false, SR_SUP, 0x4e40); /* TRAP #0 */
    assert(cpu_g->last_exception_vector == 32 && cpu_g->pc == HANDLER + 0x1001);
    const uint32_t sp_trap = cpu_g->a[7], below_frame = ram_l(f_g, sp_trap);
    STEP();
    assert(cpu_g->last_exception_vector == VEC_ADDR);
    assert(cpu_g->a[7] == sp_trap - 12 &&
           ram_l(f_g, sp_trap) == below_frame); /* format $2 は 12 byte */
    assert(ram_l(f_g, cpu_g->a[7] + 8) == HANDLER + 0x1000);

    /* 分岐先が奇数の address error（巻き戻さない）の後の別の fault
     * は、いつもどおり命令の先頭へ巻き戻す */
    init_state(cpu_g, f_g);
    word(f_g, handler(VEC_ADDR), 0x2018); /* handler: MOVE.L (A0)+,D0 */
    PLACE(false, SR_SUP, 0x4ed0);         /* JMP (A0) */
    cpu_g->a[0] = HANDLER + 0x1001;
    STEP();
    assert(cpu_g->last_exception_vector == VEC_ADDR && cpu_g->pc == handler(VEC_ADDR));
    f_g->bad_on = true;
    cpu_g->a[0] = BAD_BASE + 0x20;
    STEP();
    assert(cpu_g->last_exception_vector == VEC_BUS && cpu_g->a[0] == BAD_BASE + 0x20);
}

int main(void) {
    static fixture_t f;
    f_g = &f;
    cpu_g = make_cpu(&f, MC68K_MODEL_68060);
    test_c1_host();
    test_c1_restart();
    test_c1_mmu();
    test_c2();
    test_c3();
    test_c4();
    test_c5();
    test_c6();
    test_c7();
    test_c8();
    test_c9();
    test_c10();
    test_c11_reset();
    test_c11_lpstop();
    test_c11_movec();
    test_c11_cas();
    test_c11_rte_faults();
    test_c11_frame_faults();
    test_c11_misc_delivery();
    mc68k_destroy(cpu_g);
    return 0;
}
