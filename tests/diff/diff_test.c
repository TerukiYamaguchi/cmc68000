/*
 * diff_test.c — MC68000 差分テスト：自実装 vs Musashi 参照実装
 *
 * テスト方式:
 *   1. g_prog[] に「セットアップ命令 + テスト命令 + BRA.S $」を書く
 *   2. 自実装・Musashi それぞれで同じバイト列を実行（RUN_CYCLES サイクル）
 *   3. 実行後のレジスタ（D0–D6）と CCR を比較する
 *
 * BRA.S $ でループさせることで ILLEGAL 例外処理を避け、
 * SR から直接 CCR を読む。
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

/* ================================================================== */
/* 定数                                                                 */
/* ================================================================== */

#define RAM_SIZE   0x10000
#define RESET_SSP  0x00008000u
#define RESET_PC   0x00001000u
#define RUN_CYCLES 2000

/* ================================================================== */
/* プログラムテンプレート                                               */
/* ================================================================== */

static uint8_t g_prog[RAM_SIZE];
static int g_prog_len;

static void prog_init(void) {
    memset(g_prog, 0, RAM_SIZE);
    /* リセットベクタ: SSP = 0x8000, PC = 0x1000 */
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

static void emit_b(uint8_t v) {
    g_prog[RESET_PC + g_prog_len++] = v;
}
static void emit_w(uint16_t v) {
    emit_b(v >> 8);
    emit_b(v & 0xFF);
}
static void emit_l(uint32_t v) {
    emit_w(v >> 16);
    emit_w(v & 0xFFFFu);
}

/* MOVE.L #imm, Dn */
static void emit_move_l_dn(int n, uint32_t v) {
    emit_w((uint16_t)(0x203C | (n << 9)));
    emit_l(v);
}

/* MOVE #imm5, CCR */
static void emit_move_ccr(uint8_t v) {
    emit_w(0x44FC);
    emit_w(v & 0x1Fu);
}

/* BRA.S $ — 無限ループ（halt として使用） */
static void emit_halt(void) {
    emit_w(0x60FE);
}

/* ================================================================== */
/* Musashi メモリコールバック（g_ram を使用）                           */
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

/*
 * m68k_disassemble_raw()（tests/diff/disasm_length_diff.c専用）が
 * musashi_wrapper.c経由でm68kdasm.cをリンクするため、Musashi側が要求する
 * ディスアセンブラ用フックのシンボル解決だけが必要（このテストでは実行時
 * に呼ばれない）。読み出しはm68k_read_memory_*と同じg_ramへ委譲する。
 */
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
/* スナップショット型と実行関数                                         */
/* ================================================================== */

typedef struct {
    uint32_t d[7]; /* D0–D6 */
    uint32_t a[7]; /* A0–A6 */
    uint8_t ccr;   /* SR の下位 5 ビット (XNZVC) */
} snap_t;

static snap_t run_ours(void) {
    uint8_t *ram = (uint8_t *)malloc(RAM_SIZE);
    memcpy(ram, g_prog, RAM_SIZE);

    mem_ctx_t *mem = mem_create();
    mem_region_t *r = (mem_region_t *)calloc(1, sizeof *r);
    r->data = ram;
    r->base = 0;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);

    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create(&bus);
    mc68k_reset(cpu);
    mc68k_execute(cpu, RUN_CYCLES);

    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);

    snap_t s;
    int i;
    for (i = 0; i < 7; i++)
        s.d[i] = regs.d[i];
    for (i = 0; i < 7; i++)
        s.a[i] = regs.a[i];
    s.ccr = (uint8_t)(regs.sr & 0x1Fu);

    /* テストなのでリークを許容 */
    return s;
}

static snap_t run_musashi(void) {
    memcpy(g_ram, g_prog, RAM_SIZE);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_execute(RUN_CYCLES);

    snap_t s;
    s.d[0] = m68k_get_reg(NULL, M68K_REG_D0);
    s.d[1] = m68k_get_reg(NULL, M68K_REG_D1);
    s.d[2] = m68k_get_reg(NULL, M68K_REG_D2);
    s.d[3] = m68k_get_reg(NULL, M68K_REG_D3);
    s.d[4] = m68k_get_reg(NULL, M68K_REG_D4);
    s.d[5] = m68k_get_reg(NULL, M68K_REG_D5);
    s.d[6] = m68k_get_reg(NULL, M68K_REG_D6);
    s.a[0] = m68k_get_reg(NULL, M68K_REG_A0);
    s.a[1] = m68k_get_reg(NULL, M68K_REG_A1);
    s.a[2] = m68k_get_reg(NULL, M68K_REG_A2);
    s.a[3] = m68k_get_reg(NULL, M68K_REG_A3);
    s.a[4] = m68k_get_reg(NULL, M68K_REG_A4);
    s.a[5] = m68k_get_reg(NULL, M68K_REG_A5);
    s.a[6] = m68k_get_reg(NULL, M68K_REG_A6);
    uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
    s.ccr = (uint8_t)(sr & 0x1Fu);
    return s;
}

/* ================================================================== */
/* 比較・レポート                                                       */
/* ================================================================== */

static int g_pass = 0, g_fail = 0;

static void check(const char *name, const snap_t *o, const snap_t *m) {
    int fail = 0, i;
    for (i = 0; i < 7; i++) {
        if (o->d[i] != m->d[i]) {
            printf("  D%d: ours=0x%08X musashi=0x%08X\n", i, o->d[i], m->d[i]);
            fail = 1;
        }
    }
    if ((o->ccr & 0x1Fu) != (m->ccr & 0x1Fu)) {
        printf("  CCR: ours=%02X musashi=%02X"
               "  (XNZVC ours=%c%c%c%c%c musashi=%c%c%c%c%c)\n",
               o->ccr & 0x1Fu, m->ccr & 0x1Fu, (o->ccr & 0x10u) ? 'X' : '-',
               (o->ccr & 0x08u) ? 'N' : '-', (o->ccr & 0x04u) ? 'Z' : '-',
               (o->ccr & 0x02u) ? 'V' : '-', (o->ccr & 0x01u) ? 'C' : '-',
               (m->ccr & 0x10u) ? 'X' : '-', (m->ccr & 0x08u) ? 'N' : '-',
               (m->ccr & 0x04u) ? 'Z' : '-', (m->ccr & 0x02u) ? 'V' : '-',
               (m->ccr & 0x01u) ? 'C' : '-');
        fail = 1;
    }
    if (fail) {
        printf("FAIL  %s\n", name);
        g_fail++;
    } else {
        printf("pass  %s\n", name);
        g_pass++;
    }
}

/* --- テストヘルパーマクロ ---
 * セットアップ（D0/D1 + CCR）→ 2 バイト命令 → halt → 実行 → 比較 */
#define T2(name, d0_, d1_, ccr_, insn)      \
    do {                                    \
        prog_init();                        \
        emit_move_l_dn(0, (uint32_t)(d0_)); \
        emit_move_l_dn(1, (uint32_t)(d1_)); \
        emit_move_ccr((uint8_t)(ccr_));     \
        emit_w((uint16_t)(insn));           \
        emit_halt();                        \
        snap_t _o = run_ours();             \
        snap_t _m = run_musashi();          \
        check((name), &_o, &_m);            \
    } while (0)

/* ================================================================== */
/* テストケース                                                         */
/* ================================================================== */

int main(void) {
    /* ---- ADD.L D1, D0 = 0xD081 ---- */
    T2("ADD.L overflow  (+max+1)", 0x7FFFFFFFu, 0x00000001u, 0, 0xD081u);
    T2("ADD.L carry+zero", 0xFFFFFFFFu, 0x00000001u, 0, 0xD081u);
    T2("ADD.L neg+neg overflow", 0x80000000u, 0x80000000u, 0, 0xD081u);
    T2("ADD.L normal", 0x12345678u, 0x11111111u, 0, 0xD081u);

    /* ---- SUB.L D1, D0 = 0x9081 ---- */
    T2("SUB.L overflow  (-min-1)", 0x80000000u, 0x00000001u, 0, 0x9081u);
    T2("SUB.L borrow    (0-1)", 0x00000000u, 0x00000001u, 0, 0x9081u);
    T2("SUB.L zero      (5-5)", 0x00000005u, 0x00000005u, 0, 0x9081u);

    /* ---- ADDX.L D1, D0 = 0xD181 (Z フラグ特殊動作) ---- */
    /* result=0, Z already set → Z stays 1 */
    T2("ADDX.L result=0 Z_stays", 0xFFFFFFFFu, 0x00000000u, 0x14u /*X=1,Z=1*/, 0xD181u);
    /* result!=0, Z already set → Z cleared */
    T2("ADDX.L result=1 Z_clears", 0x00000000u, 0x00000001u, 0x04u /*Z=1*/, 0xD181u);
    /* overflow with X */
    T2("ADDX.L overflow with X=1", 0x7FFFFFFEu, 0x00000001u, 0x10u /*X=1*/, 0xD181u);

    /* ---- SUBX.L D1, D0 = 0x9181 ---- */
    T2("SUBX.L borrow+X", 0x00000001u, 0x00000001u, 0x10u /*X=1*/, 0x9181u);

    /* ---- NEGX.L D0 = 0x4080 (Z フラグ特殊動作) ---- */
    /* 0 - 0 - 0 = 0, Z stays set */
    T2("NEGX.L D0=0 X=0 Z_stays", 0x00000000u, 0x00000000u, 0x04u /*Z=1*/, 0x4080u);
    /* 0 - 0 - 1 = -1, Z cleared */
    T2("NEGX.L D0=0 X=1", 0x00000000u, 0x00000000u, 0x14u /*X=1,Z=1*/, 0x4080u);
    /* overflow: NEGX 0x80000000 */
    T2("NEGX.L 0x80000000 X=0", 0x80000000u, 0x00000000u, 0, 0x4080u);

    /* ---- NEG.L D0 = 0x4480 ---- */
    T2("NEG.L overflow (0x80000000)", 0x80000000u, 0, 0, 0x4480u);
    T2("NEG.L zero", 0x00000000u, 0, 0, 0x4480u);

    /* ---- NOT.L D0 = 0x4680 ---- */
    T2("NOT.L 0", 0x00000000u, 0, 0, 0x4680u);
    T2("NOT.L 0xFFFFFFFF", 0xFFFFFFFFu, 0, 0, 0x4680u);

    /* ---- ASL.L #1, D0 = 0xE380 ---- */
    T2("ASL.L #1 overflow   (0x40000000)", 0x40000000u, 0, 0, 0xE380u);
    T2("ASL.L #1 carry+zero (0x80000000)", 0x80000000u, 0, 0, 0xE380u);
    T2("ASL.L #1 normal     (0x00000001)", 0x00000001u, 0, 0, 0xE380u);

    /* ---- ASR.L #1, D0 = 0xE280 ---- */
    T2("ASR.L #1 sign ext   (0x80000000)", 0x80000000u, 0, 0, 0xE280u);
    T2("ASR.L #1 carry      (0x00000001)", 0x00000001u, 0, 0, 0xE280u);

    /* ---- ROXL.L #1, D0 = 0xE390 ---- */
    T2("ROXL.L #1 X=0 (0x80000000)", 0x80000000u, 0, 0x00u /*X=0*/, 0xE390u);
    T2("ROXL.L #1 X=1 (0x00000000)", 0x00000000u, 0, 0x10u /*X=1*/, 0xE390u);

    /* ---- ROXR.L #1, D0 = 0xE290 ---- */
    T2("ROXR.L #1 X=0 (0x00000001)", 0x00000001u, 0, 0x00u, 0xE290u);
    T2("ROXR.L #1 X=1 (0x00000000)", 0x00000000u, 0, 0x10u /*X=1*/, 0xE290u);

    /* ---- MULU.W D1, D0 = 0xC0C1 ---- */
    T2("MULU.W 100*200", 0x00000064u /*100*/, 0x000000C8u /*200*/, 0, 0xC0C1u);
    T2("MULU.W 0*0xFFFF", 0x00000000u, 0x0000FFFFu, 0, 0xC0C1u);

    /* ---- MULS.W D1, D0 = 0xC1C1 ---- */
    T2("MULS.W 3*(-1)", 0x00000003u, 0x0000FFFFu /*-1*/, 0, 0xC1C1u);
    T2("MULS.W (-1)*(-1)", 0x0000FFFFu /*-1*/, 0x0000FFFFu /*-1*/, 0, 0xC1C1u);

    /* ---- DIVU.W D1, D0 = 0x80C1 ---- */
    T2("DIVU.W 100/7", 0x00000064u /*100*/, 0x00000007u, 0, 0x80C1u);
    /* overflow: quotient > 0xFFFF */
    T2("DIVU.W overflow", 0x00020000u, 0x00000001u, 0, 0x80C1u);

    /* ---- DIVS.W D1, D0 = 0x81C1 ---- */
    T2("DIVS.W 100/7", 0x00000064u /*100*/, 0x00000007u, 0, 0x81C1u);
    T2("DIVS.W -100/7", 0xFFFFFF9Cu /*-100*/, 0x00000007u, 0, 0x81C1u);

    /* ---- CMP.L D1, D0 = 0xB081 (D0 は変化しない) ---- */
    T2("CMP.L equal  (5==5)", 0x00000005u, 0x00000005u, 0, 0xB081u);
    T2("CMP.L D0>D1  (10>5)", 0x0000000Au, 0x00000005u, 0, 0xB081u);
    T2("CMP.L D0<D1  (5<10)", 0x00000005u, 0x0000000Au, 0, 0xB081u);
    T2("CMP.L signed (-1>0)?", 0xFFFFFFFFu, 0x00000001u, 0, 0xB081u);

    /* ---- 結果 ---- */
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
