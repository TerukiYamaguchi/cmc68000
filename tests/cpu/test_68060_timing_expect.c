/*
 * test_68060_timing_expect.c — 68060 の期待サイクル CSV を実行結果と突き合わせる (#102)
 *
 * tests/data/cpu/mc68060_cycle_expectations.csv の全行について、code 列の命令列を 68060 で実行し、
 * 最後の命令が消費したサイクル数が expected_cycles と一致することを検査する。期待値の出典は
 * raw CSV（060UM §10 の印字値）で、expected_cycles と raw の突き合わせは
 * tools/check_68060_cycle_data.py が行う。このファイルに数値は書かない。
 *
 * setup 列は空白区切りの token: sr=<hex> d0=<dec> ebc psr=<hex> pd0=<dec> patch=<addr>:<hex>
 *   fill=<hex byte> w16=<addr>:<hex> irq=<level> iack=<vector> pc=<hex>
 *   dirty=<n>[:<first>] は D-cache へ dirty line を n 本置く（line k は OPERAND + 16*k、first から開始）
 *   ebc は CACR.EBC を立てる。psr があれば、測定の前に同じ命令を SR=psr で 1 回実行して
 *   branch cache に登録する（pd0 はそのときの D0）。patch は prime の後に命令語を書き換える。
 * code 列は命令を '/' で区切り、最後の 1 命令だけを測る（手前の命令は準備として実行する）。
 *
 * 使い方: test_68060_timing_expect <expectations.csv>
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE       0x80000u
#define SSP_INIT       0x8000u
#define RESET_PC       0x100u
#define TEST_PC        0x400u
#define OPERAND        0x6000u
#define OPERAND_FILL   0x400u
#define HANDLER        0x3000u
#define HANDLER_STRIDE 8u
#define RTE_OPCODE     0x4e73
#define SR_SUP         0x2700u
#define CACR_EBC       0x00800000u
#define D1_INIT        100u
#define D0_DEFAULT     3u
#define VECTOR_FIRST   2
#define VECTOR_COUNT   256
#define AREG_COUNT     7
#define DREG_COUNT     8
#define FIELDS         12u
#define LINE_LEN       2048u
#define FIELD_LEN      1024u
#define WORDS_MAX      64u
#define INSTR_MAX      8u
#define ROWS_MIN       1000u
#define COL_ID         0u
#define COL_CASE       5u
#define COL_SETUP      6u
#define COL_CODE       7u
#define COL_EXPECTED   9u

static uint8_t ram[RAM_SIZE];
static mc68k_state_t *cpu;
static uint8_t iack_vector;

static bool rb(void *c, uint32_t a, uint8_t *v) {
    (void)c;
    if (a >= RAM_SIZE)
        return false;
    *v = ram[a];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    (void)c;
    if (a + 2 > RAM_SIZE)
        return false;
    *v = (uint16_t)(ram[a] << 8 | ram[a + 1]);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    (void)c;
    if (a + 4 > RAM_SIZE)
        return false;
    *v = (uint32_t)ram[a] << 24 | (uint32_t)ram[a + 1] << 16 | (uint32_t)ram[a + 2] << 8 |
         ram[a + 3];
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    (void)c;
    if (a >= RAM_SIZE)
        return false;
    ram[a] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    (void)c;
    if (a + 2 > RAM_SIZE)
        return false;
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    (void)c;
    if (a + 4 > RAM_SIZE)
        return false;
    ram[a] = (uint8_t)(v >> 24);
    ram[a + 1] = (uint8_t)(v >> 16);
    ram[a + 2] = (uint8_t)(v >> 8);
    ram[a + 3] = (uint8_t)v;
    return true;
}
static uint8_t iack(void *c, int level) {
    (void)c;
    (void)level;
    return iack_vector;
}
static void reset_peripheral(void *c) {
    (void)c;
}

static void put_word(uint32_t a, uint16_t v) {
    if (!ww(NULL, a, v))
        abort();
}
static void put_long(uint32_t a, uint32_t v) {
    if (!wl(NULL, a, v))
        abort();
}

static void init_state(void) {
    memset(ram, 0, sizeof ram);
    put_long(0, SSP_INIT);
    put_long(4, RESET_PC);
    for (int v = VECTOR_FIRST; v < VECTOR_COUNT; ++v) {
        put_long((uint32_t)v * 4u, HANDLER + (uint32_t)v * HANDLER_STRIDE);
        put_word(HANDLER + (uint32_t)v * HANDLER_STRIDE, RTE_OPCODE);
    }
    mc68k_reset(cpu);
    cpu->usp = 0x7000u;
    mc68k_set_irq(cpu, 0);
    cpu->irq_defer = false;
    cpu->pcr = 0;
}

static void set_registers(uint32_t d0) {
    for (int i = 0; i < DREG_COUNT; ++i)
        cpu->d[i] = 0;
    for (int i = 0; i < AREG_COUNT; ++i)
        cpu->a[i] = OPERAND;
    cpu->d[0] = d0;
    cpu->d[1] = D1_INIT;
    cpu->a[7] = SSP_INIT;
}

/* 1 行を ',' で分ける。'"' で囲んだ欄は ',' を含められ、'""' は '"' を表す。 */
static unsigned csv_split(char *line, char field[FIELDS][FIELD_LEN]) {
    unsigned n = 0, len = 0;
    bool quoted = false;
    for (char *p = line; *p && *p != '\n' && *p != '\r'; ++p) {
        if (quoted) {
            if (*p == '"' && p[1] == '"') {
                field[n][len++] = '"';
                ++p;
            } else if (*p == '"') {
                quoted = false;
            } else {
                field[n][len++] = *p;
            }
        } else if (*p == '"') {
            quoted = true;
        } else if (*p == ',') {
            field[n][len] = '\0';
            if (++n >= FIELDS)
                return n;
            len = 0;
        } else {
            field[n][len++] = *p;
        }
        assert(len < FIELD_LEN - 1);
    }
    field[n][len] = '\0';
    return n + 1;
}

typedef struct {
    uint16_t sr;
    uint32_t d0;
    bool ebc;
    bool prime;
    uint16_t psr;
    uint32_t pd0;
    bool patch;
    uint32_t patch_addr;
    uint16_t patch_word;
    bool fill;
    uint8_t fill_byte;
    bool w16;
    uint32_t w16_addr;
    uint16_t w16_word;
    unsigned irq;
    unsigned iack;
    uint32_t pc;
    unsigned dirty_count;
    unsigned dirty_first;
} setup_t;

static bool parse_setup(const char *text, setup_t *s) {
    char buf[FIELD_LEN];
    snprintf(buf, sizeof buf, "%s", text);
    *s = (setup_t){.sr = SR_SUP, .d0 = D0_DEFAULT, .pd0 = D0_DEFAULT, .pc = TEST_PC};
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        unsigned a, v;
        if (sscanf(tok, "sr=%x", &v) == 1) {
            s->sr = (uint16_t)v;
        } else if (sscanf(tok, "d0=%u", &v) == 1) {
            s->d0 = v;
        } else if (strcmp(tok, "ebc") == 0) {
            s->ebc = true;
        } else if (sscanf(tok, "psr=%x", &v) == 1) {
            s->prime = true;
            s->psr = (uint16_t)v;
        } else if (sscanf(tok, "pd0=%u", &v) == 1) {
            s->pd0 = v;
        } else if (sscanf(tok, "patch=%x:%x", &a, &v) == 2) {
            s->patch = true;
            s->patch_addr = a;
            s->patch_word = (uint16_t)v;
        } else if (sscanf(tok, "fill=%x", &v) == 1) {
            s->fill = true;
            s->fill_byte = (uint8_t)v;
        } else if (sscanf(tok, "w16=%x:%x", &a, &v) == 2) {
            s->w16 = true;
            s->w16_addr = a;
            s->w16_word = (uint16_t)v;
        } else if (sscanf(tok, "dirty=%u:%u", &v, &a) == 2) {
            s->dirty_count = v;
            s->dirty_first = a;
        } else if (sscanf(tok, "dirty=%u", &v) == 1) {
            s->dirty_count = v;
        } else if (sscanf(tok, "irq=%u", &v) == 1) {
            s->irq = v;
        } else if (sscanf(tok, "iack=%u", &v) == 1) {
            s->iack = v;
        } else if (sscanf(tok, "pc=%x", &v) == 1) {
            s->pc = v;
        } else {
            return false;
        }
    }
    return true;
}

/* code 列を RAM に置く。各命令の先頭アドレスを start[] に返し、命令数を返す。 */
static unsigned place_code(const char *text, uint32_t pc, uint32_t start[INSTR_MAX]) {
    unsigned count = 0;
    uint32_t addr = pc;
    const char *p = text;
    start[0] = addr;
    while (*p) {
        if (*p == '/') {
            if (++count >= INSTR_MAX)
                abort();
            start[count] = addr;
            ++p;
        } else if (*p == ' ') {
            ++p;
        } else {
            char *end;
            unsigned long w = strtoul(p, &end, 16);
            assert(end != p);
            put_word(addr, (uint16_t)w);
            addr += 2;
            p = end;
        }
    }
    return count + 1;
}

/* D-cache の line k（k=0..511）を OPERAND + 16*k の dirty line にする。set は物理アドレス
 * bit10–4、way は k / set 数。line の内容は RAM（0 埋め）と同じなので push しても RAM は変わらない。 */
static void place_dirty_lines(unsigned first, unsigned count) {
    for (unsigned k = first; k < first + count; ++k) {
        assert(k < MC68K_060_CACHE_SETS * MC68K_040_CACHE_WAYS);
        mc68k_040_cache_line_t *line =
            &cpu->dcache040[k % MC68K_060_CACHE_SETS][k / MC68K_060_CACHE_SETS];
        memset(line, 0, sizeof *line);
        line->physical_line = OPERAND + k * 16u;
        line->valid = true;
        line->loaded = true;
        line->dirty = 0xf;
    }
}

static int run_row(char field[FIELDS][FIELD_LEN], bool *bad_row) {
    setup_t s;
    uint32_t start[INSTR_MAX];
    if (!parse_setup(field[COL_SETUP], &s)) {
        *bad_row = true;
        return 0;
    }
    init_state();
    if (s.fill)
        memset(ram + OPERAND, s.fill_byte, OPERAND_FILL);
    if (s.w16)
        put_word(s.w16_addr, s.w16_word);
    const unsigned count = place_code(field[COL_CODE], s.pc, start);
    set_registers(s.d0);
    if (s.ebc)
        cpu->cacr |= CACR_EBC;
    place_dirty_lines(s.dirty_first, s.dirty_count);
    for (unsigned i = 0; i + 1 < count; ++i) {
        cpu_set_sr(cpu, SR_SUP);
        cpu->pc = start[i];
        (void)mc68k_execute(cpu, 1);
    }
    const uint32_t last = start[count - 1];
    if (s.prime) {
        cpu->d[0] = s.pd0;
        cpu_set_sr(cpu, s.psr);
        cpu->pc = last;
        (void)mc68k_execute(cpu, 1);
        cpu->d[0] = s.d0;
    }
    if (s.patch)
        put_word(s.patch_addr, s.patch_word);
    iack_vector = (uint8_t)s.iack;
    if (s.irq)
        mc68k_set_irq(cpu, (uint8_t)s.irq);
    cpu_set_sr(cpu, s.sr);
    cpu->pc = last;
    return mc68k_execute(cpu, 1);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <expectations.csv>\n", argv[0]);
        return 2;
    }
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .iack = iack,
                       .reset_peripheral = reset_peripheral};
    cpu = mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68060, MC68K_FPU_68060);
    assert(cpu);

    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        perror(argv[1]);
        return 2;
    }
    static char line[LINE_LEN];
    static char field[FIELDS][FIELD_LEN];
    unsigned rows = 0, failures = 0;
    bool header = true;
    while (fgets(line, sizeof line, fp)) {
        if (csv_split(line, field) != FIELDS) {
            fprintf(stderr, "%s: malformed row: %.60s\n", argv[1], line);
            return 2;
        }
        if (header) {
            header = false;
            continue;
        }
        bool bad_row = false;
        const int got = run_row(field, &bad_row);
        const int want = atoi(field[COL_EXPECTED]);
        ++rows;
        if (bad_row) {
            fprintf(stderr, "FAIL %s: unknown setup token in '%s'\n", field[COL_ID],
                    field[COL_SETUP]);
            ++failures;
        } else if (got != want) {
            fprintf(stderr, "FAIL %s (%s): expected %d got %d\n", field[COL_ID],
                    field[COL_CASE], want, got);
            ++failures;
        }
    }
    fclose(fp);
    if (rows < ROWS_MIN) {
        fprintf(stderr, "%s: only %u rows (expected at least %u)\n", argv[1], rows, ROWS_MIN);
        return 1;
    }
    if (failures) {
        fprintf(stderr, "test_68060_timing_expect: %u of %u rows failed\n", failures, rows);
        return 1;
    }
    printf("test_68060_timing_expect: OK (%u rows)\n", rows);
    return 0;
}
