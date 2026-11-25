/*
 * test_phase2b_contract.c -- spec section 6.2.4 (Phase 2B) F-axis cases.
 *
 * Each case runs one instruction from PC=$1000 in user mode with
 * (SR & $F71F) = $0010 and checks only what the spec table lists.  Registers
 * and memory not listed as expected must stay unchanged ("表にないレジスタ・
 * メモリは未変更").  Expected values come from the spec, not the core.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu_internal.h"
#include "mc68000/cpu.h"
#include "memory/bus.h"
#include "memory/memory.h"

#define RAM_SIZE   0x10000u
#define RESET_SSP  0x8000u
#define RESET_PC   0x1000u
#define SR_USER_X  0x0010u
#define SR_CMP     0xF71Fu
#define SR_UNCHECK 0xFFFFu /* expected SR not listed in the spec row */
#define CHECK_END  0x2000u /* low RAM compared for unexpected writes */
#define MAX_REGS   8u
#define MAX_MEM    4u
#define MAX_WORDS  3u
#define MAX_BYTES  8u

/* Register kinds: 'D' data, 'A' address (A7 = active user stack = USP). */
typedef struct {
    char kind;
    unsigned index;
    uint32_t value;
} reg_t;

typedef struct {
    uint32_t address;
    unsigned count;
    uint8_t bytes[MAX_BYTES];
} mem_t;

typedef struct {
    const char *id;
    uint16_t words[MAX_WORDS];
    unsigned word_count;
    reg_t init[MAX_REGS];
    mem_t init_mem[MAX_MEM];
    reg_t expect[MAX_REGS];
    mem_t expect_mem[MAX_MEM];
    uint32_t expect_pc;
    uint16_t expect_sr; /* compared under SR_CMP unless SR_UNCHECK */
} case_t;

static void put_w(uint8_t *ram, uint32_t a, uint16_t v) {
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1] = (uint8_t)v;
}
static void put_l(uint8_t *ram, uint32_t a, uint32_t v) {
    put_w(ram, a, (uint16_t)(v >> 16));
    put_w(ram, a + 2, (uint16_t)v);
}

static uint32_t *reg_slot(mc68k_state_t *cpu, const reg_t *r) {
    return r->kind == 'D' ? &cpu->d[r->index] : &cpu->a[r->index];
}

static bool lists(const reg_t *regs, char kind, unsigned index) {
    for (unsigned i = 0; i < MAX_REGS; i++)
        if (regs[i].kind == kind && regs[i].index == index)
            return true;
    return false;
}

static void run_case(const case_t *c) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    assert(ram);
    put_l(ram, 0, RESET_SSP);
    put_l(ram, 4, RESET_PC);
    for (unsigned i = 0; i < c->word_count; i++)
        put_w(ram, RESET_PC + 2 * i, c->words[i]);
    for (unsigned i = 0; i < MAX_MEM; i++)
        if (c->init_mem[i].count)
            memcpy(&ram[c->init_mem[i].address], c->init_mem[i].bytes, c->init_mem[i].count);
    uint8_t *before = malloc(CHECK_END);
    assert(before);
    memcpy(before, ram, CHECK_END);

    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(mem && region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create(&bus);
    assert(cpu);
    mc68k_reset(cpu);

    cpu->sr = SR_USER_X; /* user mode, X=1 */
    cpu->a[7] = 0x6000u; /* default user stack; cases may override */
    cpu->usp = cpu->a[7];
    for (unsigned i = 0; i < MAX_REGS; i++)
        if (c->init[i].kind) {
            *reg_slot(cpu, &c->init[i]) = c->init[i].value;
            if (c->init[i].kind == 'A' && c->init[i].index == 7)
                cpu->usp = c->init[i].value;
        }
    uint32_t d0[8], a0[8];
    memcpy(d0, cpu->d, sizeof d0);
    memcpy(a0, cpu->a, sizeof a0);

    mc68k_execute(cpu, 1);

    for (unsigned i = 0; i < MAX_REGS; i++)
        if (c->expect[i].kind) {
            if (*reg_slot(cpu, &c->expect[i]) != c->expect[i].value) {
                fprintf(stderr, "%s: %c%u = %08x, expected %08x\n", c->id, c->expect[i].kind,
                        c->expect[i].index, *reg_slot(cpu, &c->expect[i]), c->expect[i].value);
                abort();
            }
        }
    for (unsigned n = 0; n < 8; n++) {
        if (!lists(c->expect, 'D', n) && cpu->d[n] != d0[n]) {
            fprintf(stderr, "%s: D%u changed to %08x\n", c->id, n, cpu->d[n]);
            abort();
        }
        if (!lists(c->expect, 'A', n) && cpu->a[n] != a0[n]) {
            fprintf(stderr, "%s: A%u changed to %08x\n", c->id, n, cpu->a[n]);
            abort();
        }
    }
    if (cpu->pc != c->expect_pc) {
        fprintf(stderr, "%s: PC = %08x, expected %08x\n", c->id, cpu->pc, c->expect_pc);
        abort();
    }
    if (c->expect_sr != SR_UNCHECK && (cpu->sr & SR_CMP) != c->expect_sr) {
        fprintf(stderr, "%s: SR = %04x, expected %04x\n", c->id, cpu->sr & SR_CMP, c->expect_sr);
        abort();
    }

    /* Expected memory, then everything else in low RAM must be unchanged. */
    for (unsigned i = 0; i < MAX_MEM; i++)
        if (c->expect_mem[i].count) {
            memcpy(&before[c->expect_mem[i].address], c->expect_mem[i].bytes,
                   c->expect_mem[i].count);
        }
    for (uint32_t a = 0; a < CHECK_END; a++)
        if (ram[a] != before[a]) {
            fprintf(stderr, "%s: mem[%04x] = %02x, expected %02x\n", c->id, a, ram[a], before[a]);
            abort();
        }

    mc68k_destroy(cpu);
    mem_destroy(mem);
    free(region);
    free(before);
    free(ram);
}

#define D(n, v) {'D', (n), (v)}
#define A(n, v) {'A', (n), (v)}
#define M(addr, ...) {(addr), sizeof((uint8_t[]){__VA_ARGS__}), {__VA_ARGS__}}
#define W(...) {__VA_ARGS__}, sizeof((uint16_t[]){__VA_ARGS__}) / sizeof(uint16_t)

static const case_t cases[] = {
    {"T-INS-011-01", W(0x1200), {D(0, 0x11223344u), D(1, 0xAABBCCDDu)}, {}, {D(1, 0xAABBCC44u)}, {},
     0x1002u, 0x0010u},
    {"T-INS-011-02", W(0x3318), {A(0, 0x200u), A(1, 0x300u)}, {M(0x200, 0x12, 0x34)},
     {A(0, 0x202u), A(1, 0x2FEu)}, {M(0x2FE, 0x12, 0x34)}, 0x1002u, SR_UNCHECK},
    {"T-INS-011-03", W(0x2200), {D(0, 0x12345678u)}, {}, {D(1, 0x12345678u)}, {}, 0x1002u, 0x0010u},
    {"T-INS-012-01", W(0x3240), {D(0, 0x00008000u)}, {}, {A(1, 0xFFFF8000u)}, {}, 0x1002u, 0x0010u},
    {"T-INS-012-02", W(0x2240), {D(0, 0x12345678u)}, {}, {A(1, 0x12345678u)}, {}, 0x1002u, 0x0010u},
    {"T-INS-013-01", W(0x7480), {}, {}, {D(2, 0xFFFFFF80u)}, {}, 0x1002u, 0x0018u},
    {"T-INS-014-01", W(0x4890, 0x8001), {A(0, 0x200u), D(0, 0x12345678u), A(7, 0x9ABCDEF0u)}, {},
     {A(0, 0x200u)}, {M(0x200, 0x56, 0x78, 0xDE, 0xF0)}, 0x1004u, 0x0010u},
    {"T-INS-014-02", W(0x48E0, 0x8080), {A(0, 0x200u), D(0, 0x11223344u)}, {}, {A(0, 0x1F8u)},
     {M(0x1F8, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x02, 0x00)}, 0x1004u, 0x0010u},
    {"T-INS-014-03", W(0x4C98, 0x0101), {A(0, 0x200u)}, {M(0x200, 0x80, 0x00, 0x12, 0x34)},
     {D(0, 0xFFFF8000u), A(0, 0x204u)}, {}, 0x1004u, 0x0010u},
    {"T-INS-014-04", W(0x48D0, 0x0000), {A(0, 0x200u)},
     {M(0x200, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67)}, {}, {}, 0x1004u, 0x0010u},
    {"T-INS-014-05", W(0x4CD0, 0x0201), {A(0, 0x200u)},
     {M(0x200, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0)},
     {D(0, 0x12345678u), A(1, 0x9ABCDEF0u)}, {}, 0x1004u, 0x0010u},
    {"T-INS-015-01", W(0x01C8, 0x0000), {A(0, 0x100u), D(0, 0xAABBCCDDu)}, {}, {},
     {M(0x100, 0xAA), M(0x102, 0xBB), M(0x104, 0xCC), M(0x106, 0xDD)}, 0x1004u, 0x0010u},
    {"T-INS-015-02", W(0x0108, 0x0000), {A(0, 0x100u), D(0, 0xAABB0000u)},
     {M(0x100, 0x80), M(0x102, 0x01)}, {D(0, 0xAABB8001u)}, {}, 0x1004u, 0x0010u},
    {"T-INS-015-03", W(0x0148, 0x0000), {A(0, 0x100u)},
     {M(0x100, 0x80), M(0x102, 0x01), M(0x104, 0x02), M(0x106, 0x03)}, {D(0, 0x80010203u)}, {},
     0x1004u, 0x0010u},
    {"T-INS-015-04", W(0x0188, 0x0000), {A(0, 0x100u), D(0, 0xAABBCCDDu)}, {}, {},
     {M(0x100, 0xCC), M(0x102, 0xDD)}, 0x1004u, 0x0010u},
    {"T-INS-016-01", W(0x41FA, 0x0010), {}, {}, {A(0, 0x1012u)}, {}, 0x1004u, 0x0010u},
    {"T-INS-017-01", W(0x4857), {A(7, 0x200u)}, {}, {A(7, 0x1FCu)},
     {M(0x1FC, 0x00, 0x00, 0x02, 0x00)}, 0x1002u, 0x0010u},
    {"T-INS-018-01", W(0x4E56, 0xFFF8), {A(6, 0x300u), A(7, 0x200u)}, {}, {A(6, 0x1FCu), A(7, 0x1F4u)},
     {M(0x1FC, 0x00, 0x00, 0x03, 0x00)}, 0x1004u, 0x0010u},
    {"T-INS-019-01", W(0x4E5E), {A(6, 0x200u)}, {M(0x200, 0x12, 0x34, 0x56, 0x78)},
     {A(6, 0x12345678u), A(7, 0x204u)}, {}, 0x1002u, 0x0010u},
    {"T-INS-020-01", W(0xC188), {D(0, 0x11111111u), A(0, 0x22222222u)}, {},
     {D(0, 0x22222222u), A(0, 0x11111111u)}, {}, 0x1002u, 0x0010u},
    {"T-INS-020-02", W(0xC141), {D(0, 0x11111111u), D(1, 0x22222222u)}, {},
     {D(0, 0x22222222u), D(1, 0x11111111u)}, {}, 0x1002u, 0x0010u},
    {"T-INS-020-03", W(0xC149), {A(0, 0x11111111u), A(1, 0x22222222u)}, {},
     {A(0, 0x22222222u), A(1, 0x11111111u)}, {}, 0x1002u, 0x0010u},
    {"T-INS-021-01", W(0x4880), {D(0, 0xAABBCC80u)}, {}, {D(0, 0xAABBFF80u)}, {}, 0x1002u, 0x0018u},
    {"T-INS-021-02", W(0x48C0), {D(0, 0xAABB8000u)}, {}, {D(0, 0xFFFF8000u)}, {}, 0x1002u, 0x0018u},
    {"T-INS-022-01", W(0x4840), {D(0, 0x80000000u)}, {}, {D(0, 0x00008000u)}, {}, 0x1002u, 0x0010u},
};

int main(void) {
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        run_case(&cases[i]);
    printf("test_phase2b_contract: %zu cases OK\n", sizeof cases / sizeof cases[0]);
    return 0;
}
