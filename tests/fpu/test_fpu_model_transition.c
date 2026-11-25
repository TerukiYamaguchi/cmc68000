/*
 * test_fpu_model_transition.c — 設計書 §4.2 の表 A の全セル×機種を駆動する（ Phase
 * B）。
 *
 * 期待値は tests/data/fpu/fpu2_take_transition.csv（scripts/gen_fpu2_take_transition.py が表 A
 * から写した 手書きの期待値。production のコードから取らない）。各行は「setup で状態へ到達 →
 * 事象を1つ与える → 戻り値、次の状態、probe を検査」で、状態は API
 * だけで到達する（内部欄を直接書かない）。 状態番号の確認だけは内部の fpu_state_id()
 * を使う（行見出しの (phase, arith, UNSTARTED, violation, context_valid)
 * の定義そのもの。各状態の応答は response 事象の行が別に検査する）。
 *
 * 落とす変異: CSV の1行の期待値の反転、行ループの縮小（件数 assert）、各セルの遷移の取り違え。
 */
#include <ctype.h>
#include "fpu/fpu_model.h"
#include "fpu_model_csv.h"
#include "fpu_model_setup.h"

#define MAX_ROWS 1024
#define EXPECT_INFERENCE_ROWS \
    394u /* 表の【推】を含むセルの行数（生成スクリプトが表から決める） */
#define EXPECT_ROWS  688u
#define EXPECT_CELLS 333u
#define EXPECT_THEN_ROWS                                                                         \
    26u /* then キーを持つ行数（CSV の実測。消えると TAKE などの後続応答が黙って未検査になる） \
         */

typedef struct {
    char dev[8];
    int state;
    char setup[8];
    char event[16];
    char variant[8];
    char ret;
    int next;
    char probe[PROBE_BUF];
    int inference;
    int line;
} row_t;

static row_t rows[MAX_ROWS];
static unsigned nrows;
static unsigned then_rows; /* then キーを持つ行数（load_rows が数える） */

#define ROWFAIL(r, msg)                                                                   \
    do {                                                                                  \
        fprintf(stderr, "line %d: %s/%d/%s/%s/%s: %s\n", (r)->line, (r)->dev, (r)->state, \
                (r)->setup, (r)->event, (r)->variant, msg);                               \
        exit(1);                                                                          \
    } while (0)

static void check_response_probe(const row_t *r, mc68k_fpu_response_t resp) {
    const char *why = resp_mismatch(r->probe, resp);

    if (why)
        ROWFAIL(r, why);
}

static size_t expected_bytes(mc68k_fpu_config_t cfg, const char *fmt) {
    int e82 = is82(cfg);

    if (!strcmp(fmt, "idle"))
        return e82 ? 56 : 24;
    if (!strcmp(fmt, "busy"))
        return e82 ? 212 : 180;
    return 0;
}

static uint16_t expected_format_word(mc68k_fpu_config_t cfg, const char *fmt) {
    int e82 = is82(cfg);

    if (!strcmp(fmt, "idle"))
        return e82 ? 0x1F38 : 0x1F18;
    if (!strcmp(fmt, "busy"))
        return e82 ? 0x1FD4 : 0x1FB4;
    if (!strcmp(fmt, "illegal"))
        return 0x0200;
    return 0x0000;
}

/* fsave 行の probe のキーを fmt ごとに固定する（許可するキーは load_rows の
 * probe_check_keys）。 idle・busy は BIU
 * の4キー（b31・p・b27・b26）が必須で、生成側が落とすと黙って未検査になる。img は idle
 * が必須、busy は不可（busy の body[0] は image でなく、書いても死んだ欄）。null・illegal は
 * body が無いので BIU のキーも img も不可（fmt と then だけが残る） */
static void check_fsave_keys(const row_t *r, const char *fmts) {
    static const char *const biu_keys[] = {"b31", "p", "b27", "b26"};
    int biu = !strcmp(fmts, "idle") || !strcmp(fmts, "busy");
    char tmp[16];
    size_t i;

    CHECK_DISTINCT_STR(biu_keys);
    for (i = 0; i < sizeof(biu_keys) / sizeof(biu_keys[0]); i++)
        if (probe_get(r->probe, biu_keys[i], tmp, sizeof(tmp)) != biu)
            ROWFAIL(r, biu ? "fsave の idle・busy 行に BIU のキーが無い"
                           : "fsave の null・illegal 行に BIU のキーがある");
    CHECK_EQ(i, 4u); /* BIU のキーは b31・p・b27・b26 の4つ。表を縮めても通らないようにする */
    if (probe_get(r->probe, "img", tmp, sizeof(tmp)) != !strcmp(fmts, "idle"))
        ROWFAIL(r, "img は fsave の idle 行だけが持つ");
}

static void check_fsave(const row_t *r, mc68k_fpu_t *f, mc68k_fpu_config_t cfg) {
    char fmts[16], pbits[16];
    uint16_t fmt;
    size_t bytes, i;
    uint32_t body[64];
    int v;

    if (!probe_get(r->probe, "fmt", fmts, sizeof(fmts)))
        ROWFAIL(r, "fsave probe に fmt が無い");
    csv_check_one_of("fsave の fmt", fmts, "idle,busy,illegal,null");
    check_fsave_keys(r, fmts);
    mc68k_fpu_begin_save(f, &fmt, &bytes);
    if (fmt != expected_format_word(cfg, fmts))
        ROWFAIL(r, "format word");
    if (bytes != expected_bytes(cfg, fmts))
        ROWFAIL(r, "body bytes");
    for (i = 0; i < bytes / 4; i++)
        CHECK(mc68k_fpu_read_operand(f, &body[i]));
    if (!strcmp(fmts, "idle") || !strcmp(fmts, "busy")) {
        size_t biu_idx = !strcmp(fmts, "busy") ? BUSY_BIU_INDEX : idle_biu_index(cfg);
        uint32_t biu = body[biu_idx];

        if (probe_int(r->probe, "b31", &v) && ((v != 0 && v != 1) || (int)(biu >> 31) != v))
            ROWFAIL(r, "BIU bit31");
        if (probe_get(r->probe, "p", pbits, sizeof(pbits))) {
            int p;

            if (strlen(pbits) != 3 || strspn(pbits, "01") != 3)
                ROWFAIL(r, "BIU 30-28 の値は 0/1 の3桁");
            p = (pbits[0] - '0') * 4 + (pbits[1] - '0') * 2 + (pbits[2] - '0');
            if ((int)((biu >> 28) & 7) != p)
                ROWFAIL(r, "BIU 30-28");
        }
        if (probe_int(r->probe, "b27", &v) &&
            ((v != 0 && v != 1) || (int)((biu >> 27) & 1) != v))
            ROWFAIL(r, "BIU bit27");
        if (probe_int(r->probe, "b26", &v) &&
            ((v != 0 && v != 1) || (int)((biu >> 26) & 1) != v))
            ROWFAIL(r, "BIU bit26");
        if ((biu & 0xFFFF) != 0xFFFF)
            ROWFAIL(r, "BIU 15-0 は全1");
        if (probe_int(r->probe, "img", &v)) {
            /* command/condition image は +$04 の上位 word（Figure 6-4・6-5）。下位 word は予約
             */
            if (body[0] != (uint32_t)v << 16)
                ROWFAIL(r, "command/condition image");
        }
    }
}

static void run_row(const row_t *r) {
    mc68k_fpu_config_t cfg = !strcmp(r->dev, "68881") ? MC68K_FPU_68881 : MC68K_FPU_68882;
    mc68k_fpu_t *f = setup(r->setup, cfg);
    uint32_t iar_before = regs(f).fpiar;
    int ret = -1;
    char then[96];
    mc68k_fpu_response_t resp;
    int observed;

    if (fpu_state_id(f) != r->state)
        ROWFAIL(r, "setup が開始状態に到達していない");
    if (!strcmp(r->event, "ack")) {
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
    } else if (!strcmp(r->event, "abort")) {
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    } else if (!strcmp(r->event, "reset")) {
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_RESET);
    } else if (!strcmp(r->event, "fsave")) {
        check_fsave(r, f, cfg);
    } else if (!strcmp(r->event, "frestore_bad")) {
        uint16_t rb;
        size_t bytes;

        mc68k_fpu_begin_restore(f, 0x0123, &rb, &bytes);
        if ((rb & 0xFF00) != 0x0200 || bytes != 0)
            ROWFAIL(r, "不正 format の read-back は $02xx、body 0");
    } else if (!strcmp(r->event, "command")) {
        uint16_t w =
            !strcmp(r->variant, "arith") ? CMD(0, 1, 0, OPM_FADD)
            : !strcmp(r->variant, "movem")
                ? 0xF080
                : 0x2000; /* invalid（class 001）。ロード時に event ごとの variant を検査済み */
        ret = mc68k_fpu_write_command_word(f, w);
    } else if (!strcmp(r->event, "condition")) {
        ret = mc68k_fpu_write_condition(f, 0x01);
    } else if (!strcmp(r->event, "response")) {
        resp = rd(f);
        check_response_probe(r, resp);
    } else if (!strcmp(r->event, "ia")) {
        ret = mc68k_fpu_write_instruction_address(f, IA_VALUE);
    } else {
        ROWFAIL(r, "未知の事象");
    }
    if (ret >= 0 && ret != (r->ret == 'T'))
        ROWFAIL(r, "戻り値");
    if (!strcmp(r->event, "ia")) {
        uint32_t after = regs(f).fpiar;

        if (ret && after != IA_VALUE)
            ROWFAIL(r, "合法な IA 書込みで FPIAR が更新されない");
        if (!ret && after != iar_before)
            ROWFAIL(r, "violation の IA 書込みで FPIAR が更新された");
    }
    observed = fpu_state_id(f);
    if (probe_then(r->probe, then, sizeof(then))) {
        row_t after = *r;

        csv_copy(after.probe, sizeof(after.probe), then);
        resp = rd(f);
        check_response_probe(&after, resp);
    }
    if (observed != r->next) {
        fprintf(stderr, "line %d: %s/%d/%s/%s/%s: 次の状態 期待 S%d 実際 S%d\n", r->line,
                r->dev, r->state, r->setup, r->event, r->variant, r->next, observed);
        exit(1);
    }
    mc68k_fpu_destroy(f);
}

static void load_rows(const char *path) {
    FILE *fp = fopen(path, "r");
    char line[512];
    int lineno = 0;

    CHECK(fp != NULL);
    CHECK(fgets(line, sizeof(line), fp) != NULL); /* ヘッダ */
    lineno++;
    while (fgets(line, sizeof(line), fp)) {
        char *tok[9];
        row_t *r;

        lineno++;
        split_csv(line, tok, 9);
        CHECK(nrows < MAX_ROWS);
        r = &rows[nrows++];
        r->line = lineno;
        csv_copy(r->dev, sizeof(r->dev), tok[0]);
        r->state = csv_int("state", tok[1]);
        csv_copy(r->setup, sizeof(r->setup), tok[2]);
        csv_copy(r->event, sizeof(r->event), tok[3]);
        csv_copy(r->variant, sizeof(r->variant), tok[4]);
        r->ret = tok[5][0];
        csv_check_one_of("device", r->dev, "68881,68882");
        csv_check_one_of("event", r->event,
                         "ack,abort,reset,fsave,frestore_bad,command,condition,response,ia");
        /* variant は command だけが持つ。ret は command・condition・ia だけが持つ（他は "-"）
         */
        csv_check_one_of("variant", r->variant,
                         !strcmp(r->event, "command") ? "arith,invalid,movem" : "-");
        csv_check_one_of("ret", tok[5],
                         !strcmp(r->event, "command") || !strcmp(r->event, "condition") ||
                                 !strcmp(r->event, "ia")
                             ? "T,F"
                             : "-");
        r->next = csv_int("next", tok[6]);
        csv_copy(r->probe, sizeof(r->probe), tok[7]);
        /* probe のキーは event ごと: fsave は fmt と BIU・img、response は応答の種類ごとに
         * resp_mismatch が 検査する、他は観測なし（"-"）か then だけ */
        if (!strcmp(r->event, "fsave"))
            probe_check_keys(r->probe, "then,fmt,b31,p,b27,b26,img", 0);
        else if (strcmp(r->event, "response"))
            probe_check_keys(r->probe, "then", 0);
        then_rows += (unsigned)probe_has(r->probe, "then");
        csv_check_one_of("inference", tok[8], "0,1");
        r->inference = csv_int("inference", tok[8]);
    }
    fclose(fp);
}

int main(void) {
    unsigned i, cells = 0, inferred = 0, runs = 0;

    load_rows(MC68K_TRANSITION_CSV);
    CHECK_EQ(nrows, EXPECT_ROWS);
    for (i = 0; i < nrows; i++) {
        unsigned j;
        int first = 1;

        for (j = 0; j < i; j++)
            if (!strcmp(rows[j].dev, rows[i].dev) && rows[j].state == rows[i].state &&
                !strcmp(rows[j].event, rows[i].event))
                first = 0;
        cells += first;
        inferred += (unsigned)rows[i].inference;
        run_row(&rows[i]);
        runs++;
    }
    CHECK_EQ(runs, nrows); /* 全行を実行した（ループが縮まない） */
    CHECK_EQ(cells, EXPECT_CELLS);
    CHECK_EQ(inferred, EXPECT_INFERENCE_ROWS);
    CHECK_EQ(then_rows, EXPECT_THEN_ROWS);
    printf("test_fpu_model_transition: OK（%u 行、%u セル）\n", nrows, cells);
    return 0;
}
