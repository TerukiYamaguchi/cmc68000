/*
 * test_fpu_model_pc.c — PC 要求と応答値の保持（ Phase B、設計書
 * §5.1、§5.2、§5.5、§11）。
 *
 * 68881 は PC 要求を無視でき、IA 書込みが常に合法。68882 は PC=1 の primitive を読んだ後に IA
 * 書込みを 待ち（S18）、IA 先書き・BSUN の TAKE・S18 の各 primitive
 * 種別・応答値の保持（hold）を区別する。 期待値は設計書 §4.2 の表 A と §5.1
 * の規則を手で書き下したもの。
 *
 * 落とす変異: 68881 の IA 書込みを phase で拒む、68882 の PC 待ちを省く、先書きの IA を PC
 * 要求の充足と しない/先書きが無くても PC に入らない、S18 で後続 primitive を返す（消費して
 * NULL になる）、 PC=1 の NULL を PC=0 で返す、保持を FSAVE/command 書込み/violation
 * の後も残す。
 */
#include "fpu/fpu_model.h"
#include "fpu_model_setup.h"

#define PC_IA_FIRST 0x5000u

/* ENABLE=DZ を設定したモデル（算術例外が enable で、最初の primitive が PC=1） */
static mc68k_fpu_t *armed(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);

    set_fpcr(f, FPCR_ENABLE(ENABLE_DZ));
    return f;
}

static void test_68881_ignores_pc(void) {
    mc68k_fpu_t *f = armed(MC68K_FPU_68881);
    mc68k_fpu_response_t r;

    /* 演算 FMOVE.L #imm,FP0: PC=1 の EA_TRANSFER を読んでも IA を待たず operand を書ける */
    CHECK(mc68k_fpu_write_command_word(f, CMD(2, FMT_L, 0, OPM_FMOVE)));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.request_pc, 1);
    CHECK_EQ(fpu_state_id(f), 15);
    CHECK(mc68k_fpu_write_operand(f, 7));
    expect_idle(f);
    /* 外部宛て FMOVE: PC=1 の NULL（CA=1）の後、IA を書かずに DR=1 の EA_TRANSFER が読める */
    CHECK(mc68k_fpu_write_command_word(f, CMD(3, FMT_L, 0, 0)));
    expect_null(f, 1, 1);
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.transfer_to_host, 1);
    mc68k_fpu_destroy(f);
    /* IA 書込みは phase に関係なく合法で FPIAR を更新する（S2 でも、ダイアログ中でも） */
    f = mk(MC68K_FPU_68881);
    CHECK(mc68k_fpu_write_instruction_address(f, 0x1111));
    CHECK_EQ(regs(f).fpiar, 0x1111u);
    CHECK(mc68k_fpu_write_command_word(f, CMD(2, FMT_L, 0, OPM_FMOVE)));
    CHECK(mc68k_fpu_write_instruction_address(f, 0x2222));
    CHECK_EQ(regs(f).fpiar, 0x2222u);
    mc68k_fpu_destroy(f);
}

/* 68882: PC=1 の EA_TRANSFER を読むと PC 待ち（S18）。IA 書込みで operand 待ちへ進む。
 * 読む前に IA を書けば、読んだ後に PC へ入らない（対照: 書かなければ operand 書込みは
 * violation） */
static void test_68882_pc_for_ea_in(void) {
    mc68k_fpu_t *f = armed(MC68K_FPU_68882);
    mc68k_fpu_response_t r;

    CHECK(mc68k_fpu_write_command_word(f, CMD(2, FMT_L, 0, OPM_FMOVE)));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.request_pc, 1);
    CHECK_EQ(fpu_state_id(f), 18);
    /* 消費済みの NULL（CA=1、PC=0）を繰り返し返す。EA_TRANSFER は返さない */
    expect_null(f, 1, 0);
    expect_null(f, 1, 0);
    CHECK(!mc68k_fpu_write_operand(f, 1)); /* IA を書く前の operand 書込みは violation */
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
    mc68k_fpu_destroy(f);

    f = armed(MC68K_FPU_68882);
    CHECK(mc68k_fpu_write_command_word(f, CMD(2, FMT_L, 0, OPM_FMOVE)));
    r = rd(f);
    CHECK_EQ(r.request_pc, 1);
    CHECK(mc68k_fpu_write_instruction_address(f, PC_IA_FIRST));
    CHECK_EQ(regs(f).fpiar, PC_IA_FIRST);
    CHECK_EQ(fpu_state_id(f), 15);
    expect_null(f, 1, 0); /* 応答読出し: S15 の消費済みの NULL */
    CHECK(mc68k_fpu_write_operand(f, 9));
    expect_idle(f);
    mc68k_fpu_destroy(f);

    /* 先書き: 読む前に IA を書くと PC 要求を満たし、読んだ後は S15 */
    f = armed(MC68K_FPU_68882);
    CHECK(mc68k_fpu_write_command_word(f, CMD(2, FMT_L, 0, OPM_FMOVE)));
    CHECK(mc68k_fpu_write_instruction_address(f, PC_IA_FIRST));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.request_pc, 1);
    CHECK_EQ(fpu_state_id(f), 15);
    CHECK(mc68k_fpu_write_operand(f, 9));
    expect_idle(f);
    mc68k_fpu_destroy(f);
}

/* 外部宛て FMOVE（PC=1 の NULL、CA=1）: IA を書くまで同じ NULL（CA=1、PC=1）を返し続け、
 * IA 書込みの時点で DR=1 の EA_TRANSFER を符号化する。IA を書く前の operand 読出しは violation
 */
static void test_68882_pc_for_ext_out(void) {
    mc68k_fpu_t *f = mk(MC68K_FPU_68882);
    mc68k_fpu_response_t r;
    uint32_t w;

    load_long(f, 0, 0); /* FP0 は NaN のまま。FMOVE.L FP0,<ea> は OPERR になるので 0 を入れる */
    set_fpcr(f, FPCR_ENABLE(ENABLE_DZ));
    CHECK(mc68k_fpu_write_command_word(f, CMD(3, FMT_L, 0, 0)));
    expect_null(f, 1, 1);
    CHECK_EQ(fpu_state_id(f), 18);
    expect_null(f, 1, 1); /* 2回以上読んでも同じ。EA_TRANSFER ではない */
    expect_null(f, 1, 1);
    CHECK(!mc68k_fpu_read_operand(f, &w)); /* IA 前の operand 読出しは violation */
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
    mc68k_fpu_destroy(f);

    f = mk(MC68K_FPU_68882);
    load_long(f, 0, 6);
    set_fpcr(f, FPCR_ENABLE(ENABLE_DZ));
    CHECK(mc68k_fpu_write_command_word(f, CMD(3, FMT_L, 0, 0)));
    expect_null(f, 1, 1);
    CHECK(mc68k_fpu_write_instruction_address(f, PC_IA_FIRST));
    CHECK_EQ(fpu_state_id(f), 14);
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.transfer_to_host, 1);
    CHECK_EQ(r.length, 4);
    CHECK(mc68k_fpu_read_operand(f, &w));
    CHECK_EQ(w, 6);
    expect_idle(f);
    mc68k_fpu_destroy(f);

    /* 動的 k-factor の FMOVE.P: PC=1 の MP_REGISTER を読んだ後の応答は消費済みの
     * NULL（CA=1、PC=0）。 IA を書くと Dn の書込み待ち（S15）へ進む */
    f = armed(MC68K_FPU_68882);
    CHECK(mc68k_fpu_write_command_word(f, CMD(3, 7, 0, 2 << 4)));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_MP_REGISTER);
    CHECK(r.coprocessor_active); /* Table 7-7 の $CC0x（CA=1、PC=1。p.7-21） */
    CHECK_EQ(r.request_pc, 1);
    CHECK_EQ(r.data_register, 2);
    expect_null(f, 1, 0);
    expect_null(f, 1, 0);
    CHECK(mc68k_fpu_write_instruction_address(f, PC_IA_FIRST));
    CHECK_EQ(fpu_state_id(f), 15);
    mc68k_fpu_destroy(f);
}

/* register-to-register の PC=1 の
 * NULL（CA=0、IA=1、PF=0）は、ダイアログの最初の応答としてだけ返す （Table 7-3）。68881
 * は読出しで、68882 は IA 書込み（先書きなら読出し）で完了し、以後は idle の NULL。 IDLE で
 * PC=1 の NULL を返し続けると、PF=1 の NULL を読むまで応答 CIR を読み続ける MPU が抜けられない
 * （設計書 §5.1） */
static void test_regreg_pc_not_held(void) {
    unsigned i;

    for (i = 0; i < 2; i++) {
        mc68k_fpu_config_t cfg = model_cfg(i);
        mc68k_fpu_t *f = armed(cfg);

        CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
        expect_null(f, 0, 1);
        if (cfg == MC68K_FPU_68882) {
            CHECK_EQ(fpu_state_id(f), 18);
            expect_null(f, 0, 1); /* IA 書込みまでは同じ PC=1 の NULL（CA も同じ） */
            CHECK(mc68k_fpu_write_instruction_address(f, PC_IA_FIRST));
        }
        CHECK_EQ(fpu_state_id(f), 2);
        expect_idle(f);
        expect_idle(f);
        mc68k_fpu_destroy(f);

        /* IA を先に書いてから読む: 読出しで完了し、以後は idle の NULL。続く IA 書込みは IDLE
         * の規則1 */
        f = armed(cfg);
        CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
        CHECK(mc68k_fpu_write_instruction_address(f, PC_IA_FIRST));
        expect_null(f, 0, 1);
        CHECK_EQ(fpu_state_id(f), 2);
        expect_idle(f);
        CHECK_EQ(mc68k_fpu_write_instruction_address(f, PC_IA_FIRST + 4),
                 cfg == MC68K_FPU_68881);
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

/* 応答値の保持: 残る事象と空になる事象（§5.1） */
static void test_hold_rules(void) {
    unsigned i;

    for (i = 0; i < 2; i++) {
        mc68k_fpu_config_t cfg = model_cfg(i);
        mc68k_fpu_t *f = mk(cfg);
        uint16_t fmt, rb;
        size_t bytes, k;
        uint32_t w;

        /* TF=0 の NULL（条件命令、predicate F）も保持する。繰り返し読んでも結果の
         * NULL（PF=0。idle の NULL は PF=1） */
        CHECK(mc68k_fpu_write_condition(f, 0x00));
        {
            mc68k_fpu_response_t r = rd(f);

            CHECK_EQ(r.condition_true, 0);
            CHECK_EQ(r.processing, 1u);
            r = rd(f);
            CHECK_EQ(r.condition_true, 0);
            CHECK_EQ(r.processing, 1u);
        }
        /* TF=1 の NULL（条件命令）。繰り返し読んでも TF=1 */
        CHECK(mc68k_fpu_write_condition(f, 0x0F));
        {
            mc68k_fpu_response_t r = rd(f);

            CHECK_EQ(r.condition_true, 1);
            r = rd(f);
            CHECK_EQ(r.condition_true, 1);
        }
        /* 残る: 不正 format の FRESTORE、ACK/ABORT、68881 の IA 書込み（FPIAR の更新だけ） */
        bytes = 1;
        mc68k_fpu_begin_restore(f, 0x0123, &rb, &bytes);
        CHECK_EQ(bytes, 0u); /* 不正 format は body を持たない */
        CHECK_EQ(rd(f).condition_true, 1);
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
        CHECK_EQ(rd(f).condition_true, 1);
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
        CHECK_EQ(rd(f).condition_true, 1);
        if (cfg == MC68K_FPU_68881) {
            CHECK(mc68k_fpu_write_instruction_address(f, 0x77));
            CHECK_EQ(rd(f).condition_true, 1);
        }
        /* 空になる: FSAVE（body を読み終えた後も TF=0 の NULL） */
        mc68k_fpu_begin_save(f, &fmt, &bytes);
        for (k = 0; k < bytes / 4; k++)
            CHECK(mc68k_fpu_read_operand(f, &w));
        CHECK_EQ(rd(f).condition_true, 0);
        /* 空になる: command 書込み（完了後の最終応答は TF=0。古い TF=1 が残らない） */
        CHECK(mc68k_fpu_write_condition(f, 0x0F));
        rd_kind(f, MC68K_FPU_RESPONSE_NULL); /* 条件命令の TF=1 の NULL */
        drive_regreg(f, CMD(0, 1, 0, OPM_FADD));
        CHECK_EQ(rd(f).condition_true, 0);
        /* 空になる: 有効 format の FRESTORE（null frame） */
        CHECK(mc68k_fpu_write_condition(f, 0x0F));
        rd_kind(f, MC68K_FPU_RESPONSE_NULL); /* 条件命令の TF=1 の NULL */
        bytes = 1;
        mc68k_fpu_begin_restore(f, 0x0000, &rb, &bytes);
        CHECK_EQ(bytes, 0u); /* null frame は body を持たない */
        CHECK_EQ(rd(f).condition_true, 0);
        /* 空になる: violation の ACK */
        CHECK(mc68k_fpu_write_condition(f, 0x0F));
        rd_kind(f, MC68K_FPU_RESPONSE_NULL);   /* 条件命令の TF=1 の NULL */
        CHECK(!mc68k_fpu_write_operand(f, 0)); /* violation */
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
        CHECK_EQ(rd(f).condition_true, 0);
        /* 空になる: RESET */
        CHECK(mc68k_fpu_write_condition(f, 0x0F));
        rd_kind(f, MC68K_FPU_RESPONSE_NULL); /* 条件命令の TF=1 の NULL */
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_RESET);
        CHECK_EQ(rd(f).condition_true, 0);
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

/* 条件命令の TF=1 の NULL を読んで S2 に入った後、command を書いた後の最終応答は TF=0（§4.2 の
 * S2） */
static void test_tf_cleared_by_command(void) {
    unsigned i;

    for (i = 0; i < 2; i++) {
        mc68k_fpu_t *f = mk(model_cfg(i));

        CHECK(mc68k_fpu_write_condition(f, 0x0F));
        rd_kind(f, MC68K_FPU_RESPONSE_NULL); /* 条件命令の TF=1 の NULL */
        CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
        expect_null(f, 0, 0);
        {
            mc68k_fpu_response_t r = rd(f);

            CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
            CHECK_EQ(r.condition_true, 0);
        }
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

/* 要求の無い命令の完了が保留算術例外を作ると S3 に入る。その後の応答は idle の NULL（PC=1 の
 * NULL は 返し続けない）。次の算術 command が保留を報告する */
static void test_pc_null_not_held_in_s3(void) {
    unsigned i;

    for (i = 0; i < 2; i++) {
        mc68k_fpu_t *f = mk(model_cfg(i));

        load_long(f, 0, 1);
        load_long(f, 1, 0);
        set_fpcr(f, FPCR_ENABLE(ENABLE_DZ));
        CHECK(
            mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FDIV))); /* FP0 / FP1 = 1/0: DZ */
        expect_null(f, 0, 1);
        if (model_cfg(i) == MC68K_FPU_68882)
            write_ia_ok(f);
        CHECK_EQ(fpu_state_id(f), 3);
        expect_idle(f);
        expect_idle(f);
        CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
        expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, 50, 0);
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

int main(void) {
    test_68881_ignores_pc();
    test_68882_pc_for_ea_in();
    test_68882_pc_for_ext_out();
    test_regreg_pc_not_held();
    test_hold_rules();
    test_tf_cleared_by_command();
    test_pc_null_not_held_in_s3();
    printf("test_fpu_model_pc: OK\n");
    return 0;
}
