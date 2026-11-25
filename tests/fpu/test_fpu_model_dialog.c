/*
 * test_fpu_model_dialog.c — コプロセッサモデルの命令ダイアログ。
 *
 * 入力・出力の B/W/L/S/D/X/P、FPCR/FPSR/FPIAR、FMOVEM（静的/動的 list、方向、list 0）、
 * FMOVECR、FSINCOS、条件命令の全 predicate を、両機種で同じ応答列・同じ状態になることとして
 * 検査する。期待値は [881UM] の形式とビット表現から手で求めた値で、production の定数や
 * 関数から取らない。各ダイアログは最後に最終応答が idle の NULL であること（`expect_idle`）を
 * assert し、途中で読み捨てる応答は種類を検査する（`expect_kind`）。`test_valid_ea_and_length`
 * と `test_pc_flag_rules` は最初の primitive の観測だけで ABORT するダイアログで、最後に idle
 * の NULL を 検査する。
 *
 * 落とす変異: 余分な NULL、primitive の欠落、B/W の配置（上位側）の取り違え、FMOVEM の走査順、
 * FSINCOS の同一宛先、packed の桁、k-factor（静的/動的）の取り違え、predicate の下位5bit 化。
 */
#include "fpu_model_util.h"

#define SE_ONE  0x3FFFu
#define SIG_ONE UINT64_C(0x8000000000000000)

/* 次の応答の種類を検査する（応答の中身は見ない）。読み捨てた応答が想定どおりの primitive
 * か確かめる */
#define expect_kind(f, want) check_response_kind(rd(f), (want))

/* MP_REGISTER は動的 list の FMOVEM で（§7.4.2.3）、FP_REGISTERS は静的 list を含む FMOVEM
 * 全般で （§7.4.2.4）CA が常に立つ（[881UM]、印字 p.7-15）。MP_REGISTER は CSV
 * に行が無いので、CA はここで検査する（FP_REGISTERS は transition の CSV も `ca`
 * を照合する）。FMOVE.P の動的 k-factor の MP_REGISTER も CA=1（Table 7-7 の
 * $8C0x・$CC0x、p.7-20〜21） */
static void check_response_kind(mc68k_fpu_response_t r, mc68k_fpu_response_kind_t want) {
    CHECK_EQ(r.kind, want);
    if (want == MC68K_FPU_RESPONSE_MP_REGISTER || want == MC68K_FPU_RESPONSE_FP_REGISTERS)
        CHECK(r.coprocessor_active);
}

static void check_fp(const mc68k_fpu_t *f, unsigned n, uint16_t se, uint64_t signif) {
    mc68k_fpu_regs_t r = regs(f);

    CHECK_EQ(r.fp[n].sign_exp, se);
    CHECK_EQ(r.fp[n].signif, signif);
}

static void test_input_formats(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    uint32_t w[3];

    /* B/W は longword の上位側（[881UM Figure 7-4]） */
    w[0] = 0xFD000000u; /* -3 */
    drive_ea_in(f, CMD(2, FMT_B, 0, OPM_FMOVE), w, 1, 1);
    check_fp(f, 0, 0xC000, UINT64_C(0xC000000000000000));
    w[0] = 0x01000000u; /* W: 0x0100 = 256 */
    drive_ea_in(f, CMD(2, FMT_W, 1, OPM_FMOVE), w, 1, 2);
    check_fp(f, 1, 0x3FFF + 8, SIG_ONE);
    w[0] = 5; /* L */
    drive_ea_in(f, CMD(2, FMT_L, 2, OPM_FMOVE), w, 1, 4);
    check_fp(f, 2, 0x4001, UINT64_C(0xA000000000000000));
    w[0] = 0x3FC00000u; /* S: 1.5 */
    drive_ea_in(f, CMD(2, FMT_S, 3, OPM_FMOVE), w, 1, 4);
    check_fp(f, 3, SE_ONE, UINT64_C(0xC000000000000000));
    w[0] = 0x3FE00000u; /* D: 0.5 */
    w[1] = 0;
    drive_ea_in(f, CMD(2, FMT_D, 4, OPM_FMOVE), w, 2, 8);
    check_fp(f, 4, 0x3FFE, SIG_ONE);
    w[0] = 0xC0000000u; /* X: -2.5 = -(1.25 * 2^1)。指数 0x4000 */
    w[1] = 0xA0000000u;
    w[2] = 0;
    drive_ea_in(f, CMD(2, FMT_X, 5, OPM_FMOVE), w, 3, 12);
    check_fp(f, 5, 0xC000, UINT64_C(0xA000000000000000));
    w[0] = 0x00010001u; /* P: 1.5E+1 = 15 */
    w[1] = 0x50000000u;
    w[2] = 0;
    drive_ea_in(f, CMD(2, FMT_P, 6, OPM_FMOVE), w, 3, 12);
    check_fp(f, 6, 0x4002, UINT64_C(0xF000000000000000));
    w[0] = 0x00000001u; /* P: 1.0E+0 */
    w[1] = 0;
    w[2] = 0;
    drive_ea_in(f, CMD(2, FMT_P, 7, OPM_FMOVE), w, 3, 12);
    check_fp(f, 7, SE_ONE, SIG_ONE);
    mc68k_fpu_destroy(f);
}

static void test_arith_and_regreg(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    uint32_t w[3] = {0x3FC00000u, 0, 0}; /* S: 1.5 */
    uint32_t w2 = 0x40100000u;           /* S: 2.25 */

    drive_ea_in(f, CMD(2, FMT_S, 0, OPM_FMOVE), w, 1, 4);
    drive_ea_in(f, CMD(2, FMT_S, 1, OPM_FMOVE), &w2, 1, 4);
    drive_regreg(f, CMD(0, 1, 0, OPM_FADD)); /* FADD FP1,FP0: 1.5 + 2.25 = 3.75 */
    check_fp(f, 0, 0x4000, UINT64_C(0xF000000000000000));
    check_fp(f, 1, 0x4000, UINT64_C(0x9000000000000000));
    drive_regreg(f, CMD(0, 1, 0, OPM_FSUB)); /* FP0 - FP1 = 3.75 - 2.25 = 1.5 */
    check_fp(f, 0, SE_ONE, UINT64_C(0xC000000000000000));
    drive_regreg(f, CMD(0, 1, 0, OPM_FMUL)); /* 1.5 * 2.25 = 3.375 = 1.6875 * 2 */
    check_fp(f, 0, 0x4000, UINT64_C(0xD800000000000000));
    drive_regreg(f, CMD(0, 0, 2, OPM_FMOVE)); /* FMOVE FP0,FP2 */
    check_fp(f, 2, 0x4000, UINT64_C(0xD800000000000000));
    drive_regreg(f, CMD(0, 2, 2, OPM_FNEG)); /* FNEG FP2,FP2 */
    check_fp(f, 2, 0xC000, UINT64_C(0xD800000000000000));
    drive_regreg(f, CMD(0, 2, 3, OPM_FABS)); /* FABS FP2,FP3 */
    check_fp(f, 3, 0x4000, UINT64_C(0xD800000000000000));
    /* FCMP・FTST は FPn を書かない */
    drive_regreg(f, CMD(0, 3, 0, OPM_FCMP));
    check_fp(f, 0, 0x4000, UINT64_C(0xD800000000000000));
    CHECK(regs(f).fpsr & FPSR_CC_Z); /* 3.375 と 3.375 は等しい */
    CHECK_EQ(regs(f).fpsr & (FPSR_CC_N | FPSR_CC_NAN), 0);
    /* FCMP FPm,FPn は FPn - FPm の cc を作る（向きの取り違えを検出する）。FP0=3.375、FP1=2.25
     */
    drive_regreg(f, CMD(0, 1, 0, OPM_FCMP)); /* 3.375 - 2.25 > 0 */
    CHECK_EQ(regs(f).fpsr & (FPSR_CC_N | FPSR_CC_Z | FPSR_CC_NAN), 0);
    drive_regreg(f, CMD(0, 0, 1, OPM_FCMP)); /* 2.25 - 3.375 < 0 */
    CHECK_EQ(regs(f).fpsr & (FPSR_CC_N | FPSR_CC_Z | FPSR_CC_NAN), FPSR_CC_N);
    check_fp(f, 0, 0x4000, UINT64_C(0xD800000000000000));
    check_fp(f, 1, 0x4000, UINT64_C(0x9000000000000000));
    mc68k_fpu_destroy(f);
}

static void test_output_formats(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    uint32_t out[3];
    uint32_t w[3] = {0x00010001u, 0x50000000u, 0};

    drive_ea_in(f, CMD(2, FMT_P, 0, OPM_FMOVE), w, 3, 12); /* FP0 = 15.0 */
    drive_ea_out(f, CMD(3, FMT_B, 0, 0), out, 1, 1);
    CHECK_EQ(out[0], 0x0F000000u);
    drive_ea_out(f, CMD(3, FMT_W, 0, 0), out, 1, 2);
    CHECK_EQ(out[0], 0x000F0000u);
    drive_ea_out(f, CMD(3, FMT_L, 0, 0), out, 1, 4);
    CHECK_EQ(out[0], 0x0000000Fu);
    drive_ea_out(f, CMD(3, FMT_S, 0, 0), out, 1, 4);
    CHECK_EQ(out[0], 0x41700000u);
    drive_ea_out(f, CMD(3, FMT_D, 0, 0), out, 2, 8);
    CHECK_EQ(out[0], 0x402E0000u);
    CHECK_EQ(out[1], 0);
    drive_ea_out(f, CMD(3, FMT_X, 0, 0), out, 3, 12);
    CHECK_EQ(out[0], 0x40020000u);
    CHECK_EQ(out[1], 0xF0000000u);
    CHECK_EQ(out[2], 0);
    /* X の下位 longword（仮数の下位32bit）が出力されること */
    w[0] = 0x40010000u;
    w[1] = 0xA0000000u;
    w[2] = 0x12345678u;
    drive_ea_in(f, CMD(2, FMT_X, 1, OPM_FMOVE), w, 3, 12);
    drive_ea_out(f, CMD(3, FMT_X, 1, 0), out, 3, 12);
    CHECK_EQ(out[0], 0x40010000u);
    CHECK_EQ(out[1], 0xA0000000u);
    CHECK_EQ(out[2], 0x12345678u);
    /* P の静的 k-factor +17: 1.5000000000000000E+1 */
    drive_ea_out(f, CMD(3, FMT_P, 0, 17), out, 3, 12);
    CHECK_EQ(out[0], 0x00010001u);
    CHECK_EQ(out[1], 0x50000000u);
    CHECK_EQ(out[2], 0);
    /* k-factor +1: 有効数字1桁。15.0 → 2E+1（既定の RN で、1.5 は同点なので偶数側の 2） */
    drive_ea_out(f, CMD(3, FMT_P, 0, 1), out, 3, 12);
    CHECK_EQ(out[0], 0x00010002u);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(out[2], 0);
    mc68k_fpu_destroy(f);
}

/* 動的 k-factor: MP_REGISTER の後に Dn を書く。後続の EA_TRANSFER は DR=1 */
static void test_dynamic_k_factor(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    mc68k_fpu_response_t r;
    uint32_t out[3];
    uint32_t w[3] = {0x00010001u, 0x50000000u, 0};

    drive_ea_in(f, CMD(2, FMT_P, 0, OPM_FMOVE), w, 3, 12);
    CHECK(mc68k_fpu_write_command_word(f, CMD(3, 7, 0, 5 << 4))); /* FMOVE.P FP0,<ea>{D5} */
    r = rd(f);
    check_response_kind(r, MC68K_FPU_RESPONSE_MP_REGISTER);
    CHECK_EQ(r.data_register, 5);
    CHECK_EQ(r.transfer_to_host, 0);
    CHECK_EQ(r.request_pc, 0);
    CHECK(mc68k_fpu_write_operand(f, 17)); /* Dn = +17 */
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.transfer_to_host, 1);
    CHECK_EQ(r.length, 12);
    CHECK(mc68k_fpu_read_operand(f, &out[0]));
    CHECK(mc68k_fpu_read_operand(f, &out[1]));
    CHECK(mc68k_fpu_read_operand(f, &out[2]));
    expect_idle(f);
    CHECK_EQ(out[0], 0x00010001u);
    CHECK_EQ(out[1], 0x50000000u);
    /* Dn の下位7bitだけを使う（p.4-66〜4-69）: 0xABCDEF11 → 下位7bit = 0x11 = 17 */
    CHECK(mc68k_fpu_write_command_word(f, CMD(3, 7, 0, 5 << 4)));
    expect_kind(f, MC68K_FPU_RESPONSE_MP_REGISTER);
    CHECK(mc68k_fpu_write_operand(f, 0xABCDEF11u));
    expect_kind(f, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK(mc68k_fpu_read_operand(f, &out[0]));
    CHECK(mc68k_fpu_read_operand(f, &out[1]));
    CHECK(mc68k_fpu_read_operand(f, &out[2]));
    expect_idle(f);
    CHECK_EQ(out[0], 0x00010001u);
    CHECK_EQ(out[1], 0x50000000u);
    CHECK_EQ(out[2], 0);
    /* k=+1（Dn=0xABCDEF01）: 有効数字1桁。15.0 → 2E+1（上の静的 k-factor +1 と同じ値） */
    CHECK(mc68k_fpu_write_command_word(f, CMD(3, 7, 0, 5 << 4)));
    expect_kind(f, MC68K_FPU_RESPONSE_MP_REGISTER);
    CHECK(mc68k_fpu_write_operand(f, 0xABCDEF01u));
    expect_kind(f, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK(mc68k_fpu_read_operand(f, &out[0]));
    CHECK(mc68k_fpu_read_operand(f, &out[1]));
    CHECK(mc68k_fpu_read_operand(f, &out[2]));
    expect_idle(f);
    CHECK_EQ(out[0], 0x00010002u);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(out[2], 0);
    mc68k_fpu_destroy(f);
}

#define MOVEM_IN(mode, low)  ((uint16_t)((6 << 13) | ((mode) << 11) | (low)))
#define MOVEM_OUT(mode, low) ((uint16_t)((7 << 13) | ((mode) << 11) | (low)))
#define MODE_PRE_STATIC      0
#define MODE_PRE_DYNAMIC     1
#define MODE_POST_STATIC     2
#define MODE_POST_DYNAMIC    3

static void movem_in_static(mc68k_fpu_t *f, uint8_t mask, const uint32_t *w, unsigned nwords) {
    mc68k_fpu_response_t r;
    uint16_t sel = 0;
    unsigned i;

    CHECK(mc68k_fpu_write_command_word(f, MOVEM_IN(MODE_POST_STATIC, mask)));
    r = rd(f);
    check_response_kind(r, MC68K_FPU_RESPONSE_FP_REGISTERS);
    CHECK_EQ(r.transfer_to_host, 0);
    CHECK_EQ(r.length, 12);
    CHECK_EQ(r.request_pc, 0);
    CHECK(mc68k_fpu_read_register_select(f, &sel));
    CHECK_EQ(sel, (uint16_t)(mask << 8));
    for (i = 0; i < nwords; i++)
        CHECK(mc68k_fpu_write_operand(f, w[i]));
    expect_idle(f);
}

static void test_movem(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    mc68k_fpu_response_t r;
    uint32_t in[6] = {0x3FFF0000u, 0x80000000u, 0, 0x40000000u, 0x80000000u, 0}; /* 1.0, 2.0 */
    uint32_t out[9];
    uint16_t sel;
    unsigned i;

    /* 静的 list（postincrement）で FP0, FP1 を入力。bit7=FP0 */
    movem_in_static(f, 0xC0, in, 6);
    check_fp(f, 0, SE_ONE, SIG_ONE);
    check_fp(f, 1, 0x4000, SIG_ONE);
    /* 同じ list で出力 */
    CHECK(mc68k_fpu_write_command_word(f, MOVEM_OUT(MODE_POST_STATIC, 0xC0)));
    r = rd(f);
    check_response_kind(r, MC68K_FPU_RESPONSE_FP_REGISTERS);
    CHECK_EQ(r.transfer_to_host, 1);
    CHECK(mc68k_fpu_read_register_select(f, &sel));
    CHECK_EQ(sel, 0xC000);
    for (i = 0; i < 6; i++)
        CHECK(mc68k_fpu_read_operand(f, &out[i]));
    CHECK_EQ(i, 6); /* ループが縮まない */
    expect_idle(f);
    CHECK_EQ(out[0], 0x3FFF0000u);
    CHECK_EQ(out[3], 0x40000000u);
    /* predecrement: MSB=FP7、走査順は FP1→FP0（mask bit1=FP1、bit0=FP0） */
    CHECK(mc68k_fpu_write_command_word(f, MOVEM_OUT(MODE_PRE_STATIC, 0x03)));
    expect_kind(f, MC68K_FPU_RESPONSE_FP_REGISTERS);
    CHECK(mc68k_fpu_read_register_select(f, &sel));
    CHECK_EQ(sel, 0x0300);
    for (i = 0; i < 6; i++)
        CHECK(mc68k_fpu_read_operand(f, &out[i]));
    CHECK_EQ(i, 6); /* ループが縮まない */
    expect_idle(f);
    CHECK_EQ(out[0], 0x40000000u); /* FP1 が先 */
    CHECK_EQ(out[3], 0x3FFF0000u);
    /* 入力の predecrement mode（mode 00。Table 4-18 Note 1）: MSB=FP7 で、走査順は FP1→FP0。
     * 先に受ける3 word が FP1、次が FP0 */
    {
        uint32_t pre_in[6] = {0x3FFF0000u, 0x80000000u, 0,
                              0x40000000u, 0x80000000u, 0}; /* 1.0、2.0 */

        CHECK(mc68k_fpu_write_command_word(f, MOVEM_IN(MODE_PRE_STATIC, 0x03)));
        expect_kind(f, MC68K_FPU_RESPONSE_FP_REGISTERS);
        CHECK(mc68k_fpu_read_register_select(f, &sel));
        CHECK_EQ(sel, 0x0300);
        for (i = 0; i < 6; i++)
            CHECK(mc68k_fpu_write_operand(f, pre_in[i]));
        CHECK_EQ(i, 6); /* ループが縮まない */
        expect_idle(f);
        check_fp(f, 1, SE_ONE, SIG_ONE);
        check_fp(f, 0, 0x4000, SIG_ONE);
        /* 元の値（FP0=1.0、FP1=2.0）へ戻す: FP1 に 2.0、FP0 に 1.0 */
        CHECK(mc68k_fpu_write_command_word(f, MOVEM_IN(MODE_PRE_STATIC, 0x03)));
        expect_kind(f, MC68K_FPU_RESPONSE_FP_REGISTERS);
        CHECK(mc68k_fpu_read_register_select(f, &sel));
        for (i = 0; i < 6; i++)
            CHECK(mc68k_fpu_write_operand(f, pre_in[(i + 3) % 6]));
        CHECK_EQ(i, 6); /* ループが縮まない */
        expect_idle(f);
    }
    /* 動的 list（postincrement、D3）: MP_REGISTER → Dn → FP_REGISTERS → register select */
    CHECK(mc68k_fpu_write_command_word(f, MOVEM_OUT(MODE_POST_DYNAMIC, 3 << 4)));
    r = rd(f);
    check_response_kind(r, MC68K_FPU_RESPONSE_MP_REGISTER);
    CHECK_EQ(r.data_register, 3);
    CHECK(mc68k_fpu_write_operand(f, 0x40)); /* FP1 だけ（bit6） */
    r = rd(f);
    check_response_kind(r, MC68K_FPU_RESPONSE_FP_REGISTERS);
    CHECK_EQ(r.transfer_to_host, 1);
    CHECK(mc68k_fpu_read_register_select(f, &sel));
    CHECK_EQ(sel, 0x4000);
    for (i = 0; i < 3; i++)
        CHECK(mc68k_fpu_read_operand(f, &out[i]));
    CHECK_EQ(i, 3); /* ループが縮まない */
    expect_idle(f);
    CHECK_EQ(out[0], 0x40000000u);
    /* list 0: 転送無し。register select 読出しで完了し、次の応答が idle の NULL */
    CHECK(mc68k_fpu_write_command_word(f, MOVEM_OUT(MODE_POST_STATIC, 0x00)));
    r = rd(f);
    check_response_kind(r, MC68K_FPU_RESPONSE_FP_REGISTERS);
    CHECK(mc68k_fpu_read_register_select(f, &sel));
    CHECK_EQ(sel, 0);
    expect_idle(f);
    CHECK(mc68k_fpu_write_command_word(f, MOVEM_IN(MODE_POST_DYNAMIC, 0 << 4)));
    expect_kind(f, MC68K_FPU_RESPONSE_MP_REGISTER);
    CHECK(mc68k_fpu_write_operand(f, 0)); /* Dn=0: list 0 */
    r = rd(f);
    check_response_kind(r, MC68K_FPU_RESPONSE_FP_REGISTERS);
    CHECK(mc68k_fpu_read_register_select(f, &sel));
    CHECK_EQ(sel, 0);
    expect_idle(f);
    mc68k_fpu_destroy(f);
}

static void test_control_moves(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    mc68k_fpu_response_t r;
    uint32_t in[3] = {0x00001090u, 0x0A123458u, 0xDEADBEEFu};
    uint32_t out[3];
    unsigned i;

    /* FPCR: ENABLE=OVFL（0x10<<8）、PREC=倍精度（bit7-6=10）、RND=RZ（bit5-4=01） */
    drive_ctrl_in(f, CTL_FPCR, &in[0], 1);
    CHECK_EQ(regs(f).fpcr, 0x1090u);
    drive_ctrl_in(f, CTL_FPSR, &in[1], 1);
    CHECK_EQ(regs(f).fpsr, 0x0A123458u);
    drive_ctrl_in(f, CTL_FPIAR, &in[2], 1);
    CHECK_EQ(regs(f).fpiar, 0xDEADBEEFu);
    /* 出力（FPIAR 単独は alterable、FPCR/FPSR 単独は data alterable。Table 7-5） */
    CHECK(mc68k_fpu_write_command_word(f, CMD(5, CTL_FPCR, 0, 0)));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.transfer_to_host, 1);
    CHECK_EQ(r.length, 4);
    CHECK_EQ(r.valid_ea, 1);
    CHECK(mc68k_fpu_read_operand(f, &out[0]));
    expect_idle(f);
    CHECK_EQ(out[0], 0x1090u);
    CHECK(mc68k_fpu_write_command_word(f, CMD(5, CTL_FPIAR, 0, 0)));
    r = rd(f);
    CHECK_EQ(r.valid_ea, 3);
    CHECK(mc68k_fpu_read_operand(f, &out[0]));
    expect_idle(f);
    CHECK_EQ(out[0], 0xDEADBEEFu);
    /* 3本同時: 転送順は FPCR、FPSR、FPIAR */
    CHECK(mc68k_fpu_write_command_word(f, CMD(5, CTL_FPCR | CTL_FPSR | CTL_FPIAR, 0, 0)));
    r = rd(f);
    CHECK_EQ(r.length, 12);
    CHECK_EQ(r.valid_ea, 2);
    for (i = 0; i < 3; i++)
        CHECK(mc68k_fpu_read_operand(f, &out[i]));
    CHECK_EQ(i, 3); /* ループが縮まない */
    expect_idle(f);
    CHECK_EQ(out[0], 0x1090u);
    CHECK_EQ(out[1], 0x0A123458u);
    CHECK_EQ(out[2], 0xDEADBEEFu);
    {
        uint32_t in2[2] = {0x00000000u,
                           0x11111111u}; /* FPCR=0、FPIAR=0x11111111（FPCR,FPIAR の2本） */
        CHECK(mc68k_fpu_write_command_word(f, CMD(4, CTL_FPCR | CTL_FPIAR, 0, 0)));
        r = rd(f);
        CHECK_EQ(r.length, 8);
        CHECK_EQ(r.valid_ea, 6);
        CHECK(mc68k_fpu_write_operand(f, in2[0]));
        CHECK(mc68k_fpu_write_operand(f, in2[1]));
        expect_idle(f);
        CHECK_EQ(regs(f).fpcr, 0);
        CHECK_EQ(regs(f).fpiar, 0x11111111u);
        CHECK_EQ(regs(f).fpsr, 0x0A123458u); /* 触れていない */
    }
    mc68k_fpu_destroy(f);
}

static void test_movecr_and_sincos(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);

    CHECK(mc68k_fpu_write_command_word(f, CMD(2, 7, 0, 0x00))); /* FMOVECR #$00,FP0: π */
    expect_null(f, 0, 0);
    expect_idle(f);
    check_fp(f, 0, 0x4000, UINT64_C(0xC90FDAA22168C235));
    CHECK(mc68k_fpu_write_command_word(f, CMD(2, 7, 1, 0x0F))); /* FMOVECR #$0F,FP1: 0.0 */
    expect_null(f, 0, 0);
    expect_idle(f);
    check_fp(f, 1, 0, 0);
    /* FSINCOS FP1,FP2:FP3（sin→FP3、cos→FP2）。入力 0: sin=0、cos=1 */
    drive_regreg(f, CMD(0, 1, 3, OPM_FSINCOS | 2));
    check_fp(f, 3, 0, 0);
    check_fp(f, 2, SE_ONE, SIG_ONE);
    /* 同一宛先 FP4:FP4 は cos を先に書き sin で上書きする（sin=0 が残る） */
    load_long(f, 4, 7);
    drive_regreg(f, CMD(0, 1, 4, OPM_FSINCOS | 4));
    check_fp(f, 4, 0, 0);
    mc68k_fpu_destroy(f);
}

/* 設計書 §11 の「各 predicate の全値」。真偽は IEEE の方程式から手で求めた表（cc ごと） */
static const struct {
    uint32_t cc;
    uint16_t truth; /* bit p が predicate (p & 15) の結果 */
} cc_truth[] = {
    {0, 0xCCCC}, /* N=Z=NAN=0 */
    {FPSR_CC_N, 0xF0F0},
    {FPSR_CC_Z, 0xAAAA},
    {FPSR_CC_NAN, 0xFF00},
    {FPSR_CC_I, 0xCCCC},
    {FPSR_CC_N | FPSR_CC_I, 0xF0F0},
    {FPSR_CC_N | FPSR_CC_Z, 0xAAAA},
    /* NAN が立つと N・Z の値によらず UN 系（8〜D と T）が真。Z=1 なら EQ・OGE・OLE が真、NE
       が偽 */
    {FPSR_CC_N | FPSR_CC_NAN, 0xFF00},
    {FPSR_CC_Z | FPSR_CC_NAN, 0xBF2A},
    {FPSR_CC_N | FPSR_CC_Z | FPSR_CC_NAN, 0xBF2A},
};

static void test_predicates(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    size_t i;
    unsigned p;
    unsigned checked = 0;

    for (i = 0; i < sizeof(cc_truth) / sizeof(cc_truth[0]); i++) {
        for (p = 0; p < 64; p++) {
            mc68k_fpu_response_t r;
            uint32_t out;
            int expect_true = (cc_truth[i].truth >> (p & 15)) & 1;
            /* $20〜$3F は下位5bit と同じ。bit4=1 が IEEE nonaware（BSUN の対象） */
            int signaling = (p & 0x10) != 0;
            int nan = (cc_truth[i].cc & FPSR_CC_NAN) != 0;

            set_fpsr(f, cc_truth[i].cc);
            CHECK(mc68k_fpu_write_condition(f, (uint8_t)p));
            r = rd(f);
            CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
            CHECK_EQ(r.coprocessor_active, 0);
            CHECK_EQ(r.request_pc, 0);
            CHECK_EQ(r.condition_true, expect_true);
            /* BSUN: signaling 型で NAN のときだけ EXC.BSUN が立つ（ENABLE は0なので trap
             * しない） */
            CHECK(mc68k_fpu_write_command_word(f, CMD(5, CTL_FPSR, 0, 0)));
            expect_kind(f, MC68K_FPU_RESPONSE_EA_TRANSFER);
            CHECK(mc68k_fpu_read_operand(f, &out));
            expect_idle(f);
            CHECK_EQ((out & FPSR_EXC(ENABLE_BSUN)) != 0, signaling && nan);
            CHECK_EQ(out & (FPSR_CC_N | FPSR_CC_Z | FPSR_CC_I | FPSR_CC_NAN), cc_truth[i].cc);
            checked++;
        }
        CHECK_EQ(p, 64); /* ループが縮まない */
    }
    CHECK_EQ(checked, 10u * 64u);
    mc68k_fpu_destroy(f);
}

/* [881UM Table 7-5] の length と valid EA（入力: data か memory、出力: data alterable か memory
 * alterable） */
static void test_valid_ea_and_length(mc68k_fpu_config_t cfg) {
    static const struct {
        unsigned fmt, length, in_ea, out_ea;
    } fmts[] = {
        {FMT_B, 1, 5, 1}, {FMT_W, 2, 5, 1},  {FMT_L, 4, 5, 1},  {FMT_S, 4, 5, 1},
        {FMT_D, 8, 6, 2}, {FMT_X, 12, 6, 2}, {FMT_P, 12, 6, 2},
    };
    static const struct {
        unsigned select, length, in_ea, out_ea;
    } ctrls[] = {
        {CTL_FPIAR, 4, 7, 3},
        {CTL_FPSR, 4, 5, 1},
        {CTL_FPCR, 4, 5, 1},
        {CTL_FPSR | CTL_FPIAR, 8, 6, 2},
        {CTL_FPCR | CTL_FPIAR, 8, 6, 2},
        {CTL_FPCR | CTL_FPSR, 8, 6, 2},
        {CTL_FPCR | CTL_FPSR | CTL_FPIAR, 12, 6, 2},
    };
    mc68k_fpu_t *f = mk(cfg);
    mc68k_fpu_response_t r;
    size_t i;

    CHECK_EQ(sizeof(fmts) / sizeof(fmts[0]),
             7u); /* fmts の要素数。表の行の削除・ループの縮小を見逃さない */
    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        CHECK(mc68k_fpu_write_command_word(f, CMD(2, fmts[i].fmt, 0, OPM_FMOVE)));
        r = rd(f);
        CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
        CHECK_EQ(r.length, fmts[i].length);
        CHECK_EQ(r.valid_ea, fmts[i].in_ea);
        CHECK_EQ(r.transfer_to_host, 0);
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
        CHECK(mc68k_fpu_write_command_word(f, CMD(3, fmts[i].fmt, 0, 0)));
        expect_kind(f, MC68K_FPU_RESPONSE_NULL); /* 変換待ちの NULL */
        r = rd(f);
        CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
        CHECK_EQ(r.length, fmts[i].length);
        CHECK_EQ(r.valid_ea, fmts[i].out_ea);
        CHECK_EQ(r.transfer_to_host, 1);
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    }
    CHECK_EQ(i, 7u); /* ループが縮まない */
    CHECK_EQ(sizeof(ctrls) / sizeof(ctrls[0]),
             7u); /* ctrls の要素数。表の行の削除・ループの縮小を見逃さない */
    for (i = 0; i < sizeof(ctrls) / sizeof(ctrls[0]); i++) {
        CHECK(mc68k_fpu_write_command_word(f, CMD(4, ctrls[i].select, 0, 0)));
        r = rd(f);
        CHECK_EQ(r.length, ctrls[i].length);
        CHECK_EQ(r.valid_ea, ctrls[i].in_ea);
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
        CHECK(mc68k_fpu_write_command_word(f, CMD(5, ctrls[i].select, 0, 0)));
        r = rd(f);
        CHECK_EQ(r.length, ctrls[i].length);
        CHECK_EQ(r.valid_ea, ctrls[i].out_ea);
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    }
    CHECK_EQ(i, 7u); /* ループが縮まない */
    expect_idle(f);
    mc68k_fpu_destroy(f);
}

/* 最初の primitive の PC=1 は、算術・FMOVE・FMOVECR で ENABLE の BSUN
 * 以外が非0のとき（§5.2、§6）。 BSUN だけの enable、FMOVEM、control move では PC=0 */
static void test_pc_flag_rules(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    mc68k_fpu_response_t r;
    unsigned bit;

    set_fpcr(f, FPCR_ENABLE(ENABLE_BSUN));
    CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
    expect_null(f, 0, 0);
    CHECK(mc68k_fpu_write_command_word(f, CMD(2, FMT_L, 0, OPM_FMOVE)));
    r = rd(f);
    CHECK_EQ(r.request_pc, 0);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    for (bit = 0; bit < 7; bit++) { /* BSUN 以外の ENABLE ビットのどれでも PC=1 */
        set_fpcr(f, FPCR_ENABLE(1u << bit));
        CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
        r = rd(f);
        CHECK_EQ(r.request_pc, 1);
        mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    }
    CHECK_EQ(bit, 7u); /* ENABLE の BSUN 以外の7ビットを全て回る（ループが縮まない） */
    set_fpcr(f, FPCR_ENABLE(ENABLE_DZ));
    CHECK(mc68k_fpu_write_command_word(f, CMD(2, 7, 0, 0x0F))); /* FMOVECR */
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
    CHECK_EQ(r.request_pc, 1);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    CHECK(mc68k_fpu_write_command_word(f, 0xF080)); /* FMOVEM */
    r = rd(f);
    CHECK_EQ(r.request_pc, 0);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    CHECK(mc68k_fpu_write_command_word(f, CMD(5, CTL_FPCR, 0, 0))); /* control move */
    r = rd(f);
    CHECK_EQ(r.request_pc, 0);
    mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
    expect_idle(f);
    mc68k_fpu_destroy(f);
}

/* FPCR の PREC=11（予約）を書くと、読み返しの PREC は拡張精度（00）になる（設計書 §6） */
static void test_fpcr_reserved_prec(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);

    set_fpcr(f, 0x00C0u | 0x0030u);
    CHECK_EQ(regs(f).fpcr, 0x0030u);
    mc68k_fpu_destroy(f);
}

int main(void) {
    unsigned i;

    for (i = 0; i < 2; i++) {
        test_input_formats(model_cfg(i));
        test_arith_and_regreg(model_cfg(i));
        test_output_formats(model_cfg(i));
        test_dynamic_k_factor(model_cfg(i));
        test_movem(model_cfg(i));
        test_control_moves(model_cfg(i));
        test_movecr_and_sincos(model_cfg(i));
        test_predicates(model_cfg(i));
        test_valid_ea_and_length(model_cfg(i));
        test_pc_flag_rules(model_cfg(i));
        test_fpcr_reserved_prec(model_cfg(i));
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
    printf("test_fpu_model_dialog: OK\n");
    return 0;
}
