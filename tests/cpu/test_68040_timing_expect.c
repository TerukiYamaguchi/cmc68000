/*
 * test_68040_timing_expect.c — 68040 の期待サイクル CSV を実行結果と突き合わせる
 *
 * tests/data/cpu/mc68040_cycle_expectations.csv の全行について、組立表の命令列を 68040 で 1 命令
 * 実行し、返ったサイクル数が expected_cycles と一致することを検査する。
 * 期待値の出典は CSV（040UM の印字値）で、このファイルに数値は書かない。
 *
 * 組立表に無い行は失敗とする。組立てられない行は理由つきの除外表に置く。
 * 組立表・除外表の項目が CSV の行を指さない場合（表の陳腐化）も失敗とする。
 *
 * 続けて raw CSV（040UM Table 10-6 の印字値）を読み、EA 形ごとに行が分かれている命令族
 * （ADD、ADDA、ADDQ、ADDI、CLR）の全 EA 形を実行して、ea_calculate + execute_base +
 * ea_fetch_accesses の和と突き合わせる。命令族の raw 行が 1 行でも EA 形の表に当たらなければ
 * 失敗とする。
 *
 * 使い方: test_68040_timing_expect <expectations.csv> <raw.csv>
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE                0x4000u
#define RESET_SP                0x3000u
#define CODE_PC                 0x0100u
#define RETURN_PC               0x0200u
#define HANDLER_PC              0x0900u
#define DATA_BASE               0x1000u
#define DATA_END                0x1C00u
#define POINTER_SLOT            0x1520u /* A1 + bd(0x100) + D0: 間接参照の指す先 */
#define POINTER_TARGET          0x1800u
#define AREG_BASE               0x1400u
#define AREG_STEP               0x20u
#define VECTOR_COUNT            256u
#define DATA_FILL               0x00030003u
#define SR_SUPERVISOR           0x2700u
#define SR_SUPERVISOR_Z         0x2704u
#define SR_SUPERVISOR_V         0x2702u
#define RTE_FRAME_SR            0x2000u
#define RTE_FRAME_PC_OFFSET     2u
#define RTE_FRAME_FORMAT_OFFSET 6u
#define RTE_FORMAT_SHIFT        12u
#define MAX_WORDS               8u /* 命令語 + 即値 FAMILY_IMM_MAX + EA 拡張 EA_EXT_MAX */
#define CSV_FIELDS              7u
#define RAW_CSV_FIELDS          16u
#define CSV_FIELDS_MAX          RAW_CSV_FIELDS
#define RAW_COL_INSTRUCTION     4u
#define RAW_COL_CONDITION       5u
#define RAW_COL_CALCULATE       7u
#define RAW_COL_EXECUTE_BASE    9u
#define RAW_COL_FETCH           10u
#define RAW_COL_PROJECTION      12u
#define RAW_PROJECTION_NOMINAL  "nominal_nonoverlap"
#define RAW_ROWS_MAX            512u
#define EA_EXT_MAX              6u
#define FAMILY_IMM_MAX          2u
#define LINE_LEN                1024u
#define FIELD_LEN               128u
#define ROWS_MAX                256u
#define MOVE16_ABS_ADDRESS_HIGH 0x0000u
#define MOVE16_ABS_ADDRESS_LOW  0x1800u
#define EXPECT_ARGC             3

static uint8_t g_ram[RAM_SIZE];

static void wr(uint32_t a, unsigned n, uint32_t v) {
    for (unsigned i = 0; i < n; i++)
        if (a + i < RAM_SIZE)
            g_ram[a + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}
static bool rd(uint32_t a, unsigned n, uint32_t *v) {
    *v = 0;
    for (unsigned i = 0; i < n; i++)
        *v = (*v << 8) | (a + i < RAM_SIZE ? g_ram[a + i] : 0u);
    return true;
}
static bool rb(void *c, uint32_t a, uint8_t *v) {
    (void)c;
    uint32_t t;
    rd(a, 1, &t);
    *v = (uint8_t)t;
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    (void)c;
    uint32_t t;
    rd(a, 2, &t);
    *v = (uint16_t)t;
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    (void)c;
    return rd(a, 4, v);
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    (void)c;
    wr(a, 1, v);
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    (void)c;
    wr(a, 2, v);
    return true;
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    (void)c;
    wr(a, 4, v);
    return true;
}
static uint8_t pb(void *c, uint32_t a) {
    (void)c;
    return a < RAM_SIZE ? g_ram[a] : 0;
}
static uint16_t pw(void *c, uint32_t a) {
    return (uint16_t)((pb(c, a) << 8) | pb(c, a + 1));
}
static uint32_t pl(void *c, uint32_t a) {
    return ((uint32_t)pw(c, a) << 16) | pw(c, a + 2);
}

/* 命令ごとの追加の初期状態。 */
static void prime_return_address(mc68k_state_t *cpu) {
    wr(cpu->a[7], 4, RETURN_PC);
}
static void prime_indirect_pointer(mc68k_state_t *cpu) {
    (void)cpu;
    wr(POINTER_SLOT, 4, POINTER_TARGET);
}
static void prime_rte(mc68k_state_t *cpu, unsigned format) {
    wr(cpu->a[7], 2, RTE_FRAME_SR);
    wr(cpu->a[7] + RTE_FRAME_PC_OFFSET, 4, RETURN_PC);
    wr(cpu->a[7] + RTE_FRAME_FORMAT_OFFSET, 2, format << RTE_FORMAT_SHIFT);
}
static void prime_rte_f0(mc68k_state_t *cpu) {
    prime_rte(cpu, 0);
}
static void prime_rte_f2(mc68k_state_t *cpu) {
    prime_rte(cpu, 2);
}
static void prime_rte_f3(mc68k_state_t *cpu) {
    prime_rte(cpu, 3);
}
static void prime_rte_f4(mc68k_state_t *cpu) {
    prime_rte(cpu, 4);
}
static void prime_rte_f7(mc68k_state_t *cpu) {
    prime_rte(cpu, 7);
}
static void prime_rtr(mc68k_state_t *cpu) {
    wr(cpu->a[7], 2, 0);
    wr(cpu->a[7] + 2, 4, RETURN_PC);
}

typedef struct {
    const char *instruction;
    const char *condition;
    uint16_t words[MAX_WORDS];
    unsigned count;
    uint16_t sr; /* 0 のとき SR_SUPERVISOR */
    void (*prime)(mc68k_state_t *);
    bool ec_lc; /* FPU を持たない MC68EC040/MC68LC040 で実行する */
    bool covered;
} assembly_t;

#define ROW(ins, cond, n, sr, prime, ...) {ins, cond, {__VA_ARGS__}, n, sr, prime, false, false}
#define ROW_EC_LC(ins, cond, n, sr, prime, ...) \
    {ins, cond, {__VA_ARGS__}, n, sr, prime, true, false}

static assembly_t g_assembly[] = {
    ROW("DBcc", "false,count>-1", 2, 0, NULL, 0x51C9, 0x0004),
    ROW("DBcc", "false,count=-1", 2, 0, NULL, 0x51C8, 0x0004),
    ROW("DBcc", "true", 2, 0, NULL, 0x50C8, 0x0004),
    ROW("Bcc", "branch-taken", 1, SR_SUPERVISOR_Z, NULL, 0x6704),
    ROW("Bcc", "branch-not-taken", 1, 0, NULL, 0x6704),
    ROW("BSR", "always", 1, 0, NULL, 0x6104),
    ROW("RTS", "always", 1, 0, prime_return_address, 0x4E75),
    ROW("NOP", "always", 1, 0, NULL, 0x4E71),
    ROW("RESET", "always", 1, 0, NULL, 0x4E70),
    ROW("TRAPV", "not-taken", 1, 0, NULL, 0x4E76),
    ROW("TRAPV", "taken", 1, SR_SUPERVISOR_V, NULL, 0x4E76),
    ROW("MOVE USP", "An-to-USP", 1, 0, NULL, 0x4E61),
    ROW("MOVE USP", "USP-to-An", 1, 0, NULL, 0x4E69),
    ROW("ILLEGAL/A-line/F-line", "unimplemented", 1, 0, NULL, 0x4AFC),
    ROW("MOVEC", "Rn-to-Rc", 2, 0, NULL, 0x4E7B, 0x1002),
    ROW("MOVEC", "Rc-to-Rn", 2, 0, NULL, 0x4E7A, 0x1002),
    ROW("MOVEP", "word-memory-to-register", 2, 0, NULL, 0x0308, 0x0010),
    ROW("MOVEP", "long-memory-to-register", 2, 0, NULL, 0x0348, 0x0010),
    ROW("MOVEP", "word-register-to-memory", 2, 0, NULL, 0x0388, 0x0010),
    ROW("MOVEP", "long-register-to-memory", 2, 0, NULL, 0x03C8, 0x0010),
    ROW("PFLUSH/PFLUSHA/PFLUSHN", "typical", 1, 0, NULL, 0xF518),
    ROW("PFLUSHAN", "typical", 1, 0, NULL, 0xF510),
    ROW("RTE", "format-0", 1, 0, prime_rte_f0, 0x4E73),
    ROW("RTE", "format-2", 1, 0, prime_rte_f2, 0x4E73),
    ROW("RTE", "format-3", 1, 0, prime_rte_f3, 0x4E73),
    ROW_EC_LC("RTE", "format-4", 1, 0, prime_rte_f4, 0x4E73),
    ROW("RTE", "format-7", 1, 0, prime_rte_f7, 0x4E73),
    ROW("TRAP#", "always", 1, 0, NULL, 0x4E40),
    ROW("MOVEQ", "always", 1, 0, NULL, 0x7001),
    ROW("ADD/SUB", "register-source", 1, 0, NULL, 0xD282),
    ROW("ADDA/SUBA", "Dn-source", 1, 0, NULL, 0xD5C1),
    ROW("ADDA/SUBA", "An-source", 1, 0, NULL, 0xD5C9),
    ROW("AND/OR", "Dn-source", 1, 0, NULL, 0xC481),
    ROW("SWAP", "always", 1, 0, NULL, 0x4841),
    ROW("EXT", "word", 1, 0, NULL, 0x4881),
    ROW("EXT", "long", 1, 0, NULL, 0x48C1),
    ROW("EXG", "Dy/Dx", 1, 0, NULL, 0xC342),
    ROW("EXG", "Ay/Ax", 1, 0, NULL, 0xC34A),
    ROW("EXG", "Dy/Ax", 1, 0, NULL, 0xC38A),
    ROW("ABCD", "Dy/Dx", 1, 0, NULL, 0xC501),
    ROW("ABCD", "-(Ay)/-(Ax)", 1, 0, NULL, 0xC509),
    ROW("ADDX", "Dy/Dx", 1, 0, NULL, 0xD581),
    ROW("ADDX", "-(Ay)/-(Ax)", 1, 0, NULL, 0xD589),
    ROW("CMPM", "always", 1, 0, NULL, 0xB589),
    ROW("LINK", "always", 2, 0, NULL, 0x4E51, 0xFFF8),
    ROW("EXTB", "long", 1, 0, NULL, 0x49C1),
    ROW("RTR", "always", 1, 0, prime_rtr, 0x4E77),
    ROW("SBCD", "Dy/Dx", 1, 0, NULL, 0x8501),
    ROW("SBCD", "-(Ay)/-(Ax)", 1, 0, NULL, 0x8509),
    ROW("SUBX", "Dy/Dx", 1, 0, NULL, 0x9581),
    ROW("SUBX", "-(Ay)/-(Ax)", 1, 0, NULL, 0x9589),
    ROW("UNLK", "always", 1, 0, NULL, 0x4E59),
    ROW("MOVE16", "(Ax)+,(Ay)+", 2, 0, NULL, 0xF621, 0xA000),
    ROW("MOVE16", "xxx.L,(An)", 3, 0, NULL, 0xF619, MOVE16_ABS_ADDRESS_HIGH,
        MOVE16_ABS_ADDRESS_LOW),
    ROW("MOVE16", "xxx.L,(An)+", 3, 0, NULL, 0xF609, MOVE16_ABS_ADDRESS_HIGH,
        MOVE16_ABS_ADDRESS_LOW),
    ROW("MOVE16", "(An),xxx.L", 3, 0, NULL, 0xF611, MOVE16_ABS_ADDRESS_HIGH,
        MOVE16_ABS_ADDRESS_LOW),
    ROW("MOVE16", "(An)+,xxx.L", 3, 0, NULL, 0xF601, MOVE16_ABS_ADDRESS_HIGH,
        MOVE16_ABS_ADDRESS_LOW),
    ROW("ANDI", "CCR", 2, 0, NULL, 0x023C, 0x00FF),
    ROW("ANDI", "SR", 2, 0, NULL, 0x027C, 0xFFFF),
    ROW("EORI", "CCR", 2, 0, NULL, 0x0A3C, 0x0000),
    ROW("EORI", "SR", 2, 0, NULL, 0x0A7C, 0x0000),
    ROW("ORI", "CCR", 2, 0, NULL, 0x003C, 0x0000),
    ROW("ORI", "SR", 2, 0, NULL, 0x007C, 0x0000),
    ROW("PACK", "register", 2, 0, NULL, 0x8541, 0x0000),
    ROW("PACK", "memory", 2, 0, NULL, 0x8549, 0x0000),
    ROW("RTD", "always", 2, 0, prime_return_address, 0x4E74, 0x0004),
    ROW("TRAPcc", "taken", 1, 0, NULL, 0x50FC),
    ROW("TRAPcc", "not-taken", 1, 0, NULL, 0x51FC),
    ROW("UNPK", "register", 2, 0, NULL, 0x8581, 0x0000),
    ROW("UNPK", "memory", 2, 0, NULL, 0x8589, 0x0000),
    ROW("ADDQ/SUBQ", "Dn/An", 1, 0, NULL, 0x5281),
    ROW("CLR", "Dn", 1, 0, NULL, 0x4281),
    ROW("TST", "Dn/An", 1, 0, NULL, 0x4A81),
    ROW("MOVE", "Dn to Dn", 1, 0, NULL, 0x2401),
    ROW("MOVE", "(An) to Dn", 1, 0, NULL, 0x2411),
    ROW("MOVE", "(An)+ to (An)+", 1, 0, NULL, 0x24D9),
    ROW("MOVE", "Dn to (d8,An,Xn)", 2, 0, NULL, 0x2581, 0x0000),
    ROW("MOVE", "(bd,An,Xn) to Dn", 4, 0, NULL, 0x2431, 0x0130, 0x0000, 0x0100),
    ROW("MOVE", "Dn to (bd,An,Xn)", 4, 0, NULL, 0x2581, 0x0130, 0x0000, 0x0100),
    ROW("MOVE", "([bd,An,Xn]) to Dn", 4, 0, prime_indirect_pointer, 0x2431, 0x0131, 0x0000,
        0x0100),
    ROW("ADD/SUB", "memory-source-basic", 1, 0, NULL, 0xD491),
    ROW("ADD/SUB", "brief-index-source", 2, 0, NULL, 0xD4B1, 0x0000),
    ROW("ADD/SUB", "IS=1 memory-indirect", 4, 0, prime_indirect_pointer, 0xD4B1, 0x0171, 0x0000,
        0x0100),
    ROW("ADDA/SUBA", "memory-source-basic", 1, 0, NULL, 0xD5D1),
    ROW("ADDA/SUBA", "brief-index-source", 2, 0, NULL, 0xD5F1, 0x0000),
    ROW("AND/OR", "memory-source-basic", 1, 0, NULL, 0xC491),
    ROW("TST", "memory-source-brief-index", 2, 0, NULL, 0x4AB1, 0x0000),
    ROW("ADDQ/SUBQ", "memory-destination-basic", 1, 0, NULL, 0x5291),
    ROW("ADDQ/SUBQ", "brief-index-destination", 2, 0, NULL, 0x52B1, 0x0000),
    ROW("CHK", "in-bounds", 1, 0, NULL, 0x4591),
    ROW("ADDI/SUBI", "memory-destination-basic", 3, 0, NULL, 0x0691, 0x0000, 0x0001),
    ROW("ANDI/ORI/EORI", "memory-destination-basic", 3, 0, NULL, 0x0291, 0x0000, 0x0001),
    ROW("CLR", "memory-destination-basic", 1, 0, NULL, 0x4291),
    ROW("CLR", "brief-index-destination", 2, 0, NULL, 0x42B1, 0x0000),
    ROW("CLR", "active-index-full-extension", 3, 0, NULL, 0x42B1, 0x0120, 0x0100),
    ROW("CMP", "memory-source-basic", 1, 0, NULL, 0xB491),
    ROW("ADD/SUB/EOR", "memory-destination-standard", 1, 0, NULL, 0xB391),
    ROW("CMPI/CMPA.L", "memory-source-basic", 3, 0, NULL, 0x0C91, 0x0000, 0x0001),
    ROW("NEG/NEGX/NOT", "memory-destination-basic", 1, 0, NULL, 0x4491),
    ROW("ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR", "register", 1, 0, NULL, 0xE389),
    ROW("ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR", "memory-destination", 1, 0, NULL, 0xE2D1),
    ROW("MULU.W", "memory-source-basic", 1, 0, NULL, 0xC4D1),
    ROW("MULS.W", "memory-source-basic", 1, 0, NULL, 0xC5D1),
    ROW("NBCD", "memory-destination-basic", 1, 0, NULL, 0x4811),
    ROW("LEA", "control-ea", 1, 0, NULL, 0x45D1),
    ROW("PEA", "control-ea", 1, 0, NULL, 0x4851),
    ROW("MOVE to CCR", "source-ea", 1, 0, NULL, 0x44D1),
    ROW("MOVE from CCR", "destination-ea", 1, 0, NULL, 0x42D1),
    ROW("MOVE from SR", "destination-ea", 1, 0, NULL, 0x40D1),
    ROW("MOVE to SR", "source-ea-minimum", 1, 0, NULL, 0x46D1),
    ROW("DIVS.W/DIVU.W", "source-ea", 1, 0, NULL, 0x84D1),
    ROW("JMP", "control-ea", 1, 0, NULL, 0x4ED1),
    ROW("JSR", "control-ea", 1, 0, NULL, 0x4E91),
    ROW("MULS.L/MULU.L", "source-ea", 4, 0, NULL, 0x4C3C, 0x2000, 0x0000, 0x0002),
    ROW("CMP2", "control-ea", 2, 0, NULL, 0x04D1, 0x2000),
    ROW("bit-field", "Dn", 2, 0, NULL, 0xE8C1, 0x0008),
    ROW("MOVEM", "register-to-memory", 2, 0, NULL, 0x48D1, 0x0001),
    ROW("MOVEM.L", "memory-to-register", 2, 0, NULL, 0x4CD1, 0x0001),
    ROW("MOVES", "memory-to-register", 2, 0, NULL, 0x0E91, 0x2000),
    ROW("bit-field", "memory", 2, 0, NULL, 0xE8D1, 0x0008),
    ROW("CHK", "in-bounds (BR,Xn)", 2, 0, NULL, 0x4531, 0x0110),
    ROW("CHK", "in-bounds (bd,BR,Xn)", 4, 0, NULL, 0x4531, 0x0130, 0x0000, 0x0100),
    ROW("CHK", "in-bounds one-level memory-indirect", 4, 0, prime_indirect_pointer, 0x4531,
        0x0131, 0x0000, 0x0100),
    ROW("CHK", "in-bounds memory-indirect with od", 6, 0, prime_indirect_pointer, 0x4531,
        0x0133, 0x0000, 0x0100, 0x0000, 0x0100),
    ROW("CINVL", "base", 1, 0, NULL, 0xF448),
    ROW("CINVP", "base", 1, 0, NULL, 0xF450),
    ROW("CINVA", "base", 1, 0, NULL, 0xF458),
    ROW("CPUSHL", "best-case", 1, 0, NULL, 0xF468),
    ROW("CPUSHP", "best-case", 1, 0, NULL, 0xF470),
    ROW("CPUSHA", "best-case", 1, 0, NULL, 0xF478),
};
#define ASSEMBLY_COUNT (sizeof g_assembly / sizeof g_assembly[0])

/* 組立てられない CSV 行。理由は必須。 */
typedef struct {
    const char *instruction;
    const char *condition;
    const char *reason;
    bool covered;
} exclusion_t;

static exclusion_t g_exclusion[] = {
    {NULL, NULL, NULL, false},
};

static void prepare(mc68k_state_t *cpu, const assembly_t *a) {
    memset(g_ram, 0, 0x0800);
    for (uint32_t addr = DATA_BASE; addr < DATA_END; addr += 4)
        wr(addr, 4, DATA_FILL);
    for (unsigned v = 0; v < VECTOR_COUNT; v++)
        wr(v * 4, 4, HANDLER_PC);
    wr(HANDLER_PC, 2, 0x60FE);
    for (unsigned i = 0; i < a->count; i++)
        wr(CODE_PC + 2u * i, 2, a->words[i]);
    for (int i = 0; i < 8; i++) {
        cpu->d[i] = 1;
        cpu->a[i] = AREG_BASE + (uint32_t)i * AREG_STEP;
    }
    cpu->d[0] = 0;
    cpu->a[7] = RESET_SP;
    cpu->usp = RESET_SP - 0x100;
    cpu->pc = CODE_PC;
    cpu->exec_state = CPU_NORMAL;
    cpu->sr = a->sr ? a->sr : SR_SUPERVISOR;
    if (a->prime)
        a->prime(cpu);
}

/* CSV 1 行を欄へ分ける（二重引用符で囲んだ欄の中のカンマと "" に対応）。 */
static unsigned csv_split(char *line, char field[CSV_FIELDS_MAX][FIELD_LEN],
                          unsigned max_fields) {
    unsigned n = 0;
    char *p = line;
    while (n < max_fields) {
        size_t len = 0;
        if (*p == '"') {
            p++;
            for (;;) {
                if (*p == '"' && p[1] == '"') {
                    if (len + 1 < FIELD_LEN)
                        field[n][len++] = '"';
                    p += 2;
                } else if (*p == '"' || *p == 0) {
                    break;
                } else if (len + 1 < FIELD_LEN) {
                    field[n][len++] = *p++;
                } else {
                    p++;
                }
            }
            if (*p == '"')
                p++;
        } else {
            while (*p && *p != ',' && len + 1 < FIELD_LEN)
                field[n][len++] = *p++;
        }
        field[n][len] = 0;
        n++;
        if (*p != ',')
            break;
        p++;
    }
    return n;
}

static assembly_t *find_assembly(const char *instruction, const char *condition) {
    for (size_t i = 0; i < ASSEMBLY_COUNT; i++)
        if (strcmp(g_assembly[i].instruction, instruction) == 0 &&
            strcmp(g_assembly[i].condition, condition) == 0)
            return &g_assembly[i];
    return NULL;
}

static exclusion_t *find_exclusion(const char *instruction, const char *condition) {
    for (exclusion_t *e = g_exclusion; e->instruction; e++)
        if (strcmp(e->instruction, instruction) == 0 && strcmp(e->condition, condition) == 0)
            return e;
    return NULL;
}

/* raw CSV の EA 形別の行。 */
typedef struct {
    char instruction[FIELD_LEN];
    char condition[FIELD_LEN];
    int sum;
    bool checked;
} raw_row_t;

static raw_row_t g_raw[RAW_ROWS_MAX];
static unsigned g_raw_count;

static void load_raw(FILE *f) {
    char line[LINE_LEN];
    char field[CSV_FIELDS_MAX][FIELD_LEN];
    bool header = true;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (header) {
            header = false;
            continue;
        }
        if (csv_split(line, field, RAW_CSV_FIELDS) != RAW_CSV_FIELDS) {
            fprintf(stderr, "bad raw CSV row: %s\n", line);
            exit(2);
        }
        if (strcmp(field[RAW_COL_PROJECTION], RAW_PROJECTION_NOMINAL) != 0)
            continue;
        assert(g_raw_count < RAW_ROWS_MAX);
        raw_row_t *r = &g_raw[g_raw_count++];
        snprintf(r->instruction, sizeof r->instruction, "%s", field[RAW_COL_INSTRUCTION]);
        snprintf(r->condition, sizeof r->condition, "%s", field[RAW_COL_CONDITION]);
        r->sum = atoi(field[RAW_COL_CALCULATE]) + atoi(field[RAW_COL_EXECUTE_BASE]) +
                 atoi(field[RAW_COL_FETCH]);
        r->checked = false;
    }
}

/* EA 形。ea は命令語の下位 6 ビット（A1 / D0 を基準レジスタにする）。 */
typedef struct {
    const char *condition;
    uint16_t ea;
    uint16_t ext[EA_EXT_MAX];
    unsigned ext_count;
    void (*prime)(mc68k_state_t *);
} ea_form_t;

static const ea_form_t g_ea_forms[] = {
    {"Dn", 0x01, {0}, 0, NULL},
    {"An", 0x09, {0}, 0, NULL},
    {"Dn/An", 0x01, {0}, 0, NULL},
    {"(An)", 0x11, {0}, 0, NULL},
    {"(An)+", 0x19, {0}, 0, NULL},
    {"-(An)", 0x21, {0}, 0, NULL},
    {"(d16,An)", 0x29, {0x0000}, 1, NULL},
    {"(d8,An,Xn)", 0x31, {0x0000}, 1, NULL},
    {"(xxx).W/(xxx).L", 0x38, {0x1800}, 1, NULL},
    {"#<xxx>", 0x3C, {0x0000, 0x0001}, 2, NULL},
    {"(d16,PC)", 0x3A, {0x0000}, 1, NULL},
    {"(d8,PC,Xn)", 0x3B, {0x0000}, 1, NULL},
    {"(BR,Xn)", 0x31, {0x0110}, 1, NULL},
    {"(bd,BR,Xn)", 0x31, {0x0130, 0x0000, 0x0100}, 3, NULL},
    {"([bd,BR,Xn])", 0x31, {0x0131, 0x0000, 0x0100}, 3, prime_indirect_pointer},
    {"([bd,BR,Xn],od)",
     0x31,
     {0x0133, 0x0000, 0x0100, 0x0000, 0x0100},
     5,
     prime_indirect_pointer},
    {"([bd,BR],Xn)", 0x31, {0x0135, 0x0000, 0x0100}, 3, prime_indirect_pointer},
    {"([bd,BR],Xn,od)",
     0x31,
     {0x0137, 0x0000, 0x0100, 0x0000, 0x0100},
     5,
     prime_indirect_pointer},
};
#define EA_FORM_COUNT (sizeof g_ea_forms / sizeof g_ea_forms[0])

/* EA 形ごとに行が分かれている命令族。命令語は opcode | ea、即値語は EA 拡張語の前に置く。 */
typedef struct {
    const char *instruction;
    uint16_t opcode;
    uint16_t imm[FAMILY_IMM_MAX];
    unsigned imm_count;
} family_t;

static const family_t g_families[] = {
    {"ADD/AND/EOR/OR/SUB/TST", 0xD480, {0}, 0},               /* ADD.L <ea>,D2 */
    {"ADDA", 0xD5C0, {0}, 0},                                 /* ADDA.L <ea>,A2 */
    {"ADDQ/SUBQ", 0x5280, {0}, 0},                            /* ADDQ.L #1,<ea> */
    {"ADDI/ANDI/EORI/ORI/SUBI", 0x0680, {0x0000, 0x0001}, 2}, /* ADDI.L #1,<ea> */
    {"CLR", 0x4280, {0}, 0},                                  /* CLR.L <ea> */
};
#define FAMILY_COUNT (sizeof g_families / sizeof g_families[0])

/* production が raw の印字和と食い違う行を固定する表。#95 で全件解消したため現在は空。
 * 食い違いを意図的に残す場合だけ実測値を登録する。登録後に production が変わったら
 * （食い違いが消えても別の値になっても）失敗する。 */
typedef struct {
    const char *instruction;
    const char *condition;
    int actual;
    const char *reason;
    bool covered;
} deviation_t;

static deviation_t g_deviation[] = {
    {NULL, NULL, 0, NULL, false},
};

static deviation_t *find_deviation(const char *instruction, const char *condition) {
    for (deviation_t *d = g_deviation; d->instruction; d++)
        if (strcmp(d->instruction, instruction) == 0 && strcmp(d->condition, condition) == 0)
            return d;
    return NULL;
}

static unsigned check_raw_families(mc68k_state_t *cpu, unsigned *bad) {
    unsigned checked = 0;
    for (size_t fi = 0; fi < FAMILY_COUNT; fi++) {
        const family_t *fam = &g_families[fi];
        for (size_t ei = 0; ei < EA_FORM_COUNT; ei++) {
            const ea_form_t *ea = &g_ea_forms[ei];
            bool found = false;
            for (unsigned ri = 0; ri < g_raw_count; ri++)
                found |= strcmp(g_raw[ri].instruction, fam->instruction) == 0 &&
                         strcmp(g_raw[ri].condition, ea->condition) == 0;
            if (!found)
                continue;
            assembly_t a = {fam->instruction, ea->condition, {0},  0, 0,
                            ea->prime,        false,         false};
            a.words[a.count++] = (uint16_t)(fam->opcode | ea->ea);
            for (unsigned i = 0; i < fam->imm_count; i++)
                a.words[a.count++] = fam->imm[i];
            for (unsigned i = 0; i < ea->ext_count; i++)
                a.words[a.count++] = ea->ext[i];
            assert(a.count <= MAX_WORDS);
            prepare(cpu, &a);
            int cycles = mc68k_execute(cpu, 1);
            deviation_t *dev = find_deviation(fam->instruction, ea->condition);
            if (dev)
                dev->covered = true;
            for (unsigned ri = 0; ri < g_raw_count; ri++) {
                raw_row_t *row = &g_raw[ri];
                if (strcmp(row->instruction, fam->instruction) != 0 ||
                    strcmp(row->condition, ea->condition) != 0)
                    continue;
                row->checked = true;
                checked++;
                int want = dev ? dev->actual : row->sum;
                if (dev && dev->actual == row->sum) {
                    fprintf(stderr, "deviation equals raw sum: %s | %s\n", fam->instruction,
                            ea->condition);
                    (*bad)++;
                }
                if (cycles != want) {
                    fprintf(stderr, "raw cycle mismatch: %s | %s raw-sum=%d actual=%d%s\n",
                            fam->instruction, ea->condition, row->sum, cycles,
                            dev ? " (pinned deviation changed)" : "");
                    (*bad)++;
                }
            }
        }
        for (unsigned i = 0; i < g_raw_count; i++)
            if (strcmp(g_raw[i].instruction, fam->instruction) == 0 && !g_raw[i].checked) {
                fprintf(stderr, "raw row reaches no EA form: %s | %s\n", g_raw[i].instruction,
                        g_raw[i].condition);
                (*bad)++;
            }
    }
    for (deviation_t *d = g_deviation; d->instruction; d++)
        if (!d->covered) {
            fprintf(stderr, "stale deviation entry: %s | %s\n", d->instruction, d->condition);
            (*bad)++;
        }
    return checked;
}

int main(int argc, char **argv) {
    if (argc != EXPECT_ARGC) {
        fprintf(stderr, "usage: %s <expectations.csv> <raw.csv>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, NULL};
    wr(0, 4, RESET_SP);
    wr(4, 4, CODE_PC);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68040);
    mc68k_state_t *cpu_ec_lc =
        mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68040, MC68K_FPU_NONE_040);
    assert(cpu && cpu_ec_lc);
    mc68k_state_t *models[] = {cpu, cpu_ec_lc};
    for (size_t i = 0; i < sizeof models / sizeof models[0]; i++) {
        mc68k_set_peek_hooks(models[i], pb, pw, pl, NULL);
        mc68k_reset(models[i]);
    }

    char line[LINE_LEN];
    char field[CSV_FIELDS_MAX][FIELD_LEN];
    bool header = true;
    unsigned rows = 0, checked = 0, excluded = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (header) {
            header = false;
            continue;
        }
        if (csv_split(line, field, CSV_FIELDS) != CSV_FIELDS) {
            fprintf(stderr, "bad CSV row: %s\n", line);
            return 2;
        }
        rows++;
        const char *instruction = field[0], *condition = field[1];
        int expected = atoi(field[3]);
        assert(expected > 0);
        exclusion_t *skip = find_exclusion(instruction, condition);
        assembly_t *a = find_assembly(instruction, condition);
        if (skip && a) {
            fprintf(stderr, "both assembled and excluded: %s | %s\n", instruction, condition);
            bad++;
            continue;
        }
        if (skip) {
            skip->covered = true;
            excluded++;
            continue;
        }
        if (!a) {
            fprintf(stderr, "no assembly for expectation row: %s | %s\n", instruction,
                    condition);
            bad++;
            continue;
        }
        a->covered = true;
        mc68k_state_t *target = a->ec_lc ? cpu_ec_lc : cpu;
        prepare(target, a);
        int cycles = mc68k_execute(target, 1);
        checked++;
        if (cycles != expected) {
            fprintf(stderr, "cycle mismatch: %s | %s expected=%d actual=%d\n", instruction,
                    condition, expected, cycles);
            bad++;
        }
    }
    fclose(f);

    for (size_t i = 0; i < ASSEMBLY_COUNT; i++)
        if (!g_assembly[i].covered) {
            fprintf(stderr, "stale assembly entry: %s | %s\n", g_assembly[i].instruction,
                    g_assembly[i].condition);
            bad++;
        }
    for (exclusion_t *e = g_exclusion; e->instruction; e++)
        if (!e->covered) {
            fprintf(stderr, "stale exclusion entry: %s | %s\n", e->instruction, e->condition);
            bad++;
        }
    assert(rows <= ROWS_MAX);

    FILE *raw_file = fopen(argv[2], "r");
    if (!raw_file) {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    load_raw(raw_file);
    fclose(raw_file);
    unsigned raw_checked = check_raw_families(cpu, &bad);
    printf("rows=%u checked=%u excluded=%u raw-checked=%u mismatches=%u\n", rows, checked,
           excluded, raw_checked, bad);
    mc68k_destroy(cpu);
    mc68k_destroy(cpu_ec_lc);
    return bad ? 1 : 0;
}
