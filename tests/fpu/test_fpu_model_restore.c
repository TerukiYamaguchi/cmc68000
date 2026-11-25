/*
 * test_fpu_model_restore.c — 設計書 §4.2 の表 B（有効な format word の FRESTORE）の全セル×機種
 * 。
 *
 * 期待値は tests/data/fpu/fpu2_restore_transition.csv（scripts/gen_fpu2_take_transition.py が表 B
 * から写した 手書きの期待値）。各行は「setup で開始状態へ到達 → frame を組み立てて
 * begin_restore → body を 全部 write_operand → 状態、応答を検査」。frame の idle 部分は設計書
 * §7.1・Figure 6-4/6-5 の配置 （68881: image +$04、BIU +$18。68882: image +$04、BIU
 * +$38）から手で組み立て、busy frame は 実際の FSAVE の出力（donor）を使う。
 *
 * 落とす変異: CSV の1行の期待値の反転、行ループの縮小（件数 assert）、begin_restore() が
 * arith・UNSTARTED・violation・hold を捨てない、B9・B11 で context_valid を変える、B5・B7 で
 * live の FPSR/FPCR を使わない（FSAVE 時点の値で固定する）。
 */
#include "fpu/fpu_model.h"
#include "fpu_model_csv.h"
#include "fpu_model_setup.h"

#define MAX_ROWS 1024
#define EXPECT_INFERENCE_ROWS \
    574u /* 表の【推】を含むセルの行数（生成スクリプトが表から決める） */
#define EXPECT_ROWS  656u
#define EXPECT_CELLS 407u
#define MAX_BODY     64

typedef struct {
    char dev[8];
    int state;
    char setup[8];
    char frame[8];
    char variant[8];
    int next;
    char probe[PROBE_BUF];
    int inference;
    int line;
} row_t;

typedef struct {
    uint16_t fmt;
    uint32_t body[MAX_BODY];
    unsigned words;
} frame_t;

static row_t rows[MAX_ROWS];
static unsigned nrows;

#define ROWFAIL(r, msg)                                                                   \
    do {                                                                                  \
        fprintf(stderr, "line %d: %s/%d/%s/%s/%s: %s\n", (r)->line, (r)->dev, (r)->state, \
                (r)->setup, (r)->frame, (r)->variant, msg);                               \
        exit(1);                                                                          \
    } while (0)

static uint32_t biu(unsigned b31, unsigned p, unsigned b27) {
    return ((uint32_t)b31 << 31) | ((uint32_t)p << 28) | ((uint32_t)b27 << 27) | (1u << 26) |
           0xFFFFu;
}

static frame_t idle_frame(mc68k_fpu_config_t cfg, unsigned b31, unsigned p, unsigned b27,
                          uint32_t image) {
    frame_t fr;
    unsigned biu_idx = idle_biu_index(cfg);

    memset(&fr, 0, sizeof(fr));
    fr.fmt = idle_format_word(cfg);
    fr.words = biu_idx + 1;   /* 68881: 6 longword、68882: 14 longword */
    fr.body[0] = image << 16; /* +$04 の上位 word */
    fr.body[biu_idx] = biu(b31, p, b27);
    return fr;
}

/* donor: 別のモデルを setup して FSAVE した busy frame（モデル固定表現）。実際の FSAVE の出力
 */
static frame_t donor_frame(mc68k_fpu_config_t cfg, const char *setup_name) {
    mc68k_fpu_t *d = setup(setup_name, cfg);
    frame_t fr;
    size_t bytes;
    unsigned i;

    memset(&fr, 0, sizeof(fr));
    mc68k_fpu_begin_save(d, &fr.fmt, &bytes);
    CHECK_EQ(fr.fmt, busy_format_word(cfg));
    fr.words = (unsigned)(bytes / 4);
    CHECK_EQ(fr.words, is82(cfg) ? 53u : 45u);
    for (i = 0; i < fr.words; i++)
        CHECK(mc68k_fpu_read_operand(d, &fr.body[i]));
    mc68k_fpu_destroy(d);
    return fr;
}

static uint32_t image_word(const char *variant) {
    if (!strcmp(variant, "arith"))
        return CMD(0, 1, 0, OPM_FADD);
    if (!strcmp(variant, "movem"))
        return 0xF080;
    CHECK(!strcmp(variant, "invalid")); /* B4・B5 の variant は arith・movem・invalid
                                           の3種。他を invalid へ流さない */
    return 0x2000;                      /* invalid（class 001） */
}

static frame_t build_frame(mc68k_fpu_config_t cfg, const row_t *r) {
    frame_t fr;
    const char *b = r->frame;
    unsigned num = (unsigned)csv_int("frame", b + 1);

    memset(&fr, 0, sizeof(fr));
    switch (num) {
    case 1:
        fr.fmt = 0x0042; /* null: size byte は無視される */
        return fr;
    case 2:
        return idle_frame(cfg, 0, 7, 1, 0);
    case 3:
        return idle_frame(cfg, 0, 7, 0, 0);
    case 4:
        return idle_frame(cfg, 0, 3, 1, image_word(r->variant));
    case 5:
        return idle_frame(cfg, 0, 3, 0, image_word(r->variant));
    case 6:
        return idle_frame(cfg, 0, 1, 1, 0x01); /* predicate EQ */
    case 7:
        return idle_frame(cfg, 0, 1, 0, 0x01);
    case 8:
        if (!strcmp(r->variant, "idle"))
            return idle_frame(cfg, 1, 7, 1, 0);
        fr = donor_frame(cfg, "s16");
        fr.body[0] |= 0x80000000u;
        return fr;
    case 9:
        return idle_frame(cfg, 0, (unsigned)strtoul(r->variant + 1, NULL, 2), 1,
                          0); /* "p010" の3桁は2進数 */
    case 10:
        return donor_frame(cfg, "s16");
    default:
        fr = donor_frame(cfg, "s16");
        fr.body[1] ^= 1; /* 固定表現の magic を壊す */
        return fr;
    }
}

static void run_row(const row_t *r) {
    mc68k_fpu_config_t cfg = !strcmp(r->dev, "68881") ? MC68K_FPU_68881 : MC68K_FPU_68882;
    mc68k_fpu_t *f = setup(r->setup, cfg);
    frame_t fr = build_frame(cfg, r);
    uint16_t readback;
    size_t bytes;
    unsigned i;
    char then[96];
    int v;
    char s[16];

    if (fpu_state_id(f) != r->state)
        ROWFAIL(r, "setup が開始状態に到達していない");
    mc68k_fpu_begin_restore(f, fr.fmt, &readback, &bytes);
    if (readback != fr.fmt)
        ROWFAIL(r, "有効な format word の read-back は書込み値と同値");
    if (bytes != (size_t)fr.words * 4)
        ROWFAIL(r, "body の byte 数");
    if (fr.words) {
        if (fpu_state_id(f) != 20)
            ROWFAIL(r, "begin_restore の後は RESTORE_BODY（S20）");
        for (i = 0; i < fr.words; i++) {
            if (!mc68k_fpu_write_operand(f, fr.body[i]))
                ROWFAIL(r, "body の write_operand が false");
        }
    }
    if (fpu_state_id(f) != r->next) {
        fprintf(stderr, "line %d: %s/%d/%s/%s/%s: 次の状態 期待 S%d 実際 S%d\n", r->line,
                r->dev, r->state, r->setup, r->frame, r->variant, r->next, fpu_state_id(f));
        exit(1);
    }
    if (probe_then(r->probe, then, sizeof(then))) {
        const char *why = resp_mismatch(then, rd(f));

        if (why)
            ROWFAIL(r, why);
    }
    if (probe_get(r->probe, "regs", s, sizeof(s))) {
        mc68k_fpu_regs_t g = regs(f);

        if (strcmp(s, "reset"))
            ROWFAIL(r, "regs の値は reset だけ");
        for (i = 0; i < 8; i++)
            if (g.fp[i].sign_exp != 0x7FFF || g.fp[i].signif != UINT64_MAX)
                ROWFAIL(r, "null FRESTORE は FPn を reset 値（NaN）にする");
        CHECK_EQ(i, 8); /* ループが縮まない */
        if (g.fpcr || g.fpsr || g.fpiar)
            ROWFAIL(r, "null FRESTORE は FPCR/FPSR/FPIAR を0にする");
    }
    /* ctx と fsave_b27 はどちらも begin_save を呼ぶ。同じ行で2回呼ぶと2回目は $02xx・body 0
     * になる */
    CHECK(!(probe_has(r->probe, "ctx") && probe_has(r->probe, "fsave_b27")));
    if (probe_int(r->probe, "ctx", &v)) {
        uint16_t fmt;
        size_t nb;

        if (v != 0 && v != 1)
            ROWFAIL(r, "ctx の値は 0/1");
        mc68k_fpu_begin_save(f, &fmt, &nb);
        if ((fmt == 0) != (v == 0))
            ROWFAIL(r, "context_valid（FSAVE が null frame か）");
    }
    if (probe_int(r->probe, "fsave_b27", &v)) {
        /* FSAVE の BIU bit27（EXC-PEND）で arith が NONE（1）か保留あり（0）かを見る */
        uint16_t fmt;
        size_t nb;
        uint32_t body[MAX_BODY];
        size_t k;
        unsigned idx;

        if (v != 0 && v != 1)
            ROWFAIL(r, "fsave_b27 の値は 0/1");
        mc68k_fpu_begin_save(f, &fmt, &nb);
        if (fmt != idle_format_word(cfg) && fmt != busy_format_word(cfg))
            ROWFAIL(r, "fsave_b27 を見るには FSAVE が idle・busy frame であること（null・$02xx "
                       "では body が無い）");
        if (nb != save_body_bytes(cfg, fmt) || nb / 4 > MAX_BODY)
            ROWFAIL(r,
                    "FSAVE の body 長が frame の大きさと違う（BIU の位置を読む前に確かめる）");
        for (k = 0; k < nb / 4; k++)
            CHECK(mc68k_fpu_read_operand(f, &body[k]));
        idx = (fmt == busy_format_word(cfg)) ? BUSY_BIU_INDEX : idle_biu_index(cfg);
        if ((int)((body[idx] >> 27) & 1) != v)
            ROWFAIL(r, "FSAVE の BIU bit27（arith）");
    }
    mc68k_fpu_destroy(f);
}

static void load_rows(const char *path) {
    FILE *fp = fopen(path, "r");
    char line[512];
    int lineno = 0;

    CHECK(fp != NULL);
    CHECK(fgets(line, sizeof(line), fp) != NULL);
    lineno++;
    while (fgets(line, sizeof(line), fp)) {
        char *tok[8];
        row_t *r;

        lineno++;
        split_csv(line, tok, 8);
        CHECK(nrows < MAX_ROWS);
        r = &rows[nrows++];
        r->line = lineno;
        csv_copy(r->dev, sizeof(r->dev), tok[0]);
        r->state = csv_int("state", tok[1]);
        csv_copy(r->setup, sizeof(r->setup), tok[2]);
        csv_copy(r->frame, sizeof(r->frame), tok[3]);
        csv_copy(r->variant, sizeof(r->variant), tok[4]);
        r->next = csv_int("next", tok[5]);
        csv_copy(r->probe, sizeof(r->probe), tok[6]);
        probe_check_keys(r->probe,
                         !strcmp(r->frame, "B1") ? "then,regs,ctx"
                         : !strcmp(r->frame, "B4") || !strcmp(r->frame, "B5") ||
                                 !strcmp(r->frame, "B6") || !strcmp(r->frame, "B7")
                             ? "then"
                             : "then,fsave_b27", /* B2・B3・B8・B9・B10・B11 */
                         0);
        csv_check_one_of("inference", tok[7], "0,1");
        r->inference = csv_int("inference", tok[7]);
        csv_check_one_of("device", r->dev, "68881,68882");
        csv_check_one_of("frame", r->frame,
                         "B1,B2,B3,B4,B5,B6,B7,B8,B9,B10,B11"); /* build_frame の default を B11
                                                                   以外へ流さない */
        /* variant は frame ごとに絞る: B4・B5 は image の3種、B8 は idle・busy、B9 は PREC
         * 3bit、他は "-" */
        csv_check_one_of("variant", r->variant,
                         !strcmp(r->frame, "B4") || !strcmp(r->frame, "B5")
                             ? "arith,movem,invalid"
                         : !strcmp(r->frame, "B8") ? "idle,busy"
                         : !strcmp(r->frame, "B9") ? "p000,p001,p010,p011,p100,p101,p110,p111"
                                                   : "-");
        /* probe
         * のキーの有無は行ごとの規則で固定する（件数の総数照合では、キーが別の行へ移る変異が通る）。
         * then: B4・B6 は持たない（次状態が S12/S5/S13 で応答が無い）。B5・B7 は
         * next=S4（pending 応答）の行だけ。 他の frame は全行。 fsave_b27: B2・B3・B8・B10
         * は全行。B9・B11 は next=S10 の行だけ。他は持たない。 regs・ctx: B1 だけ（null
         * FRESTORE の観測）。キーが消えるとその観測が黙って未検査になる */
        {
            int is_b5b7 = !strcmp(r->frame, "B5") || !strcmp(r->frame, "B7");
            int is_b9b11 = !strcmp(r->frame, "B9") || !strcmp(r->frame, "B11");
            int is_b4b6 = !strcmp(r->frame, "B4") || !strcmp(r->frame, "B6");
            int want_then = is_b4b6 ? 0 : is_b5b7 ? (r->next == 4) : 1;
            int want_b27 = is_b9b11 ? (r->next == 10)
                                    : (!strcmp(r->frame, "B2") || !strcmp(r->frame, "B3") ||
                                       !strcmp(r->frame, "B8") || !strcmp(r->frame, "B10"));
            int want_b1 = !strcmp(r->frame, "B1");

            if (probe_has(r->probe, "then") != want_then)
                ROWFAIL(r, "then キーの有無が規則（B4・B6 無し、B5・B7 は next=4 "
                           "のみ、他は有り）と違う");
            if (probe_has(r->probe, "fsave_b27") != want_b27)
                ROWFAIL(r, "fsave_b27 キーの有無が規則（B2・B3・B8・B10 有り、B9・B11 は "
                           "next=10 のみ）と違う");
            if (probe_has(r->probe, "regs") != want_b1 || probe_has(r->probe, "ctx") != want_b1)
                ROWFAIL(r, "regs・ctx は B1 の行だけが持つ");
        }
    }
    fclose(fp);
}

int main(void) {
    unsigned i, j, cells = 0, inferred = 0;

    load_rows(MC68K_RESTORE_CSV);
    CHECK_EQ(nrows, EXPECT_ROWS);
    for (i = 0; i < nrows; i++) {
        int first = 1;

        for (j = 0; j < i; j++)
            if (!strcmp(rows[j].dev, rows[i].dev) && rows[j].state == rows[i].state &&
                !strcmp(rows[j].frame, rows[i].frame))
                first = 0;
        cells += first;
        inferred += (unsigned)rows[i].inference;
        run_row(&rows[i]);
    }
    CHECK_EQ(cells, EXPECT_CELLS);
    CHECK_EQ(inferred, EXPECT_INFERENCE_ROWS);
    printf("test_fpu_model_restore: OK（%u 行、%u セル）\n", nrows, cells);
    return 0;
}
