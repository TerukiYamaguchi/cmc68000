/*
 * test_fpcp_model.c — 68020/68030 のコプロセッサインタフェース（Issue
 * #61、現行仕様）を、 内蔵 FPU モデル（cpID=1）経由の生バイナリで検査する。
 *
 * 全 CPU model（68020・68EC030・68030）× FPU
 * 構成（68881・68882）で同じ結果になることを確かめる。 期待値は production
 * の定数・関数から取らず、命令の符号化は [UM20 §7.2]・[881UM §4.7] のビット配置から、 値は
 * 80-bit 拡張精度の定義から手で組み立てる。
 */
#include "cpu/cpu_internal.h"
#include "fpcp_machine.h"

#define OP_FPGEN(ea)  ((uint16_t)(0xF200u | (ea)))
#define EA_DN(n)      (n)
#define EA_AN_IND(n)  (0x10 | (n))
#define EA_AN_POST(n) (0x18 | (n))
#define EA_AN_PRE(n)  (0x20 | (n))
#define EA_IMM        0x3C

typedef struct {
    mc68k_model_t model;
    mc68k_fpu_config_t fpu;
    const char *name;
} config_t;

static const config_t configs[] = {
    {MC68K_MODEL_68020, MC68K_FPU_68881, "68020/68881"},
    {MC68K_MODEL_68020, MC68K_FPU_68882, "68020/68882"},
    {MC68K_MODEL_68EC030, MC68K_FPU_68881, "68EC030/68881"},
    {MC68K_MODEL_68EC030, MC68K_FPU_68882, "68EC030/68882"},
    {MC68K_MODEL_68030, MC68K_FPU_68881, "68030/68881"},
    {MC68K_MODEL_68030, MC68K_FPU_68882, "68030/68882"},
};
#define NCONFIGS (sizeof(configs) / sizeof(configs[0]))

static uint32_t prog(mach_t *m, uint32_t at, const uint16_t *w, size_t n) {
    for (size_t i = 0; i < n; i++)
        put16(m, at + 2 * (uint32_t)i, w[i]);
    return at + 2 * (uint32_t)n;
}
#define PROG(m, at, ...)                             \
    prog((m), (at), (const uint16_t[]){__VA_ARGS__}, \
         sizeof((const uint16_t[]){__VA_ARGS__}) / sizeof(uint16_t))

static mc68k_fpu_regs_t fpu_regs(mach_t *m) {
    mc68k_fpu_regs_t r;
    mc68k_get_fpu_regs(mc68k_get_fpu_const(m->cpu), &r);
    return r;
}

static void check_fp(const mc68k_fp80_t *v, uint16_t sign_exp, uint64_t signif) {
    CHECK_EQ(v->sign_exp, sign_exp);
    CHECK_EQ(v->signif, signif);
}

static void test_arith_and_transfers(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[0] = 0x4000;
    cpu->a[1] = 0x4100;
    cpu->d[5] = 7;
    put32(m, 0x4100, 0x3FFF0000); /* 1.0 */
    put32(m, 0x4104, 0x80000000);
    put32(m, 0x4108, 0);
    uint32_t end =
        PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000,
             0x0003,                                                        /* FMOVE.L #3,FP0 */
             OP_FPGEN(EA_IMM), CMD(2, FMT_L, 1, OPM_FMOVE), 0x0000, 0x0004, /* FMOVE.L #4,FP1 */
             OP_FPGEN(0), CMD(0, 0, 1, OPM_FMUL),                           /* FMUL.X FP0,FP1 */
             OP_FPGEN(EA_AN_IND(0)), CMD(3, FMT_X, 1, 0),          /* FMOVE.X FP1,(A0) */
             OP_FPGEN(EA_DN(2)), CMD(3, FMT_L, 1, 0),              /* FMOVE.L FP1,D2 */
             OP_FPGEN(EA_DN(3)), CMD(3, FMT_W, 1, 0),              /* FMOVE.W FP1,D3 */
             OP_FPGEN(EA_DN(5)), CMD(2, FMT_L, 3, OPM_FMOVE),      /* FMOVE.L D5,FP3 */
             OP_FPGEN(EA_AN_IND(1)), CMD(2, FMT_X, 4, OPM_FMOVE)); /* FMOVE.X (A1),FP4 */
    mach_run_until(m, end, 100);
    CHECK_EQ(mc68k_get_pc(cpu), end);
    CHECK_EQ(cpu->last_exception_vector, 0);

    mc68k_fpu_regs_t r = fpu_regs(m);
    check_fp(&r.fp[0], 0x4000, 0xC000000000000000ull); /* 3.0 */
    check_fp(&r.fp[1], 0x4002, 0xC000000000000000ull); /* 12.0 */
    check_fp(&r.fp[3], 0x4001, 0xE000000000000000ull); /* 7.0 */
    check_fp(&r.fp[4], 0x3FFF, 0x8000000000000000ull); /* 1.0 */
    CHECK_EQ(get32(m, 0x4000), 0x40020000u);
    CHECK_EQ(get32(m, 0x4004), 0xC0000000u);
    CHECK_EQ(get32(m, 0x4008), 0);
    CHECK_EQ(cpu->d[2], 12);
    CHECK_EQ(cpu->d[3] & 0xFFFF, 12);
    mach_destroy(m);
}

/* 1.0 / 2.0 / 3.0 を拡張精度で置く helper（メモリ 12 バイト:
 * sign_exp、予約、仮数上位、仮数下位） */
static void put_ext(mach_t *m, uint32_t at, uint16_t sign_exp, uint64_t signif) {
    put32(m, at, (uint32_t)sign_exp << 16);
    put32(m, at + 4, (uint32_t)(signif >> 32));
    put32(m, at + 8, (uint32_t)signif);
}

static void test_fmovem(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[0] = 0x4000;
    cpu->a[1] = 0x4100;
    put_ext(m, 0x4100, 0x3FFF, 0x8000000000000000ull); /* 1.0 */
    put_ext(m, 0x410C, 0x4000, 0x8000000000000000ull); /* 2.0 */
    put_ext(m, 0x4118, 0x4000, 0xC000000000000000ull); /* 3.0 */
    cpu->a[7] = 0x6000;
    uint32_t end = PROG(m, MACH_START,
                        /* FMOVEM.X (A1)+,FP0-FP2 : 110 10 000 mask（FP0=bit7）*/
                        OP_FPGEN(EA_AN_POST(1)), 0xD0E0,
                        /* FMOVEM.X FP0-FP2,(A0) : 111 10 000 mask */
                        OP_FPGEN(EA_AN_IND(0)), 0xF0E0,
                        /* FMOVEM.X FP1-FP2,-(A7) : 111 00 000 mask（predecrement は FP7=MSB
                           なので FP1,FP2 = 0x06）*/
                        OP_FPGEN(EA_AN_PRE(7)), 0xE006);
    mach_run_until(m, end, 100);
    CHECK_EQ(mc68k_get_pc(cpu), end);
    CHECK_EQ(cpu->last_exception_vector, 0);
    CHECK_EQ(cpu->a[1], 0x4124); /* (A1)+ は 3 * 12 */
    mc68k_fpu_regs_t r = fpu_regs(m);
    check_fp(&r.fp[0], 0x3FFF, 0x8000000000000000ull);
    check_fp(&r.fp[1], 0x4000, 0x8000000000000000ull);
    check_fp(&r.fp[2], 0x4000, 0xC000000000000000ull);
    /* (A0): FP0、FP1、FP2 の順に昇順 */
    CHECK_EQ(get32(m, 0x4000), 0x3FFF0000u);
    CHECK_EQ(get32(m, 0x4004), 0x80000000u);
    CHECK_EQ(get32(m, 0x400C), 0x40000000u);
    CHECK_EQ(get32(m, 0x4014), 0);
    CHECK_EQ(get32(m, 0x4018), 0x40000000u);
    CHECK_EQ(get32(m, 0x401C), 0xC0000000u);
    /* -(A7): A7 は 2 * 12 減り、最初のオペランド（FP2）が最上位のアドレスに入る */
    CHECK_EQ(cpu->a[7], 0x6000 - 24);
    CHECK_EQ(get32(m, 0x6000 - 24), 0x40000000u); /* FP1 sign_exp */
    CHECK_EQ(get32(m, 0x6000 - 24 + 4), 0x80000000u);
    CHECK_EQ(get32(m, 0x6000 - 12), 0x40000000u); /* FP2 sign_exp */
    CHECK_EQ(get32(m, 0x6000 - 12 + 4), 0xC0000000u);
    mach_destroy(m);
}

static void test_control_registers(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[7] = 0x6000;
    cpu->d[6] = 0xAAAAAAAAu;
    uint32_t arith_pc = MACH_START + 2 * 4 + 2 * 4; /* 2 つ目の演算命令（FMUL）の先頭 */
    uint32_t end =
        PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(4, CTL_FPCR, 0, 0), 0x0000,
             0x0290, /* FMOVE.L #$0290,FPCR（INEX2 を許可、MODE=$90） */
             OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000, 0x0002, /* FMOVE.L #2,FP0 */
             OP_FPGEN(0), CMD(0, 0, 0, OPM_FMUL),         /* FMUL.X FP0,FP0。FPIAR = この命令 */
             OP_FPGEN(EA_DN(1)), CMD(5, CTL_FPCR, 0, 0),  /* FMOVE.L FPCR,D1 */
             OP_FPGEN(EA_DN(2)), CMD(5, CTL_FPSR, 0, 0),  /* FMOVE.L FPSR,D2 */
             OP_FPGEN(EA_DN(3)), CMD(5, CTL_FPIAR, 0, 0), /* FMOVE.L FPIAR,D3 */
             OP_FPGEN(EA_AN_PRE(7)), CMD(5, 7, 0, 0));    /* FMOVEM.L FPCR/FPSR/FPIAR,-(A7) */
    mach_run_until(m, end, 100);
    CHECK_EQ(mc68k_get_pc(cpu), end);
    CHECK_EQ(cpu->last_exception_vector, 0);
    CHECK_EQ(cpu->d[1], 0x0290);
    CHECK_EQ(cpu->d[2] & 0x0F000000u, 0); /* 4 * 2 = 4 は N/Z/I/NAN のいずれでもない */
    /* モデルは、例外を許可しているとき（BSUN だけの場合を除く）最初の primitive に PC
     * 要求を付け、 MPU が命令アドレス CIR へ命令先頭を書く（fpu_proto.c の pc_flag）。 */
    CHECK_EQ(cpu->d[3], arith_pc);
    CHECK_EQ(cpu->a[7], 0x6000 - 12);
    /* 3 本とも転送するときは FPCR が最下位のアドレス、FPSR、FPIAR の順 */
    CHECK_EQ(get32(m, 0x6000 - 12), 0x0290);
    CHECK_EQ(get32(m, 0x6000 - 4), arith_pc);
    mach_destroy(m);
}

/* 条件命令。FCMP の結果（等しい）に対する FBcc / FScc / FDBcc / FTRAPcc */
#define FCOND_F         0x00
#define FCOND_EQ        0x01
#define FCOND_NE        0x0E
#define FCOND_T         0x0F
#define OP_FBCC_W(cond) ((uint16_t)(0xF280u | (cond)))
#define OP_FBCC_L(cond) ((uint16_t)(0xF2C0u | (cond)))
#define OP_FSCC(ea)     ((uint16_t)(0xF240u | (ea)))
#define OP_FDBCC(dn)    ((uint16_t)(0xF248u | (dn)))
#define OP_FTRAPCC_W    0xF27A
#define OP_FTRAPCC_NONE 0xF27C
#define MOVEQ(n, d)     ((uint16_t)(0x7000u | ((d) << 9) | ((n) & 0xFF)))

static void test_conditions(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[0] = 0x4000;
    put16(m, 0x4000, 0x1234);
    cpu->d[2] = 2;
    cpu->a[7] = 0x6000;
    /* FP0 = FP1 = 3 → FCMP で Z。FBEQ.W は成立し MOVEQ #1,D0 を飛ばす。FBNE.W は不成立で MOVEQ
     * #1,D6 が実行される。 FBEQ.L も成立し MOVEQ #1,D7 を飛ばす。 */
    uint32_t pc = MACH_START;
    pc = PROG(m, pc, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000, 0x0003,
              OP_FPGEN(EA_IMM), CMD(2, FMT_L, 1, OPM_FMOVE), 0x0000, 0x0003, OP_FPGEN(0),
              CMD(0, 0, 1, OPM_FCMP), OP_FBCC_W(FCOND_EQ), 0x0004, MOVEQ(1, 0), MOVEQ(2, 1),
              OP_FBCC_W(FCOND_NE), 0x0004, MOVEQ(1, 6), MOVEQ(2, 5), OP_FBCC_L(FCOND_EQ),
              0x0000, 0x0006, MOVEQ(1, 7), MOVEQ(2, 4));
    /* FSEQ D4 / FSNE (A0) / FSEQ -(A7)（バイト） */
    pc = PROG(m, pc, OP_FSCC(EA_DN(4)), FCOND_EQ, OP_FSCC(EA_AN_IND(0)), FCOND_NE,
              OP_FSCC(EA_AN_PRE(7)), FCOND_EQ);
    /* FDBF D2: 条件が偽のあいだ D2.w を減らして戻る。ADDQ.W #1,D3 の 3 回で D2.w = $FFFF */
    const uint32_t loop = pc;
    pc = PROG(m, pc, 0x5243, OP_FDBCC(2), FCOND_F);
    pc = PROG(m, pc, (uint16_t)(loop - pc));
    /* FDBEQ D2: 条件が真なので D2 を変えず次へ */
    pc = PROG(m, pc, OP_FDBCC(2), FCOND_EQ, 0x7FFE);
    /* FTRAPF.W #$1234 は何もしない。FTRAPT（オペランド無し）は vector 7 */
    pc = PROG(m, pc, OP_FTRAPCC_W, FCOND_F, 0x1234);
    const uint32_t trap_pc = pc;
    pc = PROG(m, pc, OP_FTRAPCC_NONE, FCOND_T);
    mach_run_until(m, MACH_HANDLER(7), 200);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(7));
    CHECK_EQ(cpu->d[0], 0); /* FBEQ.W が成立して飛ばした */
    CHECK_EQ(cpu->d[1], 2);
    CHECK_EQ(cpu->d[6], 1); /* FBNE.W は不成立 */
    CHECK_EQ(cpu->d[5], 2);
    CHECK_EQ(cpu->d[7], 0); /* FBEQ.L が成立 */
    CHECK_EQ(cpu->d[4] & 0xFF, 0xFF);
    CHECK_EQ(m->ram[0x4000], 0x00); /* FSNE は偽 */
    CHECK_EQ(m->ram[0x4001], 0x34);
    CHECK_EQ(cpu->d[3], 3);
    CHECK_EQ(cpu->d[2] & 0xFFFF, 0xFFFF);
    /* FSEQ -(A7): A7 は 2 減る（A7 でバイト）。バイトは減らした位置に入る */
    /* vector 7 は format 2 のフレーム: SR、PC=次の命令、$201C、命令アドレス。FTRAPcc.W/L
     * のオペランドは無し */
    const uint32_t sp = cpu->a[7];
    CHECK_EQ(get32(m, sp + 2), pc);
    CHECK_EQ(get16(m, sp + 6), 0x2000 | (7 * 4));
    CHECK_EQ(get32(m, sp + 8), trap_pc);
    mach_destroy(m);
}

/* FSAVE / FRESTORE */
#define OP_FSAVE(ea)    ((uint16_t)(0xF300u | (ea)))
#define OP_FRESTORE(ea) ((uint16_t)(0xF340u | (ea)))

/* idle frame の format word（[881UM §6.4.2.2] の初期モデル。現行仕様） */
static uint16_t idle_format(mc68k_fpu_config_t fpu) {
    return fpu == MC68K_FPU_68882 ? 0x1F38 : 0x1F18;
}

static void test_save_restore(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;
    const unsigned body = c->fpu == MC68K_FPU_68882 ? 0x38 : 0x18;

    cpu->a[7] = 0x6000;
    cpu->a[1] = 0x4000;
    /* リセット直後は null frame（format $0000、予約 word 0）。-(A7) へ 4 バイト */
    uint32_t pc = MACH_START;
    pc = PROG(m, pc, OP_FSAVE(EA_AN_PRE(7)));
    mach_run_until(m, pc, 10);
    CHECK_EQ(cpu->a[7], 0x6000 - 4);
    CHECK_EQ(get32(m, 0x6000 - 4), 0);
    /* FRESTORE (A7)+ で null を戻すと FP レジスタは NaN（全ビット 1）に初期化される */
    pc = PROG(m, pc, OP_FRESTORE(EA_AN_POST(7)));
    mach_run_until(m, pc, 10);
    CHECK_EQ(cpu->a[7], 0x6000);

    /* FMOVE.L #3,FP0 の後は idle frame。-(A7) へ 4 + body バイト。先頭が format word と予約
     * word $FFFF */
    pc = PROG(m, pc, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000, 0x0003,
              OP_FSAVE(EA_AN_PRE(7)));
    mach_run_until(m, pc, 10);
    CHECK_EQ(cpu->a[7], 0x6000 - 4 - body);
    const uint32_t frame = cpu->a[7];
    CHECK_EQ(get16(m, frame), idle_format(c->fpu));
    CHECK_EQ(get16(m, frame + 2), 0xFFFF);
    /* FRESTORE (A7)+ で戻し、(A1) へ保存し直したフレームは元のフレームとバイト単位で一致する */
    pc = PROG(m, pc, OP_FRESTORE(EA_AN_POST(7)), OP_FSAVE(EA_AN_IND(1)));
    mach_run_until(m, pc, 10);
    CHECK_EQ(cpu->a[7], 0x6000);
    CHECK_EQ(cpu->last_exception_vector, 0);
    CHECK(memcmp(&m->ram[frame], &m->ram[0x4000], 4 + body) == 0);
    /* BIU flags は frame の最後の long（881 は +$18、882 は +$38。[881UM Figure 6-4、6-5]）。
     * オペランド CIR からの読出しは逆順で、最初に読んだ long が最高アドレスに入る */
    CHECK_EQ(get32(m, frame + body) & 0xFC000000u,
             0x7C000000u); /* bit31=0、30-28=111（保留なし）、27-26=11 */
    for (unsigned off = 4; off < body; off += 4)
        CHECK_EQ(get32(m, frame + off), 0);
    check_fp(&fpu_regs(m).fp[0], 0x4000, 0xC000000000000000ull);

    /* null frame の FRESTORE は FP レジスタを NaN（sign_exp $7FFF、仮数全ビット 1）へ初期化 */
    put32(m, 0x4800, 0);
    cpu->a[2] = 0x4800;
    pc = PROG(m, pc, OP_FRESTORE(EA_AN_IND(2)));
    mach_run_until(m, pc, 10);
    check_fp(&fpu_regs(m).fp[0], 0x7FFF, 0xFFFFFFFFFFFFFFFFull);

    /* FRESTORE の後のオペランド CIR の読出しは、先の FSAVE の body
     * の残りでなくモデルのオペランドを返す （#68 のレビュー: model_write() の CIR_RESTORE の
     * `cp_save_left = 0`） */
    cpu->d[2] = 0;
    pc = PROG(m, pc, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000, 0x0007,
              OP_FPGEN(EA_DN(2)), CMD(3, FMT_L, 0, 0)); /* FMOVE.L #7,FP0 ; FMOVE.L FP0,D2 */
    mach_run_until(m, pc, 10);
    CHECK_EQ(cpu->d[2], 7u);
    mach_destroy(m);
}

static void test_save_restore_errors(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    /* 無効な format word（$02xx）の FRESTORE は format error（vector 14、format
     * 0、PC=命令先頭） */
    cpu->a[1] = 0x4000;
    cpu->a[7] = 0x6000;
    put32(m, 0x4000, 0x0200FFFFu);
    PROG(m, MACH_START, OP_FRESTORE(EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(14), 10);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(14));
    CHECK_EQ(cpu->a[7], 0x6000 - 8);
    CHECK_EQ(get32(m, 0x6000 - 8 + 2), MACH_START);
    CHECK_EQ(get16(m, 0x6000 - 8 + 6), 14 * 4);
    mach_destroy(m);

    /* ユーザモードの FSAVE / FRESTORE は特権違反（vector 8、CIR に触れる前） */
    for (int restore = 0; restore < 2; restore++) {
        m = mach_create(c->model, c->fpu, false);
        cpu = m->cpu;
        cpu->a[7] = 0x6000; /* SSP */
        cpu->a[1] = 0x4000;
        PROG(m, MACH_START, restore ? OP_FRESTORE(EA_AN_IND(1)) : OP_FSAVE(EA_AN_PRE(7)));
        cpu_set_sr(cpu, 0x0000);
        cpu->a[7] = 0x5000; /* ユーザスタック */
        mach_run_until(m, MACH_HANDLER(8), 10);
        CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(8));
        CHECK_EQ(cpu->a[7], 0x6000 - 8);
        CHECK_EQ(get16(m, 0x6000 - 8), 0x0000); /* 積まれた SR はユーザモードのもの */
        CHECK_EQ(get32(m, 0x6000 - 8 + 2), MACH_START);
        CHECK_EQ(get16(m, 0x6000 - 8 + 6), 8 * 4);
        CHECK_EQ(cpu->usp, 0x5000); /* FSAVE の -(A7) は実行されていない */
        mach_destroy(m);
    }
}

/* 例外。フレームの欄は [UM20 Figure 7-41、7-43] */
#define VEC_BSUN  48
#define VEC_DZ    50
#define VEC_OPERR 52

static void test_exceptions(const config_t *c) {
    mach_t *m;
    mc68k_state_t *cpu;

    /* (1) F-line: クラス 001 の command word は不正で vector 11（format 0、PC=命令先頭） */
    m = mach_create(c->model, c->fpu, false);
    cpu = m->cpu;
    cpu->a[7] = 0x6000;
    PROG(m, MACH_START, OP_FPGEN(0), 0x2000);
    mach_run_until(m, MACH_HANDLER(11), 10);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(11));
    CHECK_EQ(cpu->a[7], 0x6000 - 8);
    CHECK_EQ(get32(m, 0x6000 - 8 + 2), MACH_START);
    CHECK_EQ(get16(m, 0x6000 - 8 + 6), 11 * 4);
    mach_destroy(m);

    /* (2) 応答の valid EA が命令の EA と合わない: FMOVE.L A1,FP0 は データ EA が必要で An
     * は不可。 命令を中止（abort）して vector 11。その後モデルは idle で、次の FP 命令が動く */
    m = mach_create(c->model, c->fpu, false);
    cpu = m->cpu;
    cpu->a[7] = 0x6000;
    cpu->d[5] = 9;
    uint32_t pc = PROG(m, MACH_START, OP_FPGEN(0x09), CMD(2, FMT_L, 0, OPM_FMOVE));
    mach_run_until(m, MACH_HANDLER(11), 10);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(11));
    CHECK_EQ(get32(m, 0x6000 - 8 + 2), MACH_START);
    cpu->pc = pc; /* ハンドラの NOP は実行せず次の命令へ */
    pc = PROG(m, pc, OP_FPGEN(EA_DN(5)), CMD(2, FMT_L, 2, OPM_FMOVE));
    mach_run_until(m, pc, 10);
    CHECK_EQ(mc68k_get_pc(cpu), pc);
    check_fp(&fpu_regs(m).fp[2], 0x4002, 0x9000000000000000ull); /* 9.0 */
    mach_destroy(m);

    /* (3) BSUN: 非数に対する符号付き述語（FBGT = $12）は、BSUN を許可していると pre-instruction
     * 例外 （vector 48、format 0、PC=FBcc の先頭）。許可していなければ BSUN
     * を立てるだけで続行する */
    for (int enabled = 0; enabled < 2; enabled++) {
        m = mach_create(c->model, c->fpu, false);
        cpu = m->cpu;
        cpu->a[7] = 0x6000;
        cpu->a[1] = 0x4000;
        put_ext(m, 0x4000, 0x7FFF, 0xC000000000000000ull); /* 非シグナリング NaN */
        const uint32_t fbcc = MACH_START + 8 + 4 + 4;
        pc = PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(4, CTL_FPCR, 0, 0), 0x0000,
                  enabled ? FPCR_ENABLE(ENABLE_BSUN) : 0, OP_FPGEN(EA_AN_IND(1)),
                  CMD(2, FMT_X, 0, OPM_FMOVE), OP_FPGEN(0), CMD(0, 0, 0, OPM_FTST),
                  OP_FBCC_W(0x12), 0x0004, MOVEQ(1, 0), MOVEQ(2, 1));
        if (enabled) {
            mach_run_until(m, MACH_HANDLER(VEC_BSUN), 20);
            CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(VEC_BSUN));
            CHECK_EQ(cpu->a[7], 0x6000 - 8);
            CHECK_EQ(get32(m, 0x6000 - 8 + 2), fbcc);
            CHECK_EQ(get16(m, 0x6000 - 8 + 6), VEC_BSUN * 4);
        } else {
            mach_run_until(m, pc, 20);
            CHECK_EQ(mc68k_get_pc(cpu), pc);
            CHECK_EQ(cpu->last_exception_vector, 0);
        }
        mach_destroy(m);
    }
}

#define OP_RTE      0x4E73
#define ADDQ_W_1_D7 0x5247

/* 例外ハンドラを「D7 を 1 増やして RTE」にする。68882 は ACK
 * では保留例外を捨てないので、ハンドラが FSAVE し、idle
 * frame の BIU flags の bit27（EXC PEND）を立てて FRESTORE する。BIU flags は frame の最後の
 * long（+$38）で、bit27 は先頭 byte の bit3 */
static void handler_count_rte(mach_t *m, int vec, mc68k_fpu_config_t fpu) {
    if (fpu == MC68K_FPU_68882)
        PROG(m, MACH_HANDLER(vec), ADDQ_W_1_D7, OP_FSAVE(EA_AN_PRE(7)), 0x08EF, 0x0003, 0x0038,
             OP_FRESTORE(EA_AN_POST(7)), OP_RTE);
    else
        PROG(m, MACH_HANDLER(vec), ADDQ_W_1_D7, OP_RTE);
}

/* 算術例外は次の FP 命令の開始時に pre-instruction 例外（vector 50、format
 * 0）として報告され、RTE で その命令が再実行される */
static void test_arithmetic_exception_pre(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[7] = 0x6000;
    handler_count_rte(m, VEC_DZ, c->fpu);
    uint32_t pc =
        PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(4, CTL_FPCR, 0, 0), 0x0000,
             FPCR_ENABLE(ENABLE_DZ), OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000,
             0x0001, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 1, OPM_FMOVE), 0x0000, 0x0000, OP_FPGEN(0),
             CMD(0, 1, 0, OPM_FDIV)); /* FDIV.X FP1,FP0 → DZ（許可済み。報告は次の命令） */
    const uint32_t next = pc;
    pc = PROG(m, pc, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 2, OPM_FMOVE), 0x0000, 0x0005);
    mach_run_until(m, MACH_HANDLER(VEC_DZ), 30);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(VEC_DZ));
    CHECK_EQ(cpu->a[7], 0x6000 - 8);
    CHECK_EQ(get32(m, 0x6000 - 8 + 2), next); /* 報告時にまだ実行していない次の FP 命令の先頭 */
    CHECK_EQ(get16(m, 0x6000 - 8 + 6), VEC_DZ * 4);
    mach_run_until(m, pc, 30);
    CHECK_EQ(mc68k_get_pc(cpu), pc);
    CHECK_EQ(cpu->d[7] & 0xFFFF, 1);
    check_fp(&fpu_regs(m).fp[2], 0x4001, 0xA000000000000000ull); /* 5.0。再実行された */
    mach_destroy(m);
}

/* 命令の途中の例外（vector 52、format 9）。RTE で応答 CIR の読出しから再開する */
static void test_midinstruction_exception(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[7] = 0x6000;
    cpu->a[1] = 0x4000;
    put_ext(m, 0x4000, 0x4008, 0xFA00000000000000ull); /* 1000.0 */
    handler_count_rte(m, VEC_OPERR, c->fpu);
    const uint32_t instr = MACH_START + 8 + 4;
    uint32_t pc = PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(4, CTL_FPCR, 0, 0), 0x0000,
                       FPCR_ENABLE(ENABLE_OPERR), OP_FPGEN(EA_AN_IND(1)),
                       CMD(2, FMT_X, 0, OPM_FMOVE), OP_FPGEN(EA_DN(1)),
                       CMD(3, FMT_B, 0, 0)); /* FMOVE.B FP0,D1: 1000 は B に入らず OPERR */
    cpu->d[1] = 0x11223344;
    mach_run_until(m, MACH_HANDLER(VEC_OPERR), 30);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(VEC_OPERR));
    CHECK_EQ(cpu->a[7], 0x6000 - 20);
    const uint32_t f = cpu->a[7];
    CHECK_EQ(get32(m, f + 2), pc);                       /* scanPC: command word の次 */
    CHECK_EQ(get16(m, f + 6), 0x9000 | (VEC_OPERR * 4)); /* format 9 */
    CHECK_EQ(get32(m, f + 8), instr);                    /* 命令の先頭 */
    CHECK_EQ(get16(m, f + 14), OP_FPGEN(EA_DN(1)));      /* オペレーションワード */
    mach_run_until(m, pc, 30);                           /* RTE で再開 */
    CHECK_EQ(mc68k_get_pc(cpu), pc);
    CHECK_EQ(cpu->d[7] & 0xFFFF, 1);
    CHECK_EQ(cpu->d[1], 0x1122337Fu); /* 例外の前に書かれた飽和値（B の最大値）が残る */
    mach_destroy(m);
}

/* レビュー（Opus、A-B9）で指摘された、ポートの符号化・変換の検査 */

/* PF の符号化: トレース中の FP 命令は、最終応答（PF=1 の NULL）で完了してトレース例外になる。
 * PF が反転していると応答を上限まで読み直して protocol violation（vector 13）になる */
static void test_trace_with_fp_instruction(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[7] = 0x6000;
    PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000, 0x0003);
    cpu->sr |= SR_T1;
    mc68k_execute(cpu, 1);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(9));
    check_fp(&fpu_regs(m).fp[0], 0x4000, 0xC000000000000000ull);
    mach_destroy(m);
}

/* 動的な FMOVEM のレジスタリスト（Dn）と動的な k-factor（Dn）。MP レジスタ番号が正しく渡ること
 */
static void test_dynamic_register_transfers(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[1] = 0x4100;
    cpu->d[3] = 0xC0; /* FP0 と FP1（postincrement/control では MSB=FP0） */
    put_ext(m, 0x4100, 0x3FFF, 0x8000000000000000ull);
    put_ext(m, 0x410C, 0x4000, 0x8000000000000000ull);
    /* FMOVEM.X (A1)+,D3 : 110 11 000 0 Dn(3) 0000 */
    uint32_t end =
        PROG(m, MACH_START, OP_FPGEN(EA_AN_POST(1)), (uint16_t)(0xD800u | (3u << 4)));
    mach_run_until(m, end, 20);
    CHECK_EQ(mc68k_get_pc(cpu), end);
    CHECK_EQ(cpu->a[1], 0x4100 + 24);
    check_fp(&fpu_regs(m).fp[0], 0x3FFF, 0x8000000000000000ull);
    check_fp(&fpu_regs(m).fp[1], 0x4000, 0x8000000000000000ull);
    mach_destroy(m);

    /* FMOVE.P FP0,(A0){Dn}: 動的 k-factor は Dn の下位 7bit。静的な k と同じ結果になり、k
     * が違えば結果も違う */
    uint32_t packed[2][3];
    for (unsigned i = 0; i < 2; i++) {
        const unsigned k = i ? 3u : 1u;
        for (unsigned dynamic = 0; dynamic < 2; dynamic++) {
            m = mach_create(c->model, c->fpu, false);
            cpu = m->cpu;
            cpu->a[0] = 0x4000;
            cpu->a[1] = 0x4100;
            cpu->d[2] = k | 0xFFFFFF00u;                       /* 下位 7bit だけ使う */
            put_ext(m, 0x4100, 0x3FFF, 0xC000000000000000ull); /* 1.5 */
            const uint16_t cmd = dynamic ? CMD(3, 7, 0, 2u << 4) : CMD(3, 3, 0, k);
            end = PROG(m, MACH_START, OP_FPGEN(EA_AN_IND(1)), CMD(2, FMT_X, 0, OPM_FMOVE),
                       OP_FPGEN(EA_AN_IND(0)), cmd);
            mach_run_until(m, end, 20);
            CHECK_EQ(mc68k_get_pc(cpu), end);
            CHECK_EQ(cpu->last_exception_vector, 0);
            uint32_t w[3] = {get32(m, 0x4000), get32(m, 0x4004), get32(m, 0x4008)};
            if (dynamic)
                CHECK(memcmp(w, packed[i], sizeof(w)) == 0);
            else
                memcpy(packed[i], w, sizeof(w));
            mach_destroy(m);
        }
    }
    CHECK(memcmp(packed[0], packed[1], sizeof(packed[0])) != 0);
}

/* ACK は未開始の命令（UNSTARTED）を捨てる。BSUN の pre 例外のあとの FSAVE は、idle frame の BIU
 * flags の bit30-28 が 111（保留なし）になる。XA を ABORT に取り違えると condition の UNSTARTED
 * が残る */
static void test_ack_discards_unstarted(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;
    const unsigned body = c->fpu == MC68K_FPU_68882 ? 0x38 : 0x18;

    cpu->a[7] = 0x6000;
    cpu->a[1] = 0x4000;
    cpu->a[6] = 0x4800;
    put_ext(m, 0x4000, 0x7FFF, 0xC000000000000000ull);
    PROG(m, MACH_HANDLER(VEC_BSUN), OP_FSAVE(EA_AN_IND(6)));
    PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(4, CTL_FPCR, 0, 0), 0x0000,
         FPCR_ENABLE(ENABLE_BSUN), OP_FPGEN(EA_AN_IND(1)), CMD(2, FMT_X, 0, OPM_FMOVE),
         OP_FPGEN(0), CMD(0, 0, 0, OPM_FTST), OP_FBCC_W(0x12), 0x0004);
    mach_run_until(m, MACH_HANDLER(VEC_BSUN) + 2, 30);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(VEC_BSUN) + 2);
    CHECK_EQ(get16(m, 0x4800), idle_format(c->fpu));
    CHECK_EQ((get32(m, 0x4800 + body) >> 28) & 7u, 7u);
    mach_destroy(m);
}

/* リストア CIR の読返しはモデルの値を使う。他機種の idle format word はモデルが invalid
 * と返し、 format error（vector 14）になる */
static void test_restore_foreign_format(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[7] = 0x6000;
    cpu->a[1] = 0x4000;
    put32(m, 0x4000, (c->fpu == MC68K_FPU_68882 ? 0x1F18u : 0x1F38u) << 16 | 0xFFFF);
    PROG(m, MACH_START, OP_FRESTORE(EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(14), 10);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(14));
    mach_destroy(m);
}

/* 再レビュー（Opus、A-B11）: FMOVE.B/.W <ea>,FPn。モデルへのオペランドは MSB
 * 側に詰めた形で、ポートが 1・2 バイトの転送を変換する（[UM20 Figure
 * 7-21]）。Dn、即値、メモリのすべてで確かめる */
static void test_byte_word_sources(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[1] = 0x4000;
    cpu->a[2] = 0x4010;
    cpu->d[4] = 0xAAAA0007u;
    m->ram[0x4000] = 0xFD;    /* B: -3 */
    put16(m, 0x4010, 0xFF9C); /* W: -100 */
    uint32_t end =
        PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(2, FMT_B, 0, OPM_FMOVE),
             0x00FD,                                                /* FMOVE.B #-3,FP0 */
             OP_FPGEN(EA_IMM), CMD(2, FMT_W, 1, OPM_FMOVE), 0xFF9C, /* FMOVE.W #-100,FP1 */
             OP_FPGEN(EA_DN(4)), CMD(2, FMT_B, 2, OPM_FMOVE),       /* FMOVE.B D4,FP2 */
             OP_FPGEN(EA_DN(4)), CMD(2, FMT_W, 3, OPM_FMOVE),       /* FMOVE.W D4,FP3 */
             OP_FPGEN(EA_AN_IND(1)), CMD(2, FMT_B, 4, OPM_FMOVE),   /* FMOVE.B (A1),FP4 */
             OP_FPGEN(EA_AN_IND(2)), CMD(2, FMT_W, 5, OPM_FMOVE));  /* FMOVE.W (A2),FP5 */
    mach_run_until(m, end, 100);
    CHECK_EQ(mc68k_get_pc(cpu), end);
    CHECK_EQ(cpu->last_exception_vector, 0);
    mc68k_fpu_regs_t r = fpu_regs(m);
    check_fp(&r.fp[0], 0xC000, 0xC000000000000000ull); /* -3.0 */
    check_fp(&r.fp[1], 0xC005, 0xC800000000000000ull); /* -100.0 = -1.5625 * 2^6 */
    check_fp(&r.fp[2], 0x4001, 0xE000000000000000ull); /* 7.0 */
    check_fp(&r.fp[3], 0x4001, 0xE000000000000000ull);
    check_fp(&r.fp[4], 0xC000, 0xC000000000000000ull);
    check_fp(&r.fp[5], 0xC005, 0xC800000000000000ull);
    mach_destroy(m);
}

/* 再々レビュー（Opus、A-B25）: FSAVE の body の書込みがバスエラーになると AB
 * で中止する。ポートが保持する 残りの body は捨てられ、その後の FMOVE FPn,<ea> が古い body
 * を読まない */
static void test_save_abort_discards_body(const config_t *c) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[7] = 0x6000;
    cpu->a[1] =
        MACH_RAM - 8; /* format long は入るが、body の最初の書込み（最高アドレス）は RAM の外 */
    cpu->a[0] = 0x4000;
    /* idle frame にするため、先に FP 命令を 1 つ実行する */
    uint32_t pc =
        PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000, 0x0003);
    pc = PROG(m, pc, OP_FSAVE(EA_AN_IND(1)));
    const uint32_t after = pc;
    mach_run_until(m, MACH_HANDLER(2), 10);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(2));
    CHECK_EQ(cpu->last_exception_vector, 2);
    cpu->pc = after;
    uint32_t end = PROG(m, after, OP_FPGEN(EA_AN_IND(0)), CMD(3, FMT_X, 0, 0));
    mach_run_until(m, end, 10);
    CHECK_EQ(mc68k_get_pc(cpu), end);
    CHECK_EQ(get32(m, 0x4000), 0x40000000u); /* 3.0 の sign_exp */
    CHECK_EQ(get32(m, 0x4004), 0xC0000000u);
    CHECK_EQ(get32(m, 0x4008), 0);
    mach_destroy(m);
}

/* モデルの EA 転送が要求する valid EA の分類（[881UM Table 7-4]）を、分類ごとに許されない EA
 * で突く （#68 backlog: model_encode_response の valid EA の符号化）。分類のビットを 1
 * つ増やす変異は、 許されない EA を通して protocol violation（vector
 * 13）に化ける。正しくは分類の外の EA は F-line（vector 11）。 */
#define VEC_FLINE_EXPECTED 11

static void expect_first_exception(const config_t *c, const uint16_t *words, size_t n,
                                   unsigned vec) {
    mach_t *m = mach_create(c->model, c->fpu, false);

    m->cpu->a[7] = MACH_STACK;
    prog(m, MACH_START, words, n);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(m->cpu->last_exception_vector, vec);
    mach_destroy(m);
}

static void test_valid_ea_classes(const config_t *c) {
    /* 拡張精度（X）はメモリだけ: Dn への書出し、Dn からの読込みは F-line */
    const uint16_t x_to_dn[] = {OP_FPGEN(EA_DN(2)), CMD(3, FMT_X, 0, 0)};
    const uint16_t x_from_dn[] = {OP_FPGEN(EA_DN(2)), CMD(2, FMT_X, 0, OPM_FMOVE)};
    /* long（L）の書出しは data alterable: PC 相対は変更できないので F-line */
    const uint16_t l_to_pcrel[] = {OP_FPGEN(0x3A), CMD(3, FMT_L, 0, 0), 0x0000};

    expect_first_exception(c, x_to_dn, sizeof(x_to_dn) / sizeof(x_to_dn[0]),
                           VEC_FLINE_EXPECTED);
    expect_first_exception(c, x_from_dn, sizeof(x_from_dn) / sizeof(x_from_dn[0]),
                           VEC_FLINE_EXPECTED);
    expect_first_exception(c, l_to_pcrel, sizeof(l_to_pcrel) / sizeof(l_to_pcrel[0]),
                           VEC_FLINE_EXPECTED);
}

/* FSAVE の body は最上位アドレスの long から逆順に読み、最後に読む long
 * が最下位（+$04）に入る。その long が 0 でない frame を作り、CPU 経由の FSAVE
 * の結果を、同じ状態のモデルから公開 API（mc68k_fpu_begin_save()、
 * mc68k_fpu_read_operand()）で直接取り出した body と照合する（#68 backlog: model_read の body
 * 読出し）。 作り方: idle frame の BIU flags の P（bit30-28）を conditional（001）にし、+$04
 * の上位 word に条件 word を置いて FRESTORE する。モデルは条件を開始して busy になり、busy
 * frame の最下位 long は 0 でない。 */
static mach_t *restored_conditional_machine(const config_t *c, uint32_t *pc_out) {
    mach_t *m = mach_create(c->model, c->fpu, false);
    mc68k_state_t *cpu = m->cpu;
    const unsigned body = c->fpu == MC68K_FPU_68882 ? 0x38 : 0x18;
    const uint32_t frame = 0x4000;
    const uint16_t image = 0x0012;

    cpu->a[7] = 0x6000;
    cpu->a[1] = frame;
    uint32_t pc = PROG(m, MACH_START, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000,
                       0x0003, OP_FSAVE(EA_AN_IND(1)));
    mach_run_until(m, pc, 20);
    put32(m, frame + 4, (uint32_t)image << 16);
    put32(m, frame + body, (get32(m, frame + body) & ~(7u << 28)) | (1u << 28));
    pc = PROG(m, pc, OP_FRESTORE(EA_AN_IND(1)));
    mach_run_until(m, pc, 20);
    CHECK_EQ(cpu->last_exception_vector, 0);
    *pc_out = pc;
    return m;
}

static void test_save_body_last_long(const config_t *c) {
    const uint32_t out = 0x4400;
    uint32_t pc_a, pc_b;
    mach_t *a = restored_conditional_machine(c, &pc_a);
    mach_t *b = restored_conditional_machine(c, &pc_b);
    uint32_t want[CP_SAVE_BODY_MAX_WORDS];
    uint16_t fw = 0;
    size_t bytes = 0;

    a->cpu->a[2] = out;
    pc_a = PROG(a, pc_a, OP_FSAVE(EA_AN_IND(2)));
    mach_run_until(a, pc_a, 20);
    CHECK_EQ(a->cpu->last_exception_vector, 0);

    mc68k_fpu_begin_save(b->cpu->fpu, &fw, &bytes);
    CHECK(bytes / sizeof(uint32_t) <= CP_SAVE_BODY_MAX_WORDS);
    for (size_t i = 0; i < bytes / sizeof(uint32_t); i++)
        CHECK(mc68k_fpu_read_operand(b->cpu->fpu, &want[i]));
    CHECK_EQ(get16(a, out), fw);
    CHECK(want[0] != 0); /* 最下位の long が 0 でないこと（この試験の前提） */
    for (size_t i = 0; i < bytes / sizeof(uint32_t); i++)
        CHECK_EQ(get32(a, out + 4 + 4 * (uint32_t)i), want[i]);
    mach_destroy(a);
    mach_destroy(b);
}

/* cpScc が条件 word の下位 6 ビットをそのまま FPU モデルへ渡す（#68 のレビュー: model_write()
 * の `& 0x3Fu`）。 64 通りの predicate × FPCC の 4 状態（Z、N、greater、unordered）で、CPU
 * 経由の FScc の結果を、同じ状態の別機の mc68k_fpu_write_condition() を直接呼んだ TF
 * と比べる。上位 10 ビット（予約）が立っていても同じ */
static void test_condition_predicate_bits(const config_t *c) {
    enum { PREDICATES = 64, FPCC_STATES = 4 };
    unsigned cases = 0;

    for (unsigned st = 0; st < FPCC_STATES; st++) {
        for (unsigned p = 0; p < PREDICATES; p++) {
            mach_t *a = mach_create(c->model, c->fpu, false);
            mach_t *b = mach_create(c->model, c->fpu, false);
            mach_t *ms[2] = {a, b};

            for (unsigned k = 0; k < 2; k++) {
                mach_t *m = ms[k];
                uint32_t pc = MACH_START;

                m->cpu->a[7] = 0x6000;
                if (st ==
                    FPCC_STATES - 1) { /* null FRESTORE で FP0・FP1 は NaN。FCMP は unordered */
                    put32(m, 0x4800, 0);
                    m->cpu->a[2] = 0x4800;
                    pc = PROG(m, pc, OP_FRESTORE(EA_AN_IND(2)));
                } else {
                    /* FP0 = 3 と FP1 = 3（Z）/ 4（N）/ 2（greater）の FCMP */
                    static const uint16_t fp1[FPCC_STATES] = {3, 4, 2};
                    pc = PROG(m, pc, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 0, OPM_FMOVE), 0x0000,
                              0x0003, OP_FPGEN(EA_IMM), CMD(2, FMT_L, 1, OPM_FMOVE), 0x0000,
                              fp1[st]);
                }
                pc = PROG(m, pc, OP_FPGEN(0), CMD(0, 0, 1, OPM_FCMP));
                if (k == 0) {
                    m->cpu->d[4] = 0x5A5A5A00u;
                    pc = PROG(m, pc, OP_FSCC(EA_DN(4)), (uint16_t)(0xFC00u | p));
                }
                mach_run_until(m, pc, 20);
            }
            bool ok = mc68k_fpu_write_condition(mc68k_get_fpu(b->cpu), (uint8_t)p);
            const mc68k_fpu_response_t r = mc68k_fpu_read_response(mc68k_get_fpu(b->cpu));
            CHECK(ok);
            CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
            CHECK_EQ(a->cpu->d[4], 0x5A5A5A00u | (r.condition_true ? 0xFFu : 0x00u));
            mach_destroy(a);
            mach_destroy(b);
            cases++;
        }
    }
    CHECK_EQ(cases, (unsigned)(PREDICATES * FPCC_STATES));
}

int main(void) {
    for (size_t i = 0; i < NCONFIGS; i++) {
        fprintf(stderr, "config %s\n", configs[i].name);
        test_arith_and_transfers(&configs[i]);
        test_fmovem(&configs[i]);
        test_control_registers(&configs[i]);
        test_conditions(&configs[i]);
        test_save_restore(&configs[i]);
        test_save_restore_errors(&configs[i]);
        test_exceptions(&configs[i]);
        test_arithmetic_exception_pre(&configs[i]);
        test_midinstruction_exception(&configs[i]);
        test_trace_with_fp_instruction(&configs[i]);
        test_dynamic_register_transfers(&configs[i]);
        test_ack_discards_unstarted(&configs[i]);
        test_restore_foreign_format(&configs[i]);
        test_byte_word_sources(&configs[i]);
        test_save_abort_discards_body(&configs[i]);
        test_valid_ea_classes(&configs[i]);
        test_save_body_last_long(&configs[i]);
        test_condition_predicate_bits(&configs[i]);
    }
    return 0;
}
