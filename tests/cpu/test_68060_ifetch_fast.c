/*
 * MC68060 命令フェッチの ATC 変換省略（高速経路, #125）の試験。
 * 設計書 docs/mc68000-internals/reference/cpu_68060.md §5（命令フェッチ高速経路）。
 *
 * 同じ手順を「高速経路あり」と「なし（atc060_ifast_disable）」で実行し、各命令後の PC・D0・
 * 消費サイクル・例外ベクタ・ATC の内容（LRU の history を含む）・atc060_clock が一致することを
 * 確認する。あわせて、PFLUSH・ATC entry の上書き・TC/ITT の変更・user↔supervisor 切替などの後に
 * 古い entry を使わないこと（D0 が新しい割付のコードの値になること）を固定する。
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
#define HANDLER       0x3000u
#define SR_SUP        0x2700
#define SR_USER       0x0000
#define VEC_NONE      0xffffu
#define TT_REGION     0x01000000u /* ITT で恒等にした論理領域。RAM の TT_RAM_BASE に折り返す */
#define TT_RAM_BASE   0x40000u
#define TT_ADDR_MASK  0x00ffffffu
#define ROOT_U        0x10000u
#define PTR_U         0x10200u
#define PAGE_U        0x10400u
#define ROOT_S        0x11000u
#define PTR_S         0x11200u
#define PAGE_S        0x11400u
#define PHYS_BASE     0x20000u
#define PHYS_SLOT     0x2000u /* TC.P=1（8KB）でも使えるよう 8KB 単位で置く */
#define LOG_BASE      0x01000000u
#define LOOP_OFFSET   0x1000u /* 4KB ページの page1、8KB ページでは page0 の後半 */
#define LOOP_ADDR     (LOG_BASE + LOOP_OFFSET)
#define PTR_INDEX     64u
#define DESC_RESIDENT 1u
#define DESC_TABLE    3u
#define TC_E          0x8000u
#define TC_P          0x4000u
#define TTR_E         0x8000u
#define TTR_LOW_ALL   0x0000c000u /* base $00・mask $00・E=1・S=任意 */
#define TTR_REGION_01 0x0100c000u /* base $01・mask $00・E=1・S=任意 */
#define OP_MOVEQ_D0   0x7000u
#define OP_BRA_BACK4  0x60fcu /* BRA.S *-2: MOVEQ へ戻る */
#define OP_PFLUSHA    0xf518u
#define OP_MOVEC_TO   0x4e7bu /* MOVEC Rn,Rc */
#define MOVEC_FROM_D1 0x1000u /* 拡張語: 汎用レジスタ D1（bit15=0、bit14-12=1） */
#define CREG_TC       0x003u
#define CREG_ITT0     0x004u
#define CREG_ITT1     0x005u
#define PHASE_STEPS   4
#define MAX_STEPS     (PHASE_STEPS * 3)

/* 論理 page n に割り当てる値 k（D0 に載る）。 */
#define K_OLD     1u
#define K_NEW     2u
#define K_8K      3u
#define K_TT      4u
#define K_USER    5u
#define SLOT_OLD  0u
#define SLOT_NEW  1u
#define SLOT_8K   2u
#define SLOT_USER 3u

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
/* 全アクセスに同じ待ちを課す。CACR.IE=0 の命令フェッチ（高速経路が直接バスを読む）でも、待ちの
 * 計上が通常経路と一致して消費サイクルに載ること（#134）を、各場面の比較で見る。 */
#define TEST_WAIT_CYCLES 3u
static void timing_cb(void *c, const mc68k_bus_access_t *a, mc68k_bus_timing_t *t) {
    (void)c;
    (void)a;
    t->wait_cycles = TEST_WAIT_CYCLES;
    t->cacheable = false;
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
/* phys の addr に MOVEQ #k,D0; BRA.S back を置く。 */
static void place_loop(uint32_t addr, unsigned k) {
    word(addr, (uint16_t)(OP_MOVEQ_D0 | k));
    word(addr + 2, OP_BRA_BACK4);
}
/* 4KB ページ時の page1 の descriptor を物理 slot へ向ける。 */
static void map_page1(bool user, unsigned slot) {
    lng((user ? PAGE_U : PAGE_S) + 4u, slot_base(slot) | DESC_RESIDENT);
}

typedef struct {
    uint32_t pc, d0, clock;
    int cycles;
    uint16_t vector;
    uint64_t atc_hash;
} step_t;

static uint64_t hash_atc(const mc68k_state_t *cpu) {
    uint64_t h = 1469598103934665603ull;
    const mc68k_atc_entry_t *atc = cpu->atc060_i;
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++) {
        const uint32_t v[] = {
            atc[i].valid,         atc[i].fc,      atc[i].page_shift, atc[i].logical_page,
            atc[i].physical_page, atc[i].history, atc[i].cache_mode, atc[i].cache_inhibit};
        for (unsigned j = 0; j < sizeof v / sizeof v[0]; j++)
            h = (h ^ v[j]) * 1099511628211ull;
    }
    return h;
}

static void init_cpu(bool disable_fast) {
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
    cpu_g->atc060_ifast_disable = disable_fast;
    cpu_g->mmu060_urp = ROOT_U;
    cpu_g->mmu060_srp = ROOT_S;
    cpu_g->mmu060_itt0 = cpu_g->mmu060_dtt0 = TTR_LOW_ALL;
    cpu_g->mmu060_itt1 = cpu_g->mmu060_dtt1 = 0;
    cpu_g->mmu060_tc = TC_E;
    /* user・supervisor の 2 本の木。page0（8KB 時は page0 全体）と page1 を持つ。 */
    lng(ROOT_U, PTR_U | DESC_TABLE);
    lng(ROOT_S, PTR_S | DESC_TABLE);
    lng(PTR_U + PTR_INDEX * 4u, PAGE_U | DESC_TABLE);
    lng(PTR_S + PTR_INDEX * 4u, PAGE_S | DESC_TABLE);
    lng(PAGE_S, slot_base(SLOT_8K) | DESC_RESIDENT);
    lng(PAGE_U, slot_base(SLOT_8K) | DESC_RESIDENT);
    map_page1(false, SLOT_OLD);
    map_page1(true, SLOT_USER);
    place_loop(slot_base(SLOT_OLD), K_OLD);
    place_loop(slot_base(SLOT_NEW), K_NEW);
    place_loop(slot_base(SLOT_8K) + LOOP_OFFSET, K_8K);
    place_loop(slot_base(SLOT_USER), K_USER);
    place_loop(TT_RAM_BASE + LOOP_OFFSET, K_TT);
    cpu_g->pc = LOOP_ADDR;
    cpu_set_sr(cpu_g, SR_SUP);
    cpu_g->last_exception_vector = VEC_NONE;
}

static void run_steps(step_t *tr, unsigned *n, int steps) {
    for (int i = 0; i < steps; i++) {
        cpu_g->last_exception_vector = VEC_NONE;
        const int cycles = mc68k_execute(cpu_g, 1);
        tr[*n] = (step_t){.pc = cpu_g->pc,
                          .d0 = cpu_g->d[0],
                          .clock = cpu_g->atc060_clock,
                          .cycles = cycles,
                          .vector = (uint16_t)cpu_g->last_exception_vector,
                          .atc_hash = hash_atc(cpu_g)};
        (*n)++;
    }
}

/* ---- 各場面の操作 ----
 * ATC の entry と TC・ITT をゲスト命令を介さず直接書き換える操作は、命令フェッチ高速経路の
 * 世代を進めない。直接書き換えの後は cpu_ifetch_fast_invalidate() を呼び、無効化イベントとして
 * 通知する。 */
static void go_loop(void) {
    cpu_g->pc = LOOP_ADDR;
}
/* 低位メモリの TT 領域で特権命令 1 つを実行する。 */
static void exec_at_test_pc(uint16_t op) {
    word(TEST_PC, op);
    const uint32_t pc = cpu_g->pc;
    cpu_g->pc = TEST_PC;
    cpu_g->last_exception_vector = VEC_NONE;
    mc68k_execute(cpu_g, 1);
    assert(cpu_g->last_exception_vector == VEC_NONE);
    cpu_g->pc = pc;
}
static void op_none(void) {
}
static void op_pflusha_remap(void) {
    map_page1(false, SLOT_NEW);
    exec_at_test_pc(OP_PFLUSHA);
}
static void op_entry_overwrite_physical(void) {
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid && cpu_g->atc060_i[i].logical_page == LOOP_ADDR)
            cpu_g->atc060_i[i].physical_page = slot_base(SLOT_NEW);
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_entry_overwrite_logical(void) {
    map_page1(false, SLOT_NEW);
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid && cpu_g->atc060_i[i].logical_page == LOOP_ADDR)
            cpu_g->atc060_i[i].logical_page = LOOP_ADDR + 0x100000u;
    cpu_ifetch_fast_invalidate(cpu_g);
}
/* ATC entry の物理先を RAM の外へ向ける。次のフェッチは高速経路でバスエラーになる（#134）。 */
#define PHYS_BEYOND_RAM 0x100000u
static void op_entry_overwrite_bad_physical(void) {
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid && cpu_g->atc060_i[i].logical_page == LOOP_ADDR)
            cpu_g->atc060_i[i].physical_page = PHYS_BEYOND_RAM;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_entry_invalidate(void) {
    map_page1(false, SLOT_NEW);
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        cpu_g->atc060_i[i].valid = false;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_tc_page8k(void) {
    cpu_g->mmu060_tc = TC_E | TC_P;
    exec_at_test_pc(OP_PFLUSHA); /* ページ幅が変わるので、実機同様に ATC を捨てる */
}
static void op_tc_page8k_noflush(void) {
    cpu_g->mmu060_tc = TC_E | TC_P;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_itt1_region(void) {
    cpu_g->mmu060_itt1 = TTR_REGION_01;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_itt0_region(void) {
    cpu_g->mmu060_itt0 = TTR_REGION_01;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_tc_disable_enable(void) {
    cpu_g->mmu060_tc = 0;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_tc_reenable(void) {
    cpu_g->mmu060_tc = TC_E;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_to_user(void) {
    cpu_set_sr(cpu_g, SR_USER);
    cpu_g->pc = LOOP_ADDR;
}
static void op_to_super(void) {
    cpu_set_sr(cpu_g, SR_SUP);
    cpu_g->pc = LOOP_ADDR;
}
static void op_srp_urp_change(void) {
    map_page1(false, SLOT_NEW);
    cpu_g->mmu060_srp = ROOT_U;
    cpu_g->mmu060_urp = ROOT_S;
}
static void op_reset_restore(void) {
    mc68k_reset(cpu_g);
    cpu_g->mmu060_tc = TC_E;
    cpu_g->mmu060_itt0 = TTR_LOW_ALL;
    cpu_ifetch_fast_invalidate(cpu_g);
    cpu_g->pc = LOOP_ADDR;
    cpu_set_sr(cpu_g, SR_SUP);
    map_page1(false, SLOT_NEW);
}

/* ゲスト命令 MOVEC D1,<creg> を低位メモリで実行する。TC・ITT0/1 はこの経路で書かれるので、
 * 直接書き換える操作と違い、無効化が実装側から行われることを確かめられる。 */
static void exec_movec_d1_to(uint32_t creg, uint32_t value) {
    word(TEST_PC, OP_MOVEC_TO);
    word(TEST_PC + 2, (uint16_t)(MOVEC_FROM_D1 | creg));
    const uint32_t pc = cpu_g->pc;
    const uint32_t d1 = cpu_g->d[1];
    cpu_g->pc = TEST_PC;
    cpu_g->d[1] = value;
    cpu_g->last_exception_vector = VEC_NONE;
    mc68k_execute(cpu_g, 1);
    assert(cpu_g->last_exception_vector == VEC_NONE);
    cpu_g->d[1] = d1;
    cpu_g->pc = pc;
}
static void op_tc_page8k_noflush_movec(void) {
    exec_movec_d1_to(CREG_TC, TC_E | TC_P);
}
static void op_itt1_region_movec(void) {
    exec_movec_d1_to(CREG_ITT1, TTR_REGION_01);
}
static void op_itt0_region_movec(void) {
    exec_movec_d1_to(CREG_ITT0, TTR_REGION_01);
}
static void op_tc_disable_movec(void) {
    exec_movec_d1_to(CREG_TC, 0);
}
static void op_tc_reenable_movec(void) {
    exec_movec_d1_to(CREG_TC, TC_E);
}
/* reset は TC と ITT を書き換える。その後は何も戻さず、高速経路が古い TC・ITT
 * を使わないことを見る。 */
static void op_reset_only(void) {
    mc68k_reset(cpu_g);
    cpu_g->pc = LOOP_ADDR;
    cpu_set_sr(cpu_g, SR_SUP);
}

typedef struct {
    const char *name;
    void (*op1)(void);
    void (*op2)(void);
    uint32_t d0_after1; /* 1 つ目の操作後の D0。0 は検証しない（等価性だけを見る） */
    uint32_t d0_after2;
} scenario_t;

static const scenario_t scenarios[] = {
    {"no perturbation", op_none, op_none, K_OLD, K_OLD},
    {"PFLUSHA + remap", op_pflusha_remap, op_none, K_NEW, K_NEW},
    {"ATC entry physical overwritten in place", op_entry_overwrite_physical, op_none, K_NEW,
     K_NEW},
    {"ATC entry logical page overwritten", op_entry_overwrite_logical, op_none, K_NEW, K_NEW},
    {"ATC entry points beyond RAM (fetch bus error)", op_entry_overwrite_bad_physical, op_none,
     0, 0},
    {"ATC entry invalidated", op_entry_invalidate, op_none, K_NEW, K_NEW},
    {"TC.P 4KB -> 8KB", op_tc_page8k, op_none, K_8K, K_8K},
    {"TC.P 4KB -> 8KB without flush", op_tc_page8k_noflush, op_none, 0, 0},
    {"ITT1 region TT", op_itt1_region, op_none, K_TT, K_TT},
    {"ITT0 replaced by region TT", op_itt0_region, op_none, K_TT, K_TT},
    {"TC.E off then on", op_tc_disable_enable, op_tc_reenable, 0, K_OLD},
    {"supervisor -> user -> supervisor", op_to_user, op_to_super, K_USER, K_OLD},
    {"SRP/URP change keeps ATC", op_srp_urp_change, op_none, K_OLD, K_OLD},
    {"reset keeps ATC", op_reset_restore, op_none, 0, 0},
    {"TC.P 4KB -> 8KB without flush (MOVEC)", op_tc_page8k_noflush_movec, op_none, 0, 0},
    {"ITT1 region TT (MOVEC)", op_itt1_region_movec, op_none, K_TT, K_TT},
    {"ITT0 replaced by region TT (MOVEC)", op_itt0_region_movec, op_none, K_TT, K_TT},
    {"TC.E off then on (MOVEC)", op_tc_disable_movec, op_tc_reenable_movec, 0, K_OLD},
    {"reset clears TC and ITT", op_reset_only, op_none, 0, 0},
};

static void run_scenario(const scenario_t *s, bool disable_fast, step_t *tr, unsigned *n) {
    *n = 0;
    init_cpu(disable_fast);
    run_steps(tr, n, PHASE_STEPS);
    assert(tr[*n - 1].d0 == K_OLD);
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
    assert(nf == ns && nf == MAX_STEPS);
    for (unsigned i = 0; i < nf; i++) {
        if (fast[i].pc != slow[i].pc || fast[i].d0 != slow[i].d0 ||
            fast[i].clock != slow[i].clock || fast[i].cycles != slow[i].cycles ||
            fast[i].vector != slow[i].vector || fast[i].atc_hash != slow[i].atc_hash)
            abort();
    }
    /* 操作後に古い割付を使っていないこと（等価性だけでは slow 側の期待が崩れても通るため）。
     * MOVEQ→BRA の 2 命令ループなので、各フェーズの最後の MOVEQ 直後の D0 を見る。 */
    if (s->d0_after1 != 0 && slow[PHASE_STEPS * 2 - 2].d0 != s->d0_after1)
        abort();
    if (s->d0_after2 != 0 && slow[MAX_STEPS - 2].d0 != s->d0_after2)
        abort();
}

/* snapshot 復元の後でも、高速経路ありとなしで同じ結果になること。復元は
 * ATC・TC・ITT・高速経路の 記憶をスナップショットの時点へ戻すが、世代（ifast_gen）は戻さない。
 */
static uint64_t run_snapshot_restore(bool disable_fast, step_t *tr, unsigned *n) {
    *n = 0;
    init_cpu(disable_fast);
    run_steps(tr, n, PHASE_STEPS);
    assert(tr[*n - 1].d0 == K_OLD);
    mc68k_snapshot_status_t status;
    mc68k_snapshot_t *snap = mc68k_snapshot_create(cpu_g, &status);
    assert(snap && status == MC68K_SNAPSHOT_OK);
    op_pflusha_remap();
    go_loop();
    run_steps(tr, n, PHASE_STEPS);
    assert(tr[*n - 1].d0 == K_NEW);
    const uint64_t gen_before_restore = cpu_g->ifast_gen;
    mc68k_restore_plan_t *plan = mc68k_snapshot_prepare(cpu_g, snap, &status);
    assert(plan && status == MC68K_SNAPSHOT_OK);
    mc68k_snapshot_commit(cpu_g, snap, plan);
    mc68k_snapshot_destroy(snap);
    go_loop();
    run_steps(tr, n, PHASE_STEPS);
    return gen_before_restore;
}

static void test_snapshot_restore(void) {
    step_t fast[MAX_STEPS], slow[MAX_STEPS];
    unsigned nf, ns;
    const uint64_t gen_before = run_snapshot_restore(false, fast, &nf);
    /* 復元後は ATC がスナップショットの時点（旧割付）に戻る。世代は戻らず、復元前より進む。 */
    assert(cpu_g->ifast_gen > gen_before);
    assert(fast[MAX_STEPS - 2].d0 == K_OLD);
    run_snapshot_restore(true, slow, &ns);
    assert(nf == ns && nf == MAX_STEPS);
    for (unsigned i = 0; i < nf; i++) {
        if (fast[i].pc != slow[i].pc || fast[i].d0 != slow[i].d0 ||
            fast[i].clock != slow[i].clock || fast[i].cycles != slow[i].cycles ||
            fast[i].vector != slow[i].vector || fast[i].atc_hash != slow[i].atc_hash)
            abort();
    }
}

/* 高速経路が実際に働くこと: 通常の分岐ループ後に記憶が有効になる。 */
static void test_fast_path_engaged(void) {
    step_t tr[MAX_STEPS];
    unsigned n = 0;
    init_cpu(false);
    assert(!cpu_g->atc060_ifast.valid);
    run_steps(tr, &n, PHASE_STEPS);
    assert(cpu_g->atc060_ifast.valid);
    assert(cpu_g->atc060_ifast.logical_page == LOOP_ADDR);
    init_cpu(true);
    run_steps(tr, &n, PHASE_STEPS);
    assert(!cpu_g->atc060_ifast.valid);
}

/* 待ちの計上が消費サイクルに載っていること（上の比較が待ち無しで通る見かけ倒しを防ぐ）。 */
static void test_wait_is_charged(void) {
    step_t with_wait[MAX_STEPS], no_wait[MAX_STEPS];
    unsigned n1 = 0, n2 = 0;
    init_cpu(false);
    run_steps(with_wait, &n1, PHASE_STEPS);
    void (*saved)(void *, const mc68k_bus_access_t *, mc68k_bus_timing_t *) =
        cpu_g->bus.access_timing;
    cpu_g->bus.access_timing = NULL;
    init_cpu(false);
    run_steps(no_wait, &n2, PHASE_STEPS);
    cpu_g->bus.access_timing = saved;
    int sum_with = 0, sum_no = 0;
    for (unsigned i = 0; i < PHASE_STEPS; i++) {
        sum_with += with_wait[i].cycles;
        sum_no += no_wait[i].cycles;
    }
    if (sum_with <= sum_no)
        abort();
}

/* atc060_clock が 32bit を超えても、直近に使った entry が追い出されない（#125）。 */
static void test_clock_beyond_32bit(void) {
    step_t tr[MAX_STEPS];
    unsigned n = 0;
    init_cpu(false);
    cpu_g->atc060_clock = UINT64_C(0xfffffffe);
    run_steps(tr, &n, MAX_STEPS);
    if (cpu_g->atc060_clock <= UINT64_C(0xffffffff))
        abort();
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid && cpu_g->atc060_i[i].history > cpu_g->atc060_clock)
            abort();
    /* 周回しないので、時刻は単調に増える（直近の entry の history が最大）。 */
    uint64_t newest = 0;
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid && cpu_g->atc060_i[i].history > newest)
            newest = cpu_g->atc060_i[i].history;
    if (newest != cpu_g->atc060_clock)
        abort();
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
                       .access_timing = timing_cb,
                       .ctx = &f};
    cpu_g = mc68k_create_model(&bus, MC68K_MODEL_68060);
    if (!cpu_g)
        abort();
    test_fast_path_engaged();
    test_clock_beyond_32bit();
    test_wait_is_charged();
    for (unsigned i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++)
        test_scenario(&scenarios[i]);
    test_snapshot_restore();
    mc68k_destroy(cpu_g);
    return 0;
}
