/* MC68030 PMMU page-walk boundary regressions (x68000 ).
 *
 * The expected physical address is assembled here from the number of logical
 * bits consumed by the table search.  It deliberately does not call PMMU
 * implementation helpers: an early page descriptor retains its unconsumed
 * index bits as part of the page offset. */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define RAM_SIZE    0x4000u
#define TC_8BIT_ABC UINT32_C(0x80808880) /* E, PS=8, IS=0, A/B/C=8 */
#define A_TABLE     0x1000u
#define B_TABLE     0x2000u
#define C_TABLE     0x3000u

typedef struct {
    uint8_t mem[RAM_SIZE];
    uint32_t fail_read_at;
    unsigned long_reads;
} ctx_t;

static bool rb(void *p, uint32_t a, uint8_t *v) {
    ctx_t *c = p;
    if (a >= RAM_SIZE)
        return false;
    *v = c->mem[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    *v = (uint16_t)((c->mem[a] << 8) | c->mem[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    c->long_reads++;
    if (a == c->fail_read_at || a + 3 >= RAM_SIZE)
        return false;
    *v = ((uint32_t)c->mem[a] << 24) | ((uint32_t)c->mem[a + 1] << 16) |
         ((uint32_t)c->mem[a + 2] << 8) | c->mem[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    if (a >= RAM_SIZE)
        return false;
    c->mem[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    c->mem[a] = (uint8_t)(v >> 8);
    c->mem[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    return ww(p, a, (uint16_t)(v >> 16)) && ww(p, a + 2, (uint16_t)v);
}

static mc68k_bus_t make_bus(ctx_t *ctx) {
    return (mc68k_bus_t){rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, ctx};
}

static void put_long(ctx_t *c, uint32_t a, uint32_t v) {
    assert(a + 3 < RAM_SIZE);
    c->mem[a] = (uint8_t)(v >> 24);
    c->mem[a + 1] = (uint8_t)(v >> 16);
    c->mem[a + 2] = (uint8_t)(v >> 8);
    c->mem[a + 3] = (uint8_t)v;
}

static uint32_t expected_page(uint32_t logical, uint32_t descriptor_base, unsigned consumed) {
    uint32_t offset_mask = consumed ? UINT32_MAX >> consumed : UINT32_MAX;
    return (descriptor_base & ~(UINT32_C(0xff) & offset_mask)) | (logical & offset_mask);
}

static void clear_atc(mc68k_state_t *cpu) {
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; ++i)
        cpu->atc[i].valid = false;
}

enum stop_level { STOP_ROOT, STOP_A, STOP_B, STOP_C };

/* Build a short-root A/B/C path and terminate it at the selected level. */
static void install_path(mc68k_state_t *cpu, ctx_t *c, uint32_t logical, enum stop_level stop,
                         uint32_t page_descriptor) {
    unsigned a = logical >> 24;
    unsigned b = (logical >> 16) & 0xffu;
    unsigned d = (logical >> 8) & 0xffu;
    cpu->mmu_tc = TC_8BIT_ABC;
    if (stop == STOP_ROOT) {
        /* A root page descriptor is long-form, so use lower LIMIT=0 rather
         * than upper LIMIT=0; every first-level index is then in range. */
        cpu->mmu_crp = UINT64_C(0x80000001) << 32 | page_descriptor;
        return;
    }
    cpu->mmu_crp = UINT64_C(0x0000000200001000);
    put_long(c, A_TABLE + a * 4u, stop == STOP_A ? page_descriptor : B_TABLE | 2u);
    if (stop == STOP_A)
        return;
    put_long(c, B_TABLE + b * 4u, stop == STOP_B ? page_descriptor : C_TABLE | 2u);
    if (stop == STOP_B)
        return;
    put_long(c, C_TABLE + d * 4u, page_descriptor);
}

static mc68k_translate_result_t translate(mc68k_state_t *cpu, uint32_t logical,
                                          const mc68k_access_t *access) {
    return cpu_mmu_translate_result(cpu, logical, access, MC68K_TRANSLATE_DEBUG);
}

static void test_early_termination_matrix(void) {
    static const struct {
        const char *name;
        uint32_t logical;
        enum stop_level stop;
        uint32_t base;
        unsigned consumed;
    } cases[] = {
        {"root", 0x01020380u, STOP_ROOT, 0x00f00000u, 0},
        {"A-nonaligned-2MiB", 0x01020380u, STOP_A, 0x00f00000u, 8},
        {"B", 0x01020380u, STOP_B, 0x00500000u, 16},
        {"C", 0x01020380u, STOP_C, 0x00500000u, 24},
        {"A-table-first", 0x00000000u, STOP_A, 0x00200000u, 8},
        {"C-table-last", 0xffffffffu, STOP_C, 0x00600000u, 24},
    };
    const mc68k_access_t read_super = {.fc = 5, .read = true, .size = 4};

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        ctx_t c = {.fail_read_at = UINT32_MAX};
        mc68k_bus_t bus = make_bus(&c);
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
        mc68k_reset(cpu);
        install_path(cpu, &c, cases[i].logical, cases[i].stop, cases[i].base | 1u);
        mc68k_translate_result_t r = translate(cpu, cases[i].logical, &read_super);
        if (!r.ok || r.source != MC68K_TRANSLATE_WALK)
            fprintf(stderr, "%s: ok=%d source=%d fault=%d level=%u physical=%08x\n",
                    cases[i].name, r.ok, r.source, r.fault, r.level, r.physical);
        assert(r.ok && r.source == MC68K_TRANSLATE_WALK);
        assert(r.level == (uint8_t)cases[i].stop);
        assert(r.physical == expected_page(cases[i].logical, cases[i].base, cases[i].consumed));
        mc68k_destroy(cpu);
    }
}

static void test_fault_matrix(void) {
    const mc68k_access_t read_super = {.fc = 5, .read = true, .size = 4};
    const mc68k_access_t read_user = {.fc = 1, .read = true, .size = 4};
    const uint32_t logical = 0x01020380u;

    for (unsigned stop = STOP_A; stop <= STOP_C; ++stop) {
        ctx_t c = {.fail_read_at = UINT32_MAX};
        mc68k_bus_t bus = make_bus(&c);
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
        mc68k_reset(cpu);
        install_path(cpu, &c, logical, (enum stop_level)stop, 0);
        mc68k_translate_result_t r = translate(cpu, logical, &read_super);
        assert(!r.ok && r.fault == MMU_FAULT_INVALID && r.level == stop);
        mc68k_destroy(cpu);
    }

    ctx_t c = {.fail_read_at = UINT32_MAX};
    mc68k_bus_t bus = make_bus(&c);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
    mc68k_reset(cpu);
    cpu->mmu_tc = TC_8BIT_ABC;
    cpu->mmu_crp = UINT64_C(0x0000000300001000); /* long root, upper LIMIT=0 */
    mc68k_translate_result_t r = translate(cpu, logical, &read_super);
    assert(!r.ok && r.fault == MMU_FAULT_LIMIT && r.level == 0);

    cpu->mmu_crp = UINT64_C(0x8000010300001000); /* long root, lower LIMIT=0, S */
    r = translate(cpu, logical, &read_user);
    assert(!r.ok && r.fault == MMU_FAULT_SUPERVISOR && r.level == 0);
    mc68k_destroy(cpu);
}

static void test_table_boundaries_and_bus_error(void) {
    static const struct {
        uint32_t logical, failing_descriptor;
    } cases[] = {
        {0x00000000u, A_TABLE},
        {0xff000000u, A_TABLE + 0x3fcu},
    };
    const mc68k_access_t read_super = {.fc = 5, .read = true, .size = 4};
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        ctx_t c = {.fail_read_at = cases[i].failing_descriptor};
        mc68k_bus_t bus = make_bus(&c);
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
        mc68k_reset(cpu);
        cpu->mmu_tc = TC_8BIT_ABC;
        cpu->mmu_crp = UINT64_C(0x0000000200001000);
        mc68k_translate_result_t r = translate(cpu, cases[i].logical, &read_super);
        assert(!r.ok && r.fault == MMU_FAULT_TABLE_BUS_ERROR);
        assert(r.table_bus_error_physical == cases[i].failing_descriptor && r.level == 0);
        mc68k_destroy(cpu);
    }
}

static void test_sre_selects_root_by_fc2(void) {
    ctx_t c = {.fail_read_at = UINT32_MAX};
    mc68k_bus_t bus = make_bus(&c);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68030);
    const mc68k_access_t user = {.fc = 1, .read = true, .size = 4};
    const mc68k_access_t supervisor = {.fc = 5, .read = true, .size = 4};
    const uint32_t logical = 0x00123456u;
    mc68k_reset(cpu);
    cpu->mmu_tc = TC_8BIT_ABC | UINT32_C(0x02000000); /* SRE */
    cpu->mmu_crp = UINT64_C(0x0000000100000000);
    cpu->mmu_srp = UINT64_C(0x0000000110000000);
    mc68k_translate_result_t r = translate(cpu, logical, &user);
    assert(r.ok && r.physical == expected_page(logical, 0x00000000u, 0));
    clear_atc(cpu);
    r = translate(cpu, logical, &supervisor);
    assert(r.ok && r.physical == expected_page(logical, 0x10000000u, 0));
    mc68k_destroy(cpu);
}

int main(void) {
    test_early_termination_matrix();
    test_fault_matrix();
    test_table_boundaries_and_bus_error();
    test_sre_selects_root_by_fc2();
    return 0;
}
