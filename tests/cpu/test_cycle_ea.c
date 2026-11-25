/* Phase 2a EA timing primary-source oracle.  The arrays below are copied
 * independently from UM00 8-1, DS10 7-4, UM20 8.2.1--8.2.5, and UM30
 * 11.6.1--11.6.5 (Cache/I-Cache columns). */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <string.h>

enum { P00, P10, P20, P30 };
enum { EA_ROWS = 12 };

static mc68k_state_t *make_cpu(mc68k_model_t model) {
    mc68k_bus_t bus;
    memset(&bus, 0, sizeof(bus));
    return mc68k_create_model(&bus, model);
}

static int expected_basic(unsigned p, ea_cost_kind_t kind, unsigned row, unsigned col) {
    static const uint8_t f00[EA_ROWS][2] = {
        {0, 0},   {0, 0},  {4, 8},   {4, 8},  {6, 10},  {8, 12},
        {10, 14}, {8, 12}, {12, 16}, {8, 12}, {10, 14}, {4, 8},
    };
    static const uint8_t c10[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {2, 2}, {4, 4}, {4, 4}, {4, 4},
        {8, 8}, {4, 4}, {8, 8}, {8, 8}, {8, 8}, {0, 0},
    };
    static const uint8_t f20[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {4, 4}, {4, 4}, {5, 5}, {5, 5},
        {7, 7}, {4, 4}, {4, 4}, {5, 5}, {7, 7}, {2, 4},
    };
    static const uint8_t fi20[EA_ROWS][2] = {
        {2, 4},  {0, 0}, {4, 4}, {6, 8}, {5, 7},  {5, 7},
        {9, 11}, {5, 7}, {6, 8}, {5, 7}, {9, 11}, {4, 8},
    };
    static const uint8_t c20[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
        {4, 4}, {2, 2}, {4, 4}, {2, 2}, {4, 4}, {0, 0},
    };
    static const uint8_t ci20[EA_ROWS][2] = {
        {2, 4}, {0, 0}, {2, 4}, {4, 6}, {2, 4}, {4, 6},
        {6, 8}, {4, 6}, {6, 8}, {4, 6}, {6, 8}, {4, 8},
    };
    static const uint8_t f30[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {3, 3}, {3, 3}, {4, 4}, {4, 4},
        {6, 6}, {4, 4}, {4, 4}, {4, 4}, {6, 6}, {2, 4},
    };
    static const uint8_t fi30[EA_ROWS][2] = {
        {2, 4},  {0, 0}, {3, 4}, {5, 7}, {4, 4},  {4, 6},
        {8, 10}, {6, 8}, {6, 8}, {4, 6}, {8, 10}, {6, 8},
    };
    static const uint8_t c30[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
        {4, 4}, {2, 2}, {4, 4}, {2, 2}, {4, 4}, {0, 0},
    };
    static const uint8_t ci30[EA_ROWS][2] = {
        {2, 4}, {0, 0}, {2, 4}, {4, 6}, {2, 4}, {4, 6},
        {6, 8}, {4, 6}, {6, 8}, {4, 6}, {6, 8}, {6, 8},
    };
    static const uint8_t j00[EA_ROWS] = {0, 0, 0, 0, 0, 2, 6, 2, 4, 2, 6, 0};
    static const uint8_t j20[EA_ROWS] = {0, 0, 2, 0, 0, 4, 6, 2, 2, 4, 6, 0};
    static const uint8_t j30[EA_ROWS] = {0, 0, 2, 0, 0, 4, 6, 2, 2, 4, 6, 0};
    if (kind == EA_COST_NONE)
        return 0;
    if (kind == EA_COST_JUMP)
        return p == P30 ? j30[row] : p == P20 ? j20[row] : j00[row];
    if (p == P00)
        return f00[row][col];
    if (p == P10)
        return (kind == EA_COST_CALC || kind == EA_COST_CALC_IMM) ? c10[row][col]
                                                                  : f00[row][col];
    if (p == P20) {
        if (kind == EA_COST_FETCH_IMM)
            return fi20[row][col];
        if (kind == EA_COST_CALC_IMM)
            return ci20[row][col];
        return kind == EA_COST_CALC ? c20[row][col] : f20[row][col];
    }
    if (kind == EA_COST_FETCH_IMM)
        return fi30[row][col];
    if (kind == EA_COST_CALC_IMM)
        return ci30[row][col];
    return kind == EA_COST_CALC ? c30[row][col] : f30[row][col];
}

static int expected_full(unsigned p, ea_cost_kind_t kind, unsigned size, unsigned bd,
                         unsigned iis, unsigned base_suppressed) {
    static const uint8_t f20d[2][3] = {{7, 7, 13}, {7, 9, 13}};
    static const uint8_t c20d[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t j20d[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t f20i[3][3] = {{12, 14, 14}, {14, 16, 16}, {18, 20, 20}};
    static const uint8_t c20i[3][3] = {{11, 13, 13}, {13, 15, 15}, {17, 19, 19}};
    static const uint8_t j20i[3][3] = {{11, 13, 13}, {13, 15, 15}, {17, 19, 19}};
    static const uint8_t f30d[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t f30i[3][3] = {{10, 12, 12}, {12, 14, 14}, {16, 18, 18}};
    if (kind == EA_COST_NONE || p < P20 || iis == 4)
        return 0;
    unsigned od = iis == 0 ? 0 : (iis & 3) - 1;
    bool fetch = kind == EA_COST_FETCH || kind == EA_COST_FETCH_IMM;
    int n;
    if (p == P20) {
        if (kind == EA_COST_JUMP)
            n = iis ? j20i[bd][od] : j20d[base_suppressed][bd];
        else if (fetch)
            n = iis ? f20i[bd][od] : f20d[base_suppressed][bd];
        else
            n = iis ? c20i[bd][od] : c20d[base_suppressed][bd];
    } else {
        n = iis ? f30i[bd][od] : f30d[base_suppressed][bd];
    }
    if (kind == EA_COST_FETCH_IMM || kind == EA_COST_CALC_IMM)
        n += size == 4 ? 4 : 2;
    return n;
}

static void test_basic_rows(void) {
    static const mc68k_model_t models[] = {
        MC68K_MODEL_68000,
        MC68K_MODEL_68010,
        MC68K_MODEL_68020,
        MC68K_MODEL_68030,
    };
    for (unsigned p = 0; p < 4; ++p) {
        mc68k_state_t *cpu = make_cpu(models[p]);
        for (unsigned row = 0; row < EA_ROWS; ++row)
            for (unsigned col = 0; col < 2; ++col)
                for (unsigned kind = EA_COST_NONE; kind < EA_COST_KIND_COUNT; ++kind) {
                    ea_desc_t ea = {.size = col ? 4 : 2};
                    if (row < 7)
                        ea.mode = (uint8_t)row;
                    else {
                        ea.mode = 7;
                        ea.reg = (uint8_t)(row - 7);
                    }
                    assert(ea_cycles(cpu, (ea_cost_kind_t)kind, &ea) ==
                           expected_basic(p, (ea_cost_kind_t)kind, row, col));
                }
        mc68k_destroy(cpu);
    }
}

static void test_extension_forms(void) {
    static const mc68k_model_t models[] = {
        MC68K_MODEL_68000,
        MC68K_MODEL_68010,
        MC68K_MODEL_68020,
        MC68K_MODEL_68030,
    };
    for (unsigned p = 0; p < 4; ++p) {
        mc68k_state_t *cpu = make_cpu(models[p]);
        for (unsigned mode7 = 0; mode7 < 2; ++mode7)
            for (unsigned col = 0; col < 2; ++col)
                for (unsigned kind = EA_COST_NONE; kind < EA_COST_KIND_COUNT; ++kind) {
                    ea_desc_t brief = {.mode = mode7 ? 7 : 6,
                                       .reg = mode7 ? 3 : 0,
                                       .size = col ? 4 : 2,
                                       .ext_form = EA_EXT_BRIEF};
                    unsigned row = mode7 ? 10 : 6;
                    assert(ea_cycles(cpu, (ea_cost_kind_t)kind, &brief) ==
                           expected_basic(p, (ea_cost_kind_t)kind, row, col));
                }
        for (unsigned mode7 = 0; mode7 < 2; ++mode7)
            for (unsigned bd = 0; bd < 3; ++bd)
                for (unsigned iis = 0; iis < 8; ++iis)
                    for (unsigned base_suppressed = 0; base_suppressed < 2; ++base_suppressed)
                        for (unsigned index_suppressed = 0; index_suppressed < 2;
                             ++index_suppressed)
                            for (unsigned col = 0; col < 2; ++col)
                                for (unsigned kind = EA_COST_NONE; kind < EA_COST_KIND_COUNT;
                                     ++kind) {
                                    ea_desc_t ea = {.mode = mode7 ? 7 : 6,
                                                    .reg = mode7 ? 3 : 0,
                                                    .size = col ? 4 : 2,
                                                    .ext_form = EA_EXT_FULL,
                                                    .bd_size = bd,
                                                    .od_size = iis == 0 ? 0 : (iis & 3),
                                                    .iis = iis,
                                                    .base_suppressed = base_suppressed,
                                                    .index_suppressed = index_suppressed};
                                    assert(ea_cycles(cpu, (ea_cost_kind_t)kind, &ea) ==
                                           expected_full(p, (ea_cost_kind_t)kind, ea.size, bd,
                                                         iis, base_suppressed));
                                }
        mc68k_destroy(cpu);
    }
}

static void test_class_derived_kind(void) {
    static const struct {
        mc68k_model_t model;
        ea_cost_kind_t moves, sr;
    } cases[] = {
        {MC68K_MODEL_68000, EA_COST_NONE, EA_COST_FETCH},
        {MC68K_MODEL_68010, EA_COST_CALC, EA_COST_CALC},
        {MC68K_MODEL_68020, EA_COST_NONE, EA_COST_CALC},
        {MC68K_MODEL_68030, EA_COST_FETCH, EA_COST_CALC},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        mc68k_state_t *cpu = make_cpu(cases[i].model);
        assert(cyc_ea_kind(cpu, CYC_MOVES_L) == cases[i].moves);
        assert(cyc_ea_kind(cpu, CYC_MOVE_FROM_SR_REG) == cases[i].sr);
        mc68k_destroy(cpu);
    }
}

/* #102: 060 は EA 時間を cyc060_instruction() が決める。ea_cycles() が低い profile の表へ
 * 落ちると EA contribution が二重に載るので、全 mode・kind・size で 0 であること。 */
static void test_060_ea_cycles_has_no_implicit_fallback(void) {
    mc68k_state_t *cpu = make_cpu(MC68K_MODEL_68060);
    for (unsigned row = 0; row < EA_ROWS; ++row)
        for (unsigned col = 0; col < 2; ++col)
            for (unsigned kind = EA_COST_NONE; kind < EA_COST_KIND_COUNT; ++kind) {
                ea_desc_t ea = {.size = col ? 4 : 2};
                if (row < 7)
                    ea.mode = (uint8_t)row;
                else {
                    ea.mode = 7;
                    ea.reg = (uint8_t)(row - 7);
                }
                assert(ea_cycles(cpu, (ea_cost_kind_t)kind, &ea) == 0);
            }
    ea_desc_t full = {.mode = 6, .size = 4, .ext_form = EA_EXT_FULL};
    for (unsigned kind = EA_COST_NONE; kind < EA_COST_KIND_COUNT; ++kind)
        assert(ea_cycles(cpu, (ea_cost_kind_t)kind, &full) == 0);
    mc68k_destroy(cpu);
}

int main(void) {
    test_basic_rows();
    test_extension_forms();
    test_class_derived_kind();
    test_060_ea_cycles_has_no_implicit_fallback();
    return 0;
}
