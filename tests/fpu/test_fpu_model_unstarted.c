/*
 * test_fpu_model_unstarted.c — UNSTARTED（算術の先取り・F-line・BSUN）と FSAVE/FRESTORE/ABORT
 * の組合せ （ Phase B、設計書 §4.2、§5.4、§11）。
 *
 * 期待値は設計書 §11 の各項目を手で書き下したもの。frame の BIU は FSAVE の出力から読む。
 *  - S9（転送後 TAKE_MID）で command/condition を書くと TAKE_PRE になり、FSAVE が
 * 011/001・b27=0
 *  - UNSTARTED の ABORT（TAKE を読んだ後の経路を含む）
 *  - S7・S8 の FSAVE→FRESTORE の後、TAKE を読むと IA が1回合法で2回目は violation
 *  - BSUN の frame を live の FPCC/ENABLE を変えて FRESTORE すると TAKE が TF の NULL になる
 *  - 算術の先取りの frame を ENABLE を変えて FRESTORE すると vector が live で変わる
 *  - 入れ子の begin_save() の後の ABORT（SAVE_BODY・RESTORE_BODY）
 *
 * 落とす変異: FRESTORE が FSAVE 時点の vector で固定する、BSUN を image
 * から評価せず再提示する、 S9 から TAKE_MID を残す、ABORT で UNSTARTED
 * の種類を取り違える、入れ子 save の ABORT を無視する。
 */
#include "fpu/fpu_model.h"
#include "fpu_model_setup.h"

#define MAX_BODY 64

typedef struct {
    uint16_t fmt;
    uint32_t body[MAX_BODY];
    unsigned words;
} frame_t;

static frame_t save(mc68k_fpu_t *f) {
    frame_t fr;
    size_t bytes;
    unsigned i;

    memset(&fr, 0, sizeof(fr));
    mc68k_fpu_begin_save(f, &fr.fmt, &bytes);
    CHECK_EQ(bytes,
             save_body_bytes_any(fr.fmt)); /* null・idle・busy 以外の format は body 無し */
    fr.words = (unsigned)(bytes / 4);
    CHECK(fr.words <= MAX_BODY);
    for (i = 0; i < fr.words; i++)
        CHECK(mc68k_fpu_read_operand(f, &fr.body[i]));
    return fr;
}

static void restore(mc68k_fpu_t *f, const frame_t *fr) {
    uint16_t rb;
    size_t bytes;
    unsigned i;

    mc68k_fpu_begin_restore(f, fr->fmt, &rb, &bytes);
    CHECK_EQ(rb, fr->fmt);
    CHECK_EQ(bytes, (size_t)fr->words * 4);
    for (i = 0; i < fr->words; i++)
        CHECK(mc68k_fpu_write_operand(f, fr->body[i]));
}

/* idle frame の BIU（30–28 と b27）と image（+$04 の上位 word） */
static uint32_t idle_biu(const frame_t *fr, mc68k_fpu_config_t cfg) {
    return fr->body[idle_biu_index(cfg)];
}

static unsigned pbits(uint32_t biu) {
    return (biu >> 28) & 7;
}

static unsigned b27(uint32_t biu) {
    return (biu >> 27) & 1;
}

static void test_s9_commands(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = setup("s9", cfg); /* 転送後の TAKE_MID（OPERR=52）。arith=PRESENTED_MID */
    frame_t fr;

    CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 52, 0); /* TAKE_MID でない */
    fr = save(f);
    CHECK_EQ(pbits(idle_biu(&fr, cfg)), 3);
    CHECK_EQ(b27(idle_biu(&fr, cfg)), 0);
    CHECK_EQ(fr.body[0] >> 16, CMD(0, 1, 0, OPM_FADD));
    mc68k_fpu_destroy(f);

    f = setup("s9", cfg);
    CHECK(mc68k_fpu_write_condition(f, 0x01));
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 52, 0);
    fr = save(f);
    CHECK_EQ(pbits(idle_biu(&fr, cfg)), 1);
    CHECK_EQ(b27(idle_biu(&fr, cfg)), 0);
    CHECK_EQ(fr.body[0] >> 16, 0x01);
    mc68k_fpu_destroy(f);
}

/* UNSTARTED の ABORT（68882。TAKE を読んだ後） */
static void test_unstarted_abort_after_take(void) {
    mc68k_fpu_config_t cfg = MC68K_FPU_68882;
    mc68k_fpu_t *f = setup("s4", cfg);
    frame_t fr;

    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 50, 0);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 50, 0); /* arith は残る（S9） */
    fr = save(f);
    CHECK_EQ(pbits(idle_biu(&fr, cfg)), 7); /* command の UNSTARTED は捨てた */
    CHECK_EQ(b27(idle_biu(&fr, cfg)), 0);
    mc68k_fpu_destroy(f);

    f = setup("s5", cfg); /* F-line */
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 11, 0);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    expect_idle(f);
    fr = save(f);
    CHECK_EQ(pbits(idle_biu(&fr, cfg)), 7);
    CHECK_EQ(b27(idle_biu(&fr, cfg)), 1);
    mc68k_fpu_destroy(f);

    f = setup("s4c", cfg); /* condition の UNSTARTED: ABORT で残る */
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 50, 0);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 50, 0);
    fr = save(f);
    CHECK_EQ(pbits(idle_biu(&fr, cfg)), 1);
    mc68k_fpu_destroy(f);

    f = setup("s6", cfg); /* BSUN: TAKE を読んだ後（S8）の ABORT でも残る */
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 48, 1);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 48, 1);
    CHECK(mc68k_fpu_write_instruction_address(f, IA_VALUE));
    mc68k_fpu_destroy(f);
}

/* S7・S8 の FSAVE→FRESTORE: IA 充足は保存されず、FRESTORE 後に TAKE を読むと PC に入る */
static void test_bsun_restore_ia(void) {
    const char *names[2] = {"s8", "s7"};
    unsigned i;

    CHECK_DISTINCT_STR(names);
    for (i = 0; i < 2; i++) {
        mc68k_fpu_config_t cfg = MC68K_FPU_68882;
        mc68k_fpu_t *f = setup(names[i], cfg);
        frame_t fr = save(f);

        CHECK_EQ(pbits(idle_biu(&fr, cfg)), 1);
        restore(f, &fr);
        expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 48, 1);
        CHECK(mc68k_fpu_write_instruction_address(f, IA_VALUE)); /* 1回目は合法（FPIAR 更新） */
        CHECK(!mc68k_fpu_write_instruction_address(f, IA_VALUE + 4)); /* 2回目は IDLE の規則1 */
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

/* BSUN の idle frame（001、b27=1、image=GT）。live の FPCC/ENABLE を変えて FRESTORE する */
static void test_bsun_restore_live(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = setup("s6", cfg);
    frame_t fr = save(f);
    mc68k_fpu_response_t r;

    /* 変えない: vector 48（PC=1） */
    restore(f, &fr);
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 48, 1);
    mc68k_fpu_destroy(f);
    /* FPCC の NaN を解消: GT は真の TF の NULL（TAKE ではない） */
    f = setup("s6", cfg);
    fr = save(f);
    set_fpsr(f, 0);
    restore(f, &fr);
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
    CHECK_EQ(r.condition_true, 1);
    mc68k_fpu_destroy(f);
    /* BSUN の enable を0に: NaN のまま GT は偽。TAKE ではなく TF=0 の NULL */
    f = setup("s6", cfg);
    fr = save(f);
    set_fpcr(f, 0);
    restore(f, &fr);
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
    CHECK_EQ(r.condition_true, 0);
    mc68k_fpu_destroy(f);
}

/* 算術の先取りの frame を ENABLE を変えて FRESTORE すると、vector は live の EXC&ENABLE
 * で決まる */
static void test_arith_restore_live_vector(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = to_s3(cfg);
    frame_t fr;
    mc68k_fpu_response_t r;

    set_fpsr(f, FPSR_EXC(ENABLE_OVFL | ENABLE_DZ));
    set_fpcr(f, FPCR_ENABLE(ENABLE_OVFL));
    CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 53, 0);
    fr = save(f);
    CHECK_EQ(pbits(idle_biu(&fr, cfg)), 3);
    CHECK_EQ(b27(idle_biu(&fr, cfg)), 0);
    set_fpcr(f, FPCR_ENABLE(ENABLE_DZ)); /* FRESTORE の前に ENABLE を OVFL から DZ へ */
    restore(f, &fr);
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 50, 0);
    /* ENABLE を0にすると候補が無く、保留は解消して image の command の通常の最初の primitive */
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
    (void)save(f); /* S2 へ */
    set_fpcr(f, 0);
    restore(f, &fr);
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL); /* FADD FP1,FP0 の NULL（要求の無い命令） */
    CHECK_EQ(fpu_state_id(f), 2);
    fr = save(f);
    CHECK_EQ(b27(idle_biu(&fr, cfg)), 1); /* 保留は解消済み */
    mc68k_fpu_destroy(f);
}

/* 入れ子の begin_save() の後の ABORT: 転送は破壊的に中止され、中断側の operand アクセスは
 * violation */
static void test_nested_save_abort(void) {
    mc68k_fpu_config_t cfg = MC68K_FPU_68882;
    mc68k_fpu_t *f = setup("s2", cfg);
    uint16_t fmt;
    size_t bytes;
    uint32_t w;

    mc68k_fpu_begin_save(f, &fmt, &bytes);
    CHECK(mc68k_fpu_read_operand(f, &w));
    mc68k_fpu_begin_save(f, &fmt, &bytes);
    CHECK_EQ(fmt, 0x0200);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    CHECK_EQ(fpu_state_id(f), 2);          /* idle に戻る。以後の command 書込みは受理 */
    CHECK(!mc68k_fpu_read_operand(f, &w)); /* 中断側の operand 読出しは violation */
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION, 13, 0);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
    CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
    mc68k_fpu_destroy(f);

    /* RESTORE_BODY: busy frame の残りは適用されない */
    {
        mc68k_fpu_t *d = setup("s16", cfg);
        frame_t fr = save(d);
        uint16_t rb;
        unsigned i;

        mc68k_fpu_destroy(d);
        f = setup("s2", cfg);
        mc68k_fpu_begin_restore(f, fr.fmt, &rb, &bytes);
        for (i = 0; i < 4; i++)
            CHECK(mc68k_fpu_write_operand(f, fr.body[i]));
        CHECK_EQ(i, 4); /* ループが縮まない */
        mc68k_fpu_begin_save(f, &fmt, &bytes);
        CHECK_EQ(fmt, 0x0200);
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
        CHECK_EQ(fpu_state_id(f), 2);
        CHECK(!mc68k_fpu_write_operand(f, fr.body[4])); /* 残りの書込みは violation */
        expect_take(f, MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION, 13, 0);
        mc68k_fpu_destroy(f);
    }
}

/* b27=1（EXC-PEND なし）の idle frame は、live の EXC&ENABLE に候補があっても TAKE
 * を再提示しない。 保留の有無は frame の b27 だけで決まり、live の FPSR/FPCR
 * からは作らない（§5.4） */
static void test_restore_b27_set_ignores_live_candidate(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f =
        setup("s2", cfg); /* S1 の FSAVE は null frame なので、S2（idle）から保存する */
    frame_t fr = save(f);
    mc68k_fpu_response_t r;

    CHECK_EQ(fr.fmt, idle_format_word(cfg));
    CHECK_EQ(b27(idle_biu(&fr, cfg)), 1);
    set_fpsr(f, FPSR_EXC(ENABLE_DZ));
    set_fpcr(f, FPCR_ENABLE(ENABLE_DZ)); /* live の候補あり */
    restore(f, &fr);
    CHECK_EQ(fpu_state_id(f), 2);
    CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL); /* TAKE_PRE ではない */
    mc68k_fpu_destroy(f);
}

/* 保留算術例外がある状態の protocol violation を ACK すると、保留が消えて次の算術命令は TAKE
 * を受けない（§4.2 の S10p） */
static void test_violation_ack_drops_pending(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = to_s3(cfg);
    mc68k_fpu_response_t r;

    CHECK(!mc68k_fpu_write_operand(f, 0)); /* S10: violation */
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
    CHECK_EQ(fpu_state_id(f), 2);
    CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
    mc68k_fpu_destroy(f);
}

int main(void) {
    unsigned i;

    for (i = 0; i < 2; i++) {
        test_s9_commands(model_cfg(i));
        test_bsun_restore_live(model_cfg(i));
        test_arith_restore_live_vector(model_cfg(i));
        test_restore_b27_set_ignores_live_candidate(model_cfg(i));
        test_violation_ack_drops_pending(model_cfg(i));
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
    test_unstarted_abort_after_take();
    test_bsun_restore_ia();
    test_nested_save_abort();
    printf("test_fpu_model_unstarted: OK\n");
    return 0;
}
