/* Independent UM20 Cache Case checks for review-sensitive Phase 2c paths. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include "memory/bus.h"

#define RAM_SIZE 0x10000u
#define PC       0x1000u

static void w16(uint8_t *p, uint32_t a, uint16_t v) {
    p[a] = v >> 8;
    p[a + 1] = v;
}
static void w32(uint8_t *p, uint32_t a, uint32_t v) {
    w16(p, a, v >> 16);
    w16(p, a + 2, v);
}
static uint32_t r32(const uint8_t *p, uint32_t a) {
    return ((uint32_t)p[a] << 24) | ((uint32_t)p[a + 1] << 16) | ((uint32_t)p[a + 2] << 8) |
           p[a + 3];
}

static int run(uint16_t op, uint16_t ext, uint32_t d0, uint32_t a0) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    w16(ram, PC, op);
    w16(ram, PC + 2, ext);
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
    int cycles = mc68k_execute(cpu, 1);
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
    return cycles;
}

typedef struct {
    int cycles;
    uint32_t d1;
    uint32_t a1;
    uint16_t word_2000;
} basic_move_result_t;

static basic_move_result_t run_basic_move(const uint16_t *words, size_t count, uint32_t d0,
                                          uint32_t a0) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    for (size_t i = 0; i < count; ++i)
        w16(ram, PC + (uint32_t)(i * 2), words[i]);
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
    cpu->d[1] = 0xabcd0000;
    cpu->a[0] = a0;
    cpu->a[1] = 0x11111111;
    basic_move_result_t result = {
        .cycles = mc68k_execute(cpu, 1),
        .d1 = cpu->d[1],
        .a1 = cpu->a[1],
        .word_2000 = (uint16_t)((ram[0x2000] << 8) | ram[0x2001]),
    };
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
    return result;
}

typedef struct {
    int cycles;
    uint16_t word_3000;
    uint16_t word_3500;
} move_result_t;

static move_result_t run_move_words(const uint16_t *words, size_t count) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    for (size_t i = 0; i < count; ++i)
        w16(ram, PC + (uint32_t)(i * 2), words[i]);
    /* Direct and memory-indirect full extensions use distinct pointer and
     * operand locations, so a missing indirect read cannot pass by chance. */
    w16(ram, 0x2000, 0x1234);
    w16(ram, 0x2500, 0x5678);
    if ((words[0] & 0x0038) == 0x0030 && (words[1] & 7) != 0)
        w32(ram, 0x2000, 0x2500);
    w16(ram, 0x3000, 0);
    w32(ram, 0x3000, 0x3500);
    w16(ram, 0x3500, 0);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->d[0] = 0x9abc;
    cpu->d[7] = 0;
    cpu->a[0] = 0x2000;
    cpu->a[1] = 0x3000;
    move_result_t result = {.cycles = mc68k_execute(cpu, 1),
                            .word_3000 = (uint16_t)((ram[0x3000] << 8) | ram[0x3001]),
                            .word_3500 = (uint16_t)((ram[0x3500] << 8) | ram[0x3501])};
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
    return result;
}

static void test_move_basic_ea_rows(void) {
    /* UM20 §8.2.6: PC d16/indexed source and indexed destination. */
    assert(run(0x303A, 0, 0, 0) == 7);      /* (d16,PC) -> D0 */
    assert(run(0x303B, 0, 0, 0) == 9);      /* (d8,PC,Xn) -> D0 */
    assert(run(0x3180, 0, 0, 0x2000) == 7); /* D0 -> (d8,A0,Xn) */
    assert(run(0x3390, 0, 0, 0x2000) == 9); /* (A0) -> (d8,A1,Xn) */
    assert(run(0x31C0, 0x2000, 0, 0) == 4); /* D0 -> (xxx).W */
}

static void test_move_full_extension_rows(void) {
    /* UM20 §8.2.6.  The expected values are copied from the full 23x22
     * Cache Case matrix, not derived through ea_cycles(). */
    static const uint16_t source_bd_word[] = {
        0x33f0, 0x7120, 0x0000, 0x0000, 0x3000,
    }; /* (d16,A0,Xn) -> (xxx).L: IX_BD_W x AL = 12 */
    static const uint16_t source_indirect[] = {
        0x33f0,
        0x7111,
        0x0000,
        0x3000,
    }; /* ([A0],Xn) -> (xxx).L: IND_B_NULL x AL = 17 */
    static const uint16_t destination_bd_word[] = {
        0x3380,
        0x7120,
        0x0000,
    }; /* D0 -> (d16,A1,Xn): RN x IX_BD_W = 9 */
    static const uint16_t destination_indirect[] = {
        0x3380,
        0x7111,
    }; /* D0 -> ([A1],Xn): RN x IND_B_NULL = 12 */
    move_result_t result =
        run_move_words(source_bd_word, sizeof(source_bd_word) / sizeof(source_bd_word[0]));
    assert(result.cycles == 12);
    assert(result.word_3000 == 0x1234);
    result =
        run_move_words(source_indirect, sizeof(source_indirect) / sizeof(source_indirect[0]));
    assert(result.cycles == 17);
    assert(result.word_3000 == 0x5678);
    result = run_move_words(destination_bd_word,
                            sizeof(destination_bd_word) / sizeof(destination_bd_word[0]));
    assert(result.cycles == 9);
    assert(result.word_3000 == 0x9abc);
    result = run_move_words(destination_indirect,
                            sizeof(destination_indirect) / sizeof(destination_indirect[0]));
    assert(result.cycles == 12);
    assert(result.word_3500 == 0x9abc);
}

/* Independent UM20 8.2.6 Cache Case matrix.  This deliberately does not
 * share the production selector or its table. */
static const uint8_t move_matrix[23][22] = {
    {2, 2, 4, 4, 5, 5, 4, 6, 7, 9, 8, 10, 14, 12, 14, 15, 14, 16, 17, 18, 20, 21},
    {4, 4, 6, 6, 7, 7, 6, 8, 7, 9, 8, 10, 14, 12, 14, 15, 14, 16, 17, 18, 20, 21},
    {6, 6, 8, 8, 9, 9, 8, 10, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {7, 7, 8, 8, 8, 8, 8, 10, 10, 12, 11, 13, 17, 15, 17, 18, 17, 19, 20, 21, 23, 24},
    {7, 7, 8, 8, 8, 8, 8, 10, 10, 12, 11, 13, 17, 15, 17, 18, 17, 19, 20, 21, 23, 24},
    {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {9, 9, 10, 10, 10, 10, 10, 12, 12, 14, 13, 15, 19, 17, 19, 20, 19, 21, 22, 23, 25, 26},
    {9, 9, 10, 10, 10, 10, 10, 12, 12, 14, 13, 15, 19, 17, 19, 20, 19, 21, 22, 23, 25, 26},
    {9, 9, 10, 10, 10, 10, 10, 12, 12, 14, 13, 15, 19, 17, 19, 20, 19, 21, 22, 23, 25, 26},
    {11, 11, 12, 12, 12, 12, 12, 14, 14, 16, 15, 17, 21, 19, 21, 22, 21, 23, 24, 25, 27, 28},
    {15, 15, 16, 16, 16, 16, 16, 18, 18, 20, 19, 21, 25, 23, 25, 26, 25, 27, 28, 29, 31, 32},
    {14, 14, 15, 15, 15, 15, 15, 17, 17, 19, 18, 20, 24, 22, 24, 25, 24, 26, 27, 28, 30, 31},
    {16, 16, 17, 17, 17, 17, 17, 19, 19, 21, 20, 22, 26, 24, 26, 27, 26, 28, 29, 30, 32, 33},
    {16, 16, 17, 17, 17, 17, 17, 19, 19, 21, 20, 22, 26, 24, 26, 27, 26, 28, 29, 30, 32, 33},
    {16, 16, 17, 17, 17, 17, 17, 19, 19, 21, 20, 22, 26, 24, 26, 27, 26, 28, 29, 30, 32, 33},
    {18, 18, 19, 19, 19, 19, 19, 21, 21, 23, 22, 24, 28, 26, 28, 29, 28, 30, 31, 32, 34, 35},
    {18, 18, 19, 19, 19, 19, 19, 21, 21, 23, 22, 24, 28, 26, 28, 29, 28, 30, 31, 32, 34, 35},
    {20, 20, 21, 21, 21, 21, 21, 23, 23, 25, 24, 26, 30, 28, 30, 31, 30, 32, 33, 34, 36, 37},
    {22, 22, 23, 23, 23, 23, 23, 25, 25, 27, 26, 28, 32, 30, 32, 33, 32, 34, 35, 36, 38, 39},
    {22, 22, 23, 23, 23, 23, 23, 25, 25, 27, 26, 28, 32, 30, 32, 33, 32, 34, 35, 36, 38, 39},
};

static int emit_full(unsigned row, uint32_t base, uint16_t *words, int *n) {
    if (row == 9) {
        words[(*n)++] = 0x7000;
        return 0;
    }
    if (row == 10) {
        words[(*n)++] = 0x7120;
        words[(*n)++] = 0;
        return 0;
    }
    if (row == 11) {
        words[(*n)++] = 0x7110;
        return 0;
    }
    if (row == 12) {
        words[(*n)++] = 0x71a0;
        words[(*n)++] = (uint16_t)base;
        return 0;
    }
    if (row == 13) {
        words[(*n)++] = 0x7130;
        words[(*n)++] = 0;
        words[(*n)++] = 0;
        return 0;
    }
    unsigned x = row - 14, bd = x / 3 + 1, od = x % 3 + 1;
    words[(*n)++] = (uint16_t)(0x7100 | (bd << 4) | od);
    if (bd == 2)
        words[(*n)++] = 0;
    if (bd == 3) {
        words[(*n)++] = 0;
        words[(*n)++] = 0;
    }
    if (od == 2)
        words[(*n)++] = 0;
    if (od == 3) {
        words[(*n)++] = 0;
        words[(*n)++] = 0;
    }
    return 1;
}

static int emit_source(unsigned row, int size, uint16_t *words, int *n, int *mode, int *reg) {
    if (row == 0) {
        *mode = 0;
        *reg = 0;
        return 0;
    }
    if (row == 1 || row == 2) {
        *mode = 7;
        *reg = 4;
        words[(*n)++] = 0x1234;
        if (size == 4)
            words[(*n)++] = 0x5678;
        return 0;
    }
    if (row >= 3 && row <= 6) {
        *mode = (int)row - 1;
        *reg = 0;
        if (row == 6)
            words[(*n)++] = 0;
        return 0;
    }
    if (row == 7) {
        *mode = 7;
        *reg = 0;
        words[(*n)++] = 0x2200;
        return 0;
    }
    if (row == 8) {
        *mode = 7;
        *reg = 1;
        words[(*n)++] = 0;
        words[(*n)++] = 0x2200;
        return 0;
    }
    *mode = 6;
    *reg = 0;
    return emit_full(row, 0x2000, words, n);
}

static int emit_destination(unsigned col, uint16_t *words, int *n, int *mode, int *reg) {
    if (col == 0) {
        *mode = 1;
        *reg = 1;
        return 0;
    }
    if (col == 1) {
        *mode = 0;
        *reg = 1;
        return 0;
    }
    if (col <= 5) {
        *mode = (int)col;
        *reg = 1;
        if (col == 5)
            words[(*n)++] = 0;
        return 0;
    }
    if (col == 6) {
        *mode = 7;
        *reg = 0;
        words[(*n)++] = 0x3200;
        return 0;
    }
    if (col == 7) {
        *mode = 7;
        *reg = 1;
        words[(*n)++] = 0;
        words[(*n)++] = 0x3200;
        return 0;
    }
    *mode = 6;
    *reg = 1;
    return emit_full(col + 1, 0x3000, words, n);
}

typedef struct {
    int cycles;
    uint32_t value;
} matrix_result_t;

static matrix_result_t run_move_matrix_cell(unsigned row, unsigned col) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    uint16_t words[16];
    int n = 1, sm, sr, dm, dr;
    int source_indirect = emit_source(row, row == 2 ? 4 : 2, words, &n, &sm, &sr);
    int destination_indirect = emit_destination(col, words, &n, &dm, &dr);
    words[0] =
        (uint16_t)((row == 2 ? 0x2000 : 0x3000) | (dr << 9) | (dm << 6) | (sm << 3) | sr);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    for (int i = 0; i < n; ++i)
        w16(ram, PC + (uint32_t)(i * 2), words[i]);
    w32(ram, 0x1ffe, 0x5a5a5a5a);
    w32(ram, 0x2000, 0x5a5a5a5a);
    w32(ram, 0x2200, 0x5a5a5a5a);
    w32(ram, 0x2400, 0x5a5a5a5a);
    if (source_indirect)
        w32(ram, 0x2000, 0x2400);
    if (destination_indirect)
        w32(ram, 0x3000, 0x3400);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->d[0] = 0x9abc;
    cpu->d[7] = 0;
    cpu->a[0] = 0x2000;
    cpu->a[1] = 0x3000;
    int cycles = mc68k_execute(cpu, 1);
    uint32_t value;
    if (col == 0)
        value = cpu->a[1];
    else if (col == 1)
        value = cpu->d[1];
    else {
        uint32_t address = col == 4               ? (row == 2 ? 0x2ffcu : 0x2ffeu)
                           : col == 6 || col == 7 ? 0x3200
                           : col >= 13            ? (destination_indirect ? 0x3400 : 0x3000)
                                                  : 0x3000;
        value = row == 2 ? r32(ram, address) : ((uint32_t)ram[address] << 8) | ram[address + 1];
    }
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
    return (matrix_result_t){cycles, value};
}

static void test_move_matrix(void) {
    assert(sizeof(move_matrix) / sizeof(move_matrix[0]) == 23);
    for (unsigned row = 0; row < 23; ++row)
        for (unsigned col = 0; col < 22; ++col) {
            matrix_result_t actual = run_move_matrix_cell(row, col);
            uint32_t expected = row == 0   ? 0x9abcu
                                : row == 1 ? 0x1234u
                                : row == 2 ? 0x12345678u
                                           : 0x5a5au;
            if (col == 0 && row != 2)
                expected = (uint32_t)(int32_t)(int16_t)expected;
            assert(actual.cycles == move_matrix[row][col]);
            if (actual.value != expected)
                fprintf(stderr, "MOVE value mismatch row=%u col=%u actual=%08X expected=%08X\n",
                        row, col, actual.value, expected);
            assert(actual.value == expected);
        }
}

static uint16_t r16(const uint8_t *p, uint32_t a) {
    return (uint16_t)((p[a] << 8) | p[a + 1]);
}

static unsigned alias_source_row(unsigned bd, unsigned iis, unsigned bs) {
    if (iis == 0) {
        if (bd == 1)
            return 11;
        if (bd == 2)
            return bs ? 12 : 10;
        return 13;
    }
    return 14 + (bd - 1) * 3 + ((iis - 1) % 4);
}

static unsigned alias_destination_column(unsigned bd, unsigned iis, unsigned bs) {
    if (iis == 0) {
        if (bd == 1)
            return 10;
        if (bd == 2)
            return bs ? 11 : 9;
        return 12;
    }
    return 13 + (bd - 1) * 3 + ((iis - 1) % 4);
}

static unsigned append_alias_extension(uint16_t *words, unsigned n, unsigned bd, unsigned iis,
                                       unsigned bs, unsigned is) {
    words[n++] = (uint16_t)(0x0100 | (bs << 7) | (is << 6) | (bd << 4) | iis);
    if (bd == 2)
        words[n++] = 0;
    else if (bd == 3) {
        words[n++] = 0;
        words[n++] = 0;
    }
    if ((iis & 3) == 2)
        words[n++] = 0;
    else if ((iis & 3) == 3) {
        words[n++] = 0;
        words[n++] = 0;
    }
    return n;
}

static uint32_t alias_index_value(unsigned is) {
    return is ? 0 : 0x1234;
}

static uint32_t alias_final_address(uint8_t *ram, uint32_t base, uint32_t index, unsigned iis,
                                    unsigned bs, unsigned is, unsigned bd,
                                    uint32_t extension_address, unsigned word_count) {
    uint32_t base_bd = bs ? 0 : base;
    uint32_t indexed = is ? 0 : index;
    unsigned bd_words = bd == 1 ? 0 : bd - 1;
    uint32_t bd_value = 0;
    if (bd_words == 1)
        bd_value = (uint32_t)(int32_t)(int16_t)r16(ram, extension_address + 2);
    else if (bd_words == 2)
        bd_value = r32(ram, extension_address + 2);
    base_bd += bd_value;
    if (iis == 0)
        return base_bd + indexed;

    uint32_t pointer_address = iis >= 5 ? base_bd : base_bd + indexed;
    uint32_t pointer;
    uint32_t code_end = PC + word_count * 2;
    if (pointer_address >= PC && pointer_address < code_end)
        pointer = r32(ram, pointer_address);
    else {
        pointer = 0x010800u;
        w32(ram, pointer_address, pointer);
    }
    return pointer + (iis >= 5 ? indexed : 0);
}

static void run_move_selector_alias(unsigned group, unsigned bd, unsigned iis, unsigned bs,
                                    unsigned is) {
    enum { ALIAS_RAM_SIZE = 0x02000000 };
    uint8_t *ram = calloc(1, ALIAS_RAM_SIZE);
    assert(ram);
    uint16_t words[8] = {0};
    uint32_t base = group == 0 ? 0x2000u : group == 1 ? PC + 2 : 0x3000u;
    words[0] = group == 0 ? 0x3230 : group == 1 ? 0x323b : 0x3380;
    unsigned word_count = append_alias_extension(words, 1, bd, iis, bs, is);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    for (unsigned i = 0; i < word_count; ++i)
        w16(ram, PC + i * 2, words[i]);

    uint32_t final_address = alias_final_address(ram, base, alias_index_value(is), iis, bs, is,
                                                 bd, PC + 2, word_count);
    uint16_t expected_value = 0x5aa5;
    if (group != 2 && !(final_address >= PC && final_address < PC + word_count * 2))
        w16(ram, final_address, expected_value);
    else if (group != 2)
        expected_value = r16(ram, final_address);

    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = ALIAS_RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->d[0] = 0x1234;
    cpu->a[0] = 0x2000;
    cpu->a[1] = 0x3000;
    int cycles = mc68k_execute(cpu, 1);
    unsigned source = alias_source_row(bd, iis, bs);
    unsigned destination = alias_destination_column(bd, iis, bs);
    int expected_cycles = group == 2 ? move_matrix[0][destination] : move_matrix[source][1];
    if (cycles != expected_cycles)
        fprintf(stderr,
                "MOVE alias cycle mismatch group=%u bd=%u iis=%u bs=%u is=%u actual=%d "
                "expected=%d\n",
                group, bd, iis, bs, is, cycles, expected_cycles);
    assert(cycles == expected_cycles);
    if (group == 2)
        assert(r16(ram, final_address) == 0x1234);
    else
        assert((uint16_t)cpu->d[1] == expected_value);
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
}

static void test_move_basic_selector_aliases(void) {
    static const uint16_t an_word[] = {0x3208};
    static const uint16_t an_long[] = {0x2208};
    static const uint16_t movea_d0[] = {0x3240};
    static const uint16_t movea_a0[] = {0x3248};
    static const uint16_t immediate_byte[] = {0x123c, 0x005a};
    static const uint16_t immediate_word[] = {0x323c, 0x5aa5};
    static const uint16_t immediate_long[] = {0x223c, 0x1234, 0x5678};
    static const uint16_t absolute_word[] = {0x31c0, 0x2000};
    basic_move_result_t result = run_basic_move(an_word, 1, 0, 0x00001234);
    assert(result.cycles == 2);
    assert(result.d1 == 0xabcd1234);
    result = run_basic_move(an_long, 1, 0, 0x12345678);
    assert(result.cycles == 2);
    assert(result.d1 == 0x12345678);
    result = run_basic_move(movea_d0, 1, 0x0000ffff, 0);
    assert(result.cycles == 2);
    assert(result.a1 == 0xffffffff);
    result = run_basic_move(movea_a0, 1, 0, 0x00008001);
    assert(result.cycles == 2);
    assert(result.a1 == 0xffff8001);
    result = run_basic_move(immediate_byte, 2, 0, 0);
    assert(result.cycles == 4);
    assert(result.d1 == 0xabcd005a);
    result = run_basic_move(immediate_word, 2, 0, 0);
    assert(result.cycles == 4);
    assert(result.d1 == 0xabcd5aa5);
    result = run_basic_move(immediate_long, 3, 0, 0);
    assert(result.cycles == 6);
    assert(result.d1 == 0x12345678);
    result = run_basic_move(absolute_word, 2, 0x00001234, 0);
    assert(result.cycles == 4);
    assert(result.word_2000 == 0x1234);
}

static void test_move_selector_aliases(void) {
    static const unsigned iis_values[] = {0, 1, 2, 3, 5, 6, 7};
    unsigned cases = 0;
    unsigned ext_0160_seen = 0;
    for (unsigned group = 0; group < 3; ++group) {
        for (unsigned bd = 1; bd <= 3; ++bd) {
            for (unsigned i = 0; i < sizeof(iis_values) / sizeof(iis_values[0]); ++i) {
                unsigned iis = iis_values[i];
                for (unsigned bs = 0; bs <= 1; ++bs) {
                    unsigned max_is = iis >= 5 ? 0 : 1;
                    for (unsigned is = 0; is <= max_is; ++is) {
                        if (bd == 2 && iis == 0 && bs == 0 && is == 1)
                            ++ext_0160_seen;
                        run_move_selector_alias(group, bd, iis, bs, is);
                        ++cases;
                    }
                }
            }
        }
    }
    assert(cases == 198);
    assert(ext_0160_seen == 3);
}

static void test_move_index_selector_aliases(void) {
    static const struct {
        uint16_t extension;
        uint32_t d6;
        uint32_t a6;
        uint32_t offset;
    } cases[] = {
        {0x6110, 0xffff0001u, 0, 1}, /* D6.W ×1 */
        {0x6b10, 2, 0, 4},           /* D6.L ×2 */
        {0xe510, 0, 3, 12},          /* A6.W ×4 */
        {0xef10, 0, 2, 16},          /* A6.L ×8 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t *ram = calloc(1, RAM_SIZE);
        assert(ram);
        w32(ram, 0, 0x8000);
        w32(ram, 4, PC);
        w16(ram, PC, 0x3230);
        w16(ram, PC + 2, cases[i].extension);
        w16(ram, 0x2000 + cases[i].offset, (uint16_t)(0x5100 + i));
        mem_ctx_t *mem = mem_create();
        mem_region_t *region = calloc(1, sizeof(*region));
        assert(mem && region);
        region->data = ram;
        region->size = RAM_SIZE;
        mem_add_region(mem, region);
        mc68k_bus_t bus = bus_make_hooks(mem);
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
        mc68k_reset(cpu);
        cpu->a[0] = 0x2000;
        cpu->d[6] = cases[i].d6;
        cpu->a[6] = cases[i].a6;
        assert(mc68k_execute(cpu, 1) == move_matrix[11][1]);
        if ((uint16_t)cpu->d[1] != (uint16_t)(0x5100 + i))
            fprintf(stderr, "MOVE index alias mismatch ext=%04X actual=%04X expected=%04X\n",
                    cases[i].extension, (uint16_t)cpu->d[1], (uint16_t)(0x5100 + i));
        assert((uint16_t)cpu->d[1] == (uint16_t)(0x5100 + i));
        mc68k_destroy(cpu);
        mem_destroy(mem);
        free(region);
        free(ram);
    }
}

typedef struct {
    int cycles;
    uint32_t d0;
    uint8_t bytes[8];
} movep_result_t;

static movep_result_t run_movep(uint16_t opcode, uint32_t d0) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    w16(ram, PC, opcode);
    w16(ram, PC + 2, 0);
    for (unsigned i = 0; i < 8; ++i)
        ram[0x2000 + i] = (i & 1) ? 0xee : (uint8_t)(0x12 + i * 0x11);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->a[0] = 0x2000;
    cpu->d[0] = d0;
    movep_result_t result = {.cycles = mc68k_execute(cpu, 1), .d0 = cpu->d[0]};
    for (unsigned i = 0; i < 8; ++i)
        result.bytes[i] = ram[0x2000 + i];
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
    return result;
}

static void test_movep_directions(void) {
    movep_result_t result = run_movep(0x0108, 0xaabbccdd); /* W mem -> D0 */
    assert(result.cycles == 12);
    assert(result.d0 == 0xaabb1234);
    result = run_movep(0x0148, 0xaabbccdd); /* L mem -> D0 */
    assert(result.cycles == 18);
    assert(result.d0 == 0x12345678);
    result = run_movep(0x0188, 0xa1b2c3d4); /* W D0 -> mem */
    assert(result.cycles == 11);
    assert(result.bytes[0] == 0xc3 && result.bytes[2] == 0xd4);
    assert(result.bytes[4] == 0x56 && result.bytes[6] == 0x78);
    assert(result.bytes[1] == 0xee && result.bytes[3] == 0xee && result.bytes[5] == 0xee &&
           result.bytes[7] == 0xee);
    result = run_movep(0x01c8, 0xa1b2c3d4); /* L D0 -> mem */
    assert(result.cycles == 17);
    assert(result.bytes[0] == 0xa1 && result.bytes[2] == 0xb2 && result.bytes[4] == 0xc3 &&
           result.bytes[6] == 0xd4);
    assert(result.bytes[1] == 0xee && result.bytes[3] == 0xee && result.bytes[5] == 0xee &&
           result.bytes[7] == 0xee);
}

typedef struct {
    int cycles;
    uint32_t d0, d1, a0, a1;
} exg_result_t;

static exg_result_t run_exg(uint16_t opcode) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    w16(ram, PC, opcode);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->d[0] = 0x11112222;
    cpu->d[1] = 0x33334444;
    cpu->a[0] = 0x55556666;
    cpu->a[1] = 0x77778888;
    exg_result_t result = {.cycles = mc68k_execute(cpu, 1),
                           .d0 = cpu->d[0],
                           .d1 = cpu->d[1],
                           .a0 = cpu->a[0],
                           .a1 = cpu->a[1]};
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
    return result;
}

static void test_exg_forms(void) {
    exg_result_t result = run_exg(0xc141); /* EXG D0,D1 */
    assert(result.cycles == 2);
    assert(result.d0 == 0x33334444 && result.d1 == 0x11112222);
    result = run_exg(0xc149); /* EXG A0,A1 */
    assert(result.cycles == 2);
    assert(result.a0 == 0x77778888 && result.a1 == 0x55556666);
    result = run_exg(0xc188); /* EXG D0,A0 */
    assert(result.cycles == 2);
    assert(result.d0 == 0x55556666 && result.a0 == 0x11112222);
}

/* UM20 §8.2.7 special MOVE manifest.  These values are copied here rather
 * than obtained from ea_cycles(): CIEA uses the fixed, one-word instruction
 * extension, even when the transfer itself is long. */
enum {
    P2C_EA_AN,
    P2C_EA_PI,
    P2C_EA_PD,
    P2C_EA_DI,
    P2C_EA_BRIEF,
    P2C_EA_AW,
    P2C_EA_AL,
    P2C_EA_PC_DI,
    P2C_EA_PC_BRIEF,
    P2C_EA_FULL
};
typedef struct {
    uint8_t form, bd, iis, bs;
} p2c_ea_case_t;

/* 7 memory-alterable basic forms plus all full-extension timing classes.
 * Pre/post-indexed forms are kept distinct: their UM20 value happens to be
 * identical, but they execute different EA paths. */
static const p2c_ea_case_t p2c_memory_eas[] = {
    {P2C_EA_AN, 0, 0, 0},   {P2C_EA_PI, 0, 0, 0},    {P2C_EA_PD, 0, 0, 0},
    {P2C_EA_DI, 0, 0, 0},   {P2C_EA_BRIEF, 0, 0, 0}, {P2C_EA_AW, 0, 0, 0},
    {P2C_EA_AL, 0, 0, 0},   {P2C_EA_FULL, 1, 0, 0},  {P2C_EA_FULL, 2, 0, 0},
    {P2C_EA_FULL, 2, 0, 1}, {P2C_EA_FULL, 3, 0, 0},  {P2C_EA_FULL, 1, 1, 0},
    {P2C_EA_FULL, 1, 2, 0}, {P2C_EA_FULL, 1, 3, 0},  {P2C_EA_FULL, 2, 1, 0},
    {P2C_EA_FULL, 2, 2, 0}, {P2C_EA_FULL, 2, 3, 0},  {P2C_EA_FULL, 3, 1, 0},
    {P2C_EA_FULL, 3, 2, 0}, {P2C_EA_FULL, 3, 3, 0},  {P2C_EA_FULL, 1, 5, 0},
    {P2C_EA_FULL, 1, 6, 0}, {P2C_EA_FULL, 1, 7, 0},  {P2C_EA_FULL, 2, 5, 0},
    {P2C_EA_FULL, 2, 6, 0}, {P2C_EA_FULL, 2, 7, 0},  {P2C_EA_FULL, 3, 5, 0},
    {P2C_EA_FULL, 3, 6, 0}, {P2C_EA_FULL, 3, 7, 0},
};
enum { P2C_MEMORY_EA_CASES = 29 };

/* The two PC-relative basic forms, legal wherever a data/control source is. */
static const p2c_ea_case_t p2c_pc_relative_eas[] = {
    {P2C_EA_PC_DI, 0, 0, 0},
    {P2C_EA_PC_BRIEF, 0, 0, 0},
};
enum { P2C_PC_RELATIVE_EA_CASES = 2 };

static int p2c_ciea(const p2c_ea_case_t *ea) {
    /* UM20 §8.2.1--§8.2.5, CIEA word column.
     * PC_DI: the CIEA/FIEA tables print no basic (d16,PC) row.  Their
     * "(bd,PC)" rows (CIEA 14/16, FIEA 15/17) equal CEA (d32,B) 12 and FEA
     * (d32,B) 13 plus the immediate width, so they are the full-format form.
     * The CEA and FEA tables print "(d16,An) or (d16,PC)" as one row (the FEA
     * table prints "of", a misprint), so the "(bd,An)" row of CIEA/FIEA is
     * read as covering both forms and PC_DI takes the (d16,An) value (an
     * inference, not a printed row).  CIEA = CEA + imm does not hold on every
     * basic row (e.g. (An)), so it is not the evidence.
     * PD: the CIEA table prints no -(An) row either (FIEA does); 2 (word) is an
     * inference: UM30 §11.6.4 ord 7/8 print -(An) as 2/4, the same as (An)
     * (ord 3/4), and UM20's (An) row is 2/4.  Carrying that over to UM20 assumes
     * the two CIEA tables agree, which holds on 6 of the 7 printed basic rows
     * (the exception is the xxx.L misprint below).  CIEA does not fetch the
     * destination (UM30 r column 0), so the 68030 data cache does not change it;
     * FIEA does fetch (r column 1) and the two manuals differ there, so the
     * same carry-over would not be valid for FIEA.
     * AL: UM20 prints 4 for "#<data>.W,xxx.L" but 8 for ".L,xxx.L" (the
     * xxx.W pair is 4/6, the FIEA xxx.L pair is 6/8), and UM30 §11.6.4 ord 13/14
     * print 6/8.  The 4 is treated as a UM20 misprint and 6 is used, matching
     * UM30 and production. */
    static const uint8_t basic[] = {2, 4, 2, 4, 6, 4, 6, 4, 6};
    static const uint8_t direct[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t indirect[3][3] = {{11, 13, 13}, {13, 15, 15}, {17, 19, 19}};
    if (ea->form != P2C_EA_FULL)
        return basic[ea->form];
    if (ea->iis == 0)
        return direct[ea->bs][ea->bd - 1] + 2;
    return indirect[ea->bd - 1][(ea->iis & 3) - 1] + 2;
}

static int p2c_cea(const p2c_ea_case_t *ea) {
    /* UM20 §8.2.1--§8.2.5, CEA word column (MOVE SR,EA). */
    static const uint8_t basic[] = {2, 2, 2, 2, 4, 2, 4, 0, 4};
    static const uint8_t direct[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t indirect[3][3] = {{11, 13, 13}, {13, 15, 15}, {17, 19, 19}};
    if (ea->form != P2C_EA_FULL)
        return basic[ea->form];
    if (ea->iis == 0)
        return direct[ea->bs][ea->bd - 1];
    return indirect[ea->bd - 1][(ea->iis & 3) - 1];
}

static int p2c_fea(const p2c_ea_case_t *ea) {
    /* UM20 §8.2.1--§8.2.5, FEA word column (MOVE EA,SR). */
    static const uint8_t basic[] = {4, 4, 5, 5, 7, 4, 4, 5, 7};
    static const uint8_t direct[2][3] = {{7, 7, 13}, {7, 9, 13}};
    static const uint8_t indirect[3][3] = {{12, 14, 14}, {14, 16, 16}, {18, 20, 20}};
    if (ea->form != P2C_EA_FULL)
        return basic[ea->form];
    if (ea->iis == 0)
        return direct[ea->bs][ea->bd - 1];
    return indirect[ea->bd - 1][(ea->iis & 3) - 1];
}

static int p2c_fiea(const p2c_ea_case_t *ea, unsigned size) {
    /* UM20 §8.2.1--§8.2.5, FIEA.  Unlike CIEA, ADDI/SUBI/CMPI
     * consume an immediate whose width follows the operand width.  The PC
     * entries are exercised by CMPI only (ADDI/SUBI destinations are
     * alterable, so PC forms are illegal there); PC_DI takes the (d16,An)
     * value (see p2c_ciea) and PC_BRIEF is the printed (d8,PC,Xn) row. */
    static const uint8_t basic_word[] = {4, 6, 5, 5, 9, 5, 6, 5, 9};
    static const uint8_t basic_long[] = {4, 8, 7, 7, 11, 7, 8, 7, 11};
    if (ea->form != P2C_EA_FULL)
        return (size == 4 ? basic_long : basic_word)[ea->form];
    return p2c_fea(ea) + (size == 4 ? 4 : 2);
}

/* (d16,PC) resolves to a fixed target that differs from A0 (0x2000), so a
 * PC-relative form resolved as an A0-indirect form reads the wrong operand. */
enum { P2C_PC_DI_TARGET = 0x2100, P2C_PC_DI_DISPLACEMENT = P2C_PC_DI_TARGET - (PC + 2) };

static unsigned p2c_append_ea(uint16_t *words, unsigned n, const p2c_ea_case_t *ea, int *mode,
                              int *reg) {
    *mode = 2;
    *reg = 0;
    switch (ea->form) {
    case P2C_EA_AN:
        break;
    case P2C_EA_PI:
        *mode = 3;
        break;
    case P2C_EA_PD:
        *mode = 4;
        break;
    case P2C_EA_DI:
        *mode = 5;
        words[n++] = 0;
        break;
    case P2C_EA_BRIEF:
        *mode = 6;
        words[n++] = 0;
        break;
    case P2C_EA_AW:
        *mode = 7;
        *reg = 0;
        words[n++] = 0x2000;
        break;
    case P2C_EA_AL:
        *mode = 7;
        *reg = 1;
        words[n++] = 0;
        words[n++] = 0x2000;
        break;
    case P2C_EA_PC_DI:
        *mode = 7;
        *reg = 2;
        words[n++] = P2C_PC_DI_DISPLACEMENT;
        break;
    case P2C_EA_PC_BRIEF:
        *mode = 7;
        *reg = 3;
        words[n++] = 0x007e;
        break;
    case P2C_EA_FULL:
        *mode = 6;
        n = append_alias_extension(words, n, ea->bd, ea->iis, ea->bs, 0);
        break;
    default:
        assert(0);
    }
    return n;
}

static uint32_t p2c_ea_address(const p2c_ea_case_t *ea, unsigned size) {
    if (ea->form == P2C_EA_PD)
        return 0x2010u - size;
    if (ea->form == P2C_EA_PC_DI)
        return P2C_PC_DI_TARGET;
    if (ea->form == P2C_EA_PC_BRIEF)
        return 0x1080;
    if (ea->form != P2C_EA_FULL)
        return 0x2000;
    if (ea->iis != 0)
        return 0x2400;
    return ea->bs ? 0 : 0x2000;
}

/* PC-relative extension words are written for one prefix-free extension word.
 * Each extra word before the EA extension moves the PC base by 2 bytes. */
static void p2c_rebase_pc_relative(uint16_t *words, unsigned first_ea_word,
                                   const p2c_ea_case_t *ea) {
    if (ea->form == P2C_EA_PC_DI || ea->form == P2C_EA_PC_BRIEF)
        words[first_ea_word] = (uint16_t)(words[first_ea_word] - (first_ea_word - 1) * 2);
}

/* Memory-indirect full-extension forms read the final address from a pointer. */
static void p2c_install_ea_pointer(uint8_t *ram, const p2c_ea_case_t *ea) {
    if (ea->form == P2C_EA_FULL && ea->iis != 0)
        w32(ram, ea->bs ? 0 : 0x2000, 0x2400);
}

static uint32_t p2c_ea_a0(const p2c_ea_case_t *ea) {
    return ea->form == P2C_EA_PD ? 0x2010 : 0x2000;
}

static void p2c_install_ea_data(uint8_t *ram, const p2c_ea_case_t *ea, unsigned size,
                                uint32_t value) {
    uint32_t address = p2c_ea_address(ea, size);
    p2c_install_ea_pointer(ram, ea);
    if (size == 1)
        ram[address] = (uint8_t)value;
    else if (size == 2)
        w16(ram, address, (uint16_t)value);
    else
        w32(ram, address, value);
}

typedef struct {
    int cycles;
    uint32_t d0, d1, a0, a1, vbr;
    uint16_t sr;
    uint32_t memory;
    uint8_t bytes[5];
} p2c_result_t;
typedef struct {
    uint32_t address;
    uint8_t fc;
    int seen;
} p2c_fc_probe_t;
static p2c_fc_probe_t *p2c_fc_probe;

static void p2c_probe_fc(void *ctx, const mc68k_bus_access_t *access,
                         mc68k_bus_timing_t *timing) {
    (void)ctx;
    timing->wait_cycles = 0;
    timing->cacheable = false;
    if (p2c_fc_probe && !access->instruction && access->physical == p2c_fc_probe->address) {
        p2c_fc_probe->fc = access->fc;
        ++p2c_fc_probe->seen;
    }
}
static p2c_result_t p2c_run(const uint16_t *words, unsigned count, const p2c_ea_case_t *ea,
                            unsigned size, uint32_t memory_value, uint32_t d1, int to_memory) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    for (unsigned i = 0; i < count; ++i)
        w16(ram, PC + i * 2, words[i]);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->sr = 0x2705;
    cpu->a[0] = ea && ea->form == P2C_EA_PD ? 0x2010 : 0x2000;
    cpu->a[1] = 0xa1b2c3d4;
    cpu->d[0] = ea ? 0 : 0xaabb0000;
    cpu->d[1] = d1;
    cpu->vbr = 0x12345678;
    if (ea)
        p2c_install_ea_data(ram, ea, size, memory_value);
    int cycles = mc68k_execute(cpu, 1);
    uint32_t at = ea ? p2c_ea_address(ea, size) : 0x2000;
    p2c_result_t result = {.cycles = cycles,
                           .d0 = cpu->d[0],
                           .d1 = cpu->d[1],
                           .a0 = cpu->a[0],
                           .a1 = cpu->a[1],
                           .vbr = cpu->vbr,
                           .sr = cpu->sr,
                           .memory = size == 1   ? ram[at]
                                     : size == 2 ? r16(ram, at)
                                                 : r32(ram, at)};
    (void)to_memory;
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
    return result;
}

/* UM20 §8.2.8 Arithmetic/Logical.  A manifest entry owns one handler
 * direction; the loop below deliberately drives every legal memory timing
 * class with real opcodes instead of asking the production EA selector. */
typedef struct {
    const char *row;
    uint16_t add_base;
    uint16_t sub_base;
    unsigned base_cycles;
    bool writes_ea;
} p2c_arith_manifest_t;

static const p2c_arith_manifest_t p2c_arith_manifest[] = {
    {"EA,Dn", 0xd000, 0x9000, 2, false},
    {"Dn,EA", 0xd100, 0x9100, 4, true},
    {"ADDA/SUBA", 0xd0c0, 0x90c0, 2, false},
    {"CMP/CMPA", 0xb000, 0xb0c0, 2, false},
};
enum { P2C_ARITH_MANIFEST_CASES = 4 };

static void test_arith_manifest(void) {
    assert(sizeof(p2c_arith_manifest) / sizeof(p2c_arith_manifest[0]) ==
           P2C_ARITH_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i) {
        const p2c_ea_case_t *ea = &p2c_memory_eas[i];
        int mode, reg;
        uint16_t words[16] = {0};
        unsigned n = p2c_append_ea(words, 1, ea, &mode, &reg);
        uint16_t ea_bits = (uint16_t)((mode << 3) | reg);

        for (unsigned size_code = 0; size_code < 3; ++size_code) {
            unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
            uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
            int fea = p2c_fea(ea);
            p2c_result_t result;

            /* ADD/SUB EA,D1 and D1,EA. */
            words[0] = (uint16_t)(p2c_arith_manifest[0].add_base | 0x0200 | (size_code << 6) |
                                  ea_bits);
            result = p2c_run(words, n, ea, size, 3, 5, 0);
            assert(result.cycles == p2c_arith_manifest[0].base_cycles + fea);
            assert((result.d1 & mask) == ((5 + 3) & mask));
            words[0] = (uint16_t)(p2c_arith_manifest[0].sub_base | 0x0200 | (size_code << 6) |
                                  ea_bits);
            result = p2c_run(words, n, ea, size, 3, 5, 0);
            assert(result.cycles == p2c_arith_manifest[0].base_cycles + fea);
            assert((result.d1 & mask) == ((5 - 3) & mask));

            words[0] = (uint16_t)(p2c_arith_manifest[1].add_base | 0x0200 | (size_code << 6) |
                                  ea_bits);
            result = p2c_run(words, n, ea, size, 5, 3, 1);
            assert(result.cycles == p2c_arith_manifest[1].base_cycles + fea);
            assert(result.memory == ((5 + 3) & mask));
            words[0] = (uint16_t)(p2c_arith_manifest[1].sub_base | 0x0200 | (size_code << 6) |
                                  ea_bits);
            result = p2c_run(words, n, ea, size, 5, 3, 1);
            assert(result.cycles == p2c_arith_manifest[1].base_cycles + fea);
            assert(result.memory == ((5 - 3) & mask));

            /* CMP changes CCR but not the destination register. */
            words[0] = (uint16_t)(p2c_arith_manifest[3].add_base | 0x0200 | (size_code << 6) |
                                  ea_bits);
            result = p2c_run(words, n, ea, size, 3, 5, 0);
            assert(result.cycles == p2c_arith_manifest[3].base_cycles + fea);
            assert(result.d1 == 5 && (result.sr & CCR_N) == 0);
        }

        /* ADDA/SUBA and CMPA have only word and long forms. */
        for (unsigned size_code = 1; size_code <= 2; ++size_code) {
            unsigned size = size_code == 1 ? 2 : 4;
            p2c_result_t result;
            words[0] = (uint16_t)(p2c_arith_manifest[2].add_base | 0x0200 |
                                  ((size_code == 1 ? 0 : 4) << 6) | ea_bits);
            result = p2c_run(words, n, ea, size, 3, 0, 0);
            assert(result.cycles == p2c_arith_manifest[2].base_cycles + p2c_fea(ea));
            assert(result.a1 == 0xa1b2c3d7u);
            words[0] = (uint16_t)(p2c_arith_manifest[2].sub_base | 0x0200 |
                                  ((size_code == 1 ? 0 : 4) << 6) | ea_bits);
            result = p2c_run(words, n, ea, size, 3, 0, 0);
            assert(result.cycles == p2c_arith_manifest[2].base_cycles + p2c_fea(ea));
            assert(result.a1 == 0xa1b2c3d1u);
            words[0] = (uint16_t)(p2c_arith_manifest[3].sub_base | 0x0200 |
                                  ((size_code == 1 ? 0 : 4) << 6) | ea_bits);
            result = p2c_run(words, n, ea, size, 3, 0, 0);
            assert(result.cycles == 4 + p2c_fea(ea));
            assert(result.a1 == 0xa1b2c3d4u);
        }
    }
}

/* The memory-alterable manifest above cannot represent source-only aliases.
 * Keep PC-relative source forms in their own manifest so ADD/SUB/CMP do not
 * silently lose a legal FEA class when the memory destination set changes. */
static const p2c_ea_case_t p2c_arith_source_aliases[] = {
    {P2C_EA_PC_DI, 0, 0, 0},
    {P2C_EA_PC_BRIEF, 0, 0, 0},
};
enum { P2C_ARITH_SOURCE_ALIAS_CASES = 2 };

static void test_arith_source_aliases(void) {
    assert(sizeof(p2c_arith_source_aliases) / sizeof(p2c_arith_source_aliases[0]) ==
           P2C_ARITH_SOURCE_ALIAS_CASES);
    for (unsigned i = 0; i < P2C_ARITH_SOURCE_ALIAS_CASES; ++i) {
        const p2c_ea_case_t *ea = &p2c_arith_source_aliases[i];
        for (unsigned size_code = 0; size_code < 3; ++size_code) {
            unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
            uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
            uint16_t words[8] = {0};
            int mode, reg;
            unsigned n = p2c_append_ea(words, 1, ea, &mode, &reg);
            for (unsigned op = 0; op < 3; ++op) {
                uint16_t base = op == 0   ? p2c_arith_manifest[0].add_base
                                : op == 1 ? p2c_arith_manifest[0].sub_base
                                          : p2c_arith_manifest[3].add_base;
                words[0] = (uint16_t)(base | 0x0200 | (size_code << 6) | (mode << 3) | reg);
                p2c_result_t result = p2c_run(words, n, ea, size, 3, 5, 0);
                assert(result.cycles == 2 + p2c_fea(ea));
                if (op == 0)
                    assert((result.d1 & mask) == ((5 + 3) & mask));
                else if (op == 1)
                    assert((result.d1 & mask) == ((5 - 3) & mask));
                else
                    assert(result.d1 == 5 && (result.sr & CCR_N) == 0);
            }
        }
        for (unsigned size_code = 1; size_code <= 2; ++size_code) {
            unsigned size = size_code == 1 ? 2 : 4;
            uint16_t words[8] = {0};
            int mode, reg;
            unsigned n = p2c_append_ea(words, 1, ea, &mode, &reg);
            uint16_t long_bit = size_code == 2 ? 0x0100 : 0;
            words[0] = (uint16_t)(p2c_arith_manifest[2].add_base | 0x0200 | long_bit |
                                  (mode << 3) | reg);
            p2c_result_t result = p2c_run(words, n, ea, size, 3, 0, 0);
            assert(result.cycles == 2 + p2c_fea(ea) && result.a1 == 0xa1b2c3d7u);
            words[0] = (uint16_t)(p2c_arith_manifest[2].sub_base | 0x0200 | long_bit |
                                  (mode << 3) | reg);
            result = p2c_run(words, n, ea, size, 3, 0, 0);
            assert(result.cycles == 2 + p2c_fea(ea) && result.a1 == 0xa1b2c3d1u);
            words[0] = (uint16_t)(p2c_arith_manifest[3].sub_base | 0x0200 | long_bit |
                                  (mode << 3) | reg);
            result = p2c_run(words, n, ea, size, 3, 0, 0);
            assert(result.cycles == 4 + p2c_fea(ea) && result.a1 == 0xa1b2c3d4u);
        }
    }
    for (unsigned size_code = 0; size_code < 3; ++size_code) {
        unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
        uint16_t words[4] = {0};
        unsigned n = 1;
        words[n++] = size == 4 ? 0 : 3;
        if (size == 4)
            words[n++] = 3;
        for (unsigned op = 0; op < 3; ++op) {
            uint16_t base = op == 0   ? p2c_arith_manifest[0].add_base
                            : op == 1 ? p2c_arith_manifest[0].sub_base
                                      : p2c_arith_manifest[3].add_base;
            words[0] = (uint16_t)(base | 0x0200 | (size_code << 6) | 0x003c);
            p2c_result_t result = p2c_run(words, n, NULL, size, 0, 5, 0);
            assert(result.cycles == 2 + (size == 4 ? 4 : 2));
            if (op == 0)
                assert((result.d1 & (size == 1   ? 0xffu
                                     : size == 2 ? 0xffffu
                                                 : 0xffffffffu)) == 8);
            else if (op == 1)
                assert((result.d1 & (size == 1   ? 0xffu
                                     : size == 2 ? 0xffffu
                                                 : 0xffffffffu)) == 2);
            else
                assert(result.d1 == 5 && (result.sr & CCR_N) == 0);
        }
    }
    /* Dn and An direct source rows do not fetch memory; An is W/L only. */
    for (unsigned op = 0; op < 3; ++op) {
        uint16_t base = op == 0   ? p2c_arith_manifest[0].add_base
                        : op == 1 ? p2c_arith_manifest[0].sub_base
                                  : p2c_arith_manifest[3].add_base;
        uint16_t d1_words[] = {(uint16_t)(base | 0x0200 | 1)};
        p2c_result_t result = p2c_run(d1_words, 1, NULL, 1, 0, 5, 0);
        assert(result.cycles == 2);
        if (op == 0)
            assert(result.d1 == 10);
        else if (op == 1)
            assert(result.d1 == 0);
        else
            assert(result.d1 == 5 && (result.sr & CCR_Z));
        for (unsigned size_code = 1; size_code <= 2; ++size_code) {
            uint16_t an_words[] = {(uint16_t)(base | 0x0200 | (size_code << 6) | 0x0008)};
            result = p2c_run(an_words, 1, NULL, size_code == 1 ? 2 : 4, 0, 5, 0);
            assert(result.cycles == 2);
            if (op == 0)
                assert(result.d1 == 0x2005);
            else if (op == 1)
                assert(result.d1 == (size_code == 1 ? 0x0000e005u : 0xffffe005u));
            else
                assert(result.d1 == 5);
        }
    }

    /* ADDA/SUBA/CMPA have the same source aliases, restricted to W/L. */
    for (unsigned size_code = 1; size_code <= 2; ++size_code) {
        unsigned size = size_code == 1 ? 2 : 4;
        uint16_t long_bit = size_code == 2 ? 0x0100 : 0;
        uint16_t words[4] = {0};
        unsigned n = 1;
        words[n++] = size == 4 ? 0 : 3;
        if (size == 4)
            words[n++] = 3;
        words[0] = (uint16_t)(p2c_arith_manifest[2].add_base | 0x0200 | long_bit | 0x003c);
        p2c_result_t result = p2c_run(words, n, NULL, size, 0, 0, 0);
        assert(result.cycles == 2 + (size == 4 ? 4 : 2) && result.a1 == 0xa1b2c3d7u);
        words[0] = (uint16_t)(p2c_arith_manifest[2].sub_base | 0x0200 | long_bit | 0x003c);
        result = p2c_run(words, n, NULL, size, 0, 0, 0);
        assert(result.cycles == 2 + (size == 4 ? 4 : 2) && result.a1 == 0xa1b2c3d1u);
        words[0] = (uint16_t)(p2c_arith_manifest[3].sub_base | 0x0200 | long_bit | 0x003c);
        result = p2c_run(words, n, NULL, size, 0, 0, 0);
        assert(result.cycles == 4 + (size == 4 ? 4 : 2) && result.a1 == 0xa1b2c3d4u);

        uint16_t dn_words[] = {
            (uint16_t)(p2c_arith_manifest[2].add_base | 0x0200 | long_bit | 1)};
        result = p2c_run(dn_words, 1, NULL, size, 0, 5, 0);
        assert(result.cycles == 2 && result.a1 == 0xa1b2c3d9u);
        dn_words[0] = (uint16_t)(p2c_arith_manifest[2].sub_base | 0x0200 | long_bit | 1);
        result = p2c_run(dn_words, 1, NULL, size, 0, 5, 0);
        assert(result.cycles == 2 && result.a1 == 0xa1b2c3cfu);
        dn_words[0] = (uint16_t)(p2c_arith_manifest[3].sub_base | 0x0200 | long_bit | 1);
        result = p2c_run(dn_words, 1, NULL, size, 0, 5, 0);
        assert(result.cycles == 4 && result.a1 == 0xa1b2c3d4u);
    }
}

/* UM20 §8.2.9 Immediate Arithmetic/Logical. */
typedef struct {
    const char *row;
    uint16_t opcode;
    unsigned base_cycles;
    bool writes;
} p2c_immediate_manifest_t;
static const p2c_immediate_manifest_t p2c_immediate_manifest[] = {
    {"ADDI", 0x0600, 4, true}, {"SUBI", 0x0400, 4, true}, {"CMPI", 0x0c00, 2, false},
    {"ADDQ", 0x5200, 4, true}, {"SUBQ", 0x5300, 4, true}, {"MOVEQ", 0x7280, 2, true},
};
enum { P2C_IMMEDIATE_MANIFEST_CASES = 6 };

static void test_immediate_manifest(void) {
    assert(sizeof(p2c_immediate_manifest) / sizeof(p2c_immediate_manifest[0]) ==
           P2C_IMMEDIATE_MANIFEST_CASES);

    /* ADDI/SUBI/CMPI: Dn plus every data-alterable memory timing class. */
    for (unsigned kind = 0; kind < 3; ++kind)
        for (unsigned size_code = 0; size_code < 3; ++size_code) {
            unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
            uint32_t immediate = 1, initial = 5,
                     mask = size == 1   ? 0xffu
                            : size == 2 ? 0xffffu
                                        : 0xffffffffu;
            uint16_t words[16] = {0};
            unsigned n = 1;
            words[n++] = size == 4 ? 0 : 1;
            if (size == 4)
                words[n++] = 1;
            words[0] = (uint16_t)(p2c_immediate_manifest[kind].opcode | (size_code << 6) | 1);
            p2c_result_t result = p2c_run(words, n, NULL, size, 0, initial, 0);
            assert(result.cycles == 2 + (size == 4 ? 4 : 2));
            if (kind == 0) {
                assert((result.d1 & mask) == ((initial + immediate) & mask));
            } else if (kind == 1)
                assert((result.d1 & mask) == ((initial - immediate) & mask));
            else
                assert(result.d1 == initial && (result.sr & CCR_N) == 0);
        }
    for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i)
        for (unsigned kind = 0; kind < 3; ++kind)
            for (unsigned size_code = 0; size_code < 3; ++size_code) {
                const p2c_ea_case_t *ea = &p2c_memory_eas[i];
                unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
                uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
                uint16_t words[16] = {0};
                int mode, reg;
                unsigned n = p2c_append_ea(words, size == 4 ? 3 : 2, ea, &mode, &reg);
                words[1] = size == 4 ? 0 : 1;
                if (size == 4)
                    words[2] = 1;
                words[0] = (uint16_t)(p2c_immediate_manifest[kind].opcode | (size_code << 6) |
                                      (mode << 3) | reg);
                p2c_result_t result = p2c_run(words, n, ea, size, 5, 0, kind != 2);
                assert(result.cycles ==
                       p2c_immediate_manifest[kind].base_cycles + p2c_fiea(ea, size));
                if (kind == 0)
                    assert(result.memory == ((5 + 1) & mask));
                else if (kind == 1)
                    assert(result.memory == ((5 - 1) & mask));
                else
                    assert(result.memory == 5 && (result.sr & CCR_N) == 0);
            }
    /* CMPI additionally accepts the two PC-relative basic EAs on 68020. */
    unsigned cmpi_pc_cases = 0;
    for (unsigned i = 0; i < P2C_PC_RELATIVE_EA_CASES; ++i)
        for (unsigned size_code = 0; size_code < 3; ++size_code) {
            const p2c_ea_case_t *ea = &p2c_pc_relative_eas[i];
            unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
            const unsigned first_ea_word = size == 4 ? 3 : 2;
            uint16_t words[16] = {0};
            int mode, reg;
            unsigned n = p2c_append_ea(words, first_ea_word, ea, &mode, &reg);
            p2c_rebase_pc_relative(words, first_ea_word, ea);
            words[1] = size == 4 ? 0 : 1;
            if (size == 4)
                words[2] = 1;
            words[0] = (uint16_t)(p2c_immediate_manifest[2].opcode | (size_code << 6) |
                                  (mode << 3) | reg);
            p2c_result_t result = p2c_run(words, n, ea, size, 5, 0, 0);
            assert(result.cycles == p2c_immediate_manifest[2].base_cycles + p2c_fiea(ea, size));
            assert(result.memory == 5 && (result.sr & CCR_N) == 0);
            ++cmpi_pc_cases;
        }
    assert(cmpi_pc_cases == P2C_PC_RELATIVE_EA_CASES * 3);

    /* ADDQ/SUBQ: Dn, all memory classes, and the W/L address-register path. */
    for (unsigned kind = 3; kind <= 4; ++kind) {
        for (unsigned size_code = 0; size_code < 3; ++size_code) {
            unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
            uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
            uint16_t words[] = {
                (uint16_t)(p2c_immediate_manifest[kind].opcode | (size_code << 6) | 1)};
            p2c_result_t result = p2c_run(words, 1, NULL, size, 0, 5, 0);
            assert(result.cycles == 2);
            assert((result.d1 & mask) == (kind == 3 ? 6u : 4u));
        }
        for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i)
            for (unsigned size_code = 0; size_code < 3; ++size_code) {
                const p2c_ea_case_t *ea = &p2c_memory_eas[i];
                unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
                uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
                uint16_t words[16] = {0};
                int mode, reg;
                unsigned n = p2c_append_ea(words, 1, ea, &mode, &reg);
                words[0] = (uint16_t)(p2c_immediate_manifest[kind].opcode | (size_code << 6) |
                                      (mode << 3) | reg);
                p2c_result_t result = p2c_run(words, n, ea, size, 5, 0, 1);
                assert(result.cycles == 4 + p2c_fea(ea));
                assert(result.memory == ((kind == 3 ? 6u : 4u) & mask));
            }
        for (unsigned size_code = 1; size_code <= 2; ++size_code) {
            uint16_t words[] = {
                (uint16_t)(p2c_immediate_manifest[kind].opcode | (size_code << 6) | 0x09)};
            p2c_result_t result = p2c_run(words, 1, NULL, size_code == 1 ? 2 : 4, 0, 0, 0);
            assert(result.cycles == 2);
            assert(result.a1 == (kind == 3 ? 0xa1b2c3d5u : 0xa1b2c3d3u));
        }
    }
    { /* MOVEQ #$80,D1: sign extension is part of the observable path. */
        uint16_t words[] = {p2c_immediate_manifest[5].opcode};
        p2c_result_t result = p2c_run(words, 1, NULL, 4, 0, 0, 0);
        assert(result.cycles == 2 && result.d1 == 0xffffff80u && (result.sr & CCR_N));
    }
}

/* UM20 §8.2.10 PACK/UNPK and §8.2.11 TAS. */
typedef struct {
    const char *row;
    uint16_t opcode;
    int cycles;
} p2c_bcd_manifest_t;
static const p2c_bcd_manifest_t p2c_bcd_manifest[] = {
    {"PACK Dn,Dn", 0x8340, 6},
    {"PACK -(An),-(An)", 0x8348, 13},
    {"UNPK Dn,Dn", 0x8380, 8},
    {"UNPK -(An),-(An)", 0x8388, 13},
};
enum { P2C_BCD_MANIFEST_CASES = 4 };

static void test_bcd_and_tas_manifest(void) {
    assert(sizeof(p2c_bcd_manifest) / sizeof(p2c_bcd_manifest[0]) == P2C_BCD_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_BCD_MANIFEST_CASES; ++i) {
        uint8_t *ram = calloc(1, RAM_SIZE);
        assert(ram);
        w32(ram, 0, 0x8000);
        w32(ram, 4, PC);
        w16(ram, PC, p2c_bcd_manifest[i].opcode);
        w16(ram, PC + 2, 0);
        mem_ctx_t *mem = mem_create();
        mem_region_t *region = calloc(1, sizeof(*region));
        assert(mem && region);
        region->data = ram;
        region->size = RAM_SIZE;
        mem_add_region(mem, region);
        mc68k_bus_t bus = bus_make_hooks(mem);
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
        mc68k_reset(cpu);
        cpu->d[0] = 0x1234;
        cpu->a[0] = i == 3 ? 0x201 : 0x202;
        cpu->a[1] = 0x300;
        ram[0x200] = i == 3 ? 0x24 : 2;
        ram[0x201] = 4;
        assert(mc68k_execute(cpu, 1) == p2c_bcd_manifest[i].cycles);
        if (i == 0)
            assert((cpu->d[1] & 0xff) == 0x24);
        else if (i == 1)
            assert(cpu->a[0] == 0x200 && cpu->a[1] == 0x2ff && ram[0x2ff] == 0x24);
        else if (i == 2)
            assert((cpu->d[1] & 0xffff) == 0x0304);
        else
            assert(cpu->a[0] == 0x200 && cpu->a[1] == 0x2fe && ram[0x2fe] == 2 &&
                   ram[0x2ff] == 4);
        mc68k_destroy(cpu);
        mem_destroy(mem);
        free(region);
        free(ram);
    }
    { /* TAS D1 uses its distinct register row. */
        uint16_t words[] = {0x4ac1};
        p2c_result_t result = p2c_run(words, 1, NULL, 1, 0, 0x12, 0);
        assert(result.cycles == 4 && result.d1 == 0x92 && (result.sr & CCR_N) == 0);
    }
    for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i) {
        const p2c_ea_case_t *ea = &p2c_memory_eas[i];
        uint16_t words[16] = {0};
        int mode, reg;
        unsigned n = p2c_append_ea(words, 1, ea, &mode, &reg);
        words[0] = (uint16_t)(0x4ac0 | (mode << 3) | reg);
        p2c_result_t result = p2c_run(words, n, ea, 1, 0x12, 0, 1);
        assert(result.cycles == 12 + p2c_cea(ea));
        assert(result.memory == 0x92 && (result.sr & CCR_N) == 0);
    }
}

/* UM20 §8.2.14 Bit Field.  The basic CIEA(word) values are transcribed here
 * independently, except the inferred/corrected AL and PC_DI entries, which are
 * shared with p2c_ciea (the PD entry is shared too but never read: bit-field
 * memory operands are control EAs, which exclude (An)+ and -(An)).  The
 * full-format rows are p2c_cea + 2, which holds on every full-format row of
 * UM20 §8.2.3/§8.2.4.  Bit-field's extension is always one word, irrespective
 * of the field width. */
typedef struct {
    const char *row;
    uint8_t dn_cycles;
    uint8_t mem4_cycles;
    uint8_t mem5_cycles;
} p2c_bitfield_manifest_t;

static const p2c_bitfield_manifest_t p2c_bitfield_manifest[] = {
    {"BFTST", 6, 11, 15},  {"BFEXTU", 8, 13, 18}, {"BFCHG", 12, 16, 24}, {"BFEXTS", 8, 13, 18},
    {"BFCLR", 12, 16, 24}, {"BFFFO", 18, 24, 32}, {"BFSET", 12, 16, 24}, {"BFINS", 10, 14, 20},
};
enum { P2C_BITFIELD_MANIFEST_CASES = 8 };

static int p2c_bitfield_ciea_word(const p2c_ea_case_t *ea) {
    /* UM20 §8.2.1--§8.2.5 CIEA, word extension column.  Identical to
     * p2c_ciea's basic row, including the inferred PC_DI/PD entries and the
     * (xxx).L value 6 that overrides UM20's misprinted 4 (see p2c_ciea). */
    static const uint8_t basic[] = {2, 4, 2, 4, 6, 4, 6, 4, 6};
    if (ea->form != P2C_EA_FULL)
        return basic[ea->form];
    return p2c_cea(ea) + 2;
}

static p2c_result_t p2c_run_bitfield(unsigned kind, uint16_t extension, const p2c_ea_case_t *ea,
                                     uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                                     uint8_t memory, uint32_t middle_bytes, uint8_t fifth_byte,
                                     uint8_t previous_byte) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    uint16_t words[16] = {(uint16_t)(0xe8c0 | (kind << 8)), extension};
    int mode = 0, reg = 0;
    unsigned count = 2;
    if (ea) {
        count = p2c_append_ea(words, count, ea, &mode, &reg);
        words[0] |= (uint16_t)((mode << 3) | reg);
        /* The bit-field extension precedes the EA extension. */
        p2c_rebase_pc_relative(words, 2, ea);
    }
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    for (unsigned i = 0; i < count; ++i)
        w16(ram, PC + i * 2, words[i]);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->a[0] = ea && ea->form == P2C_EA_PD ? 0x2010 : 0x2000;
    cpu->d[0] = d0;
    cpu->d[1] = d1;
    cpu->d[2] = d2;
    cpu->d[3] = d3;
    if (ea) {
        uint32_t address = p2c_ea_address(ea, 1);
        for (unsigned i = 0; i < 5; ++i)
            ram[address + i] = 0;
        p2c_install_ea_data(ram, ea, 1, memory);
        ram[address + 1] = (uint8_t)(middle_bytes >> 16);
        ram[address + 2] = (uint8_t)(middle_bytes >> 8);
        ram[address + 3] = (uint8_t)middle_bytes;
        ram[address + 4] = fifth_byte;
        if (address)
            ram[address - 1] = previous_byte;
    }
    int cycles = mc68k_execute(cpu, 1);
    uint32_t address = ea ? p2c_ea_address(ea, 1) : 0;
    p2c_result_t result = {.cycles = cycles,
                           .d0 = cpu->d[0],
                           .d1 = cpu->d[1],
                           .a0 = cpu->a[0],
                           .sr = cpu->sr,
                           .memory = ea ? ram[address] : 0};
    if (ea)
        for (unsigned i = 0; i < sizeof(result.bytes); ++i)
            result.bytes[i] = ram[address + i];
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
    return result;
}

static void p2c_assert_bitfield_effect(unsigned kind, const p2c_result_t *result,
                                       bool memory_form, bool span) {
    uint32_t value = memory_form ? result->memory : result->d0;
    uint32_t first_bit = memory_form ? (span ? 0x40u : 0x80u) : 0x80000000u;
    switch (kind) {
    case 0:
        assert(result->sr & CCR_N);
        break; /* BFTST */
    case 1:
        assert(result->d1 == (span ? 0x802468adu : 0x80000000u));
        break; /* BFEXTU */
    case 2:
        assert((value & first_bit) == 0);
        break; /* BFCHG */
    case 3:
        assert(result->d1 == (span ? 0x802468adu : 0x80000000u));
        break; /* BFEXTS */
    case 4:
        assert((value & first_bit) == 0);
        break; /* BFCLR */
    case 5:
        assert(result->d1 == (span ? 1u : 0u));
        break; /* BFFFO */
    case 6:
        assert(value & first_bit);
        break; /* BFSET */
    case 7:
        assert(span ? (value & first_bit) == 0 : value & first_bit);
        break; /* BFINS */
    }
}

static void p2c_assert_bitfield_span_write(unsigned kind, const p2c_result_t *result) {
    static const uint8_t expected[8][5] = {
        {0x40, 0x12, 0x34, 0x56, 0x80}, {0x40, 0x12, 0x34, 0x56, 0x80},
        {0x3f, 0xed, 0xcb, 0xa9, 0x00}, {0x40, 0x12, 0x34, 0x56, 0x80},
        {0x00, 0x00, 0x00, 0x00, 0x00}, {0x40, 0x12, 0x34, 0x56, 0x80},
        {0x7f, 0xff, 0xff, 0xff, 0x80}, {0x00, 0x00, 0x00, 0x00, 0x00},
    };
    for (unsigned i = 0; i < sizeof(result->bytes); ++i)
        assert(result->bytes[i] == expected[kind][i]);
}

/* Executed memory-EA rows: 27 control EAs (29 memory classes minus (An)+ and
 * -(An)) x 8 operations x 2 spans; PC-relative rows: 2 EAs x 4 read-only
 * operations x 2 spans. */
enum { P2C_BITFIELD_MEMORY_EXECUTED_CASES = 432, P2C_BITFIELD_PC_EXECUTED_CASES = 16 };

static void test_bitfield_manifest(void) {
    assert(sizeof(p2c_bitfield_manifest) / sizeof(p2c_bitfield_manifest[0]) ==
           P2C_BITFIELD_MANIFEST_CASES);

    /* All eight operations use the register row.  Width zero encodes 32. */
    for (unsigned kind = 0; kind < P2C_BITFIELD_MANIFEST_CASES; ++kind) {
        uint32_t initial = kind == 6 || kind == 7 ? 0 : 0x80000000u;
        uint32_t source = kind == 7 ? 0x80000000u : 1;
        p2c_result_t result =
            p2c_run_bitfield(kind, 0x1000, NULL, initial, source, 0, 0, 0, 0, 0, 0);
        assert(result.cycles == p2c_bitfield_manifest[kind].dn_cycles);
        p2c_assert_bitfield_effect(kind, &result, false, false);
    }

    /* Every legal memory EA class executes both <5-byte and 5-byte rows.
     * Offset 0,width 32 consumes four bytes; offset 1,width 32 consumes five. */
    assert(sizeof(p2c_memory_eas) / sizeof(p2c_memory_eas[0]) == P2C_MEMORY_EA_CASES);
    unsigned memory_cases = 0;
    for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i)
        for (unsigned kind = 0; kind < P2C_BITFIELD_MANIFEST_CASES; ++kind)
            for (unsigned span = 0; span < 2; ++span) {
                if (p2c_memory_eas[i].form == P2C_EA_PI || p2c_memory_eas[i].form == P2C_EA_PD)
                    continue; /* Bit-field memory operands are control EAs. */
                uint16_t extension = (uint16_t)(0x1000 | (span << 6));
                uint8_t initial = kind == 6 || kind == 7 ? 0 : span ? 0x40 : 0x80;
                uint32_t source = kind == 7 ? (span ? 0 : 0x80000000u) : 1;
                p2c_result_t result =
                    p2c_run_bitfield(kind, extension, &p2c_memory_eas[i], 0, source, 0, 0,
                                     initial, span ? 0x123456u : 0, span ? 0x80 : 0, 0);
                int base = span ? p2c_bitfield_manifest[kind].mem5_cycles
                                : p2c_bitfield_manifest[kind].mem4_cycles;
                assert(result.cycles == base + p2c_bitfield_ciea_word(&p2c_memory_eas[i]));
                p2c_assert_bitfield_effect(kind, &result, true, span);
                if (span) {
                    if (kind == 1 || kind == 3)
                        assert(result.d1 == 0x802468adu);
                    p2c_assert_bitfield_span_write(kind, &result);
                }
                ++memory_cases;
            }
    assert(memory_cases == P2C_BITFIELD_MEMORY_EXECUTED_CASES);

    /* BFTST and BFFFO must also read the fifth byte: it is the only set
     * bit in this five-byte field. */
    p2c_result_t result =
        p2c_run_bitfield(0, 0x1040, &p2c_memory_eas[0], 0, 0, 0, 0, 0, 0, 0x80, 0);
    assert(result.cycles ==
           p2c_bitfield_manifest[0].mem5_cycles + p2c_bitfield_ciea_word(&p2c_memory_eas[0]));
    assert((result.sr & CCR_Z) == 0);
    result = p2c_run_bitfield(5, 0x1040, &p2c_memory_eas[0], 0, 0, 0, 0, 0, 0, 0x80, 0);
    assert(result.cycles ==
           p2c_bitfield_manifest[5].mem5_cycles + p2c_bitfield_ciea_word(&p2c_memory_eas[0]));
    assert(result.d1 == 32);

    /* Read-only forms additionally accept the two PC-relative basic EAs. */
    static const unsigned read_only_kinds[] = {0, 1, 3, 5};
    unsigned pc_cases = 0;
    for (unsigned i = 0; i < P2C_PC_RELATIVE_EA_CASES; ++i)
        for (unsigned k = 0; k < sizeof(read_only_kinds) / sizeof(read_only_kinds[0]); ++k)
            for (unsigned span = 0; span < 2; ++span) {
                unsigned kind = read_only_kinds[k];
                uint16_t extension = (uint16_t)(0x1000 | (span << 6));
                uint8_t initial = span ? 0x40 : 0x80;
                p2c_result_t result =
                    p2c_run_bitfield(kind, extension, &p2c_pc_relative_eas[i], 0, 1, 0, 0,
                                     initial, span ? 0x123456u : 0, span ? 0x80 : 0, 0);
                int base = span ? p2c_bitfield_manifest[kind].mem5_cycles
                                : p2c_bitfield_manifest[kind].mem4_cycles;
                assert(result.cycles == base + p2c_bitfield_ciea_word(&p2c_pc_relative_eas[i]));
                p2c_assert_bitfield_effect(kind, &result, true, span);
                ++pc_cases;
            }
    assert(pc_cases == P2C_BITFIELD_PC_EXECUTED_CASES);

    /* Static width below 32 can still span five bytes (7 + 31 = 38). */
    result = p2c_run_bitfield(1, 0x11df, &p2c_memory_eas[0], 0, 0, 0, 0, 0x01, 0, 0, 0);
    assert(result.cycles ==
           p2c_bitfield_manifest[1].mem5_cycles + p2c_bitfield_ciea_word(&p2c_memory_eas[0]));
    assert(result.d1 == 0x40000000u);

    /* Dynamic offset and width, including a negative offset, use the same
     * field semantics but distinct extension-word paths. */
    result = p2c_run_bitfield(1, 0x18a3, &p2c_memory_eas[0], 0, 0, 4, 8, 0x0f, 0, 0, 0);
    assert(result.cycles ==
           p2c_bitfield_manifest[1].mem4_cycles + p2c_bitfield_ciea_word(&p2c_memory_eas[0]));
    assert(result.d1 == 0xf0);
    result =
        p2c_run_bitfield(0, 0x18a3, &p2c_memory_eas[0], 0, 0, UINT32_MAX, 1, 0, 0, 0, 0x01);
    assert(result.cycles ==
           p2c_bitfield_manifest[0].mem4_cycles + p2c_bitfield_ciea_word(&p2c_memory_eas[0]));
    assert(result.sr & CCR_N); /* D2=-1 must address A0-1, not A0. */
}

typedef struct {
    const char *row;
    uint16_t opcode;
    uint16_t extension;
    uint32_t d0;
    uint32_t d1;
    uint32_t operand;
    int cycles;
    uint32_t expected_d0;
    uint32_t expected_d1;
    bool overflow;
} p2c_long_arith_case_t;

/* UM20 §8.2.8: the extension is always one word.  The cycle constants below
 * are the Cache Case bases plus the independently copied FIEA(word)=4 for
 * immediate source EA; no production timing helper participates. */
static const p2c_long_arith_case_t p2c_long_arith_manifest[] = {
    {"MULU.L 32", 0x4c3c, 0x0001, 3, 0, 4, 47, 12, 0, false},
    {"MULS.L 32", 0x4c3c, 0x0801, 3, 0, 0xfffffffcu, 47, 0xfffffff4u, 0, false},
    {"MULU.L 64", 0x4c3c, 0x0401, 3, 0, 4, 47, 12, 0, false},
    {"MULS.L 64", 0x4c3c, 0x0c01, 3, 0, 0xfffffffcu, 47, 0xfffffff4u, 0xffffffffu, false},
    {"DIVU.L 32 normal", 0x4c7c, 0x0001, 10, 0, 3, 82, 3, 1, false},
    {"DIVS.L 32 normal", 0x4c7c, 0x0801, 0xfffffff6u, 0, 3, 94, 0xfffffffdu, 0xffffffffu,
     false},
    {"DIVU.L 32 max quotient", 0x4c7c, 0x0001, 0xffffffffu, 0, 1, 82, 0xffffffffu, 0, false},
    {"DIVS.L 32 overflow", 0x4c7c, 0x0801, 0x80000000u, 0, 0xffffffffu, 94, 0x80000000u, 0,
     true},
    {"DIVU.L 64 normal", 0x4c7c, 0x0401, 10, 0, 3, 82, 3, 1, false},
    {"DIVS.L 64 normal", 0x4c7c, 0x0c01, 0xfffffff6u, 0xffffffffu, 3, 94, 0xfffffffdu,
     0xffffffffu, false},
    {"DIVU.L 64 overflow", 0x4c7c, 0x0401, 0, 1, 1, 82, 0, 1, true},
    {"DIVS.L 64 overflow", 0x4c7c, 0x0c01, 0, 1, 1, 94, 0, 1, true},
};
enum { P2C_LONG_ARITH_MANIFEST_CASES = 12 };

static void test_long_arith_manifest(void) {
    assert(sizeof(p2c_long_arith_manifest) / sizeof(p2c_long_arith_manifest[0]) ==
           P2C_LONG_ARITH_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_LONG_ARITH_MANIFEST_CASES; ++i) {
        const p2c_long_arith_case_t *c = &p2c_long_arith_manifest[i];
        uint8_t *ram = calloc(1, RAM_SIZE);
        assert(ram);
        w32(ram, 0, 0x8000);
        w32(ram, 4, PC);
        w16(ram, PC, c->opcode);
        w16(ram, PC + 2, c->extension);
        w32(ram, PC + 4, c->operand);
        mem_ctx_t *mem = mem_create();
        mem_region_t *region = calloc(1, sizeof(*region));
        assert(mem && region);
        region->data = ram;
        region->size = RAM_SIZE;
        mem_add_region(mem, region);
        mc68k_bus_t bus = bus_make_hooks(mem);
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
        mc68k_reset(cpu);
        cpu->d[0] = c->d0;
        cpu->d[1] = c->d1;
        assert(mc68k_execute(cpu, 1) == c->cycles);
        assert(cpu->d[0] == c->expected_d0 && cpu->d[1] == c->expected_d1);
        assert(((cpu->sr & CCR_V) != 0) == c->overflow);
        mc68k_destroy(cpu);
        mem_destroy(mem);
        free(region);
        free(ram);
    }
}

typedef struct {
    const char *row;
    unsigned size_code;
    uint32_t value;
    bool out_of_range;
} p2c_cmp2_case_t;
static const p2c_cmp2_case_t p2c_cmp2_manifest[] = {
    {"CMP2.B in", 0, 3, false}, {"CMP2.B out", 0, 8, true}, {"CMP2.W in", 1, 3, false},
    {"CMP2.W out", 1, 8, true}, {"CMP2.L in", 2, 3, false}, {"CMP2.L out", 2, 8, true},
};
enum { P2C_CMP2_MANIFEST_CASES = 6 };

static void test_cmp2_manifest(void) {
    assert(sizeof(p2c_cmp2_manifest) / sizeof(p2c_cmp2_manifest[0]) == P2C_CMP2_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_CMP2_MANIFEST_CASES; ++i) {
        const p2c_cmp2_case_t *c = &p2c_cmp2_manifest[i];
        unsigned size = c->size_code == 0 ? 1 : c->size_code == 1 ? 2 : 4;
        uint8_t *ram = calloc(1, RAM_SIZE);
        assert(ram);
        w32(ram, 0, 0x8000);
        w32(ram, 4, PC);
        w16(ram, PC, (uint16_t)(0x00d0 | (c->size_code << 9)));
        w16(ram, PC + 2, 0);
        if (size == 1) {
            ram[0x2000] = 2;
            ram[0x2001] = 4;
        } else if (size == 2) {
            w16(ram, 0x2000, 2);
            w16(ram, 0x2002, 4);
        } else {
            w32(ram, 0x2000, 2);
            w32(ram, 0x2004, 4);
        }
        mem_ctx_t *mem = mem_create();
        mem_region_t *region = calloc(1, sizeof(*region));
        assert(mem && region);
        region->data = ram;
        region->size = RAM_SIZE;
        mem_add_region(mem, region);
        mc68k_bus_t bus = bus_make_hooks(mem);
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
        mc68k_reset(cpu);
        cpu->a[0] = 0x2000;
        cpu->d[0] = c->value;
        assert(mc68k_execute(cpu, 1) == 22); /* base 18 + FIEA(word) for (An): 4 */
        assert(((cpu->sr & CCR_C) != 0) == c->out_of_range);
        mc68k_destroy(cpu);
        mem_destroy(mem);
        free(region);
        free(ram);
    }
}

static void test_special_move_system(void) {
    unsigned cases = 0;
    uint16_t words[16] = {0x4e7a, 0x1801}; /* MOVEC VBR,D1 */
    p2c_result_t result = p2c_run(words, 2, NULL, 2, 0, 0, 0);
    ++cases;
    assert(result.cycles == 6 && result.d1 == 0x12345678);
    words[0] = 0x4e7b; /* MOVEC D1,VBR */
    result = p2c_run(words, 2, NULL, 2, 0, 0x89abcdef, 0);
    ++cases;
    assert(result.cycles == 12 && result.vbr == 0x89abcdef);

    words[0] = 0x40c0; /* MOVE SR,D0 */
    result = p2c_run(words, 1, NULL, 2, 0, 0xaabb0000, 0);
    ++cases;
    assert(result.cycles == 4 && result.d0 == 0xaabb2705 && result.d1 == 0xaabb0000);
    assert(sizeof(p2c_memory_eas) / sizeof(p2c_memory_eas[0]) == P2C_MEMORY_EA_CASES);
    for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i) {
        int mode, reg;
        unsigned n = p2c_append_ea(words, 1, &p2c_memory_eas[i], &mode, &reg);
        words[0] = (uint16_t)(0x40c0 | (mode << 3) | reg); /* MOVE SR,EA */
        result = p2c_run(words, n, &p2c_memory_eas[i], 2, 0xa55a, 0, 1);
        ++cases;
        assert(result.cycles == 5 + p2c_cea(&p2c_memory_eas[i]));
        assert(result.memory == 0x2705);
    }
    /* EA→SR additionally accepts the two PC-relative basic forms. */
    words[0] = 0x46c1; /* MOVE D1,SR */
    result = p2c_run(words, 1, NULL, 2, 0, 0xaabb1234, 0);
    ++cases;
    assert(result.cycles == 8 && result.sr == 0x1234);
    for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i) {
        int mode, reg;
        unsigned n = p2c_append_ea(words, 1, &p2c_memory_eas[i], &mode, &reg);
        words[0] = (uint16_t)(0x46c0 | (mode << 3) | reg);
        result = p2c_run(words, n, &p2c_memory_eas[i], 2, 0x1234, 0, 0);
        ++cases;
        assert(result.cycles == 8 + p2c_fea(&p2c_memory_eas[i]));
        assert(result.sr == 0x1234);
    }
    const p2c_ea_case_t pc_sources[] = {
        {P2C_EA_PC_DI, 0, 0, 0},
        {P2C_EA_PC_BRIEF, 0, 0, 0},
    };
    for (unsigned i = 0; i < sizeof(pc_sources) / sizeof(pc_sources[0]); ++i) {
        int mode, reg;
        unsigned n = p2c_append_ea(words, 1, &pc_sources[i], &mode, &reg);
        words[0] = (uint16_t)(0x46c0 | (mode << 3) | reg);
        result = p2c_run(words, n, &pc_sources[i], 2, 0x1234, 0, 0);
        ++cases;
        if (result.cycles != 8 + p2c_fea(&pc_sources[i]) || result.sr != 0x1234)
            fprintf(stderr, "MOVE PC-relative SR mismatch form=%u cycles=%d sr=%04X\n",
                    pc_sources[i].form, result.cycles, result.sr);
        assert(result.cycles == 8 + p2c_fea(&pc_sources[i]) && result.sr == 0x1234);
    }
    words[0] = 0x46fc;
    words[1] = 0x1234; /* MOVE #$1234,SR */
    result = p2c_run(words, 2, NULL, 2, 0, 0, 0);
    ++cases;
    assert(result.cycles == 10 && result.sr == 0x1234);
    /* Full-format PC-relative source with a word base displacement. */
    words[0] = 0x46fb;
    words[1] = 0x7120;
    words[2] = 4;
    words[3] = 0x1234;
    result = p2c_run(words, 4, NULL, 2, 0, 0, 0);
    ++cases;
    if (result.cycles != 15 || result.sr != 0x1234)
        fprintf(stderr, "MOVE full PC-relative SR mismatch cycles=%d sr=%04X\n", result.cycles,
                result.sr);
    assert(result.cycles == 15 && result.sr == 0x1234);
    assert(cases == 66); /* MOVEC, MOVE SR→EA, and MOVE EA→SR manifest. */
}

static void test_special_move_movem(void) {
    static const uint16_t masks[] = {0x0000, 0x0001, 0xffff};
    unsigned cases = 0;
    /* Store additionally permits -(An); load additionally permits PC-relative
     * basic forms.  The full-extension manifest above supplies every 020
     * full timing class. */
    for (unsigned dir = 0; dir < 2; ++dir)
        for (unsigned sz = 0; sz < 2; ++sz)
            for (unsigned m = 0; m < 3; ++m)
                for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i) {
                    const p2c_ea_case_t *ea = &p2c_memory_eas[i];
                    if (!dir && ea->form == P2C_EA_PI)
                        continue; /* MOVEM store excludes (An)+ */
                    if (dir && ea->form == P2C_EA_PD)
                        continue; /* MOVEM load excludes -(An) */
                    ++cases;
                    uint16_t words[16] = {0};
                    int mode, reg;
                    unsigned n = p2c_append_ea(words, 2, ea, &mode, &reg);
                    words[0] = (uint16_t)((dir ? 0x4c80 : 0x4880) | (sz ? 0x40 : 0) |
                                          (mode << 3) | reg);
                    words[1] = masks[m];
                    unsigned count = masks[m] == 0xffff ? 16 : masks[m] != 0;
                    p2c_result_t result =
                        p2c_run(words, n, ea, sz ? 4 : 2, 0x80018002, 0x11223344, !dir);
                    int expected =
                        (dir ? 8 + 4 * (int)count : 4 + 3 * (int)count) + p2c_ciea(ea);
                    assert(result.cycles == expected);
                    if (dir && ea->form == P2C_EA_PI) {
                        if (result.a0 != 0x2000 + count * (sz ? 4 : 2))
                            fprintf(
                                stderr,
                                "MOVEM postincrement mismatch sz=%u mask=%04X actual=%08X\n",
                                sz, masks[m], result.a0);
                        assert(result.a0 == 0x2000 + count * (sz ? 4 : 2));
                    }
                    if (!dir && ea->form == P2C_EA_PD)
                        assert(result.a0 == 0x2010 - count * (sz ? 4 : 2));
                    if (!dir && masks[m] == 1) {
                        uint32_t stored = ea->form == P2C_EA_PD ? 0x8000u : 0;
                        if (result.memory != stored)
                            fprintf(stderr,
                                    "MOVEM store result mismatch ea=%u sz=%u value=%08X\n", i,
                                    sz, result.memory);
                        assert(result.memory == stored);
                    }
                    if (dir && masks[m] == 1) {
                        uint32_t loaded = sz ? 0x80018002u : 0xffff8002u;
                        if (result.d0 != loaded)
                            fprintf(stderr,
                                    "MOVEM load result mismatch ea=%u sz=%u value=%08X\n", i,
                                    sz, result.d0);
                        assert(result.d0 == loaded);
                    }
                }
    /* d16(PC) and brief-indexed PC are the two legal PC-relative MOVEM
     * load classes.  Full-extension classes use the same CIEA manifest
     * above via mode 6; these two execute the distinct PC base paths. */
    for (unsigned sz = 0; sz < 2; ++sz)
        for (unsigned pc_index = 0; pc_index < 2; ++pc_index)
            for (unsigned m = 0; m < 3; ++m) {
                const p2c_ea_case_t *ea = &p2c_pc_relative_eas[pc_index];
                uint8_t *ram = calloc(1, RAM_SIZE);
                assert(ram);
                uint16_t words[16] = {0};
                int mode, reg;
                unsigned n = p2c_append_ea(words, 2, ea, &mode, &reg);
                p2c_rebase_pc_relative(words, 2, ea);
                words[0] = (uint16_t)(0x4c80 | (sz ? 0x40 : 0) | (mode << 3) | reg);
                words[1] = masks[m];
                w32(ram, 0, 0x8000);
                w32(ram, 4, PC);
                for (unsigned w = 0; w < n; ++w)
                    w16(ram, PC + w * 2, words[w]);
                if (sz)
                    w32(ram, p2c_ea_address(ea, 4), 0x00008002);
                else
                    w16(ram, p2c_ea_address(ea, 2), 0x8002);
                mem_ctx_t *mem = mem_create();
                mem_region_t *region = calloc(1, sizeof(*region));
                assert(mem && region);
                region->data = ram;
                region->size = RAM_SIZE;
                mem_add_region(mem, region);
                mc68k_bus_t bus = bus_make_hooks(mem);
                mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
                mc68k_reset(cpu);
                int cycles = mc68k_execute(cpu, 1);
                unsigned count = masks[m] == 0xffff ? 16 : masks[m] != 0;
                assert(cycles == 8 + 4 * (int)count + p2c_ciea(ea));
                if (masks[m] == 1) {
                    uint32_t expected_value = sz ? 0x00008002u : 0xffff8002u;
                    if (cpu->d[0] != expected_value)
                        fprintf(stderr, "MOVEM PC load mismatch index=%u sz=%u actual=%08X\n",
                                pc_index, sz, cpu->d[0]);
                    assert(cpu->d[0] == expected_value);
                }
                ++cases;
                mc68k_destroy(cpu);
                mem_destroy(mem);
                free(region);
                free(ram);
            }
    assert(cases == 348); /* 2 directions × 2 sizes × 3 masks × legal EA manifest. */
}

static void test_special_move_movem_full_masks(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    for (unsigned i = 0; i < 8; ++i) {
        cpu->d[i] = 0x11110000 + i;
        cpu->a[i] = 0x22220000 + i;
    }
    cpu->a[0] = 0x2000;
    w16(ram, PC, 0x48d0);
    w16(ram, PC + 2, 0xffff); /* MOVEM.L D0-A7,(A0) */
    assert(mc68k_execute(cpu, 1) == 54);
    for (unsigned i = 0; i < 8; ++i)
        assert(r32(ram, 0x2000 + i * 4) == 0x11110000 + i);
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t expected = i == 0 ? 0x2000 : 0x22220000 + i;
        assert(r32(ram, 0x2020 + i * 4) == expected);
    }
    for (unsigned i = 0; i < 16; ++i)
        w32(ram, 0x2000 + i * 4, 0x80000000 + i);
    w16(ram, PC, 0x4cd0);
    w16(ram, PC + 2, 0xffff); /* MOVEM.L (A0),D0-A7 */
    cpu->pc = PC;
    cpu->a[0] = 0x2000;
    assert(mc68k_execute(cpu, 1) == 74);
    for (unsigned i = 0; i < 8; ++i)
        assert(cpu->d[i] == 0x80000000 + i);
    for (unsigned i = 0; i < 8; ++i)
        assert(cpu->a[i] == 0x80000000 + 8 + i);
    for (unsigned i = 0; i < 8; ++i) {
        cpu->d[i] = 0x55550000 + i;
        cpu->a[i] = 0x66660000 + i;
    }
    cpu->a[0] = 0x2100;
    w32(ram, 0x2100, 0xdeadbeef);
    w16(ram, PC, 0x4cd0);
    w16(ram, PC + 2, 0);
    cpu->pc = PC;
    assert(mc68k_execute(cpu, 1) == 10);
    for (unsigned i = 0; i < 8; ++i)
        assert(cpu->d[i] == 0x55550000 + i);
    assert(cpu->a[0] == 0x2100);
    for (unsigned i = 1; i < 8; ++i)
        assert(cpu->a[i] == 0x66660000 + i);
    w32(ram, 0x2100, 0xdeadbeef);
    w16(ram, PC, 0x48d0);
    w16(ram, PC + 2, 0);
    cpu->pc = PC;
    cpu->a[0] = 0x2100;
    assert(mc68k_execute(cpu, 1) == 6 && r32(ram, 0x2100) == 0xdeadbeef);
    for (unsigned i = 0; i < 16; ++i)
        w32(ram, 0x2000 + i * 4, 0x80000000 + i);
    w16(ram, PC, 0x4cd8);
    w16(ram, PC + 2, 0xffff); /* MOVEM.L (A0)+,D0-A7 */
    cpu->pc = PC;
    cpu->a[0] = 0x2000;
    assert(mc68k_execute(cpu, 1) == 76);
    for (unsigned i = 0; i < 8; ++i)
        assert(cpu->d[i] == 0x80000000 + i);
    for (unsigned i = 1; i < 8; ++i)
        assert(cpu->a[i] == 0x80000000 + 8 + i);
    assert(cpu->a[0] == 0x2040);
    for (unsigned i = 0; i < 8; ++i) {
        cpu->d[i] = 0x33330000 + i;
        cpu->a[i] = 0x44440000 + i;
    }
    cpu->a[0] = 0x2200;
    w16(ram, PC, 0x48e0);
    w16(ram, PC + 2, 0xffff); /* MOVEM.L D0-A7,-(A0) */
    cpu->pc = PC;
    assert(mc68k_execute(cpu, 1) == 54 && cpu->a[0] == 0x21c0);
    for (unsigned i = 0; i < 8; ++i)
        assert(r32(ram, 0x21c0 + i * 4) == 0x33330000 + i);
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t expected = i == 0 ? 0x21fc : 0x44440000 + i;
        assert(r32(ram, 0x21e0 + i * 4) == expected);
    }
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
}

static void test_special_move_moves(void) {
    unsigned cases = 0;
    for (unsigned size_code = 0; size_code < 3; ++size_code)
        for (unsigned store = 0; store < 2; ++store)
            for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i) {
                const p2c_ea_case_t *ea = &p2c_memory_eas[i];
                uint16_t words[16] = {0};
                int mode, reg;
                ++cases;
                unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
                unsigned n = p2c_append_ea(words, 2, ea, &mode, &reg);
                words[0] = (uint16_t)(0x0e00 | (size_code << 6) | (mode << 3) | reg);
                words[1] = (uint16_t)(0x1000 | (store << 11)); /* D1, direction */
                p2c_result_t result =
                    p2c_run(words, n, ea, size, 0x00008001, 0xaabbccdd, store);
                assert(result.cycles == (store ? 5 : 7) + p2c_ciea(ea));
                if (store) {
                    uint32_t expected = size == 1 ? 0xdd : size == 2 ? 0xccdd : 0xaabbccdd;
                    assert(result.memory == expected);
                } else {
                    uint32_t expected = size == 1   ? 0xaabbcc01
                                        : size == 2 ? 0xaabb8001
                                                    : 0x00008001;
                    if (result.d1 != expected)
                        fprintf(stderr,
                                "MOVES load result mismatch ea=%u size=%u value=%08X "
                                "expected=%08X\n",
                                i, size, result.d1, expected);
                    assert(result.d1 == expected);
                }
                if (ea->form == P2C_EA_PI)
                    assert(result.a0 == 0x2000 + size);
                if (ea->form == P2C_EA_PD)
                    assert(result.a0 == 0x2010 - size);
            }
    /* Address-register destination is a separate MOVES result path: B/W are
     * sign-extended, while the bus direction remains SFC. */
    for (unsigned size_code = 0; size_code < 3; ++size_code) {
        unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
        uint16_t words[] = {(uint16_t)(0x0e00 | (size_code << 6) | 0x10), 0x9000};
        p2c_ea_case_t ea = {P2C_EA_AN, 0, 0, 0};
        p2c_result_t result =
            p2c_run(words, 2, &ea, size, size == 1 ? 0x00000080 : 0x00008001, 0, 0);
        uint32_t expected = size == 1 ? 0xffffff80 : size == 2 ? 0xffff8001 : 0x00008001;
        assert(result.cycles == 7 + p2c_ciea(&ea) && result.a1 == expected);
        ++cases;
    }
    assert(cases == 177); /* B/W/L × two directions × 29 EA classes, plus An loads. */
}

static void test_special_move_moves_function_codes(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    w16(ram, PC, 0x0e10); /* MOVES.B (A0),D1 */
    w16(ram, PC + 2, 0x1000);
    ram[0x2000] = 0x5a;
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    bus.access_timing = p2c_probe_fc;
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->a[0] = 0x2000;
    cpu->sfc = 3;
    p2c_fc_probe_t probe = {.address = 0x2000};
    p2c_fc_probe = &probe;
    assert(mc68k_execute(cpu, 1) == 9 && cpu->d[1] == 0x0000005a);
    assert(probe.seen == 1 && probe.fc == 3);
    w16(ram, PC, 0x0e10);
    w16(ram, PC + 2, 0x1800); /* MOVES.B D1,(A0) */
    cpu->pc = PC;
    cpu->d[1] = 0xa5;
    cpu->dfc = 5;
    probe.fc = 0;
    probe.seen = 0;
    assert(mc68k_execute(cpu, 1) == 7 && ram[0x2000] == 0xa5);
    assert(probe.seen == 1 && probe.fc == 5);
    /* Memory-indirect EA must use the MOVES function code for its pointer
     * fetch too, not merely for the final operand transfer. */
    w16(ram, PC, 0x0e30);
    w16(ram, PC + 2, 0x1000);
    w16(ram, PC + 4, 0x7111);
    w32(ram, 0x2000, 0x2400);
    ram[0x2400] = 0x80;
    cpu->pc = PC;
    cpu->a[0] = 0x2000;
    cpu->d[1] = 0;
    cpu->sfc = 2;
    probe.fc = 0;
    probe.seen = 0;
    assert(mc68k_execute(cpu, 1) > 0 && cpu->d[1] == 0x00000080);
    assert(probe.seen == 1 && probe.fc == 2);
    w16(ram, PC, 0x0e30);
    w16(ram, PC + 2, 0x1800);
    w16(ram, PC + 4, 0x7111);
    cpu->pc = PC;
    cpu->d[1] = 0x3c;
    cpu->dfc = 6;
    probe.fc = 0;
    probe.seen = 0;
    assert(mc68k_execute(cpu, 1) > 0 && ram[0x2400] == 0x3c);
    assert(probe.seen == 1 && probe.fc == 6);
    p2c_fc_probe = NULL;
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);
}

typedef struct {
    uint8_t *ram;
    mem_ctx_t *mem;
    mem_region_t *region;
    mc68k_state_t *cpu;
} p2c_machine_t;

static unsigned p2c_reset_peripheral_calls;

static void p2c_reset_peripheral_probe(void *ctx) {
    (void)ctx;
    ++p2c_reset_peripheral_calls;
}

static p2c_machine_t p2c_machine_create_model(mc68k_model_t model) {
    p2c_machine_t m = {0};
    m.ram = calloc(1, RAM_SIZE);
    assert(m.ram);
    w32(m.ram, 0, 0x8000);
    w32(m.ram, 4, PC);
    m.mem = mem_create();
    m.region = calloc(1, sizeof(*m.region));
    assert(m.mem && m.region);
    m.region->data = m.ram;
    m.region->size = RAM_SIZE;
    mem_add_region(m.mem, m.region);
    mc68k_bus_t bus = bus_make_hooks(m.mem);
    bus.reset_peripheral = p2c_reset_peripheral_probe;
    m.cpu = mc68k_create_model(&bus, model);
    assert(m.cpu);
    mc68k_reset(m.cpu);
    return m;
}

static p2c_machine_t p2c_machine_create(void) {
    return p2c_machine_create_model(MC68K_MODEL_68020);
}

static void p2c_machine_destroy(p2c_machine_t *m) {
    mc68k_destroy(m->cpu);
    mem_destroy(m->mem);
    free(m->region);
    free(m->ram);
}

static void test_rte_formats(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    w32(ram, 0, 0x8000);
    w32(ram, 4, PC);
    w16(ram, PC, 0x4e73);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    const struct {
        uint16_t format;
        int expected;
        uint32_t bytes;
    } cases[] = {
        {0, 21, 8}, {2, 21, 12}, {9, 32, 20}, {10, 43, 32}, {11, 92, 92},
    };
    enum { P2C_RTE_FORMAT_MANIFEST_CASES = 5 };
    assert(sizeof(cases) / sizeof(cases[0]) == P2C_RTE_FORMAT_MANIFEST_CASES);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cpu->pc = PC;
        cpu->sr = 0x2700;
        cpu->a[7] = 0x7000;
        w16(ram, 0x7000, 0x2700);
        w32(ram, 0x7002, 0x1200);
        w16(ram, 0x7006, cases[i].format << 12);
        assert(mc68k_execute(cpu, 1) == cases[i].expected);
        assert(cpu->pc == 0x1200 && cpu->a[7] == 0x7000 + cases[i].bytes);
    }
    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(ram);

    /* Format 1 (throwaway) RTE: frame[0] is popped from the current stack and
     * frame[1] from MSP.  Each row owns its opcode, initial registers, both
     * frame images, the format-error vector and every expected effect. */
    typedef struct {
        uint16_t sr;
        uint32_t pc;
        uint16_t format_vector;
    } p2c_rte_frame_t;
    typedef struct {
        const char *row, *branch;
        uint16_t opcode, sr;
        uint32_t a7, msp;
        p2c_rte_frame_t frame[2];
        uint32_t format_error_handler;
        int cycles;
        uint32_t pc, a7_after;
        bool check_stack_regs;
        uint32_t msp_after, ssp_after;
        bool check_error_frame;
        uint32_t error_frame_pc;
    } p2c_rte_throwaway_case_t;
    static const p2c_rte_throwaway_case_t p2c_rte_throwaway_manifest[] = {
        {"format 1 then format 0",
         "valid next frame",
         0x4e73,
         0x2000,
         0x7000,
         0x6000,
         {{0x3000, 0xDEADBEEF, 0x1000}, {0x2000, 0x1200, 0x0000}},
         0,
         37,
         0x1200,
         0x7008,
         true,
         0x6008,
         0x7008,
         false,
         0},
        {"format 1 then format error",
         "invalid next frame",
         0x4e73,
         0x2000,
         0x7000,
         0x6000,
         {{0x3000, 0xDEADBEEF, 0x1000}, {0x3000, 0, 0x3000}},
         0x1400,
         66,
         0x1400,
         0x5ff8,
         false,
         0,
         0,
         true,
         PC},
    };
    enum { P2C_RTE_THROWAWAY_MANIFEST_CASES = 2 };
    assert(sizeof(p2c_rte_throwaway_manifest) / sizeof(p2c_rte_throwaway_manifest[0]) ==
           P2C_RTE_THROWAWAY_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_RTE_THROWAWAY_MANIFEST_CASES; ++i) {
        const p2c_rte_throwaway_case_t *c = &p2c_rte_throwaway_manifest[i];
        p2c_machine_t m = p2c_machine_create();
        w16(m.ram, PC, c->opcode);
        w32(m.ram, 14 * 4, c->format_error_handler);
        m.cpu->sr = c->sr;
        m.cpu->a[7] = c->a7;
        m.cpu->msp = c->msp;
        const uint32_t frame_base[2] = {c->a7, c->msp};
        for (unsigned f = 0; f < 2; ++f) {
            w16(m.ram, frame_base[f], c->frame[f].sr);
            w32(m.ram, frame_base[f] + 2, c->frame[f].pc);
            w16(m.ram, frame_base[f] + 6, c->frame[f].format_vector);
        }
        assert(mc68k_execute(m.cpu, 1) == c->cycles);
        assert(m.cpu->pc == c->pc && m.cpu->a[7] == c->a7_after);
        if (c->check_stack_regs)
            assert(m.cpu->msp == c->msp_after && m.cpu->ssp == c->ssp_after);
        if (c->check_error_frame)
            assert(r32(m.ram, m.cpu->a[7] + 2) == c->error_frame_pc);
        p2c_machine_destroy(&m);
    }
}

/* UM20 §8.2.1--§8.2.5 values copied for control-EA rows.  The existing
 * p2c_* helpers deliberately omit PC forms where those forms are illegal
 * for their original instruction, so these remain separate oracles. */
static int p2c_control_calc(const p2c_ea_case_t *ea) {
    static const uint8_t basic[] = {2, 0, 0, 2, 4, 2, 4, 2, 4};
    if (ea->form != P2C_EA_FULL)
        return basic[ea->form];
    return p2c_cea(ea);
}

static int p2c_control_jump(const p2c_ea_case_t *ea) {
    /* The JEA table prints only (d16,An); PC_DI takes that value by the same
     * inference as the CIEA/FIEA tables (see p2c_ciea). */
    static const uint8_t basic[] = {2, 0, 0, 4, 6, 2, 2, 4, 6};
    if (ea->form != P2C_EA_FULL)
        return basic[ea->form];
    /* JEA has the same full-extension Cache Case rows as CEA on 68020. */
    return p2c_cea(ea);
}

static int p2c_control_fiea_word(const p2c_ea_case_t *ea) {
    /* PC_DI takes the (d16,An) value (see p2c_ciea). */
    static const uint8_t basic[] = {4, 0, 0, 5, 9, 5, 6, 5, 9};
    if (ea->form != P2C_EA_FULL)
        return basic[ea->form];
    return p2c_fea(ea) + 2;
}

/* Every control-legal EA timing class (no (An)+/-(An)): 5 memory basic forms,
 * 2 PC-relative basic forms, and the 22 full-extension classes.  The list is
 * explicit so a filter change cannot silently shrink the covered set. */
static const p2c_ea_case_t p2c_control_eas[] = {
    {P2C_EA_AN, 0, 0, 0},       {P2C_EA_DI, 0, 0, 0},   {P2C_EA_BRIEF, 0, 0, 0},
    {P2C_EA_AW, 0, 0, 0},       {P2C_EA_AL, 0, 0, 0},   {P2C_EA_PC_DI, 0, 0, 0},
    {P2C_EA_PC_BRIEF, 0, 0, 0}, {P2C_EA_FULL, 1, 0, 0}, {P2C_EA_FULL, 2, 0, 0},
    {P2C_EA_FULL, 2, 0, 1},     {P2C_EA_FULL, 3, 0, 0}, {P2C_EA_FULL, 1, 1, 0},
    {P2C_EA_FULL, 1, 2, 0},     {P2C_EA_FULL, 1, 3, 0}, {P2C_EA_FULL, 2, 1, 0},
    {P2C_EA_FULL, 2, 2, 0},     {P2C_EA_FULL, 2, 3, 0}, {P2C_EA_FULL, 3, 1, 0},
    {P2C_EA_FULL, 3, 2, 0},     {P2C_EA_FULL, 3, 3, 0}, {P2C_EA_FULL, 1, 5, 0},
    {P2C_EA_FULL, 1, 6, 0},     {P2C_EA_FULL, 1, 7, 0}, {P2C_EA_FULL, 2, 5, 0},
    {P2C_EA_FULL, 2, 6, 0},     {P2C_EA_FULL, 2, 7, 0}, {P2C_EA_FULL, 3, 5, 0},
    {P2C_EA_FULL, 3, 6, 0},     {P2C_EA_FULL, 3, 7, 0},
};
enum { P2C_CONTROL_EA_CASES = 29 };

/* UM20 §8.2.15 — Bcc/BRA and DBcc dynamic paths. */
typedef struct {
    const char *row;
    uint16_t opcode, extension;
    uint16_t sr;
    int cycles;
    uint32_t pc;
} p2c_branch_case_t;
static const p2c_branch_case_t p2c_branch_manifest[] = {
    {"Bcc.B taken", 0x6606, 0, 0x2700, 6, PC + 8},
    {"Bcc.W taken", 0x6600, 6, 0x2700, 6, PC + 8},
    {"Bcc.L taken", 0x66ff, 6, 0x2700, 6, PC + 8},
    {"Bcc.B not taken", 0x6606, 0, 0x2704, 4, PC + 2},
    {"Bcc.W not taken", 0x6600, 6, 0x2704, 6, PC + 4},
    {"Bcc.L not taken", 0x66ff, 0, 0x2704, 6, PC + 6},
    {"BRA.B alias", 0x6006, 0, 0x2700, 6, PC + 8},
    {"BRA.W alias", 0x6000, 6, 0x2700, 6, PC + 8},
    {"BRA.L alias", 0x60ff, 6, 0x2700, 6, PC + 8},
};
enum { P2C_BRANCH_MANIFEST_CASES = 9 };

typedef struct {
    const char *row;
    uint16_t opcode, extension, d0;
    int cycles;
    uint32_t pc;
} p2c_dbcc_case_t;
static const p2c_dbcc_case_t p2c_dbcc_manifest[] = {
    {"DBT condition true", 0x50c8, 6, 3, 6, PC + 4},
    {"DBF branch", 0x51c8, 6, 1, 6, PC + 8},
    {"DBF expired", 0x51c8, 6, 0, 10, PC + 4},
};
enum { P2C_DBCC_MANIFEST_CASES = 3 };

static void test_branch_manifest(void) {
    assert(sizeof(p2c_branch_manifest) / sizeof(p2c_branch_manifest[0]) ==
           P2C_BRANCH_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_BRANCH_MANIFEST_CASES; ++i) {
        const p2c_branch_case_t *c = &p2c_branch_manifest[i];
        p2c_machine_t m = p2c_machine_create();
        w16(m.ram, PC, c->opcode);
        w16(m.ram, PC + 2, c->extension);
        if ((c->opcode & 0xff) == 0xff)
            w32(m.ram, PC + 2, c->extension);
        m.cpu->sr = c->sr;
        assert(mc68k_execute(m.cpu, 1) == c->cycles);
        assert(m.cpu->pc == c->pc);
        p2c_machine_destroy(&m);
    }
    assert(sizeof(p2c_dbcc_manifest) / sizeof(p2c_dbcc_manifest[0]) == P2C_DBCC_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_DBCC_MANIFEST_CASES; ++i) {
        const p2c_dbcc_case_t *c = &p2c_dbcc_manifest[i];
        p2c_machine_t m = p2c_machine_create();
        w16(m.ram, PC, c->opcode);
        w16(m.ram, PC + 2, c->extension);
        m.cpu->d[0] = c->d0;
        assert(mc68k_execute(m.cpu, 1) == c->cycles);
        assert((uint16_t)m.cpu->d[0] == (uint16_t)(c->d0 - (c->opcode == 0x51c8)));
        assert(m.cpu->pc == c->pc);
        p2c_machine_destroy(&m);
    }
}

/* UM20 §8.2.16. */
static void test_control_manifest(void) {
    static const struct {
        const char *row;
        uint16_t opcode, extension;
        unsigned length;
        int cycles;
        uint32_t pc;
    } bsr[] = {
        {"BSR.B", 0x6106, 0, 2, 7, PC + 8},
        {"BSR.W", 0x6100, 6, 4, 7, PC + 8},
        {"BSR.L", 0x61ff, 6, 6, 7, PC + 8},
    };
    enum { P2C_BSR_MANIFEST_CASES = 3 };
    assert(sizeof(bsr) / sizeof(bsr[0]) == P2C_BSR_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_BSR_MANIFEST_CASES; ++i) {
        p2c_machine_t m = p2c_machine_create();
        w16(m.ram, PC, bsr[i].opcode);
        w16(m.ram, PC + 2, bsr[i].extension);
        if ((bsr[i].opcode & 0xff) == 0xff)
            w32(m.ram, PC + 2, 6);
        assert(mc68k_execute(m.cpu, 1) == bsr[i].cycles);
        assert(m.cpu->pc == bsr[i].pc && m.cpu->a[7] == 0x7ffc &&
               r32(m.ram, 0x7ffc) == PC + bsr[i].length);
        p2c_machine_destroy(&m);
    }

    /* CAS B/W/L, success/failure, every memory-alterable basic/full class. */
    typedef struct {
        const char *row;
        uint16_t opcode;
        unsigned size;
        bool success;
        int cycles;
    } p2c_cas_case_t;
    static const p2c_cas_case_t p2c_cas_manifest[] = {
        {"CAS.B fail", 0x0ac0, 1, false, 12}, {"CAS.B success", 0x0ac0, 1, true, 15},
        {"CAS.W fail", 0x0cc0, 2, false, 12}, {"CAS.W success", 0x0cc0, 2, true, 15},
        {"CAS.L fail", 0x0ec0, 4, false, 12}, {"CAS.L success", 0x0ec0, 4, true, 15},
    };
    enum { P2C_CAS_MANIFEST_CASES = 6 };
    assert(sizeof(p2c_cas_manifest) / sizeof(p2c_cas_manifest[0]) == P2C_CAS_MANIFEST_CASES);
    assert(sizeof(p2c_memory_eas) / sizeof(p2c_memory_eas[0]) == P2C_MEMORY_EA_CASES);
    for (unsigned k = 0; k < P2C_CAS_MANIFEST_CASES; ++k)
        for (unsigned i = 0; i < P2C_MEMORY_EA_CASES; ++i) {
            const p2c_cas_case_t *c = &p2c_cas_manifest[k];
            const p2c_ea_case_t *ea = &p2c_memory_eas[i];
            unsigned size = c->size;
            uint16_t words[16] = {0};
            int mode, reg;
            unsigned n = p2c_append_ea(words, 2, ea, &mode, &reg);
            words[0] = (uint16_t)(c->opcode | (mode << 3) | reg);
            words[1] = 0x0081; /* Du=D2, Dc=D1 */
            p2c_machine_t m = p2c_machine_create();
            for (unsigned w = 0; w < n; ++w)
                w16(m.ram, PC + w * 2, words[w]);
            m.cpu->a[0] = ea->form == P2C_EA_PD ? 0x2010 : 0x2000;
            m.cpu->d[1] = 0x11223344;
            m.cpu->d[2] = 0xa1b2c3d4;
            uint32_t value = c->success ? 0x11223344 : 0x55667788;
            p2c_install_ea_data(m.ram, ea, size, value);
            assert(mc68k_execute(m.cpu, 1) == c->cycles + p2c_ciea(ea));
            uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : UINT32_MAX;
            assert((m.cpu->sr & CCR_Z) != 0 ? c->success : !c->success);
            if (c->success) {
                uint32_t address = p2c_ea_address(ea, size);
                uint32_t actual = size == 1   ? m.ram[address]
                                  : size == 2 ? r16(m.ram, address)
                                              : r32(m.ram, address);
                assert((actual & mask) == (m.cpu->d[2] & mask));
            } else
                assert((m.cpu->d[1] & mask) == (value & mask));
            p2c_machine_destroy(&m);
        }

    /* CAS2 W/L: first fail, second fail, and both success. */
    typedef struct {
        const char *row;
        uint16_t opcode;
        unsigned size, path;
        int cycles;
    } p2c_cas2_case_t;
    static const p2c_cas2_case_t p2c_cas2_manifest[] = {
        {"CAS2.W first fail", 0x0cfc, 2, 0, 22},  {"CAS2.W second fail", 0x0cfc, 2, 1, 22},
        {"CAS2.W success", 0x0cfc, 2, 2, 25},     {"CAS2.L first fail", 0x0efc, 4, 0, 22},
        {"CAS2.L second fail", 0x0efc, 4, 1, 22}, {"CAS2.L success", 0x0efc, 4, 2, 25},
    };
    enum { P2C_CAS2_MANIFEST_CASES = 6 };
    assert(sizeof(p2c_cas2_manifest) / sizeof(p2c_cas2_manifest[0]) == P2C_CAS2_MANIFEST_CASES);
    for (unsigned k = 0; k < P2C_CAS2_MANIFEST_CASES; ++k) {
        const p2c_cas2_case_t *c = &p2c_cas2_manifest[k];
        unsigned size = c->size;
        p2c_machine_t m = p2c_machine_create();
        w16(m.ram, PC, c->opcode);
        w16(m.ram, PC + 2, 0x8081);
        w16(m.ram, PC + 4, 0x9103); /* A0/D1/D2, A1/D3/D4 */
        m.cpu->a[0] = 0x2000;
        m.cpu->a[1] = 0x2010;
        m.cpu->d[1] = 0x11112222;
        m.cpu->d[2] = 0xa1a2a3a4;
        m.cpu->d[3] = 0x33334444;
        m.cpu->d[4] = 0xb1b2b3b4;
        uint32_t v1 = c->path == 0 ? 0x55556666 : 0x11112222;
        uint32_t v2 = c->path == 1 ? 0x77778888 : 0x33334444;
        p2c_install_ea_data(m.ram, &(p2c_ea_case_t){P2C_EA_AN, 0, 0, 0}, size, v1);
        if (size == 2)
            w16(m.ram, 0x2010, (uint16_t)v2);
        else
            w32(m.ram, 0x2010, v2);
        assert(mc68k_execute(m.cpu, 1) == c->cycles);
        if (c->path == 2) {
            if (size == 2) {
                assert(r16(m.ram, 0x2000) == 0xa3a4 && r16(m.ram, 0x2010) == 0xb3b4);
            } else {
                assert(r32(m.ram, 0x2000) == 0xa1a2a3a4 && r32(m.ram, 0x2010) == 0xb1b2b3b4);
            }
        } else {
            uint32_t mask = size == 2 ? 0xffffu : UINT32_MAX;
            assert((m.cpu->d[1] & mask) == (v1 & mask) && (m.cpu->d[3] & mask) == (v2 & mask));
        }
        p2c_machine_destroy(&m);
    }
}

/* Runners return 1 once every assertion has passed, so the case counters cannot
 * be inflated by a runner that returns early.  A cycle mismatch names the
 * manifest row/branch and EA form before the assert aborts. */
static void p2c_report_cycles(const char *row, const char *branch, const p2c_ea_case_t *ea,
                              int actual, int expected) {
    if (actual != expected)
        fprintf(stderr, "%s (%s) form=%d bd=%d iis=%d bs=%d: cycles %d, expected %d\n", row,
                branch, ea ? (int)ea->form : -1, ea ? ea->bd : 0, ea ? ea->iis : 0,
                ea ? ea->bs : 0, actual, expected);
}

/* CHK.L normal path: Dn plus every data-addressing memory/PC timing class.
 * A row is a fixed Cartesian product with p2c_chk_l_ea_groups; `cases` is the
 * exact number of completed runs (1 Dn + 29 memory + 2 PC-relative). */
typedef struct {
    const char *row, *branch;
    uint16_t opcode;
    unsigned size;
    uint32_t d1, bound, d1_after;
    bool n_flag;
    int cycles;
    unsigned cases;
} p2c_chk_case_t;
static const p2c_chk_case_t p2c_chk_manifest[] = {
    {"CHK.L", "in range, no trap", 0x4300, 4, 3, 5, 3, false, 8, 32},
};
enum { P2C_CHK_MANIFEST_CASES = 1, P2C_CHK_EXECUTED_CASES = 32 };

typedef struct {
    const char *row;
    const p2c_ea_case_t *entries;
    unsigned count;
} p2c_ea_group_t;
static const p2c_ea_group_t p2c_chk_l_ea_groups[] = {
    {"memory EA", p2c_memory_eas, P2C_MEMORY_EA_CASES},
    {"PC-relative EA", p2c_pc_relative_eas, P2C_PC_RELATIVE_EA_CASES},
};
enum { P2C_CHK_L_EA_GROUPS = 2 };

/* ea == NULL selects the Dn source form (CHK.L D0,D1).  The EA column below
 * is the long-operand FEA oracle, so a CHK.W row needs its own column. */
static unsigned p2c_run_chk_case(const p2c_chk_case_t *c, const p2c_ea_case_t *ea) {
    assert(c->size == 4);
    uint16_t words[16] = {0};
    int mode = 0, reg = 0;
    unsigned n = 1;
    if (ea)
        n = p2c_append_ea(words, 1, ea, &mode, &reg);
    words[0] = (uint16_t)(c->opcode | (mode << 3) | reg);
    p2c_machine_t m = p2c_machine_create();
    for (unsigned w = 0; w < n; ++w)
        w16(m.ram, PC + w * 2, words[w]);
    m.cpu->d[1] = c->d1;
    int ea_extra = 0;
    if (ea) {
        m.cpu->a[0] = p2c_ea_a0(ea);
        p2c_install_ea_data(m.ram, ea, c->size, c->bound);
        ea_extra = p2c_fea(ea);
    } else {
        m.cpu->d[0] = c->bound;
    }
    int cycles = mc68k_execute(m.cpu, 1);
    p2c_report_cycles(c->row, c->branch, ea, cycles, c->cycles + ea_extra);
    assert(cycles == c->cycles + ea_extra);
    assert(m.cpu->d[1] == c->d1_after && ((m.cpu->sr & CCR_N) != 0) == c->n_flag);
    assert(m.cpu->pc == PC + n * 2);
    p2c_machine_destroy(&m);
    return 1;
}

/* CHK2 in-bounds path, all control EA classes for B/W/L.  Extension 0x1800 is
 * CHK2 with D1 as the compared register; D0 stays zero as the index. */
typedef struct {
    const char *row, *branch;
    uint16_t opcode, extension;
    unsigned size;
    uint32_t a0, d0, d1, lower, upper;
    bool carry, zero;
    int cycles;
    unsigned cases;
} p2c_chk2_case_t;
static const p2c_chk2_case_t p2c_chk2_manifest[] = {
    {"CHK2.B", "in bounds, no trap", 0x00c0, 0x1800, 1, 0x2000, 0, 3, 2, 4, false, false, 18,
     29},
    {"CHK2.W", "in bounds, no trap", 0x02c0, 0x1800, 2, 0x2000, 0, 3, 2, 4, false, false, 18,
     29},
    {"CHK2.L", "in bounds, no trap", 0x04c0, 0x1800, 4, 0x2000, 0, 3, 2, 4, false, false, 18,
     29},
};
enum { P2C_CHK2_MANIFEST_CASES = 3, P2C_CHK2_EXECUTED_CASES = 87 };

static void p2c_install_bounds(uint8_t *ram, uint32_t address, unsigned size, uint32_t lower,
                               uint32_t upper) {
    if (size == 1) {
        ram[address] = (uint8_t)lower;
        ram[address + 1] = (uint8_t)upper;
    } else if (size == 2) {
        w16(ram, address, (uint16_t)lower);
        w16(ram, address + 2, (uint16_t)upper);
    } else {
        w32(ram, address, lower);
        w32(ram, address + 4, upper);
    }
}

static unsigned p2c_run_chk2_case(const p2c_chk2_case_t *c, const p2c_ea_case_t *ea) {
    uint16_t words[16] = {0};
    int mode, reg;
    unsigned n = p2c_append_ea(words, 2, ea, &mode, &reg);
    words[0] = (uint16_t)(c->opcode | (mode << 3) | reg);
    words[1] = c->extension;
    p2c_rebase_pc_relative(words, 2, ea);
    p2c_machine_t m = p2c_machine_create();
    for (unsigned w = 0; w < n; ++w)
        w16(m.ram, PC + w * 2, words[w]);
    m.cpu->a[0] = c->a0;
    m.cpu->d[0] = c->d0;
    m.cpu->d[1] = c->d1;
    p2c_install_ea_pointer(m.ram, ea);
    p2c_install_bounds(m.ram, p2c_ea_address(ea, c->size), c->size, c->lower, c->upper);
    const int expected = c->cycles + p2c_control_fiea_word(ea);
    int cycles = mc68k_execute(m.cpu, 1);
    p2c_report_cycles(c->row, c->branch, ea, cycles, expected);
    assert(cycles == expected);
    assert(((m.cpu->sr & CCR_C) != 0) == c->carry && ((m.cpu->sr & CCR_Z) != 0) == c->zero);
    assert(m.cpu->d[1] == c->d1 && m.cpu->pc == PC + n * 2);
    p2c_machine_destroy(&m);
    return 1;
}

/* JMP/JSR/LEA/PEA through p2c_control_eas.  The expected result of each row is
 * data: where PC ends up, whether A0 receives the EA, and what is pushed. */
typedef enum { P2C_PUSH_NONE, P2C_PUSH_RETURN_PC, P2C_PUSH_EA_ADDRESS } p2c_push_t;
typedef struct {
    const char *row, *branch;
    uint16_t opcode;
    int cycles;
    bool jump_ea;    /* cycle column: JEA (JMP/JSR) or CEA (LEA/PEA) */
    uint32_t a0, a7; /* initial */
    bool pc_is_ea, a0_is_ea;
    p2c_push_t push;
    uint32_t a7_after;
    unsigned cases;
} p2c_control_op_case_t;
static const p2c_control_op_case_t p2c_control_op_manifest[] = {
    {"JMP", "jump to EA", 0x4ec0, 4, true, 0x2000, 0x8000, true, false, P2C_PUSH_NONE, 0x8000,
     29},
    {"JSR", "push return, jump to EA", 0x4e80, 5, true, 0x2000, 0x8000, true, false,
     P2C_PUSH_RETURN_PC, 0x7ffc, 29},
    {"LEA", "load EA into A0", 0x41c0, 2, false, 0x2000, 0x8000, false, true, P2C_PUSH_NONE,
     0x8000, 29},
    {"PEA", "push EA address", 0x4840, 5, false, 0x2000, 0x8000, false, false,
     P2C_PUSH_EA_ADDRESS, 0x7ffc, 29},
};
enum { P2C_CONTROL_OP_MANIFEST_CASES = 4, P2C_CONTROL_OP_EXECUTED_CASES = 116 };

static unsigned p2c_run_control_op_case(const p2c_control_op_case_t *c,
                                        const p2c_ea_case_t *ea) {
    uint16_t words[16] = {0};
    int mode, reg;
    unsigned n = p2c_append_ea(words, 1, ea, &mode, &reg);
    words[0] = (uint16_t)(c->opcode | (mode << 3) | reg);
    const uint32_t address = p2c_ea_address(ea, 2);
    p2c_machine_t m = p2c_machine_create();
    for (unsigned w = 0; w < n; ++w)
        w16(m.ram, PC + w * 2, words[w]);
    m.cpu->a[0] = c->a0;
    m.cpu->a[7] = c->a7;
    p2c_install_ea_pointer(m.ram, ea);
    const int expected = c->cycles + (c->jump_ea ? p2c_control_jump(ea) : p2c_control_calc(ea));
    int cycles = mc68k_execute(m.cpu, 1);
    p2c_report_cycles(c->row, c->branch, ea, cycles, expected);
    assert(cycles == expected);
    assert(m.cpu->pc == (c->pc_is_ea ? address : PC + n * 2));
    assert(m.cpu->a[0] == (c->a0_is_ea ? address : c->a0));
    assert(m.cpu->a[7] == c->a7_after);
    if (c->push == P2C_PUSH_RETURN_PC)
        assert(r32(m.ram, m.cpu->a[7]) == PC + n * 2);
    if (c->push == P2C_PUSH_EA_ADDRESS)
        assert(r32(m.ram, m.cpu->a[7]) == address);
    p2c_machine_destroy(&m);
    return 1;
}

static void test_control_ea_manifest(void) {
    assert(sizeof(p2c_control_eas) / sizeof(p2c_control_eas[0]) == P2C_CONTROL_EA_CASES);
    assert(sizeof(p2c_pc_relative_eas) / sizeof(p2c_pc_relative_eas[0]) ==
           P2C_PC_RELATIVE_EA_CASES);
    assert(sizeof(p2c_chk_l_ea_groups) / sizeof(p2c_chk_l_ea_groups[0]) == P2C_CHK_L_EA_GROUPS);
    assert(sizeof(p2c_memory_eas) / sizeof(p2c_memory_eas[0]) == P2C_MEMORY_EA_CASES);

    assert(sizeof(p2c_chk_manifest) / sizeof(p2c_chk_manifest[0]) == P2C_CHK_MANIFEST_CASES);
    unsigned total = 0;
    for (unsigned k = 0; k < P2C_CHK_MANIFEST_CASES; ++k) {
        unsigned executed = 0;
        executed += p2c_run_chk_case(&p2c_chk_manifest[k], NULL);
        for (unsigned g = 0; g < P2C_CHK_L_EA_GROUPS; ++g)
            for (unsigned i = 0; i < p2c_chk_l_ea_groups[g].count; ++i) {
                executed +=
                    p2c_run_chk_case(&p2c_chk_manifest[k], &p2c_chk_l_ea_groups[g].entries[i]);
            }
        assert(executed == p2c_chk_manifest[k].cases);
        total += executed;
    }
    assert(total == P2C_CHK_EXECUTED_CASES);

    assert(sizeof(p2c_chk2_manifest) / sizeof(p2c_chk2_manifest[0]) == P2C_CHK2_MANIFEST_CASES);
    total = 0;
    for (unsigned k = 0; k < P2C_CHK2_MANIFEST_CASES; ++k) {
        unsigned executed = 0;
        for (unsigned i = 0; i < P2C_CONTROL_EA_CASES; ++i) {
            executed += p2c_run_chk2_case(&p2c_chk2_manifest[k], &p2c_control_eas[i]);
        }
        assert(executed == p2c_chk2_manifest[k].cases);
        total += executed;
    }
    assert(total == P2C_CHK2_EXECUTED_CASES);

    assert(sizeof(p2c_control_op_manifest) / sizeof(p2c_control_op_manifest[0]) ==
           P2C_CONTROL_OP_MANIFEST_CASES);
    total = 0;
    for (unsigned k = 0; k < P2C_CONTROL_OP_MANIFEST_CASES; ++k) {
        unsigned executed = 0;
        for (unsigned i = 0; i < P2C_CONTROL_EA_CASES; ++i) {
            executed +=
                p2c_run_control_op_case(&p2c_control_op_manifest[k], &p2c_control_eas[i]);
        }
        assert(executed == p2c_control_op_manifest[k].cases);
        total += executed;
    }
    assert(total == P2C_CONTROL_OP_EXECUTED_CASES);
}

/* FIEA/CIEA 列へ到達するすべての production path で、68020 と 68030 の (d16,PC) を検査する。
 * 命令の絶対 base cycle は不要である。同じ命令を (An)、(d16,An)、(d16,PC) で実行し、両方の
 * d16 形式が manual の行差分だけ (An) 形式と異なることを確認するため、期待値は印字された
 * (An) と (bd,An)/(d16,An) の行だけから得る。Cache Case 列は UM20 §8.2.2 の FIEA
 * (An) 4/4、(bd,An) 5/7、§8.2.4 の CIEA (An) 2/4、(bd,An) 4/6、UM30 §11.6.2 の
 * FIEA (An) 3/4、(d16,An) 4/6、§11.6.4 の CIEA (An) 2/4、(d16,An) 4/6
 * （tests/data/cpu/um30_11_6_icache_case.csv）による。 */
typedef struct {
    mc68k_model_t model;
    const char *source;
    /* [word, long].  The 68030 CIEA fields document UM30 §11.6.4 but are not
     * executed: every 68030 CIEA path is still flat (see only_68020). */
    uint8_t fiea_an[2], fiea_d16[2], ciea_an[2], ciea_d16[2];
} p2c_imm_oracle_t;
static const p2c_imm_oracle_t p2c_imm_oracles[] = {
    {MC68K_MODEL_68020, "UM20 8.2.2/8.2.4", {4, 4}, {5, 7}, {2, 4}, {4, 6}},
    {MC68K_MODEL_68030, "UM30 11.6.2/11.6.4", {3, 4}, {4, 6}, {2, 4}, {4, 6}},
};
enum { P2C_IMM_ORACLE_CASES = 2 };

typedef struct {
    const char *row, *branch;
    uint16_t opcode;    /* EA field zero */
    uint16_t prefix[2]; /* words between the opcode and the EA extension */
    unsigned prefix_count;
    bool calc_column; /* CIEA (true) or FIEA (false) */
    bool long_column; /* immediate/operand width column */
    bool only_68020;  /* 68030 timing of this path is still flat (no EA cost) */
} p2c_imm_path_t;
static const p2c_imm_path_t p2c_imm_path_manifest[] = {
    {"CMPI.W", "FIEA word", 0x0c40, {1, 0}, 1, false, false, false},
    {"CMPI.L", "FIEA long", 0x0c80, {0, 1}, 2, false, true, false},
    {"BTST #imm", "FIEA word", 0x0800, {0, 0}, 1, false, false, false},
    {"MULU.L", "FIEA word", 0x4c00, {0x1000, 0}, 1, false, false, true},
    {"DIVU.L", "FIEA word", 0x4c40, {0x1001, 0}, 1, false, false, true},
    {"CHK2.W", "FIEA word", 0x02c0, {0x1800, 0}, 1, false, false, true},
    {"BFEXTU", "CIEA word", 0xe9c0, {0x1000, 0}, 1, true, false, true},
    {"MOVEM.W to regs", "CIEA word", 0x4c80, {0x0001, 0}, 1, true, false, true},
};
enum { P2C_IMM_PATH_CASES = 8, P2C_IMM_EXECUTED_CASES = 11 };

typedef enum { P2C_IMM_EA_AI, P2C_IMM_EA_DI, P2C_IMM_EA_PC_DI, P2C_IMM_EA_FORMS } p2c_imm_ea_t;

static int p2c_run_imm_path(mc68k_model_t model, const p2c_imm_path_t *path,
                            p2c_imm_ea_t form) {
    /* Both d16 forms address 0x2100 (A0 = 0x2000 + 0x100); (An) addresses
     * 0x2000.  The same operand words sit at both so no path traps: CHK2.W
     * bounds are 0..4 against D1 = 3, and a long divisor is 4. */
    static const uint16_t operand[] = {0x0000, 0x0004};
    p2c_machine_t m = p2c_machine_create_model(model);
    uint16_t words[8] = {0};
    unsigned n = 1;
    for (unsigned i = 0; i < path->prefix_count; ++i)
        words[n++] = path->prefix[i];
    uint16_t field = 0x10; /* (An) */
    if (form == P2C_IMM_EA_DI) {
        field = 0x28;
        words[n++] = 0x0100;
    }
    if (form == P2C_IMM_EA_PC_DI) {
        field = 0x3a;
        words[n] = (uint16_t)(P2C_PC_DI_TARGET - (PC + n * 2));
        ++n;
    }
    words[0] = (uint16_t)(path->opcode | field);
    for (unsigned w = 0; w < n; ++w)
        w16(m.ram, PC + w * 2, words[w]);
    for (unsigned w = 0; w < 2; ++w) {
        w16(m.ram, 0x2000 + w * 2, operand[w]);
        w16(m.ram, P2C_PC_DI_TARGET + w * 2, operand[w]);
    }
    m.cpu->a[0] = 0x2000;
    m.cpu->d[1] = 3;
    int cycles = mc68k_execute(m.cpu, 1);
    assert(m.cpu->pc == PC + n * 2);
    p2c_machine_destroy(&m);
    return cycles;
}

static void test_pc_di_immediate_paths(void) {
    assert(sizeof(p2c_imm_oracles) / sizeof(p2c_imm_oracles[0]) == P2C_IMM_ORACLE_CASES);
    assert(sizeof(p2c_imm_path_manifest) / sizeof(p2c_imm_path_manifest[0]) ==
           P2C_IMM_PATH_CASES);
    unsigned executed = 0;
    for (unsigned o = 0; o < P2C_IMM_ORACLE_CASES; ++o)
        for (unsigned k = 0; k < P2C_IMM_PATH_CASES; ++k) {
            const p2c_imm_oracle_t *oracle = &p2c_imm_oracles[o];
            const p2c_imm_path_t *path = &p2c_imm_path_manifest[k];
            if (path->only_68020 && oracle->model != MC68K_MODEL_68020)
                continue;
            const unsigned col = path->long_column ? 1 : 0;
            const int delta = path->calc_column ? oracle->ciea_d16[col] - oracle->ciea_an[col]
                                                : oracle->fiea_d16[col] - oracle->fiea_an[col];
            int cycles[P2C_IMM_EA_FORMS];
            for (unsigned f = 0; f < P2C_IMM_EA_FORMS; ++f)
                cycles[f] = p2c_run_imm_path(oracle->model, path, (p2c_imm_ea_t)f);
            if (cycles[P2C_IMM_EA_DI] != cycles[P2C_IMM_EA_AI] + delta ||
                cycles[P2C_IMM_EA_PC_DI] != cycles[P2C_IMM_EA_AI] + delta)
                fprintf(stderr,
                        "%s %s (%s): (An) %d, (d16,An) %d, (d16,PC) %d, expected delta %d\n",
                        oracle->source, path->row, path->branch, cycles[P2C_IMM_EA_AI],
                        cycles[P2C_IMM_EA_DI], cycles[P2C_IMM_EA_PC_DI], delta);
            assert(cycles[P2C_IMM_EA_DI] == cycles[P2C_IMM_EA_AI] + delta);
            assert(cycles[P2C_IMM_EA_PC_DI] == cycles[P2C_IMM_EA_AI] + delta);
            ++executed;
        }
    /* 8 paths on 68020; on 68030 only CMPI.W/L and BTST #imm reach the EA cost. */
    assert(executed == P2C_IMM_EXECUTED_CASES);
}

/* Remaining non-EA rows of UM20 §8.2.16, then §8.2.17. */
static void test_system_manifest(void) {
    static const struct {
        const char *row;
        uint16_t opcode;
        int cycles;
        uint32_t pc;
    } simple[] = {
        {"LINK.W", 0x4e56, 5, PC + 4}, {"NOP", 0x4e71, 2, PC + 2},  {"RTR", 0x4e77, 14, 0x1234},
        {"RTS", 0x4e75, 10, 0x1234},   {"UNLK", 0x4e5e, 6, PC + 2},
    };
    enum { P2C_SYSTEM_MANIFEST_CASES = 5 };
    assert(sizeof(simple) / sizeof(simple[0]) == P2C_SYSTEM_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_SYSTEM_MANIFEST_CASES; ++i) {
        p2c_machine_t m = p2c_machine_create();
        w16(m.ram, PC, simple[i].opcode);
        if (simple[i].opcode == 0x4e56) {
            w16(m.ram, PC + 2, 0xfff8);
            m.cpu->a[6] = 0x12345678;
        }
        if (simple[i].opcode == 0x4e77) {
            w16(m.ram, 0x8000, 0x0015);
            w32(m.ram, 0x8002, 0x1234);
        }
        if (simple[i].opcode == 0x4e75)
            w32(m.ram, 0x8000, 0x1234);
        if (simple[i].opcode == 0x4e5e) {
            m.cpu->a[6] = 0x7000;
            w32(m.ram, 0x7000, 0x12345678);
        }
        assert(mc68k_execute(m.cpu, 1) == simple[i].cycles);
        assert(m.cpu->pc == simple[i].pc);
        if (simple[i].opcode == 0x4e56)
            assert(m.cpu->a[6] == 0x7ffc && m.cpu->a[7] == 0x7ff4);
        if (simple[i].opcode == 0x4e77)
            assert(m.cpu->pc == 0x1234 && m.cpu->a[7] == 0x8006 && (m.cpu->sr & 0x1f) == 0x15);
        if (simple[i].opcode == 0x4e75)
            assert(m.cpu->pc == 0x1234 && m.cpu->a[7] == 0x8004);
        if (simple[i].opcode == 0x4e5e)
            assert(m.cpu->a[6] == 0x12345678 && m.cpu->a[7] == 0x7004);
        p2c_machine_destroy(&m);
    }
    /* RESET and STOP supervisor-success paths. */
    static const struct {
        const char *row;
        uint16_t opcode, extension;
        int cycles;
        uint32_t pc;
        uint16_t sr;
        cpu_exec_state_t exec_state;
    } p2c_reset_stop_manifest[] = {
        {"RESET", 0x4e70, 0, 518, PC + 2, 0x2700, CPU_NORMAL},
        {"STOP", 0x4e72, 0x2700, 8, PC + 4, 0x2700, CPU_STOPPED},
    };
    enum { P2C_RESET_STOP_MANIFEST_CASES = 2 };
    assert(sizeof(p2c_reset_stop_manifest) / sizeof(p2c_reset_stop_manifest[0]) ==
           P2C_RESET_STOP_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_RESET_STOP_MANIFEST_CASES; ++i) {
        p2c_machine_t m = p2c_machine_create();
        w16(m.ram, PC, p2c_reset_stop_manifest[i].opcode);
        w16(m.ram, PC + 2, p2c_reset_stop_manifest[i].extension);
        p2c_reset_peripheral_calls = 0;
        assert(mc68k_execute(m.cpu, 1) == p2c_reset_stop_manifest[i].cycles);
        assert(m.cpu->pc == p2c_reset_stop_manifest[i].pc &&
               m.cpu->sr == p2c_reset_stop_manifest[i].sr &&
               m.cpu->exec_state == p2c_reset_stop_manifest[i].exec_state);
        assert(p2c_reset_peripheral_calls == (p2c_reset_stop_manifest[i].opcode == 0x4e70));
        p2c_machine_destroy(&m);
    }
    static const struct {
        const char *row;
        uint16_t opcode, sr;
        int cycles;
        uint32_t pc;
    } trapv[] = {
        {"TRAPV clear", 0x4e76, 0x2700, 4, PC + 2},
        {"TRAPV set", 0x4e76, 0x2702, 25, 0x1400},
    };
    enum { P2C_TRAPV_MANIFEST_CASES = 2 };
    assert(sizeof(trapv) / sizeof(trapv[0]) == P2C_TRAPV_MANIFEST_CASES);
    for (unsigned i = 0; i < P2C_TRAPV_MANIFEST_CASES; ++i) {
        p2c_machine_t m = p2c_machine_create();
        w16(m.ram, PC, trapv[i].opcode);
        w32(m.ram, 7 * 4, 0x1400);
        m.cpu->sr = trapv[i].sr;
        assert(mc68k_execute(m.cpu, 1) == trapv[i].cycles);
        assert(m.cpu->pc == trapv[i].pc);
        p2c_machine_destroy(&m);
    }
}

int main(void) {
    test_move_basic_ea_rows();
    test_move_full_extension_rows();
    test_move_matrix();
    test_move_basic_selector_aliases();
    test_move_selector_aliases();
    test_move_index_selector_aliases();
    test_arith_manifest();
    test_arith_source_aliases();
    test_long_arith_manifest();
    test_cmp2_manifest();
    test_immediate_manifest();
    test_bcd_and_tas_manifest();
    test_bitfield_manifest();
    test_movep_directions();
    test_exg_forms();
    test_special_move_system();
    test_special_move_movem();
    test_special_move_movem_full_masks();
    test_special_move_moves();
    test_special_move_moves_function_codes();
    test_branch_manifest();
    test_control_manifest();
    test_control_ea_manifest();
    test_pc_di_immediate_paths();
    test_system_manifest();
    test_rte_formats();
    return 0;
}
