/*
 * MC68060 データアクセスの ATC ヒット高速経路（#128）の試験。
 * 設計書 docs/mc68000-internals/reference/cpu_68060.md §5（データアクセス高速経路）。
 *
 * 同じ手順を「高速経路あり」と「なし（atc060_dfast_disable）」で実行し、各命令後の PC・D1/D2・
 * 消費サイクル・例外ベクタ・D 側 ATC の内容（history・W・M を含む）・ヒント表・atc060_clock・
 * RAM 全体が一致することを確認する。W/M/RMW（TAS）・PFLUSH・slot 上書き・TC/DTT の変更・
 * user↔supervisor 切替・WP フォルトの順序などの後に、古い entry を使わないことも固定する。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE      0x80000u
#define SSP_INIT      0x8000u
#define USP_INIT      0x7000u
#define RESET_PC      0x100u
#define TEST_PC       0x400u
#define CODE_ADDR     0x500u
#define HANDLER       0x3000u
#define SR_SUP        0x2700
#define SR_USER       0x0000
#define VEC_NONE      0xffffu
#define VEC_ACCESS    2u
#define TT_REGION     0x01000000u /* DTT で恒等にした論理領域。RAM の TT_RAM_BASE に折り返す */
#define TT_RAM_BASE   0x40000u
#define TT_ADDR_MASK  0x00ffffffu
#define ROOT          0x10000u
#define PTR           0x10200u
#define PT_LOW0       0x10400u /* 論理 0x00000-0x3ffff を恒等に写す 4KB ページ表 */
#define PT_LOW1       0x10500u /* 論理 0x40000-0x7ffff */
#define PT_DATA       0x10600u /* 論理 0x01000000 から */
#define PHYS_BASE     0x20000u
#define PHYS_SLOT     0x2000u /* TC.P=1（8KB）でも使えるよう 8KB 単位で置く */
#define LOG_BASE      0x01000000u
#define DATA_OFFSET   0x1000u /* 4KB ページの page1、8KB ページでは page0 の後半 */
#define DATA_ADDR     (LOG_BASE + DATA_OFFSET)
#define PTR_INDEX_LOW1 1u
#define PTR_INDEX_DATA 64u
#define LOW_PAGES     64u
#define PAGE_4K       0x1000u
#define DESC_RESIDENT 1u
#define DESC_TABLE    3u
#define DESC_W        0x04u
#define TC_E          0x8000u
#define TC_P          0x4000u
#define TTR_E         0x8000u
#define TTR_LOW_ALL   0x0000c000u /* base $00・mask $00・E=1・S=任意 */
#define TTR_REGION_01 0x0100c000u /* base $01・mask $00・E=1・S=任意 */
#define OP_TAS_A0     0x4ad0u
#define OP_MOVE_A0_D2 0x2410u /* MOVE.L (A0),D2 */
#define OP_ADDQ_D1    0x5281u /* ADDQ.L #1,D1 */
#define OP_MOVE_D1_A0 0x2081u /* MOVE.L D1,(A0) */
#define OP_BRA_BACK   0x60f6u /* BRA.S: TAS へ戻る */
#define OP_PFLUSHA    0xf518u
#define LOOP_INSNS    5
#define PHASE_STEPS   (LOOP_INSNS * 2)
#define MAX_STEPS     (PHASE_STEPS * 3)
#define READ_STEP     1 /* 各フェーズ先頭から数えた MOVE.L (A0),D2 の位置 */

#define MARK_OLD 0xa1a1a1a1u
#define MARK_NEW 0xb2b2b2b2u
#define MARK_8K  0xc3c3c3c3u
#define MARK_TT  0xd4d4d4d4u
#define SLOT_OLD 0u
#define SLOT_NEW 1u
#define SLOT_8K  2u

typedef struct {
    uint8_t ram[RAM_SIZE];
} fixture_t;

static fixture_t *f_g;
static mc68k_state_t *cpu_g;

static bool map_addr(uint32_t a, uint32_t *idx) {
    if ((a >> 24) == (TT_REGION >> 24))
        a = TT_RAM_BASE + (a & TT_ADDR_MASK);
    if (a >= RAM_SIZE)
        return false;
    *idx = a;
    return true;
}
static bool rb(void *c, uint32_t a, uint8_t *v) {
    uint32_t i;
    if (!map_addr(a, &i))
        return false;
    *v = ((fixture_t *)c)->ram[i];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    uint8_t hi, lo;
    if (!rb(c, a, &hi) || !rb(c, a + 1, &lo))
        return false;
    *v = (uint16_t)(hi << 8 | lo);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    uint16_t hi, lo;
    if (!rw(c, a, &hi) || !rw(c, a + 2, &lo))
        return false;
    *v = (uint32_t)hi << 16 | lo;
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    uint32_t i;
    if (!map_addr(a, &i))
        return false;
    ((fixture_t *)c)->ram[i] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    return wb(c, a, (uint8_t)(v >> 8)) && wb(c, a + 1, (uint8_t)v);
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    return ww(c, a, (uint16_t)(v >> 16)) && ww(c, a + 2, (uint16_t)v);
}
static void reset_peripheral(void *c) {
    (void)c;
}


static void word(uint32_t a, uint16_t v) {
    f_g->ram[a] = (uint8_t)(v >> 8);
    f_g->ram[a + 1] = (uint8_t)v;
}
static void lng(uint32_t a, uint32_t v) {
    word(a, (uint16_t)(v >> 16));
    word(a + 2, (uint16_t)v);
}
static uint32_t slot_base(unsigned slot) {
    return PHYS_BASE + slot * PHYS_SLOT;
}
/* 4KB ページ時の data page1 の descriptor を物理 slot へ向ける。 */
static void map_data_page(unsigned slot, uint32_t flags) {
    lng(PT_DATA + 4u, slot_base(slot) | DESC_RESIDENT | flags);
}

typedef struct {
    uint32_t pc, d1, d2, clock, sr;
    int cycles;
    uint16_t vector;
    bool tt_hit;
    uint8_t cache_mode;
    uint64_t atc_hash, ram_hash;
} step_t;

static uint64_t fnv(uint64_t h, uint64_t v) {
    return (h ^ v) * 1099511628211ull;
}

static uint64_t hash_atc(const mc68k_state_t *cpu) {
    uint64_t h = 1469598103934665603ull;
    const mc68k_atc_entry_t *atc = cpu->atc060_d;
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++) {
        const uint64_t v[] = {atc[i].valid,         atc[i].fc,
                              atc[i].page_shift,    atc[i].logical_page,
                              atc[i].physical_page, atc[i].history,
                              atc[i].cache_mode,    atc[i].cache_inhibit,
                              atc[i].write_protect, atc[i].modified};
        for (unsigned j = 0; j < sizeof v / sizeof v[0]; j++)
            h = fnv(h, v[j]);
    }
    for (unsigned i = 0; i < MC68K_060_ATC_HINTS; i++)
        h = fnv(h, cpu->atc060_hint[0][i]);
    return h;
}

static uint64_t hash_ram(void) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < RAM_SIZE; i += 8) {
        uint64_t v;
        memcpy(&v, &f_g->ram[i], sizeof v);
        h = fnv(h, v);
    }
    return h;
}

static uint32_t data_addr_g = DATA_ADDR; /* ループが使うデータアドレス（A0 の初期値） */

static void init_cpu(bool disable_fast) {
    data_addr_g = DATA_ADDR;
    memset(f_g->ram, 0, sizeof f_g->ram);
    lng(0, SSP_INIT);
    lng(4, RESET_PC);
    for (int v = 2; v < 256; ++v)
        lng((uint32_t)v * 4, HANDLER + (uint32_t)v * 8u);
    mc68k_reset(cpu_g);
    cpu_g->usp = USP_INIT;
    memset(cpu_g->atc060_i, 0, sizeof cpu_g->atc060_i);
    memset(cpu_g->atc060_d, 0, sizeof cpu_g->atc060_d);
    memset(cpu_g->atc060_hint, 0, sizeof cpu_g->atc060_hint);
    memset(&cpu_g->atc060_ifast, 0, sizeof cpu_g->atc060_ifast);
    cpu_g->atc060_clock = 0;
    cpu_g->atc060_dfast_disable = disable_fast;
    cpu_g->mmu060_urp = ROOT;
    cpu_g->mmu060_srp = ROOT;
    cpu_g->mmu060_itt0 = TTR_LOW_ALL; /* 命令フェッチは恒等。データ側は ATC を通す */
    cpu_g->mmu060_itt1 = 0;
    cpu_g->mmu060_dtt0 = cpu_g->mmu060_dtt1 = 0;
    cpu_g->mmu060_tc = TC_E;
    lng(ROOT, PTR | DESC_TABLE);
    lng(PTR, PT_LOW0 | DESC_TABLE);
    lng(PTR + PTR_INDEX_LOW1 * 4u, PT_LOW1 | DESC_TABLE);
    lng(PTR + PTR_INDEX_DATA * 4u, PT_DATA | DESC_TABLE);
    for (unsigned i = 0; i < LOW_PAGES; i++) {
        lng(PT_LOW0 + i * 4u, (i * PAGE_4K) | DESC_RESIDENT);
        lng(PT_LOW1 + i * 4u, (LOW_PAGES * PAGE_4K + i * PAGE_4K) | DESC_RESIDENT);
    }
    lng(PT_DATA, slot_base(SLOT_8K) | DESC_RESIDENT);
    map_data_page(SLOT_OLD, 0);
    lng(slot_base(SLOT_OLD), MARK_OLD);
    lng(slot_base(SLOT_NEW), MARK_NEW);
    lng(slot_base(SLOT_8K) + DATA_OFFSET, MARK_8K);
    lng(TT_RAM_BASE + DATA_OFFSET, MARK_TT);
    word(CODE_ADDR, OP_TAS_A0);
    word(CODE_ADDR + 2, OP_MOVE_A0_D2);
    word(CODE_ADDR + 4, OP_ADDQ_D1);
    word(CODE_ADDR + 6, OP_MOVE_D1_A0);
    word(CODE_ADDR + 8, OP_BRA_BACK);
    cpu_g->pc = CODE_ADDR;
    cpu_g->a[0] = data_addr_g;
    cpu_g->d[1] = 0;
    cpu_set_sr(cpu_g, SR_SUP);
    cpu_g->last_exception_vector = VEC_NONE;
}

static void run_steps(step_t *tr, unsigned *n, int steps) {
    for (int i = 0; i < steps; i++) {
        cpu_g->last_exception_vector = VEC_NONE;
        const int cycles = mc68k_execute(cpu_g, 1);
        tr[*n] = (step_t){.pc = cpu_g->pc,
                          .d1 = cpu_g->d[1],
                          .d2 = cpu_g->d[2],
                          .clock = (uint32_t)cpu_g->atc060_clock,
                          .sr = cpu_g->sr,
                          .cycles = cycles,
                          .vector = (uint16_t)cpu_g->last_exception_vector,
                          .tt_hit = cpu_g->access_tt_hit,
                          .cache_mode = cpu_g->access_cache_mode,
                          .atc_hash = hash_atc(cpu_g),
                          .ram_hash = hash_ram()};
        (*n)++;
    }
}

/* ---- 各場面の操作 ---- */
static void go_loop(void) {
    cpu_g->pc = CODE_ADDR;
    cpu_g->a[0] = data_addr_g;
}
/* 低位メモリで特権命令 1 つを実行する。 */
static void exec_at_test_pc(uint16_t op) {
    word(TEST_PC, op);
    const uint32_t pc = cpu_g->pc;
    cpu_g->pc = TEST_PC;
    cpu_g->last_exception_vector = VEC_NONE;
    mc68k_execute(cpu_g, 1);
    if (cpu_g->last_exception_vector != VEC_NONE)
        abort();
    cpu_g->pc = pc;
}
static mc68k_atc_entry_t *data_entry(void) {
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_d[i].valid && cpu_g->atc060_d[i].logical_page == DATA_ADDR)
            return &cpu_g->atc060_d[i];
    abort();
}
static void op_none(void) {
}
static void op_pflusha_remap(void) {
    map_data_page(SLOT_NEW, 0);
    exec_at_test_pc(OP_PFLUSHA);
}
static void op_pflusha_wp(void) {
    map_data_page(SLOT_OLD, DESC_W);
    exec_at_test_pc(OP_PFLUSHA);
}
static void op_entry_wp(void) {
    data_entry()->write_protect = true;
}
static void op_entry_clear_m(void) {
    data_entry()->modified = false;
}
static void op_entry_overwrite_physical(void) {
    data_entry()->physical_page = slot_base(SLOT_NEW);
}
static void op_entry_overwrite_logical(void) {
    map_data_page(SLOT_NEW, 0);
    data_entry()->logical_page = DATA_ADDR + 0x100000u;
}
static void op_entry_invalidate(void) {
    map_data_page(SLOT_NEW, 0);
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        cpu_g->atc060_d[i].valid = false;
}
static void op_tc_page8k(void) {
    cpu_g->mmu060_tc = TC_E | TC_P;
    exec_at_test_pc(OP_PFLUSHA);
}
/* 8KB ページの entry（logical_page = 0x01000000）が残ったまま 4KB に戻し、その 4KB ページ
 * 先頭と一致する別アドレスをアクセスする。page_shift の照合が無いと古い 8KB entry を使う。 */
static void op_tc_page4k_alias(void) {
    cpu_g->mmu060_tc = TC_E;
    lng(PT_DATA, slot_base(SLOT_NEW) | DESC_RESIDENT);
    data_addr_g = LOG_BASE + 0x800u;
}
static void op_tc_page8k_noflush(void) {
    cpu_g->mmu060_tc = TC_E | TC_P;
}
static void op_dtt1_region(void) {
    cpu_g->mmu060_dtt1 = TTR_REGION_01;
}
static void op_dtt0_region(void) {
    cpu_g->mmu060_dtt0 = TTR_REGION_01;
}
static void op_dtt1_clear(void) {
    cpu_g->mmu060_dtt1 = 0;
}
static void op_tc_disable(void) {
    cpu_g->mmu060_tc = 0;
}
static void op_tc_enable(void) {
    cpu_g->mmu060_tc = TC_E;
}
static void op_to_user(void) {
    cpu_set_sr(cpu_g, SR_USER);
}
static void op_to_super(void) {
    cpu_set_sr(cpu_g, SR_SUP);
}

typedef struct {
    const char *name;
    void (*op1)(void);
    void (*op2)(void);
    uint32_t d2_after1; /* 1 つ目の操作後の最初の MOVE.L (A0),D2 の値。0 は検証しない */
    uint32_t d2_after2;
    int vec_after1; /* 1 つ目の操作後のフェーズ内で期待する例外ベクタ。-1 は検証しない */
} scenario_t;

#define NOCHK 0u
#define NOVEC (-1)

static const scenario_t scenarios[] = {
    {"no perturbation", op_none, op_none, NOCHK, NOCHK, NOVEC},
    {"PFLUSHA + remap", op_pflusha_remap, op_none, MARK_NEW, NOCHK, NOVEC},
    {"PFLUSHA + W=1 descriptor (WP fault)", op_pflusha_wp, op_none, NOCHK, NOCHK,
     (int)VEC_ACCESS},
    {"ATC entry W set directly (WP fault)", op_entry_wp, op_none, NOCHK, NOCHK, (int)VEC_ACCESS},
    {"ATC entry M cleared (walk sets M)", op_entry_clear_m, op_none, NOCHK, NOCHK, NOVEC},
    {"ATC entry physical overwritten in place", op_entry_overwrite_physical, op_none, MARK_NEW,
     NOCHK, NOVEC},
    {"ATC entry logical page overwritten", op_entry_overwrite_logical, op_none, MARK_NEW,
     NOCHK, NOVEC},
    {"ATC entry invalidated", op_entry_invalidate, op_none, MARK_NEW, NOCHK, NOVEC},
    {"TC.P 4KB -> 8KB", op_tc_page8k, op_none, MARK_8K, NOCHK, NOVEC},
    {"TC.P 4KB -> 8KB -> 4KB alias without flush", op_tc_page8k, op_tc_page4k_alias, MARK_8K,
     NOCHK, NOVEC},
    {"TC.P 4KB -> 8KB without flush", op_tc_page8k_noflush, op_none, NOCHK, NOCHK, NOVEC},
    {"DTT1 region TT", op_dtt1_region, op_none, MARK_TT, NOCHK, NOVEC},
    {"DTT1 region TT then removed", op_dtt1_region, op_dtt1_clear, MARK_TT, NOCHK, NOVEC},
    {"DTT0 region TT", op_dtt0_region, op_none, MARK_TT, NOCHK, NOVEC},
    {"TC.E off then on", op_tc_disable, op_tc_enable, NOCHK, NOCHK, NOVEC},
    {"supervisor -> user -> supervisor", op_to_user, op_to_super, NOCHK, NOCHK, NOVEC},
};

static void run_scenario(const scenario_t *s, bool disable_fast, step_t *tr, unsigned *n) {
    *n = 0;
    init_cpu(disable_fast);
    run_steps(tr, n, PHASE_STEPS);
    if (tr[*n - 1].vector != VEC_NONE)
        abort();
    s->op1();
    go_loop();
    run_steps(tr, n, PHASE_STEPS);
    s->op2();
    go_loop();
    run_steps(tr, n, PHASE_STEPS);
}

static void test_scenario(const scenario_t *s) {
    step_t fast[MAX_STEPS], slow[MAX_STEPS];
    unsigned nf, ns;
    run_scenario(s, false, fast, &nf);
    run_scenario(s, true, slow, &ns);
    if (nf != ns || nf != MAX_STEPS)
        abort();
    for (unsigned i = 0; i < nf; i++) {
        if (fast[i].pc != slow[i].pc || fast[i].d1 != slow[i].d1 || fast[i].d2 != slow[i].d2 ||
            fast[i].clock != slow[i].clock || fast[i].sr != slow[i].sr ||
            fast[i].cycles != slow[i].cycles || fast[i].vector != slow[i].vector || fast[i].tt_hit != slow[i].tt_hit ||
            fast[i].cache_mode != slow[i].cache_mode ||
            fast[i].atc_hash != slow[i].atc_hash || fast[i].ram_hash != slow[i].ram_hash)
            abort();
    }
    /* 等価性だけでは slow 側の期待が崩れても通るため、操作後に古い割付を使っていないことと、
     * WP フォルトが起きることを slow 側で固定する。 */
    if (s->d2_after1 != NOCHK && slow[PHASE_STEPS + READ_STEP].d2 != s->d2_after1)
        abort();
    if (s->d2_after2 != NOCHK && slow[PHASE_STEPS * 2 + READ_STEP].d2 != s->d2_after2)
        abort();
    if (s->vec_after1 != NOVEC) {
        bool seen = false;
        for (unsigned i = PHASE_STEPS; i < PHASE_STEPS * 2; i++)
            seen |= slow[i].vector == (uint16_t)s->vec_after1;
        if (!seen)
            abort();
    }
}

/* 通常のループで D 側の hint が埋まり、W/M の立った entry を高速経路で使える状態になる。 */
static void test_hint_populated(void) {
    step_t tr[MAX_STEPS];
    unsigned n = 0;
    init_cpu(false);
    run_steps(tr, &n, PHASE_STEPS);
    const mc68k_atc_entry_t *e = data_entry();
    if (!e->modified || e->write_protect)
        abort();
    if (cpu_g->atc060_hint[0][(DATA_ADDR >> 12) & (MC68K_060_ATC_HINTS - 1u)] !=
        (uint8_t)(e - cpu_g->atc060_d))
        abort();
}

/* 高速経路が TAS（RMW）の read 部でも write の条件（W=0・M=1）を守ること:
 * M=0 の entry への TAS は walk に進み、M を立てる。 */
static void test_rmw_requires_modified(void) {
    for (int disable = 0; disable < 2; disable++) {
        step_t tr[MAX_STEPS];
        unsigned n = 0;
        init_cpu(disable != 0);
        run_steps(tr, &n, PHASE_STEPS);
        data_entry()->modified = false;
        go_loop();
        run_steps(tr, &n, 1); /* TAS */
        if (!data_entry()->modified)
            abort();
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
    if (!cpu_g)
        abort();
    test_hint_populated();
    test_rmw_requires_modified();
    for (unsigned i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++)
        test_scenario(&scenarios[i]);
    mc68k_destroy(cpu_g);
    return 0;
}
