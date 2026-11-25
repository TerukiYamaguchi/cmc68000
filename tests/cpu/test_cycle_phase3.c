/*
 * test_cycle_phase3.c — UM30 §11.6 I-Cache Case の oracle。
 *
 * `tests/data/cpu/um30_11_6_icache_case.csv` は値をレビュー可能にする正本である。
 * Phase 3 の各節は該当する CSV 行を C literal として転記し、期待 cycle を cycles.c、
 * profile selector、ea_cycles() から生成してはならない。Stage 3-0 は production value を
 * 接続する前の完全な入力一覧を記録する。
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include "memory/bus.h"
#include "memory/memory.h"

#define P3_RAM_SIZE  0x10000u
#define P3_PC        0x1000u
#define P3_SSP       0x8000u
#define P3_NO_MEMORY UINT32_MAX

typedef struct {
    const char *section;
    size_t expected_rows;
} p3_input_section_t;

/* UM30 §11.6.1--§11.6.18, copied from the CSV input inventory.  This is a
 * manifest of source rows, not a cycle oracle for an opcode yet: 3a--3e add
 * opcode, machine state, expected cycle, and side-effect fields per row. */
static const p3_input_section_t p3_input_sections[] = {
    {"11.6.1", 41},  {"11.6.2", 75},  {"11.6.3", 38},  {"11.6.4", 74},  {"11.6.5", 35},
    {"11.6.6", 42},  {"11.6.7", 22},  {"11.6.8", 39},  {"11.6.9", 17},  {"11.6.10", 13},
    {"11.6.11", 16}, {"11.6.12", 16}, {"11.6.13", 16}, {"11.6.14", 24}, {"11.6.15", 7},
    {"11.6.16", 28}, {"11.6.17", 19}, {"11.6.18", 8},
};

enum { P3_EXPECTED_INPUT_ROWS = 530 };

static void test_input_inventory(void) {
    size_t rows = 0;
    for (size_t i = 0; i < sizeof(p3_input_sections) / sizeof(p3_input_sections[0]); i++) {
        assert(p3_input_sections[i].section != NULL);
        assert(p3_input_sections[i].expected_rows != 0);
        rows += p3_input_sections[i].expected_rows;
    }
    assert(rows == P3_EXPECTED_INPUT_ROWS);
}

static mc68k_state_t *make_cpu(void) {
    mc68k_bus_t bus;
    memset(&bus, 0, sizeof(bus));
    return mc68k_create_model(&bus, MC68K_MODEL_68030);
}

static void p3_write_word(uint8_t *ram, uint32_t address, uint16_t value) {
    ram[address] = (uint8_t)(value >> 8);
    ram[address + 1] = (uint8_t)value;
}

static void p3_write_long(uint8_t *ram, uint32_t address, uint32_t value) {
    p3_write_word(ram, address, (uint16_t)(value >> 16));
    p3_write_word(ram, address + 2, (uint16_t)value);
}

static uint32_t p3_read_long(const uint8_t *ram, uint32_t address) {
    return ((uint32_t)ram[address] << 24) | ((uint32_t)ram[address + 1] << 16) |
           ((uint32_t)ram[address + 2] << 8) | ram[address + 3];
}

/* UM30 §11.6.1--§11.6.5 I-Cache Case.  These literals are copied from the
 * stage 3-0 CSV, independently of cycles.c.  The first four tables select
 * word/long immediate width where applicable; jump EA has no size column. */
static int p3_ea_basic(ea_cost_kind_t kind, unsigned row, unsigned size) {
    static const unsigned fetch[12][2] = {
        {0, 0}, {0, 0}, {3, 3}, {3, 3}, {4, 4}, {4, 4},
        {6, 6}, {4, 4}, {4, 4}, {4, 4}, {6, 6}, {2, 4},
    };
    static const unsigned fetch_imm[12][2] = {
        {2, 4},  {0, 0}, {3, 4}, {5, 7}, {4, 4},  {4, 6},
        {8, 10}, {6, 8}, {6, 8}, {4, 6}, {8, 10}, {6, 8},
    };
    static const unsigned calc[12][2] = {
        {0, 0}, {0, 0}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
        {4, 4}, {2, 2}, {4, 4}, {2, 2}, {4, 4}, {0, 0},
    };
    static const unsigned calc_imm[12][2] = {
        {2, 4}, {0, 0}, {2, 4}, {4, 6}, {2, 4}, {4, 6},
        {6, 8}, {4, 6}, {6, 8}, {4, 6}, {6, 8}, {6, 8},
    };
    static const unsigned jump[12] = {0, 0, 2, 0, 0, 4, 6, 2, 2, 4, 6, 0};
    switch (kind) {
    case EA_COST_FETCH:
        return (int)fetch[row][size == 4];
    case EA_COST_FETCH_IMM:
        return (int)fetch_imm[row][size == 4];
    case EA_COST_CALC:
        return (int)calc[row][size == 4];
    case EA_COST_CALC_IMM:
        return (int)calc_imm[row][size == 4];
    case EA_COST_JUMP:
        return (int)jump[row];
    default:
        return 0;
    }
}

static int p3_ea_full(ea_cost_kind_t kind, unsigned size, unsigned bd, unsigned iis,
                      unsigned base_suppressed) {
    static const unsigned direct[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const unsigned indirect[3][3] = {{10, 12, 12}, {12, 14, 14}, {16, 18, 18}};
    unsigned value = iis == 0 ? direct[base_suppressed][bd] : indirect[bd][(iis & 3) - 1];
    if (kind == EA_COST_FETCH_IMM || kind == EA_COST_CALC_IMM)
        value += size == 4 ? 4 : 2;
    return (int)value;
}

static void test_um30_ea_tables(void) {
    enum { P3_EA_BASIC_CASES = 120, P3_EA_FULL_CASES = 1680 };
    size_t basic_cases = 0;
    size_t full_cases = 0;
    mc68k_state_t *cpu = make_cpu();

    for (unsigned row = 0; row < 12; row++)
        for (unsigned size = 2; size <= 4; size += 2)
            for (unsigned kind = EA_COST_FETCH; kind < EA_COST_KIND_COUNT; kind++) {
                ea_desc_t ea = {.size = (uint8_t)size};
                if (row < 7)
                    ea.mode = (uint8_t)row;
                else {
                    ea.mode = 7;
                    ea.reg = (uint8_t)(row - 7);
                }
                assert(ea_cycles(cpu, (ea_cost_kind_t)kind, &ea) ==
                       p3_ea_basic((ea_cost_kind_t)kind, row, size));
                basic_cases++;
            }
    for (unsigned mode7 = 0; mode7 < 2; mode7++)
        for (unsigned bd = 0; bd < 3; bd++)
            for (unsigned iis = 0; iis < 8; iis++)
                for (unsigned bs = 0; bs < 2; bs++)
                    for (unsigned is = 0; is < 2; is++)
                        for (unsigned size = 2; size <= 4; size += 2)
                            for (unsigned kind = EA_COST_FETCH; kind < EA_COST_KIND_COUNT;
                                 kind++) {
                                if (iis == 4)
                                    continue;
                                ea_desc_t ea = {.mode = mode7 ? 7 : 6,
                                                .reg = mode7 ? 3 : 0,
                                                .size = (uint8_t)size,
                                                .ext_form = EA_EXT_FULL,
                                                .bd_size = bd,
                                                .od_size = iis == 0 ? 0 : (iis & 3),
                                                .iis = iis,
                                                .base_suppressed = bs,
                                                .index_suppressed = is};
                                assert(ea_cycles(cpu, (ea_cost_kind_t)kind, &ea) ==
                                       p3_ea_full((ea_cost_kind_t)kind, size, bd, iis, bs));
                                full_cases++;
                            }
    assert(basic_cases == P3_EA_BASIC_CASES);
    assert(full_cases == P3_EA_FULL_CASES);
    mc68k_destroy(cpu);
}

/* JMP/JSR manifests execute decoded EA forms, rather than calling ea_cycles()
 * directly.  The expected values are UM30 §11.6.5 I-Cache Case: basic brief
 * index = 6, full direct = 6, and full memory indirect = 10. */
static void test_um30_jump_opcode_paths(void) {
    typedef struct {
        const char *row;
        uint16_t words[4];
        size_t word_count;
        unsigned cycles;
        uint32_t pc;
        uint32_t a7;
        uint32_t return_address;
    } p3_jump_row_t;
    static const p3_jump_row_t rows[] = {
        {"JMP (d8,A0,D1)", {0x4ef0, 0x1010}, 2, 10, 0x2020, P3_SSP, 0},
        {"JSR (d8,PC,D1)", {0x4ebb, 0x1010}, 2, 10, 0x1022, P3_SSP - 4, P3_PC + 4},
        {"JMP (bd.W,A0) full", {0x4ef0, 0x0160, 0x0010}, 3, 10, 0x2010, P3_SSP, 0},
        {"JMP ([bd.W,A0],od.W) full",
         {0x4ef0, 0x0162, 0x0400, 0x0010},
         4,
         18,
         0x2510,
         P3_SSP,
         0},
    };
    enum { P3_JUMP_OPCODE_CASES = 4 };
    assert(sizeof(rows) / sizeof(rows[0]) == P3_JUMP_OPCODE_CASES);

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        uint8_t *ram = calloc(1, P3_RAM_SIZE);
        mem_ctx_t *mem = mem_create();
        mem_region_t *region = calloc(1, sizeof(*region));
        assert(ram && mem && region);
        region->data = ram;
        region->size = P3_RAM_SIZE;
        mem_add_region(mem, region);
        p3_write_long(ram, 0, P3_SSP);
        p3_write_long(ram, 4, P3_PC);
        for (size_t word = 0; word < rows[i].word_count; word++)
            p3_write_word(ram, P3_PC + (uint32_t)(word * 2), rows[i].words[word]);
        p3_write_long(ram, 0x2400, 0x2500);

        mc68k_bus_t bus = bus_make_hooks(mem);
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
        assert(cpu);
        mc68k_reset(cpu);
        cpu->a[0] = 0x2000;
        cpu->d[1] = 0x10;
        int actual_cycles = mc68k_execute(cpu, 1);
        if (actual_cycles != (int)rows[i].cycles)
            fprintf(stderr, "%s: cycles %d != %u\n", rows[i].row, actual_cycles,
                    rows[i].cycles);
        assert(actual_cycles == (int)rows[i].cycles);
        if (cpu->pc != rows[i].pc)
            fprintf(stderr, "%s: pc %x != %x\n", rows[i].row, cpu->pc, rows[i].pc);
        assert(cpu->pc == rows[i].pc);
        assert(cpu->a[7] == rows[i].a7);
        if (rows[i].return_address != 0)
            assert(p3_read_long(ram, cpu->a[7]) == rows[i].return_address);
        mc68k_destroy(cpu);
        mem_destroy(mem);
        free(region);
        free(ram);
    }
}

/* UM30 §11.6.6 I-Cache Case.  These are literal table values plus the
 * applicable source FEA: (An)=3, brief=6, full direct BD.W=6, and full
 * indirect BD.W/outer.W=14.  The full destination rows cover every printed
 * direct, base-suppressed, and memory-indirect family; each case executes a
 * real MOVE.W and observes its destination side effect. */
static void test_um30_move_opcode_paths(void) {
    typedef struct {
        const char *row;
        uint16_t words[5];
        size_t word_count;
        unsigned cycles;
        uint32_t memory_destination;
        uint16_t expected_value;
    } p3_move_row_t;
    static const p3_move_row_t rows[] = {
        {"MOVE.W D0,D2", {0x3400}, 1, 2, P3_NO_MEMORY, 0x0012},
        {"MOVE.W (A0),D2", {0x3410}, 1, 5, P3_NO_MEMORY, 0x3456},
        {"MOVE.W (d8,A0,D1),D2", {0x3430, 0x1010}, 2, 8, P3_NO_MEMORY, 0x3456},
        {"MOVE.W (bd.W,A0),D2 full", {0x3430, 0x0160, 0x0020}, 3, 8, P3_NO_MEMORY, 0x3456},
        {"MOVE.W ([bd.W,A0],od.W),D2 full",
         {0x3430, 0x0162, 0x0010, 0x0010},
         4,
         16,
         P3_NO_MEMORY,
         0x3456},
        {"MOVE.W D0,(A1)", {0x3280}, 1, 3, 0x3000, 0x0012},
        {"MOVE.W (A0),(A1)", {0x3290}, 1, 7, 0x3000, 0x3456},
        {"MOVE.W D0,(A1)+", {0x32c0}, 1, 3, 0x3000, 0x0012},
        {"MOVE.W D0,-(A1)", {0x3300}, 1, 4, 0x2ffe, 0x0012},
        {"MOVE.W D0,(d16,A1)", {0x3340, 0x0010}, 2, 4, 0x3010, 0x0012},
        {"MOVE.W D0,(xxx).W", {0x31c0, 0x3020}, 2, 4, 0x3020, 0x0012},
        {"MOVE.W D0,(xxx).L", {0x33c0, 0x0000, 0x3020}, 3, 6, 0x3020, 0x0012},
        {"MOVE.W D0,(d8,A1,D1)", {0x3380, 0x1010}, 2, 6, 0x3020, 0x0012},
        {"MOVE.W D0,(bd.W,A1) full", {0x3380, 0x0160, 0x0010}, 3, 8, 0x3010, 0x0012},
        {"MOVE.W D0,(B) full", {0x3380, 0x01d0}, 2, 8, 0x0000, 0x0012},
        {"MOVE.W D0,(d16,B) full", {0x3380, 0x01e0, 0x0010}, 3, 10, 0x0010, 0x0012},
        {"MOVE.W D0,(d32,B) full", {0x3380, 0x01f0, 0x0000, 0x0010}, 4, 14, 0x0010, 0x0012},
        {"MOVE.W D0,([bd.W,A1]) full", {0x3380, 0x0161, 0x0010}, 3, 10, 0x3400, 0x0012},
        {"MOVE.W D0,([bd.W,A1],od.W) full",
         {0x3380, 0x0162, 0x0010, 0x0010},
         4,
         12,
         0x3410,
         0x0012},
        {"MOVE.W D0,([bd.W,A1],od.L) full",
         {0x3380, 0x0163, 0x0010, 0x0000, 0x0010},
         5,
         14,
         0x3410,
         0x0012},
    };
    enum { P3_MOVE_OPCODE_CASES = 20 };
    assert(sizeof(rows) / sizeof(rows[0]) == P3_MOVE_OPCODE_CASES);

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        uint8_t *ram = calloc(1, P3_RAM_SIZE);
        mem_ctx_t *mem = mem_create();
        mem_region_t *region = calloc(1, sizeof(*region));
        assert(ram && mem && region);
        region->data = ram;
        region->size = P3_RAM_SIZE;
        mem_add_region(mem, region);
        p3_write_long(ram, 0, P3_SSP);
        p3_write_long(ram, 4, P3_PC);
        p3_write_word(ram, 0x2000, 0x3456);
        p3_write_word(ram, 0x2010, 0x3456);
        p3_write_word(ram, 0x2020, 0x3456);
        p3_write_long(ram, 0x3010, 0x3400);
        p3_write_long(ram, 0x2010, 0x2110);
        p3_write_word(ram, 0x2110, 0x3456);
        p3_write_word(ram, 0x2120, 0x3456);
        for (size_t word = 0; word < rows[i].word_count; word++)
            p3_write_word(ram, P3_PC + (uint32_t)(word * 2), rows[i].words[word]);

        mc68k_bus_t bus = bus_make_hooks(mem);
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
        assert(cpu);
        mc68k_reset(cpu);
        cpu->a[0] = 0x2000;
        cpu->a[1] = 0x3000;
        cpu->d[0] = 0x0012;
        cpu->d[1] = 0x0010;
        int actual_cycles = mc68k_execute(cpu, 1);
        if (actual_cycles != (int)rows[i].cycles)
            fprintf(stderr, "%s: cycles %d != %u\n", rows[i].row, actual_cycles,
                    rows[i].cycles);
        assert(actual_cycles == (int)rows[i].cycles);
        if (rows[i].memory_destination != P3_NO_MEMORY) {
            uint16_t actual_value = (uint16_t)ram[rows[i].memory_destination] << 8 |
                                    ram[rows[i].memory_destination + 1];
            if (actual_value != rows[i].expected_value)
                fprintf(stderr, "%s: memory %x != %x\n", rows[i].row, actual_value,
                        rows[i].expected_value);
            assert(((uint16_t)ram[rows[i].memory_destination] << 8 |
                    ram[rows[i].memory_destination + 1]) == rows[i].expected_value);
        } else {
            if ((uint16_t)cpu->d[2] != rows[i].expected_value)
                fprintf(stderr, "%s: D2 %x != %x\n", rows[i].row, (uint16_t)cpu->d[2],
                        rows[i].expected_value);
            assert((uint16_t)cpu->d[2] == rows[i].expected_value);
        }
        mc68k_destroy(cpu);
        mem_destroy(mem);
        free(region);
        free(ram);
    }
}

/* The §11.6.6 full-extension destination table has 42 legal descriptor
 * combinations (2 base states × 3 BD sizes × [direct + 6 IIS forms]).  Run
 * every one as MOVE.W D0,<full EA>; the table below is independent of the
 * production selector. */
static void test_um30_move_full_destination_manifest(void) {
    static const unsigned direct[2][3] = {{6, 8, 12}, {8, 10, 14}};
    static const unsigned indirect[2][3][3] = {
        {{8, 10, 12}, {10, 12, 14}, {14, 16, 18}},
        {{10, 12, 14}, {12, 14, 16}, {16, 18, 20}},
    };
    enum { P3_MOVE_FULL_DESTINATION_CASES = 42 };
    size_t cases = 0;

    for (unsigned bs = 0; bs < 2; bs++)
        for (unsigned bd = 0; bd < 3; bd++)
            for (unsigned iis = 0; iis < 8; iis++) {
                if (iis == 4)
                    continue;
                uint8_t *ram = calloc(1, P3_RAM_SIZE);
                mem_ctx_t *mem = mem_create();
                mem_region_t *region = calloc(1, sizeof(*region));
                uint16_t words[6] = {0x3380, (uint16_t)(0x0100 | (iis < 5 ? 0x0040 : 0) |
                                                        (bs << 7) | ((bd + 1) << 4) | iis)};
                size_t word_count = 2;
                assert(ram && mem && region);
                if (bd == 1)
                    words[word_count++] = 0x0010;
                if (bd == 2) {
                    words[word_count++] = 0;
                    words[word_count++] = 0x0010;
                }
                if ((iis & 3) == 2)
                    words[word_count++] = 0x0010;
                if ((iis & 3) == 3) {
                    words[word_count++] = 0;
                    words[word_count++] = 0x0010;
                }
                region->data = ram;
                region->size = P3_RAM_SIZE;
                mem_add_region(mem, region);
                p3_write_long(ram, 0, P3_SSP);
                p3_write_long(ram, 4, P3_PC);
                for (size_t word = 0; word < word_count; word++)
                    p3_write_word(ram, P3_PC + (uint32_t)(word * 2), words[word]);
                mc68k_bus_t bus = bus_make_hooks(mem);
                mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
                assert(cpu);
                mc68k_reset(cpu);
                /* All possible base and base-suppressed pointer locations. */
                p3_write_long(ram, 0x0000, 0x3400);
                p3_write_long(ram, 0x0010, 0x3400);
                p3_write_long(ram, 0x0012, 0x3400);
                p3_write_long(ram, 0x3000, 0x3400);
                p3_write_long(ram, 0x3010, 0x3400);
                p3_write_long(ram, 0x3012, 0x3400);
                cpu->a[1] = 0x3000;
                cpu->d[0] = 0x0012;
                unsigned expected = iis == 0 ? direct[bs][bd] : indirect[bs][bd][(iis & 3) - 1];
                int actual = mc68k_execute(cpu, 1);
                if (actual != (int)expected)
                    fprintf(stderr, "full destination bs=%u bd=%u iis=%u: %d != %u\n", bs, bd,
                            iis, actual, expected);
                assert(actual == (int)expected);
                assert(cpu->pc == P3_PC + word_count * 2);
                int wrote = 0;
                for (uint32_t address = 0; address + 1 < P3_RAM_SIZE; address += 2)
                    if ((((uint16_t)ram[address] << 8) | ram[address + 1]) == 0x0012)
                        wrote = 1;
                assert(wrote);
                mc68k_destroy(cpu);
                mem_destroy(mem);
                free(region);
                free(ram);
                cases++;
            }
    assert(cases == P3_MOVE_FULL_DESTINATION_CASES);
}

/* Phase 3c exercises the execution paths behind UM30 11.6.7--11.6.12.
 * Values are copied from the printed I-Cache Case bases plus the table's
 * marked FEA/FIEA/CEA/CIEA contribution for (A0).  The same literal oracle
 * is executed on both members of the 68030 timing family. */
typedef struct {
    const char *row;
    uint16_t words[4];
    uint8_t nwords;
    uint32_t d0, d1;
    unsigned cycles;
} p3c_row_t;

/* UM30 11.6.7--11.6.12, one literal I-Cache Case base per printed ordinal.
 * Formula rows use the concrete n=1, (A0) sample exercised below: 14 for
 * EA->RL and 8 for RL->EA.  This is deliberately separate from the decoder
 * and EA timing code, so a production-table edit cannot regenerate it. */
typedef struct {
    const char *section;
    uint8_t ordinal;
    unsigned cycles;
} p3c_input_t;
static const p3c_input_t p3c_input_1167[] = {
    {"11.6.7", 1, 4},   {"11.6.7", 2, 6},   {"11.6.7", 3, 6},   {"11.6.7", 4, 12},
    {"11.6.7", 5, 4},   {"11.6.7", 6, 4},   {"11.6.7", 7, 4},   {"11.6.7", 8, 4},
    {"11.6.7", 9, 4},   {"11.6.7", 10, 4},  {"11.6.7", 11, 8},  {"11.6.7", 12, 14},
    {"11.6.7", 13, 8},  {"11.6.7", 14, 10}, {"11.6.7", 15, 10}, {"11.6.7", 16, 14},
    {"11.6.7", 17, 14}, {"11.6.7", 18, 7},  {"11.6.7", 19, 5},  {"11.6.7", 20, 4},
    {"11.6.7", 21, 4},  {"11.6.7", 22, 4},
};
static const p3c_input_t p3c_input_1168[] = {
    {"11.6.8", 1, 2},   {"11.6.8", 2, 4},   {"11.6.8", 3, 2},   {"11.6.8", 4, 2},
    {"11.6.8", 5, 4},   {"11.6.8", 6, 2},   {"11.6.8", 7, 3},   {"11.6.8", 8, 2},
    {"11.6.8", 9, 2},   {"11.6.8", 10, 3},  {"11.6.8", 11, 2},  {"11.6.8", 12, 3},
    {"11.6.8", 13, 2},  {"11.6.8", 14, 2},  {"11.6.8", 15, 3},  {"11.6.8", 16, 2},
    {"11.6.8", 17, 2},  {"11.6.8", 18, 3},  {"11.6.8", 19, 4},  {"11.6.8", 20, 2},
    {"11.6.8", 21, 4},  {"11.6.8", 22, 2},  {"11.6.8", 23, 2},  {"11.6.8", 24, 2},
    {"11.6.8", 25, 4},  {"11.6.8", 26, 4},  {"11.6.8", 27, 20}, {"11.6.8", 28, 28},
    {"11.6.8", 29, 44}, {"11.6.8", 30, 28}, {"11.6.8", 31, 44}, {"11.6.8", 32, 56},
    {"11.6.8", 33, 56}, {"11.6.8", 34, 90}, {"11.6.8", 35, 90}, {"11.6.8", 36, 44},
    {"11.6.8", 37, 44}, {"11.6.8", 38, 78}, {"11.6.8", 39, 78},
};
static const p3c_input_t p3c_input_1169[] = {
    {"11.6.9", 1, 2},  {"11.6.9", 2, 2},  {"11.6.9", 3, 3},  {"11.6.9", 4, 2},
    {"11.6.9", 5, 3},  {"11.6.9", 6, 2},  {"11.6.9", 7, 3},  {"11.6.9", 8, 2},
    {"11.6.9", 9, 3},  {"11.6.9", 10, 2}, {"11.6.9", 11, 3}, {"11.6.9", 12, 2},
    {"11.6.9", 13, 3}, {"11.6.9", 14, 2}, {"11.6.9", 15, 3}, {"11.6.9", 16, 2},
    {"11.6.9", 17, 2},
};
static const p3c_input_t p3c_input_11610[] = {
    {"11.6.10", 1, 4},   {"11.6.10", 2, 13}, {"11.6.10", 3, 4},   {"11.6.10", 4, 13},
    {"11.6.10", 5, 2},   {"11.6.10", 6, 9},  {"11.6.10", 7, 2},   {"11.6.10", 8, 9},
    {"11.6.10", 9, 8},   {"11.6.10", 10, 6}, {"11.6.10", 11, 11}, {"11.6.10", 12, 8},
    {"11.6.10", 13, 11},
};
static const p3c_input_t p3c_input_11611[] = {
    {"11.6.11", 1, 2},  {"11.6.11", 2, 3},   {"11.6.11", 3, 2},  {"11.6.11", 4, 3},
    {"11.6.11", 5, 2},  {"11.6.11", 6, 3},   {"11.6.11", 7, 2},  {"11.6.11", 8, 3},
    {"11.6.11", 9, 4},  {"11.6.11", 10, 6},  {"11.6.11", 11, 4}, {"11.6.11", 12, 5},
    {"11.6.11", 13, 4}, {"11.6.11", 14, 12}, {"11.6.11", 15, 2}, {"11.6.11", 16, 2},
};
static const p3c_input_t p3c_input_11612[] = {
    {"11.6.12", 1, 4},  {"11.6.12", 2, 6},   {"11.6.12", 3, 8},   {"11.6.12", 4, 4},
    {"11.6.12", 5, 6},  {"11.6.12", 6, 8},   {"11.6.12", 7, 6},   {"11.6.12", 8, 4},
    {"11.6.12", 9, 6},  {"11.6.12", 10, 10}, {"11.6.12", 11, 4},  {"11.6.12", 12, 6},
    {"11.6.12", 13, 8}, {"11.6.12", 14, 6},  {"11.6.12", 15, 12}, {"11.6.12", 16, 4},
};

/* Each entry is the explicitly selected EA contribution for the matching
 * printed ordinal and concrete opcode below.  The two MOVEM formula entries
 * above already include n=1 and CIEA(A0), so their delta is zero. */
static const uint8_t p3c_delta_1167[] = {
    0, 0, 0, 0, 0, 2, 0, 3, 0, 2, 3, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0,
};
static const uint8_t p3c_delta_1168[] = {
    0, 0, 0, 3, 3, 3, 3, 0, 3, 3, 0, 3, 0, 3, 3, 0, 3, 3, 0, 0,
    3, 3, 0, 3, 0, 3, 3, 3, 3, 3, 3, 0, 3, 0, 3, 0, 3, 0, 3,
};
static const uint8_t p3c_delta_1169[] = {
    0, 0, 3, 0, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
};
static const uint8_t p3c_delta_11610[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t p3c_delta_11611[] = {
    0, 2, 0, 3, 0, 3, 0, 3, 0, 0, 0, 2, 0, 2, 0, 3,
};
static const uint8_t p3c_delta_11612[] = {
    0, 0, 0, 3, 0, 0, 3, 0, 0, 0, 3, 0, 0, 3, 0, 3,
};

static void test_um30_phase3c_input_manifest(void) {
    const p3c_input_t *const tables[] = {p3c_input_1167,  p3c_input_1168,  p3c_input_1169,
                                         p3c_input_11610, p3c_input_11611, p3c_input_11612};
    const size_t counts[] = {22, 39, 17, 13, 16, 16};
    const size_t actual_counts[] = {
        sizeof(p3c_input_1167) / sizeof(p3c_input_1167[0]),
        sizeof(p3c_input_1168) / sizeof(p3c_input_1168[0]),
        sizeof(p3c_input_1169) / sizeof(p3c_input_1169[0]),
        sizeof(p3c_input_11610) / sizeof(p3c_input_11610[0]),
        sizeof(p3c_input_11611) / sizeof(p3c_input_11611[0]),
        sizeof(p3c_input_11612) / sizeof(p3c_input_11612[0]),
    };
    const size_t delta_counts[] = {
        sizeof(p3c_delta_1167) / sizeof(p3c_delta_1167[0]),
        sizeof(p3c_delta_1168) / sizeof(p3c_delta_1168[0]),
        sizeof(p3c_delta_1169) / sizeof(p3c_delta_1169[0]),
        sizeof(p3c_delta_11610) / sizeof(p3c_delta_11610[0]),
        sizeof(p3c_delta_11611) / sizeof(p3c_delta_11611[0]),
        sizeof(p3c_delta_11612) / sizeof(p3c_delta_11612[0]),
    };
    size_t total = 0;
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); ++t) {
        assert(actual_counts[t] == counts[t] && delta_counts[t] == counts[t]);
        for (size_t i = 0; i < counts[t]; ++i) {
            assert(tables[t][i].ordinal == i + 1);
            assert(tables[t][i].cycles != 0);
        }
        total += counts[t];
    }
    assert(total == 123);
}

/* Functional-state fingerprints are regression snapshots, not timing oracles.
 * They include the registers and operand-memory window touched by these
 * one-instruction fixtures.  The UM30 cycle literals above remain independent. */
static uint32_t p3c_effect_hash(const mc68k_state_t *cpu, const uint8_t *ram) {
    const uint32_t values[] = {cpu->d[0], cpu->d[1], cpu->a[0], cpu->a[1], cpu->a[7],
                               cpu->sr,   cpu->usp,  cpu->vbr,  cpu->sfc,  cpu->dfc};
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint32_t value = values[i];
        for (int byte = 0; byte < 4; ++byte) {
            hash = (hash ^ (value & 0xffu)) * UINT32_C(16777619);
            value >>= 8;
        }
    }
    for (uint32_t address = 0x1ff0u; address < 0x2020u; ++address)
        hash = (hash ^ ram[address]) * UINT32_C(16777619);
    return hash;
}

static void p3c_assert_semantics(size_t index, const mc68k_state_t *cpu, const uint8_t *ram) {
    switch (index) {
    case 0: /* §11.6.7 ord 1 EXG D0,D1 */
        assert(cpu->d[0] == 2 && cpu->d[1] == 1);
        break;
    case 6: /* ord 7 MOVE D0,CCR */
        assert(cpu->sr == 0x2701);
        break;
    case 9: /* ord 10 MOVE SR,(A0) */
        assert(ram[0x2000] == 0x27 && ram[0x2001] == 0x04);
        break;
    case 10: /* ord 11 MOVE (A0),SR enters user mode */
        assert(cpu->sr == 0x0103 && cpu->ssp == P3_SSP);
        break;
    case 11: /* ord 12 MOVEM.L (A0),D0 */
        assert(cpu->d[0] == 0x01030203u);
        break;
    case 14: /* ord 15 MOVEP.W (d16,A0),D0 */
        assert(cpu->d[0] == 0x0102u);
        break;
    case 16: /* ord 17 MOVEP.L (d16,A0),D0 */
        assert(cpu->d[0] == 0x01020406u);
        break;
    case 22: /* §11.6.8 ord 1 ADD.B D1,D0 */
        assert(cpu->d[0] == 3);
        break;
    case 25: /* ord 4 ADD.B (A0),D0 */
        assert(cpu->d[0] == 2);
        break;
    case 26: /* ord 5 ADDA.W (A0),A0 */
        assert(cpu->a[0] == 0x2103u);
        break;
    case 66: /* §11.6.9 ord 6 ADDI.B #1,D0 */
        assert(cpu->d[0] == 0x81u);
        break;
    case 91: /* §11.6.11 ord 1 CLR.W D0 */
        assert(cpu->d[0] == 0);
        break;
    case 104: /* ord 14 TAS (A0) */
        assert(ram[0x2000] == 0x81);
        break;
    case 106: /* ord 16 TST.W (A0) clears Z */
        assert(cpu->sr == 0x2700);
        break;
    case 122: /* §11.6.12 ord 16 ROXL.W (A0) */
        assert(ram[0x2000] == 0x02 && ram[0x2001] == 0x06);
        break;
    default:
        break;
    }
}

static const uint32_t p3c_effect_hashes[123] = {
    /* UM30 §11.6.7: ord 1--22. */
    UINT32_C(0x632297ed),
    UINT32_C(0xaf9ec0e2),
    UINT32_C(0xa3e012e2),
    UINT32_C(0x7dc35082),
    UINT32_C(0xfc75f25e),
    UINT32_C(0xc022cef6),
    UINT32_C(0xe128a32e),
    UINT32_C(0x1bb75729),
    UINT32_C(0xa7794497),
    UINT32_C(0x2ad92a79),
    UINT32_C(0xbebbb1a3),
    UINT32_C(0x08a8efb3),
    UINT32_C(0xa663cb11),
    UINT32_C(0x50a0a669),
    UINT32_C(0x8ff4b9a7),
    UINT32_C(0xae27fe0f),
    UINT32_C(0xe8692c51),
    UINT32_C(0x0584673f),
    UINT32_C(0x1fdac9af),
    UINT32_C(0x9de15942),
    UINT32_C(0x899a9802),
    UINT32_C(0x61d73f1e),
    /* UM30 §11.6.8: ord 1--39. */
    UINT32_C(0x75807a7b),
    UINT32_C(0xc8758e02),
    UINT32_C(0xc8758e02),
    UINT32_C(0x4ad13f1c),
    UINT32_C(0x2df174a7),
    UINT32_C(0x7a252256),
    UINT32_C(0x7b42308c),
    UINT32_C(0xe69c8370),
    UINT32_C(0xc305811b),
    UINT32_C(0xc305811b),
    UINT32_C(0xaf9ec0e2),
    UINT32_C(0xf7b4e872),
    UINT32_C(0x75807a7b),
    UINT32_C(0xc305811b),
    UINT32_C(0xc305811b),
    UINT32_C(0x39a9e972),
    UINT32_C(0xaf9ec0e2),
    UINT32_C(0xf7b4e872),
    UINT32_C(0x5c9b7d81),
    UINT32_C(0x5c9b7d81),
    UINT32_C(0xfd2a52f0),
    UINT32_C(0x17843406),
    UINT32_C(0x5a22d794),
    UINT32_C(0x0584673f),
    UINT32_C(0xc305811b),
    UINT32_C(0xab3cd0f6),
    UINT32_C(0x0584673f),
    UINT32_C(0x4f306c82),
    UINT32_C(0xc20a4317),
    UINT32_C(0x4f306c82),
    UINT32_C(0xc20a4317),
    UINT32_C(0xa30a41c5),
    UINT32_C(0x2f5d5067),
    UINT32_C(0xe69c8370),
    UINT32_C(0xaf9ec0e2),
    UINT32_C(0xa30a41c5),
    UINT32_C(0x2f5d5067),
    UINT32_C(0xe69c8370),
    UINT32_C(0xaf9ec0e2),
    /* UM30 §11.6.9: ord 1--17. */
    UINT32_C(0xc305811b),
    UINT32_C(0x637c4b9b),
    UINT32_C(0x1324e657),
    UINT32_C(0xaf9ec0e2),
    UINT32_C(0x9ce12629),
    UINT32_C(0x3fcb9ee3),
    UINT32_C(0xca26b591),
    UINT32_C(0xaf9ec0e2),
    UINT32_C(0x7562ab1e),
    UINT32_C(0x3fcb9ee3),
    UINT32_C(0x5de50a2f),
    UINT32_C(0x3fcb9ee3),
    UINT32_C(0x7562ab1e),
    UINT32_C(0xaf9ec0e2),
    UINT32_C(0x5de50a2f),
    UINT32_C(0x66b7d77c),
    UINT32_C(0x50158b62),
    /* UM30 §11.6.10: ord 1--13. */
    UINT32_C(0x75807a7b),
    UINT32_C(0x0a35a7d0),
    UINT32_C(0x007e818c),
    UINT32_C(0x0a35a7d0),
    UINT32_C(0x75807a7b),
    UINT32_C(0x0a35a7d0),
    UINT32_C(0x39a9e972),
    UINT32_C(0x0a35a7d0),
    UINT32_C(0x232a00e9),
    UINT32_C(0x1be8bd42),
    UINT32_C(0x625c0450),
    UINT32_C(0x1be8bd42),
    UINT32_C(0xd055790a),
    /* UM30 §11.6.11: ord 1--16. */
    UINT32_C(0xaf9ec0e2),
    UINT32_C(0xb4059592),
    UINT32_C(0x2831f05d),
    UINT32_C(0x2e893f72),
    UINT32_C(0x2831f05d),
    UINT32_C(0x2e893f72),
    UINT32_C(0x01b7911d),
    UINT32_C(0x11a1af88),
    UINT32_C(0xc305811b),
    UINT32_C(0x7468057b),
    UINT32_C(0x981003b1),
    UINT32_C(0x56d4ff24),
    UINT32_C(0x3f76d8e6),
    UINT32_C(0x37586a9e),
    UINT32_C(0x7562ab1e),
    UINT32_C(0x7562ab1e),
    /* UM30 §11.6.12: ord 1--16. */
    UINT32_C(0x7d52fce5),
    UINT32_C(0x11df13b2),
    UINT32_C(0x0b26563f),
    UINT32_C(0x968b0122),
    UINT32_C(0x7d52fce5),
    UINT32_C(0xb3e867c8),
    UINT32_C(0x968b0122),
    UINT32_C(0xeeff485e),
    UINT32_C(0x11df13b2),
    UINT32_C(0x0b26563f),
    UINT32_C(0xe239d320),
    UINT32_C(0x7d52fce5),
    UINT32_C(0xb3e867c8),
    UINT32_C(0x968b0122),
    UINT32_C(0x3f76d8e6),
    UINT32_C(0x968b0122),
};

static void test_um30_phase3c_opcode_paths(bool emit_effects) {
    static const p3c_row_t rows[] = {
        /* 11.6.7 */
        {"EXG", {0xc141}, 1, 1, 2, 4},
        {"MOVEC Cr,Rn", {0x4e7a, 0x0801}, 2, 0, 0, 6},
        {"MOVEC Rn,Cr-A", {0x4e7b, 0x0801}, 2, 0, 0, 6},
        {"MOVEC Rn,Cr-B", {0x4e7b, 0x0000}, 2, 1, 0, 12},
        {"MOVE CCR,Dn", {0x42c0}, 1, 0, 0, 4},
        {"MOVE CCR,Mem", {0x42d0}, 1, 0, 0, 6},
        {"MOVE Dn,CCR", {0x44c0}, 1, 1, 0, 4},
        {"MOVE EA,CCR", {0x44d0}, 1, 0, 0, 7},
        {"MOVE SR,Dn", {0x40c0}, 1, 0, 0, 4},
        {"MOVE SR,Mem", {0x40d0}, 1, 0, 0, 6},
        {"MOVE EA,SR", {0x46d0}, 1, 0, 0, 11},
        {"MOVEM EA,RL", {0x4cd0, 0x0001}, 2, 0, 0, 14},
        {"MOVEM RL,EA", {0x48d0, 0x0001}, 2, 0, 0, 8},
        {"MOVEP.W store", {0x0188, 0}, 2, 0, 0, 10},
        {"MOVEP.W load", {0x0108, 0}, 2, 0, 0, 10},
        {"MOVEP.L store", {0x01c8, 0}, 2, 0, 0, 14},
        {"MOVEP.L load", {0x0148, 0}, 2, 0, 0, 14},
        {"MOVES EA,Rn", {0x0e10, 0}, 2, 0, 0, 9},
        {"MOVES Rn,EA", {0x0e10, 0x0800}, 2, 0, 0, 7},
        {"MOVE USP,An", {0x4e68}, 1, 0, 0, 4},
        {"MOVE An,USP", {0x4e60}, 1, 0, 0, 4},
        {"SWAP", {0x4840}, 1, 0, 0, 4},
        /* 11.6.8 */
        {"ADD Rn,Dn", {0xd001}, 1, 1, 2, 2},
        {"ADDA.W Rn,An", {0xd0c0}, 1, 1, 0, 4},
        {"ADDA.L Rn,An", {0xd1c0}, 1, 1, 0, 2},
        {"ADD EA,Dn", {0xd010}, 1, 1, 0, 5},
        {"ADDA.W EA,An", {0xd0d0}, 1, 1, 0, 7},
        {"ADDA.L EA,An", {0xd1d0}, 1, 1, 0, 5},
        {"ADD Dn,EA", {0xd110}, 1, 1, 0, 6},
        {"AND Dn,Dn", {0xc001}, 1, 1, 2, 2},
        {"AND EA,Dn", {0xc010}, 1, 1, 0, 5},
        {"AND Dn,EA", {0xc110}, 1, 1, 0, 6},
        {"EOR Dn,Dn", {0xb100}, 1, 1, 0, 2},
        {"EOR Dn,EA", {0xb110}, 1, 1, 0, 6},
        {"OR Dn,Dn", {0x8001}, 1, 1, 2, 2},
        {"OR EA,Dn", {0x8010}, 1, 1, 0, 5},
        {"OR Dn,EA", {0x8110}, 1, 1, 0, 6},
        {"SUB Rn,Dn", {0x9001}, 1, 1, 2, 2},
        {"SUB EA,Dn", {0x9010}, 1, 1, 0, 5},
        {"SUB Dn,EA", {0x9110}, 1, 1, 0, 6},
        {"SUBA.W Rn,An", {0x90c0}, 1, 1, 0, 4},
        {"SUBA.L Rn,An", {0x91c0}, 1, 1, 0, 2},
        {"SUBA.W EA,An", {0x90d0}, 1, 1, 0, 7},
        {"SUBA.L EA,An", {0x91d0}, 1, 1, 0, 5},
        {"CMP Rn,Dn", {0xb001}, 1, 1, 2, 2},
        {"CMP EA,Dn", {0xb010}, 1, 1, 0, 5},
        {"CMPA Rn,An", {0xb0c0}, 1, 1, 0, 4},
        {"CMPA.L EA,An", {0xb1d0}, 1, 1, 0, 7},
        {"CMP2 EA,Rn", {0x00d0, 0}, 2, 1, 0, 23},
        {"MULS.W EA,Dn", {0xc1d0}, 1, 1, 0, 31},
        {"MULS.L EA,Dn", {0x4c10, 0x0800}, 2, 1, 0, 47},
        {"MULU.W EA,Dn", {0xc0d0}, 1, 1, 0, 31},
        {"MULU.L EA,Dn", {0x4c10, 0}, 2, 1, 0, 47},
        {"DIVS.W Dn,Dn", {0x81c1}, 1, 1, 2, 56},
        {"DIVS.W EA,Dn", {0x81d0}, 1, 1, 0, 59},
        {"DIVS.L Dn,Dn", {0x4c41, 0x0800}, 2, 1, 2, 90},
        {"DIVS.L EA,Dn", {0x4c50, 0x0800}, 2, 1, 0, 93},
        {"DIVU.W Dn,Dn", {0x80c1}, 1, 1, 2, 44},
        {"DIVU.W EA,Dn", {0x80d0}, 1, 1, 0, 47},
        {"DIVU.L Dn,Dn", {0x4c41, 0}, 2, 1, 2, 78},
        {"DIVU.L EA,Dn", {0x4c50, 0}, 2, 1, 0, 81},
        /* 11.6.9 */
        {"MOVEQ", {0x7001}, 1, 0, 0, 2},
        {"ADDQ Rn", {0x5240}, 1, 0, 0, 2},
        {"ADDQ Mem", {0x5250}, 1, 0, 0, 6},
        {"SUBQ Rn", {0x5340}, 1, 1, 0, 2},
        {"SUBQ Mem", {0x5350}, 1, 0, 0, 6},
        {"ADDI Dn", {0x0600, 1}, 2, 0, 0, 4},
        {"ADDI Mem", {0x0610, 1}, 2, 0, 0, 6},
        {"ANDI Dn", {0x0200, 1}, 2, 0, 0, 4},
        {"ANDI Mem", {0x0210, 1}, 2, 0, 0, 6},
        {"EORI Dn", {0x0a00, 1}, 2, 0, 0, 4},
        {"EORI Mem", {0x0a10, 1}, 2, 0, 0, 6},
        {"ORI Dn", {0x0000, 1}, 2, 0, 0, 4},
        {"ORI Mem", {0x0010, 1}, 2, 0, 0, 6},
        {"SUBI Dn", {0x0400, 1}, 2, 1, 0, 4},
        {"SUBI Mem", {0x0410, 1}, 2, 0, 0, 6},
        {"CMPI Dn", {0x0c00, 1}, 2, 0, 0, 4},
        {"CMPI Mem", {0x0c10, 1}, 2, 0, 0, 5},
        /* 11.6.10 */
        {"ABCD Dn", {0xc101}, 1, 1, 2, 4},
        {"ABCD Mem", {0xc109}, 1, 1, 2, 13},
        {"SBCD Dn", {0x8101}, 1, 1, 2, 4},
        {"SBCD Mem", {0x8109}, 1, 1, 2, 13},
        {"ADDX Dn", {0xd101}, 1, 1, 2, 2},
        {"ADDX Mem", {0xd109}, 1, 1, 2, 9},
        {"SUBX Dn", {0x9101}, 1, 1, 2, 2},
        {"SUBX Mem", {0x9109}, 1, 1, 2, 9},
        {"CMPM", {0xb109}, 1, 1, 2, 8},
        {"PACK Dn", {0x8141, 0}, 2, 1, 2, 6},
        {"PACK Mem", {0x8148, 0}, 2, 1, 2, 11},
        {"UNPK Dn", {0x8181, 0}, 2, 1, 2, 8},
        {"UNPK Mem", {0x8188, 0}, 2, 1, 2, 11},
        /* 11.6.11 */
        {"CLR Dn", {0x4240}, 1, 0, 0, 2},
        {"CLR Mem", {0x4250}, 1, 0, 0, 5},
        {"NEG Dn", {0x4440}, 1, 1, 0, 2},
        {"NEG Mem", {0x4450}, 1, 0, 0, 6},
        {"NEGX Dn", {0x4040}, 1, 1, 0, 2},
        {"NEGX Mem", {0x4050}, 1, 0, 0, 6},
        {"NOT Dn", {0x4640}, 1, 1, 0, 2},
        {"NOT Mem", {0x4650}, 1, 0, 0, 6},
        {"EXT Dn", {0x4880}, 1, 1, 0, 4},
        {"NBCD Dn", {0x4800}, 1, 0, 0, 6},
        {"Scc Dn", {0x57c0}, 1, 0, 0, 4},
        {"Scc Mem", {0x57d0}, 1, 0, 0, 7},
        {"TAS Dn", {0x4ac0}, 1, 0, 0, 4},
        {"TAS Mem", {0x4ad0}, 1, 0, 0, 14},
        {"TST Dn", {0x4a40}, 1, 0, 0, 2},
        {"TST Mem", {0x4a50}, 1, 0, 0, 5},
        /* 11.6.12 */
        {"LSL #", {0xe348}, 1, 0, 0, 4},
        {"LSR reg <= bits", {0xe268}, 1, 0, 16, 6},
        {"LSR reg > bits", {0xe268}, 1, 0, 17, 8},
        {"LSL Mem", {0xe3d0}, 1, 0, 0, 7},
        {"ASL #", {0xe340}, 1, 0, 0, 6},
        {"ASL reg", {0xe360}, 1, 0, 1, 8},
        {"ASL Mem", {0xe1d0}, 1, 0, 0, 9},
        {"ASR #", {0xe240}, 1, 0, 0, 4},
        {"ASR reg <= bits", {0xe260}, 1, 0, 16, 6},
        {"ASR reg > bits", {0xe260}, 1, 0, 17, 10},
        {"ASR Mem", {0xe0d0}, 1, 0, 0, 7},
        {"ROL #", {0xe358}, 1, 0, 0, 6},
        {"ROL reg", {0xe378}, 1, 0, 1, 8},
        {"ROL Mem", {0xe7d0}, 1, 0, 0, 9},
        {"ROXL", {0xe330}, 1, 0, 0, 12},
        {"ROXL Mem", {0xe5d0}, 1, 0, 0, 7},
    };
    enum { P3C_OPCODE_CASES = 123 };
    assert(sizeof(rows) / sizeof(rows[0]) == P3C_OPCODE_CASES);
    assert(sizeof(p3c_effect_hashes) / sizeof(p3c_effect_hashes[0]) == P3C_OPCODE_CASES);
    const p3c_input_t *const inputs[] = {p3c_input_1167,  p3c_input_1168,  p3c_input_1169,
                                         p3c_input_11610, p3c_input_11611, p3c_input_11612};
    const uint8_t *const deltas[] = {p3c_delta_1167,  p3c_delta_1168,  p3c_delta_1169,
                                     p3c_delta_11610, p3c_delta_11611, p3c_delta_11612};
    const size_t counts[] = {22, 39, 17, 13, 16, 16};
    size_t offset = 0;
    for (size_t t = 0; t < sizeof(counts) / sizeof(counts[0]); ++t) {
        for (size_t i = 0; i < counts[t]; ++i) {
            assert(inputs[t][i].ordinal == i + 1);
            if (rows[offset + i].cycles != inputs[t][i].cycles + deltas[t][i])
                fprintf(stderr, "%s ord %zu %s: %u != %u + %u\n", inputs[t][i].section, i + 1,
                        rows[offset + i].row, rows[offset + i].cycles, inputs[t][i].cycles,
                        deltas[t][i]);
            assert(rows[offset + i].cycles == inputs[t][i].cycles + deltas[t][i]);
            assert(rows[offset + i].nwords > 0 && rows[offset + i].nwords <= 4);
        }
        offset += counts[t];
    }
    assert(offset == P3C_OPCODE_CASES);
    const mc68k_model_t models[] = {MC68K_MODEL_68030, MC68K_MODEL_68EC030};
    size_t executions = 0;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
        for (size_t m = 0; m < 2; ++m) {
            ++executions;
            uint8_t *ram = calloc(1, P3_RAM_SIZE);
            mem_ctx_t *mem = mem_create();
            mem_region_t *region = calloc(1, sizeof(*region));
            assert(ram && mem && region);
            region->data = ram;
            region->size = P3_RAM_SIZE;
            mem_add_region(mem, region);
            p3_write_long(ram, 0, P3_SSP);
            p3_write_long(ram, 4, P3_PC);
            p3_write_word(ram, 0x2000, 0x0103);
            p3_write_word(ram, 0x1ffe, 3);
            p3_write_word(ram, 0x2002, 0x0203);
            p3_write_word(ram, 0x2004, 0x0405);
            p3_write_word(ram, 0x2006, 0x0607);
            for (size_t w = 0; w < rows[i].nwords; ++w)
                p3_write_word(ram, P3_PC + 2 * w, rows[i].words[w]);
            mc68k_bus_t bus = bus_make_hooks(mem);
            mc68k_state_t *cpu = mc68k_create_model(&bus, models[m]);
            assert(cpu);
            mc68k_reset(cpu);
            cpu->sr = 0x2704;
            cpu->a[0] = 0x2000;
            cpu->a[1] = 0x2010;
            cpu->d[0] = rows[i].d0 ? rows[i].d0 : 0x80u;
            cpu->d[1] = rows[i].d1;
            int got = mc68k_execute(cpu, 1);
            if (got != (int)rows[i].cycles)
                fprintf(stderr, "%s model %d: %d != %u\n", rows[i].row, models[m], got,
                        rows[i].cycles);
            assert(got == (int)rows[i].cycles);
            assert(cpu->pc == P3_PC + 2u * rows[i].nwords);
            assert(cpu->exec_state == CPU_NORMAL);
            assert(cpu->a[7] == (rows[i].words[0] == 0x46d0 ? 0u : P3_SSP));
            p3c_assert_semantics(i, cpu, ram);
            uint32_t effect = p3c_effect_hash(cpu, ram);
            if (emit_effects && m == 0)
                printf("%zu,%s,%08x\n", i + 1, rows[i].row, effect);
            if (!emit_effects) {
                if (effect != p3c_effect_hashes[i])
                    fprintf(stderr, "%s model %d: effect %08x != %08x\n", rows[i].row,
                            models[m], effect, p3c_effect_hashes[i]);
                assert(effect == p3c_effect_hashes[i]);
            }
            mc68k_destroy(cpu);
            mem_destroy(mem);
            free(region);
            free(ram);
        }
    assert(executions == P3C_OPCODE_CASES * (sizeof(models) / sizeof(models[0])));
}

/* UM30 §11.6.13 は '*' に FEA、'#' に FIEA を使う。§11.6.14 は '*' が FEA でなく
 * CIEA を表す例外表である。literal は tests/data/cpu/um30_11_6_icache_case.csv の
 * 印字された I-Cache Case 基準値である。 */
static const p3c_input_t p3d_input_11613[] = {
    {"11.6.13", 1, 4},  {"11.6.13", 2, 4},  {"11.6.13", 3, 4},  {"11.6.13", 4, 4},
    {"11.6.13", 5, 6},  {"11.6.13", 6, 6},  {"11.6.13", 7, 6},  {"11.6.13", 8, 6},
    {"11.6.13", 9, 6},  {"11.6.13", 10, 6}, {"11.6.13", 11, 6}, {"11.6.13", 12, 6},
    {"11.6.13", 13, 6}, {"11.6.13", 14, 6}, {"11.6.13", 15, 6}, {"11.6.13", 16, 6},
};
static const p3c_input_t p3d_input_11614[] = {
    {"11.6.14", 1, 8},   {"11.6.14", 2, 10},  {"11.6.14", 3, 14},  {"11.6.14", 4, 14},
    {"11.6.14", 5, 14},  {"11.6.14", 6, 22},  {"11.6.14", 7, 14},  {"11.6.14", 8, 14},
    {"11.6.14", 9, 22},  {"11.6.14", 10, 14}, {"11.6.14", 11, 14}, {"11.6.14", 12, 22},
    {"11.6.14", 13, 10}, {"11.6.14", 14, 12}, {"11.6.14", 15, 18}, {"11.6.14", 16, 10},
    {"11.6.14", 17, 12}, {"11.6.14", 18, 18}, {"11.6.14", 19, 12}, {"11.6.14", 20, 12},
    {"11.6.14", 21, 18}, {"11.6.14", 22, 20}, {"11.6.14", 23, 22}, {"11.6.14", 24, 28},
};
static const uint8_t p3d_delta_11613[] = {
    0, 0, 3, 3, 0, 0, 3, 3, 0, 0, 3, 3, 0, 0, 3, 3,
};
static const uint8_t p3d_delta_11614[] = {
    0, 2, 2, 0, 2, 2, 0, 2, 2, 0, 2, 2, 0, 2, 2, 0, 2, 2, 0, 2, 2, 0, 2, 2,
};

static void test_um30_phase3d_input_manifest(void) {
    const p3c_input_t *const tables[] = {p3d_input_11613, p3d_input_11614};
    const size_t counts[] = {16, 24};
    const size_t actual_counts[] = {
        sizeof(p3d_input_11613) / sizeof(p3d_input_11613[0]),
        sizeof(p3d_input_11614) / sizeof(p3d_input_11614[0]),
    };
    const size_t delta_counts[] = {
        sizeof(p3d_delta_11613) / sizeof(p3d_delta_11613[0]),
        sizeof(p3d_delta_11614) / sizeof(p3d_delta_11614[0]),
    };
    size_t total = 0;
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); ++t) {
        assert(actual_counts[t] == counts[t] && delta_counts[t] == counts[t]);
        for (size_t i = 0; i < counts[t]; ++i) {
            assert(tables[t][i].ordinal == i + 1);
            assert(tables[t][i].cycles != 0);
        }
        total += counts[t];
    }
    assert(total == 40);
}

typedef struct {
    const char *row;
    uint16_t words[2];
    uint8_t nwords;
    uint32_t d0, d1;
    unsigned cycles;
} p3d_row_t;

static const uint32_t p3d_effect_hashes[40] = {
    UINT32_C(0x318869bf), UINT32_C(0x318869bf), UINT32_C(0xe19aa423), UINT32_C(0xe19aa423),
    UINT32_C(0x262d6aa6), UINT32_C(0x262d6aa6), UINT32_C(0x3527fc0a), UINT32_C(0x3527fc0a),
    UINT32_C(0x318869bf), UINT32_C(0x318869bf), UINT32_C(0x3527fc0a), UINT32_C(0x3527fc0a),
    UINT32_C(0x262d6aa6), UINT32_C(0x262d6aa6), UINT32_C(0xe19aa423), UINT32_C(0xe19aa423),
    UINT32_C(0x318869bf), UINT32_C(0xe19aa423), UINT32_C(0xe19aa423), UINT32_C(0xb9393a02),
    UINT32_C(0x794a5c64), UINT32_C(0xdf0e3c6f), UINT32_C(0x318869bf), UINT32_C(0x3527fc0a),
    UINT32_C(0xad990cac), UINT32_C(0xb9393a02), UINT32_C(0x82f7d46d), UINT32_C(0x7320d0c0),
    UINT32_C(0x318869bf), UINT32_C(0x01b9a93a), UINT32_C(0x1576fa53), UINT32_C(0x318869bf),
    UINT32_C(0x01b9a93a), UINT32_C(0x1576fa53), UINT32_C(0x318869bf), UINT32_C(0x8515c1a6),
    UINT32_C(0x2cc0eb38), UINT32_C(0xcb01b587), UINT32_C(0xdda7ea10), UINT32_C(0xdda7ea10),
};

static void p3d_assert_semantics(size_t index, const mc68k_state_t *cpu, const uint8_t *ram) {
    switch (index) {
    case 0: /* BTST #0,D0: D0 bit 0 is clear. */
        assert(cpu->d[0] == 0x80 && (cpu->sr & CCR_Z));
        break;
    case 3: /* BTST D0,(A0): bit 0 of 0x01 is set. */
        assert(ram[0x2000] == 0x01 && (cpu->sr & CCR_Z) == 0);
        break;
    case 5: /* BCHG D0,D0. */
        assert(cpu->d[0] == 0x81);
        break;
    case 6: /* BCHG #0,(A0). */
        assert(ram[0x2000] == 0x00);
        break;
    case 14: /* BSET #0,(A0). */
        assert(ram[0x2000] == 0x01);
        break;
    case 13: /* BSET D0,D0. */
        assert(cpu->d[0] == 0x81);
        break;
    case 16: /* BFTST D0{0:8}. */
        assert((cpu->sr & CCR_Z) != 0);
        break;
    case 19: /* BFCHG D0{0:8}. */
        assert(cpu->d[0] == 0xff000080u);
        break;
    case 35: /* BFINS (A0){0:8},D1. */
        assert(ram[0x2000] == 0x00);
        break;
    case 39: /* BFFFO (A0){4:32},D1. */
        assert(cpu->d[1] == 7);
        break;
    default:
        break;
    }
}

static void test_um30_phase3d_opcode_paths(bool emit_effects) {
    static const p3d_row_t rows[] = {
        /* 11.6.13 */
        {"BTST #,Dn", {0x0800, 0}, 2, 0, 0, 4},
        {"BTST Dn,Dn", {0x0100, 0}, 1, 0, 0, 4},
        {"BTST #,Mem", {0x0810, 0}, 2, 0, 0, 7},
        {"BTST Dn,Mem", {0x0110, 0}, 1, 0, 0, 7},
        {"BCHG #,Dn", {0x0840, 0}, 2, 0, 0, 6},
        {"BCHG Dn,Dn", {0x0140, 0}, 1, 0, 0, 6},
        {"BCHG #,Mem", {0x0850, 0}, 2, 0, 0, 9},
        {"BCHG Dn,Mem", {0x0150, 0}, 1, 0, 0, 9},
        {"BCLR #,Dn", {0x0880, 0}, 2, 0, 0, 6},
        {"BCLR Dn,Dn", {0x0180, 0}, 1, 0, 0, 6},
        {"BCLR #,Mem", {0x0890, 0}, 2, 0, 0, 9},
        {"BCLR Dn,Mem", {0x0190, 0}, 1, 0, 0, 9},
        {"BSET #,Dn", {0x08c0, 0}, 2, 0, 0, 6},
        {"BSET Dn,Dn", {0x01c0, 0}, 1, 0, 0, 6},
        {"BSET #,Mem", {0x08d0, 0}, 2, 0, 0, 9},
        {"BSET Dn,Mem", {0x01d0, 0}, 1, 0, 0, 9},
        /* 11.6.14: (A0) rows have CIEA(A0)=2; 4:32 spans five bytes. */
        {"BFTST Dn", {0xe8c0, 0x0008}, 2, 0, 0, 8},
        {"BFTST Mem <5", {0xe8d0, 0x0008}, 2, 0, 0, 12},
        {"BFTST Mem 5", {0xe8d0, 0x0100}, 2, 0, 0, 16},
        {"BFCHG Dn", {0xeac0, 0x0008}, 2, 0, 0, 14},
        {"BFCHG Mem <5", {0xead0, 0x0008}, 2, 0, 0, 16},
        {"BFCHG Mem 5", {0xead0, 0x0100}, 2, 0, 0, 24},
        {"BFCLR Dn", {0xecc0, 0x0008}, 2, 0, 0, 14},
        {"BFCLR Mem <5", {0xecd0, 0x0008}, 2, 0, 0, 16},
        {"BFCLR Mem 5", {0xecd0, 0x0100}, 2, 0, 0, 24},
        {"BFSET Dn", {0xeec0, 0x0008}, 2, 0, 0, 14},
        {"BFSET Mem <5", {0xeed0, 0x0008}, 2, 0, 0, 16},
        {"BFSET Mem 5", {0xeed0, 0x0100}, 2, 0, 0, 24},
        {"BFEXTS Dn", {0xebc0, 0x1008}, 2, 0, 0, 10},
        {"BFEXTS Mem <5", {0xebd0, 0x1008}, 2, 0, 0, 14},
        {"BFEXTS Mem 5", {0xebd0, 0x1100}, 2, 0, 0, 20},
        {"BFEXTU Dn", {0xe9c0, 0x1008}, 2, 0, 0, 10},
        {"BFEXTU Mem <5", {0xe9d0, 0x1008}, 2, 0, 0, 14},
        {"BFEXTU Mem 5", {0xe9d0, 0x1100}, 2, 0, 0, 20},
        {"BFINS Dn", {0xefc0, 0x1008}, 2, 0, 0, 12},
        {"BFINS Mem <5", {0xefd0, 0x1008}, 2, 0, 0, 14},
        {"BFINS Mem 5", {0xefd0, 0x1100}, 2, 0, 0, 20},
        {"BFFFO Dn", {0xedc0, 0x1008}, 2, 0, 0, 20},
        {"BFFFO Mem <5", {0xedd0, 0x1008}, 2, 0, 0, 24},
        {"BFFFO Mem 5", {0xedd0, 0x1100}, 2, 0, 0, 30},
    };
    enum { P3D_OPCODE_CASES = 40 };
    assert(sizeof(rows) / sizeof(rows[0]) == P3D_OPCODE_CASES);
    assert(sizeof(p3d_effect_hashes) / sizeof(p3d_effect_hashes[0]) == P3D_OPCODE_CASES);
    const p3c_input_t *const inputs[] = {p3d_input_11613, p3d_input_11614};
    const uint8_t *const deltas[] = {p3d_delta_11613, p3d_delta_11614};
    const size_t counts[] = {16, 24};
    size_t offset = 0;
    for (size_t t = 0; t < sizeof(counts) / sizeof(counts[0]); ++t) {
        for (size_t i = 0; i < counts[t]; ++i) {
            assert(inputs[t][i].ordinal == i + 1);
            assert(rows[offset + i].cycles == inputs[t][i].cycles + deltas[t][i]);
        }
        offset += counts[t];
    }
    assert(offset == P3D_OPCODE_CASES);
    const mc68k_model_t models[] = {MC68K_MODEL_68030, MC68K_MODEL_68EC030};
    size_t executions = 0;
    for (size_t i = 0; i < P3D_OPCODE_CASES; ++i)
        for (size_t m = 0; m < 2; ++m) {
            ++executions;
            uint8_t *ram = calloc(1, P3_RAM_SIZE);
            mem_ctx_t *mem = mem_create();
            mem_region_t *region = calloc(1, sizeof(*region));
            assert(ram && mem && region);
            region->data = ram;
            region->size = P3_RAM_SIZE;
            mem_add_region(mem, region);
            p3_write_long(ram, 0, P3_SSP);
            p3_write_long(ram, 4, P3_PC);
            ram[0x2000] = 0x01;
            ram[0x2001] = 0x03;
            ram[0x2002] = 0x02;
            ram[0x2003] = 0x03;
            ram[0x2004] = 0x04;
            for (size_t w = 0; w < rows[i].nwords; ++w)
                p3_write_word(ram, P3_PC + 2 * w, rows[i].words[w]);
            mc68k_bus_t bus = bus_make_hooks(mem);
            mc68k_state_t *cpu = mc68k_create_model(&bus, models[m]);
            assert(cpu);
            mc68k_reset(cpu);
            cpu->sr = 0x2704;
            cpu->a[0] = 0x2000;
            cpu->a[1] = 0x2010;
            cpu->d[0] = rows[i].d0 ? rows[i].d0 : 0x80u;
            cpu->d[1] = rows[i].d1;
            int got = mc68k_execute(cpu, 1);
            assert(got == (int)rows[i].cycles);
            assert(cpu->pc == P3_PC + 2u * rows[i].nwords);
            assert(cpu->exec_state == CPU_NORMAL && cpu->a[7] == P3_SSP);
            p3d_assert_semantics(i, cpu, ram);
            uint32_t effect = p3c_effect_hash(cpu, ram);
            if (emit_effects && m == 0)
                printf("3d,%zu,%s,%08x\n", i + 1, rows[i].row, effect);
            if (!emit_effects)
                assert(effect == p3d_effect_hashes[i]);
            mc68k_destroy(cpu);
            mem_destroy(mem);
            free(region);
            free(ram);
        }
    assert(executions == P3D_OPCODE_CASES * (sizeof(models) / sizeof(models[0])));
}

/* UM30 §11.6.15--§11.6.18 I-Cache Case printed bases.  These literals are
 * deliberately kept separate from the production timing classes. */
static const p3c_input_t p3e_input_11615[] = {
    {"11.6.15", 1, 6}, {"11.6.15", 2, 4},  {"11.6.15", 3, 6}, {"11.6.15", 4, 6},
    {"11.6.15", 5, 6}, {"11.6.15", 6, 10}, {"11.6.15", 7, 6},
};
static const p3c_input_t p3e_input_11616[] = {
    {"11.6.16", 1, 12},  {"11.6.16", 2, 12},  {"11.6.16", 3, 12},  {"11.6.16", 4, 12},
    {"11.6.16", 5, 12},  {"11.6.16", 6, 12},  {"11.6.16", 7, 6},   {"11.6.16", 8, 13},
    {"11.6.16", 9, 11},  {"11.6.16", 10, 24}, {"11.6.16", 11, 24}, {"11.6.16", 12, 8},
    {"11.6.16", 13, 28}, {"11.6.16", 14, 8},  {"11.6.16", 15, 28}, {"11.6.16", 16, 18},
    {"11.6.16", 17, 40}, {"11.6.16", 18, 4},  {"11.6.16", 19, 4},  {"11.6.16", 20, 2},
    {"11.6.16", 21, 4},  {"11.6.16", 22, 6},  {"11.6.16", 23, 2},  {"11.6.16", 24, 4},
    {"11.6.16", 25, 10}, {"11.6.16", 26, 12}, {"11.6.16", 27, 9},  {"11.6.16", 28, 5},
};
static const p3c_input_t p3e_input_11617[] = {
    {"11.6.17", 1, 9},  {"11.6.17", 2, 23},  {"11.6.17", 3, 33},  {"11.6.17", 4, 518},
    {"11.6.17", 5, 8},  {"11.6.17", 6, 22},  {"11.6.17", 7, 18},  {"11.6.17", 8, 18},
    {"11.6.17", 9, 18}, {"11.6.17", 10, 18}, {"11.6.17", 11, 18}, {"11.6.17", 12, 22},
    {"11.6.17", 13, 4}, {"11.6.17", 14, 24}, {"11.6.17", 15, 6},  {"11.6.17", 16, 26},
    {"11.6.17", 17, 8}, {"11.6.17", 18, 22}, {"11.6.17", 19, 4},
};
static const p3c_input_t p3e_input_11618[] = {
    {"11.6.18", 1, 36}, {"11.6.18", 2, 62}, {"11.6.18", 3, 18}, {"11.6.18", 4, 18},
    {"11.6.18", 5, 12}, {"11.6.18", 6, 26}, {"11.6.18", 7, 36}, {"11.6.18", 8, 76},
};

static void test_um30_phase3e_input_manifest(void) {
    const p3c_input_t *const tables[] = {p3e_input_11615, p3e_input_11616, p3e_input_11617,
                                         p3e_input_11618};
    const size_t counts[] = {7, 28, 19, 8};
    const size_t actual_counts[] = {
        sizeof(p3e_input_11615) / sizeof(p3e_input_11615[0]),
        sizeof(p3e_input_11616) / sizeof(p3e_input_11616[0]),
        sizeof(p3e_input_11617) / sizeof(p3e_input_11617[0]),
        sizeof(p3e_input_11618) / sizeof(p3e_input_11618[0]),
    };
    size_t total = 0;
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); ++t) {
        assert(actual_counts[t] == counts[t]);
        for (size_t i = 0; i < counts[t]; ++i) {
            assert(tables[t][i].ordinal == i + 1);
            assert(tables[t][i].cycles != 0);
        }
        total += counts[t];
    }
    assert(total == 62);
}

typedef enum {
    P3E_PLAIN,
    P3E_BCC_TAKEN,
    P3E_DB_BRANCH,
    P3E_DB_EXPIRED,
    P3E_DB_TRUE,
    P3E_CAS_OK,
    P3E_CAS_FAIL,
    P3E_CAS2_OK,
    P3E_CAS2_FAIL,
    P3E_CHK_DN_OK,
    P3E_CHK_DN_FAULT,
    P3E_CHK_EA_OK,
    P3E_CHK_EA_FAULT,
    P3E_CHK2_OK,
    P3E_CHK2_FAULT,
    P3E_RTD,
    P3E_RTR,
    P3E_RTS,
    P3E_UNLK,
    P3E_IRQ_I,
    P3E_IRQ_M,
    P3E_STOP,
    P3E_TRACE,
    P3E_TRAP,
    P3E_EXCEPTION,
    P3E_TRAPCC_FALSE,
    P3E_TRAPCC_TRUE,
    P3E_TRAPV_TRUE,
    P3E_BUS_SHORT,
    P3E_BUS_LONG,
    P3E_RTE0,
    P3E_RTE2,
    P3E_RTE_THROW,
    P3E_RTE9,
    P3E_RTE10,
    P3E_RTE11,
} p3e_setup_t;

typedef struct {
    const char *row;
    uint16_t words[4];
    uint8_t nwords;
    uint8_t section;
    uint8_t delta;
    p3e_setup_t setup;
} p3e_row_t;

static uint32_t p3e_hash(const mc68k_state_t *cpu, const uint8_t *ram) {
    /* The hash carries PC, A7 (via p3c_effect_hash), execution state, and
     * exception vector for every ordinal.  Keep these state checks separate
     * from the independent UM30 timing literals. */
    uint32_t hash = p3c_effect_hash(cpu, ram);
    const uint32_t values[] = {cpu->pc, cpu->last_exception_vector, cpu->exec_state};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint32_t value = values[i];
        for (int byte = 0; byte < 4; ++byte) {
            hash = (hash ^ (value & 0xffu)) * UINT32_C(16777619);
            value >>= 8;
        }
    }
    return hash;
}

static void p3e_set_vector(uint8_t *ram, unsigned vector) {
    p3_write_long(ram, vector * 4u, 0x1800);
}

typedef struct {
    mem_ctx_t *mem;
    uint32_t fault_addr;
    unsigned remaining;
} p3e_fault_bus_t;

static bool p3e_should_fault(p3e_fault_bus_t *fault, uint32_t addr) {
    if (addr != fault->fault_addr || fault->remaining == 0)
        return false;
    --fault->remaining;
    return true;
}

static bool p3e_fault_rb(void *ctx, uint32_t addr, uint8_t *out) {
    p3e_fault_bus_t *fault = ctx;
    if (p3e_should_fault(fault, addr))
        return false;
    *out = mem_read_byte(fault->mem, addr);
    return true;
}
static bool p3e_fault_rw(void *ctx, uint32_t addr, uint16_t *out) {
    p3e_fault_bus_t *fault = ctx;
    if (p3e_should_fault(fault, addr))
        return false;
    *out = mem_read_word(fault->mem, addr);
    return true;
}
static bool p3e_fault_rl(void *ctx, uint32_t addr, uint32_t *out) {
    p3e_fault_bus_t *fault = ctx;
    if (p3e_should_fault(fault, addr))
        return false;
    *out = mem_read_long(fault->mem, addr);
    return true;
}
static bool p3e_fault_wb(void *ctx, uint32_t addr, uint8_t value) {
    p3e_fault_bus_t *fault = ctx;
    if (p3e_should_fault(fault, addr))
        return false;
    mem_write_byte(fault->mem, addr, value);
    return true;
}
static bool p3e_fault_ww(void *ctx, uint32_t addr, uint16_t value) {
    p3e_fault_bus_t *fault = ctx;
    if (p3e_should_fault(fault, addr))
        return false;
    mem_write_word(fault->mem, addr, value);
    return true;
}
static bool p3e_fault_wl(void *ctx, uint32_t addr, uint32_t value) {
    p3e_fault_bus_t *fault = ctx;
    if (p3e_should_fault(fault, addr))
        return false;
    mem_write_long(fault->mem, addr, value);
    return true;
}

static void p3e_prepare(const p3e_row_t *row, mc68k_state_t *cpu, uint8_t *ram) {
    for (unsigned vector = 2; vector < 64; ++vector)
        p3e_set_vector(ram, vector);
    for (size_t word = 0; word < row->nwords; ++word)
        p3_write_word(ram, P3_PC + 2u * word, row->words[word]);
    cpu->sr = 0x2700;
    cpu->a[0] = 0x2000;
    cpu->a[1] = 0x2010;
    cpu->a[2] = 0x2020;
    cpu->d[0] = 1;
    cpu->d[1] = 2;
    cpu->d[2] = 3;
    cpu->d[3] = 4;
    p3_write_word(ram, 0x2000, 0x0103);
    p3_write_long(ram, 0x8000, 0x2000);

    switch (row->setup) {
    case P3E_BCC_TAKEN:
        cpu->sr &= ~CCR_Z;
        break;
    case P3E_DB_BRANCH:
        cpu->d[0] = 1;
        break;
    case P3E_DB_EXPIRED:
        cpu->d[0] = 0;
        break;
    case P3E_CAS_OK:
        ram[0x2000] = 1;
        cpu->d[0] = 1;
        cpu->d[1] = 2;
        break;
    case P3E_CAS_FAIL:
        ram[0x2000] = 2;
        cpu->d[0] = 1;
        cpu->d[1] = 3;
        break;
    case P3E_CAS2_OK:
        p3_write_word(ram, 0x2000, 1);
        p3_write_word(ram, 0x2020, 2);
        cpu->d[0] = 1;
        cpu->d[1] = 3;
        cpu->d[2] = 2;
        cpu->d[3] = 4;
        break;
    case P3E_CAS2_FAIL:
        p3_write_word(ram, 0x2000, 7);
        p3_write_word(ram, 0x2020, 2);
        cpu->d[0] = 1;
        cpu->d[1] = 3;
        cpu->d[2] = 2;
        cpu->d[3] = 4;
        break;
    case P3E_CHK_DN_OK:
        cpu->d[0] = 1;
        break;
    case P3E_CHK_DN_FAULT:
        cpu->d[0] = UINT32_C(0xffff);
        break;
    case P3E_CHK_EA_OK:
        cpu->d[0] = 1;
        p3_write_word(ram, 0x2000, 3);
        break;
    case P3E_CHK_EA_FAULT:
        cpu->d[0] = UINT32_C(0xffff);
        p3_write_word(ram, 0x2000, 3);
        break;
    case P3E_CHK2_OK:
        ram[0x2000] = 1;
        ram[0x2001] = 3;
        cpu->d[0] = 2;
        break;
    case P3E_CHK2_FAULT:
        ram[0x2000] = 1;
        ram[0x2001] = 3;
        cpu->d[0] = 7;
        break;
    case P3E_RTD:
    case P3E_RTS:
        p3_write_long(ram, P3_SSP, 0x2000);
        break;
    case P3E_RTR:
        p3_write_word(ram, P3_SSP, 0x0015);
        p3_write_long(ram, P3_SSP + 2, 0x2000);
        break;
    case P3E_UNLK:
        cpu->a[0] = P3_SSP - 4;
        p3_write_long(ram, cpu->a[0], 0x2222);
        break;
    case P3E_IRQ_I:
        cpu->sr = 0x2000;
        mc68k_set_irq(cpu, 3);
        p3e_set_vector(ram, 27);
        break;
    case P3E_IRQ_M:
        cpu_set_sr(cpu, SR_S | SR_M);
        cpu->a[7] = P3_SSP;
        mc68k_set_irq(cpu, 3);
        p3e_set_vector(ram, 27);
        break;
    case P3E_STOP:
        break;
    case P3E_TRACE:
        cpu->sr |= SR_T1;
        p3e_set_vector(ram, 9);
        break;
    case P3E_TRAP:
        p3e_set_vector(ram, 32);
        break;
    case P3E_EXCEPTION:
        break;
    case P3E_TRAPCC_FALSE:
        cpu->sr &= ~CCR_Z;
        break;
    case P3E_TRAPCC_TRUE:
        cpu->sr |= CCR_Z;
        p3e_set_vector(ram, 7);
        break;
    case P3E_TRAPV_TRUE:
        cpu->sr |= CCR_V;
        p3e_set_vector(ram, 7);
        break;
    case P3E_BUS_SHORT:
        p3e_set_vector(ram, 2);
        break;
    case P3E_BUS_LONG:
        p3_write_word(ram, P3_PC, 0x3010); /* MOVE.W (A0),D0 */
        cpu->a[0] = 0x3000;
        p3e_set_vector(ram, 2);
        break;
    case P3E_RTE0:
    case P3E_RTE2:
    case P3E_RTE9:
    case P3E_RTE10:
    case P3E_RTE11:
        cpu->a[7] = P3_SSP - 0x100;
        p3_write_word(ram, cpu->a[7], 0x2700);
        p3_write_long(ram, cpu->a[7] + 2, 0x2000);
        p3_write_word(ram, cpu->a[7] + 6,
                      (uint16_t)((row->setup == P3E_RTE0    ? 0x0
                                  : row->setup == P3E_RTE2  ? 0x2
                                  : row->setup == P3E_RTE9  ? 0x9
                                  : row->setup == P3E_RTE10 ? 0xa
                                                            : 0xb)
                                 << 12));
        break;
    case P3E_RTE_THROW:
        cpu->a[7] = P3_SSP - 0x100;
        cpu->msp = P3_SSP - 0x200;
        p3_write_word(ram, cpu->a[7], SR_S | SR_M);
        p3_write_long(ram, cpu->a[7] + 2, 0x2000);
        p3_write_word(ram, cpu->a[7] + 6, 0x1000);
        p3_write_word(ram, cpu->msp, 0x2700);
        p3_write_long(ram, cpu->msp + 2, 0x2000);
        p3_write_word(ram, cpu->msp + 6, 0);
        break;
    default:
        break;
    }
}

static void p3e_assert_semantics(size_t index, const mc68k_state_t *cpu, const uint8_t *ram) {
    switch (index) {
    case 0: /* Bcc taken */
        assert(cpu->pc == P3_PC + 4 && cpu->a[7] == P3_SSP);
        break;
    case 13: /* BSR */
        assert(cpu->pc == P3_PC + 4 && cpu->a[7] == P3_SSP - 4);
        assert(p3_read_long(ram, P3_SSP - 4) == P3_PC + 2);
        break;
    case 14: /* CAS success */
        assert(ram[0x2000] == 2 && (cpu->sr & CCR_Z));
        break;
    case 19:
    case 21:
    case 23: /* CHK/CHK2 exception paths */
        assert(cpu->pc == 0x1800 && cpu->last_exception_vector == 6);
        assert(cpu->a[7] == P3_SSP - 12);
        break;
    case 25: /* JSR */
        assert(cpu->pc == 0x2000 && cpu->a[7] == P3_SSP - 4);
        assert(p3_read_long(ram, P3_SSP - 4) == P3_PC + 2);
        break;
    case 36:
    case 37: /* I-stack/M-stack interrupt */
        assert(cpu->pc == 0x1800);
        assert(ram[cpu->a[7] + 6] == 0 && ram[cpu->a[7] + 7] == 27 * 4);
        break;
    case 39: /* STOP */
        assert(cpu->exec_state == CPU_STOPPED && cpu->pc == P3_PC + 4);
        break;
    case 41: /* TRAP #0 */
        assert(cpu->pc == 0x1800 && cpu->last_exception_vector == 32);
        break;
    case 54: /* Short bus-cycle fault */
        assert(cpu->pc == 0x1800 && cpu->last_exception_vector == 2);
        assert(cpu->a[7] == P3_SSP - 32);
        break;
    case 55: /* Long bus-cycle fault */
        assert(cpu->pc == 0x1800 && cpu->last_exception_vector == 2);
        assert(cpu->a[7] == P3_SSP - 92);
        break;
    case 56: /* RTE normal four-word */
        assert(cpu->pc == 0x2000 && cpu->a[7] == P3_SSP - 0xf8);
        break;
    case 58: /* Throwaway + selected master-stack frame */
        assert(cpu->pc == 0x2000);
        break;
    default:
        break;
    }
}

static void test_um30_phase3e_opcode_paths(bool emit_effects) {
    static const p3e_row_t rows[] = {
        {"Bcc taken", {0x6602}, 1, 0, 0, P3E_BCC_TAKEN},
        {"Bcc.B not taken", {0x6702}, 1, 0, 0, P3E_PLAIN},
        {"Bcc.W not taken", {0x6700, 0}, 2, 0, 0, P3E_PLAIN},
        {"Bcc.L not taken", {0x67ff, 0, 0}, 3, 0, 0, P3E_PLAIN},
        {"DBcc branch", {0x51c8, 2}, 2, 0, 0, P3E_DB_BRANCH},
        {"DBcc expired", {0x51c8, 2}, 2, 0, 0, P3E_DB_EXPIRED},
        {"DBcc true", {0x50c8, 2}, 2, 0, 0, P3E_DB_TRUE},
        {"ANDI to SR", {0x027c, 0x20ff}, 2, 1, 0, P3E_PLAIN},
        {"EORI to SR", {0x0a7c, 0x0011}, 2, 1, 0, P3E_PLAIN},
        {"ORI to SR", {0x007c, 0x0011}, 2, 1, 0, P3E_PLAIN},
        {"ANDI to CCR", {0x023c, 0x001e}, 2, 1, 0, P3E_PLAIN},
        {"EORI to CCR", {0x0a3c, 0x0011}, 2, 1, 0, P3E_PLAIN},
        {"ORI to CCR", {0x003c, 0x0011}, 2, 1, 0, P3E_PLAIN},
        {"BSR", {0x6102}, 1, 1, 0, P3E_PLAIN},
        {"CAS success", {0x0ad0, 0x0040}, 2, 1, 2, P3E_CAS_OK},
        {"CAS failure", {0x0ad0, 0x0040}, 2, 1, 2, P3E_CAS_FAIL},
        {"CAS2 success", {0x0cfc, 0x8040, 0xa083}, 3, 1, 0, P3E_CAS2_OK},
        {"CAS2 failure", {0x0cfc, 0x8040, 0xa083}, 3, 1, 0, P3E_CAS2_FAIL},
        {"CHK Dn no exception", {0x4180}, 1, 1, 0, P3E_CHK_DN_OK},
        {"CHK Dn exception", {0x4180}, 1, 1, 0, P3E_CHK_DN_FAULT},
        {"CHK EA no exception", {0x4190}, 1, 1, 3, P3E_CHK_EA_OK},
        {"CHK EA exception", {0x4190}, 1, 1, 3, P3E_CHK_EA_FAULT},
        {"CHK2 no exception", {0x00d0, 0x0800}, 2, 1, 3, P3E_CHK2_OK},
        {"CHK2 exception", {0x00d0, 0x0800}, 2, 1, 3, P3E_CHK2_FAULT},
        {"JMP", {0x4ed0}, 1, 1, 2, P3E_PLAIN},
        {"JSR", {0x4e90}, 1, 1, 2, P3E_PLAIN},
        {"LEA", {0x41d0}, 1, 1, 2, P3E_PLAIN},
        {"LINK.W", {0x4e50, 0}, 2, 1, 0, P3E_PLAIN},
        {"LINK.L", {0x4808, 0, 0}, 3, 1, 0, P3E_PLAIN},
        {"NOP", {0x4e71}, 1, 1, 0, P3E_PLAIN},
        {"PEA", {0x4850}, 1, 1, 2, P3E_PLAIN},
        {"RTD", {0x4e74, 0}, 2, 1, 0, P3E_RTD},
        {"RTR", {0x4e77}, 1, 1, 0, P3E_RTR},
        {"RTS", {0x4e75}, 1, 1, 0, P3E_RTS},
        {"UNLK", {0x4e58}, 1, 1, 0, P3E_UNLK},
        {"BKPT", {0x4848}, 1, 2, 0, P3E_EXCEPTION},
        {"Interrupt I-stack", {0x4e71}, 1, 2, 0, P3E_IRQ_I},
        {"Interrupt M-stack", {0x4e71}, 1, 2, 0, P3E_IRQ_M},
        {"RESET", {0x4e70}, 1, 2, 0, P3E_PLAIN},
        {"STOP", {0x4e72, 0x2700}, 2, 2, 0, P3E_STOP},
        {"TRACE", {0x4e71}, 1, 2, 2, P3E_TRACE},
        {"TRAP", {0x4e40}, 1, 2, 0, P3E_TRAP},
        {"Illegal", {0x4afc}, 1, 2, 0, P3E_EXCEPTION},
        {"A-Line", {0xa000}, 1, 2, 0, P3E_EXCEPTION},
        {"F-Line", {0xf000}, 1, 2, 0, P3E_EXCEPTION},
        {"Privilege", {0x4e70}, 1, 2, 0, P3E_EXCEPTION},
        {"TRAPcc", {0x50fc}, 1, 2, 0, P3E_TRAPCC_TRUE},
        {"TRAPcc no trap", {0x51fc}, 1, 2, 0, P3E_TRAPCC_FALSE},
        {"TRAPcc.W", {0x50fa, 0}, 2, 2, 0, P3E_TRAPCC_TRUE},
        {"TRAPcc.W no trap", {0x51fa, 0}, 2, 2, 0, P3E_TRAPCC_FALSE},
        {"TRAPcc.L", {0x50fb, 0, 0}, 3, 2, 0, P3E_TRAPCC_TRUE},
        {"TRAPcc.L no trap", {0x51fb, 0, 0}, 3, 2, 0, P3E_TRAPCC_FALSE},
        {"TRAPV", {0x4e76}, 1, 2, 0, P3E_TRAPV_TRUE},
        {"TRAPV no trap", {0x4e76}, 1, 2, 0, P3E_PLAIN},
        {"Bus fault short", {0}, 0, 3, 0, P3E_BUS_SHORT},
        {"Bus fault long", {0}, 0, 3, 0, P3E_BUS_LONG},
        {"RTE four-word", {0x4e73}, 1, 3, 0, P3E_RTE0},
        {"RTE six-word", {0x4e73}, 1, 3, 0, P3E_RTE2},
        {"RTE throwaway", {0x4e73}, 1, 3, 18, P3E_RTE_THROW},
        {"RTE coprocessor", {0x4e73}, 1, 3, 0, P3E_RTE9},
        {"RTE short fault", {0x4e73}, 1, 3, 0, P3E_RTE10},
        {"RTE long fault", {0x4e73}, 1, 3, 0, P3E_RTE11},
    };
    enum { P3E_OPCODE_CASES = 62 };
    static const uint32_t effect_hashes[P3E_OPCODE_CASES] = {
        UINT32_C(0x499bc49f), UINT32_C(0x7e32ee19), UINT32_C(0x499bc49f), UINT32_C(0xa282e43d),
        UINT32_C(0xca6c6966), UINT32_C(0x7d156480), UINT32_C(0x499bc49f), UINT32_C(0xa3a46746),
        UINT32_C(0x93649826), UINT32_C(0x93649826), UINT32_C(0x499bc49f), UINT32_C(0x93649826),
        UINT32_C(0x93649826), UINT32_C(0xeda3f1da), UINT32_C(0xf1e9e87c), UINT32_C(0x1c8b16c2),
        UINT32_C(0xb80a5ca7), UINT32_C(0xef842d77), UINT32_C(0x7e32ee19), UINT32_C(0xd826d123),
        UINT32_C(0x86359120), UINT32_C(0x4f4e9032), UINT32_C(0x5680c38c), UINT32_C(0x555ebd43),
        UINT32_C(0x66791e8b), UINT32_C(0xd0c697ee), UINT32_C(0x7e32ee19), UINT32_C(0x0dcce787),
        UINT32_C(0x9095d1c5), UINT32_C(0x7e32ee19), UINT32_C(0x6adb079c), UINT32_C(0x3d1fa36f),
        UINT32_C(0x5b507e98), UINT32_C(0x3d1fa36f), UINT32_C(0xa501f755), UINT32_C(0xe99b2f5e),
        UINT32_C(0x84046996), UINT32_C(0x84046996), UINT32_C(0x7e32ee19), UINT32_C(0xa943467d),
        UINT32_C(0x15aba437), UINT32_C(0x204196fa), UINT32_C(0xe99b2f5e), UINT32_C(0xce47fb90),
        UINT32_C(0x2ba95ab1), UINT32_C(0x8b150f52), UINT32_C(0xcf0e65ad), UINT32_C(0x7e32ee19),
        UINT32_C(0xcf0e65ad), UINT32_C(0x499bc49f), UINT32_C(0xcf0e65ad), UINT32_C(0xa282e43d),
        UINT32_C(0x6c700bc3), UINT32_C(0x7e32ee19), UINT32_C(0x92afe3a0), UINT32_C(0xeddea75c),
        UINT32_C(0x620b63e2), UINT32_C(0x0c4f12be), UINT32_C(0x620b63e2), UINT32_C(0x86183b96),
        UINT32_C(0xf3424bca), UINT32_C(0xd758a14e),
    };
    const p3c_input_t *const inputs[] = {p3e_input_11615, p3e_input_11616, p3e_input_11617,
                                         p3e_input_11618};
    const size_t counts[] = {7, 28, 19, 8};
    const mc68k_model_t models[] = {MC68K_MODEL_68030, MC68K_MODEL_68EC030};
    size_t offset = 0, executions = 0;
    assert(sizeof(rows) / sizeof(rows[0]) == P3E_OPCODE_CASES);
    for (size_t t = 0; t < sizeof(counts) / sizeof(counts[0]); ++t) {
        for (size_t i = 0; i < counts[t]; ++i) {
            assert(rows[offset + i].section == t);
            assert(inputs[t][i].ordinal == i + 1);
            assert(inputs[t][i].cycles + rows[offset + i].delta != 0);
        }
        offset += counts[t];
    }
    assert(offset == P3E_OPCODE_CASES);
    for (size_t i = 0; i < P3E_OPCODE_CASES; ++i)
        for (size_t m = 0; m < 2; ++m) {
            uint8_t *ram = calloc(1, P3_RAM_SIZE);
            mem_ctx_t *mem = mem_create();
            mem_region_t *region = calloc(1, sizeof(*region));
            assert(ram && mem && region);
            region->data = ram;
            region->size = P3_RAM_SIZE;
            mem_add_region(mem, region);
            p3_write_long(ram, 0, P3_SSP);
            p3_write_long(ram, 4, P3_PC);
            p3e_fault_bus_t fault = {.mem = mem,
                                     .fault_addr =
                                         rows[i].setup == P3E_BUS_SHORT ? P3_PC : 0x3000,
                                     .remaining = 1};
            mc68k_bus_t bus = bus_make_hooks(mem);
            if (rows[i].setup == P3E_BUS_SHORT || rows[i].setup == P3E_BUS_LONG) {
                bus.read_byte = p3e_fault_rb;
                bus.read_word = p3e_fault_rw;
                bus.read_long = p3e_fault_rl;
                bus.write_byte = p3e_fault_wb;
                bus.write_word = p3e_fault_ww;
                bus.write_long = p3e_fault_wl;
                bus.ctx = &fault;
            }
            mc68k_state_t *cpu = mc68k_create_model(&bus, models[m]);
            assert(cpu);
            mc68k_reset(cpu);
            p3e_prepare(&rows[i], cpu, ram);
            if (strcmp(rows[i].row, "Privilege") == 0)
                cpu->sr &= ~SR_S;
            ++executions;
            int got = mc68k_execute(cpu, 1);
            unsigned want = inputs[rows[i].section][i - (rows[i].section == 0   ? 0
                                                         : rows[i].section == 1 ? 7
                                                         : rows[i].section == 2 ? 35
                                                                                : 54)]
                                .cycles +
                            rows[i].delta;
            if (got != (int)want)
                fprintf(stderr,
                        "%s model %d: %d != %u (pc=%x vec=%u state=%d fault=%d kind=%d)\n",
                        rows[i].row, models[m], got, want, cpu->pc, cpu->last_exception_vector,
                        cpu->exec_state, cpu->fault_vector, cpu->fault_access_kind);
            assert(got == (int)want);
            if (rows[i].setup == P3E_STOP)
                assert(cpu->exec_state == CPU_STOPPED);
            else
                assert(cpu->exec_state == CPU_NORMAL);
            if (rows[i].setup == P3E_CHK_DN_FAULT || rows[i].setup == P3E_CHK_EA_FAULT ||
                rows[i].setup == P3E_CHK2_FAULT)
                assert(cpu->last_exception_vector == 6 && cpu->pc == 0x1800);
            if (rows[i].setup == P3E_CAS_OK)
                assert(ram[0x2000] == 2);
            if (rows[i].setup == P3E_CAS_FAIL)
                assert(cpu->d[0] == 2);
            if (strcmp(rows[i].row, "JSR") == 0)
                assert(cpu->pc == 0x2000 && cpu->a[7] == P3_SSP - 4);
            if (rows[i].setup == P3E_RTE0 || rows[i].setup == P3E_RTE2 ||
                rows[i].setup == P3E_RTE_THROW || rows[i].setup == P3E_RTE9 ||
                rows[i].setup == P3E_RTE10 || rows[i].setup == P3E_RTE11)
                assert(cpu->pc == 0x2000);
            p3e_assert_semantics(i, cpu, ram);
            uint32_t effect = p3e_hash(cpu, ram);
            if (emit_effects && m == 0)
                printf("3e,%zu,%s,%08x\n", i + 1, rows[i].row, effect);
            if (!emit_effects)
                assert(effect == effect_hashes[i]);
            if (rows[i].setup == P3E_BUS_LONG) {
                /* A failed JSR stack write returns the distinct sentinel 8.
                 * The one-shot callback lets group-0 construct its frame after
                 * that write failure, proving the UM30 total is sentinel-free. */
                mc68k_reset(cpu);
                cpu->sr = 0x2700;
                cpu->a[0] = P3_PC;
                p3_write_word(ram, P3_PC, 0x4e90); /* JSR (A0) */
                fault.fault_addr = P3_SSP - 4;
                fault.remaining = 1;
                assert(mc68k_execute(cpu, 1) == 62);
                assert(cpu->pc == 0x1800 && cpu->last_exception_vector == 2);
            }
            mc68k_destroy(cpu);
            mem_destroy(mem);
            free(region);
            free(ram);
        }
    assert(executions == P3E_OPCODE_CASES * (sizeof(models) / sizeof(models[0])));
}

/* Every §11.6.16 EA-marked row gets one legal full direct (bd.W,An) opcode.
 * The totals below are independent literals: CIEA/FIEA=8 and FEA/CEA/JEA=6
 * for this descriptor.  JMP's direct and indirect full forms are covered by
 * test_um30_jump_opcode_paths(). */
static void test_um30_phase3e_full_ea_paths(void) {
    static const p3e_row_t rows[] = {
        {"CAS success full", {0x0af0, 0x0040, 0x0160, 0x0010}, 4, 1, 8, P3E_CAS_OK},
        {"CAS failure full", {0x0af0, 0x0040, 0x0160, 0x0010}, 4, 1, 8, P3E_CAS_FAIL},
        {"CHK EA normal full", {0x41b0, 0x0160, 0x0010}, 3, 1, 6, P3E_CHK_EA_OK},
        {"CHK EA exception full", {0x41b0, 0x0160, 0x0010}, 3, 1, 6, P3E_CHK_EA_FAULT},
        {"CHK2 normal full", {0x00f0, 0x0800, 0x0160, 0x0010}, 4, 1, 8, P3E_CHK2_OK},
        {"CHK2 exception full", {0x00f0, 0x0800, 0x0160, 0x0010}, 4, 1, 8, P3E_CHK2_FAULT},
        {"JSR full", {0x4eb0, 0x0160, 0x0010}, 3, 1, 6, P3E_PLAIN},
        {"LEA full", {0x41f0, 0x0160, 0x0010}, 3, 1, 6, P3E_PLAIN},
        {"PEA full", {0x4870, 0x0160, 0x0010}, 3, 1, 6, P3E_PLAIN},
    };
    enum { P3E_FULL_EA_CASES = 9 };
    const unsigned bases[] = {13, 11, 8, 28, 18, 40, 4, 2, 4};
    const mc68k_model_t models[] = {MC68K_MODEL_68030, MC68K_MODEL_68EC030};
    size_t executions = 0;
    assert(sizeof(rows) / sizeof(rows[0]) == P3E_FULL_EA_CASES);
    assert(sizeof(bases) / sizeof(bases[0]) == P3E_FULL_EA_CASES);
    for (size_t i = 0; i < P3E_FULL_EA_CASES; ++i)
        for (size_t m = 0; m < 2; ++m) {
            uint8_t *ram = calloc(1, P3_RAM_SIZE);
            mem_ctx_t *mem = mem_create();
            mem_region_t *region = calloc(1, sizeof(*region));
            assert(ram && mem && region);
            region->data = ram;
            region->size = P3_RAM_SIZE;
            mem_add_region(mem, region);
            p3_write_long(ram, 0, P3_SSP);
            p3_write_long(ram, 4, P3_PC);
            mc68k_bus_t bus = bus_make_hooks(mem);
            mc68k_state_t *cpu = mc68k_create_model(&bus, models[m]);
            assert(cpu);
            mc68k_reset(cpu);
            p3e_prepare(&rows[i], cpu, ram);
            for (size_t word = 0; word < rows[i].nwords; ++word)
                p3_write_word(ram, P3_PC + 2u * word, rows[i].words[word]);
            if (rows[i].setup == P3E_CAS_OK || rows[i].setup == P3E_CAS_FAIL)
                ram[0x2010] = ram[0x2000];
            if (rows[i].setup == P3E_CHK_EA_OK || rows[i].setup == P3E_CHK_EA_FAULT)
                p3_write_word(ram, 0x2010, 3);
            if (rows[i].setup == P3E_CHK2_OK || rows[i].setup == P3E_CHK2_FAULT) {
                ram[0x2010] = 1;
                ram[0x2011] = 3;
            }
            ++executions;
            int got = mc68k_execute(cpu, 1);
            assert(got == (int)(bases[i] + rows[i].delta));
            assert(cpu->exec_state == CPU_NORMAL);
            switch (i) {
            case 0:
                assert(ram[0x2010] == 2);
                break;
            case 1:
                assert(cpu->d[0] == 2);
                break;
            case 3:
            case 5:
                assert(cpu->pc == 0x1800 && cpu->last_exception_vector == 6);
                assert(cpu->a[7] == P3_SSP - 12);
                break;
            case 6:
                assert(cpu->pc == 0x2010 && cpu->a[7] == P3_SSP - 4);
                assert(p3_read_long(ram, P3_SSP - 4) == P3_PC + 6);
                break;
            case 7:
                assert(cpu->a[0] == 0x2010 && cpu->pc == P3_PC + 6);
                break;
            case 8:
                assert(cpu->a[7] == P3_SSP - 4 && cpu->pc == P3_PC + 6);
                assert(p3_read_long(ram, P3_SSP - 4) == 0x2010);
                break;
            default:
                break;
            }
            mc68k_destroy(cpu);
            mem_destroy(mem);
            free(region);
            free(ram);
        }
    assert(executions == P3E_FULL_EA_CASES * (sizeof(models) / sizeof(models[0])));
}

int main(int argc, char **argv) {
    bool emit_effects = argc == 2 && strcmp(argv[1], "--emit-effects") == 0;
    test_input_inventory();
    test_um30_ea_tables();
    test_um30_jump_opcode_paths();
    test_um30_move_opcode_paths();
    test_um30_move_full_destination_manifest();
    test_um30_phase3c_input_manifest();
    test_um30_phase3c_opcode_paths(emit_effects);
    test_um30_phase3d_input_manifest();
    test_um30_phase3d_opcode_paths(emit_effects);
    test_um30_phase3e_input_manifest();
    test_um30_phase3e_opcode_paths(emit_effects);
    test_um30_phase3e_full_ea_paths();
    return 0;
}
