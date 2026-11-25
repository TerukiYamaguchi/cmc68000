/*
 * MC68060-B (#108) T-B1〜T-B6。設計書 docs/mc68000-internals/reference/cpu_68060.md §5、§10.1。
 * ATC への entry の有無は「table を書き換えた後に旧 translation
 * が使われるか（stale）」で観測する。 access error の frame は 060-C の範囲なので、fault
 * したあとの再試行はホストが行う。
 */
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <string.h>

#define RAM_SIZE  0x80000u
#define SSP_INIT  0x8000u
#define USP_INIT  0x7000u
#define RESET_PC  0x100u
#define TEST_PC   0x400u
#define HANDLER   0x3000u
#define SR_SUP    0x2700
#define SR_USER   0x0000
#define VEC_BUS   2
#define VEC_PRIV  8
#define NO_VECTOR 0xffffu

/* 変換テーブル（物理）。user 用と supervisor 用の 2 本の木 */
#define ROOT_U           0x10000u
#define PTR_U            0x10200u
#define PAGE_U           0x10400u
#define ROOT_S           0x11000u
#define PTR_S            0x11200u
#define PAGE_S           0x11400u
#define PHYS_BASE        0x20000u
#define PHYS_PAGE        0x1000u
/* 論理テスト page（A31–A24 = 1 なので TTR の外。ITT0/DTT0 が $00 領域を恒等にする） */
#define LOG_BASE         0x01000000u
#define LOG_PAGE         0x1000u
#define TTR_IDENTITY_LOW 0x0000c000u /* base $00・mask $00・E=1・S=任意 */

#define PTR_INDEX     64u /* (LOG_BASE >> 18) & 0x7f */
#define DESC_RESIDENT 1u
#define DESC_TABLE    3u
#define DESC_W        0x04u
#define DESC_U        0x08u
#define DESC_M        0x10u
#define DESC_S        0x80u
#define DESC_G        0x400u
#define TC_E          0x8000u
#define TC_P          0x4000u
#define CODE_OFFSET   0x10u

#define OLD_DATA(n) (0xaaaa0000u + (n))
#define NEW_DATA(n) (0xbbbb0000u + (n))
#define OLD_K       1
#define NEW_K       2

typedef struct {
    uint8_t ram[RAM_SIZE];
    unsigned resets;
} fixture_t;

static bool rb(void *c, uint32_t a, uint8_t *v) {
    if (a >= RAM_SIZE)
        return false;
    *v = ((fixture_t *)c)->ram[a];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    if (a + 1 >= RAM_SIZE)
        return false;
    fixture_t *f = c;
    *v = (uint16_t)(f->ram[a] << 8 | f->ram[a + 1]);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    if (a + 3 >= RAM_SIZE)
        return false;
    fixture_t *f = c;
    *v = (uint32_t)f->ram[a] << 24 | (uint32_t)f->ram[a + 1] << 16 |
         (uint32_t)f->ram[a + 2] << 8 | f->ram[a + 3];
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    if (a >= RAM_SIZE)
        return false;
    ((fixture_t *)c)->ram[a] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    if (a + 1 >= RAM_SIZE)
        return false;
    fixture_t *f = c;
    f->ram[a] = (uint8_t)(v >> 8);
    f->ram[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    if (a + 3 >= RAM_SIZE)
        return false;
    fixture_t *f = c;
    f->ram[a] = (uint8_t)(v >> 24);
    f->ram[a + 1] = (uint8_t)(v >> 16);
    f->ram[a + 2] = (uint8_t)(v >> 8);
    f->ram[a + 3] = (uint8_t)v;
    return true;
}
static void reset_peripheral(void *c) {
    ((fixture_t *)c)->resets++;
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
static uint32_t ram_l(const fixture_t *f, uint32_t a) {
    return (uint32_t)f->ram[a] << 24 | (uint32_t)f->ram[a + 1] << 16 |
           (uint32_t)f->ram[a + 2] << 8 | f->ram[a + 3];
}

static mc68k_state_t *cpu_g;
static fixture_t *f_g;

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
static uint8_t data_fc(space_t s) {
    return s == SPACE_USER ? 1 : 5;
}

/* 論理 page n（LOG_BASE + n*LOG_PAGE）を物理 phys へ。flags は DESC_* の組。 */
static uint32_t page_desc_addr(space_t s, unsigned n) {
    return page_of(s) + n * 4u;
}
static void map_page(space_t s, unsigned n, uint32_t phys, uint32_t flags) {
    lng(f_g, root_of(s), ptr_of(s) | DESC_TABLE);
    lng(f_g, ptr_of(s) + PTR_INDEX * 4u, page_of(s) | DESC_TABLE);
    lng(f_g, page_desc_addr(s, n), (phys & ~(PHYS_PAGE - 1u)) | flags | DESC_RESIDENT);
}
static uint32_t log_addr(unsigned n) {
    return LOG_BASE + n * LOG_PAGE;
}
static uint32_t phys_of(unsigned slot) {
    return PHYS_BASE + slot * PHYS_PAGE;
}

/* 物理 page に旧／新の内容（データ long と命令 MOVEQ #k,D0; RTS）を置く。 */
static void fill_phys(unsigned slot, bool fresh, unsigned n) {
    uint32_t p = phys_of(slot);
    lng(f_g, p, fresh ? NEW_DATA(n) : OLD_DATA(n));
    word(f_g, p + CODE_OFFSET, (uint16_t)(0x7000 | (fresh ? NEW_K : OLD_K)));
    word(f_g, p + CODE_OFFSET + 2, 0x4e75);
}

static void init_cpu(void) {
    memset(f_g->ram, 0, sizeof f_g->ram);
    lng(f_g, 0, SSP_INIT);
    lng(f_g, 4, RESET_PC);
    for (int v = 2; v < 256; ++v)
        lng(f_g, (uint32_t)v * 4, HANDLER + (uint32_t)v * 8u);
    mc68k_reset(cpu_g);
    cpu_g->usp = USP_INIT;
    memset(cpu_g->atc060_i, 0, sizeof cpu_g->atc060_i);
    memset(cpu_g->atc060_d, 0, sizeof cpu_g->atc060_d);
    cpu_g->mmu060_urp = ROOT_U;
    cpu_g->mmu060_srp = ROOT_S;
    cpu_g->mmu060_itt0 = cpu_g->mmu060_dtt0 = TTR_IDENTITY_LOW;
    cpu_g->mmu060_tc = TC_E;
}

static void run_ops(bool user, const uint16_t *w, int n) {
    for (int i = 0; i < n; ++i)
        word(f_g, TEST_PC + (uint32_t)i * 2u, w[i]);
    cpu_g->a[7] = SSP_INIT;
    cpu_g->pc = TEST_PC;
    cpu_set_sr(cpu_g, user ? SR_USER : SR_SUP);
    cpu_g->last_exception_vector = NO_VECTOR;
    mc68k_execute(cpu_g, 1);
}
#define RUN(user, ...)                                      \
    do {                                                    \
        const uint16_t w_[] = {__VA_ARGS__};                \
        run_ops(user, w_, (int)(sizeof w_ / sizeof w_[0])); \
    } while (0)

/* データ probe: supervisor の MOVES.L (A0),D0（SFC = space の fc）。fault なら false。 */
static bool probe_data(space_t s, uint32_t logical, uint32_t *out) {
    cpu_g->sfc = data_fc(s);
    cpu_g->a[0] = logical;
    cpu_g->d[0] = 0xdeadbeefu;
    RUN(false, 0x0e90, 0x0000);
    *out = cpu_g->d[0];
    return cpu_g->last_exception_vector == NO_VECTOR;
}
/* 命令 probe: space の権限で JSR (A0) → MOVEQ → RTS を実行する。 */
static bool probe_instr(space_t s, uint32_t logical, uint32_t *out) {
    word(f_g, TEST_PC, 0x4e90);
    cpu_g->a[7] = SSP_INIT;
    cpu_g->usp = USP_INIT;
    cpu_g->a[0] = logical + CODE_OFFSET;
    cpu_g->d[0] = 0;
    cpu_g->pc = TEST_PC;
    cpu_set_sr(cpu_g, s == SPACE_USER ? SR_USER : SR_SUP);
    cpu_g->last_exception_vector = NO_VECTOR;
    for (int i = 0; i < 3; ++i)
        mc68k_execute(cpu_g, 1);
    *out = cpu_g->d[0];
    return cpu_g->last_exception_vector == NO_VECTOR;
}
/* 書込み probe: MOVES.L D1,(A0)（DFC = space の fc）。 */
static bool write_data(space_t s, uint32_t logical, uint32_t value) {
    cpu_g->dfc = data_fc(s);
    cpu_g->a[0] = logical;
    cpu_g->d[1] = value;
    RUN(false, 0x0e90, 0x1800);
    return cpu_g->last_exception_vector == NO_VECTOR;
}
static void flush_atc(void) {
    memset(cpu_g->atc060_i, 0, sizeof cpu_g->atc060_i);
    memset(cpu_g->atc060_d, 0, sizeof cpu_g->atc060_d);
}

/* ---- T-B1: PFLUSH の範囲 ---- */
typedef struct {
    const char *name;
    uint16_t op;
    unsigned target;
    bool keep[4];
} flush_case_t;
/* entry: [0]=(1) DFC 空間 P non-global、[1]=(2) DFC 空間 Q global、[2]=(3) 反対空間 P
 * non-global、[3]=(4) DFC 空間 R */
static const flush_case_t flush_cases[] = {
    {"PFLUSH (P)", 0xf509, 0, {false, true, true, true}},
    {"PFLUSH (Q)", 0xf509, 1, {true, false, true, true}},
    {"PFLUSHN (P)", 0xf501, 0, {false, true, true, true}},
    {"PFLUSHN (Q)", 0xf501, 1, {true, true, true, true}},
    {"PFLUSHA", 0xf518, 0, {false, false, false, false}},
    {"PFLUSHAN", 0xf510, 0, {false, true, false, false}},
};
static void place_entries(space_t s, bool instr) {
    space_t o = s == SPACE_USER ? SPACE_SUP : SPACE_USER;
    /* slot 0..3 = 論理 P(page0)/Q(page1)/R(page2)/反対空間 P の旧内容。slot+4.. = 新内容 */
    for (unsigned i = 0; i < 4; ++i) {
        fill_phys(i, false, i);
        fill_phys(4 + i, true, i);
    }
    map_page(s, 0, phys_of(0), 0);
    map_page(s, 1, phys_of(1), DESC_G);
    map_page(s, 2, phys_of(2), 0);
    map_page(o, 0, phys_of(3), 0);
    uint32_t v;
    (void)v;
    uint32_t pages[4] = {log_addr(0), log_addr(1), log_addr(2), log_addr(0)};
    space_t spaces[4] = {s, s, s, o};
    for (int i = 0; i < 4; ++i)
        assert(instr ? probe_instr(spaces[i], pages[i], &v)
                     : probe_data(spaces[i], pages[i], &v));
}
static void check_entries(const flush_case_t *c, space_t s, bool instr) {
    space_t o = s == SPACE_USER ? SPACE_SUP : SPACE_USER;
    uint32_t pages[4] = {log_addr(0), log_addr(1), log_addr(2), log_addr(0)};
    space_t spaces[4] = {s, s, s, o};
    /* table を新内容へ書き換えてから、entry の残存を stale で判定する */
    map_page(s, 0, phys_of(4), 0);
    map_page(s, 1, phys_of(5), DESC_G);
    map_page(s, 2, phys_of(6), 0);
    map_page(o, 0, phys_of(7), 0);
    for (int i = 0; i < 4; ++i) {
        uint32_t v;
        assert(instr ? probe_instr(spaces[i], pages[i], &v)
                     : probe_data(spaces[i], pages[i], &v));
        uint32_t stale = instr ? OLD_K : OLD_DATA(i), fresh = instr ? NEW_K : NEW_DATA(i);
        assert(v == (c->keep[i] ? stale : fresh));
        (void)fresh;
    }
}
static void test_b1_pflush(void) {
    static const uint8_t dfcs[] = {1, 2, 5, 6};
    for (unsigned d = 0; d < 4; ++d) {
        space_t s = dfcs[d] >= 4 ? SPACE_SUP : SPACE_USER;
        for (unsigned ci = 0; ci < sizeof flush_cases / sizeof flush_cases[0]; ++ci) {
            for (int instr = 0; instr <= 1; ++instr) {
                /* An は A1 を使う: オペコードの register 欄を 1 にする */
                init_cpu();
                place_entries(s, instr);
                cpu_g->dfc = dfcs[d];
                cpu_g->sfc = (uint8_t)(dfcs[d] >= 4 ? 1 : 5);
                cpu_g->a[1] = log_addr(flush_cases[ci].target);
                uint16_t op = (uint16_t)((flush_cases[ci].op & ~7u) | 1u);
                RUN(false, op);
                assert(cpu_g->last_exception_vector == NO_VECTOR);
                check_entries(&flush_cases[ci], s, instr);
            }
        }
    }
}

/* ---- T-B1: PLPA ---- */
static void test_b1_plpa(void) {
    static const uint8_t dfcs[] = {1, 2, 5, 6};
    static const uint16_t ops[] = {0xf5c9, 0xf589}; /* PLPAR (A1)、PLPAW (A1) */
    for (unsigned d = 0; d < 4; ++d) {
        for (unsigned k = 0; k < 2; ++k) {
            space_t s = dfcs[d] >= 4 ? SPACE_SUP : SPACE_USER;
            space_t o = s == SPACE_USER ? SPACE_SUP : SPACE_USER;
            init_cpu();
            map_page(SPACE_USER, 3, phys_of(1), 0);
            map_page(SPACE_SUP, 3, phys_of(2), 0);
            cpu_g->dfc = dfcs[d];
            cpu_g->sfc = data_fc(o);
            cpu_g->a[1] = log_addr(3) + 0x123;
            RUN(false, ops[k]);
            assert(cpu_g->last_exception_vector == NO_VECTOR);
            assert(cpu_g->a[1] == phys_of(s == SPACE_USER ? 1 : 2) + 0x123);
        }
    }
    /* PLPA は data ATC を引く（DFC=2/6 でも）: 通常のデータアクセスで置いた旧 translation
     * が使われる */
    for (unsigned d = 0; d < 4; ++d) {
        space_t s = dfcs[d] >= 4 ? SPACE_SUP : SPACE_USER;
        init_cpu();
        map_page(s, 3, phys_of(1), 0);
        uint32_t v;
        assert(probe_data(s, log_addr(3), &v));
        map_page(s, 3, phys_of(2), 0);
        cpu_g->dfc = dfcs[d];
        cpu_g->a[1] = log_addr(3) + 8;
        RUN(false, 0xf5c9);
        assert(cpu_g->a[1] == phys_of(1) + 8);
    }
    /* TTR hit と TC.E=0 では An を変えない */
    init_cpu();
    cpu_g->dfc = 5;
    cpu_g->a[1] = 0x00001234;
    RUN(false, 0xf5c9);
    assert(cpu_g->a[1] == 0x00001234); /* data TTR hit（$00 領域） */
    init_cpu();
    cpu_g->mmu060_tc = 0;
    cpu_g->dfc = 5;
    cpu_g->a[1] = log_addr(3) + 4;
    RUN(false, 0xf5c9);
    assert(cpu_g->a[1] == log_addr(3) + 4); /* E=0 */
    /* 失敗は An でなく access error */
    init_cpu();
    cpu_g->dfc = 5;
    cpu_g->a[1] = log_addr(9); /* 未 map の page */
    RUN(false, 0xf5c9);
    assert(cpu_g->last_exception_vector == VEC_BUS);
}

/* ---- T-B2: snapshot / debug translate ---- */
static void test_b2(void) {
    init_cpu();
    map_page(SPACE_SUP, 3, phys_of(1), 0);
    mc68k_debug_translate_result_t t;
    assert(mc68k_debug_translate(cpu_g, log_addr(3) + 5, 5, 1, &t) == 0);
    assert(t.physical == phys_of(1) + 5 && t.mmusr == 0);
    mc68k_mmu060_regs_t r;
    mc68k_get_mmu060_regs(cpu_g, &r);
    assert(r.urp == ROOT_U && r.srp == ROOT_S && r.tc == TC_E && r.itt0 == TTR_IDENTITY_LOW &&
           r.dtt0 == TTR_IDENTITY_LOW && r.itt1 == 0 && r.dtt1 == 0);
    /* 030/040 の snapshot には 060 の値が入らない */
    mc68k_mmu_regs_t r30;
    mc68k_get_mmu_regs(cpu_g, &r30);
    assert(r30.tt0 == 0 && r30.tt1 == 0 && r30.mmusr == 0);
    mc68k_mmu040_regs_t r40;
    mc68k_get_mmu040_regs(cpu_g, &r40);
    assert(r40.urp == 0 && r40.tc == 0);
    /* debug translate は ATC や table を変えない */
    flush_atc();
    assert(mc68k_debug_translate(cpu_g, log_addr(3), 5, 1, &t) == 0);
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; ++i)
        assert(!cpu_g->atc060_d[i].valid);
    assert((ram_l(f_g, page_desc_addr(SPACE_SUP, 3)) & DESC_U) == 0);
    /* 失敗は -1 */
    assert(mc68k_debug_translate(cpu_g, log_addr(9), 5, 1, &t) == -1);
    /* fault ring は 060 の fault を記録し、tc/crp/srp 欄は 0（060 の値は 030 の欄に入れない）
     */
    mc68k_set_fault_ring(cpu_g, 1);
    uint32_t v;
    assert(!probe_data(SPACE_SUP, log_addr(9), &v));
    mc68k_fault_ring_entry_t ring[4];
    size_t n = mc68k_read_fault_ring(cpu_g, ring, 4);
    assert(n >= 1);
    assert(ring[n - 1].tc == 0 && ring[n - 1].crp == 0 && ring[n - 1].srp == 0);
    mc68k_set_fault_ring(cpu_g, 0);
}

/* ---- T-B3: TC のビット保持 ---- */
static void test_b3(void) {
    static const uint32_t bits[] = {0x2000, 0x1000, 0x0800, 0x0400, 0x4000};
    for (unsigned i = 0; i < sizeof bits / sizeof bits[0]; ++i) {
        init_cpu();
        cpu_g->mmu060_tc = 0;
        cpu_g->d[0] = bits[i];
        RUN(false, 0x4e7b, 0x0003);
        cpu_g->d[1] = 0;
        RUN(false, 0x4e7a, 0x1003);
        assert(cpu_g->d[1] == bits[i]);
    }
}

/* TC.E=0 でも既定属性（DWO）が書込みを保護する。DWO=0 なら書ける。 */
static void test_b3_default_attributes(void) {
    for (int dwo = 0; dwo <= 1; ++dwo) {
        init_cpu();
        cpu_g->mmu060_tc = dwo ? 0x0020u : 0;
        cpu_g->mmu060_itt0 = cpu_g->mmu060_dtt0 = 0;
        uint32_t v;
        assert(probe_data(SPACE_SUP, PHYS_BASE, &v));
        /* DWO=1 では例外 frame の push も保護されるので、戻り値でなくメモリの内容で判定する。
         */
        (void)write_data(SPACE_SUP, PHYS_BASE, 0x13572468u);
        assert(ram_l(f_g, PHYS_BASE) == (dwo ? 0u : 0x13572468u));
    }
}

/* ---- T-B4: 翻訳器の選択 ---- */
static void test_b4(void) {
    init_cpu();
    fill_phys(0, false, 0);
    map_page(SPACE_SUP, 0, phys_of(0), 0);
    uint32_t v;
    assert(probe_data(SPACE_SUP, log_addr(0), &v) && v == OLD_DATA(0)); /* 実行経路（データ） */
    assert(probe_instr(SPACE_SUP, log_addr(0), &v) && v == OLD_K);      /* 実行経路（命令） */
    mc68k_debug_translate_result_t t;
    assert(mc68k_debug_translate(cpu_g, log_addr(0) + 4, 5, 1, &t) == 0 &&
           t.physical == phys_of(0) + 4);
}

/* ---- T-B5: ATC を flush しない操作 ---- */
static void test_b5_noflush(void) {
    init_cpu();
    fill_phys(0, false, 0);
    fill_phys(1, true, 0);
    map_page(SPACE_SUP, 0, phys_of(0), 0);
    uint32_t v;
    assert(probe_data(SPACE_SUP, log_addr(0), &v) && v == OLD_DATA(0));
    map_page(SPACE_SUP, 0, phys_of(1), 0);
    /* TC・URP・SRP への MOVEC 書込みと reset は ATC を flush しない */
    cpu_g->d[0] = TC_E;
    RUN(false, 0x4e7b, 0x0003);
    cpu_g->d[0] = ROOT_U;
    RUN(false, 0x4e7b, 0x0806);
    cpu_g->d[0] = ROOT_S;
    RUN(false, 0x4e7b, 0x0807);
    assert(probe_data(SPACE_SUP, log_addr(0), &v) && v == OLD_DATA(0));
    mc68k_reset(cpu_g);
    cpu_g->mmu060_tc = TC_E;
    cpu_g->mmu060_dtt0 = TTR_IDENTITY_LOW;
    cpu_g->mmu060_itt0 = TTR_IDENTITY_LOW;
    assert(probe_data(SPACE_SUP, log_addr(0), &v) && v == OLD_DATA(0));
}

/* fault した命令を修復後に再実行する（frame は 060-C のため、ホストが再試行する）。 */
typedef struct {
    const char *name;
    space_t space;
    bool write;
} fault_kind_t;
static void test_b5_failed_search(void) {
    for (int sp = 0; sp <= 1; ++sp) {
        space_t s = sp ? SPACE_SUP : SPACE_USER;
        /* (a) 無効な page descriptor（PDT=00）、(b)(c) 無効な table descriptor（UDT=00、01） */
        for (int kind = 0; kind < 3; ++kind) {
            init_cpu();
            fill_phys(0, false, 0);
            map_page(s, 0, phys_of(0), 0);
            uint32_t good =
                ram_l(f_g, kind == 0 ? page_desc_addr(s, 0) : ptr_of(s) + PTR_INDEX * 4u);
            uint32_t bad = kind == 0 ? 0 : (kind == 1 ? 0 : 1);
            lng(f_g, kind == 0 ? page_desc_addr(s, 0) : ptr_of(s) + PTR_INDEX * 4u, bad);
            uint32_t v;
            assert(!probe_data(s, log_addr(0), &v));
            assert(cpu_g->last_exception_vector == VEC_BUS);
            /* 修復して、PFLUSH なしで同じ space の同じ側の ATC を引く読みを再試行する */
            lng(f_g, kind == 0 ? page_desc_addr(s, 0) : ptr_of(s) + PTR_INDEX * 4u, good);
            assert(probe_data(s, log_addr(0), &v) && v == OLD_DATA(0));
        }
        /* 命令フェッチ側の fault も entry を作らない */
        init_cpu();
        fill_phys(0, false, 0);
        map_page(s, 0, phys_of(0), 0);
        uint32_t good = ram_l(f_g, page_desc_addr(s, 0)), v;
        lng(f_g, page_desc_addr(s, 0), 0);
        assert(!probe_instr(s, log_addr(0), &v));
        lng(f_g, page_desc_addr(s, 0), good);
        cpu_g->a[7] = SSP_INIT;
        assert(probe_instr(s, log_addr(0), &v) && v == OLD_K);
        /* W=1 の page への書込み fault も entry を作らず、W を落とせば再試行が成功する */
        init_cpu();
        fill_phys(0, false, 0);
        map_page(s, 0, phys_of(0), DESC_W);
        assert(!write_data(s, log_addr(0), 0x12345678u));
        assert(cpu_g->last_exception_vector == VEC_BUS &&
               ram_l(f_g, phys_of(0)) == OLD_DATA(0));
        map_page(s, 0, phys_of(0), 0);
        assert(write_data(s, log_addr(0), 0x12345678u) &&
               ram_l(f_g, phys_of(0)) == 0x12345678u);
    }
}

static void test_b5_write_protect_entry(void) {
    for (int sp = 0; sp <= 1; ++sp) {
        space_t s = sp ? SPACE_SUP : SPACE_USER;
        init_cpu();
        fill_phys(0, false, 0);
        fill_phys(1, true, 0);
        map_page(s, 0, phys_of(0), DESC_W);
        uint32_t v;
        assert(probe_data(s, log_addr(0), &v) &&
               v == OLD_DATA(0));                         /* W=1・M=0 の entry を作る */
        assert(!write_data(s, log_addr(0), 0x11111111u)); /* 書込みは fault。entry は不変 */
        map_page(s, 0, phys_of(1), 0);                    /* table を W=0・別 page に */
        assert(!write_data(s, log_addr(0), 0x22222222u)); /* 再び fault（entry が不変） */
        assert(ram_l(f_g, phys_of(0)) == OLD_DATA(0) && ram_l(f_g, phys_of(1)) == NEW_DATA(0));
        assert((ram_l(f_g, page_desc_addr(s, 0)) & DESC_M) == 0);
    }
}

static void test_b5_modified_update(void) {
    for (int sp = 0; sp <= 1; ++sp) {
        space_t s = sp ? SPACE_SUP : SPACE_USER;
        init_cpu();
        fill_phys(0, false, 0);
        fill_phys(1, true, 0);
        fill_phys(2, true, 0);
        map_page(s, 0, phys_of(0), 0);
        uint32_t v;
        assert(probe_data(s, log_addr(0), &v) && v == OLD_DATA(0)); /* W=0・M=0 の entry */
        map_page(s, 0, phys_of(1), 0);                              /* table を Y に */
        assert(write_data(s, log_addr(0), 0x77777777u));
        assert(ram_l(f_g, phys_of(1)) == 0x77777777u);              /* 書込みは Y に届く */
        assert(ram_l(f_g, phys_of(0)) == OLD_DATA(0));              /* X は不変 */
        assert((ram_l(f_g, page_desc_addr(s, 0)) & DESC_M) != 0);   /* M が立つ */
        map_page(s, 0, phys_of(2), 0);                              /* table を Z に */
        assert(probe_data(s, log_addr(0), &v) && v == 0x77777777u); /* Y の entry が使われる */
        /* ATC の M=1 で walk しない: host が M を 0 に戻しても書込みで立たない */
        uint32_t d = ram_l(f_g, page_desc_addr(s, 0));
        (void)d;
        lng(f_g, page_desc_addr(s, 0), ram_l(f_g, page_desc_addr(s, 0)) & ~DESC_M);
        assert(write_data(s, log_addr(0), 0x88888888u));
        assert((ram_l(f_g, page_desc_addr(s, 0)) & DESC_M) == 0);
    }
}

/* ---- T-B6: user の PFLUSH / PLPA ---- */
static void test_b6(void) {
    static const uint16_t ops[] = {0xf501, 0xf509, 0xf510, 0xf518, 0xf589, 0xf5c9};
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; ++i) {
        for (int instr = 0; instr <= 1; ++instr) {
            init_cpu();
            place_entries(SPACE_SUP, instr);
            map_page(SPACE_SUP, 3, phys_of(2), 0);
            map_page(SPACE_USER, 3, phys_of(1), 0);
            cpu_g->dfc = 5;
            cpu_g->a[1] = log_addr(3) + 0x10;
            uint32_t hist = ram_l(f_g, page_desc_addr(SPACE_SUP, 3));
            RUN(true, ops[i]);
            assert(cpu_g->last_exception_vector == VEC_PRIV &&
                   cpu_g->a[1] == log_addr(3) + 0x10);
            assert(ram_l(f_g, page_desc_addr(SPACE_SUP, 3)) == hist);
            /* entry は全て不変 */
            flush_case_t keep_all = {"", 0, 0, {true, true, true, true}};
            check_entries(&keep_all, SPACE_SUP, instr);
        }
    }
}

int main(void) {
    static fixture_t f;
    f_g = &f;
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral,
                       .ctx = &f};
    cpu_g = mc68k_create_model(&bus, MC68K_MODEL_68060);
    assert(cpu_g);
    mc68k_set_peek_hooks(cpu_g, pk_b, pk_w, pk_l, &f);
    test_b1_pflush();
    test_b1_plpa();
    test_b2();
    test_b3();
    test_b3_default_attributes();
    test_b4();
    test_b5_noflush();
    test_b5_failed_search();
    test_b5_write_protect_entry();
    test_b5_modified_update();
    test_b6();
    mc68k_destroy(cpu_g);
    return 0;
}
