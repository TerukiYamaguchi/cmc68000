/*
 * test_fpu_model_abort_window.c — phase 別の frame 種別・BIU 値と、68882 の abort window
 * （ Phase B、設計書 §4.1、§5.4、§11）。
 *
 * tests/data/fpu/fpu2_phase_biu_assignment.csv の全14行を、その phase に到達する手順で駆動する。
 *  - FSAVE の frame 種別（idle/busy/`$02xx`）と BIU の30–28・bit26 が CSV と一致すること。
 *  - 68882 の ABORT: `inside` は IDLE に戻り保留算術例外が残り、`outside` は状態が変わらず、
 *    `abort_effective`（FSAVE/FRESTORE の転送中）は転送が止まること。
 * 行数14を assert する。condition 欄の文言は読まない（設計書 §4.1 との照合は
 * check_fpu2_tables.py）。phase と到達手順の対応は下の表（CSV の phase 名と30–28で引く）。
 *
 * 落とす変異: CSV の BIU 欄・window 欄のうち、状態が変わる行（IDLE の command の UNSTARTED、PC
 * 以外の busy 行）の 所属の反転（毒入れ）、行ループの縮小（行数 assert）、window 内の ABORT で
 * 保留算術例外を捨てる、command の UNSTARTED を window 外として残す、condition の UNSTARTED
 * を捨てる、 window 外の ABORT で S13 の TF を消す。
 */
#include "fpu/fpu_model.h"
#include "fpu_model_csv.h"
#include "fpu_model_setup.h"

#define EXPECT_ROWS 14u

static const struct {
    const char *phase;
    const char *p3028; /* frame の30–28（無ければ ""） */
    const char *fsave; /* CSV の fsave_frame */
    const char *setup;
} map[] = {
    {"IDLE", "111", "idle", "s2"},
    {"IDLE", "011", "idle", "s4"},
    {"IDLE", "001", "idle", "s4c"},
    {"COMMAND_PENDING", "011", "busy", "s12"},
    {"CONDITION_PENDING", "001", "busy", "s13"},
    {"RESPONSE_NEXT", "110", "busy", "s14"},
    {"RESPONSE_NEXT", "011", "busy", "s14d"},
    {"OPERAND_IN", "100", "busy", "s15"},
    {"OPERAND_OUT", "110", "busy", "s16"},
    {"REGISTER_SELECT", "011", "busy", "s17"},
    {"PC", "011", "busy", "s18n"},
    {"PC", "001", "idle", "s8"},
    {"SAVE_BODY", "", "02xx", "s19"},
    {"RESTORE_BODY", "", "02xx", "s20"},
};

typedef struct {
    char phase[24], cond[160], dev[8], fsave[8], p[8], b26[4], window[24];
} row_t;

static const char *setup_for(const row_t *r) {
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(map[i].phase, r->phase) && !strcmp(map[i].p3028, r->p) &&
            !strcmp(map[i].fsave, r->fsave))
            return map[i].setup;
    fprintf(stderr, "未知の行: %s/%s/%s\n", r->phase, r->p, r->fsave);
    exit(1);
}

static void check_frame(const row_t *r, const char *setup_name, mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = setup(setup_name, cfg);
    uint16_t fmt;
    size_t bytes, k;
    uint32_t body[64];

    if (!strcmp(r->fsave, "02xx")) {
        /* 転送中の save CIR 読出しは illegal format word（$02xx）で body 0 */
        mc68k_fpu_begin_save(f, &fmt, &bytes);
        CHECK_EQ(fmt >> 8, 0x02);
        CHECK_EQ(bytes, 0);
        mc68k_fpu_destroy(f);
        return;
    }
    mc68k_fpu_begin_save(f, &fmt, &bytes);
    CHECK_EQ(fmt, !strcmp(r->fsave, "idle") ? idle_format_word(cfg) : busy_format_word(cfg));
    CHECK_EQ(bytes, save_body_bytes(cfg, fmt)); /* BIU の位置を読む前に body 長を確かめる */
    CHECK(bytes / 4 <= sizeof(body) / sizeof(body[0]));
    for (k = 0; k < bytes / 4; k++)
        CHECK(mc68k_fpu_read_operand(f, &body[k]));
    {
        size_t idx = !strcmp(r->fsave, "busy") ? BUSY_BIU_INDEX : idle_biu_index(cfg);
        unsigned p = (unsigned)((body[idx] >> 28) & 7);
        unsigned want = (unsigned)((r->p[0] - '0') * 4 + (r->p[1] - '0') * 2 + (r->p[2] - '0'));

        CHECK_EQ(p, want);
        CHECK_EQ((body[idx] >> 26) & 1, (unsigned)(r->b26[0] - '0'));
    }
    mc68k_fpu_destroy(f);
}

static void check_abort(const row_t *r, const char *setup_name) {
    mc68k_fpu_t *f = setup(setup_name, MC68K_FPU_68882);
    int before = fpu_state_id(f);
    int after;

    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    after = fpu_state_id(f);
    if (!strcmp(r->window, "outside")) {
        CHECK_EQ(after, before); /* 状態が変わらない */
        if (before == 13) {      /* CONDITION_PENDING: 同じ TF の NULL が読める */
            mc68k_fpu_response_t resp = rd(f);

            CHECK_EQ(resp.kind, MC68K_FPU_RESPONSE_NULL);
            CHECK_EQ(resp.condition_true, 0); /* s13 の predicate は F（偽） */
            mc68k_fpu_destroy(f);
            f = setup("s13t",
                      MC68K_FPU_68882); /* predicate T（TF=1）でも ABORT で TF は変わらない */
            mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
            resp = rd(f);
            CHECK_EQ(resp.kind, MC68K_FPU_RESPONSE_NULL);
            CHECK_EQ(resp.condition_true, 1);
        }
        if (before ==
            8) { /* UNSTARTED（BSUN）のある PC: 同じ TAKE が読め、続く IA 書込みは合法 */
            expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 48, 1);
            CHECK(mc68k_fpu_write_instruction_address(f, IA_VALUE));
        }
        if (before ==
            2) { /* IDLE（setup は T 条件の TF=1 の NULL を保持）: ABORT で保持は消えない */
            mc68k_fpu_response_t resp = rd(f);

            CHECK_EQ(resp.kind, MC68K_FPU_RESPONSE_NULL);
            CHECK_EQ(resp.condition_true, 1);
        }
        if (before == 4) { /* condition の UNSTARTED（算術の先取り）: 同じ TAKE が残る */
            expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 50, 0);
        }
    } else if (!strcmp(r->window, "inside")) {
        /* IDLE に戻る。command の UNSTARTED（s4）は捨てて S9（保留算術例外 PRESENTED_PRE
         * が残る）、 他は S2（保留算術例外の無い setup） */
        CHECK(before != 19 && before != 20); /* body 転送中は abort_effective の行 */
        CHECK_EQ(after, before == 4 ? 9 : 2);
        if (after == 2)
            expect_idle(f); /* 応答は idle の NULL（TF=0）。inside 行の setup は TF
                             * を保持しない（command 書込みで消える）。 TF=1 を保持する S2
                             * の行を inside に毒入れすると、ここで落ちる */
    } else {
        /* abort_effective は body 転送中（S19・S20）だけ。window 名が未知の値ならここで落とす
         */
        CHECK(!strcmp(r->window, "abort_effective"));
        CHECK(before == 19 || before == 20);
        CHECK_EQ(after, 2); /* 転送が止まる */
    }
    mc68k_fpu_destroy(f);
}

int main(void) {
    FILE *fp = fopen(MC68K_BIU_CSV, "r");
    char line[768];
    unsigned rows = 0, abort_checks = 0, frame_checks = 0;

    CHECK(fp != NULL);
    CHECK(fgets(line, sizeof(line), fp) != NULL);
    while (fgets(line, sizeof(line), fp)) {
        char *tok[7];
        row_t r;
        const char *setup_name;

        split_csv(line, tok, 7);
        memset(&r, 0, sizeof(r));
        csv_copy(r.phase, sizeof(r.phase), tok[0]);
        csv_copy(r.cond, sizeof(r.cond), tok[1]);
        csv_copy(r.dev, sizeof(r.dev), tok[2]);
        csv_copy(r.fsave, sizeof(r.fsave), tok[3]);
        csv_copy(r.p, sizeof(r.p), tok[4]);
        csv_copy(r.b26, sizeof(r.b26), tok[5]);
        tok[6][strcspn(tok[6], ",")] = 0; /* window の後ろは注記 */
        csv_copy(r.window, sizeof(r.window), tok[6]);
        csv_check_one_of("device", r.dev, "68882,both");
        csv_check_one_of("fsave", r.fsave, "idle,busy,02xx");
        csv_check_one_of("window", r.window, "outside,inside,abort_effective");
        if (!strcmp(r.fsave, "02xx"))
            CHECK(!r.p[0] && !r.b26[0]); /* 02xx の frame は BIU を持たない（p・b26 は空欄） */
        else
            csv_check_one_of("biu_26", r.b26,
                             "0,1"); /* p は setup_for() が map の完全一致で絞る */
        setup_name = setup_for(&r);
        if (strcmp(r.dev, "68882") != 0) {
            check_frame(&r, setup_name, MC68K_FPU_68881);
            frame_checks++;
        }
        check_frame(&r, setup_name, MC68K_FPU_68882);
        frame_checks++;
        check_abort(&r, setup_name);
        abort_checks++;
        rows++;
    }
    fclose(fp);
    CHECK_EQ(rows, EXPECT_ROWS);
    CHECK_EQ(abort_checks, EXPECT_ROWS);
    CHECK_EQ(frame_checks, 14u + 12u); /* both の12行は両機種、68882 のみの2行は68882 */
    printf("test_fpu_model_abort_window: OK（%u 行）\n", rows);
    return 0;
}
