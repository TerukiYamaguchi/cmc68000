/*
 * musashi_020_record.c — Phase 2c の 68020 列を Musashi の 68020 と突き合わせて記録する
 * ( 作業計画 5、現行仕様。
 *
 * test_cycle_phase2c_models.c の行表（実 opcode・初期状態）をそのまま使い、同じ RAM 像で
 * 自実装の 68020 と Musashi の 68020 に 1 命令ずつ実行させ、cycle・最終 PC・最終 A7 を
 * CSV で出力する。差分は「記録のみ」で、判定には使わない: 一次資料は UM20 であり、
 * Musashi は二次資料である（Musashi の 030 列は 020 列の複製、§3.3）。終了コードは常に 0。
 *
 * 使い方: musashi_020_record > local/investigation/musashi_020_cycle_diff.csv
 */
#define main p2c_models_main
#include "../cpu/test_cycle_phase2c_models.c"
#undef main

#include "m68k.h"

static uint8_t g_ram[RAM_SIZE];

unsigned int m68k_read_memory_8(unsigned int a) {
    return a < RAM_SIZE ? g_ram[a] : 0xFFu;
}
unsigned int m68k_read_memory_16(unsigned int a) {
    return a + 1u < RAM_SIZE ? ((unsigned)g_ram[a] << 8) | g_ram[a + 1] : 0xFFFFu;
}
unsigned int m68k_read_memory_32(unsigned int a) {
    return a + 3u < RAM_SIZE ? ((unsigned)g_ram[a] << 24) | ((unsigned)g_ram[a + 1] << 16) |
                                   ((unsigned)g_ram[a + 2] << 8) | g_ram[a + 3]
                             : 0xFFFFFFFFu;
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
unsigned int m68k_read_disassembler_8(unsigned int a) {
    return m68k_read_memory_8(a);
}
unsigned int m68k_read_disassembler_16(unsigned int a) {
    return m68k_read_memory_16(a);
}
unsigned int m68k_read_disassembler_32(unsigned int a) {
    return m68k_read_memory_32(a);
}

static p2c_snap_t musashi_run(const p2c_row_t *row) {
    memset(g_ram, 0, sizeof g_ram);
    p2c_build_ram(row, g_ram);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68020);
    m68k_pulse_reset();
    m68k_execute(1); /* the first call after reset only eats RESET_CYCLES (cycle_diff.c) */
    m68k_set_reg(M68K_REG_SR, row->sr);
    m68k_set_reg(M68K_REG_D0, row->d0);
    m68k_set_reg(M68K_REG_D1, INDEX_REG_VALUE);
    m68k_set_reg(M68K_REG_D2, D2_VALUE);
    m68k_set_reg(M68K_REG_D3, row->d3);
    m68k_set_reg(M68K_REG_D4, D4_VALUE);
    m68k_set_reg(M68K_REG_A0, A0_VALUE);
    m68k_set_reg(M68K_REG_A1, A1_VALUE);
    m68k_set_reg(M68K_REG_A2, A2_VALUE);
    m68k_set_reg(M68K_REG_A3, A3_VALUE);
    m68k_set_reg(M68K_REG_A6, A6_VALUE);
    m68k_set_reg(M68K_REG_A7, row->a7);
    m68k_set_reg(M68K_REG_MSP, MSP_VALUE);
    p2c_snap_t got;
    got.cycles = (int16_t)m68k_execute(1);
    got.pc = m68k_get_reg(NULL, M68K_REG_PC);
    got.a7 = m68k_get_reg(NULL, M68K_REG_A7);
    return got;
}

int main(void) {
    printf("section,row,ours_cycles,musashi_cycles,cycles_equal,ours_pc,musashi_pc,ours_a7,"
           "musashi_a7,"
           "state_equal\n");
    for (size_t s = 0; s < sizeof(p2c_sections) / sizeof(p2c_sections[0]); ++s) {
        for (size_t i = 0; i < p2c_sections[s].actual_cases; ++i) {
            const p2c_row_t *row = &p2c_sections[s].rows[i];
            p2c_snap_t ours = p2c_run(row, MC68K_MODEL_68020);
            p2c_snap_t mus = musashi_run(row);
            printf("\"%s\",\"%s\",%d,%d,%d,%X,%X,%X,%X,%d\n", p2c_sections[s].title, row->row,
                   ours.cycles, mus.cycles, ours.cycles == mus.cycles, ours.pc, mus.pc, ours.a7,
                   mus.a7, ours.pc == mus.pc && ours.a7 == mus.a7);
        }
    }
    return 0;
}
