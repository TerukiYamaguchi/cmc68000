/* Phase 2C 設計書 §6.3.4 の代表F軸ケースを実行する回帰テスト。 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include "memory/bus.h"

#define RAM_SIZE  0x10000u
#define RESET_PC  0x1000u
#define RESET_SSP 0x8000u

static void write_word(uint8_t *ram, uint32_t a, uint16_t v) {
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1] = (uint8_t)v;
}
static void write_long(uint8_t *ram, uint32_t a, uint32_t v) {
    write_word(ram, a, (uint16_t)(v >> 16));
    write_word(ram, a + 2, (uint16_t)v);
}

typedef struct {
    uint16_t w0, w1, sr, expected_sr, sr_mask;
    uint32_t d0, d1, a0, expected_d0, expected_d1, expected_a0;
} phase2c_case_t;

static void run_case(const phase2c_case_t *c) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    write_long(ram, 0, RESET_SSP);
    write_long(ram, 4, RESET_PC);
    write_word(ram, RESET_PC, c->w0);
    write_word(ram, RESET_PC + 2, c->w1);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create(&bus);
    mc68k_reset(cpu);
    cpu->d[0] = c->d0;
    cpu->d[1] = c->d1;
    cpu->a[0] = c->a0;
    cpu->sr = c->sr;
    mc68k_execute(cpu, 1);
    assert(cpu->d[0] == c->expected_d0);
    assert(cpu->d[1] == c->expected_d1);
    assert(cpu->a[0] == c->expected_a0);
    assert((cpu->sr & c->sr_mask) == c->expected_sr);
    /* 拡張語を持つ命令（w1 != 0）は PC が4進み、それ以外は2進む（仕様 §6.3.4） */
    assert(cpu->pc == RESET_PC + (c->w1 ? 4u : 2u));
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
}

/* UM20 §8.2 Cache Case の独立転記。ここは implementation の timing
 * classes を参照せず、実 opcode が返す値だけを検査する。 */
static void run_68020_cycle(uint16_t w0, uint16_t w1, uint32_t d0, uint32_t a0, int expected) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    write_long(ram, 0, RESET_SSP);
    write_long(ram, 4, RESET_PC);
    write_word(ram, RESET_PC, w0);
    write_word(ram, RESET_PC + 2, w1);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->d[0] = d0;
    cpu->a[0] = a0;
    assert(mc68k_execute(cpu, 1) == expected);
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
}

static void test_um20_cache_case_specials(void) {
    /* BFTST Dn and STOP remain representative functional regressions here.
     * MOVEQ, EXG and MOVEP Cache Case cycle rows live exclusively in
     * test_cycle_phase2c.c's independent manifests. */
    run_68020_cycle(0xE8C0, 0x0001, 0x80000000u, 0, 6);
    run_68020_cycle(0x4E72, 0x2700, 0, 0, 8);
}

static void test_um20_bitfield_all_operations(void) {
    static const int dn[8] = {6, 8, 12, 8, 12, 18, 12, 10};
    static const int mem4[8] = {11, 13, 16, 13, 16, 24, 16, 14};
    static const int mem5[8] = {15, 18, 24, 18, 24, 32, 24, 20};
    for (int kind = 0; kind < 8; ++kind) {
        uint16_t op = (uint16_t)(0xE8C0 | (kind << 8));
        run_68020_cycle(op, 0x0001, 0x80000000u, 0, dn[kind]);
        run_68020_cycle((uint16_t)(op | 0x10), 0x0001, 0, 0x2000, mem4[kind] + 2);
        run_68020_cycle((uint16_t)(op | 0x10), 0x0040, 0, 0x2000, mem5[kind] + 2);
    }
}

/* 仕様 §6.3.4 の表のうち、D0/D1/A0 だけでは書けないケース（A1・メモリ・PC を比較する）。
 * 初期 SR は $0010（X のみ）、比較 mask は $F71F。期待値は仕様の表から転記する。 */
typedef struct {
    const char *id;
    uint16_t w0, w1;
    uint32_t d0, d1, a0, a1;
    uint16_t sr;
    uint8_t mem200, mem300;
    uint32_t e_d0, e_d1, e_a0, e_a1;
    uint32_t e_pc;
    uint16_t e_sr;
    uint16_t sr_mask; /* 0xF71F が既定。未比較のフラグは含めない */
    uint8_t e_mem200, e_mem300;
} spec_case_t;

#define SPEC_SR_MASK 0xF71Fu
#define SPEC_SR_INIT 0x0010u

static void run_spec_case(const spec_case_t *c) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    write_long(ram, 0, RESET_SSP);
    write_long(ram, 4, RESET_PC);
    write_word(ram, RESET_PC, c->w0);
    write_word(ram, RESET_PC + 2, c->w1);
    ram[0x200] = c->mem200;
    ram[0x300] = c->mem300;
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create(&bus);
    mc68k_reset(cpu);
    cpu->d[0] = c->d0;
    cpu->d[1] = c->d1;
    cpu->a[0] = c->a0;
    cpu->a[1] = c->a1;
    cpu->sr = c->sr ? c->sr : SPEC_SR_INIT;
    mc68k_execute(cpu, 1);
    const uint16_t mask = c->sr_mask ? c->sr_mask : SPEC_SR_MASK;
    bool ok = cpu->d[0] == c->e_d0 && cpu->d[1] == c->e_d1 && cpu->a[0] == c->e_a0 &&
              cpu->a[1] == c->e_a1 && cpu->pc == c->e_pc && (cpu->sr & mask) == (c->e_sr & mask) &&
              ram[0x200] == c->e_mem200 && ram[0x300] == c->e_mem300;
    if (!ok) {
        fprintf(stderr,
                "%s: d0=%08X d1=%08X a0=%08X a1=%08X pc=%04X sr=%04X m200=%02X m300=%02X\n",
                c->id, cpu->d[0], cpu->d[1], cpu->a[0], cpu->a[1], cpu->pc, cpu->sr & mask,
                ram[0x200], ram[0x300]);
        abort();
    }
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
}

static void test_spec_634_remaining_cases(void) {
    static const spec_case_t cases[] = {
        {"T-INS-026-01", 0xD081, 0, 0x7fffffff, 1, 0, 0, 0, 0, 0, 0x80000000, 1, 0, 0, 0x1002, 0x000A, 0, 0, 0},
        {"T-INS-027-02", 0x5048, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0x18, 0, 0x1002, 0x0010, 0, 0, 0},
        {"T-INS-028-01", 0x9081, 0, 0x80000000, 1, 0, 0, 0, 0, 0, 0x7fffffff, 1, 0, 0, 0x1002, 0x0002, 0, 0, 0},
        {"T-INS-030-01", 0xB041, 0, 0x00010000, 1, 0, 0, 0x0010, 0, 0, 0x00010000, 1, 0, 0, 0x1002, 0x0019, 0, 0, 0},
        {"T-INS-030-02", 0xB308, 0, 0, 0, 0x200, 0x300, 0, 2, 3, 0, 0, 0x201, 0x301, 0x1002, 0x0010, 0, 2, 3},
        {"T-INS-031-01", 0xC0C1, 0, 3, 4, 0, 0, 0, 0, 0, 0xC, 4, 0, 0, 0x1002, 0x0010, 0, 0, 0},
        {"T-INS-031-03", 0x80C1, 0, 0xFFFFFFFF, 1, 0, 0, 0, 0, 0, 0xFFFFFFFF, 1, 0, 0, 0x1002, 0x0012, 0xF717, 0, 0},
        {"T-INS-031-02", 0x80C1, 0, 0x00010000, 2, 0, 0, 0, 0, 0, 0x8000, 2, 0, 0, 0x1002, 0x0018, 0, 0, 0},
        {"T-INS-032-01", 0xC001, 0, 0xF0, 0x0F, 0, 0, 0, 0, 0, 0, 0x0F, 0, 0, 0x1002, 0x0014, 0, 0, 0},
        {"T-INS-032-02", 0xB101, 0, 0xAA, 0xFF, 0, 0, 0, 0, 0, 0xAA, 0x55, 0, 0, 0x1002, 0x0010, 0, 0, 0},
        {"T-INS-033-02", 0x4000, 0, 0, 0, 0, 0, 0x0014, 0, 0, 0xFF, 0, 0, 0, 0x1002, 0x0019, 0, 0, 0},
        {"T-INS-033-03", 0x4240, 0, 0x12345678, 0, 0, 0, 0x0010, 0, 0, 0x12340000, 0, 0, 0, 0x1002, 0x0014, 0, 0, 0},
        {"T-INS-034-01", 0x0100, 0, 0x20, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0x1002, 0x0014, 0, 0, 0},
        {"T-INS-034-02", 0x08C0, 0x0005, 0x20, 0, 0, 0, 0x0010, 0, 0, 0x20, 0, 0, 0, 0x1004, 0x0010, 0, 0, 0},
        {"T-INS-034-03", 0x03D0, 0, 0, 9, 0x200, 0, 0, 0, 0, 0, 9, 0x200, 0, 0x1002, 0x0014, 0, 2, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        run_spec_case(&cases[i]);
}

int main(void) {
    static const phase2c_case_t cases[] = {
        {0xD0C0, 0, 0x0010, 0x0010, 0x001f, 0xffff8000, 0, 0x1000, 0xffff8000, 0, 0xffff9000},
        {0xD101, 0, 0x0014, 0x0015, 0x001f, 0xffffffff, 0, 0, 0xffffff00, 0, 0},
        {0x9100, 0, 0x0014, 0x0019, 0x001f, 0, 0, 0, 0x000000ff, 0, 0},
        {0x0640, 1, 0x0010, 0x0015, 0x001f, 0x1234ffff, 0, 0, 0x12340000, 0, 0},
        {0x0440, 1, 0x0010, 0x0019, 0x001f, 0x12340000, 0, 0, 0x1234ffff, 0, 0},
        {0xC1C1, 0, 0x0010, 0x0018, 0x001f, 0x0000fffe, 3, 0, 0xfffffffa, 3, 0},
        {0x81C1, 0, 0x0010, 0x0010, 0x001f, 0xfffffff6, 0x0000fffd, 0, 0xffff0003, 0x0000fffd,
         0},
        {0x0040, 0x00f0, 0x0010, 0x0010, 0x001f, 0x12340000, 0, 0, 0x123400f0, 0, 0},
        {0x0200, 0x000f, 0x0010, 0x0014, 0x001f, 0x000000f0, 0, 0, 0, 0, 0},
        {0x0A00, 0x00ff, 0x0010, 0x0018, 0x001f, 0x55, 0, 0, 0xaa, 0, 0},
        {0x8001, 0, 0x0010, 0x0018, 0x001f, 0x80, 1, 0, 0x81, 1, 0},
        {0xC300, 0, 0x0014, 0x0015, 0x0015, 0x99, 0, 0, 0x99, 0, 0},
        {0x8101, 0, 0x0014, 0x0000, 0x0015, 0x23, 9, 0, 0x13, 9, 0},
        {0x4800, 0, 0x0000, 0x0011, 0x0015, 7, 0, 0, 0x93, 0, 0},
        {0x4400, 0, 0x0010, 0x0019, 0x001f, 0x12340001, 0, 0, 0x123400ff, 0, 0},
        {0x4600, 0, 0x0010, 0x0010, 0x001f, 0x123400f0, 0, 0, 0x1234000f, 0, 0},
        {0x4A00, 0, 0x0010, 0x0018, 0x001f, 0x12340080, 0, 0, 0x12340080, 0, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        run_case(&cases[i]);
    test_spec_634_remaining_cases();
    test_um20_cache_case_specials();
    test_um20_bitfield_all_operations();
    return 0;
}
