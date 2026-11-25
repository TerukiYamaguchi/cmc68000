/*
 * test_68040_timing_coverage.c — 68040 の timing 決定出所の網羅検査
 *
 * 68040 で全 16 ビット opcode を、代表的な拡張語（brief、full IS=0/1 × I/IS 0-7）で
 * 1 命令ずつ実行する。timing が 040UM の表値から決まった実行は
 * cpu_040_tag() / cyc_get() が cyc040_tagged を進める。進まなかった実行は
 * 他機種の値へ落ちた fallback であり、その (mnemonic, 拡張語の形) が
 * 分類 CSV の out-of-scope 行に対応する許可表に載っていなければ失敗する。
 *
 * 逆向きも検査する。許可表の項目が 1 件も観測されなければ（表の陳腐化）失敗し、
 * 許可表が指す CSV 行は実在して status=out-of-scope でなければならない。
 *
 * 使い方: test_68040_timing_coverage <classification.csv> [dump]
 *   dump を付けると fallback の (mnemonic|scenario) を全件出力する。
 */
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE          0x4000u
#define RESET_SP          0x3000u
#define CODE_PC           0x0100u
#define HANDLER_PC        0x0900u
#define DATA_BASE         0x1000u
#define DATA_END          0x1C00u
#define AREG_BASE         0x1400u
#define AREG_STEP         0x20u
#define VECTOR_COUNT      256u
#define STREAM_WORDS      12u
#define MAX_FALLBACK_KEYS 4096u
#define KEY_LEN           64u
#define LINE_LEN          4096u

static uint8_t g_ram[RAM_SIZE];

static bool rd(uint32_t a, unsigned n, uint32_t *v) {
    *v = 0;
    for (unsigned i = 0; i < n; i++)
        *v = (*v << 8) | (a + i < RAM_SIZE ? g_ram[a + i] : 0u);
    return true;
}
static void wr(uint32_t a, unsigned n, uint32_t v) {
    for (unsigned i = 0; i < n; i++)
        if (a + i < RAM_SIZE)
            g_ram[a + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
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

/* 拡張語の形。brief 1 種と、full の (IS, I/IS) 16 種。 */
typedef struct {
    char name[16];
    uint16_t ext;
    bool full;
} scenario_t;

static scenario_t g_scen[17];
static unsigned g_scen_count;

static void build_scenarios(void) {
    snprintf(g_scen[0].name, sizeof g_scen[0].name, "brief");
    g_scen[0].ext = 0x0000; /* D0.W*1 + d8=0 */
    g_scen[0].full = false;
    g_scen_count = 1;
    for (unsigned is = 0; is < 2; is++)
        for (unsigned iis = 0; iis < 8; iis++) {
            scenario_t *s = &g_scen[g_scen_count++];
            snprintf(s->name, sizeof s->name, "full:IS%u:%u", is, iis);
            /* bit8=1 full、BS=0、IS、BD size=11(long)、I/IS */
            s->ext = (uint16_t)(0x0100u | (is << 6) | (3u << 4) | iis);
            s->full = true;
        }
}

static void prepare(mc68k_state_t *cpu, uint16_t opcode, const scenario_t *s) {
    memset(g_ram, 0, 0x0800);
    for (uint32_t a = DATA_BASE; a < DATA_END; a += 4)
        wr(a, 4, 0x00030003u);
    for (unsigned v = 0; v < VECTOR_COUNT; v++)
        wr(v * 4, 4, HANDLER_PC);
    wr(HANDLER_PC, 2, 0x60FE);
    wr(CODE_PC, 2, opcode);
    /* 拡張語列: [ext][bd.l=0x100][od.l=0x100] を繰り返す。brief/即値の語は
     * 先頭から同じ列を読むので、命令ごとの語数に合わせない代表値で足りる。 */
    uint32_t p = CODE_PC + 2;
    for (unsigned i = 0; i < STREAM_WORDS / 5 + 1; i++) {
        wr(p, 2, s->ext);
        p += 2;
        if (s->full) {
            wr(p, 4, 0x100);
            p += 4;
            wr(p, 4, 0x100);
            p += 4;
        }
    }
    for (int i = 0; i < 8; i++) {
        cpu->d[i] = 1;
        cpu->a[i] = AREG_BASE + (uint32_t)i * AREG_STEP;
    }
    cpu->d[0] = 0;
    cpu->a[7] = RESET_SP;
    cpu->usp = RESET_SP - 0x100;
    cpu->pc = CODE_PC;
    cpu->exec_state = CPU_NORMAL;
}

typedef struct {
    char key[MAX_FALLBACK_KEYS][KEY_LEN];
    uint16_t first_op[MAX_FALLBACK_KEYS];
    unsigned op_count[MAX_FALLBACK_KEYS];
    unsigned count;
} keyset_t;

static void keyset_add(keyset_t *k, const char *key, uint16_t op) {
    for (unsigned i = 0; i < k->count; i++)
        if (strcmp(k->key[i], key) == 0) {
            k->op_count[i]++;
            return;
        }
    k->first_op[k->count] = op;
    k->op_count[k->count] = 1;
    assert(k->count < MAX_FALLBACK_KEYS);
    snprintf(k->key[k->count++], KEY_LEN, "%s", key);
}

static void mnemonic_of(const char *text, char *out, size_t n) {
    size_t i = 0;
    while (text[i] && text[i] != ' ' && text[i] != '.' && i + 1 < n) {
        out[i] = text[i];
        i++;
    }
    out[i] = 0;
}

/* 許可表: fallback してよい (mnemonic, 拡張語の形) と、その根拠の分類 CSV 行。
 * mnemonic は逆アセンブラの語頭（'.' と空白の前）。'/' 区切りで複数、"*" は全て。
 * scenario は完全一致、末尾 '*' で接頭辞一致、"*" は全て。csv_instruction /
 * csv_condition は分類 CSV の instruction・condition 欄と完全一致する out-of-scope 行。
 * 1 つの観測キーが複数項目に当たってよい（MOVE の IS=1 直接形と source An など）。 */
typedef struct {
    const char *mnemonic;
    const char *scenario;
    const char *csv_instruction;
    const char *csv_condition;
} allow_t;

#define IS1_DIRECT "IS=1 direct full-extension"
#define LOCKED     "locked-RMW"

static const allow_t g_allow[] = {
    {"ADD/SUB", "full:IS1:0", "ADD/SUB", IS1_DIRECT},
    {"ADDA/SUBA", "full:IS1:0", "ADDA/SUBA", IS1_DIRECT},
    {"AND/OR/TST", "full:IS1:0", "AND/OR/TST", IS1_DIRECT},
    {"ADDQ/SUBQ", "full:IS1:0", "ADDQ/SUBQ", IS1_DIRECT},
    {"CLR", "full:IS1:0", "CLR", IS1_DIRECT},
    {"CMP/EOR/CHK", "full:IS1:0", "CMP/EOR/CHK", IS1_DIRECT},
    {"NEG/NEGX/NOT/NBCD/ST/SF/SHI/SLS/SCC/SCS/SNE/SEQ/SVC/SVS/SPL/SMI/SGE/SLT/SGT/SLE",
     "full:IS1:0", "NEG/NEGX/NOT/NBCD/Scc", IS1_DIRECT},
    {"ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR", "full:IS1:0",
     "ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR memory", IS1_DIRECT},
    {"BTST/BCHG/BSET", "full:IS1:0", "BTST/BCHG/BSET", IS1_DIRECT},
    {"JMP/JSR/LEA/PEA", "full:IS1:0", "JMP/JSR/LEA/PEA", IS1_DIRECT},
    {"MOVE", "full:IS1:0", "MOVE", "destination An / IS=1 direct full extension"},
    {"MOVEA", "*", "MOVE", "destination An / IS=1 direct full extension"},
    {"MOVE", "*", "MOVE", "source An"},
    {"CMPA", "*", "CMPA.W", "all EA"},
    {"BTST", "*", "BTST", "Dn bit number, #<xxx> operand"},
    {"STOP", "*", "STOP", "always"},
    {"CAS", "*", "CAS", LOCKED},
    {"CAS2", "*", "CAS2", LOCKED},
    {"TAS", "*", "TAS", LOCKED},
    /* 逆アセンブラは PTEST（0xF548-0xF56F）を LINE-F と表示する */
    {"LINE-F", "*", "PTESTR/PTESTW", "three-level-search"},
    {NULL, NULL, NULL, NULL},
};
#define ALLOW_MAX 64u

/* 分類 CSV の out-of-scope 行のうち、1 命令の公称実行では到達しないもの
 * （例外経路は pc がハンドラへ移るので観測キーに現れない）。 */
static const allow_t g_unreachable[] = {
    {NULL, NULL, "CHK", "exception/interlocked"},
    {NULL, NULL, NULL, NULL},
};

static bool word_in_list(const char *list, const char *word) {
    size_t n = strlen(word);
    for (const char *p = list; *p;) {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && strncmp(p, word, n) == 0)
            return true;
        if (!end)
            break;
        p = end + 1;
    }
    return false;
}

static bool scenario_match(const char *pat, const char *s) {
    size_t n = strlen(pat);
    if (strcmp(pat, "*") == 0)
        return true;
    if (n && pat[n - 1] == '*')
        return strncmp(pat, s, n - 1) == 0;
    return strcmp(pat, s) == 0;
}

static bool allow_match(const allow_t *a, const char *mn, const char *scen) {
    return (strcmp(a->mnemonic, "*") == 0 || word_in_list(a->mnemonic, mn)) &&
           scenario_match(a->scenario, scen);
}

/* CSV 1 行を欄へ分ける（二重引用符で囲んだ欄の中のカンマと "" に対応）。 */
#define CSV_FIELDS 9u
static unsigned csv_split(char *line, char *field[CSV_FIELDS]) {
    unsigned n = 0;
    char *p = line;
    while (n < CSV_FIELDS) {
        char *out = p;
        field[n++] = out;
        if (*p == '"') {
            p++;
            for (;;) {
                if (*p == '"' && p[1] == '"') {
                    *out++ = '"';
                    p += 2;
                } else if (*p == '"' || *p == 0) {
                    break;
                } else {
                    *out++ = *p++;
                }
            }
            if (*p == '"')
                p++;
        } else {
            while (*p && *p != ',')
                *out++ = *p++;
        }
        bool more = *p == ',';
        *out = 0;
        if (!more)
            break;
        p++;
    }
    return n;
}

typedef struct {
    char instruction[128];
    char condition[128];
    bool out_of_scope;
    bool referenced;
} csv_row_t;

#define CSV_ROWS_MAX 512u
static csv_row_t g_rows[CSV_ROWS_MAX];
static unsigned g_row_count;

static void load_classification(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    char line[LINE_LEN];
    bool header = true;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (header) {
            header = false;
            continue;
        }
        char *field[CSV_FIELDS];
        if (csv_split(line, field) != CSV_FIELDS) {
            fprintf(stderr, "bad CSV row: %s\n", line);
            exit(2);
        }
        assert(g_row_count < CSV_ROWS_MAX);
        csv_row_t *r = &g_rows[g_row_count++];
        snprintf(r->instruction, sizeof r->instruction, "%s", field[0]);
        snprintf(r->condition, sizeof r->condition, "%s", field[1]);
        r->out_of_scope = strcmp(field[7], "out-of-scope") == 0;
    }
    fclose(f);
}

static csv_row_t *find_row(const char *instruction, const char *condition) {
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].instruction, instruction) == 0 &&
            strcmp(g_rows[i].condition, condition) == 0)
            return &g_rows[i];
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    bool dump = argc >= 3 && strcmp(argv[2], "dump") == 0;
    bool dump_ops = argc >= 3 && strcmp(argv[2], "dumpops") == 0;
    build_scenarios();

    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, NULL};
    wr(0, 4, RESET_SP);
    wr(4, 4, CODE_PC);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68040);
    assert(cpu);
    mc68k_set_peek_hooks(cpu, pb, pw, pl, NULL);
    mc68k_reset(cpu);

    if (argc >= 4 && !dump_ops) { /* 単一 opcode の調査: 結果の pc / 周期 / 例外 / tag を出す */
        uint16_t op = (uint16_t)strtoul(argv[3], NULL, 16);
        for (unsigned si = 0; si < g_scen_count; si++) {
            prepare(cpu, op, &g_scen[si]);
            cpu->sr = 0x2700;
            uint32_t before = cpu->cyc040_tagged;
            int cyc = mc68k_execute(cpu, 1);
            printf("%s pc=%04x cycles=%d vec=%d tagged=%u\n", g_scen[si].name, cpu->pc, cyc,
                   cpu->last_exception_vector, cpu->cyc040_tagged - before);
        }
        return 0;
    }
    keyset_t *fb = calloc(1, sizeof *fb);
    assert(fb);
    unsigned long runs = 0, tagged_runs = 0, fault_runs = 0;
    for (uint32_t op = 0; op <= 0xFFFF; op++) {
        if (op >= 0xF200 && op <= 0xF3FF)
            continue; /* 内蔵 FPU（pipeline timing は #89 の範囲外） */
        for (unsigned si = 0; si < g_scen_count; si++) {
            prepare(cpu, (uint16_t)op, &g_scen[si]);
            cpu->sr = 0x2700;
            uint32_t before = cpu->cyc040_tagged;
            (void)mc68k_execute(cpu, 1);
            runs++;
            if (cpu->pc == HANDLER_PC) {
                fault_runs++; /* 例外経路は (別途) 例外の分類で扱う */
                continue;
            }
            if (cpu->cyc040_tagged != before) {
                tagged_runs++;
                continue;
            }
            prepare(cpu, (uint16_t)op, &g_scen[si]);
            char text[128], mn[32], key[KEY_LEN];
            (void)mc68k_disassemble(cpu, CODE_PC, text, sizeof text);
            mnemonic_of(text, mn, sizeof mn);
            snprintf(key, sizeof key, "%s|%s", mn, g_scen[si].name);
            keyset_add(fb, key, (uint16_t)op);
            if (dump_ops)
                printf("%s %04x\n", key, op);
        }
    }
    fprintf(stderr, "runs=%lu tagged=%lu exception=%lu fallback-keys=%u\n", runs, tagged_runs,
            fault_runs, fb->count);
    unsigned fb_count = fb->count;
    if (dump_ops)
        return 0;
    if (dump) {
        for (unsigned i = 0; i < fb->count; i++)
            printf("%s %04x %u\n", fb->key[i], fb->first_op[i], fb->op_count[i]);
        return 0;
    }

    load_classification(argv[1]);

    /* 許可表の各項目は out-of-scope の CSV 行を指す。 */
    unsigned allow_count = 0;
    bool seen[ALLOW_MAX] = {false};
    unsigned bad = 0;
    for (const allow_t *a = g_allow; a->mnemonic; a++, allow_count++) {
        csv_row_t *row = find_row(a->csv_instruction, a->csv_condition);
        if (!row || !row->out_of_scope) {
            fprintf(stderr, "allow entry points at no out-of-scope CSV row: %s / %s\n",
                    a->csv_instruction, a->csv_condition);
            bad++;
        } else {
            row->referenced = true;
        }
    }
    assert(allow_count < ALLOW_MAX);

    /* 観測した fallback は全て許可表の項目に当たること。 */
    for (unsigned i = 0; i < fb->count; i++) {
        char mn[KEY_LEN], *bar;
        snprintf(mn, sizeof mn, "%s", fb->key[i]);
        bar = strchr(mn, '|');
        *bar = 0;
        bool matched = false;
        for (unsigned k = 0; k < allow_count; k++)
            if (allow_match(&g_allow[k], mn, bar + 1)) {
                seen[k] = true;
                matched = true;
            }
        if (!matched) {
            fprintf(stderr, "unclassified 040 fallback: %s (first opcode %04x)\n", fb->key[i],
                    fb->first_op[i]);
            bad++;
        }
    }

    /* 許可表の項目が 1 件も観測されなければ表が陳腐化している。 */
    for (unsigned k = 0; k < allow_count; k++)
        if (!seen[k]) {
            fprintf(stderr, "stale allow entry (never observed): %s | %s\n",
                    g_allow[k].mnemonic, g_allow[k].csv_condition);
            bad++;
        }

    /* CSV の out-of-scope 行は、許可表が指すか到達不能表に載っていること。 */
    for (unsigned i = 0; i < g_row_count; i++) {
        csv_row_t *r = &g_rows[i];
        if (!r->out_of_scope || r->referenced)
            continue;
        bool exempt = false;
        for (const allow_t *u = g_unreachable; u->csv_instruction; u++)
            exempt |= strcmp(u->csv_instruction, r->instruction) == 0 &&
                      strcmp(u->csv_condition, r->condition) == 0;
        if (!exempt) {
            fprintf(stderr, "out-of-scope CSV row never observed: %s / %s\n", r->instruction,
                    r->condition);
            bad++;
        }
    }

    free(fb);
    mc68k_destroy(cpu);
    if (!bad)
        printf("ok: %lu runs, %lu tagged, %u fallback keys classified\n", runs, tagged_runs,
               fb_count);
    return bad ? 1 : 0;
}
