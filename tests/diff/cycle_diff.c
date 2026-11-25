/*
 * cycle_diff.c — サイクル数差分テスト：自実装 vs Musashi 参照実装
 * ( Phase 1b、現行仕様 Phase 1b・§9.2)
 *
 * diff_test.c はレジスタ・CCRの一致を検証するが、サイクル数は比較しない。
 * 本ファイルは mc68k_execute(cpu, 1) / m68k_execute(1)
 * （どちらも「最低1命令は実行する」シングルステップ実行）で1命令だけ実行し、
 * 消費サイクル数を比較する。
 *
 * 方針（design書§10 Phase 1b）:
 *   - 68000モデルでの比較のみブロッキング（不変性回帰）。
 *   - 68010モデルでの比較は記録するだけで判定に使わない
 *     （Musashiの68010サイクル値は[DS10]と食い違う既知の問題があるため、§9.2）。
 *   - 対象命令は世代間で議論の余地がない基本命令に絞る（NOP・MOVEQ・DBcc・ADDQ
 *     レジスタ直接）。Phase 1a/1bで是正した微妙な差分（ADD.L EA=Dn直接の
 *     基本時間バンプ等）はここでは扱わない。一次資料（[UM00]/[DS10]）に基づく
 *     是正はnotes/phase1_68010_diff_survey.mdの調査で既に検証済みであり、
 *     Musashiとの一致・不一致それ自体を正誤の根拠にしない。
 *
 * M68K_EMULATE_010=M68K_OPT_ON でビルドした musashi_010 とリンクする
 * （既存 musashi ターゲットは OFF 固定のため diff_test.c 用に温存し、
 * 本ファイル専用の別ターゲットにする）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Musashi */
#include "m68k.h"

/* 自実装 */
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "memory/memory.h"
#include "memory/bus.h"

#define RAM_SIZE  0x10000
#define RESET_SSP 0x00008000u
#define RESET_PC  0x00001000u

static uint8_t g_prog[RAM_SIZE];
static int g_prog_len;

static void prog_init(void) {
    memset(g_prog, 0, RAM_SIZE);
    g_prog[0] = 0x00;
    g_prog[1] = 0x00;
    g_prog[2] = 0x80;
    g_prog[3] = 0x00;
    g_prog[4] = 0x00;
    g_prog[5] = 0x00;
    g_prog[6] = 0x10;
    g_prog[7] = 0x00;
    g_prog_len = 0;
}

static void emit_w(uint16_t v) {
    g_prog[RESET_PC + g_prog_len++] = (uint8_t)(v >> 8);
    g_prog[RESET_PC + g_prog_len++] = (uint8_t)v;
}

/* ================================================================== */
/* Musashi メモリコールバック（g_ram を使用、diff_test.c と同じ流儀）      */
/* ================================================================== */

static uint8_t g_ram[RAM_SIZE];

unsigned int m68k_read_memory_8(unsigned int a) {
    return a < RAM_SIZE ? g_ram[a] : 0xFFu;
}
unsigned int m68k_read_memory_16(unsigned int a) {
    if (a + 1u < RAM_SIZE)
        return ((unsigned)g_ram[a] << 8) | g_ram[a + 1];
    return 0xFFFFu;
}
unsigned int m68k_read_memory_32(unsigned int a) {
    if (a + 3u < RAM_SIZE)
        return ((unsigned)g_ram[a] << 24) | ((unsigned)g_ram[a + 1] << 16) |
               ((unsigned)g_ram[a + 2] << 8) | (unsigned)g_ram[a + 3];
    return 0xFFFFFFFFu;
}
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    if (a < RAM_SIZE)
        g_ram[a] = (uint8_t)v;
}
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    if (a + 1u < RAM_SIZE) {
        g_ram[a] = (uint8_t)(v >> 8);
        g_ram[a + 1] = (uint8_t)v;
    }
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    if (a + 3u < RAM_SIZE) {
        g_ram[a] = (uint8_t)(v >> 24);
        g_ram[a + 1] = (uint8_t)(v >> 16);
        g_ram[a + 2] = (uint8_t)(v >> 8);
        g_ram[a + 3] = (uint8_t)v;
    }
}
/* m68kmake が生成するディスアセンブラ経路のシンボル解決用（実行時未使用）。 */
unsigned int m68k_read_disassembler_8(unsigned int a) {
    return m68k_read_memory_8(a);
}
unsigned int m68k_read_disassembler_16(unsigned int a) {
    return m68k_read_memory_16(a);
}
unsigned int m68k_read_disassembler_32(unsigned int a) {
    return m68k_read_memory_32(a);
}

/* ================================================================== */
/* 1命令だけ実行してサイクル数を得る                                     */
/* ================================================================== */

/* skip 回だけ空実行してから、測定対象の1命令だけを実行しコストを返す。
 * DBccケースは「セットアップ命令(MOVE等) → 測定対象命令」の2命令構成なので
 * skip=1 で測定対象だけを取り出す。 */
static int run_ours_n(mc68k_model_t model, int skip) {
    uint8_t *ram = (uint8_t *)malloc(RAM_SIZE);
    memcpy(ram, g_prog, RAM_SIZE);

    mem_ctx_t *mem = mem_create();
    mem_region_t *r = (mem_region_t *)calloc(1, sizeof *r);
    r->data = ram;
    r->base = 0;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);

    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    mc68k_reset(cpu);
    for (int i = 0; i < skip; i++)
        mc68k_execute(cpu, 1);
    int consumed = mc68k_execute(cpu, 1);
    /* テストなのでリークを許容（diff_test.cと同じ流儀） */
    return consumed;
}

/* Musashiのm68k_execute()は、reset直後の最初の呼び出しでRESET_CYCLES
 * （固定40）を消費するだけで命令を1つも実行せずに返す仕様
 * （m68kcpu.c "eat up any reset cycles"）。そのため空撃ちで1回消費してから
 * skip回スキップし、測定対象の1命令を実行する。 */
static int run_musashi_n(unsigned int cpu_type, int skip) {
    memcpy(g_ram, g_prog, RAM_SIZE);
    m68k_init();
    m68k_set_cpu_type(cpu_type);
    m68k_pulse_reset();
    m68k_execute(1); /* RESET_CYCLESの消費のみ。命令は実行されない */
    for (int i = 0; i < skip; i++)
        m68k_execute(1);
    return m68k_execute(1);
}

/* ================================================================== */
/* テストケース                                                         */
/* ================================================================== */

static int g_pass = 0, g_fail = 0;
static int g_record = 0;

/* ブロッキング（68000のみ）: 一次資料・世代間で議論の余地が無い基本命令 */
static void check_68000(const char *name, void (*setup)(void), int skip) {
    setup();
    int ours = run_ours_n(MC68K_MODEL_68000, skip);
    int mus = run_musashi_n(M68K_CPU_TYPE_68000, skip);
    if (ours != mus) {
        printf("FAIL  [68000] %-28s ours=%d musashi=%d\n", name, ours, mus);
        g_fail++;
    } else {
        printf("pass  [68000] %-28s ours=musashi=%d\n", name, ours);
        g_pass++;
    }
}

/* 記録のみ（68010）: Musashiの68010サイクル値は[DS10]と食い違う既知の問題が
 * あるため、一致しなくても失敗にしない（design書§9.2）。 */
static void record_68010(const char *name, void (*setup)(void), int skip) {
    setup();
    int ours = run_ours_n(MC68K_MODEL_68010, skip);
    int mus = run_musashi_n(M68K_CPU_TYPE_68010, skip);
    printf("%s [68010] %-28s ours=%d musashi=%d\n", (ours == mus) ? "==== " : "diff ", name,
           ours, mus);
    g_record++;
}

static void setup_nop(void) {
    prog_init();
    emit_w(0x4E71); /* NOP */
}
static void setup_moveq(void) {
    prog_init();
    emit_w(0x7005); /* MOVEQ #5,D0 */
}
static void setup_addq_w_dn(void) {
    prog_init();
    emit_w(0x5240); /* ADDQ.W #1,D0 */
}
static void setup_dbra_branch(void) {
    prog_init();
    emit_w(0x303C); /* MOVE.W #1,D0 */
    emit_w(0x0001);
    emit_w(0x51C8); /* DBRA D0,$ (自分自身へ分岐し続ける最初の1回=分岐成立) */
    emit_w((uint16_t)-2);
}
static void setup_dbra_expired(void) {
    prog_init();
    emit_w(0x303C); /* MOVE.W #0,D0 */
    emit_w(0x0000);
    emit_w(0x51C8); /* DBRA D0,$ (D0=0→-1でカウント終了) */
    emit_w((uint16_t)-2);
}
static void setup_dbeq_cc_true(void) {
    prog_init();
    emit_w(0x44FC); /* MOVE #imm5,CCR: Z=1 */
    emit_w(0x0004);
    emit_w(0x57C8); /* DBEQ D0,$ */
    emit_w((uint16_t)-2);
}

int main(void) {
    /* --- ブロッキング: 68000不変性回帰 --- */
    check_68000("NOP", setup_nop, 0);
    check_68000("MOVEQ #5,D0", setup_moveq, 0);
    check_68000("ADDQ.W #1,D0", setup_addq_w_dn, 0);
    check_68000("DBRA branch", setup_dbra_branch, 1);
    check_68000("DBRA expired", setup_dbra_expired, 1);
    check_68000("DBEQ cc_true", setup_dbeq_cc_true, 1);

    printf("---\n");

    /* --- 記録のみ: 68010（Musashiの68010値は[DS10]と食い違う既知の問題あり） --- */
    record_68010("NOP", setup_nop, 0);
    record_68010("MOVEQ #5,D0", setup_moveq, 0);
    record_68010("ADDQ.W #1,D0", setup_addq_w_dn, 0);
    record_68010("DBRA branch", setup_dbra_branch, 1);
    record_68010("DBRA expired", setup_dbra_expired, 1);
    record_68010("DBEQ cc_true", setup_dbeq_cc_true, 1);

    printf("---\n%d/%d ブロッキングpass、%d件を68010で記録（判定に使わない）\n", g_pass,
           g_pass + g_fail, g_record);
    return g_fail ? 1 : 0;
}
