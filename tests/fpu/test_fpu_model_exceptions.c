/*
 * test_fpu_model_exceptions.c — 保留算術例外・例外優先順位・trap 時の書戻し（、設計書
 * §8）。
 *
 * 設計書 §11 の Phase C に属する規則だが、状態機械（表 A の S3・S4・S9）がこれに依存するため
 * Phase B の実装に含めた。ここでは次を手で求めた期待値で検査する。
 *   - §8.1 の優先順位とベクタ（EXC×ENABLE の全 65536 組合せ）、OVFL 起因の INEX2 候補
 *   - §8.3 の書戻し（DZ/OPERR/SNAN は FPn 不変、OVFL/INEX2 は丸め済み既定結果を書く）
 *   - 外部宛て FMOVE の転送後の TAKE_MID_EXCEPTION
 *   - EXC byte のクリア（算術は開始時にクリア、FMOVEM と control move はクリアしない）、商 byte
 *
 * 落とす変異: 優先順位の隣接2項の入替え、OVFL 起因の INEX 候補を省く、UNFL にも INEX
 * 候補を加える、 §8.3 の表の1行の書戻しの反転（INEX2・UNFL・INEX1 を抑止側へ）、外部宛て FMOVE
 * で転送前に TAKE を返す、FMOVEM で EXC をクリアする、 命令開始ごとに商 byte をクリアする。
 */
#include "fpu/fpu_model.h"
#include "fpu_model_setup.h"

#define EXC_BSUN  0x80u
#define EXC_SNAN  0x40u
#define EXC_OPERR 0x20u
#define EXC_OVFL  0x10u
#define EXC_UNFL  0x08u
#define EXC_DZ    0x04u
#define EXC_INEX2 0x02u
#define EXC_INEX1 0x01u

/* 任意のダイアログを完了まで駆動する（PC=1 の primitive があれば IA を書く）。out（容量 out_cap
 * word）に 読出し operand を入れる。容量を超える読出しは異常終了する */
/* command word から決まる最初の応答の種類（[881UM §4.7] の class 別: reg-reg・FMOVECR は
 * NULL、<ea> と control move は EA 転送、FMOVEM は静的 list が FP レジスタ、動的 list が MP
 * レジスタ） */
static mc68k_fpu_response_kind_t first_response_kind(uint16_t cmd) {
    unsigned cls = cmd >> 13;

    if (cls == 0 || (cls == 2 && ((cmd >> 10) & 7) == 7))
        return MC68K_FPU_RESPONSE_NULL;
    if (cls == 6 || cls == 7)
        return (cmd & 0x0800) ? MC68K_FPU_RESPONSE_MP_REGISTER
                              : MC68K_FPU_RESPONSE_FP_REGISTERS;
    return MC68K_FPU_RESPONSE_EA_TRANSFER;
}

static unsigned drive_any(mc68k_fpu_t *f, uint16_t cmd, const uint32_t *words, unsigned nwords,
                          uint32_t *out, unsigned out_cap) {
    unsigned wi = 0, oi = 0, guard;

    CHECK(mc68k_fpu_write_command_word(f, cmd));
    for (guard = 0; guard < 64; guard++) {
        int s = fpu_state_id(f);
        uint16_t sel;

        if (s == 13) {
            rd_kind(f, MC68K_FPU_RESPONSE_NULL);
        } else if (s == 12 || s == 14) {
            /* class の応答、または PC 要求などの NULL。TAKE や別 class の応答は不可 */
            mc68k_fpu_response_t r = rd(f);

            CHECK(r.kind == MC68K_FPU_RESPONSE_NULL || r.kind == first_response_kind(cmd));
        } else if (s == 15) {
            CHECK(wi < nwords);
            CHECK(mc68k_fpu_write_operand(f, words[wi++]));
        } else if (s == 16) {
            CHECK(oi < out_cap);
            CHECK(mc68k_fpu_read_operand(f, &out[oi++]));
        } else if (s == 17) {
            CHECK(mc68k_fpu_read_register_select(f, &sel));
        } else if (s == 18) {
            rd_kind(f, MC68K_FPU_RESPONSE_NULL);
            write_ia_ok(f);
        } else {
            /* S12〜S18 以外へ出た（IDLE・UNSTARTED のほか、S10・S11・S19・S20
             * も受理する）。渡した入力 word は 全て使い切っている */
            CHECK_EQ(wi, nwords);
            return oi; /* 読み出した operand の word 数 */
        }
    }
    CHECK(0);
    return 0;
}

static uint32_t read_fpsr(mc68k_fpu_t *f) {
    uint32_t w = 0;

    CHECK_EQ(drive_any(f, CMD(5, CTL_FPSR, 0, 0), NULL, 0, &w, 1), 1u); /* FPSR を1 word 読む */
    return w;
}

/* §8.1: 優先順位 BSUN > SNAN > OPERR > OVFL > UNFL > DZ > INEX2 > INEX1 とベクタ。
 * INEX は OVFL 起因なら EXC.INEX2 が無くても ENABLE.INEX2 で候補（[881UM §6.1.10]）。0=候補なし
 */
static unsigned expected_vector(unsigned exc, unsigned en) {
    static const struct {
        unsigned bit;
        unsigned vector;
    } order[] = {{EXC_BSUN, 48}, {EXC_SNAN, 54}, {EXC_OPERR, 52}, {EXC_OVFL, 53},
                 {EXC_UNFL, 51}, {EXC_DZ, 50},   {EXC_INEX2, 49}, {EXC_INEX1, 49}};
    unsigned cand = exc & en;
    size_t i;

    if ((exc & EXC_OVFL) && (en & EXC_INEX2))
        cand |= EXC_INEX2;
    CHECK_EQ(sizeof(order) / sizeof(order[0]),
             8u); /* order の要素数。表の行の削除・ループの縮小を見逃さない */
    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (cand & order[i].bit)
            return order[i].vector;
    CHECK_EQ(i, 8u); /* ループが縮まない */
    return 0;
}

static void test_priority_and_vector(mc68k_fpu_config_t cfg) {
    unsigned exc, en, checked = 0;

    for (exc = 0; exc < 256; exc++) {
        for (en = 0; en < 256; en++) {
            mc68k_fpu_t *f = to_s3(cfg); /* arith=PENDING */
            unsigned want;
            mc68k_fpu_response_t r;

            set_fpsr(f, FPSR_EXC(exc));
            set_fpcr(f, FPCR_ENABLE(en));
            want = expected_vector(exc, en);
            CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
            r = rd(f);
            if (want) {
                CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION);
                CHECK_EQ(r.vector, want);
                CHECK_EQ(r.request_pc, 0);
            } else {
                /* 候補が無ければ保留は報告せず解消し、通常の最初の primitive（NULL） */
                CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
            }
            mc68k_fpu_destroy(f);
            checked++;
        }
        CHECK_EQ(en, 256); /* ループが縮まない */
    }
    CHECK_EQ(exc, 256); /* ループが縮まない */
    CHECK_EQ(checked, 65536u);
}

static void check_fp(const mc68k_fpu_t *f, unsigned n, uint16_t se, uint64_t signif) {
    mc68k_fpu_regs_t r = regs(f);

    CHECK_EQ(r.fp[n].sign_exp, se);
    CHECK_EQ(r.fp[n].signif, signif);
}

static mc68k_fpu_response_t next_arith_take(mc68k_fpu_t *f) {
    CHECK(mc68k_fpu_write_command_word(f, CMD(0, 1, 0, OPM_FADD)));
    return rd(f);
}

/* §8.3 の書戻し。trap enabled のとき、SNAN/OPERR/DZ は FPn 不変、OVFL/UNFL/INEX2/INEX1
 * は既定結果 （BSUN は算術では立たないので対象外） */
static void test_writeback(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f;
    mc68k_fpu_response_t r;
    uint32_t w[3];

    /* DZ: 1 ÷ 0。FP0 は不変 */
    f = to_s3(cfg);
    check_fp(f, 0, 0x3FFF, UINT64_C(0x8000000000000000));
    CHECK(read_fpsr(f) & FPSR_EXC(EXC_DZ));
    r = next_arith_take(f);
    CHECK_EQ(r.vector, 50);
    mc68k_fpu_destroy(f);

    /* OPERR: FSQRT(-4) の結果は書かない。FP1 は 2.0 のまま */
    f = mk(cfg);
    load_long(f, 0, -4);
    load_long(f, 1, 2);
    set_fpcr(f, FPCR_ENABLE(ENABLE_OPERR));
    CHECK_EQ(drive_any(f, CMD(0, 0, 1, OPM_FSQRT), NULL, 0, w, 0),
             0u); /* 読出し operand は無い */
    check_fp(f, 1, 0x4000, UINT64_C(0x8000000000000000));
    CHECK(read_fpsr(f) & FPSR_EXC(EXC_OPERR));
    r = next_arith_take(f);
    CHECK_EQ(r.vector, 52);
    mc68k_fpu_destroy(f);

    /* SNAN: FMOVE.S #sNaN,FP2 は FP2 を書かない（FP2=5 のまま） */
    f = mk(cfg);
    load_long(f, 2, 5);
    set_fpcr(f, FPCR_ENABLE(ENABLE_SNAN));
    w[0] = 0x7F800001u;
    drive_any(f, CMD(2, FMT_S, 2, OPM_FMOVE), w, 1, NULL, 0);
    check_fp(f, 2, 0x4001, UINT64_C(0xA000000000000000));
    r = next_arith_take(f);
    CHECK_EQ(r.vector, 54);
    mc68k_fpu_destroy(f);

    /* INEX2: 1 ÷ 3 の丸め済み結果（0x3FFD, 0xAAAA…AB）を書き戻し、報告は次の命令（vector 49）
     */
    f = mk(cfg);
    load_long(f, 0, 1);
    load_long(f, 1, 3);
    set_fpcr(f, FPCR_ENABLE(ENABLE_INEX2));
    CHECK_EQ(drive_any(f, CMD(0, 1, 0, OPM_FDIV), NULL, 0, w, 0),
             0u); /* 読出し operand は無い */
    check_fp(f, 0, 0x3FFD, UINT64_C(0xAAAAAAAAAAAAAAAB));
    r = next_arith_take(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION);
    CHECK_EQ(r.vector, 49);
    mc68k_fpu_destroy(f);

    /* OVFL（INEX2 も立つ）: 既定結果（最近接なら ∞）を書き戻し、優先順位で vector 53 */
    f = mk(cfg);
    load_x(f, 0, 0x7FFE, UINT64_C(0x8000000000000000));
    load_long(f, 1, 2);
    set_fpcr(f, FPCR_ENABLE(ENABLE_OVFL | ENABLE_INEX2));
    CHECK_EQ(drive_any(f, CMD(0, 1, 0, OPM_FMUL), NULL, 0, w, 0),
             0u); /* 読出し operand は無い */
    {
        mc68k_fpu_regs_t g = regs(f);

        CHECK_EQ(g.fp[0].sign_exp, 0x7FFF);
        CHECK_EQ(g.fp[0].signif & UINT64_C(0x7FFFFFFFFFFFFFFF), 0); /* ∞（仮数の小数部が0） */
        CHECK_EQ(read_fpsr(f) & FPSR_EXC(EXC_OVFL | EXC_INEX2), FPSR_EXC(EXC_OVFL | EXC_INEX2));
    }
    r = next_arith_take(f);
    CHECK_EQ(r.vector, 53);
    mc68k_fpu_destroy(f);

    /* UNFL（INEX2 も立つ）: 2^-16382 同士の積は 0 へ
     * underflow。丸め済み既定結果（+0）を書き戻し、vector 51 */
    f = mk(cfg);
    load_x(f, 0, 0x0001, UINT64_C(0x8000000000000000));
    load_x(f, 1, 0x0001, UINT64_C(0x8000000000000000));
    set_fpcr(f, FPCR_ENABLE(ENABLE_UNFL));
    CHECK_EQ(drive_any(f, CMD(0, 1, 0, OPM_FMUL), NULL, 0, w, 0),
             0u); /* 読出し operand は無い */
    check_fp(f, 0, 0, 0);
    CHECK_EQ(read_fpsr(f) & FPSR_EXC(EXC_UNFL), FPSR_EXC(EXC_UNFL));
    r = next_arith_take(f);
    CHECK_EQ(r.vector, 51);
    mc68k_fpu_destroy(f);

    /* INEX1（FMOVE.P #1.1,FP0）: trap disabled と同じ結果（1.1
     * を最近接へ丸めた拡張精度値）を書き戻し、 報告は次の命令（vector 49） */
    f = mk(cfg);
    set_fpcr(f, FPCR_ENABLE(ENABLE_INEX1));
    w[0] = 0x00000001u; /* 符号・指数 0、整数部の桁 1 */
    w[1] = 0x10000000u; /* 小数第1位 1 */
    w[2] = 0;
    drive_any(f, CMD(2, FMT_P, 0, OPM_FMOVE), w, 3, NULL, 0);
    check_fp(f, 0, 0x3FFF, UINT64_C(0x8CCCCCCCCCCCCCCD));
    CHECK_EQ(read_fpsr(f) & FPSR_EXC(EXC_INEX1), FPSR_EXC(EXC_INEX1));
    r = next_arith_take(f);
    CHECK_EQ(r.vector, 49);
    mc68k_fpu_destroy(f);

    /* trap disabled: 同じ OPERR でも結果（既定 NaN）を書き、保留は作らない */
    f = mk(cfg);
    load_long(f, 0, -4);
    load_long(f, 1, 2);
    CHECK_EQ(drive_any(f, CMD(0, 0, 1, OPM_FSQRT), NULL, 0, w, 0),
             0u); /* 読出し operand は無い */
    CHECK_EQ(regs(f).fp[1].sign_exp & 0x7FFF, 0x7FFF);
    CHECK(read_fpsr(f) & FPSR_EXC(EXC_OPERR));
    CHECK_EQ(fpu_state_id(f), 2);
    mc68k_fpu_destroy(f);
}

/* 外部宛て FMOVE: enabled exception があれば、結果を転送し終えた後で TAKE_MID_EXCEPTION */
static void test_external_move_mid_exception(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    uint32_t out[3];
    uint32_t w = 0x40600000u; /* S: 3.5 */
    mc68k_fpu_response_t r;

    drive_any(f, CMD(2, FMT_S, 0, OPM_FMOVE), &w, 1, NULL, 0); /* FP0 = 3.5 */
    set_fpcr(f, FPCR_ENABLE(ENABLE_INEX2));
    CHECK(mc68k_fpu_write_command_word(f, CMD(3, FMT_L, 0, 0)));
    expect_null(f, 1, 1);
    if (cfg == MC68K_FPU_68882)
        write_ia_ok(f);
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.transfer_to_host, 1);
    /* 転送前に TAKE は返らない。結果（最近接偶数: 3.5 → 4）を転送し終えてから TAKE_MID */
    CHECK_EQ(fpu_state_id(f), 16);
    CHECK(mc68k_fpu_read_operand(f, &out[0]));
    CHECK_EQ(out[0], 4);
    expect_take(f, MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION, 49, 0);
    check_fp(f, 0, 0x4000, UINT64_C(0xE000000000000000)); /* FP0 は不変 */
    mc68k_fpu_destroy(f);
    /* trap disabled: TAKE_MID は返らず、結果の転送だけ */
    f = mk(cfg);
    drive_any(f, CMD(2, FMT_S, 0, OPM_FMOVE), &w, 1, NULL, 0);
    CHECK_EQ(drive_any(f, CMD(3, FMT_L, 0, 0), NULL, 0, out, 3),
             1u); /* 1 word 転送。0 だと下の out[0] は古い値 */
    CHECK_EQ(out[0], 4);
    expect_idle(f);
    mc68k_fpu_destroy(f);
}

/* EXC byte: 算術・FMOVE・条件命令の開始でクリア。FMOVEM と control move はクリアしない */
static void test_exc_clear(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    uint32_t w3[3] = {0x3FFF0000u, 0x80000000u, 0};
    uint32_t out[3];
    uint16_t sel;
    unsigned i;

    set_fpsr(f, FPSR_EXC(EXC_SNAN | EXC_OVFL)); /* EXC に値を置く */
    /* control move（FPCR 書込み、FPSR 読出し）はクリアしない */
    set_fpcr(f, 0);
    CHECK_EQ(read_fpsr(f) & FPSR_EXC(0xFF), FPSR_EXC(EXC_SNAN | EXC_OVFL));
    /* FMOVEM（入力・出力）はクリアしない */
    CHECK(mc68k_fpu_write_command_word(f, 0xD000 | 0x80));
    rd_kind(f, MC68K_FPU_RESPONSE_FP_REGISTERS);
    CHECK(mc68k_fpu_read_register_select(f, &sel));
    for (i = 0; i < 3; i++)
        CHECK(mc68k_fpu_write_operand(f, w3[i]));
    CHECK_EQ(i, 3); /* ループが縮まない */
    expect_idle(f);
    CHECK(mc68k_fpu_write_command_word(f, 0xF000 | 0x80));
    rd_kind(f, MC68K_FPU_RESPONSE_FP_REGISTERS);
    CHECK(mc68k_fpu_read_register_select(f, &sel));
    for (i = 0; i < 3; i++)
        CHECK(mc68k_fpu_read_operand(f, &out[i]));
    CHECK_EQ(i, 3); /* ループが縮まない */
    expect_idle(f);
    CHECK_EQ(read_fpsr(f) & FPSR_EXC(0xFF), FPSR_EXC(EXC_SNAN | EXC_OVFL));
    /* 算術（例外の無い演算）は開始時にクリアする */
    drive_regreg(f, CMD(0, 0, 1, OPM_FMOVE));
    CHECK_EQ(read_fpsr(f) & FPSR_EXC(0xFF), 0);
    /* 外部宛ての FMOVE（FPn→<ea>）も開始時にクリアする（例外の無い FMOVE.L） */
    load_long(f, 0, 5);
    set_fpsr(f, FPSR_EXC(EXC_SNAN | EXC_OVFL));
    CHECK_EQ(drive_any(f, CMD(3, FMT_L, 0, 0), NULL, 0, out, 3), 1u); /* FMOVE.L は1 word */
    CHECK_EQ(out[0], 5u);
    CHECK_EQ(read_fpsr(f) & FPSR_EXC(0xFF), 0);
    /* 条件命令も EXC をクリアする（BSUN を立てない aware な predicate） */
    set_fpsr(f, FPSR_EXC(EXC_DZ));
    CHECK(mc68k_fpu_write_condition(f, 0x01));
    rd_kind(f, MC68K_FPU_RESPONSE_NULL);
    CHECK_EQ(read_fpsr(f) & FPSR_EXC(0xFF), 0);
    mc68k_fpu_destroy(f);
}

/* FMOD/FREM の商 byte: 自身の開始時だけクリアし、他の命令は変えない */
static void test_quotient_byte(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    uint32_t w;

    load_long(f, 0, 7);
    load_long(f, 1, 2);
    drive_regreg(f, CMD(0, 1, 0, 0x21)); /* FMOD FP1,FP0: 7 - 2*INT(3.5)=1、商 3 */
    check_fp(f, 0, 0x3FFF, UINT64_C(0x8000000000000000));
    w = read_fpsr(f);
    CHECK_EQ((w >> 16) & 0xFF, 3);
    drive_regreg(f, CMD(0, 1, 0, OPM_FADD));  /* FP0 = 3 */
    CHECK_EQ((read_fpsr(f) >> 16) & 0xFF, 3); /* 他の命令では変わらない */
    drive_regreg(f, CMD(0, 1, 0, 0x21));      /* 3 mod 2: 商 1 */
    CHECK_EQ((read_fpsr(f) >> 16) & 0xFF, 1);
    load_long(f, 0, 7);
    drive_regreg(f, CMD(0, 1, 0, 0x25)); /* FREM FP1,FP0: 7 - 2*4 = -1（商は最近接偶数 4） */
    check_fp(f, 0, 0xBFFF, UINT64_C(0x8000000000000000));
    CHECK_EQ((read_fpsr(f) >> 16) & 0xFF, 4);
    /* 商の符号は bit23（[881UM Figure 2-5、p.2-6]。符号は被除数と除数の符号の XOR、FMOD は
     * p.4-62）。-7 mod 2: 商 -3 → 0x80 | 3 */
    load_long(f, 0, -7);
    drive_regreg(f, CMD(0, 1, 0, 0x21));
    CHECK_EQ((read_fpsr(f) >> 16) & 0xFF, 0x83u);
    mc68k_fpu_destroy(f);
}

/* FMOVE の条件コード（[881UM] Table 2-1）と FSAVE/FRESTORE/FMOVEM/control move
 * が保留を素通しすること */
static void test_condition_codes_and_passthrough(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    uint32_t w;
    uint16_t fmt;
    size_t bytes;

    load_long(f, 0, -5);
    CHECK_EQ(read_fpsr(f) & (FPSR_CC_N | FPSR_CC_Z | FPSR_CC_NAN | FPSR_CC_I), FPSR_CC_N);
    load_long(f, 0, 0);
    CHECK_EQ(read_fpsr(f) & (FPSR_CC_N | FPSR_CC_Z | FPSR_CC_NAN | FPSR_CC_I), FPSR_CC_Z);
    mc68k_fpu_destroy(f);

    f = to_s3(cfg);
    /* FMOVEM と control move は PENDING を素通し（EXC/FPCC を更新せず、S3 に戻る） */
    w = read_fpsr(f);
    CHECK_EQ(fpu_state_id(f), 3);
    CHECK(w & FPSR_EXC(EXC_DZ));
    {
        uint32_t out3[3];

        CHECK_EQ(drive_any(f, 0xF000 | 0x80, NULL, 0, out3, 3),
                 3u); /* 静的 list（FP0 のみ）の FMOVEM は拡張精度1本 = 3 longword を読み出す */
    }
    CHECK_EQ(fpu_state_id(f), 3);
    /* FSAVE は素通しで idle frame（b27 の値はここでは見ない。transition の fsave
     * 行が照合する）。その後は arith が NONE（S2） */
    mc68k_fpu_begin_save(f, &fmt, &bytes);
    CHECK_EQ(fmt, idle_format_word(cfg));
    {
        size_t k;

        for (k = 0; k < bytes / 4; k++)
            CHECK(mc68k_fpu_read_operand(f, &w));
    }
    CHECK_EQ(fpu_state_id(f), 2);
    mc68k_fpu_destroy(f);
}

/* 候補の無い保留（S3）は次の算術命令の開始で解消し、完了後は S2（S3 に残らない） */
static void test_pending_resolved_without_candidate(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = to_s3(cfg);

    set_fpcr(f, 0);
    CHECK_EQ(fpu_state_id(f), 3);
    drive_regreg(f, CMD(0, 1, 0, OPM_FADD));
    CHECK_EQ(fpu_state_id(f), 2);
    mc68k_fpu_destroy(f);
}

/* 候補が無ければ条件命令でも保留を解消する（§7.2）。応答を読んだ後は S2 */
static void test_pending_resolved_by_condition(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = to_s3(cfg);
    mc68k_fpu_response_t r;

    set_fpcr(f, 0);
    CHECK_EQ(fpu_state_id(f), 3);
    CHECK(mc68k_fpu_write_condition(f, 0x0F));
    CHECK_EQ(fpu_state_id(f), 13);
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
    CHECK_EQ(r.condition_true, 1);
    CHECK_EQ(fpu_state_id(f), 2);
    mc68k_fpu_destroy(f);
}

/* FPCR の PREC（bit7-6）と RND（bit5-4）の位置を、実際の丸めで確かめる。FMOVE.D 1.1 → FP0 */
static void test_fpcr_rounding_fields(mc68k_fpu_config_t cfg) {
    static const struct {
        uint32_t fpcr;
        uint64_t signif;
    } cases[] = {
        {0x40, UINT64_C(0x8CCCCD0000000000)}, /* PREC=単精度、最近接: 1.1f */
        {0x50, UINT64_C(0x8CCCCC0000000000)}, /* PREC=単精度、RZ（0方向） */
        {0x80,
         UINT64_C(0x8CCCCCCCCCCCD000)}, /* PREC=倍精度、最近接（1.1 の倍精度値そのもの） */
    };
    size_t k;

    CHECK_EQ(sizeof(cases) / sizeof(cases[0]),
             3u); /* cases の要素数。表の行の削除・ループの縮小を見逃さない */
    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        mc68k_fpu_t *f = mk(cfg);
        uint32_t w[2] = {0x3FF19999u, 0x9999999Au}; /* D: 1.1 */

        set_fpcr(f, cases[k].fpcr);
        drive_ea_in(f, CMD(2, FMT_D, 0, OPM_FMOVE), w, 2, 8);
        check_fp(f, 0, 0x3FFF, cases[k].signif);
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(k, 3u); /* ループが縮まない */
}

int main(void) {
    unsigned i;

    for (i = 0; i < 2; i++) {
        test_priority_and_vector(model_cfg(i));
        test_writeback(model_cfg(i));
        test_external_move_mid_exception(model_cfg(i));
        test_exc_clear(model_cfg(i));
        test_quotient_byte(model_cfg(i));
        test_condition_codes_and_passthrough(model_cfg(i));
        test_pending_resolved_without_candidate(model_cfg(i));
        test_pending_resolved_by_condition(model_cfg(i));
        test_fpcr_rounding_fields(model_cfg(i));
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
    printf("test_fpu_model_exceptions: OK\n");
    return 0;
}
