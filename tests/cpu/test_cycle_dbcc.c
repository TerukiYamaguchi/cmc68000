/*
 * test_cycle_dbcc.c — DBcc のモデル別サイクル値を実命令で検証する
 * (, 現行仕様 T1-5〜T1-7)
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include "memory/bus.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE  0x10000
#define RESET_SSP 0x00008000u
#define RESET_PC  0x00001000u

static void write_word_be(uint8_t *ram, uint32_t addr, uint16_t val) {
    ram[addr + 0] = (uint8_t)(val >> 8);
    ram[addr + 1] = (uint8_t)(val & 0xFF);
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t val) {
    ram[addr + 0] = (uint8_t)(val >> 24);
    ram[addr + 1] = (uint8_t)(val >> 16);
    ram[addr + 2] = (uint8_t)(val >> 8);
    ram[addr + 3] = (uint8_t)(val >> 0);
}

static mc68k_state_t *make_cpu(uint8_t *ram, mc68k_model_t model) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = ram;
    r->base = 0x000000;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);
    mc68k_bus_t bus = bus_make_hooks(mem);
    return mc68k_create_model(&bus, model);
}

static const mc68k_model_t g_models[] = {
    MC68K_MODEL_68000, MC68K_MODEL_68010,   MC68K_MODEL_68020,
    MC68K_MODEL_68030, MC68K_MODEL_68EC030,
};
#define NUM_MODELS (sizeof(g_models) / sizeof(g_models[0]))

/* T1-5: 5モデル × DBcc の3経路すべてを実命令で検証する。1命令だけ実行して
 * mc68k_execute() の戻り値（=消費サイクル）を見る。ループ総和では見ない
 * （総和にすると3経路のどれが誤っていても打ち消し合う可能性が残るため）。 */

/* cc true 経路: MOVEQ #0,D0（Z=1にする）→ DBEQ D1,* */
static void test_t1_5_cc_true(mc68k_model_t model, int expect) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x7000); /* MOVEQ #0,D0 → Z=1 */
    p += 2;
    uint32_t dbeq_addr = p;
    write_word_be(ram, p, 0x57C9); /* DBEQ D1,disp（cond=7=EQ, reg=1） */
    p += 2;
    write_word_be(ram, p, (uint16_t)-2); /* disp: 分岐しても自己に戻るだけ（不実行） */

    mc68k_state_t *cpu = make_cpu(ram, model);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 1); /* MOVEQ */
    assert(cpu->pc == dbeq_addr);
    int consumed = mc68k_execute(cpu, 1); /* DBEQ */
    assert(consumed == expect);
    mc68k_destroy(cpu);
}

/* cc false 分岐成立経路: MOVEQ #5,D1 → DBRA D1,* */
static void test_t1_5_branch(mc68k_model_t model, int expect) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x7205); /* MOVEQ #5,D1 */
    p += 2;
    uint32_t dbra_addr = p;
    write_word_be(ram, p, 0x51C9); /* DBRA D1,disp（cond=1=F, reg=1） */
    p += 2;
    write_word_be(ram, p, (uint16_t)-2);

    mc68k_state_t *cpu = make_cpu(ram, model);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 1); /* MOVEQ */
    assert(cpu->pc == dbra_addr);
    int consumed = mc68k_execute(cpu, 1); /* DBRA: D1 5→4, 0xFFFF ではないので分岐 */
    assert(consumed == expect);
    assert((cpu->d[1] & 0xFFFF) == 4);
    mc68k_destroy(cpu);
}

/* count expired 経路: MOVEQ #0,D1 → DBRA D1,* （デクリメントで 0xFFFF になり抜ける） */
static void test_t1_5_expired(mc68k_model_t model, int expect) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    uint32_t p = RESET_PC;
    write_word_be(ram, p, 0x7200); /* MOVEQ #0,D1 */
    p += 2;
    uint32_t dbra_addr = p;
    write_word_be(ram, p, 0x51C9); /* DBRA D1,disp */
    p += 2;
    write_word_be(ram, p, (uint16_t)-2);

    mc68k_state_t *cpu = make_cpu(ram, model);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 1); /* MOVEQ */
    assert(cpu->pc == dbra_addr);
    int consumed = mc68k_execute(cpu, 1); /* DBRA: D1 0→0xFFFF, 抜ける */
    assert(consumed == expect);
    assert((cpu->d[1] & 0xFFFF) == 0xFFFF);
    mc68k_destroy(cpu);
}

/* [DS10 Table 7-18] / [UM20 §8.2.15] / [UM30 §11.6.15] の期待値（§8.2 の表）。
 * 外部裏付け: 68020/68030 の (6/6/10) は Musashi の CYC_DBCC_F_NOEXP/EXP 合成値と
 * 一致する（§3.3）。68010 は Musashi（12/12/18）が [DS10] と食い違うため
 * 裏付けに使わない（§9.2）。 */
static void test_t1_5(void) {
    struct {
        mc68k_model_t model;
        int cc_true, branch, expired;
    } cases[] = {
        {MC68K_MODEL_68000, 12, 10, 14}, {MC68K_MODEL_68010, 10, 10, 16},
        {MC68K_MODEL_68020, 6, 6, 10},   {MC68K_MODEL_68030, 6, 6, 10},
        {MC68K_MODEL_68EC030, 6, 6, 10},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        test_t1_5_cc_true(cases[i].model, cases[i].cc_true);
        test_t1_5_branch(cases[i].model, cases[i].branch);
        test_t1_5_expired(cases[i].model, cases[i].expired);
    }
}

/* T1-6: IPLROM 較正の再現。DBRA の自己ループ（cpu_clock 相当のサイクル予算 C
 * を与えたときの反復回数が C / 期待値 になること、±1反復）。
 * 較正ルーチンと同じ構造（分岐成立経路のみを回す自己ループ1命令）。 */
static void test_t1_6_iplrom_calibration_reproduction(void) {
    const int budget = 6000;
    const uint16_t initial_count = 60000; /* 予算内で expired しない十分な大きさ */

    for (size_t i = 0; i < NUM_MODELS; i++) {
        uint8_t ram[RAM_SIZE] = {0};
        write_long_be(ram, 0, RESET_SSP);
        write_long_be(ram, 4, RESET_PC);
        uint32_t p = RESET_PC;
        /* D1 に大きな値を即値ロングでセットする: MOVE.L #initial_count,D1 */
        write_word_be(ram, p, 0x223C); /* MOVE.L #imm,D1 */
        p += 2;
        write_long_be(ram, p, initial_count);
        p += 4;
        uint32_t loop_addr = p;
        write_word_be(ram, p, 0x51C9); /* DBRA D1,loop（自己ループ） */
        p += 2; /* p は disp fetch 前の disp_base（opcode_addr+2）と一致する */
        write_word_be(ram, p, (uint16_t)(int16_t)(loop_addr - p));

        mc68k_state_t *cpu = make_cpu(ram, g_models[i]);
        mc68k_reset(cpu);
        mc68k_execute(cpu, 1); /* MOVE.L 即値ロード */
        assert(cpu->pc == loop_addr);
        assert((cpu->d[1] & 0xFFFF) == initial_count);

        mc68k_execute(cpu, budget); /* 自己ループを予算いっぱい回す */

        int per_iter = cpu->cyc[CYC_DBCC_BRANCH];
        int expect_iters = budget / per_iter;
        int actual_iters = initial_count - (int)(cpu->d[1] & 0xFFFF);
        int diff = actual_iters - expect_iters;
        assert(diff >= -1 && diff <= 1);
        mc68k_destroy(cpu);
    }
}

/* T1-7: ホストが wait_cycles==0 を返す（＝ access_timing フック自体を
 * 登録しない）条件下で、CACR による I-cache 無効時の消費サイクルが有効時と
 * 同値であること（§3.4 の近似を仕様として固定する）。対象は 68030/68EC030
 * のみ。68020 以下は本コアがキャッシュを一切モデル化していない
 * （cpu_cache_read_eligible() が model_has_030_cache() で弾く）ため対象外。 */
static void test_t1_7_cache_on_off_same_when_no_wait(void) {
    mc68k_model_t models[] = {MC68K_MODEL_68030, MC68K_MODEL_68EC030};
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        for (int cache_on = 0; cache_on <= 1; cache_on++) {
            uint8_t ram[RAM_SIZE] = {0};
            write_long_be(ram, 0, RESET_SSP);
            write_long_be(ram, 4, RESET_PC);
            uint32_t p = RESET_PC;
            write_word_be(ram, p, 0x7205); /* MOVEQ #5,D1 */
            p += 2;
            uint32_t dbra_addr = p;
            write_word_be(ram, p, 0x51C9); /* DBRA D1,disp */
            p += 2;
            write_word_be(ram, p, (uint16_t)-2);

            /* access_timing フックは登録しない（bus_make_hooks() のバス。
             * cpu_account_bus_cycle() は access_timing==NULL で早期return
             * するので、これが wait_cycles==0 環境と同じ意味になる）。 */
            mc68k_state_t *cpu = make_cpu(ram, models[i]);
            mc68k_reset(cpu);
            /* CACR EI/ED を on/off で切り替える（本コアは MOVEC 経由でしか
             * 書けないため、テストの都合上 cpu->cacr を直接設定する）。 */
            cpu->cacr = cache_on ? (MC68K_CACR_EI | MC68K_CACR_ED | MC68K_CACR_WA) : 0u;

            mc68k_execute(cpu, 1); /* MOVEQ */
            assert(cpu->pc == dbra_addr);
            int consumed = mc68k_execute(cpu, 1); /* DBRA */

            /* CACR の値に関わらず同じ値（§3.4 の近似が仕様として固定される） */
            assert(consumed == cpu->cyc[CYC_DBCC_BRANCH]);
            mc68k_destroy(cpu);
        }
    }
}

int main(void) {
    test_t1_5();
    test_t1_6_iplrom_calibration_reproduction();
    test_t1_7_cache_on_off_same_when_no_wait();
    return 0;
}
