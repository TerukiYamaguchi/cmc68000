/* Phase 2b: UM00 Tables 8-2/3/6/10 and DS10 Tables 7-5/7/13/19. */
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
#define MEM_A     0x00004000u

static void put16(uint8_t *ram, uint32_t a, uint16_t v) {
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1] = (uint8_t)v;
}
static void put32(uint8_t *ram, uint32_t a, uint32_t v) {
    put16(ram, a, (uint16_t)(v >> 16));
    put16(ram, a + 2, (uint16_t)v);
}
typedef void (*preset_t)(mc68k_state_t *);
static int run(const uint16_t *words, size_t count, mc68k_model_t model, preset_t preset) {
    uint8_t ram[RAM_SIZE] = {0};
    put32(ram, 0, RESET_SSP);
    put32(ram, 4, RESET_PC);
    for (size_t i = 0; i < count; ++i)
        put16(ram, RESET_PC + 2 * i, words[i]);
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    mc68k_reset(cpu);
    if (preset)
        preset(cpu);
    int cycles = mc68k_execute(cpu, 1);
    mc68k_destroy(cpu);
    return cycles;
}
static void preset_a0_a1(mc68k_state_t *cpu) {
    cpu->a[0] = MEM_A;
    cpu->a[1] = MEM_A + 0x100;
}
static void preset_a1(mc68k_state_t *cpu) {
    cpu->a[1] = MEM_A;
}
static void preset_sp(mc68k_state_t *cpu) {
    cpu->a[0] = MEM_A;
    cpu->a[7] = 0x7000;
}

static size_t ea_words(uint16_t *out, unsigned ea, int size) {
    unsigned mode = ea >> 3, reg = ea & 7;
    if (mode == 5 || mode == 6 || (mode == 7 && reg == 0) || (mode == 7 && reg == 2) ||
        (mode == 7 && reg == 3)) {
        out[0] = 0;
        return 1;
    }
    if (mode == 7 && (reg == 1 || reg == 4)) {
        out[0] = 0;
        out[1] = 0;
        return size == 4 || reg == 1 ? 2 : 1;
    }
    return 0;
}
static int fea00(unsigned ea, int size) {
    static const uint8_t bw[] = {0, 0, 4, 4, 6, 8, 10, 8, 12, 8, 10, 4};
    static const uint8_t lng[] = {0, 0, 8, 8, 10, 12, 14, 12, 16, 12, 14, 8};
    unsigned row = ea < 56 ? ea >> 3 : 7 + (ea & 7);
    return size == 4 ? lng[row] : bw[row];
}
static int run_move(unsigned src, unsigned dst, int size, mc68k_model_t model) {
    uint16_t words[6] = {0};
    size_t n = 1;
    unsigned line = size == 1 ? 0x1000 : size == 2 ? 0x3000 : 0x2000;
    words[0] = (uint16_t)(line | ((dst & 7) << 9) | (((dst >> 3) & 7) << 6) | src);
    n += ea_words(words + n, src, size);
    n += ea_words(words + n, dst, size);
    return run(words, n, model, preset_a0_a1);
}

static void test_move_matrix(void) {
    static const unsigned src_bw[] = {0,    0x10, 0x18, 0x20, 0x28, 0x30,
                                      0x38, 0x39, 0x3a, 0x3b, 0x3c};
    static const unsigned src_l[] = {0,    8,    0x10, 0x18, 0x20, 0x28,
                                     0x30, 0x38, 0x39, 0x3a, 0x3b, 0x3c};
    static const unsigned dst_bw[] = {0, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x39};
    static const unsigned dst_l[] = {0, 8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x39};
    for (int size_i = 0; size_i < 3; ++size_i) {
        int size = size_i == 0 ? 1 : size_i == 1 ? 2 : 4;
        /* MOVE.W includes An-direct source and destination (MOVEA.W), like
         * MOVE.L.  Only MOVE.B excludes An-direct. */
        const unsigned *srcs = size == 1 ? src_bw : src_l, *dsts = size == 1 ? dst_bw : dst_l;
        size_t ns =
            size == 1 ? sizeof(src_bw) / sizeof(*src_bw) : sizeof(src_l) / sizeof(*src_l);
        size_t nd =
            size == 1 ? sizeof(dst_bw) / sizeof(*dst_bw) : sizeof(dst_l) / sizeof(*dst_l);
        assert(size == 1 ? (ns == 11 && nd == 8) : (ns == 12 && nd == 9));
        for (size_t s = 0; s < ns; ++s)
            for (size_t d = 0; d < nd; ++d) {
                int expected = 4 + fea00(srcs[s], size) + fea00(dsts[d], size) -
                               ((dsts[d] >> 3) == 4 ? 2 : 0);
                assert(run_move(srcs[s], dsts[d], size, MC68K_MODEL_68000) == expected);
                if (size == 4 && srcs[s] <= 8 && (dsts[d] >> 3) == 4)
                    expected += 2;
                assert(run_move(srcs[s], dsts[d], size, MC68K_MODEL_68010) == expected);
            }
    }
}
static void test_clr_matrix(void) {
    static const unsigned eas[] = {0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x39};
    static const int clr10_bw[] = {8, 8, 10, 12, 16, 12, 16};
    static const int clr10_l[] = {12, 12, 14, 16, 20, 16, 20};
    for (int si = 1; si <= 2; ++si)
        for (size_t i = 0; i < sizeof(eas) / sizeof(*eas); ++i) {
            int size = si == 1 ? 2 : 4;
            uint16_t words[4] = {0};
            size_t n = 1;
            words[0] = (uint16_t)(0x4200 | ((si == 2 ? 2 : 1) << 6) | eas[i]);
            n += ea_words(words + n, eas[i], size);
            assert(run(words, n, MC68K_MODEL_68000, preset_a1) ==
                   (size == 4 ? 12 : 8) + fea00(eas[i], size));
            assert(run(words, n, MC68K_MODEL_68010, preset_a1) ==
                   (size == 4 ? clr10_l[i] : clr10_bw[i]));
        }
}
static void test_control_matrix(void) {
    static const unsigned eas[] = {0x10, 0x28, 0x30, 0x38, 0x39, 0x3a, 0x3b};
    static const int jea[] = {0, 2, 6, 2, 4, 2, 6}, ctl[] = {0, 4, 8, 4, 8, 4, 8};
    for (size_t i = 0; i < sizeof(eas) / sizeof(*eas); ++i)
        for (int model = 0; model < 2; ++model) {
            uint16_t words[4] = {0};
            size_t n;
            words[0] = (uint16_t)(0x4EC0 | eas[i]);
            n = 1 + ea_words(words + 1, eas[i], 2);
            assert(run(words, n, model ? MC68K_MODEL_68010 : MC68K_MODEL_68000, preset_sp) ==
                   8 + jea[i]);
            words[0] = (uint16_t)(0x4E80 | eas[i]);
            assert(run(words, n, model ? MC68K_MODEL_68010 : MC68K_MODEL_68000, preset_sp) ==
                   16 + jea[i]);
            words[0] = (uint16_t)(0x41C0 | eas[i]);
            assert(run(words, n, model ? MC68K_MODEL_68010 : MC68K_MODEL_68000, preset_a0_a1) ==
                   4 + ctl[i]);
            words[0] = (uint16_t)(0x4840 | eas[i]);
            assert(run(words, n, model ? MC68K_MODEL_68010 : MC68K_MODEL_68000, preset_sp) ==
                   12 + ctl[i]);
        }
}
static void test_movem_matrix(void) {
    static const unsigned load_ea[] = {0x10, 0x18, 0x28, 0x30, 0x38, 0x39, 0x3a, 0x3b};
    static const int load_cost[] = {4, 4, 8, 10, 8, 12, 8, 10};
    static const unsigned store_ea[] = {0x10, 0x20, 0x28, 0x30, 0x38, 0x39};
    static const int store_cost[] = {0, 0, 4, 6, 4, 8};
    static const uint16_t masks[] = {0, 1, 3};
    for (int dir = 0; dir < 2; ++dir)
        for (int si = 0; si < 2; ++si) {
            const unsigned *eas = dir ? load_ea : store_ea;
            const int *cost = dir ? load_cost : store_cost;
            size_t ne =
                dir ? sizeof(load_ea) / sizeof(*load_ea) : sizeof(store_ea) / sizeof(*store_ea);
            int unit = si ? 8 : 4;
            for (size_t i = 0; i < ne; ++i)
                for (size_t m = 0; m < sizeof(masks) / sizeof(*masks); ++m) {
                    uint16_t words[5] = {0};
                    size_t n = 2;
                    words[0] = (uint16_t)((dir ? 0x4C80 : 0x4880) | (si ? 0x40 : 0) | eas[i]);
                    words[1] = masks[m];
                    n += ea_words(words + n, eas[i], si ? 4 : 2);
                    int count = masks[m] == 3 ? 2 : masks[m] == 1;
                    for (int model = 0; model < 2; ++model)
                        assert(run(words, n, model ? MC68K_MODEL_68010 : MC68K_MODEL_68000,
                                   preset_sp) == 8 + cost[i] + unit * count);
                }
        }
}
static void test_020_030_phase_boundary(void) {
    uint16_t clr[] = {0x4291};           /* CLR.L (A1): Phase 2c owns 020+ */
    uint16_t lea[] = {0x43F0, 0x0000};   /* pre-Phase-2c indexed LEA value */
    uint16_t move[] = {0x2300};          /* MOVE.L D0,-(A1) */
    uint16_t movem[] = {0x4CD0, 0x0001}; /* MOVEM.L (A0),D0 */
    for (int model = MC68K_MODEL_68020; model <= MC68K_MODEL_68030; ++model) {
        assert(run(clr, 1, (mc68k_model_t)model, preset_a1) ==
               (model == MC68K_MODEL_68020 ? 8 : 5));
        /* UM20 §8.2.16 / UM30 §11.6.16 LEA is 2+CEA: brief indexed CEA=4. */
        assert(run(lea, 2, (mc68k_model_t)model, preset_a0_a1) == 6);
        /* UM20 §8.2.6 is a self-contained matrix: Dn→-(An) is 5.
         * UM30 §11.6.6 prints MOVE Rn,-(An) as 4. */
        assert(run(move, 1, (mc68k_model_t)model, preset_a1) ==
               (model == MC68K_MODEL_68020 ? 5 : 4));
        /* Phase 2c: UM20 §8.2.7 MOVEM EA,RL is 8+4n+CIEA.  The
         * one-word list extension with (A0) contributes 2, hence 14.
         * UM30 §11.6.7 has the same 8+4n+CIEA formula. */
        assert(run(movem, 2, (mc68k_model_t)model, preset_a0_a1) == 14);
    }
}
int main(void) {
    test_move_matrix();
    test_clr_matrix();
    test_control_matrix();
    test_movem_matrix();
    test_020_030_phase_boundary();
    return 0;
}
