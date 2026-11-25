/*
 * test_fpu_model_protocol.c — protocol violation 行列（ Phase B、設計書 §11）。
 *
 * CIR アクセス7種（command 書込み、condition 書込み、operand 書込み、operand 読出し、register
 * select 読出し、register select 書込み、IA 書込み）× §4.1 の phase
 * 10種の各機種のセルを全件与える。 期待値は
 * tests/data/fpu/fpu2_protocol_matrix.csv（scripts/gen_fpu2_take_transition.py の手書き。production の
 * コードから取らない）。68881 に PC phase は無いので7セルは na で、件数に含めて assert する。
 * violation のセルは全件で、戻り値 false、次の応答が vector 13 の TAKE_MID_EXCEPTION、読出しで
 * 消費されないこと、ACK で idle に戻り次の応答が idle NULL であることを確認する。
 * 1つのセルの setups は、| で区切った全ての到達手順で同じ期待値になる。
 *
 * 落とす変異: CSV の1セルの期待値の反転（毒入れ）、行ループの縮小（行数 assert）、phase
 * の当てはめ 誤り、violation 後の ACK で idle に戻らない、violation で false を返さない。
 */
#include "fpu/fpu_model.h"
#include "fpu_model_csv.h"
#include "fpu_model_setup.h"

#define MAX_ROWS             256
#define EXPECT_ROWS_81       70u
#define EXPECT_ROWS_82       74u
#define EXPECT_NA            7u
#define EXPECT_PHASES        10u  /* 機種ごとの phase 数（command の行数と同じ） */
#define EXPECT_ROWS_82_CP_IA 5u   /* 68882 の COMMAND_PENDING × ia_write の行数 */
#define EXPECT_RUNS          253u /* setups の | 区切りを展開した実行数（na を除く） */

typedef struct {
    char dev[8];
    char phase[24];
    char access[24];
    char setups[48];
    char expect[16];
    int line;
} row_t;

static row_t rows[MAX_ROWS];
static unsigned nrows;

#define ROWFAIL(r, setup_name, msg)                                                    \
    do {                                                                               \
        fprintf(stderr, "line %d: %s/%s/%s/%s: %s\n", (r)->line, (r)->dev, (r)->phase, \
                (r)->access, setup_name, msg);                                         \
        exit(1);                                                                       \
    } while (0)

static int apply(mc68k_fpu_t *f, const char *access) {
    uint32_t w;
    uint16_t sel;

    if (!strcmp(access, "command"))
        return mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD));
    if (!strcmp(access, "condition"))
        return mc68k_fpu_write_condition(f, 0x01);
    if (!strcmp(access, "operand_write"))
        return mc68k_fpu_write_operand(f, 0);
    if (!strcmp(access, "operand_read"))
        return mc68k_fpu_read_operand(f, &w);
    if (!strcmp(access, "register_select_read"))
        return mc68k_fpu_read_register_select(f, &sel);
    if (!strcmp(access, "register_select_write"))
        return mc68k_fpu_write_register_select(f);
    CHECK(!strcmp(access, "ia_write"));
    return mc68k_fpu_write_instruction_address(f, IA_VALUE);
}

/* §4.1 の phase に当たる表 A の状態番号（setup が名乗った phase に到達したかの確認に使う） */
static int state_in_phase(const char *phase, int s) {
    static const struct {
        const char *phase;
        int lo, hi, extra; /* lo〜hi と extra（無ければ0）が当たる */
    } map[] = {
        {"IDLE", 1, 7, 9},
        {"COMMAND_PENDING", 12, 12, 0},
        {"CONDITION_PENDING", 13, 13, 0},
        {"RESPONSE_NEXT", 14, 14, 0},
        {"OPERAND_IN", 15, 15, 0},
        {"OPERAND_OUT", 16, 16, 0},
        {"REGISTER_SELECT", 17, 17, 0},
        {"PC", 18, 18, 8},
        {"SAVE_BODY", 19, 19, 0},
        {"RESTORE_BODY", 20, 20, 0},
    };
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(map[i].phase, phase))
            return (s >= map[i].lo && s <= map[i].hi) || (map[i].extra && s == map[i].extra);
    return 0;
}

static void run_cell(const row_t *r, const char *setup_name) {
    mc68k_fpu_config_t cfg = !strcmp(r->dev, "68881") ? MC68K_FPU_68881 : MC68K_FPU_68882;
    mc68k_fpu_t *f = setup(setup_name, cfg);
    int legal = !strcmp(r->expect, "legal");
    int ret;
    mc68k_fpu_response_t a, b;

    if (!state_in_phase(r->phase, fpu_state_id(f)))
        ROWFAIL(r, setup_name, "setup が名乗った phase に到達していない");
    ret = apply(f, r->access);

    if (ret != legal)
        ROWFAIL(r, setup_name, legal ? "合法のはずが false" : "violation のはずが true");
    if (!legal) {
        a = rd(f);
        b = rd(f);
        if (a.kind != MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION || a.vector != 13 || a.request_pc ||
            a.coprocessor_active)
            ROWFAIL(r, setup_name, "violation の次の応答は vector 13 の TAKE_MID（PC=0）");
        if (b.kind != a.kind || b.vector != a.vector)
            ROWFAIL(r, setup_name, "TAKE_MID は読出しで消費されない");
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
        a = rd(f);
        if (a.kind != MC68K_FPU_RESPONSE_NULL || a.coprocessor_active || a.request_pc)
            ROWFAIL(r, setup_name, "violation の ACK の後は idle の NULL");
    }
    mc68k_fpu_destroy(f);
}

int main(void) {
    FILE *fp = fopen(MC68K_MATRIX_CSV, "r");
    char line[512];
    int lineno = 0;
    unsigned i, n81 = 0, n82 = 0, na = 0, cells = 0, phases81 = 0, phases82 = 0;

    CHECK(fp != NULL);
    CHECK(fgets(line, sizeof(line), fp) != NULL);
    lineno++;
    while (fgets(line, sizeof(line), fp)) {
        char *tok[6];
        row_t *r;

        lineno++;
        split_csv(line, tok, 6);
        CHECK(nrows < MAX_ROWS);
        r = &rows[nrows++];
        r->line = lineno;
        csv_copy(r->dev, sizeof(r->dev), tok[0]);
        csv_copy(r->phase, sizeof(r->phase), tok[1]);
        csv_copy(r->access, sizeof(r->access), tok[2]);
        csv_copy(r->setups, sizeof(r->setups), tok[3]);
        csv_copy(r->expect, sizeof(r->expect), tok[4]);
        csv_check_one_of("device", r->dev, "68881,68882");
        csv_check_one_of("expect", r->expect, "legal,violation,na");
    }
    fclose(fp);
    for (i = 0; i < nrows; i++) {
        const row_t *r = &rows[i];
        char buf[48];
        char *s;
        unsigned j;

        if (!strcmp(r->dev, "68881"))
            n81++;
        else
            n82++;

        /* (device, phase, access, setups)
         * は一意（同じ組の行が増えて別の行と重なっても件数が合うのを防ぐ。 68882 の
         * COMMAND_PENDING の ia_write は setup ごとに期待が違うので setups も組に含める） */
        for (j = 0; j < i; j++)
            CHECK(strcmp(rows[j].dev, r->dev) || strcmp(rows[j].phase, r->phase) ||
                  strcmp(rows[j].access, r->access) || strcmp(rows[j].setups, r->setups));
        if (!strcmp(r->expect, "na")) {
            /* na は 68881 に PC phase が無い行だけ */
            CHECK(!strcmp(r->dev, "68881") && !strcmp(r->phase, "PC"));
            /* na 行の setups は、同じ (phase, access) の 68882 の行と同じ（68881 には無い phase
             * の欄だが、 68882 側の setup
             * の一覧と食い違う値を書いても死んだ欄にならないようにする） */
            for (j = 0; j < nrows; j++)
                if (!strcmp(rows[j].dev, "68882") && !strcmp(rows[j].phase, r->phase) &&
                    !strcmp(rows[j].access, r->access))
                    CHECK(!strcmp(rows[j].setups, r->setups));
            na++;
            continue;
        }
        csv_copy(buf, sizeof(buf), r->setups);
        for (s = strtok(buf, "|"); s; s = strtok(NULL, "|")) {
            run_cell(r, s);
            cells++;
        }
    }
    /* 行のある (device, phase) は全 access（7種）の行を、(device, phase, access)
     * ごとに決まった行数で持つ （68882 の COMMAND_PENDING の ia_write だけは setup
     * ごとに期待が違い、5行。§11）。行の access を
     * 書き換えて別の行と入れ替わっても、件数だけでは見えない。phase は機種ごとに EXPECT_PHASES
     * 個 */
    for (i = 0; i < nrows; i++) {
        static const char *const accesses[] = {"command",
                                               "condition",
                                               "operand_write",
                                               "operand_read",
                                               "register_select_read",
                                               "register_select_write",
                                               "ia_write"};
        unsigned a, j;

        CHECK_DISTINCT_STR(accesses);
        for (a = 0; a < sizeof(accesses) / sizeof(accesses[0]); a++) {
            unsigned found = 0;

            for (j = 0; j < nrows; j++)
                found += !strcmp(rows[j].dev, rows[i].dev) &&
                         !strcmp(rows[j].phase, rows[i].phase) &&
                         !strcmp(rows[j].access, accesses[a]);
            CHECK_EQ(found, !strcmp(rows[i].dev, "68882") &&
                                    !strcmp(rows[i].phase, "COMMAND_PENDING") &&
                                    !strcmp(accesses[a], "ia_write")
                                ? EXPECT_ROWS_82_CP_IA
                                : 1u);
        }
        CHECK_EQ(
            a,
            7u); /* access
                    は7種（command・condition・operand_write・operand_read・register_select_read・register_select_write・ia_write）
                  */
        if (!strcmp(rows[i].access, "command")) {
            if (!strcmp(rows[i].dev, "68881"))
                phases81++;
            else
                phases82++;
        }
    }
    CHECK_EQ(phases81, EXPECT_PHASES);
    CHECK_EQ(phases82, EXPECT_PHASES);
    CHECK_EQ(n81, EXPECT_ROWS_81);
    CHECK_EQ(n82, EXPECT_ROWS_82);
    CHECK_EQ(na, EXPECT_NA);
    CHECK_EQ(cells, EXPECT_RUNS);
    printf("test_fpu_model_protocol: OK（%u 行、%u 実行、na %u）\n", nrows, cells, na);
    return 0;
}
