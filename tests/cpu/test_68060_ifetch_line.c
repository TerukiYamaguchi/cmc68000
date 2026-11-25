/*
 * MC68060 命令側ライン高速経路（cpu_060_ifetch_fast, #129）の試験。
 * 設計書 docs/mc68000-internals/reference/cpu_68060.md §5（命令フェッチ高速経路）。
 *
 * 同じ手順を「高速経路あり」と「なし（ifetch_fast_disable）」で実行し、各命令後の PC・D0・
 * 消費サイクル・例外ベクタ・ATC の内容（LRU の history を含む）・I-cache の全ライン・フォルト
 * 記録（fault_*、access_cache_mode、access_tt_hit）が一致することを確認する。あわせて、CINV・
 * ラインの無効化と置換・CACR.IE 切替・キャッシュ不可 entry・PFLUSH などの後に古いラインを
 * 使わないことを固定する。高速経路が実際に働くことは、同一セットに内容の違う重複ラインを
 * 作り、記憶したラインを読むかどうかで確認する。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE       0x80000u
#define SSP_INIT       0x8000u
#define RESET_PC       0x100u
#define TEST_PC        0x400u
#define HANDLER        0x3000u
#define SR_SUP         0x2700
#define SR_USER        0x0000
#define VEC_NONE       0xffffu
#define ROOT_U         0x10000u
#define PTR_U          0x10200u
#define PAGE_U         0x10400u
#define ROOT_S         0x11000u
#define PTR_S          0x11200u
#define PAGE_S         0x11400u
#define PHYS_BASE      0x20000u
#define PHYS_SLOT      0x2000u /* TC.P=1（8KB）でも使えるよう 8KB 単位で置く */
#define LOG_BASE       0x01000000u
#define LOOP_OFFSET    0x1000u /* 4KB ページの page1、8KB ページでは page0 の後半 */
#define CODE_OFFSET    8u      /* ループ先頭のライン内位置。6 語で 16 バイト境界をまたぐ */
#define LOOP_ADDR      (LOG_BASE + LOOP_OFFSET + CODE_OFFSET)
#define PAGE2_OFFSET   0x2000u /* 4KB ページの page2（SLOT_NEW のコードへ割り当てる） */
#define PAGE2_ADDR     (LOG_BASE + PAGE2_OFFSET + CODE_OFFSET)
#define PTR_INDEX      64u
#define DESC_RESIDENT  1u
#define DESC_TABLE     3u
#define CM_NONCACHE    (2u << 5) /* descriptor の CM=2（noncachable, precise） */
#define TC_E           0x8000u
#define TC_P           0x4000u
#define TTR_LOW_ALL    0x0000c000u /* base $00・mask $00・E=1 */
#define TTR_REGION_01  0x0100c000u /* base $01・mask $00・E=1 */
#define TT_REGION      0x01000000u /* ITT で恒等にした論理領域。RAM の TT_RAM_BASE に折り返す */
#define TT_RAM_BASE    0x40000u
#define TT_ADDR_MASK   0x00ffffffu
#define K_TT           4u
#define OP_MOVEQ_D0    0x7000u
#define OP_MOVEQ_D1    0x7200u
#define OP_NOP         0x4e71u
#define OP_MOVEB_A1_D2 0x1411u /* MOVE.B (A1),D2: 命令の間にデータ読み出しを挟む */
#define OP_BRA_BACK    0x60ecu /* BRA.S: 先頭へ戻る（10 語ループ） */
#define LOOP_WORDS     10
#define WORD_MOVEQ_D0  1 /* 先頭ライン A の 2 語目（ライン内の先頭語は通常経路になる） */
#define WORD_MOVEB     2
#define WORD_MOVEQ_D1  5 /* ライン B の 2 語目 */
#define DATA_ADDR      0x500u
#define LINE_B_OFFSET  0x10u
#define OP_PFLUSHA     0xf518u
#define OP_CINVL_I     0xf488u /* CINVL I,(A0) */
#define OP_CINVP_I     0xf490u /* CINVP I,(A0) */
#define OP_CINVA_I     0xf498u /* CINVA I */
#define PHASE_STEPS    (LOOP_WORDS * 2)
#define MAX_STEPS      (PHASE_STEPS * 3)

/* 論理 page 1 に割り当てるコードが D0・D1 に載せる値。 */
#define K_OLD     1u
#define K_NEW     2u
#define K_8K      3u
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
static uint32_t loop_pc; /* go_loop() が戻る先。場面の操作で別ページへ向けられる */

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
/* 物理 base（ページ内の LOOP_OFFSET 相当）にループを置く。MOVEQ #k,D0（A ライン）と
 * MOVEQ #k,D1（B ライン）は、各ラインの 2 語目に置く（各ラインの先頭語は必ず通常経路になる）。
 */
static void place_loop(uint32_t base, unsigned k) {
    const uint32_t a = base + CODE_OFFSET;
    for (int i = 0; i < LOOP_WORDS; i++)
        word(a + (uint32_t)i * 2u, OP_NOP);
    word(a + WORD_MOVEQ_D0 * 2u, (uint16_t)(OP_MOVEQ_D0 | k));
    word(a + WORD_MOVEB * 2u, OP_MOVEB_A1_D2);
    word(a + WORD_MOVEQ_D1 * 2u, (uint16_t)(OP_MOVEQ_D1 | k));
    word(a + (LOOP_WORDS - 1) * 2u, OP_BRA_BACK);
}
/* 4KB ページ時の page1 の descriptor を物理 slot へ向ける。 */
static void map_page1(bool user, unsigned slot, uint32_t extra) {
    lng((user ? PAGE_U : PAGE_S) + 4u, slot_base(slot) | DESC_RESIDENT | extra);
}
/* ループ先頭（MOVEQ）の物理アドレス。4KB の page1 は物理 slot の先頭にある。 */
static uint32_t loop_phys(unsigned slot) {
    return slot_base(slot) + CODE_OFFSET;
}

typedef struct {
    uint32_t pc, d0, d1;
    int cycles;
    uint16_t vector;
    uint64_t atc_hash, icache_hash, fault_hash;
    uint64_t clock;
} step_t;

static uint64_t mix(uint64_t h, uint64_t v) {
    return (h ^ v) * 1099511628211ull;
}

static uint64_t hash_atc(const mc68k_state_t *cpu) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++) {
        const mc68k_atc_entry_t *e = &cpu->atc060_i[i];
        const uint64_t v[] = {e->valid,         e->fc,      e->page_shift, e->logical_page,
                              e->physical_page, e->history, e->cache_mode, e->cache_inhibit};
        for (unsigned j = 0; j < sizeof v / sizeof v[0]; j++)
            h = mix(h, v[j]);
    }
    return h;
}

/* I-cache
 * の全ライン（有効・ロード済み・dirty・物理ライン・データ）、置換位置、直近にヒットまたは
 * 充填したラインの添字（高速経路が同じセットの別ウェイを探したときも通常経路と同じ値になる）。
 */
static uint64_t hash_icache(const mc68k_state_t *cpu) {
    uint64_t h = 1469598103934665603ull;
    h = mix(h, cpu->icache040_last);
    for (unsigned set = 0; set < MC68K_CACHE_SETS_MAX; set++) {
        h = mix(h, cpu->icache040_next[set]);
        for (unsigned way = 0; way < MC68K_040_CACHE_WAYS; way++) {
            const mc68k_040_cache_line_t *l = &cpu->icache040[set][way];
            h = mix(h, (uint64_t)l->valid | (uint64_t)l->loaded << 1 | (uint64_t)l->dirty << 2);
            h = mix(h, l->physical_line);
            for (unsigned i = 0; i < 4; i++)
                h = mix(h, l->data[i]);
        }
    }
    return h;
}

static uint64_t hash_fault(const mc68k_state_t *cpu) {
    uint64_t h = 1469598103934665603ull;
    const uint64_t v[] = {cpu->fault_access.fc,          cpu_noted_size_code(cpu),
                          cpu->fault_access.rmw,         cpu->fault_access.alternate,
                          cpu->fault_access.has_origin,  cpu->fault_access.origin,
                          cpu->fault_access.origin_size, cpu->fault_access_kind,
                          cpu->access_cache_mode,        cpu->access_tt_hit};
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
        h = mix(h, v[i]);
    return h;
}

static void init_cpu(bool disable_fast) {
    memset(f_g->ram, 0, sizeof f_g->ram);
    lng(0, SSP_INIT);
    lng(4, RESET_PC);
    for (int v = 2; v < 256; ++v)
        lng((uint32_t)v * 4, HANDLER + (uint32_t)v * 8u);
    mc68k_reset(cpu_g);
    memset(cpu_g->atc060_i, 0, sizeof cpu_g->atc060_i);
    memset(cpu_g->atc060_d, 0, sizeof cpu_g->atc060_d);
    memset(cpu_g->atc060_hint, 0, sizeof cpu_g->atc060_hint);
    memset(&cpu_g->atc060_ifast, 0, sizeof cpu_g->atc060_ifast);
    cpu_g->atc060_clock = 0;
    cpu_g->atc060_ifast_disable = false;
    cpu_g->ifetch_fast_disable = disable_fast;
    cpu_040_cache_reset(cpu_g);
    cpu_g->cacr = MC68K_CACR_040_IE;
    cpu_g->mmu060_urp = ROOT_U;
    cpu_g->mmu060_srp = ROOT_S;
    cpu_g->mmu060_itt0 = TTR_LOW_ALL;
    cpu_g->mmu060_dtt0 = TTR_LOW_ALL | CM_NONCACHE; /* データ側は TT ヒットかつ noncachable */
    cpu_g->mmu060_itt1 = cpu_g->mmu060_dtt1 = 0;
    cpu_g->mmu060_tc = TC_E;
    /* user・supervisor の 2 本の木。page0（8KB 時は page0 全体）と page1 を持つ。 */
    lng(ROOT_U, PTR_U | DESC_TABLE);
    lng(ROOT_S, PTR_S | DESC_TABLE);
    lng(PTR_U + PTR_INDEX * 4u, PAGE_U | DESC_TABLE);
    lng(PTR_S + PTR_INDEX * 4u, PAGE_S | DESC_TABLE);
    lng(PAGE_S, slot_base(SLOT_8K) | DESC_RESIDENT);
    lng(PAGE_U, slot_base(SLOT_8K) | DESC_RESIDENT);
    map_page1(false, SLOT_OLD, 0);
    map_page1(true, SLOT_USER, 0);
    lng(PAGE_S + 8u, slot_base(SLOT_NEW) | DESC_RESIDENT);
    lng(PAGE_U + 8u, slot_base(SLOT_NEW) | DESC_RESIDENT);
    loop_pc = LOOP_ADDR;
    place_loop(slot_base(SLOT_OLD), K_OLD);
    place_loop(slot_base(SLOT_NEW), K_NEW);
    place_loop(slot_base(SLOT_8K) + LOOP_OFFSET, K_8K);
    place_loop(slot_base(SLOT_USER), K_USER);
    place_loop(TT_RAM_BASE + LOOP_OFFSET, K_TT);
    cpu_g->a[1] = DATA_ADDR;
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
                          .d1 = cpu_g->d[1],
                          .clock = cpu_g->atc060_clock,
                          .cycles = cycles,
                          .vector = (uint16_t)cpu_g->last_exception_vector,
                          .atc_hash = hash_atc(cpu_g),
                          .icache_hash = hash_icache(cpu_g),
                          .fault_hash = hash_fault(cpu_g)};
        (*n)++;
    }
}

/* ---- 各場面の操作 ----
 * ATC の entry と TC・ITT をゲスト命令を介さず直接書き換える操作は、命令フェッチ高速経路の
 * 世代を進めない。直接書き換えの後は cpu_ifetch_fast_invalidate() を呼び、無効化イベントとして
 * 通知する。 */
static void go_loop(void) {
    cpu_g->pc = loop_pc;
}
/* 低位メモリ（ITT0 で恒等）で特権命令 1 つを実行する。A0 は呼び出し側が設定する。 */
static void exec_at_test_pc(uint16_t op) {
    word(TEST_PC, op);
    const uint32_t pc = cpu_g->pc;
    cpu_g->pc = TEST_PC;
    cpu_g->last_exception_vector = VEC_NONE;
    mc68k_execute(cpu_g, 1);
    assert(cpu_g->last_exception_vector == VEC_NONE);
    cpu_g->pc = pc;
}
/* ループ先頭のラインのメモリだけを新しいコードへ書き換える（I-cache の古いラインは残る）。 */
static void rewrite_loop_memory(void) {
    place_loop(slot_base(SLOT_OLD), K_NEW);
}
static unsigned hinted_index(void) {
    return cpu_g->icache040_last;
}
/* ループのライン（0=A: MOVEQ #k,D0、1=B: MOVEQ #k,D1）を探し、記憶位置をそこへ向けて返す。
 * 見つからなければ NULL（記憶位置は変えない）。操作の前に A へ向けておき、操作直後の最初の
 * フェッチを高速経路で処理させて、その場で条件が効くかを見る。 */
static mc68k_040_cache_line_t *aim_hint_at_line(unsigned which) {
    const uint32_t base = (loop_phys(SLOT_OLD) & ~UINT32_C(0xf)) + which * LINE_B_OFFSET;
    const unsigned set = (base >> 4) & (MC68K_060_CACHE_SETS - 1u);
    for (unsigned w = 0; w < MC68K_040_CACHE_WAYS; w++) {
        if (cpu_g->icache040[set][w].valid && cpu_g->icache040[set][w].physical_line == base) {
            cpu_g->icache040_last = (uint16_t)(set * MC68K_040_CACHE_WAYS + w);
            return &cpu_g->icache040[set][w];
        }
    }
    return NULL;
}

static void op_none(void) {
}
static void op_memory_changed_no_cinv(void) {
    rewrite_loop_memory();
}
static void op_memory_changed_cinvl(void) {
    rewrite_loop_memory();
    cpu_g->a[0] = loop_phys(SLOT_OLD);
    exec_at_test_pc(OP_CINVL_I);
    cpu_g->a[0] = loop_phys(SLOT_OLD) + LINE_B_OFFSET;
    exec_at_test_pc(OP_CINVL_I);
}
static void op_memory_changed_cinvp(void) {
    rewrite_loop_memory();
    cpu_g->a[0] = loop_phys(SLOT_OLD);
    exec_at_test_pc(OP_CINVP_I);
}
static void op_memory_changed_cinva(void) {
    rewrite_loop_memory();
    exec_at_test_pc(OP_CINVA_I);
}
static void op_cacr_ie_off(void) {
    rewrite_loop_memory();
    cpu_g->cacr &= ~MC68K_CACR_040_IE;
}
static void op_cacr_ie_on(void) {
    cpu_g->cacr |= MC68K_CACR_040_IE;
}
/* 同じセットに、同じ物理ライン A で内容の違う 2 つのウェイを作る（添字の小さい方が元の内容）。
 * 記憶位置はライン B へ向けるので、ライン A
 * のフェッチは同じセットの探索になる。通常経路は添字の
 * 小さい方（元の内容）を選ぶ。lo_loaded=false なら小さい方を valid・未ロードにする（通常経路は
 * それを選んで充填し直すので、元の内容になる）。 */
static void make_duplicate_line_a(bool lo_loaded) {
    mc68k_040_cache_line_t *line = aim_hint_at_line(0);
    assert(line);
    const unsigned a = hinted_index();
    const unsigned set = a / MC68K_040_CACHE_WAYS, way = a % MC68K_040_CACHE_WAYS;
    const unsigned other = (way + 1u) % MC68K_040_CACHE_WAYS;
    const unsigned lo = way < other ? way : other, hi = way < other ? other : way;
    mc68k_040_cache_line_t orig = *line;
    mc68k_040_cache_line_t altered = orig;
    /* MOVEQ #k,D0 は物理 CODE_OFFSET + 2（ライン内 10 バイト目）の下位 16 ビット */
    const unsigned idx = (CODE_OFFSET + WORD_MOVEQ_D0 * 2u) / 4u;
    altered.data[idx] = (altered.data[idx] & 0xffff0000u) | (OP_MOVEQ_D0 | K_NEW);
    orig.loaded = lo_loaded;
    cpu_g->icache040[set][lo] = orig;
    cpu_g->icache040[set][hi] = altered;
    (void)aim_hint_at_line(1);
}
static void op_duplicate_line_a_last_at_b(void) {
    make_duplicate_line_a(true);
}
static void op_duplicate_line_a_first_unloaded(void) {
    make_duplicate_line_a(false);
}
static void op_line_a_invalid(void) {
    rewrite_loop_memory();
    aim_hint_at_line(0)->valid = false;
}
static void op_line_a_unloaded(void) {
    rewrite_loop_memory();
    aim_hint_at_line(0)->loaded = false;
}
static void op_line_a_replaced(void) {
    rewrite_loop_memory();
    aim_hint_at_line(0)->physical_line += 0x800u; /* 同じセットの別の物理ライン */
}
static void op_line_b_invalid(void) {
    rewrite_loop_memory();
    aim_hint_at_line(1)->valid = false;
}
static void op_line_b_replaced(void) {
    rewrite_loop_memory();
    aim_hint_at_line(1)->physical_line += 0x800u;
}
static void op_hint_points_elsewhere(void) {
    rewrite_loop_memory();
    (void)aim_hint_at_line(0);
    cpu_g->icache040_last = (uint16_t)((hinted_index() + 1u) % (MC68K_CACHE_SETS_MAX * 4u));
}
static void op_nocache_entry(void) {
    rewrite_loop_memory();
    map_page1(false, SLOT_OLD, CM_NONCACHE);
    exec_at_test_pc(OP_PFLUSHA);
}
static void op_restore_cache_entry(void) {
    map_page1(false, SLOT_OLD, 0);
    exec_at_test_pc(OP_PFLUSHA);
}
/* PFLUSH なしで descriptor だけ noncachable にしても、ATC の entry は変わらない */
static void op_nocache_entry_atc_kept(void) {
    rewrite_loop_memory();
    map_page1(false, SLOT_OLD, CM_NONCACHE);
}
static void op_entry_noncachable_in_place(void) {
    rewrite_loop_memory();
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid &&
            cpu_g->atc060_i[i].logical_page == (LOOP_ADDR & ~0xfffu))
            cpu_g->atc060_i[i].cache_mode = 2;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_pflusha_remap(void) {
    map_page1(false, SLOT_NEW, 0);
    exec_at_test_pc(OP_PFLUSHA);
}
static void op_entry_invalidate(void) {
    map_page1(false, SLOT_NEW, 0);
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        cpu_g->atc060_i[i].valid = false;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_entry_overwrite_physical(void) {
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid &&
            cpu_g->atc060_i[i].logical_page == (LOOP_ADDR & ~0xfffu))
            cpu_g->atc060_i[i].physical_page = slot_base(SLOT_NEW);
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
static void op_jump_page2(void) {
    loop_pc = PAGE2_ADDR; /* 別の論理ページ。ATC・ifast の記憶は page1 のまま */
}
static void op_overwrite_logical(void) {
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid &&
            cpu_g->atc060_i[i].logical_page == (LOOP_ADDR & ~0xfffu))
            cpu_g->atc060_i[i].logical_page = (LOOP_ADDR & ~0xfffu) + 0x100000u;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_overwrite_fc(void) {
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid &&
            cpu_g->atc060_i[i].logical_page == (LOOP_ADDR & ~0xfffu))
            cpu_g->atc060_i[i].fc ^= 4u;
    cpu_ifetch_fast_invalidate(cpu_g);
}
static void op_overwrite_page_shift(void) {
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (cpu_g->atc060_i[i].valid &&
            cpu_g->atc060_i[i].logical_page == (LOOP_ADDR & ~0xfffu))
            cpu_g->atc060_i[i].page_shift = 13;
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
static void op_cinva_then_flush(void) {
    exec_at_test_pc(OP_CINVA_I);
    exec_at_test_pc(OP_PFLUSHA);
}

typedef struct {
    const char *name;
    void (*op1)(void);
    void (*op2)(void);
    uint32_t after1; /* 1 つ目の操作後の D0・D1。0 は検証しない（等価性だけを見る） */
    uint32_t after2;
} scenario_t;

static const scenario_t scenarios[] = {
    {"no perturbation", op_none, op_none, K_OLD, K_OLD},
    {"memory changed, no CINV (stale line stays)", op_memory_changed_no_cinv, op_none, K_OLD,
     K_OLD},
    {"memory changed, CINVL I", op_memory_changed_cinvl, op_none, K_NEW, K_NEW},
    {"memory changed, CINVP I", op_memory_changed_cinvp, op_none, K_NEW, K_NEW},
    {"memory changed, CINVA I", op_memory_changed_cinva, op_none, K_NEW, K_NEW},
    {"CACR.IE off (memory read) then on (stale line)", op_cacr_ie_off, op_cacr_ie_on, K_NEW,
     K_OLD},
    /* 記憶したラインが外れたとき、高速経路は同じセットを探す。重複ラインの選び方は通常経路と同じ
     */
    {"same set: duplicate line A, last points at line B", op_duplicate_line_a_last_at_b,
     op_none, K_OLD, K_OLD},
    {"same set: duplicate line A, first way not loaded", op_duplicate_line_a_first_unloaded,
     op_none, K_OLD, K_OLD},
    {"line A invalidated", op_line_a_invalid, op_none, 0, 0},
    {"line A unloaded", op_line_a_unloaded, op_none, 0, 0},
    {"line A replaced by another physical line", op_line_a_replaced, op_none, 0, 0},
    {"line B invalidated", op_line_b_invalid, op_none, 0, 0},
    {"line B replaced by another physical line", op_line_b_replaced, op_none, 0, 0},
    {"hint points to another line (stale line stays)", op_hint_points_elsewhere, op_none, K_OLD,
     K_OLD},
    /* 命令の noncachable アクセスは一致ラインを無効化しない（無効化は D-cache のみ） */
    {"noncachable entry then cachable again (stale line stays)", op_nocache_entry,
     op_restore_cache_entry, K_NEW, K_OLD},
    {"descriptor noncachable without PFLUSH (ATC kept)", op_nocache_entry_atc_kept, op_none,
     K_OLD, K_OLD},
    {"ATC entry made noncachable in place", op_entry_noncachable_in_place, op_none, K_NEW,
     K_NEW},
    {"PFLUSHA + remap", op_pflusha_remap, op_none, K_NEW, K_NEW},
    {"ATC entry invalidated", op_entry_invalidate, op_none, K_NEW, K_NEW},
    {"ATC entry physical overwritten in place", op_entry_overwrite_physical, op_none, K_NEW,
     K_NEW},
    {"jump to another logical page", op_jump_page2, op_none, K_NEW, K_NEW},
    {"ATC entry logical page overwritten", op_overwrite_logical, op_none, 0, 0},
    {"ATC entry fc overwritten", op_overwrite_fc, op_none, 0, 0},
    {"ATC entry page_shift overwritten", op_overwrite_page_shift, op_none, 0, 0},
    {"TC.P 4KB -> 8KB", op_tc_page8k, op_none, K_8K, K_8K},
    {"TC.P 4KB -> 8KB without flush", op_tc_page8k_noflush, op_none, 0, 0},
    {"ITT1 region TT", op_itt1_region, op_none, K_TT, K_TT},
    {"ITT0 replaced by region TT", op_itt0_region, op_none, K_TT, K_TT},
    {"supervisor -> user -> supervisor", op_to_user, op_to_super, K_USER, K_OLD},
    {"CINVA then PFLUSHA", op_cinva_then_flush, op_none, K_OLD, K_OLD},
};

static void run_scenario(const scenario_t *s, bool disable_fast, step_t *tr, unsigned *n) {
    *n = 0;
    init_cpu(disable_fast);
    run_steps(tr, n, PHASE_STEPS);
    assert(tr[*n - 1].d0 == K_OLD && tr[*n - 1].d1 == K_OLD);
    (void)aim_hint_at_line(0); /* 操作直後の最初のフェッチを高速経路に当てる */
    s->op1();
    go_loop();
    run_steps(tr, n, PHASE_STEPS);
    (void)aim_hint_at_line(0);
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
        if (fast[i].pc != slow[i].pc || fast[i].d0 != slow[i].d0 || fast[i].d1 != slow[i].d1 ||
            fast[i].clock != slow[i].clock || fast[i].cycles != slow[i].cycles ||
            fast[i].vector != slow[i].vector || fast[i].atc_hash != slow[i].atc_hash ||
            fast[i].icache_hash != slow[i].icache_hash ||
            fast[i].fault_hash != slow[i].fault_hash) {
            fprintf(stderr, "mismatch: %s step %u\n", s->name, i);
            abort();
        }
    }
    /* 操作後に古い割付を使っていないこと（等価性だけでは slow 側の期待が崩れても通るため）。
     * 各フェーズの最後の命令後の D0・D1 を見る。 */
    const step_t *e1 = &slow[PHASE_STEPS * 2 - 1], *e2 = &slow[MAX_STEPS - 1];
    if (s->after1 != 0 && (e1->d0 != s->after1 || e1->d1 != s->after1)) {
        fprintf(stderr, "after op1: %s got D0=%u D1=%u want %u\n", s->name, e1->d0, e1->d1,
                s->after1);
        abort();
    }
    if (s->after2 != 0 && (e2->d0 != s->after2 || e2->d1 != s->after2)) {
        fprintf(stderr, "after op2: %s got D0=%u D1=%u want %u\n", s->name, e2->d0, e2->d1,
                s->after2);
        abort();
    }
}

/* 高速経路が実際に働くこと。ループを回した後、MOVEQ #k,D0 を含むライン A と同じセットに「同じ
 * 物理ラインで内容の違う」重複ラインを作り、記憶位置を書き換えた方へ向ける。通常経路は添字の
 * 小さい方（元の内容）、高速経路は記憶した方（書き換えた内容）を読むので、D0 が食い違う。 */
static uint32_t run_with_duplicate_line(bool disable_fast) {
    step_t tr[MAX_STEPS];
    unsigned n = 0;
    init_cpu(disable_fast);
    run_steps(tr, &n, PHASE_STEPS);
    assert(tr[n - 1].d0 == K_OLD);
    const mc68k_040_cache_line_t *line = aim_hint_at_line(0);
    assert(line);
    const unsigned a = hinted_index();
    const unsigned set = a / MC68K_040_CACHE_WAYS, way = a % MC68K_040_CACHE_WAYS;
    const unsigned other = (way + 1u) % MC68K_040_CACHE_WAYS;
    const unsigned lo = way < other ? way : other, hi = way < other ? other : way;
    mc68k_040_cache_line_t orig = *line;
    mc68k_040_cache_line_t altered = orig;
    /* MOVEQ #k,D0 は物理 CODE_OFFSET + 2（ライン内 10 バイト目）の下位 16 ビット */
    const unsigned idx = (CODE_OFFSET + WORD_MOVEQ_D0 * 2u) / 4u;
    altered.data[idx] = (altered.data[idx] & 0xffff0000u) | (OP_MOVEQ_D0 | K_NEW);
    cpu_g->icache040[set][lo] = orig;
    cpu_g->icache040[set][hi] = altered;
    cpu_g->icache040_last = (uint16_t)(set * MC68K_040_CACHE_WAYS + hi);
    go_loop();
    run_steps(tr, &n, 2); /* NOP, MOVEQ #k,D0 */
    return tr[n - 1].d0;
}
static void test_fast_path_engaged(void) {
    assert(run_with_duplicate_line(true) == K_OLD);  /* 通常経路: 元のライン */
    assert(run_with_duplicate_line(false) == K_NEW); /* 高速経路: 記憶したライン */
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
    test_fast_path_engaged();
    for (unsigned i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++)
        test_scenario(&scenarios[i]);
    mc68k_destroy(cpu_g);
    return 0;
}
