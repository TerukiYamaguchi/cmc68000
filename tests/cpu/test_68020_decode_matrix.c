/*
 * MC68020 decode matrix ().
 *
 * This intentionally does not call ea_is_*().  The masks below are a small
 * specification oracle: changing an EA classifier or an opcode registration
 * must be checked against all 64 mode/reg encodings, rather than against the
 * same helper that made the registration decision.
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* stdio exposes a historical putw(3); keep the fixture helper distinct. */
#define putw matrix_putw

#define RAM_SIZE   0x4000
#define RESET_PC   0x0100
#define RESET_SP   0x3000
#define ILLEGAL_PC 0x0900

/* Fixed Motorola EA classes, bit n describes mode/reg encoding n. */
#define EA_DN                UINT64_C(0x00000000000000ff)
#define EA_AN                UINT64_C(0x000000000000ff00)
#define EA_MEMORY_ALTERABLE  UINT64_C(0x03ffffffffff0000)
#define EA_CONTROL           UINT64_C(0x0fffff0000ff0000)
#define EA_CONTROL_ALTERABLE UINT64_C(0x03ffff0000ff0000)
#define EA_DATA              UINT64_C(0x1fffffffffff00ff)
#define EA_DATA_ALTERABLE    (EA_DN | EA_MEMORY_ALTERABLE)
#define EA_TST_WL            (EA_DATA | EA_AN)
#define EA_CMPI_68020        (EA_DATA_ALTERABLE | UINT64_C(0x0c00000000000000))

typedef struct {
    uint8_t ram[RAM_SIZE];
    unsigned byte_reads[RAM_SIZE];
    unsigned word_reads[RAM_SIZE / 2];
    unsigned long_reads[RAM_SIZE / 4];
    unsigned byte_writes[RAM_SIZE];
    unsigned word_writes[RAM_SIZE / 2];
    unsigned long_writes[RAM_SIZE / 4];
    unsigned byte_write_count;
    unsigned word_write_count;
    unsigned long_write_count;
} ctx_t;

static bool rb(void *p, uint32_t a, uint8_t *v) {
    ctx_t *c = p;
    assert(a < RAM_SIZE);
    c->byte_reads[a]++;
    *v = c->ram[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    assert(a + 1 < RAM_SIZE);
    c->word_reads[a >> 1]++;
    *v = ((uint16_t)c->ram[a] << 8) | c->ram[a + 1];
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    assert(a + 3 < RAM_SIZE);
    c->long_reads[a >> 2]++;
    *v = ((uint32_t)c->ram[a] << 24) | ((uint32_t)c->ram[a + 1] << 16) |
         ((uint32_t)c->ram[a + 2] << 8) | c->ram[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    assert(a < RAM_SIZE);
    c->byte_writes[a]++;
    c->byte_write_count++;
    c->ram[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    assert(a + 1 < RAM_SIZE);
    c->word_writes[a >> 1]++;
    c->word_write_count++;
    c->ram[a] = (uint8_t)(v >> 8);
    c->ram[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    assert(a + 3 < RAM_SIZE);
    c->long_writes[a >> 2]++;
    c->long_write_count++;
    c->ram[a] = (uint8_t)(v >> 24);
    c->ram[a + 1] = (uint8_t)(v >> 16);
    c->ram[a + 2] = (uint8_t)(v >> 8);
    c->ram[a + 3] = (uint8_t)v;
    return true;
}
static void putw(ctx_t *c, uint32_t a, uint16_t v) {
    c->ram[a] = (uint8_t)(v >> 8);
    c->ram[a + 1] = (uint8_t)v;
}
static void putl(ctx_t *c, uint32_t a, uint32_t v) {
    c->ram[a] = (uint8_t)(v >> 24);
    c->ram[a + 1] = (uint8_t)(v >> 16);
    c->ram[a + 2] = (uint8_t)(v >> 8);
    c->ram[a + 3] = (uint8_t)v;
}
static uint32_t getl(const ctx_t *c, uint32_t a) {
    return ((uint32_t)c->ram[a] << 24) | ((uint32_t)c->ram[a + 1] << 16) |
           ((uint32_t)c->ram[a + 2] << 8) | c->ram[a + 3];
}

static mc68k_state_t *new_cpu(ctx_t *c, mc68k_model_t model) {
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, c};
    putl(c, 0, RESET_SP);
    putl(c, 4, RESET_PC);
    putl(c, 4 * 4, ILLEGAL_PC);
    putw(c, ILLEGAL_PC, 0x60fe); /* self-loop if more than one op is run */
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    assert(cpu);
    mc68k_reset(cpu);
    return cpu;
}

static int ea_length(int ea, int size) {
    int mode = ea >> 3, reg = ea & 7;
    if (mode <= 4)
        return 0;
    if (mode == 5 || mode == 6)
        return 2;
    if (reg == 0 || reg == 2 || reg == 3)
        return 2;
    if (reg == 1)
        return 4;
    if (reg == 4)
        return size == 4 ? 4 : 2;
    return 0; /* mode 7/register 5-7 is reserved and therefore illegal */
}

/* Emit a brief (not full) extension for every EA that needs one and make the
 * resulting operand harmless.  The returned byte count is the instruction's
 * EA extension length, independently of the implementation's decoder. */
static int install_ea(ctx_t *c, mc68k_state_t *cpu, int ea, int size, uint32_t stream) {
    int mode = ea >> 3, reg = ea & 7;
    for (int i = 0; i < 8; i++) {
        cpu->a[i] = 0x0200 + (uint32_t)i * 0x20;
        cpu->d[i] = 1;
    }
    if (mode == 0)
        cpu->d[reg] = 1;
    else if (mode == 1)
        cpu->a[reg] = 1;
    else if (mode == 2 || mode == 3)
        putl(c, cpu->a[reg], 1);
    else if (mode == 4) {
        cpu->a[reg] = 0x0240 + (uint32_t)reg * 0x20;
        putl(c, cpu->a[reg] - size, 1);
    } else if (mode == 5) {
        putw(c, stream, 0);
        putl(c, cpu->a[reg], 1);
    } else if (mode == 6) {
        cpu->d[0] = 0;
        putw(c, stream, 0); /* D0.W + 0 */
        putl(c, cpu->a[reg], 1);
    } else {
        switch (reg) {
        case 0:
            putw(c, stream, 0x0300);
            putl(c, 0x0300, 1);
            break;
        case 1:
            putl(c, stream, 0x00000320);
            putl(c, 0x0320, 1);
            break;
        case 2:
        case 3:
            cpu->d[0] = 0;
            /* Point past the extension itself so division also receives a
             * nonzero operand (and cannot turn this decode test into DIV0). */
            putw(c, stream, 2);
            putl(c, stream + 2, 1);
            break;
        case 4:
            if (size == 4)
                putl(c, stream, 1);
            else
                putw(c, stream, 1);
            break;
        default:
            break;
        }
    }
    return ea_length(ea, size);
}

static void fail_matrix(const char *name, mc68k_model_t model, int ea, uint16_t opcode,
                        uint32_t expected, uint32_t actual) {
    fprintf(stderr, "%s model=%d ea=%02x opcode=%04x expected_pc=%08x actual_pc=%08x\n", name,
            model, ea, opcode, expected, actual);
    assert(!"68020 decode matrix mismatch");
}

static void assert_illegal(const char *name, ctx_t *c, mc68k_state_t *cpu, mc68k_model_t model,
                           int ea, uint16_t opcode) {
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    if (r.pc != ILLEGAL_PC)
        fail_matrix(name, model, ea, opcode, ILLEGAL_PC, r.pc);
    uint32_t expected_sp = RESET_SP - (model == MC68K_MODEL_68000 ? 6 : 8);
    if (r.a[7] != expected_sp) {
        fprintf(stderr, "%s model=%d ea=%02x opcode=%04x expected_sp=%08x actual_sp=%08x\n",
                name, model, ea, opcode, expected_sp, r.a[7]);
        assert(!"68020 decode matrix illegal-frame mismatch");
    }
    assert(getl(c, r.a[7] + 2) == RESET_PC);
}

typedef struct {
    const char *name;
    uint16_t base;
    uint16_t intrinsic_ext;
    int intrinsic_bytes;
    int size;
    uint64_t valid_ea_mask;
    uint64_t legacy_valid_ea_mask;
    mc68k_model_t min_model;
    bool force_d0_zero;
} matrix_case_t;

static void run_matrix_case(const matrix_case_t *tc) {
    static const mc68k_model_t models[] = {
        MC68K_MODEL_68000,   MC68K_MODEL_68010, MC68K_MODEL_68020,
        MC68K_MODEL_68EC030, MC68K_MODEL_68030, MC68K_MODEL_68040,
    };
    for (size_t mi = 0; mi < sizeof(models) / sizeof(models[0]); mi++) {
        for (int ea = 0; ea < 64; ea++) {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, models[mi]);
            uint16_t opcode = tc->base | (uint16_t)ea;
            putw(&c, RESET_PC, opcode);
            if (tc->intrinsic_bytes >= 2)
                putw(&c, RESET_PC + 2, tc->intrinsic_ext);
            if (tc->intrinsic_bytes == 4)
                putw(&c, RESET_PC + 4, 0);
            int ealen = install_ea(&c, cpu, ea, tc->size, RESET_PC + 2 + tc->intrinsic_bytes);
            /* CHK2 uses D0 here.  A zero value is in the range installed by
             * the generic EA fixture for every legal control EA. */
            if (tc->force_d0_zero)
                cpu->d[0] = 0;
            uint64_t mask =
                models[mi] >= tc->min_model ? tc->valid_ea_mask : tc->legacy_valid_ea_mask;
            bool legal = ((mask >> ea) & UINT64_C(1)) != 0;
            /* 0x0cfc/0x0efc are CAS2.W/L, not CAS <ea=#imm>.  They occupy
             * the otherwise-invalid CAS EA=7/4 encoding by architecture. */
            bool cas2 = (tc->base == 0x0cc0 || tc->base == 0x0ec0) && ea == 0x3c;
            if (cas2) {
                legal = models[mi] >= MC68K_MODEL_68020;
                cpu->a[7] = RESET_SP;
                cpu->d[0] = 0x0200;
                putw(&c, RESET_PC + 4, 0); /* second CAS2 extension word */
            }
            /* An invalid opcode must fault before evaluating its EA.  Keep a
             * real supervisor stack for invalid A7 encodings so the frame
             * check observes the decode failure rather than fixture state. */
            if (!legal)
                cpu->a[7] = RESET_SP;
            mc68k_execute(cpu, 1);
            if (!legal)
                assert_illegal(tc->name, &c, cpu, models[mi], ea, opcode);
            else {
                mc68k_regs_t r;
                uint32_t expected =
                    cas2 ? RESET_PC + 6 : RESET_PC + 2 + tc->intrinsic_bytes + ealen;
                mc68k_get_regs(cpu, &r);
                if (r.pc != expected || r.pc == ILLEGAL_PC)
                    fail_matrix(tc->name, models[mi], ea, opcode, expected, r.pc);
            }
            mc68k_destroy(cpu);
        }
    }
}

static void test_ea_legality_matrix(void) {
    /* BF read = Dn + Control; BF write = Dn + Control Alterable. */
    static const matrix_case_t cases[] = {
        {"BFTST", 0xe8c0, 0x0001, 2, 1, EA_DN | EA_CONTROL, 0, MC68K_MODEL_68020, false},
        {"BFEXTU", 0xe9c0, 0x0001, 2, 1, EA_DN | EA_CONTROL, 0, MC68K_MODEL_68020, false},
        {"BFEXTS", 0xebc0, 0x0001, 2, 1, EA_DN | EA_CONTROL, 0, MC68K_MODEL_68020, false},
        {"BFFFO", 0xedc0, 0x0001, 2, 1, EA_DN | EA_CONTROL, 0, MC68K_MODEL_68020, false},
        {"BFCHG", 0xeac0, 0x0001, 2, 1, EA_DN | EA_CONTROL_ALTERABLE, 0, MC68K_MODEL_68020,
         false},
        {"BFCLR", 0xecc0, 0x0001, 2, 1, EA_DN | EA_CONTROL_ALTERABLE, 0, MC68K_MODEL_68020,
         false},
        {"BFSET", 0xeec0, 0x0001, 2, 1, EA_DN | EA_CONTROL_ALTERABLE, 0, MC68K_MODEL_68020,
         false},
        {"BFINS", 0xefc0, 0x0001, 2, 1, EA_DN | EA_CONTROL_ALTERABLE, 0, MC68K_MODEL_68020,
         false},
        {"CAS.B", 0x0ac0, 0x0000, 2, 1, EA_MEMORY_ALTERABLE, 0, MC68K_MODEL_68020, false},
        {"CAS.W", 0x0cc0, 0x0000, 2, 2, EA_MEMORY_ALTERABLE, 0, MC68K_MODEL_68020, false},
        {"CAS.L", 0x0ec0, 0x0000, 2, 4, EA_MEMORY_ALTERABLE, 0, MC68K_MODEL_68020, false},
        /* Extension bit 11 distinguishes CHK2 from CMP2.  CHK2's D0 is
         * deliberately in-range, so all legal encodings complete normally. */
        {"CHK2.B", 0x00c0, 0x0800, 2, 1, EA_CONTROL, 0, MC68K_MODEL_68020, true},
        {"CHK2.W", 0x02c0, 0x0800, 2, 2, EA_CONTROL, 0, MC68K_MODEL_68020, true},
        {"CHK2.L", 0x04c0, 0x0800, 2, 4, EA_CONTROL, 0, MC68K_MODEL_68020, true},
        {"CMP2.B", 0x00c0, 0x0000, 2, 1, EA_CONTROL, 0, MC68K_MODEL_68020, false},
        {"CMP2.W", 0x02c0, 0x0000, 2, 2, EA_CONTROL, 0, MC68K_MODEL_68020, false},
        {"CMP2.L", 0x04c0, 0x0000, 2, 4, EA_CONTROL, 0, MC68K_MODEL_68020, false},
        {"MULU.L", 0x4c00, 0x0000, 2, 4, EA_DATA, 0, MC68K_MODEL_68020, false},
        {"MULS.L", 0x4c00, 0x0800, 2, 4, EA_DATA, 0, MC68K_MODEL_68020, false},
        /* install_ea() supplies 1 for each legal source; in particular this
         * keeps every DIV.L matrix case out of the divide-by-zero path. */
        {"DIVU.L", 0x4c40, 0x0000, 2, 4, EA_DATA, 0, MC68K_MODEL_68020, false},
        {"DIVS.L", 0x4c40, 0x0800, 2, 4, EA_DATA, 0, MC68K_MODEL_68020, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_matrix_case(&cases[i]);
}

static void test_tst_and_cmpi_extended_eas(void) {
    static const matrix_case_t tst_cases[] = {
        {"TST.B", 0x4a00, 0, 0, 1, EA_DATA, EA_DATA_ALTERABLE, MC68K_MODEL_68020, false},
        {"TST.W", 0x4a40, 0, 0, 2, EA_TST_WL, EA_DATA_ALTERABLE, MC68K_MODEL_68020, false},
        {"TST.L", 0x4a80, 0, 0, 4, EA_TST_WL, EA_DATA_ALTERABLE, MC68K_MODEL_68020, false},
    };
    for (size_t i = 0; i < sizeof(tst_cases) / sizeof(tst_cases[0]); i++)
        run_matrix_case(&tst_cases[i]);

    /* 68020 adds PC-relative CMPI, but deliberately not immediate CMPI.
     * Test every size: its immediate operand changes the EA stream position. */
    static const matrix_case_t cmpi_cases[] = {
        {"CMPI.B", 0x0c00, 0x0001, 2, 1, EA_CMPI_68020, EA_DATA_ALTERABLE, MC68K_MODEL_68020,
         false},
        {"CMPI.W", 0x0c40, 0x0001, 2, 2, EA_CMPI_68020, EA_DATA_ALTERABLE, MC68K_MODEL_68020,
         false},
        {"CMPI.L", 0x0c80, 0x0000, 4, 4, EA_CMPI_68020, EA_DATA_ALTERABLE, MC68K_MODEL_68020,
         false},
    };
    for (size_t i = 0; i < sizeof(cmpi_cases) / sizeof(cmpi_cases[0]); i++)
        run_matrix_case(&cmpi_cases[i]);
}

static void test_cas_success_and_failure(void) {
    static const struct {
        const char *name;
        uint16_t base;
        int size;
        uint32_t compare;
        uint32_t update;
        uint32_t mismatch;
        uint32_t preserved;
    } cases[] = {
        {"CAS.B", 0x0ac0, 1, 0x5a000012, 0x6b000034, 0x00000056, 0x5a000056},
        {"CAS.W", 0x0cc0, 2, 0x5a001234, 0x6b005678, 0x00009abc, 0x5a009abc},
        {"CAS.L", 0x0ec0, 4, 0x12345678, 0x9abcdef0, 0x0badf00d, 0x0badf00d},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int failure = 0; failure < 2; failure++) {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
            uint16_t opcode = (uint16_t)(cases[i].base | 0x10); /* (A0) */
            cpu->a[0] = 0x0200;
            cpu->d[0] = cases[i].compare; /* Dc */
            cpu->d[1] = cases[i].update;  /* Du */
            putw(&c, RESET_PC, opcode);
            putw(&c, RESET_PC + 2, 0x0040); /* Du=D1, Dc=D0 */
            if (cases[i].size == 1)
                c.ram[0x0200] = (uint8_t)(failure ? cases[i].mismatch : cases[i].compare);
            else if (cases[i].size == 2)
                putw(&c, 0x0200, (uint16_t)(failure ? cases[i].mismatch : cases[i].compare));
            else
                putl(&c, 0x0200, failure ? cases[i].mismatch : cases[i].compare);
            mc68k_execute(cpu, 1);
            assert(cpu->pc == RESET_PC + 4);
            if (!failure) {
                assert(cpu->sr & CCR_Z);
                if (cases[i].size == 1)
                    assert(c.ram[0x0200] == (uint8_t)cases[i].update);
                else if (cases[i].size == 2)
                    assert(((uint16_t)c.ram[0x0200] << 8 | c.ram[0x0201]) ==
                           (uint16_t)cases[i].update);
                else
                    assert(getl(&c, 0x0200) == cases[i].update);
            } else {
                assert(!(cpu->sr & CCR_Z));
                assert(cpu->d[0] == cases[i].preserved);
            }
            mc68k_destroy(cpu);
        }
    }
}

/* Execute the instruction after a variable-length EA.  A second instruction
 * is essential: a duplicate EA fetch can otherwise leave PC looking plausible
 * while having consumed the first word of the following instruction. */
static void assert_following_tst_a6(ctx_t *c, mc68k_state_t *cpu, uint32_t pc) {
    putw(c, pc, 0x4a8e); /* TST.L A6 */
    cpu->a[6] = 0;
    mc68k_execute(cpu, 1);
    assert(cpu->pc == pc + 2);
    assert(cpu->sr & CCR_Z);
}

static void test_ea_extension_pc_progress(void) {
    /* These are deliberately direct instruction cases, rather than a second
     * spelling of ea_helpers tests.  The expected lengths are the 68020
     * encoding lengths, and the sentinel proves that the next opcode remains
     * in the stream. */
    static const struct {
        const char *name;
        uint16_t opcode, ext0, ext1;
        int ext_bytes;
        uint32_t target;
    } cases[] = {
        {"d16(An)", 0x4aa8, 0x0000, 0, 2, 0x0200},
        {"brief indexed", 0x4ab0, 0x7800, 0, 2, 0x0200}, /* D7.L + 0 */
        {"absolute word", 0x4ab8, 0x0200, 0, 2, 0x0200},
        {"absolute long", 0x4ab9, 0x0000, 0x0200, 4, 0x0200},
        {"d16(PC)", 0x4aba, 0x00fe, 0, 2, 0x0200},
        {"PC indexed", 0x4abb, 0x7800, 0, 2, 0x0200}, /* D7.L + 0xfe */
        {"immediate long", 0x4abc, 0x0000, 0x0001, 4, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c = {{0}};
        mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
        uint32_t next = RESET_PC + 2 + (uint32_t)cases[i].ext_bytes;
        putw(&c, RESET_PC, cases[i].opcode);
        putw(&c, RESET_PC + 2, cases[i].ext0);
        if (cases[i].ext_bytes == 4)
            putw(&c, RESET_PC + 4, cases[i].ext1);
        cpu->a[0] = 0x0200;
        cpu->d[7] = cases[i].opcode == 0x4abb ? 0xfe : 0;
        if (cases[i].target)
            putl(&c, cases[i].target, 1);
        mc68k_execute(cpu, 1);
        if (cpu->pc != next)
            fail_matrix(cases[i].name, MC68K_MODEL_68020, cases[i].opcode & 0x3f,
                        cases[i].opcode, next, cpu->pc);
        assert(!(cpu->sr & CCR_Z));
        assert_following_tst_a6(&c, cpu, next);
        mc68k_destroy(cpu);
    }
}

static uint32_t cas_target_for_ea(int ea) {
    switch (ea) {
    case 0x28:
    case 0x30:
        return 0x0200;
    case 0x38:
        return 0x0300;
    case 0x39:
        return 0x0320;
    default:
        assert(!"unexpected CAS EA");
        return 0;
    }
}

static void assert_cas_data_cycles(const ctx_t *c, int size, uint32_t target) {
    if (size == 1) {
        assert(c->byte_reads[target] == 1);
        assert(c->byte_writes[target] == 1 && c->byte_write_count == 1);
    } else if (size == 2) {
        assert(c->word_reads[target >> 1] == 1);
        assert(c->word_writes[target >> 1] == 1 && c->word_write_count == 1);
    } else {
        assert(c->long_reads[target >> 2] == 1);
        assert(c->long_writes[target >> 2] == 1 && c->long_write_count == 1);
    }
}

static void test_rmw_extension_fetched_once(void) {
    static const struct {
        const char *name;
        uint16_t opcode;
    } bf_cases[] = {
        {"BFCHG d16", 0xeae8},
        {"BFCLR indexed", 0xecf0},
        {"BFSET abs.W", 0xeef8},
        {"BFINS abs.L", 0xeff9},
    };
    for (size_t i = 0; i < sizeof(bf_cases) / sizeof(bf_cases[0]); i++) {
        ctx_t c = {{0}};
        mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
        int ea = bf_cases[i].opcode & 0x3f;
        putw(&c, RESET_PC, bf_cases[i].opcode);
        putw(&c, RESET_PC + 2, 0x0001); /* offset 0, width 1 */
        int ealen = install_ea(&c, cpu, ea, 1, RESET_PC + 4);
        uint32_t next = RESET_PC + 4 + (uint32_t)ealen;
        uint32_t target = cas_target_for_ea(ea);
        c.ram[target] = 0xff;
        mc68k_execute(cpu, 1);
        assert(cpu->pc == next);
        for (int n = 0; n < ealen; n += 2)
            assert(c.word_reads[(RESET_PC + 4 + (uint32_t)n) >> 1] == 1);
        /* Bit-field RMW first reads the field, then reads and writes the same
         * byte under RMW access.  The sole write must be the resolved EA. */
        assert(c.byte_reads[target] == 2);
        assert(c.byte_writes[target] == 1 && c.byte_write_count == 1);
        assert_following_tst_a6(&c, cpu, next);
        mc68k_destroy(cpu);
    }

    static const int eas[] = {0x28, 0x30, 0x38, 0x39};
    static const uint16_t bases[] = {0x0ac0, 0x0cc0, 0x0ec0};
    static const int sizes[] = {1, 2, 4};
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        for (size_t ei = 0; ei < sizeof(eas) / sizeof(eas[0]); ei++) {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
            uint16_t opcode = (uint16_t)(bases[si] | eas[ei]);
            uint32_t target;
            putw(&c, RESET_PC, opcode);
            putw(&c, RESET_PC + 2, 0x0040); /* Du=D1, Dc=D0 */
            int ealen = install_ea(&c, cpu, eas[ei], sizes[si], RESET_PC + 4);
            if (eas[ei] == 0x30) {
                putw(&c, RESET_PC + 4, 0x7800); /* D7.L + 0 */
                cpu->d[7] = 0;
            }
            target = cas_target_for_ea(eas[ei]);
            cpu->d[0] = 1;
            cpu->d[1] = 2;
            if (sizes[si] == 1)
                c.ram[target] = 1;
            else if (sizes[si] == 2)
                putw(&c, target, 1);
            else
                putl(&c, target, 1);
            uint32_t next = RESET_PC + 4 + (uint32_t)ealen;
            mc68k_execute(cpu, 1);
            assert(cpu->pc == next && (cpu->sr & CCR_Z));
            for (int n = 0; n < ealen; n += 2)
                assert(c.word_reads[(RESET_PC + 4 + (uint32_t)n) >> 1] == 1);
            assert_cas_data_cycles(&c, sizes[si], target);
            assert_following_tst_a6(&c, cpu, next);
            mc68k_destroy(cpu);
        }
    }

    /* ea_rmw_read() owns the update; ea_rmw_write() receives its saved
     * address.  Check both auto-update modes for all CAS operand sizes. */
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        for (int mode = 3; mode <= 4; mode++) {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
            uint32_t initial = 0x0240;
            uint32_t step = sizes[si] == 1 ? 1 : (uint32_t)sizes[si];
            uint32_t target = mode == 3 ? initial : initial - step;
            putw(&c, RESET_PC, (uint16_t)(bases[si] | (mode << 3)));
            putw(&c, RESET_PC + 2, 0x0040);
            cpu->a[0] = initial;
            cpu->d[0] = 1;
            cpu->d[1] = 2;
            if (sizes[si] == 1)
                c.ram[target] = 1;
            else if (sizes[si] == 2)
                putw(&c, target, 1);
            else
                putl(&c, target, 1);
            mc68k_execute(cpu, 1);
            assert(cpu->pc == RESET_PC + 4 &&
                   cpu->a[0] == (mode == 3 ? initial + step : initial - step));
            assert_cas_data_cycles(&c, sizes[si], target);
            assert_following_tst_a6(&c, cpu, RESET_PC + 4);
            mc68k_destroy(cpu);
        }
    }
}

static void test_full_extension_from_instruction(void) {
    /* test_68020_ea.c exercises the decoder in detail.  This table instead
     * proves that actual instructions enter it at the right stream position.
     * Each target contains a nonzero longword so TST.L observes the decoded
     * address, while expected_pc is fixed by the encoded extension words. */
    static const struct {
        const char *name;
        uint16_t opcode, ext, words[4];
        int tail_words;
        uint32_t a0, d0, target;
    } cases[] = {
        {"An base, null bd", 0x4ab0, 0x0110, {0}, 0, 0x0200, 0, 0x0200},
        {"An base, word bd", 0x4ab0, 0x0120, {0x0010}, 1, 0x0200, 0, 0x0210},
        {"PC base, long bd", 0x4abb, 0x0170, {0x0000, 0x0100}, 2, 0, 0, 0x0202},
        {"An pre-index, null outer", 0x4ab0, 0x0111, {0}, 0, 0x0200, 0, 0x0300},
        {"PC pre-index, word outer", 0x4abb, 0x0122, {0x0100, 0x0010}, 2, 0, 0, 0x0310},
        {"An post-index, long outer", 0x4ab0, 0x0117, {0x0000, 0x0010}, 2, 0x0200, 4, 0x0314},
        {"base suppress", 0x4ab0, 0x01a0, {0x0200}, 1, 0xdeadbeef, 0, 0x0200},
        {"index suppress", 0x4abb, 0x0160, {0x0100}, 1, 0, 0xdeadbeef, 0x0202},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx_t c = {{0}};
        mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
        uint32_t next = RESET_PC + 4 + (uint32_t)cases[i].tail_words * 2;
        putw(&c, RESET_PC, cases[i].opcode);
        putw(&c, RESET_PC + 2, cases[i].ext);
        for (int n = 0; n < cases[i].tail_words; n++)
            putw(&c, RESET_PC + 4 + (uint32_t)n * 2, cases[i].words[n]);
        cpu->a[0] = cases[i].a0;
        cpu->d[0] = cases[i].d0;
        if (strstr(cases[i].name, "pre-index")) {
            uint32_t ptr_addr = strstr(cases[i].name, "PC") ? 0x0202 : 0x0200;
            uint32_t ptr =
                strstr(cases[i].name, "PC") ? cases[i].target - 0x10 : cases[i].target;
            putl(&c, ptr_addr, ptr);
        } else if (strstr(cases[i].name, "post-index")) {
            putl(&c, 0x0200, cases[i].target - 0x10 - cases[i].d0);
        }
        putl(&c, cases[i].target, 1);
        mc68k_execute(cpu, 1);
        assert(cpu->pc == next && !(cpu->sr & CCR_Z));
        assert_following_tst_a6(&c, cpu, next);
        mc68k_destroy(cpu);
    }
}

static void test_non_ea_forms(void) {
    /* LINK.L: all address registers consume a long displacement exactly once,
     * push the pre-link register value, and establish the documented frame. */
    for (int an = 0; an < 8; an++) {
        ctx_t c = {{0}};
        mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
        uint32_t old_an = an == 7 ? 0x2800 : 0x12340000u | (uint32_t)an;
        uint32_t frame = (an == 7 ? old_an : RESET_SP) - 4;
        cpu->a[an] = old_an;
        putw(&c, RESET_PC, (uint16_t)(0x4808 | an));
        putl(&c, RESET_PC + 2, UINT32_C(0xfffffff0));
        mc68k_execute(cpu, 1);
        assert(cpu->pc == RESET_PC + 6);
        assert(c.word_reads[(RESET_PC + 2) >> 1] == 1);
        assert(c.word_reads[(RESET_PC + 4) >> 1] == 1);
        assert(getl(&c, frame) == (an == 7 ? frame : old_an));
        if (an != 7)
            assert(cpu->a[an] == frame);
        assert(cpu->a[7] == frame - 0x10);
        mc68k_destroy(cpu);
    }

    /* PACK/UNPK cover every independent source/destination register pair.
     * The memory forms also distinguish A7's two-byte byte step. */
    for (int src = 0; src < 8; src++) {
        for (int dst = 0; dst < 8; dst++) {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
            cpu->d[src] = 0x1234;
            cpu->sr = SR_S | CCR_X | CCR_C;
            putw(&c, RESET_PC, (uint16_t)(0x8140 | (dst << 9) | src));
            putw(&c, RESET_PC + 2, 0);
            mc68k_execute(cpu, 1);
            assert(cpu->pc == RESET_PC + 4 && (cpu->d[dst] & 0xff) == 0x24);
            assert((cpu->sr & (CCR_X | CCR_C | CCR_N | CCR_Z | CCR_V)) == (CCR_X | CCR_C));
            mc68k_destroy(cpu);

            memset(&c, 0, sizeof(c));
            cpu = new_cpu(&c, MC68K_MODEL_68020);
            cpu->d[src] = 0x24;
            cpu->sr = SR_S | CCR_X | CCR_C;
            putw(&c, RESET_PC, (uint16_t)(0x8180 | (dst << 9) | src));
            putw(&c, RESET_PC + 2, 0);
            mc68k_execute(cpu, 1);
            assert(cpu->pc == RESET_PC + 4 && (cpu->d[dst] & 0xffff) == 0x0204);
            assert((cpu->sr & (CCR_X | CCR_C | CCR_N | CCR_Z | CCR_V)) == (CCR_X | CCR_C));
            mc68k_destroy(cpu);

            memset(&c, 0, sizeof(c));
            cpu = new_cpu(&c, MC68K_MODEL_68020);
            uint32_t src_step = src == 7 ? 2 : 1, dst_step = dst == 7 ? 2 : 1;
            uint32_t src_base = 0x0300, dst_base = src == dst ? src_base : 0x0340;
            cpu->a[src] = src_base;
            cpu->a[dst] = dst_base;
            c.ram[src_base - 2 * src_step] = 2;
            c.ram[src_base - src_step] = 4;
            cpu->sr = SR_S | CCR_X | CCR_C;
            putw(&c, RESET_PC, (uint16_t)(0x8148 | (dst << 9) | src));
            putw(&c, RESET_PC + 2, 0);
            mc68k_execute(cpu, 1);
            assert(cpu->pc == RESET_PC + 4);
            assert(cpu->a[src] == src_base - 2 * src_step - (src == dst ? dst_step : 0));
            assert(cpu->a[dst] == dst_base - dst_step - (src == dst ? 2 * src_step : 0));
            assert(c.ram[dst_base - dst_step - (src == dst ? 2 * src_step : 0)] == 0x24);
            assert((cpu->sr & (CCR_X | CCR_C | CCR_N | CCR_Z | CCR_V)) == (CCR_X | CCR_C));
            mc68k_destroy(cpu);

            memset(&c, 0, sizeof(c));
            cpu = new_cpu(&c, MC68K_MODEL_68020);
            src_base = 0x0300;
            dst_base = src == dst ? src_base : 0x0340;
            cpu->a[src] = src_base;
            cpu->a[dst] = dst_base;
            c.ram[src_base - src_step] = 0x24;
            cpu->sr = SR_S | CCR_X | CCR_C;
            putw(&c, RESET_PC, (uint16_t)(0x8188 | (dst << 9) | src));
            putw(&c, RESET_PC + 2, 0);
            mc68k_execute(cpu, 1);
            assert(cpu->pc == RESET_PC + 4);
            assert(cpu->a[src] == src_base - src_step - (src == dst ? 2 * dst_step : 0));
            assert(cpu->a[dst] == dst_base - 2 * dst_step - (src == dst ? src_step : 0));
            assert(c.ram[dst_base - 2 * dst_step - (src == dst ? src_step : 0)] == 2);
            assert(c.ram[dst_base - dst_step - (src == dst ? src_step : 0)] == 4);
            assert((cpu->sr & (CCR_X | CCR_C | CCR_N | CCR_Z | CCR_V)) == (CCR_X | CCR_C));
            mc68k_destroy(cpu);
        }
    }

    /* All TRAPcc condition fields and all operand forms: false conditions
     * preserve straight-line PC, true conditions save the post-operand PC. */
    for (int cc = 0; cc < 16; cc++) {
        for (int form = 0; form < 3; form++) {
            for (int wanted = 0; wanted < 2; wanted++) {
                /* T and F have only their architecturally fixed outcome. */
                if ((cc == 0 && !wanted) || (cc == 1 && wanted))
                    continue;
                int low = form == 0 ? 4 : form == 1 ? 2 : 3;
                int bytes = form == 0 ? 0 : form == 1 ? 2 : 4;
                ctx_t c = {{0}};
                mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
                uint16_t ccr = 0;
                while (eval_cond((uint16_t)(SR_S | ccr), cc) != wanted)
                    ccr++;
                cpu->sr = SR_S | ccr;
                putw(&c, RESET_PC, (uint16_t)(0x50f8 | (cc << 8) | low));
                if (bytes)
                    putw(&c, RESET_PC + 2, 0xbeef);
                if (bytes == 4)
                    putw(&c, RESET_PC + 4, 0xcafe);
                mc68k_execute(cpu, 1);
                mc68k_regs_t r;
                mc68k_get_regs(cpu, &r);
                if (!wanted)
                    assert(r.pc == RESET_PC + 2 + bytes);
                else {
                    assert(r.pc == 0); /* default vector 7 is zero */
                    assert(getl(&c, r.a[7] + 2) == RESET_PC + 2 + bytes);
                }
                mc68k_destroy(cpu);
            }
        }
    }
}

static void test_cas2_matrix(void) {
    for (int long_size = 0; long_size < 2; long_size++) {
        uint16_t opcode = long_size ? 0x0efc : 0x0cfc;
        int size = long_size ? 4 : 2;
        for (int n = 0; n < 8; n++) {
            int r1 = n, r2 = (n + 1) & 7;
            int dc1 = (n + 2) & 7, dc2 = (n + 3) & 7;
            int du1 = (n + 4) & 7, du2 = (n + 5) & 7;
            /* Bit 15 selects Dn/An independently for the two addresses. */
            for (int selector = 0; selector < 4; selector++) {
                for (int failure = 0; failure < 3; failure++) {
                    ctx_t c = {{0}};
                    mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
                    cpu->d[dc1] = 3;
                    cpu->d[dc2] = 5;
                    cpu->d[du1] = 0x11;
                    cpu->d[du2] = 0x13;
                    if (selector & 1)
                        cpu->a[r1] = 0x0200;
                    else
                        cpu->d[r1] = 0x0200;
                    if (selector & 2)
                        cpu->a[r2] = 0x0220;
                    else
                        cpu->d[r2] = 0x0220;
                    putw(&c, RESET_PC, opcode);
                    putw(&c, RESET_PC + 2,
                         (uint16_t)(((selector & 1 ? 0x8000 : 0) | (r1 << 12) | (du1 << 6) |
                                     dc1)));
                    putw(&c, RESET_PC + 4,
                         (uint16_t)(((selector & 2 ? 0x8000 : 0) | (r2 << 12) | (du2 << 6) |
                                     dc2)));
                    if (size == 2) {
                        putw(&c, 0x0200, (uint16_t)(failure == 1 ? 7 : 3));
                        putw(&c, 0x0220, (uint16_t)(failure == 2 ? 9 : 5));
                    } else {
                        putl(&c, 0x0200, failure == 1 ? 7 : 3);
                        putl(&c, 0x0220, failure == 2 ? 9 : 5);
                    }
                    mc68k_execute(cpu, 1);
                    assert(cpu->pc == RESET_PC + 6);
                    assert(c.word_reads[(RESET_PC + 2) >> 1] == 1);
                    assert(c.word_reads[(RESET_PC + 4) >> 1] == 1);
                    assert(((cpu->sr & CCR_Z) != 0) == (failure == 0));
                    if (failure == 0) {
                        assert((size == 2 ? ((uint16_t)c.ram[0x0200] << 8 | c.ram[0x0201])
                                          : getl(&c, 0x0200)) == 0x11);
                        assert((size == 2 ? ((uint16_t)c.ram[0x0220] << 8 | c.ram[0x0221])
                                          : getl(&c, 0x0220)) == 0x13);
                    } else {
                        assert((cpu->d[dc1] & (size == 2 ? 0xffff : UINT32_MAX)) ==
                               (failure == 1 ? 7 : 3));
                        assert((cpu->d[dc2] & (size == 2 ? 0xffff : UINT32_MAX)) ==
                               (failure == 2 ? 9 : 5));
                    }
                    mc68k_destroy(cpu);
                }
            }
        }
    }
}

static void test_long_branches_by_model(void) {
    static const mc68k_model_t models[] = {
        MC68K_MODEL_68000,   MC68K_MODEL_68010, MC68K_MODEL_68020,
        MC68K_MODEL_68EC030, MC68K_MODEL_68030, MC68K_MODEL_68040,
    };
    for (size_t mi = 0; mi < sizeof(models) / sizeof(models[0]); mi++) {
        mc68k_model_t model = models[mi];
        /* disp8=ff is long on 68020+, but short -1 on older CPUs and must
         * not consume either word of the apparent long displacement. */
        ctx_t c = {{0}};
        mc68k_state_t *cpu = new_cpu(&c, model);
        cpu->sr = SR_S | CCR_Z; /* BNE false */
        putw(&c, RESET_PC, 0x66ff);
        putl(&c, RESET_PC + 2, 0x00000020);
        mc68k_execute(cpu, 1);
        assert(cpu->pc == (model >= MC68K_MODEL_68020 ? RESET_PC + 6 : RESET_PC + 2));
        /* MC68000 は2語プリフェッチで RESET_PC+2/+4 を読むが、命令は消費しない（PC で確認済み）。 */
        if (model < MC68K_MODEL_68020 && model != MC68K_MODEL_68000) {
            assert(c.word_reads[(RESET_PC + 2) >> 1] == 0);
            assert(c.word_reads[(RESET_PC + 4) >> 1] == 0);
        }
        mc68k_destroy(cpu);

        memset(&c, 0, sizeof(c));
        cpu = new_cpu(&c, model);
        putw(&c, RESET_PC, 0x61ff); /* BSR long / old short -1 */
        putl(&c, RESET_PC + 2, 0x00000020);
        mc68k_execute(cpu, 1);
        if (model == MC68K_MODEL_68000) {
            /* 旧形式の disp8=-1 は奇数アドレス（RESET_PC+1）への分岐。68000 は分岐先の先読みで
             * address error（vector 3）になる。 */
            assert(cpu->last_exception_vector == 3);
        } else {
            assert(cpu->pc == (model >= MC68K_MODEL_68020 ? RESET_PC + 0x22 : RESET_PC + 1));
            assert(getl(&c, cpu->a[7]) ==
                   (model >= MC68K_MODEL_68020 ? RESET_PC + 6 : RESET_PC + 2));
        }
        /* MC68000 は2語プリフェッチで RESET_PC+2/+4 を読むが、命令は消費しない（PC で確認済み）。 */
        if (model < MC68K_MODEL_68020 && model != MC68K_MODEL_68000) {
            assert(c.word_reads[(RESET_PC + 2) >> 1] == 0);
            assert(c.word_reads[(RESET_PC + 4) >> 1] == 0);
        }
        mc68k_destroy(cpu);

        memset(&c, 0, sizeof(c));
        cpu = new_cpu(&c, model);
        putw(&c, RESET_PC, 0x60ff); /* BRA long / old short -1 */
        putl(&c, RESET_PC + 2, 0x00000020);
        mc68k_execute(cpu, 1);
        if (model == MC68K_MODEL_68000)
            assert(cpu->last_exception_vector == 3); /* 奇数アドレスへの分岐は address error */
        else
            assert(cpu->pc == (model >= MC68K_MODEL_68020 ? RESET_PC + 0x22 : RESET_PC + 1));
        /* MC68000 は2語プリフェッチで RESET_PC+2/+4 を読むが、命令は消費しない（PC で確認済み）。 */
        if (model < MC68K_MODEL_68020 && model != MC68K_MODEL_68000) {
            assert(c.word_reads[(RESET_PC + 2) >> 1] == 0);
            assert(c.word_reads[(RESET_PC + 4) >> 1] == 0);
        }
        mc68k_destroy(cpu);
    }

    /* Every Bcc condition has a long form.  Test both paths where the
     * condition admits them; BRA and BSR are covered above, and Bcc's false
     * path must still consume both extension words. */
    for (int cc = 2; cc < 16; cc++) {
        for (int wanted = 0; wanted < 2; wanted++) {
            uint16_t sr = SR_S;
            while (eval_cond(sr, cc) != wanted)
                sr++;
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, MC68K_MODEL_68020);
            cpu->sr = sr;
            putw(&c, RESET_PC, (uint16_t)(0x6000 | (cc << 8) | 0xff));
            putl(&c, RESET_PC + 2, 0x20);
            mc68k_execute(cpu, 1);
            assert(cpu->pc == (wanted ? RESET_PC + 0x22 : RESET_PC + 6));
            mc68k_destroy(cpu);
        }
    }
}

/* The EA tables above cover every encoding of the variable-format 68020
 * instructions.  Keep the model boundary for the remaining fixed-format
 * opcodes explicit too: this is deliberately a fixed-expectation test, not
 * an assertion about the opcode registration helpers. */
static void test_fixed_forms_by_model(void) {
    static const mc68k_model_t models[] = {
        MC68K_MODEL_68000,   MC68K_MODEL_68010, MC68K_MODEL_68020,
        MC68K_MODEL_68EC030, MC68K_MODEL_68030, MC68K_MODEL_68040,
    };
    for (size_t mi = 0; mi < sizeof(models) / sizeof(models[0]); mi++) {
        mc68k_model_t model = models[mi];
        bool is_68020_plus = model >= MC68K_MODEL_68020;

        /* EXTB.L is defined for all eight data registers only on 68020+. */
        for (int dn = 0; dn < 8; dn++) {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, model);
            uint16_t opcode = (uint16_t)(0x49c0 | dn);
            putw(&c, RESET_PC, opcode);
            cpu->d[dn] = 0x12340080;
            mc68k_execute(cpu, 1);
            if (!is_68020_plus)
                assert_illegal("EXTB.L", &c, cpu, model, dn, opcode);
            else {
                assert(cpu->pc == RESET_PC + 2);
                assert(cpu->d[dn] == UINT32_C(0xffffff80));
            }
            mc68k_destroy(cpu);
        }

        /* CHK.L D0,Dn must not be confused with the legacy CHK.W encoding.
         * D0 supplies the inclusive bound, and every Dn remains in range. */
        for (int dn = 0; dn < 8; dn++) {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, model);
            uint16_t opcode = (uint16_t)(0x4100 | (dn << 9));
            putw(&c, RESET_PC, opcode);
            cpu->d[0] = 1;
            cpu->d[dn] = 1;
            mc68k_execute(cpu, 1);
            if (!is_68020_plus)
                assert_illegal("CHK.L", &c, cpu, model, dn, opcode);
            else
                assert(cpu->pc == RESET_PC + 2);
            mc68k_destroy(cpu);
        }

        /* These 68020 fixed forms have no EA field; use concrete operands so
         * a legal decode proves its stream length and execution path. */
        {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, model);
            putw(&c, RESET_PC, 0x8140); /* PACK D0,D0,#0 */
            putw(&c, RESET_PC + 2, 0);
            cpu->d[0] = 0x1234;
            mc68k_execute(cpu, 1);
            if (!is_68020_plus)
                assert_illegal("PACK", &c, cpu, model, 0, 0x8140);
            else
                assert(cpu->pc == RESET_PC + 4 && (cpu->d[0] & 0xff) == 0x24);
            mc68k_destroy(cpu);
        }
        {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, model);
            putw(&c, RESET_PC, 0x8180); /* UNPK D0,D0,#0 */
            putw(&c, RESET_PC + 2, 0);
            cpu->d[0] = 0x24;
            mc68k_execute(cpu, 1);
            if (!is_68020_plus)
                assert_illegal("UNPK", &c, cpu, model, 0, 0x8180);
            else
                assert(cpu->pc == RESET_PC + 4 && (cpu->d[0] & 0xffff) == 0x0204);
            mc68k_destroy(cpu);
        }
        {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, model);
            putw(&c, RESET_PC, 0x4808); /* LINK.L A0,#0 */
            putl(&c, RESET_PC + 2, 0);
            cpu->a[0] = 0x2222;
            mc68k_execute(cpu, 1);
            if (!is_68020_plus)
                assert_illegal("LINK.L", &c, cpu, model, 0, 0x4808);
            else {
                assert(cpu->pc == RESET_PC + 6);
                assert(cpu->a[0] == RESET_SP - 4 && cpu->a[7] == RESET_SP - 4);
                assert(getl(&c, RESET_SP - 4) == 0x2222);
            }
            mc68k_destroy(cpu);
        }
        {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, model);
            putw(&c, RESET_PC, 0x51fc); /* TRAPF: never traps */
            mc68k_execute(cpu, 1);
            if (!is_68020_plus)
                assert_illegal("TRAPF", &c, cpu, model, 0, 0x51fc);
            else
                assert(cpu->pc == RESET_PC + 2);
            mc68k_destroy(cpu);
        }
        {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, model);
            putw(&c, RESET_PC, 0x0cfc); /* CAS2.W (A0):(A1),D0:D0,D0:D0 */
            putw(&c, RESET_PC + 2, 0x8000);
            putw(&c, RESET_PC + 4, 0x9000);
            cpu->a[0] = 0x0200;
            cpu->a[1] = 0x0220;
            cpu->d[0] = 1;
            putw(&c, 0x0200, 1);
            putw(&c, 0x0220, 1);
            mc68k_execute(cpu, 1);
            if (!is_68020_plus)
                assert_illegal("CAS2.W", &c, cpu, model, 0x3c, 0x0cfc);
            else
                assert(cpu->pc == RESET_PC + 6 && (cpu->sr & CCR_Z));
            mc68k_destroy(cpu);
        }

        /* BKPT is intentionally observable as vector 4 for every model in
         * this core (including the old-model ILLEGAL fallback).  Do not turn
         * that established contract into a model-dependent expectation. */
        {
            ctx_t c = {{0}};
            mc68k_state_t *cpu = new_cpu(&c, model);
            putl(&c, 4 * 4, 0x0a00);
            putw(&c, RESET_PC, 0x484f); /* BKPT #7 */
            mc68k_execute(cpu, 1);
            assert(cpu->pc == 0x0a00);
            assert(cpu->a[7] == RESET_SP - (model == MC68K_MODEL_68000 ? 6 : 8));
            assert(getl(&c, cpu->a[7] + 2) == RESET_PC);
            mc68k_destroy(cpu);
        }

        /* The full EA matrix already covers these encodings, but make the
         * 68020-only PC-relative CMPI extension visible at the model boundary
         * with fixed instruction lengths and a successful comparison. */
        for (int size_code = 0; size_code < 3; size_code++) {
            int size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
            int immediate_bytes = size == 4 ? 4 : 2;
            for (int indexed = 0; indexed < 2; indexed++) {
                int ea = indexed ? 0x3b : 0x3a;
                ctx_t c = {{0}};
                mc68k_state_t *cpu = new_cpu(&c, model);
                uint16_t opcode = (uint16_t)(0x0c00 | (size_code << 6) | ea);
                uint32_t stream = RESET_PC + 2 + (uint32_t)immediate_bytes;
                putw(&c, RESET_PC, opcode);
                if (size == 4)
                    putl(&c, RESET_PC + 2, 1);
                else
                    putw(&c, RESET_PC + 2, 1);
                assert(install_ea(&c, cpu, ea, size, stream) == 2);
                if (size == 1)
                    c.ram[stream + 2] = 1;
                else if (size == 2)
                    putw(&c, stream + 2, 1);
                else
                    putl(&c, stream + 2, 1);
                /* install_ea() initializes every An, including A7.  An
                 * invalid decode must use the reset supervisor stack so the
                 * following frame assertion observes the decoder outcome. */
                if (!is_68020_plus)
                    cpu->a[7] = RESET_SP;
                mc68k_execute(cpu, 1);
                if (!is_68020_plus)
                    assert_illegal("CMPI PC-relative", &c, cpu, model, ea, opcode);
                else {
                    assert(cpu->pc == stream + 2);
                    if (!(cpu->sr & CCR_Z)) {
                        fprintf(stderr,
                                "CMPI PC-relative model=%d size=%d indexed=%d sr=%04x\n", model,
                                size, indexed, cpu->sr);
                        assert(!"CMPI PC-relative comparison mismatch");
                    }
                }
                mc68k_destroy(cpu);
            }
        }
    }
}

int main(void) {
    test_ea_legality_matrix();
    test_tst_and_cmpi_extended_eas();
    test_cas_success_and_failure();
    test_ea_extension_pc_progress();
    test_rmw_extension_fetched_once();
    test_full_extension_from_instruction();
    test_non_ea_forms();
    test_cas2_matrix();
    test_long_branches_by_model();
    test_fixed_forms_by_model();
    return 0;
}
