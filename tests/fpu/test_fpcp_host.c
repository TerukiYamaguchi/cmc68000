/*
 * test_fpcp_host.c — 68020/68030 のコプロセッサインタフェース（Issue
 * #61、現行仕様）を、
 * ホストの偽コプロセッサ（bus.cpu_space_read/write）経由で検査する。
 *
 * 偽コプロセッサは応答 CIR に台本の値を順に返し、CIR へのアクセスを
 * fc・アドレス・サイズ・値とともに ログに残す。期待値（CPU 空間アドレス、CIR
 * のオフセット、primitive の符号化、フレームの欄）は [UM20 §7.1.4、§7.2、§7.3、§7.4、§7.5]
 * から手で組み立て、production の定数は使わない。
 */
#include "cpu/cpu_internal.h"
#include "fpcp_machine.h"

#define CPID                  1
#define OP_CP(cpid, type, ea) ((uint16_t)(0xF000u | ((cpid) << 9) | ((type) << 6) | (ea)))
#define T_GEN                 0
#define T_COND                1
#define T_BCC_W               2
#define T_BCC_L               3
#define T_SAVE                4
#define T_RESTORE             5
#define EA_DN(n)              (n)
#define EA_AN_IND(n)          (0x10 | (n))
#define EA_AN_POST(n)         (0x18 | (n))
#define EA_AN_PRE(n)          (0x20 | (n))
#define EA_IMM                0x3C
#define MOVEQ(n, d)           ((uint16_t)(0x7000u | ((d) << 9) | ((n) & 0xFF)))

/* 応答 primitive（[UM20 §7.4、§7.6]、[881UM Table 7-7] に現れる値） */
#define NULL_PF1                  0x0802u /* CA=0 PF=1: 終了 */
#define NULL_TF1                  0x0801u /* CA=0 TF=1: 条件が真 */
#define NULL_TF0                  0x0800u /* CA=0 TF=0: 条件が偽 */
#define NULL_AGAIN                0x8900u /* CA=1 IA=1: 読み直し */
#define NULL_PC_AGAIN             0xC900u /* CA=1 PC=1 IA=1 */
#define NULL_PC_FIN               0x4802u /* CA=0 PC=1 PF=1 */
#define EA_TO_CP(valid, len)      ((uint16_t)(0x9000u | ((valid) << 8) | (len))) /* CA=1 DR=0 */
#define EA_TO_MPU(valid, len)     ((uint16_t)(0xB000u | ((valid) << 8) | (len))) /* CA=1 DR=1 */
#define EA_TO_CP_LAST(valid, len) ((uint16_t)(0x1000u | ((valid) << 8) | (len)))
#define FPREGS_TO_CP(len)         ((uint16_t)(0x8100u | (len))) /* CA=1 DR=0 */
#define FPREGS_TO_MPU(len)        ((uint16_t)(0xA100u | (len))) /* CA=1 DR=1 */
#define MPREG_TO_CP(dn)           ((uint16_t)(0x8C00u | (dn)))  /* CA=1 DR=0 Dn */
#define MPREG_TO_MPU(dn)          ((uint16_t)(0xAC00u | (dn)))  /* CA=1 DR=1 Dn */
#define MPREG_TO_MPU_AN(an)       ((uint16_t)(0xAC08u | (an)))  /* CA=1 DR=1 An */
#define TAKE_PRE(v)               ((uint16_t)(0x1C00u | (v)))
#define TAKE_MID(v)               ((uint16_t)(0x1D00u | (v)))
#define TAKE_POST(v)              ((uint16_t)(0x1E00u | (v)))
#define SUPERVISOR_CHECK          0x8400u /* CA=1 */
#define BUSY                      0xA400u
#define XFER_OPWORD               0x8700u /* CA=1 */
#define XFER_STREAM(len)          ((uint16_t)(0x8F00u | (len)))

/* valid EA（[UM20 Table 7-4]） */
#define VEA_CTRL_ALT 0
#define VEA_DATA_ALT 1
#define VEA_MEM_ALT  2
#define VEA_ALT      3
#define VEA_CTRL     4
#define VEA_DATA     5
#define VEA_MEM      6
#define VEA_ANY      7

#define VEC_BUS_ERROR 2
#define VEC_PRIV      8
#define VEC_FLINE     11
#define VEC_VIOLATION 13
#define VEC_FORMAT    14
#define VEC_TRAPCC    7

static const mc68k_model_t models[] = {MC68K_MODEL_68020, MC68K_MODEL_68EC030,
                                       MC68K_MODEL_68030};
#define NMODELS (sizeof(models) / sizeof(models[0]))

static uint32_t prog(mach_t *m, uint32_t at, const uint16_t *w, size_t n) {
    for (size_t i = 0; i < n; i++)
        put16(m, at + 2 * (uint32_t)i, w[i]);
    return at + 2 * (uint32_t)n;
}
#define PROG(m, at, ...)                             \
    prog((m), (at), (const uint16_t[]){__VA_ARGS__}, \
         sizeof((const uint16_t[]){__VA_ARGS__}) / sizeof(uint16_t))

static mach_t *host_machine(mc68k_model_t model) {
    mach_t *m = mach_create(model, MC68K_FPU_NONE, true);

    m->dirty_upper = true; /* 全試験で、読出しの size より上位のビットにごみを混ぜる */
    m->cpu->a[7] = MACH_STACK;
    return m;
}

static void expect_nlog(const mach_t *m, int n) {
    CHECK_EQ(m->nlog_total, n);
}

/* ログの i 番目の CIR アクセス。FC は常に 7、アドレスは CPU 空間 */
static void expect_log(const mach_t *m, int i, bool write, unsigned cpid, unsigned cir,
                       unsigned size, uint32_t val) {
    CHECK(i < m->nlog);
    CHECK_EQ(m->log[i].fc, 7);
    CHECK_EQ(m->log[i].write, write);
    CHECK_EQ(m->log[i].addr, 0x20000u + (cpid << 13) + cir);
    CHECK_EQ(m->log[i].size, size);
    CHECK_EQ(m->log[i].val, val);
}

static void run(mach_t *m, uint32_t pc) {
    mach_run_until(m, pc, 200);
    CHECK_EQ(mc68k_get_pc(m->cpu), pc);
}

/* 例外後の PC がハンドラ、フレームが期待どおり（format 0 の 4 word） */
static void expect_pre_frame(mach_t *m, int vec, uint32_t pc_in_frame) {
    mc68k_state_t *cpu = m->cpu;

    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(vec));
    CHECK_EQ(cpu->a[7], MACH_STACK - 8);
    CHECK_EQ(get32(m, MACH_STACK - 8 + 2), pc_in_frame);
    CHECK_EQ(get16(m, MACH_STACK - 8 + 6), vec * 4);
}

/* cpGEN の基本: command CIR → 応答 CIR → オペランド CIR → 応答
 * CIR。FC=7、アドレス、サイズ、順序 */
static void test_general_cir_cycles(mc68k_model_t model, unsigned cpid) {
    mach_t *m = host_machine(model);

    mach_response(m, EA_TO_CP(VEA_DATA, 4));
    mach_response(m, NULL_PF1);
    uint32_t end = PROG(m, MACH_START, OP_CP(cpid, T_GEN, EA_IMM), 0x4000, 0x0000, 0x0003);
    run(m, end);
    int i = 0;
    expect_log(m, i++, true, cpid, CIR_COMMAND, 2, 0x4000);
    expect_log(m, i++, false, cpid, CIR_RESPONSE, 2, EA_TO_CP(VEA_DATA, 4));
    expect_log(m, i++, true, cpid, CIR_OPERAND, 4, 3);
    expect_log(m, i++, false, cpid, CIR_RESPONSE, 2, NULL_PF1);
    expect_nlog(m, i);
    mach_destroy(m);
}

/* PC ビット: 命令アドレス CIR（32bit）へ命令先頭を渡し、そのあとで primitive を処理する */
static void test_pass_pc(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(m, NULL_PC_AGAIN);                              /* PC を渡して読み直し */
    mach_response(m, (uint16_t)(EA_TO_CP(VEA_DATA, 2) | 0x4000)); /* PC=1 の EA 転送 */
    mach_response(m, NULL_PC_FIN);
    uint32_t end = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0x00AB);
    run(m, end);
    int i = 0;
    expect_log(m, i++, true, CPID, CIR_COMMAND, 2, 0x4000);
    expect_log(m, i++, false, CPID, CIR_RESPONSE, 2, NULL_PC_AGAIN);
    expect_log(m, i++, true, CPID, CIR_IA, 4, MACH_START);
    expect_log(m, i++, false, CPID, CIR_RESPONSE, 2, EA_TO_CP(VEA_DATA, 2) | 0x4000);
    expect_log(m, i++, true, CPID, CIR_IA, 4, MACH_START);
    expect_log(m, i++, true, CPID, CIR_OPERAND, 2,
               0x00AB); /* 端数の 2 バイトは word で（[UM20 §7.4.7]） */
    expect_log(m, i++, false, CPID, CIR_RESPONSE, 2, NULL_PC_FIN);
    expect_log(m, i++, true, CPID, CIR_IA, 4, MACH_START);
    expect_nlog(m, i);
    mach_destroy(m);
}

/* コプロセッサ無し: フックが NULL、または両方が NULL でないと呼ばれない。従来どおり vector 11
 */
static void test_absent_coprocessor(mc68k_model_t model) {
    mach_t *m = mach_create(model, MC68K_FPU_NONE, false);

    m->cpu->a[7] = MACH_STACK;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0, 3);
    mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
    expect_pre_frame(m, VEC_FLINE, MACH_START);
    mach_destroy(m);

    /* read だけ・write だけの片方では存在とみなさない */
    m = host_machine(model);
    m->cpu->bus.cpu_space_write = NULL;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0, 3);
    mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
    expect_pre_frame(m, VEC_FLINE, MACH_START);
    expect_nlog(m, 0);
    mach_destroy(m);

    /* cpID=0 は触らない（PMMU / 68851 の領域）。コプロセッサ命令として CIR に出ない */
    m = host_machine(model);
    PROG(m, MACH_START, OP_CP(0, T_GEN, EA_IMM), 0x4000, 0, 3);
    mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
    expect_nlog(m, 0);
    mach_destroy(m);

    /* type 6、7（bit8-6 = 110、111）は CIR に触れず F-line */
    for (unsigned type = 6; type < 8; type++) {
        m = host_machine(model);
        PROG(m, MACH_START, OP_CP(CPID, type, 0));
        mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
        expect_pre_frame(m, VEC_FLINE, MACH_START);
        expect_nlog(m, 0);
        mach_destroy(m);
    }
}

/* 68040 の F-line は内蔵 FPU 専用デコーダが扱い、CIR/FPCP 入口は登録しない（decode.c
 * の機種ゲート `model != MC68K_MODEL_68040`）。ただし cpID=2/3 の命令語空間は
 * CINV/CPUSH、040 MMU、MOVE16 が所有するため、それ以外の cpID だけを検証する。cpID 1 は内蔵 FPU
 * 専用デコーダが所有する（test_fpu040_fline）。 */
static void test_68040_has_no_cir_interface(void) {
    static const unsigned cpid_list[] = {4, 5, 6, 7};
    for (unsigned i = 0; i < sizeof(cpid_list) / sizeof(cpid_list[0]); i++) {
        unsigned cpid = cpid_list[i];
        mach_t *m = mach_create(MC68K_MODEL_68040, MC68K_FPU_NONE_040, true);

        m->cpu->a[7] = MACH_STACK;
        PROG(m, MACH_START, OP_CP(cpid, T_GEN, EA_IMM), 0x4000, 0, 3);
        mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
        expect_pre_frame(m, VEC_FLINE, MACH_START);
        expect_nlog(m, 0);
        mach_destroy(m);
    }
}

/* バスエラー: 命令を始める最初の CIR アクセスは F-line、それ以外は通常のバスエラー（AB を書く）
 */
static void test_cir_bus_errors(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    m->fail_cir = CIR_COMMAND;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0, 3);
    mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
    expect_pre_frame(m, VEC_FLINE, MACH_START);
    expect_nlog(m, 1); /* command CIR への書込みだけ */
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, EA_TO_CP(VEA_DATA, 4));
    mach_response(m, NULL_PF1);
    m->fail_cir = CIR_OPERAND;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0, 3);
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    CHECK_EQ(m->cpu->last_exception_vector, VEC_BUS_ERROR);
    /* command、応答、オペランド（失敗）、abort（コントロール CIR の AB=1） */
    expect_nlog(m, 4);
    expect_log(m, 3, true, CPID, CIR_CONTROL, 2, 1);
    /* フォルトの属性: FC=7、アドレスは失敗した CIR（operand CIR） */
    CHECK_EQ(m->cpu->fault_access.fc, 7);
    CHECK_EQ(m->cpu->addr_err_addr, 0x20000u + (CPID << 13) + CIR_OPERAND);
    mach_destroy(m);

    /* 条件系も最初の CIR アクセス（condition CIR）は F-line */
    m = host_machine(model);
    m->fail_cir = CIR_CONDITION;
    PROG(m, MACH_START, OP_CP(CPID, T_BCC_W, 1), 0x0004);
    mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
    expect_pre_frame(m, VEC_FLINE, MACH_START);
    mach_destroy(m);

    /* cpSAVE は save CIR の読出し、cpRESTORE は restore CIR の書込みが最初 */
    m = host_machine(model);
    m->fail_cir = CIR_SAVE;
    m->cpu->a[1] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
    expect_pre_frame(m, VEC_FLINE, MACH_START);
    mach_destroy(m);

    m = host_machine(model);
    m->fail_cir = CIR_RESTORE;
    m->cpu->a[1] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
    expect_pre_frame(m, VEC_FLINE, MACH_START);
    mach_destroy(m);
}

/* 条件命令: condition CIR の値、TF による分岐 */
static void test_conditional(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* cpBcc.W: コンディション CIR にはオペレーションワード全体を書く（条件は下位 6 bit）。TF=1
     * で変位を足して分岐（基点は変位ワード） */
    m = host_machine(model);
    mach_response(m, NULL_TF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_BCC_W, 0x2A), 0x0004, MOVEQ(1, 0), MOVEQ(2, 1));
    run(m, pc);
    CHECK_EQ(m->cpu->d[0], 0);
    CHECK_EQ(m->cpu->d[1], 2);
    expect_log(m, 0, true, CPID, CIR_CONDITION, 2,
               OP_CP(CPID, T_BCC_W, 0x2A)); /* オペレーションワード全体 */
    expect_log(m, 1, false, CPID, CIR_RESPONSE, 2, NULL_TF1);
    expect_nlog(m, 2);
    mach_destroy(m);

    /* cpBcc.L、TF=0 は分岐しない */
    m = host_machine(model);
    mach_response(m, NULL_TF0);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_BCC_L, 0x05), 0x0000, 0x0006, MOVEQ(1, 0),
              MOVEQ(2, 1));
    run(m, pc);
    CHECK_EQ(m->cpu->d[0], 1);
    CHECK_EQ(m->cpu->d[1], 2);
    expect_log(m, 0, true, CPID, CIR_CONDITION, 2, OP_CP(CPID, T_BCC_L, 0x05));
    mach_destroy(m);

    /* cpScc: 次の word（条件 word）を、予約ビットも含めてそのまま condition CIR へ書く（[UM20
     * §7.2.2.2.2]） */
    m = host_machine(model);
    mach_response(m, NULL_TF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(3)), 0xFFC7);
    run(m, pc);
    CHECK_EQ(m->cpu->d[3] & 0xFF, 0xFF);
    expect_log(m, 0, true, CPID, CIR_CONDITION, 2, 0xFFC7); /* 条件 word 全体 */
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, NULL_TF0);
    m->cpu->d[3] = 0x123456FFu;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(3)), 0x0007);
    run(m, pc);
    CHECK_EQ(m->cpu->d[3], 0x12345600u);
    mach_destroy(m);

    /* cpScc (A0)+（バイト）と、-(A7)（A7 は 2） */
    m = host_machine(model);
    mach_response(m, NULL_TF1);
    m->cpu->a[0] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_AN_POST(0)), 0x0001,
              OP_CP(CPID, T_COND, EA_AN_PRE(7)), 0x0001);
    run(m, pc);
    CHECK_EQ(m->ram[0x4000], 0xFF);
    CHECK_EQ(m->cpu->a[0], 0x4001);
    CHECK_EQ(m->cpu->a[7], MACH_STACK - 2);
    CHECK_EQ(m->ram[MACH_STACK - 2], 0xFF);
    mach_destroy(m);

    /* cpDBcc: TF=0 のとき D0.w を減らし、-1 でなければ分岐（基点は変位ワード）。TF=1 なら D0
     * を変えない */
    m = host_machine(model);
    mach_response(m, NULL_TF0);
    m->cpu->d[0] = 0x00010002u;
    {
        const uint32_t loop = MACH_START;
        pc = PROG(m, loop, 0x5243 /* ADDQ.W #1,D3 */, OP_CP(CPID, T_COND, 0x08 | 0), 0x0001);
        pc = PROG(m, pc, (uint16_t)(loop - pc));
    }
    run(m, pc);
    CHECK_EQ(m->cpu->d[3], 3);
    CHECK_EQ(m->cpu->d[0], 0x0001FFFFu); /* 下位 word だけ減る */
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, NULL_TF1);
    m->cpu->d[0] = 5;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, 0x08 | 0), 0x0001, 0x7FFE);
    run(m, pc);
    CHECK_EQ(m->cpu->d[0], 5);
    mach_destroy(m);

    /* cpTRAPcc: opmode 010（1 word）、011（2 word）、100（なし）。TF=1 は vector 7（format 2）
     */
    static const struct {
        uint16_t op;
        unsigned words;
    } traps[] = {{OP_CP(CPID, T_COND, 0x38 | 2), 1},
                 {OP_CP(CPID, T_COND, 0x38 | 3), 2},
                 {OP_CP(CPID, T_COND, 0x38 | 4), 0}};
    for (unsigned t = 0; t < 3; t++) {
        m = host_machine(model);
        mach_response(m, NULL_TF1);
        PROG(m, MACH_START, traps[t].op, 0x0003, 0x1111, 0x2222);
        mach_run_until(m, MACH_HANDLER(VEC_TRAPCC), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_TRAPCC));
        const uint32_t sp = m->cpu->a[7];
        CHECK_EQ(get32(m, sp + 2), MACH_START + 4 + 2 * traps[t].words);
        CHECK_EQ(get16(m, sp + 6), 0x2000 | (VEC_TRAPCC * 4));
        CHECK_EQ(get32(m, sp + 8), MACH_START);
        expect_log(m, 0, true, CPID, CIR_CONDITION, 2, 0x0003);
        mach_destroy(m);

        m = host_machine(model);
        mach_response(m, NULL_TF0);
        pc = PROG(m, MACH_START, traps[t].op, 0x0003, 0x1111, 0x2222);
        run(m, MACH_START + 4 + 2 * traps[t].words);
        mach_destroy(m);
    }
}

/* cpSAVE */
static void test_save(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* empty: format word と予約 word（0）の 1 long だけ。control alterable は EA に */
    m = host_machine(model);
    m->save_format = 0x0000;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0xAAAAAAAAu);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    run(m, pc);
    CHECK_EQ(get32(m, 0x4000), 0);
    expect_log(m, 0, false, CPID, CIR_SAVE, 2, 0x0000);
    expect_nlog(m, 1);
    mach_destroy(m);

    /* valid（長さ 8）: 予約 word は $FFFF。最初に読んだ long が最高アドレス（+8）、次が
     * +4（降順） */
    m = host_machine(model);
    m->save_format = 0x1F08;
    m->operand_in[0] = 0x11111111;
    m->operand_in[1] = 0x22222222;
    m->n_operand_in = 2;
    m->cpu->a[1] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    run(m, pc);
    CHECK_EQ(get32(m, 0x4000), 0x1F08FFFFu);
    CHECK_EQ(get32(m, 0x4004), 0x22222222u);
    CHECK_EQ(get32(m, 0x4008), 0x11111111u);
    expect_log(m, 0, false, CPID, CIR_SAVE, 2, 0x1F08);
    expect_log(m, 1, false, CPID, CIR_OPERAND, 4, 0x11111111);
    expect_log(m, 2, false, CPID, CIR_OPERAND, 4, 0x22222222);
    expect_nlog(m, 3);
    mach_destroy(m);

    /* valid の下限（$10xx）。長さ 4: body は 1 long */
    m = host_machine(model);
    m->save_format = 0x1004;
    m->operand_in[0] = 0x77777777;
    m->n_operand_in = 1;
    m->cpu->a[1] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    run(m, pc);
    CHECK_EQ(get32(m, 0x4000), 0x1004FFFFu);
    CHECK_EQ(get32(m, 0x4004), 0x77777777u);
    mach_destroy(m);

    /* -(A7): A7 を 4 + 長さ だけ減らし、そこをフレームの先頭にする */
    m = host_machine(model);
    m->save_format = 0x1F08;
    m->operand_in[0] = 0x11111111;
    m->operand_in[1] = 0x22222222;
    m->n_operand_in = 2;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_PRE(7)));
    run(m, pc);
    CHECK_EQ(m->cpu->a[7], MACH_STACK - 12);
    CHECK_EQ(get32(m, MACH_STACK - 12), 0x1F08FFFFu);
    CHECK_EQ(get32(m, MACH_STACK - 8), 0x22222222u);
    CHECK_EQ(get32(m, MACH_STACK - 4), 0x11111111u);
    mach_destroy(m);

    /* empty の -(A7) は 4 バイト */
    m = host_machine(model);
    m->save_format = 0x0000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_PRE(7)));
    run(m, pc);
    CHECK_EQ(m->cpu->a[7], MACH_STACK - 4);
    mach_destroy(m);

    /* not ready（$01xx）: 命令を最初からやり直す（PC が戻る）。割込みが入る機会がある。次に
     * valid を返せば完了 */
    m = host_machine(model);
    m->save_format = 0x0100;
    m->cpu->a[1] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_START);
    expect_nlog(m, 1);
    m->save_format = 0x0000;
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_START + 2);
    mach_destroy(m);

    /* invalid（$02xx、予約 $03xx〜$0Fxx）と、長さが 4 の倍数でない valid は、abort を書いて
     * vector 14 */
    static const uint16_t bad[] = {0x0200, 0x0300, 0x0F00, 0x1F06, 0x1F01};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        m = host_machine(model);
        m->save_format = bad[i];
        m->cpu->a[1] = 0x4000;
        PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
        mach_run_until(m, MACH_HANDLER(VEC_FORMAT), 5);
        expect_pre_frame(m, VEC_FORMAT, MACH_START);
        expect_log(m, 0, false, CPID, CIR_SAVE, 2, bad[i]);
        expect_log(m, 1, true, CPID, CIR_CONTROL, 2, 1);
        expect_nlog(m, 2);
        mach_destroy(m);
    }

    /* EA の検査: cpSAVE は control alterable か -(An)。Dn、(An)+、PC 相対、即値は CIR に触れず
     * F-line */
    static const uint16_t bad_ea[] = {EA_DN(0), 0x08, EA_AN_POST(1), 0x3A, 0x3B, EA_IMM};
    for (unsigned i = 0; i < sizeof(bad_ea) / sizeof(bad_ea[0]); i++) {
        m = host_machine(model);
        PROG(m, MACH_START, OP_CP(CPID, T_SAVE, bad_ea[i]), 0, 0);
        mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
        expect_pre_frame(m, VEC_FLINE, MACH_START);
        expect_nlog(m, 0);
        mach_destroy(m);
    }

    /* ユーザモードは特権違反（CIR に触れない） */
    m = host_machine(model);
    m->cpu->a[1] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    cpu_set_sr(m->cpu, 0x0000);
    m->cpu->ssp = MACH_STACK;
    mach_run_until(m, MACH_HANDLER(VEC_PRIV), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_PRIV));
    expect_nlog(m, 0);
    mach_destroy(m);
}

/* cpRESTORE */
static void test_restore(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* valid: format word を restore CIR に書き、読み返し、EA+4 から昇順に long を operand CIR
     * へ */
    m = host_machine(model);
    m->restore_readback = 0x1F08;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1F08FFFFu);
    put32(m, 0x4004, 0x22222222u);
    put32(m, 0x4008, 0x11111111u);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    run(m, pc);
    expect_log(m, 0, true, CPID, CIR_RESTORE, 2, 0x1F08);
    expect_log(m, 1, false, CPID, CIR_RESTORE, 2, 0x1F08);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0x22222222);
    expect_log(m, 3, true, CPID, CIR_OPERAND, 4, 0x11111111);
    expect_nlog(m, 4);
    mach_destroy(m);

    /* valid の下限（$10xx） */
    m = host_machine(model);
    m->restore_readback = 0x1004;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1004FFFFu);
    put32(m, 0x4004, 0x77777777u);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    run(m, pc);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0x77777777u);
    expect_nlog(m, 3);
    mach_destroy(m);

    /* (A1)+ は転送が済んだ後に 4 + 長さ だけ進む。empty は 4 */
    m = host_machine(model);
    m->restore_readback = 0x1F08;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1F08FFFFu);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_POST(1)));
    run(m, pc);
    CHECK_EQ(m->cpu->a[1], 0x400C);
    mach_destroy(m);

    m = host_machine(model);
    m->restore_readback = 0x0000;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_POST(1)));
    run(m, pc);
    CHECK_EQ(m->cpu->a[1], 0x4004);
    expect_nlog(m, 2); /* 書込みと読み返しだけ。operand CIR には触れない */
    mach_destroy(m);

    /* 読み返しが invalid（$02xx）: abort を書いて vector 14。(A1)+ は更新しない */
    m = host_machine(model);
    m->restore_readback = 0x0200;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1F08FFFFu);
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_POST(1)));
    mach_run_until(m, MACH_HANDLER(VEC_FORMAT), 5);
    expect_pre_frame(m, VEC_FORMAT, MACH_START);
    CHECK_EQ(m->cpu->a[1], 0x4000);
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
    expect_nlog(m, 3);
    mach_destroy(m);

    /* メモリ側の長さが 4 の倍数でない: restore CIR の読み返しの後で abort と vector 14 */
    m = host_machine(model);
    m->restore_readback = 0x1F06;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1F06FFFFu);
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_FORMAT), 5);
    expect_pre_frame(m, VEC_FORMAT, MACH_START);
    expect_log(m, 0, true, CPID, CIR_RESTORE, 2, 0x1F06);
    expect_log(m, 1, false, CPID, CIR_RESTORE, 2, 0x1F06);
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
    mach_destroy(m);

    /* not ready は割込みを受け付けず読み直す。上限を超えると protocol
     * violation（無限に止まらないため） */
    m = host_machine(model);
    m->restore_readback = 0x0100;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1F08FFFFu);
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
    CHECK(m->nlog_total > 1000);
    mach_destroy(m);

    /* EA の検査: control と (An)+ だけ。Dn、An、-(An)、即値は CIR に触れず F-line */
    static const uint16_t bad_ea[] = {EA_DN(0), 0x08, EA_AN_PRE(1), EA_IMM};
    for (unsigned i = 0; i < sizeof(bad_ea) / sizeof(bad_ea[0]); i++) {
        m = host_machine(model);
        PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, bad_ea[i]), 0, 0);
        mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
        expect_pre_frame(m, VEC_FLINE, MACH_START);
        expect_nlog(m, 0);
        mach_destroy(m);
    }
    /* PC 相対は control なので有効 */
    m = host_machine(model);
    m->restore_readback = 0x0000;
    put32(m, MACH_START + 4, 0);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, 0x3A), 0x0000);
    run(m, pc);
    expect_nlog(m, 2);
    mach_destroy(m);

    /* ユーザモードは特権違反 */
    m = host_machine(model);
    m->cpu->a[1] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    cpu_set_sr(m->cpu, 0x0000);
    m->cpu->ssp = MACH_STACK;
    mach_run_until(m, MACH_HANDLER(VEC_PRIV), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_PRIV));
    expect_nlog(m, 0);
    mach_destroy(m);
}

#define OP_RTE 0x4E73

/* 12 バイトずつの 3 long を operand_in に積む */
static void set_operands(mach_t *m, const uint32_t *v, int n) {
    for (int i = 0; i < n; i++)
        m->operand_in[i] = v[i];
    m->n_operand_in = n;
}

/* Evaluate EA and transfer data（[UM20 §7.4.9]）: コプロセッサ→MPU */
static void test_ea_transfer_to_mpu(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;
    const uint32_t x[3] = {0x11112222u, 0x33334444u, 0x55556666u};

    /* 12 バイトを (A0) へ昇順 */
    m = host_machine(model);
    set_operands(m, x, 3);
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 12));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6800);
    run(m, pc);
    CHECK_EQ(get32(m, 0x4000), x[0]);
    CHECK_EQ(get32(m, 0x4004), x[1]);
    CHECK_EQ(get32(m, 0x4008), x[2]);
    mach_destroy(m);

    /* 長さ 10: long、long、word（端数は 2 バイトの転送）。10 バイト目の次は触らない。長さ 3:
     * word + byte */
    m = host_machine(model);
    set_operands(m, x, 3);
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 10));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    put32(m, 0x4008, 0xFFFFFFFFu);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6800);
    run(m, pc);
    CHECK_EQ(get32(m, 0x4004), x[1]);
    CHECK_EQ(get32(m, 0x4008), 0x6666FFFFu); /* 端数は operand の下位 2 バイト */
    /* operand CIR の読出しは 4、4、2 バイト（端数はその大きさ。[UM20 §7.3.8 Figure
     * 7-21、§7.4.9]） */
    expect_log(m, 2, false, CPID, CIR_OPERAND, 4, x[0]);
    expect_log(m, 3, false, CPID, CIR_OPERAND, 4, x[1]);
    expect_log(m, 4, false, CPID, CIR_OPERAND, 2,
               x[2]); /* 返した値のまま。使うのは下位 2 バイト */
    mach_destroy(m);

    m = host_machine(model);
    set_operands(m, x, 1);
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 3));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    put32(m, 0x4000, 0xFFFFFFFFu);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6800);
    run(m, pc);
    CHECK_EQ(get32(m, 0x4000), 0x112222FFu); /* 端数は operand の下位 3 バイト */
    expect_log(m, 2, false, CPID, CIR_OPERAND, 3, x[0]);
    mach_destroy(m);

    /* (A0)+ は転送後に長さだけ進む。-(A0) は転送前に減らして昇順に書く。A7 のバイトは 2 */
    m = host_machine(model);
    set_operands(m, x, 3);
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 12));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_POST(0)), 0x6800);
    run(m, pc);
    CHECK_EQ(m->cpu->a[0], 0x400C);
    CHECK_EQ(get32(m, 0x4000), x[0]);
    mach_destroy(m);

    m = host_machine(model);
    set_operands(m, x, 3);
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 12));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4010;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_PRE(0)), 0x6800);
    run(m, pc);
    CHECK_EQ(m->cpu->a[0], 0x4004);
    CHECK_EQ(get32(m, 0x4004), x[0]);
    CHECK_EQ(get32(m, 0x400C), x[2]);
    mach_destroy(m);

    m = host_machine(model);
    set_operands(m, x, 1);
    mach_response(m, EA_TO_MPU(VEA_DATA_ALT, 1));
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_PRE(7)), 0x6000);
    run(m, pc);
    CHECK_EQ(m->cpu->a[7], MACH_STACK - 2);
    CHECK_EQ(m->ram[MACH_STACK - 2], 0x22); /* 下位 1 バイト */
    mach_destroy(m);

    /* レジスタ直接: Dn は下位 byte/word だけ、An は符号拡張して long。長さが 1/2/4 以外は
     * protocol violation */
    static const struct {
        uint16_t op;
        unsigned len;
        uint32_t operand;
        uint32_t before;
        uint32_t after;
        bool addr;
    } regs[] = {
        {EA_DN(1), 1, 0x0000007Fu, 0x11223344u, 0x1122337Fu, false},
        {EA_DN(1), 2, 0x00001234u, 0x11223344u, 0x11221234u, false},
        {EA_DN(1), 4, 0x12345678u, 0x11223344u, 0x12345678u, false},
        {0x09, 1, 0x00000080u, 0x11223344u, 0xFFFFFF80u, true},
        {0x09, 2, 0x00008000u, 0x11223344u, 0xFFFF8000u, true},
        {0x09, 4, 0x12345678u, 0x11223344u, 0x12345678u, true},
    };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        m = host_machine(model);
        m->operand_in[0] = regs[i].operand;
        m->n_operand_in = 1;
        mach_response(m, EA_TO_MPU(VEA_ALT, regs[i].len));
        mach_response(m, NULL_PF1);
        if (regs[i].addr)
            m->cpu->a[1] = regs[i].before;
        else
            m->cpu->d[1] = regs[i].before;
        pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, regs[i].op), 0x6000);
        run(m, pc);
        CHECK_EQ(regs[i].addr ? m->cpu->a[1] : m->cpu->d[1], regs[i].after);
        mach_destroy(m);
    }
    static const unsigned bad_len[] = {0, 3, 8, 12};
    for (unsigned i = 0; i < sizeof(bad_len) / sizeof(bad_len[0]); i++) {
        m = host_machine(model);
        mach_response(m, EA_TO_MPU(VEA_ALT, bad_len[i]));
        mach_response(m, NULL_PF1);
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_DN(1)), 0x6000);
        mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
        mach_destroy(m);
    }

    /* valid EA が命令の EA と合わない: abort を書いて F-line（制御可変に Dn、メモリに Dn、...）
     */
    static const struct {
        uint16_t valid;
        uint16_t ea;
    } mismatch[] = {{VEA_CTRL_ALT, EA_DN(1)}, {VEA_CTRL_ALT, EA_AN_POST(0)},
                    {VEA_DATA_ALT, 0x09},     {VEA_MEM_ALT, EA_DN(1)},
                    {VEA_ALT, 0x3A},          {VEA_CTRL, EA_AN_PRE(0)},
                    {VEA_DATA, 0x09},         {VEA_MEM, EA_DN(1)},
                    {VEA_ANY, 0x3D}};
    for (unsigned i = 0; i < sizeof(mismatch) / sizeof(mismatch[0]); i++) {
        m = host_machine(model);
        mach_response(m, EA_TO_MPU(mismatch[i].valid, 4));
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, mismatch[i].ea), 0x6000, 0, 0);
        mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
        expect_pre_frame(m, VEC_FLINE, MACH_START);
        expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
        expect_nlog(m, 3);
        mach_destroy(m);
    }

    /* valid EA が合っていても、変更できない EA（PC 相対）への書込みは protocol violation */
    m = host_machine(model);
    mach_response(m, EA_TO_MPU(VEA_ANY, 4));
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0x3A), 0x6000, 0x0000);
    mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
    mach_destroy(m);
}

/* コプロセッサ→MPU 以外: MPU→コプロセッサ（メモリ、レジスタ、即値） */
static void test_ea_transfer_to_coprocessor(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* メモリ 12 バイトを昇順に 3 long */
    m = host_machine(model);
    mach_response(m, EA_TO_CP(VEA_MEM, 12));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    put32(m, 0x4000, 0xA1A2A3A4u);
    put32(m, 0x4004, 0xB1B2B3B4u);
    put32(m, 0x4008, 0xC1C2C3C4u);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x4800);
    run(m, pc);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0xA1A2A3A4u);
    expect_log(m, 3, true, CPID, CIR_OPERAND, 4, 0xB1B2B3B4u);
    expect_log(m, 4, true, CPID, CIR_OPERAND, 4, 0xC1C2C3C4u);
    mach_destroy(m);

    /* 端数はその大きさ（1・2・3 バイト）で、下位詰めの数値として渡す。長さ 5 は 4 + 1 */
    static const struct {
        unsigned len;
        uint32_t operand;
    } part[] = {{1, 0xA1u}, {2, 0xA1A2u}, {3, 0xA1A2A3u}, {5, 0xA1A2A3A4u}};
    for (unsigned i = 0; i < sizeof(part) / sizeof(part[0]); i++) {
        m = host_machine(model);
        mach_response(m, EA_TO_CP(VEA_MEM, part[i].len));
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = 0x4000;
        put32(m, 0x4000, 0xA1A2A3A4u);
        put32(m, 0x4004, 0xB1B2B3B4u);
        pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x4800);
        run(m, pc);
        expect_log(m, 2, true, CPID, CIR_OPERAND, part[i].len == 5 ? 4 : part[i].len,
                   part[i].operand);
        if (part[i].len == 5)
            expect_log(m, 3, true, CPID, CIR_OPERAND, 1, 0xB1u);
        mach_destroy(m);
    }

    /* Dn は長さ（1・2・4）の大きさで、下位詰めの数値として書く */
    static const struct {
        unsigned len;
        uint32_t operand;
    } dn[] = {{1, 0x44u}, {2, 0x3344u}, {4, 0x11223344u}};
    for (unsigned i = 0; i < sizeof(dn) / sizeof(dn[0]); i++) {
        m = host_machine(model);
        mach_response(m, EA_TO_CP(VEA_ANY, dn[i].len));
        mach_response(m, NULL_PF1);
        m->cpu->d[2] = 0x11223344u;
        pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_DN(2)), 0x4000);
        run(m, pc);
        expect_log(m, 2, true, CPID, CIR_OPERAND, dn[i].len, dn[i].operand);
        mach_destroy(m);
    }

    /* 即値: 長さ 1 は拡張 word の下位 byte、長さ 12 は 6 word。奇数（1 以外）と DR=1 は
     * protocol violation */
    m = host_machine(model);
    mach_response(m, EA_TO_CP(VEA_ANY, 1));
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0x00A5);
    run(m, pc);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 1, 0xA5u);
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, EA_TO_CP(VEA_ANY, 12));
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4800, 0x1111, 0x2222, 0x3333, 0x4444,
              0x5555, 0x6666);
    run(m, pc);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0x11112222u);
    expect_log(m, 3, true, CPID, CIR_OPERAND, 4, 0x33334444u);
    expect_log(m, 4, true, CPID, CIR_OPERAND, 4, 0x55556666u);
    mach_destroy(m);

    for (unsigned dr = 0; dr < 2; dr++) {
        m = host_machine(model);
        mach_response(m, dr ? EA_TO_MPU(VEA_ANY, 4) : EA_TO_CP(VEA_ANY, 3));
        mach_response(m, NULL_PF1); /* 検出されなければ、この終了応答で正常に終わる */
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0, 0);
        mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
        mach_destroy(m);
    }
}

/* Transfer multiple coprocessor registers（[UM20 §7.4.16]） */
static void test_fp_registers(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;
    const uint32_t ops[6] = {0xA0A0A0A0u, 0xA1A1A1A1u, 0xA2A2A2A2u,
                             0xB0B0B0B0u, 0xB1B1B1B1u, 0xB2B2B2B2u};

    /* コプロセッサ→(A0)（制御可変）: 昇順。mask のビット数（16 bit 全部）= オペランド数 */
    m = host_machine(model);
    set_operands(m, ops, 6);
    m->regsel = 0x0300; /* 2 個 */
    mach_response(m, FPREGS_TO_MPU(12));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0xF0C0);
    run(m, pc);
    for (unsigned i = 0; i < 6; i++)
        CHECK_EQ(get32(m, 0x4000 + 4 * i), ops[i]);
    expect_log(m, 2, false, CPID, CIR_REGSEL, 2, 0x0300);
    mach_destroy(m);

    /* -(A7): 最初のオペランドが最上位。A7 は n * 長さ だけ減る */
    m = host_machine(model);
    set_operands(m, ops, 6);
    m->regsel = 0x0300;
    mach_response(m, FPREGS_TO_MPU(12));
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_PRE(7)), 0xE006);
    run(m, pc);
    CHECK_EQ(m->cpu->a[7], MACH_STACK - 24);
    CHECK_EQ(get32(m, MACH_STACK - 12), ops[0]);
    CHECK_EQ(get32(m, MACH_STACK - 4), ops[2]);
    CHECK_EQ(get32(m, MACH_STACK - 24), ops[3]);
    CHECK_EQ(get32(m, MACH_STACK - 16), ops[5]);
    mach_destroy(m);

    /* メモリ→コプロセッサ（(A1)+）: 昇順に読み、A1 は n * 長さ だけ進む */
    m = host_machine(model);
    m->regsel = 0x8100; /* 2 個 */
    mach_response(m, FPREGS_TO_CP(12));
    mach_response(m, NULL_PF1);
    m->cpu->a[1] = 0x4000;
    for (unsigned i = 0; i < 6; i++)
        put32(m, 0x4000 + 4 * i, ops[i]);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_POST(1)), 0xD0C0);
    run(m, pc);
    CHECK_EQ(m->cpu->a[1], 0x4018);
    for (unsigned i = 0; i < 6; i++)
        expect_log(m, 3 + (int)i, true, CPID, CIR_OPERAND, 4, ops[i]);
    mach_destroy(m);

    /* mask が 0: 転送なし（register select を読むだけ） */
    m = host_machine(model);
    m->regsel = 0x0000;
    mach_response(m, FPREGS_TO_MPU(12));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0xF000);
    run(m, pc);
    expect_nlog(m, 4);
    mach_destroy(m);

    /* EA の検査: MPU→コプロセッサは制御か (An)+、コプロセッサ→MPU は制御可変か -(An)。ほかは
     * abort + F-line */
    static const struct {
        uint16_t resp;
        uint16_t ea;
    } bad[] = {{FPREGS_TO_CP(12), EA_AN_PRE(0)}, {FPREGS_TO_CP(12), EA_DN(0)},
               {FPREGS_TO_CP(12), EA_IMM},       {FPREGS_TO_MPU(12), EA_AN_POST(0)},
               {FPREGS_TO_MPU(12), 0x3A},        {FPREGS_TO_MPU(12), EA_DN(0)}};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        m = host_machine(model);
        mach_response(m, bad[i].resp);
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, bad[i].ea), 0x0000, 0, 0);
        mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
        expect_pre_frame(m, VEC_FLINE, MACH_START);
        expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
        mach_destroy(m);
    }

    /* 奇数の長さは protocol violation */
    m = host_machine(model);
    mach_response(m, FPREGS_TO_MPU(11));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x0000);
    mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
    mach_destroy(m);
}

/* Transfer single main processor register（[UM20 §7.4.13]） */
static void test_mp_register(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    m = host_machine(model);
    mach_response(m, MPREG_TO_CP(3));
    mach_response(m, MPREG_TO_MPU(2));
    mach_response(m, MPREG_TO_MPU_AN(4));
    mach_response(m, NULL_PF1);
    m->cpu->d[3] = 0xCAFEBABEu;
    m->operand_in[0] = 0x11111111u;
    m->operand_in[1] = 0x22222222u;
    m->n_operand_in = 2;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x4000);
    run(m, pc);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0xCAFEBABEu);
    CHECK_EQ(m->cpu->d[2], 0x11111111u);
    CHECK_EQ(m->cpu->a[4], 0x22222222u);
    mach_destroy(m);
}

/* Supervisor check / Transfer operation word / Transfer from instruction stream / Busy */
static void test_control_primitives(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* supervisor check: スーパーバイザなら続行、ユーザなら abort + 特権違反（vector 8、format
     * 0、PC=命令先頭） */
    m = host_machine(model);
    mach_response(m, SUPERVISOR_CHECK);
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    run(m, pc);
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, SUPERVISOR_CHECK);
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    cpu_set_sr(m->cpu, 0x0000);
    m->cpu->ssp = MACH_STACK;
    mach_run_until(m, MACH_HANDLER(VEC_PRIV), 5);
    expect_pre_frame(m, VEC_PRIV, MACH_START);
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
    expect_nlog(m, 3);
    mach_destroy(m);

    /* transfer operation word: オペレーションワード CIR（16bit）へ F-line オペレーションワード
     */
    m = host_machine(model);
    mach_response(m, XFER_OPWORD);
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_DN(5)), 0x0000);
    run(m, pc);
    expect_log(m, 2, true, CPID, CIR_OPWORD, 2, OP_CP(CPID, T_GEN, EA_DN(5)));
    mach_destroy(m);

    /* transfer from instruction stream: length バイトを scanPC から。命令は拡張 word
     * の分だけ長くなる */
    m = host_machine(model);
    mach_response(m, XFER_STREAM(6));
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000, 0x1111, 0x2222, 0x3333);
    run(m, pc);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0x11112222u);
    expect_log(m, 3, true, CPID, CIR_OPERAND, 2, 0x3333u); /* 端数の 2 バイトは word */
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, XFER_STREAM(3)); /* 奇数は protocol violation */
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
    mach_destroy(m);

    /* busy: 命令を最初からやり直す。コマンドは書き直される */
    m = host_machine(model);
    mach_response(m, BUSY);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_START);
    m->nresponses = 0;
    m->next_response = 0;
    mach_response(m, NULL_PF1);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_START + 4);
    expect_log(m, 0, true, CPID, CIR_COMMAND, 2, 0);
    expect_log(m, 1, false, CPID, CIR_RESPONSE, 2, BUSY);
    expect_log(m, 2, true, CPID, CIR_COMMAND, 2, 0);
    mach_destroy(m);

    /* busy / not ready で命令をやり直すときは、トレース（T1）が有効でもトレース例外を起こさない
     */
    m = host_machine(model);
    mach_response(m, BUSY);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    m->cpu->sr |= SR_T1;
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_START);
    mach_destroy(m);

    m = host_machine(model);
    m->save_format = 0x0100;
    m->cpu->a[1] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    m->cpu->sr |= SR_T1;
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_START);
    mach_destroy(m);

    /* トレース保留中は、PF=1 の NULL が来るまで応答 CIR を読み続ける（[UM20 §7.5.2.5]） */
    m = host_machine(model);
    mach_response(m, 0x0900); /* CA=0 IA=1 PF=0: まだ処理中 */
    mach_response(m, 0x0900);
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    m->cpu->sr |= SR_T1;
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(9)); /* トレース例外 */
    int reads = 0;
    for (int i = 0; i < m->nlog; i++)
        reads += !m->log[i].write && (m->log[i].addr & 0x1F) == CIR_RESPONSE;
    CHECK_EQ(reads, 3); /* $0900、$0900、$0802 */
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, 0x0800);
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    run(m, pc);
    expect_nlog(m, 2); /* トレースしていなければ CA=0 で終わる */
    mach_destroy(m);

    /* 応答がいつまでも CA=1 のままなら、上限で protocol
     * violation（ゲストが止まらなくなるのを避ける） */
    m = host_machine(model);
    mach_response(m, NULL_AGAIN);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
    CHECK(m->nlog_total > 60000);
    mach_destroy(m);
}

/* take exception primitives とそのフレーム（[UM20 Figure 7-41、7-43、7-45]） */
static void test_take_exceptions(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* pre: format 0（4 word）。PC=命令先頭。コントロール CIR へ XA を書く */
    m = host_machine(model);
    mach_response(m, TAKE_PRE(0x30));
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000, 0x0000);
    mach_run_until(m, MACH_HANDLER(0x30), 5);
    expect_pre_frame(m, 0x30, MACH_START);
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 2);
    expect_nlog(m, 3);
    mach_destroy(m);

    /* post: format 2（6 word）。PC=scanPC（command word の次）、4 番目の long=命令先頭 */
    m = host_machine(model);
    mach_response(m, TAKE_POST(0x31));
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000, 0x0000);
    mach_run_until(m, MACH_HANDLER(0x31), 5);
    CHECK_EQ(m->cpu->a[7], MACH_STACK - 12);
    CHECK_EQ(get32(m, MACH_STACK - 12 + 2), MACH_START + 4);
    CHECK_EQ(get16(m, MACH_STACK - 12 + 6), 0x2000 | (0x31 * 4));
    CHECK_EQ(get32(m, MACH_STACK - 12 + 8), MACH_START);
    mach_destroy(m);

    /* mid: format 9（10
     * word）。scanPC、命令先頭、内部レジスタ、オペレーションワード、実効アドレス。 RTE は応答
     * CIR の読出しから再開する（command は書き直さない） */
    m = host_machine(model);
    put32(m, MACH_HANDLER(0x32) + 0, 0x4E73 << 16 | 0x4E71); /* RTE */
    put16(m, MACH_HANDLER(0x32), OP_RTE);
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 4));
    mach_response(m, TAKE_MID(0x32));
    mach_response(m, NULL_PF1);
    m->operand_in[0] = 0x12345678u;
    m->n_operand_in = 1;
    m->cpu->a[0] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6000, MOVEQ(7, 7));
    mach_run_until(m, MACH_HANDLER(0x32), 10);
    {
        const uint32_t f = m->cpu->a[7];
        CHECK_EQ(f, MACH_STACK - 20);
        CHECK_EQ(get32(m, f + 2), MACH_START + 4); /* scanPC */
        CHECK_EQ(get16(m, f + 6), 0x9000 | (0x32 * 4));
        CHECK_EQ(get32(m, f + 8), MACH_START);
        CHECK_EQ(get16(m, f + 14), OP_CP(CPID, T_GEN, EA_AN_IND(0)));
        CHECK_EQ(get32(m, f + 16), 0x4000); /* 直前に評価した実効アドレス */
    }
    const int before = m->nlog;
    run(m, pc);
    CHECK_EQ(m->cpu->d[7], 7); /* 次の命令が実行された */
    CHECK_EQ(m->cpu->a[7], MACH_STACK);
    /* 再開後のアクセスは応答 CIR の読出しだけ（command は書き直さない） */
    CHECK_EQ(m->nlog, before + 1);
    expect_log(m, before, false, CPID, CIR_RESPONSE, 2, NULL_PF1);
    CHECK_EQ(get32(m, 0x4000), 0x12345678u);
    mach_destroy(m);

    /* PC ビット付きの take では、命令アドレスを渡してから例外にする */
    m = host_machine(model);
    mach_response(m, (uint16_t)(TAKE_PRE(0x30) | 0x4000));
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    mach_run_until(m, MACH_HANDLER(0x30), 5);
    expect_log(m, 2, true, CPID, CIR_IA, 4, MACH_START);
    expect_log(m, 3, true, CPID, CIR_CONTROL, 2, 2);
    mach_destroy(m);

    /* 条件系でも take は使える。pre フレーム */
    m = host_machine(model);
    mach_response(m, TAKE_PRE(0x30));
    PROG(m, MACH_START, OP_CP(CPID, T_BCC_W, 0x01), 0x0004);
    mach_run_until(m, MACH_HANDLER(0x30), 5);
    expect_pre_frame(m, 0x30, MACH_START);
    mach_destroy(m);
}

/* MPU が検出する protocol violation: vector 13、format 9、コントロール CIR には書かない */
static void test_protocol_violation(mc68k_model_t model) {
    mach_t *m;

    static const uint16_t undefined[] = {
        0x0000, 0x3F00, 0x0B00, 0x1800, 0x1B00, 0x1F00,
        0x2800, 0x2B00, 0x3800, 0x3B00, /* [UM20 §7.6] の未定義 */
        0x3C00, 0x3D00, 0x3E00, /* take exception の DR=1（DR を除いた bit12-8 だけ見ると take
                                   に見える） */
        0x8200, 0x8300,         /* transfer SR and scanPC（68881 は使わないので未実装） */
        0x8500, 0x8600, 0x8A00, 0x8D00, 0x8E00, /* take address / MP registers / EA address / MP
                                                   control / top of stack */
    };
    for (unsigned i = 0; i < sizeof(undefined) / sizeof(undefined[0]); i++) {
        m = host_machine(model);
        mach_response(m, undefined[i]);
        mach_response(m, NULL_PF1);
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(2)), 0x0000, 0x0000);
        mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
        const uint32_t f = m->cpu->a[7];
        CHECK_EQ(f, MACH_STACK - 20);
        CHECK_EQ(get32(m, f + 2), MACH_START + 4);
        CHECK_EQ(get16(m, f + 6), 0x9000 | (VEC_VIOLATION * 4));
        CHECK_EQ(get32(m, f + 8), MACH_START);
        CHECK_EQ(get16(m, f + 14), OP_CP(CPID, T_GEN, EA_AN_IND(2)));
        expect_nlog(m, 2); /* command と応答の読出しだけ。control CIR には書かない */
        mach_destroy(m);
    }

    /* undefined でも PC ビットが立っていれば、命令アドレスを渡してから例外にする */
    m = host_machine(model);
    mach_response(m, 0x4000);
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
    expect_log(m, 2, true, CPID, CIR_IA, 4, MACH_START);
    expect_nlog(m, 3);
    mach_destroy(m);

    /* 条件系: EA 系 primitive、CA=0 の * 付き primitive は protocol violation */
    static const uint16_t cond_bad[] = {EA_TO_CP(VEA_ANY, 4),
                                        EA_TO_MPU(VEA_ANY, 4),
                                        FPREGS_TO_CP(12),
                                        0x0400 /* supervisor check CA=0 */,
                                        0x0700 /* operation word CA=0 */,
                                        0x0F02 /* instruction stream CA=0 */,
                                        0x0C00 /* MP register CA=0 */};
    for (unsigned i = 0; i < sizeof(cond_bad) / sizeof(cond_bad[0]); i++) {
        m = host_machine(model);
        mach_response(m, cond_bad[i]);
        mach_response(m, NULL_TF1);
        PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(0)), 0x0001);
        mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
        mach_destroy(m);
    }

    /* 条件系でも CA=1 の primitive は使える（読み直しに戻る）。ここでは MP register の後に TF=1
     */
    m = host_machine(model);
    mach_response(m, MPREG_TO_CP(1));
    mach_response(m, NULL_TF1);
    m->cpu->d[1] = 0x55;
    {
        const uint32_t pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(0)), 0x0001);
        run(m, pc);
        CHECK_EQ(m->cpu->d[0] & 0xFF, 0xFF);
    }
    expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0x55);
    mach_destroy(m);
}

/* CIR のバスサイクルは access_timing に fc=7、CPU
 * 空間アドレスで渡る。待ちは命令のサイクルに積まれる */
static int timing_calls;
static uint32_t timing_last_addr;
static uint8_t timing_last_fc;
static void timing_probe(void *ctx, const mc68k_bus_access_t *a, mc68k_bus_timing_t *out) {
    (void)ctx;
    if (a->fc == 7) {
        timing_calls++;
        timing_last_addr = a->physical;
        timing_last_fc = a->fc;
        out->wait_cycles = 5;
    }
}

static void test_bus_timing(mc68k_model_t model) {
    int cycles[2];

    for (int with_wait = 0; with_wait < 2; with_wait++) {
        mach_t *m = host_machine(model);
        if (with_wait)
            m->cpu->bus.access_timing = timing_probe;
        timing_calls = 0;
        mach_response(m, NULL_PF1);
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
        cycles[with_wait] = mc68k_execute(m->cpu, 1);
        mach_destroy(m);
    }
    CHECK_EQ(timing_calls, 2); /* command の書込みと応答の読出し */
    CHECK_EQ(timing_last_fc, 7);
    CHECK_EQ(timing_last_addr, 0x20000u + (CPID << 13) + CIR_RESPONSE);
    CHECK_EQ(cycles[1], cycles[0] + 2 * 5);
}

/* メモリアクセスのバスエラー（RAM の外）。対話中ならコントロール CIR の AB
 * で命令を中止してから通常の バスエラー。対話の前後（cpRESTORE の format word 読出し、cpScc
 * の書込み）ではコプロセッサに触れない */
static void test_memory_bus_errors(mc68k_model_t model) {
    mach_t *m;
    const uint32_t outside = MACH_RAM + 0x100;

    m = host_machine(model);
    mach_response(m, EA_TO_CP(VEA_MEM, 4));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = outside;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x4000);
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    expect_nlog(m, 3);
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
    CHECK_EQ(m->cpu->a[0], outside);
    mach_destroy(m);

    /* (A0)+ のレジスタ更新は転送が全部成功した後。失敗したら A0 は元のまま */
    m = host_machine(model);
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 4));
    mach_response(m, NULL_PF1);
    m->operand_in[0] = 1;
    m->n_operand_in = 1;
    m->cpu->a[0] = outside;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_POST(0)), 0x6000);
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    CHECK_EQ(m->cpu->a[0], outside);
    expect_log(m, 3, true, CPID, CIR_CONTROL, 2, 1);
    mach_destroy(m);

    /* cpScc: 対話は終わっているので AB は書かない */
    m = host_machine(model);
    mach_response(m, NULL_TF1);
    m->cpu->a[0] = outside;
    PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_AN_IND(0)), 0x0001);
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    expect_nlog(m, 2);
    mach_destroy(m);

    /* cpRESTORE: format word の読出しが失敗しても、まだコプロセッサに触れていない */
    m = host_machine(model);
    m->cpu->a[1] = outside;
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    expect_nlog(m, 0);
    mach_destroy(m);

    /* cpSAVE: body の書込みが途中で失敗したら AB を書く */
    m = host_machine(model);
    m->save_format = 0x1F08;
    m->operand_in[0] = 1;
    m->operand_in[1] = 2;
    m->n_operand_in = 2;
    m->cpu->a[1] = MACH_RAM - 8; /* format long は入るが +8 は RAM の外 */
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
    expect_nlog(m, 3);
    mach_destroy(m);
}

/* レビュー（Opus、A-B1〜A-B10）で指摘された点の回帰試験 */

/* A-B2: cpGEN の supervisor check は、bit15（CA）によらず S=1 なら応答 CIR を読み直す（[UM20
 * §7.4.5]） */
static void test_supervisor_check_ca0(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(m, 0x0400); /* CA=0 の supervisor check */
    mach_response(m, NULL_PF1);
    const uint32_t pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    run(m, pc);
    expect_log(m, 1, false, CPID, CIR_RESPONSE, 2, 0x0400);
    expect_log(m, 2, false, CPID, CIR_RESPONSE, 2, NULL_PF1);
    expect_nlog(m, 3);
    mach_destroy(m);
}

/* A-B3: トレース保留中は、CA=0 の Null 以外の primitive の後も PF=1 の Null
 * まで読み続ける（[UM20 §7.4.4]） */
static void test_trace_pending_non_null(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(
        m, EA_TO_CP_LAST(VEA_DATA, 4)); /* CA=0 の EA 転送（[881UM Table 7-7] の $1504） */
    mach_response(m, 0x0900);
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0x0000, 0x0003);
    m->cpu->sr |= SR_T1;
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(9));
    int reads = 0;
    for (int i = 0; i < m->nlog; i++)
        reads += !m->log[i].write && (m->log[i].addr & 0x1F) == CIR_RESPONSE;
    CHECK_EQ(reads, 3);
    mach_destroy(m);
}

/* A-B4: コプロセッサの対話を再開しない format 9 を RTE
 * で戻しても、トレースフレームの命令アドレスは RTE 自身のアドレス（[UM20 Table
 * 6-5]）。再開しないので instr_pc を書き換えてはならない */
static void test_rte_format9_keeps_instr_pc(mc68k_model_t model) {
    mach_t *m = mach_create(model, MC68K_FPU_NONE, false);
    mc68k_state_t *cpu = m->cpu;

    cpu->a[7] = 0x5000;
    put16(m, 0x5000, 0xA000); /* SR: S + T1 */
    put32(m, 0x5002, 0x5550); /* 復帰先 PC */
    put16(m, 0x5006, 0x9000); /* format 9 */
    put32(m, 0x5008, 0x7777); /* フレームの命令アドレス */
    put16(m, 0x500E, 0x0000); /* オペレーションワード: F-line ではない */
    PROG(m, MACH_START, OP_RTE);
    cpu->sr |= SR_T1;
    mc68k_execute(cpu, 1);
    CHECK_EQ(mc68k_get_pc(cpu), MACH_HANDLER(9));
    CHECK_EQ(get32(m, cpu->a[7] + 2), 0x5550);
    CHECK_EQ(get32(m, cpu->a[7] + 8), MACH_START);
    mach_destroy(m);
}

/* A-B5: コプロセッサの有無にかかわらず、ユーザモードの cpSAVE/cpRESTORE は CIR
 * に触れる前に特権違反。 スーパーバイザでコプロセッサが無ければ F-line */
static void test_privilege_before_presence(mc68k_model_t model) {
    for (unsigned restore = 0; restore < 2; restore++) {
        mach_t *m = mach_create(model, MC68K_FPU_NONE, false);

        m->cpu->a[1] = 0x4000;
        PROG(m, MACH_START, OP_CP(CPID, restore ? T_RESTORE : T_SAVE, EA_AN_IND(1)));
        cpu_set_sr(m->cpu, 0x0000);
        m->cpu->ssp = MACH_STACK;
        mach_run_until(m, MACH_HANDLER(VEC_PRIV), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_PRIV));
        mach_destroy(m);

        m = mach_create(model, MC68K_FPU_NONE, false);
        m->cpu->a[1] = 0x4000;
        m->cpu->a[7] = MACH_STACK;
        PROG(m, MACH_START, OP_CP(CPID, restore ? T_RESTORE : T_SAVE, EA_AN_IND(1)));
        mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FLINE));
        mach_destroy(m);
    }
}

/* A-B7: レジスタセレクト CIR は実効アドレスの計算（拡張 word のフェッチ）の後に読む。
 * 拡張 word を持つ EA（d16(A0)）では、ログ上の CIR アクセスの前に拡張 word
 * が消費済みで、命令長にも出る */
static void test_fp_registers_order_and_mask(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;
    /* d16(A0) へ、mask の下位 byte（0x0005）と上位 byte（0x8001）の両方が数えられる */
    static const struct {
        uint16_t mask;
        unsigned count;
    } masks[] = {{0x0005, 2}, {0x8001, 2}, {0x8100, 2}, {0x0000, 0}, {0xFFFF, 16}};
    for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        m = host_machine(model);
        m->operand_in[0] = 0;
        for (unsigned k = 0; k < 16; k++)
            m->operand_in[k] = 0x100u + k;
        m->n_operand_in = 16;
        m->regsel = masks[i].mask;
        mach_response(m, FPREGS_TO_MPU(4));
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = 0x4000;
        pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0x28), 0xF000, 0x0010);
        run(m, pc);
        /* 長さ 4、オペランド count 個。先頭アドレスは A0+0x10 */
        int ops_read = 0;
        for (int j = 0; j < m->nlog; j++)
            ops_read += !m->log[j].write && (m->log[j].addr & 0x1F) == CIR_OPERAND;
        CHECK_EQ(ops_read, (int)masks[i].count);
        for (unsigned k = 0; k < masks[i].count; k++)
            CHECK_EQ(get32(m, 0x4010 + 4 * k), 0x100u + k);
        mach_destroy(m);
    }
}

/* A-B7（順序）: 拡張 word のフェッチが失敗する EA では、レジスタセレクト CIR
 * を読む前にバスエラーになる */
static void test_fp_registers_ea_before_regsel(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    m->regsel = 0x0300;
    mach_response(m, FPREGS_TO_MPU(12));
    mach_response(m, NULL_PF1);
    put16(m, MACH_RAM - 4, OP_CP(CPID, T_GEN, 0x28)); /* d16(A0): 拡張 word は RAM の外 */
    put16(m, MACH_RAM - 2, 0xF000);
    m->cpu->pc = MACH_RAM - 4;
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    for (int i = 0; i < m->nlog; i++)
        CHECK((m->log[i].addr & 0x1F) != CIR_REGSEL);
    mach_destroy(m);
}

/* A-B8: A7 の (A7)+ のバイト転送は 2 進む。cpBcc.L の変位は 32bit（上位 word を使う）。
 * 16bit CIR の読出しは下位 size バイトだけ使う（上位に混入した値を無視する） */
static void test_misc_boundaries(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    m = host_machine(model);
    m->operand_in[0] = 0x55;
    m->n_operand_in = 1;
    mach_response(m, EA_TO_MPU(VEA_DATA_ALT, 1));
    mach_response(m, NULL_PF1);
    m->cpu->a[7] = 0x6000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_POST(7)), 0x6000);
    run(m, pc);
    CHECK_EQ(m->cpu->a[7], 0x6002);
    CHECK_EQ(m->ram[0x6000], 0x55);
    mach_destroy(m);

    /* 後方への cpBcc.L。変位は 32bit（上位 word が非 0 で、下位 word は正）。0xF900
     * の命令の変位ワードの先頭 0xF902 から 0xF800 戻った 0x0102 で MOVEQ を実行する */
    m = host_machine(model);
    mach_response(m, NULL_TF1);
    put16(m, 0x0102, MOVEQ(5, 1));
    m->cpu->pc = 0xF900;
    PROG(m, 0xF900, OP_CP(CPID, T_BCC_L, 0x01), 0xFFFF, 0x0800);
    run(m, 0x0104);
    CHECK_EQ(m->cpu->d[1], 5);
    mach_destroy(m);

    /* 応答 CIR の上位 16bit に値が混ざっても、下位 16bit だけを使う */
    m = host_machine(model);
    m->dirty_upper = true;
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    run(m, pc);
    mach_destroy(m);

    /* take post の後はトレース例外が続く（命令は完了または中断済み。[UM20 §7.4.20]）。take pre
     * では 命令が完了しておらず RTE で再実行されるのでトレースしない（[UM20 §7.4.18]） */
    for (unsigned post = 0; post < 2; post++) {
        m = host_machine(model);
        mach_response(m, post ? TAKE_POST(0x31) : TAKE_PRE(0x30));
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
        m->cpu->sr |= SR_T1;
        mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu), post ? MACH_HANDLER(9) : MACH_HANDLER(0x30));
        mach_destroy(m);
    }
}

/* A-B10: RTE 再開。PC ビット付きの primitive は、フレームの命令アドレスを渡す。条件命令の再開は
 * コンディション CIR を書き直さず、応答 CIR の読出しから。再度の mid 例外のフレームは EA
 * を引き継ぐ */
static void test_resume_details(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    m = host_machine(model);
    put16(m, MACH_HANDLER(0x32), OP_RTE);
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 4));
    mach_response(m, TAKE_MID(0x32));
    mach_response(m, (uint16_t)(0x4000 | NULL_PF1)); /* 再開後: PC=1 の最終応答 */
    m->operand_in[0] = 0x12345678u;
    m->n_operand_in = 1;
    m->cpu->a[0] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6000);
    mach_run_until(m, MACH_HANDLER(0x32), 10);
    CHECK_EQ(get16(m, m->cpu->a[7] + 12), 0); /* 内部レジスタ欄は 0 */
    run(m, pc);
    const int n = m->nlog;
    expect_log(m, n - 2, false, CPID, CIR_RESPONSE, 2, 0x4000 | NULL_PF1);
    expect_log(m, n - 1, true, CPID, CIR_IA, 4, MACH_START);
    mach_destroy(m);

    /* 条件命令（cpBcc.W）: mid 例外のあと RTE で再開し、コンディション CIR
     * は書き直さず、分岐する */
    m = host_machine(model);
    put16(m, MACH_HANDLER(0x32), OP_RTE);
    mach_response(m, TAKE_MID(0x32));
    mach_response(m, NULL_TF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_BCC_W, 0x01), 0x0004, MOVEQ(1, 0), MOVEQ(2, 1));
    run(m, pc);
    CHECK_EQ(m->cpu->d[0], 0);
    CHECK_EQ(m->cpu->d[1], 2);
    expect_log(m, 0, true, CPID, CIR_CONDITION, 2, OP_CP(CPID, T_BCC_W, 0x01));
    expect_log(m, 1, false, CPID, CIR_RESPONSE, 2, TAKE_MID(0x32));
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 2);
    expect_log(m, 3, false, CPID, CIR_RESPONSE, 2, NULL_TF1);
    expect_nlog(m, 4);
    mach_destroy(m);

    /* A-B48: cpBcc.L（type 3）も同じ。変位 long の上位ワード $0000 を命令として実行しない */
    m = host_machine(model);
    put16(m, MACH_HANDLER(0x32), OP_RTE);
    mach_response(m, TAKE_MID(0x32));
    mach_response(m, NULL_TF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_BCC_L, 0x01), 0x0000, 0x0006, MOVEQ(1, 0),
              MOVEQ(2, 1));
    run(m, pc);
    CHECK_EQ(m->cpu->d[0], 0);
    CHECK_EQ(m->cpu->d[1], 2);
    expect_log(m, 0, true, CPID, CIR_CONDITION, 2, OP_CP(CPID, T_BCC_L, 0x01));
    expect_log(m, 1, false, CPID, CIR_RESPONSE, 2, TAKE_MID(0x32));
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 2);
    expect_log(m, 3, false, CPID, CIR_RESPONSE, 2, NULL_TF1);
    expect_nlog(m, 4);
    mach_destroy(m);

    /* 再度の mid 例外のフレームは、再開時に渡した EA を引き継ぐ（フレームの EA 欄 → 再開 →
     * 次のフレーム） */
    m = host_machine(model);
    put16(m, MACH_HANDLER(0x32), OP_RTE);
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 4));
    mach_response(m, TAKE_MID(0x32));
    mach_response(m, TAKE_MID(0x32));
    mach_response(m, NULL_PF1);
    m->operand_in[0] = 1;
    m->n_operand_in = 1;
    m->cpu->a[0] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6000);
    mach_run_until(m, MACH_HANDLER(0x32), 10);
    mc68k_execute(m->cpu, 1); /* RTE → 再開 → 2 回目の mid 例外 */
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(0x32));
    CHECK_EQ(get32(m, m->cpu->a[7] + 16), 0x4000);
    CHECK_EQ(get32(m, m->cpu->a[7] + 8), MACH_START);
    mach_destroy(m);
}

/* 再レビュー（Opus、A-B12〜A-B14） */

/* A-B12: トレース保留でなければ、CA=0 の primitive（Null 以外も）で対話を終える。応答 CIR
 * を余分に読まない */
static void test_ca0_terminates_without_trace(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(m, EA_TO_CP_LAST(VEA_DATA, 4));
    mach_response(m, NULL_PF1);
    const uint32_t pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0x0000, 0x0003);
    run(m, pc);
    int reads = 0;
    for (int i = 0; i < m->nlog; i++)
        reads += !m->log[i].write && (m->log[i].addr & 0x1F) == CIR_RESPONSE;
    CHECK_EQ(reads, 1);
    mach_destroy(m);
}

/* A-B13: RTE が再開しない format 9 は、従来どおり復帰だけをする（CIR に触れない）。
 * (a) コプロセッサが無い、(b) オペレーションワードが cpSAVE/cpRESTORE（mid フレームは cpGEN
 * と条件系だけ） */
static void test_rte_format9_not_resumed(mc68k_model_t model) {
    for (unsigned variant = 0; variant < 3; variant++) {
        mach_t *m =
            variant == 0 ? mach_create(model, MC68K_FPU_NONE, false) : host_machine(model);
        mc68k_state_t *cpu = m->cpu;
        uint16_t opword = OP_CP(CPID, T_GEN, 0);

        if (variant == 1)
            opword = OP_CP(CPID, T_SAVE, EA_AN_IND(1));
        else if (variant == 2)
            opword = OP_CP(CPID, T_RESTORE, EA_AN_IND(1));
        cpu->a[7] = 0x5000;
        put16(m, 0x5000, 0x2000);
        put32(m, 0x5002, 0x1234);
        put16(m, 0x5006, 0x9000);
        put32(m, 0x5008, 0x7777);
        put16(m, 0x500E, opword);
        put16(m, 0x1234, 0x4E71);
        PROG(m, MACH_START, OP_RTE);
        mc68k_execute(cpu, 1);
        CHECK_EQ(mc68k_get_pc(cpu), 0x1234);
        CHECK_EQ(cpu->a[7], 0x5000 + 20);
        CHECK_EQ(m->nlog_total, 0);
        mach_destroy(m);
    }
}

/* A-B14: EA が不正な cpSAVE/cpRESTORE は、ユーザモードでも特権違反ではなく
 * F-line（形式の検査が先） */
static void test_invalid_ea_before_privilege(mc68k_model_t model) {
    static const struct {
        unsigned type;
        uint16_t ea;
    } bad[] = {{T_SAVE, EA_AN_POST(1)},
               {T_SAVE, EA_DN(0)},
               {T_RESTORE, EA_AN_PRE(1)},
               {T_RESTORE, EA_DN(0)}};

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        mach_t *m = host_machine(model);

        m->cpu->a[1] = 0x4000;
        PROG(m, MACH_START, OP_CP(CPID, bad[i].type, bad[i].ea));
        cpu_set_sr(m->cpu, 0x0000);
        m->cpu->ssp = MACH_STACK;
        mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FLINE));
        expect_nlog(m, 0);
        mach_destroy(m);
    }
}

/* ===== 再々レビュー（Opus、A-B17〜A-B29） ===== */

/* A-B17: cpRESTORE の長さの検査はメモリの format word で行い、転送する長さもメモリのもの。
 * 読み返し（コプロセッサの判定）のコードは empty / invalid / valid の判定にだけ使う */
static void test_restore_length_from_memory(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* メモリの長さが 4 の倍数でない valid。読み返しが empty でも format error（リストア CIR
     * を読んだ後に AB） */
    m = host_machine(model);
    m->restore_readback = 0x0000;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1F06FFFFu);
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_FORMAT), 5);
    expect_pre_frame(m, VEC_FORMAT, MACH_START);
    expect_log(m, 0, true, CPID, CIR_RESTORE, 2, 0x1F06);
    expect_log(m, 1, false, CPID, CIR_RESTORE, 2, 0x0000);
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
    expect_nlog(m, 3);
    mach_destroy(m);

    /* 読み返しの長さがメモリと違っても、転送するのはメモリの長さ（8 バイト = 2 long） */
    m = host_machine(model);
    m->restore_readback = 0x1F04;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1F08FFFFu);
    put32(m, 0x4004, 0xAAAAAAAAu);
    put32(m, 0x4008, 0xBBBBBBBBu);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_POST(1)));
    run(m, pc);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0xAAAAAAAAu);
    expect_log(m, 3, true, CPID, CIR_OPERAND, 4, 0xBBBBBBBBu);
    expect_nlog(m, 4);
    CHECK_EQ(m->cpu->a[1], 0x400C);
    mach_destroy(m);
}

/* A-B17（続き）: 転送の有無は読み返し（コプロセッサの判定）で決まり、長さはメモリの値 */
static void test_restore_transfer_follows_readback(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    m->restore_readback = 0x0000; /* コプロセッサは empty と判定 → 転送しない */
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1F08FFFFu);
    uint32_t pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_POST(1)));
    run(m, pc);
    expect_nlog(m, 2);
    CHECK_EQ(m->cpu->a[1], 0x4004);
    mach_destroy(m);

    m = host_machine(model);
    m->restore_readback =
        0x1F08; /* コプロセッサが valid と判定 → メモリの長さ（8）だけ転送する */
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x0008FFFFu);
    put32(m, 0x4004, 0xAAAAAAAAu);
    put32(m, 0x4008, 0xBBBBBBBBu);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_POST(1)));
    run(m, pc);
    expect_nlog(m, 4);
    CHECK_EQ(m->cpu->a[1], 0x400C);
    mach_destroy(m);
}

/* A-B18: cpScc の EA が mode 7・reg 5〜7（存在しない形式）は CIR に触れず F-line */
static void test_scc_invalid_ea(mc68k_model_t model) {
    for (unsigned reg = 5; reg < 8; reg++) {
        mach_t *m = host_machine(model);

        PROG(m, MACH_START, OP_CP(CPID, T_COND, 0x38 | reg), 0x0001);
        mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
        expect_pre_frame(m, VEC_FLINE, MACH_START);
        expect_nlog(m, 0);
        mach_destroy(m);
    }
}

/* A-B19: 分岐時トレース（T1,T0=01。[UM20 §7.5.2.5]）。分岐した cpBcc・cpDBcc
 * だけがトレースされる。 分岐しない cpBcc・cpDBcc（条件が真、カウンタが尽きた）、不成立の
 * cpTRAPcc、cpGEN はされない */
static void test_trace_on_change_of_flow(mc68k_model_t model) {
    static const struct {
        const char *name;
        uint16_t resp;
        uint16_t words[4];
        unsigned nwords;
        bool traced;
        uint32_t d0;
    } cases[] = {
        {"cpBcc taken", NULL_TF1, {OP_CP(CPID, T_BCC_W, 1), 0x0004, 0, 0}, 2, true, 0},
        {"cpBcc not taken", NULL_TF0, {OP_CP(CPID, T_BCC_W, 1), 0x0004, 0, 0}, 2, false, 0},
        {"cpDBcc loops", NULL_TF0, {OP_CP(CPID, T_COND, 0x08), 0x0001, 0x0004, 0}, 3, true, 5},
        {"cpDBcc exhausted",
         NULL_TF0,
         {OP_CP(CPID, T_COND, 0x08), 0x0001, 0x0004, 0},
         3,
         false,
         0},
        {"cpDBcc condition true",
         NULL_TF1,
         {OP_CP(CPID, T_COND, 0x08), 0x0001, 0x0004, 0},
         3,
         false,
         5},
        {"cpTRAPcc false", NULL_TF0, {OP_CP(CPID, T_COND, 0x3C), 0x0001, 0, 0}, 2, false, 0},
        {"cpGEN", NULL_PF1, {OP_CP(CPID, T_GEN, 0), 0x0000, 0, 0}, 2, false, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mach_t *m = host_machine(model);

        mach_response(m, cases[i].resp);
        m->cpu->d[0] = cases[i].d0;
        prog(m, MACH_START, cases[i].words, cases[i].nwords);
        m->cpu->sr |= SR_T0;
        mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu) == MACH_HANDLER(9), cases[i].traced);
        mach_destroy(m);
    }
}

/* A-B20: 条件系の対話の終了条件。CA=1 の Null は読み直す（Table
 * 7-3）。トレース（T1）でも条件系の プロトコルは変わらず、CA=0 の Null（PF=0
 * でも）で終わる（[UM20 §7.5.2.5]） */
static void test_conditional_termination(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(m, NULL_AGAIN);
    mach_response(m, NULL_TF1);
    mach_response(m, NULL_TF0); /* 読まれてはいけない */
    uint32_t pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(3)), 0x0001);
    run(m, pc);
    CHECK_EQ(m->cpu->d[3] & 0xFF, 0xFF);
    expect_log(m, 1, false, CPID, CIR_RESPONSE, 2, NULL_AGAIN);
    expect_log(m, 2, false, CPID, CIR_RESPONSE, 2, NULL_TF1);
    expect_nlog(m, 3);
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, NULL_TF1); /* CA=0 PF=0 TF=1 */
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(3)), 0x0001);
    m->cpu->sr |= SR_T1;
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(m->cpu->d[3] & 0xFF, 0xFF);
    expect_nlog(m, 2);
    mach_destroy(m);
}

/* A-B21: Evaluate EA and transfer data の valid EA 分類（[UM20 Table 7-4]）。この表は
 * production から 取らず、定義から手で書いた。ビット i = 分類 i（0=制御可変 1=データ可変
 * 2=メモリ可変 3=可変 4=制御 5=データ 6=メモリ 7=制限なし）に属する */
static void test_valid_ea_classes(mc68k_model_t model) {
    static const struct {
        const char *name;
        uint16_t ea;
        unsigned words;
        uint8_t mask;
    } eas[] = {
        {"Dn", EA_DN(2), 0, (1u << 1) | (1u << 3) | (1u << 5) | (1u << 7)},
        {"An", 0x0A, 0, (1u << 3) | (1u << 7)},
        {"(An)", EA_AN_IND(0), 0, 0xFF},
        {"(An)+", EA_AN_POST(0), 0,
         (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7)},
        {"-(An)", EA_AN_PRE(0), 0,
         (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7)},
        {"d16(An)", 0x28, 1, 0xFF},
        {"abs.W", 0x38, 1, 0xFF},
        {"abs.L", 0x39, 2, 0xFF},
        {"d16(PC)", 0x3A, 1, (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7)},
        {"#imm", EA_IMM, 2, (1u << 5) | (1u << 6) | (1u << 7)},
        {"invalid 7/5", 0x3D, 0, 0}, /* 存在しない符号化はどの分類にも属さない */
        {"invalid 7/6", 0x3E, 0, 0},
        {"invalid 7/7", 0x3F, 0, 0},
    };
    int cases = 0;

    for (unsigned c = 0; c < 8; c++) {
        for (unsigned i = 0; i < sizeof(eas) / sizeof(eas[0]); i++) {
            mach_t *m = host_machine(model);
            uint16_t w[6] = {OP_CP(CPID, T_GEN, eas[i].ea), 0x4000, 0, 0, 0, 0};

            m->cpu->a[0] = 0x4000;
            mach_response(m, EA_TO_CP(c, 4));
            mach_response(m, NULL_PF1);
            prog(m, MACH_START, w, 2 + eas[i].words);
            mc68k_execute(m->cpu, 1);
            const bool accepted = mc68k_get_pc(m->cpu) != MACH_HANDLER(VEC_FLINE);
            CHECK_EQ(accepted, (eas[i].mask >> c) & 1u);
            if (accepted)
                CHECK_EQ(mc68k_get_pc(m->cpu), MACH_START + 4 + 2 * eas[i].words);
            cases++;
            mach_destroy(m);
        }
    }
    CHECK_EQ(cases, 104);
}

/* A-B22: 再開しない format 9 の残り（cpID=0、F-line でない word、無効な EA の
 * cpScc）。ホストのフックがあっても CIR に触れず復帰だけ（設計書 §3.8） */
static void test_rte_format9_not_resumed_more(mc68k_model_t model) {
    /* A-B45: 上位 4 ビット（F）を 1 つずつ落とした cpID=1・type 0 の
     * word（$7200・$B200・$D200・$E200）も F-line でない */
    static const uint16_t words[] = {0xF000,
                                     0x7000,
                                     0x3200 /* cpID=1 の形だが F-line でない */,
                                     0x7200,
                                     0xB200,
                                     0xD200,
                                     0xE200,
                                     OP_CP(CPID, T_COND, 0x3D),
                                     OP_CP(CPID, T_COND, 0x3F)};

    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        mach_t *m = host_machine(model);

        m->cpu->a[7] = 0x5000;
        put16(m, 0x5000, 0x2000);
        put32(m, 0x5002, 0x1234);
        put16(m, 0x5006, 0x9000);
        put16(m, 0x500E, words[i]);
        put16(m, 0x1234, 0x4E71);
        PROG(m, MACH_START, OP_RTE);
        mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu), 0x1234);
        CHECK_EQ(m->cpu->a[7], 0x5000 + 20);
        CHECK_EQ(m->cpu->sr, 0x2000);
        CHECK_EQ(m->nlog_total, 0);
        mach_destroy(m);
    }
}

/* A-B23: take 系に CA は無い。CA=1 は protocol violation */
static void test_take_with_ca(mc68k_model_t model) {
    for (unsigned k = 0; k < 3; k++) {
        mach_t *m = host_machine(model);

        mach_response(m, (uint16_t)(0x8000u | (0x1C00u + 0x100u * k) | 0x30));
        mach_response(m, NULL_PF1);
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
        mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
        mach_destroy(m);
    }
}

/* A-B24: A7 でも長さはそのまま増減する（1 バイトだけ 2。[UM20 §7.4.9]）。FP
 * レジスタ群の長さは偶数なら有効 */
static void test_length_boundaries(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    for (unsigned pre = 0; pre < 2; pre++) {
        m = host_machine(model);
        m->operand_in[0] = 0x00112233u;
        m->n_operand_in = 1;
        mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 3));
        mach_response(m, NULL_PF1);
        m->cpu->a[7] = 0x6000;
        pc =
            PROG(m, MACH_START, OP_CP(CPID, T_GEN, pre ? EA_AN_PRE(7) : EA_AN_POST(7)), 0x6000);
        run(m, pc);
        CHECK_EQ(m->cpu->a[7], pre ? 0x6000u - 3 : 0x6000u + 3);
        mach_destroy(m);
    }

    /* FP レジスタ群: 長さ 2（1 個）と 6（4 + 2 バイト）。-(A0) と (A0) */
    static const struct {
        unsigned len;
        unsigned count;
    } lens[] = {{2, 2}, {6, 1}, {6, 2}};
    for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        m = host_machine(model);
        for (unsigned k = 0; k < 12; k++)
            m->operand_in[k] = 0x01020304u + k;
        m->n_operand_in = 12;
        m->regsel = (uint16_t)((1u << lens[i].count) - 1u);
        mach_response(m, FPREGS_TO_MPU(lens[i].len));
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = 0x4000;
        pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0xF000);
        run(m, pc);
        CHECK_EQ(m->cpu->last_exception_vector, 0);
        mach_destroy(m);
    }
}

/* A-B26: バスサイクルの size と read が access_timing へ正しく渡る（オペランド CIR の 3
 * バイト転送は size 3） */
#define TIMING_MAX 16
static struct {
    uint8_t size;
    uint8_t fc;
    bool read;
    uint32_t addr;
} timing_log[TIMING_MAX];
static int timing_n;
static void timing_recorder(void *ctx, const mc68k_bus_access_t *a, mc68k_bus_timing_t *out) {
    (void)ctx;
    (void)out;
    if (a->fc == 7 && timing_n < TIMING_MAX) {
        timing_log[timing_n].size = a->size;
        timing_log[timing_n].fc = a->fc;
        timing_log[timing_n].read = a->read;
        timing_log[timing_n].addr = a->physical;
        timing_n++;
    }
}

static void test_bus_timing_size_and_direction(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    m->cpu->bus.access_timing = timing_recorder;
    timing_n = 0;
    m->operand_in[0] = 0x00112233u;
    m->n_operand_in = 1;
    mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 3));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    const uint32_t pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6000);
    run(m, pc);
    /* command 書込み(2)、応答読出し(2)、operand 読出し(3)、応答読出し(2) */
    static const struct {
        uint8_t size;
        bool read;
        unsigned cir;
    } want[] = {{2, false, CIR_COMMAND},
                {2, true, CIR_RESPONSE},
                {3, true, CIR_OPERAND},
                {2, true, CIR_RESPONSE}};
    CHECK_EQ(timing_n, 4);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(timing_log[i].size, want[i].size);
        CHECK_EQ(timing_log[i].read, want[i].read);
        CHECK_EQ(timing_log[i].fc, 7);
        CHECK_EQ(timing_log[i].addr, 0x20000u + (CPID << 13) + want[i].cir);
    }
    mach_destroy(m);
}

/* A-B27: (An)+ と -(An)
 * のレジスタ更新は、転送が全部成功した後。途中のバスエラーでは変わらない。 ただし 68020 では
 * group0 の処理が失敗した命令のレジスタを命令開始時の値へ戻すので、commit
 * の順序は区別できない。
 * この試験が確認するのは「バスエラーの後に命令開始時の値に戻っている」ことだけ（A-N65） */
static void test_autoinc_not_committed_on_fault(mc68k_model_t model) {
    for (unsigned pre = 0; pre < 2; pre++) {
        mach_t *m = host_machine(model);
        const uint32_t start =
            pre ? MACH_RAM + 8 : MACH_RAM - 8; /* 12 バイトの転送が RAM の端をまたぐ */

        for (unsigned k = 0; k < 3; k++)
            m->operand_in[k] = 0x11111111u * (k + 1);
        m->n_operand_in = 3;
        mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 12));
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = start;
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, pre ? EA_AN_PRE(0) : EA_AN_POST(0)), 0x6800);
        mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
        CHECK_EQ(m->cpu->a[0], start);
        mach_destroy(m);
    }
}

/* A-B28: 例外の属性。(a) フレームに積む SR は例外前のもの（ユーザモードなら S=0）、(b) CIR
 * バスエラーの R/W、(c) 最後に起きた例外のベクタ */
static void test_exception_attributes(mc68k_model_t model) {
    static const struct {
        unsigned kind;
        unsigned vec;
        unsigned frame_bytes;
    } kinds[] = {{0, 0x30, 8}, {1, 0x31, 12}, {2, 0x32, 20}};
    const uint32_t user_sp = 0x5000;

    for (unsigned i = 0; i < 3; i++) {
        mach_t *m = host_machine(model);

        mach_response(m, kinds[i].kind == 0   ? TAKE_PRE(kinds[i].vec)
                         : kinds[i].kind == 1 ? TAKE_POST(kinds[i].vec)
                                              : TAKE_MID(kinds[i].vec));
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
        cpu_set_sr(m->cpu, 0x0005); /* ユーザモード、CCR = Z と C */
        m->cpu->ssp = MACH_STACK;
        m->cpu->a[7] = user_sp;
        mach_run_until(m, MACH_HANDLER(kinds[i].vec), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(kinds[i].vec));
        /* A-B42: フレームはスーパーバイザスタックに積む（[UM20 §6.1]）。USP は動かない */
        CHECK_EQ(m->cpu->a[7], MACH_STACK - kinds[i].frame_bytes);
        CHECK_EQ(m->cpu->usp, user_sp);
        CHECK_EQ(get16(m, m->cpu->a[7]), 0x0005);
        CHECK_EQ(m->cpu->last_exception_vector, kinds[i].vec);
        mach_destroy(m);
    }

    /* CIR のバスエラー: 読出しなら R、書込みなら W（addr_err_read） */
    for (unsigned read = 0; read < 2; read++) {
        mach_t *m = host_machine(model);

        mach_response(m, EA_TO_CP(VEA_DATA, 4));
        mach_response(m, NULL_PF1);
        m->fail_cir = read ? CIR_RESPONSE : CIR_OPERAND;
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0x0000, 0x0003);
        mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
        CHECK_EQ(m->cpu->last_exception_vector, VEC_BUS_ERROR);
        CHECK_EQ(m->cpu->addr_err_read, read != 0);
        mach_destroy(m);
    }
}

/* A-B29: コプロセッサ命令は 68020 以上だけ。68000/68010
 * ではフックを設定しても呼ばれず、従来どおり F-line （ユーザモードの cpSAVE/cpRESTORE
 * も特権違反でなく F-line） */
static void test_old_models_unaffected(void) {
    static const mc68k_model_t old[] = {MC68K_MODEL_68000, MC68K_MODEL_68010};

    for (unsigned i = 0; i < 2; i++) {
        for (unsigned user = 0; user < 2; user++) {
            mach_t *m = host_machine(old[i]);

            m->cpu->a[1] = 0x4000;
            PROG(m, MACH_START,
                 user ? OP_CP(CPID, T_SAVE, EA_AN_IND(1)) : OP_CP(CPID, T_GEN, EA_IMM), 0x4000,
                 0, 3);
            if (user)
                cpu_set_sr(m->cpu, 0x0000);
            m->cpu->ssp = MACH_STACK;
            mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
            CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FLINE));
            expect_nlog(m, 0);
            mach_destroy(m);
        }
    }
}

/* ===== 再々々レビュー（Opus、A-B30〜A-B36） ===== */

/* A-B30: メモリの format word が valid でないとき（$0006）、読み返しが
 * valid（$1F06）なら、長さは検査せず 転送した long の個数分（6 / 4 = 1
 * 個）だけ進む。規定が無い組合せで、設計書 §3.7 に推論として記す */
static void test_restore_memory_not_valid_readback_valid(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    m->restore_readback = 0x1F06;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x0006FFFFu);
    put32(m, 0x4004, 0xAAAAAAAAu);
    const uint32_t pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_POST(1)));
    run(m, pc);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0xAAAAAAAAu);
    expect_nlog(m, 3);
    CHECK_EQ(m->cpu->a[1], 0x4008);
    mach_destroy(m);
}

/* A-B32: 内蔵 FPU があるときの cpID の振り分け。cpID=1 は内蔵モデル、2..7
 * はホストのフック（無ければ F-line） */
static void test_cpid_routing_with_internal_fpu(mc68k_model_t model) {
    mach_t *m = mach_create(model, MC68K_FPU_68881, false);

    m->cpu->a[7] = MACH_STACK;
    PROG(m, MACH_START, OP_CP(2, T_GEN, EA_IMM), 0x4000, 0x0000, 0x0003);
    mach_run_until(m, MACH_HANDLER(VEC_FLINE), 4);
    expect_pre_frame(m, VEC_FLINE, MACH_START); /* cpID=2 を内蔵モデルへ渡さない */
    mach_destroy(m);

    m = mach_create(model, MC68K_FPU_68881, true);
    m->cpu->a[7] = MACH_STACK;
    mach_response(m, NULL_PF1);
    uint32_t pc = PROG(m, MACH_START, OP_CP(2, T_GEN, 0), 0x0000);
    run(m, pc);
    expect_log(m, 0, true, 2, CIR_COMMAND, 2, 0); /* cpID=2 はホストのフックへ */
    expect_nlog(m, 2);
    pc = PROG(m, pc, OP_CP(1, T_GEN, 0), 0x2000); /* cpID=1 は内蔵モデル: フックに触れない */
    mc68k_execute(m->cpu, 1);
    expect_nlog(m, 2);
    mach_destroy(m);
}

/* A-B33、A-B38、A-N26、A-N29、A-N34、A-N38: コプロセッサ例外は S をセットし、T1 と T0
 * の両方を消す（[UM20 §6.1]）。 pre・mid は命令が完了していないので、T1=1
 * でもトレース例外（ベクタ 9）を起こさず、ハンドラの先頭に留まる （[UM20 §7.5.2.5
 * p.7-56〜7-57]）。post は通常の命令関連例外（ゼロ除算など）と同じ扱い（[UM20 §7.4.20]）で、
 * 強制された例外の処理の後にトレース例外が起こる（[UM20 §6.1.7 p.6-9]）ので、T1=1
 * ならハンドラ($31)の先頭で トレース例外になる。T1,T0=01 の変更フロートレースは、汎用（general
 * category）命令では、コプロセッサが DR=1 の transfer status register and scanPC primitive
 * を返したときだけ（[UM20 §7.5.2.5 p.7-57]）なので、cpGEN の take では
 * 起こらない（条件系は対象外） */
static void test_exception_clears_trace_bits(mc68k_model_t model) {
    static const uint16_t bits[] = {SR_T1, SR_T0};
    static const struct {
        uint16_t resp;
        unsigned vec;
        unsigned frame_bytes;
        bool traced_t1;
    } kinds[] = {{TAKE_PRE(0x30), 0x30, 8, false},
                 {TAKE_MID(0x32), 0x32, 20, false},
                 {TAKE_POST(0x31), 0x31, 12, true}};
    const unsigned trace_frame_bytes =
        12; /* トレース例外は six-word frame（format $2）[UM20 Table 6-5 p.6-27] */

    for (unsigned k = 0; k < 3; k++) {
        for (unsigned i = 0; i < 2; i++) {
            mach_t *m = host_machine(model);
            const bool traced = bits[i] == SR_T1 && kinds[k].traced_t1;

            mach_response(m, kinds[k].resp);
            PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
            m->cpu->sr |= bits[i];
            mc68k_execute(m->cpu, 1);
            CHECK_EQ(m->cpu->sr & (SR_T1 | SR_T0), 0);
            CHECK(m->cpu->sr & SR_S);
            if (traced) {
                const uint32_t tf = m->cpu->a[7];

                CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(9));
                CHECK_EQ(tf, MACH_STACK - kinds[k].frame_bytes - trace_frame_bytes);
                /* トレースのフレーム: SR（コプロセッサ例外の処理で T を消した後の $2700）、PC =
                 * 次の命令（ハンドラ）、 $2000 | ベクタ 9、instruction address =
                 * 例外を起こした命令 [UM20 Table 6-5 p.6-27] */
                CHECK_EQ(get16(m, tf), 0x2700);
                CHECK_EQ(get32(m, tf + 2), MACH_HANDLER(kinds[k].vec));
                CHECK_EQ(get16(m, tf + 6), 0x2000 | (9 * 4));
                CHECK_EQ(get32(m, tf + 8), MACH_START);
                /* その下に、コプロセッサ例外のフレーム（積まれた SR は例外前のもの） */
                CHECK_EQ(get16(m, MACH_STACK - kinds[k].frame_bytes) & (SR_T1 | SR_T0),
                         bits[i]);
            } else {
                CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(kinds[k].vec));
                CHECK_EQ(m->cpu->a[7],
                         MACH_STACK -
                             kinds[k].frame_bytes); /* トレース例外のフレームが積まれない */
                CHECK_EQ(get16(m, m->cpu->a[7]) & (SR_T1 | SR_T0),
                         bits[i]); /* 積まれた SR は例外前のもの */
            }
            mach_destroy(m);
        }
    }
}

/* A-B34: 例外ベクタは VBR 相対 */
static void test_vbr_relative_vectors(mc68k_model_t model) {
    static const struct {
        uint16_t resp;
        unsigned vec;
    } cases[] = {{TAKE_PRE(0x30), 0x30},
                 {TAKE_MID(0x32), 0x32},
                 {TAKE_POST(0x31), 0x31},
                 {0x0000, VEC_VIOLATION}};

    for (unsigned i = 0; i < 4; i++) {
        mach_t *m = host_machine(model);
        const uint32_t vbr = 0x3000, handler = 0x2800 + 0x10 * i;

        put32(m, vbr + cases[i].vec * 4, handler);
        put16(m, handler, 0x4E71);
        m->cpu->vbr = vbr;
        mach_response(m, cases[i].resp);
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
        mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu), handler);
        mach_destroy(m);
    }
}

/* : format 9 の追加欄は RTE の internal-state restore なので、
 * 読出し fault はスタックを戻さず HALT する。 */
static void test_rte_format9_extra_fields_fault(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    m->cpu->a[7] = MACH_RAM - 8; /* SR、PC、format word は読めるが +8 は RAM の外 */
    put16(m, MACH_RAM - 8, 0x2000);
    put32(m, MACH_RAM - 6, 0x1234);
    put16(m, MACH_RAM - 2, 0x9000);
    PROG(m, MACH_START, OP_RTE);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_exec_state(m->cpu), CPU_HALTED);
    CHECK_EQ(m->nlog_total, 0); /* 再開しない。CIR に触れない */
    /* RTE は frame を pop / SR を commit していない。 */
    CHECK_EQ(get16(m, m->cpu->a[7]), 0x2000);
    mach_destroy(m);
}

/* A-B36: コプロセッサ無しの F-line のサイクル数は、従来の F-line（cpID=0）と等しい（設計書 §3.1
 * 優先3） */
static void test_absent_fline_cycles_match_legacy(mc68k_model_t model) {
    int cycles[2];
    static const uint16_t ops[] = {0xF040 /* cpID=0 */,
                                   OP_CP(CPID, T_GEN, 0) /* cpID=1、コプロセッサ無し */};

    for (unsigned i = 0; i < 2; i++) {
        mach_t *m = mach_create(model, MC68K_FPU_NONE, false);

        m->cpu->a[7] = MACH_STACK;
        PROG(m, MACH_START, ops[i], 0x0000);
        cycles[i] = mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FLINE));
        mach_destroy(m);
    }
    CHECK_EQ(cycles[0], cycles[1]);
}

/* : RTE format 9 の +16 は internal-state restore の入力なので
 * 読出し fault は HALT する。 */
static void test_rte_format9_ea_field_fault(mc68k_model_t model) {
    mach_t *m = host_machine(model);
    const uint32_t frame = MACH_RAM - 16;

    m->cpu->a[7] = frame;
    put16(m, frame, 0x2000);
    put32(m, frame + 2, 0x1234);
    put16(m, frame + 6, 0x9000);
    put32(m, frame + 8, MACH_START);
    put16(m, frame + 14, OP_CP(CPID, T_GEN, 0)); /* cpGEN。再開すれば応答 CIR を読む */
    PROG(m, MACH_START, OP_RTE);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_exec_state(m->cpu), CPU_HALTED);
    CHECK_EQ(m->nlog_total, 0);
    CHECK_EQ(get16(m, m->cpu->a[7]), 0x2000);
    CHECK_EQ(m->cpu->a[7], frame);             /* フレームを pop していない */
    CHECK_EQ(m->cpu->addr_err_addr, MACH_RAM); /* 失敗した +16 の読出し */
    CHECK(m->cpu->addr_err_read);
    mach_destroy(m);
}

/* A-B39: リストア CIR の読み返しが予約コード（$03、$0F）のときも、abort を書いて vector
 * 14（format error） */
static void test_restore_reserved_readback(mc68k_model_t model) {
    static const uint16_t readbacks[] = {0x0300, 0x0F00};

    for (unsigned i = 0; i < 2; i++) {
        mach_t *m = host_machine(model);

        m->restore_readback = readbacks[i];
        m->cpu->a[1] = 0x4000;
        put32(m, 0x4000, 0x1F08FFFFu);
        PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_POST(1)));
        mach_run_until(m, MACH_HANDLER(VEC_FORMAT), 5);
        expect_pre_frame(m, VEC_FORMAT, MACH_START);
        CHECK_EQ(m->cpu->a[1], 0x4000);
        expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
        expect_nlog(m, 3);
        mach_destroy(m);
    }
}

/* A-B40: リストア CIR の書込みは成功し、読み返しだけ失敗する。最初のアクセスではないので F-line
 * ではなく バスエラー（vector 2）で、abort（コントロール CIR へ $0001）を書く */
static void test_restore_readback_bus_error(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    m->fail_cir = CIR_RESTORE;
    m->fail_after = 1;
    m->cpu->a[1] = 0x4000;
    put32(m, 0x4000, 0x1F08FFFFu);
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    expect_log(m, 0, true, CPID, CIR_RESTORE, 2, 0x1F08);
    CHECK_EQ(m->log[1].write, false);
    CHECK_EQ(m->log[1].addr, 0x20000u + (CPID << 13) + CIR_RESTORE);
    CHECK_EQ(m->log[1].size, 2);
    expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
    expect_nlog(m, 3);
    mach_destroy(m);
}

/* : コプロセッサ例外フレームまたはその vector fetch の fault は
 * vector 2 へ配送する。Group 0 自身の vector fetch fault だけが HALT する。 */
static uint32_t vector_probe_addr;
static int vector_probe_hits;
static void vector_probe(void *ctx, const mc68k_bus_access_t *a, mc68k_bus_timing_t *out) {
    (void)ctx;
    (void)out;
    if (a->read && !a->instruction && a->fc != 7 && a->physical == vector_probe_addr)
        vector_probe_hits++;
}

static void test_coproc_exception_double_fault(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(m, TAKE_PRE(0x30));
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    m->cpu->a[7] = MACH_RAM + 8; /* フレーム全体が RAM の外 */
    m->cpu->bus.access_timing = vector_probe;
    vector_probe_addr = 0x30 * 4; /* RAM 上にあり、読めば成功するベクタ */
    vector_probe_hits = 0;
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(m->cpu->exec_state, CPU_NORMAL);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    CHECK_EQ(vector_probe_hits, 0); /* 元 vector は読まない */
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, TAKE_MID(0x30));
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    m->cpu->vbr = MACH_RAM; /* ベクタテーブルが RAM の外 */
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(m->cpu->exec_state, CPU_HALTED);
    CHECK_EQ(m->cpu->a[7], MACH_STACK - 20 - 92); /* partial MID の下の Group-0 $B */
    CHECK_EQ(get16(m, MACH_STACK - 20 + 6), 0x9000 | (0x30 * 4));
    mach_destroy(m);
}

/* A-B44: ユーザモードで取った mid の例外から RTE で再開するとき、SR（S
 * ビット）を復元してから応答 CIR を読む （[UM20 §6.1.12
 * p.6-21]）。再開後のオペランドの読出しはユーザのスタック（USP、FC=1）から行い、RTE 後の A7 は
 * USP、 SSP はフレームを pop した値になる */
static uint32_t user_probe_addr;
static int user_probe_hits;
static uint8_t user_probe_fc;
static void user_probe(void *ctx, const mc68k_bus_access_t *a, mc68k_bus_timing_t *out) {
    (void)ctx;
    (void)out;
    if (a->read && !a->instruction && a->fc != 7 && a->physical == user_probe_addr) {
        user_probe_hits++;
        user_probe_fc = a->fc;
    }
}

static void test_rte_format9_resume_in_user_mode(mc68k_model_t model) {
    const uint32_t user_sp = 0x5000;
    mach_t *m = host_machine(model);

    put32(m, user_sp, 0x11223344u);
    put16(m, MACH_HANDLER(0x32), OP_RTE);
    mach_response(m, TAKE_MID(0x32));
    mach_response(m, EA_TO_CP(VEA_ANY, 4));
    mach_response(m, NULL_PF1);
    const uint32_t pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_POST(7)), 0x0000);
    cpu_set_sr(m->cpu, 0x0000); /* ユーザモード */
    m->cpu->ssp = MACH_STACK;
    m->cpu->a[7] = user_sp;
    m->cpu->bus.access_timing = user_probe;
    user_probe_addr = user_sp;
    user_probe_hits = 0;
    mach_run_until(m, MACH_HANDLER(0x32), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(0x32));
    CHECK(m->cpu->sr & SR_S);
    run(m, pc);
    CHECK(!(m->cpu->sr & SR_S));
    CHECK_EQ(user_probe_hits, 1);
    CHECK_EQ(user_probe_fc, 1); /* ユーザデータ空間 */
    expect_nlog(
        m, 6); /* 0: command、1: take、2: XA、3: 再開の応答、4: オペランド、5: 終了の応答 */
    expect_log(m, 3, false, CPID, CIR_RESPONSE, 2, EA_TO_CP(VEA_ANY, 4));
    expect_log(m, 4, true, CPID, CIR_OPERAND, 4, 0x11223344u);
    expect_log(m, 5, false, CPID, CIR_RESPONSE, 2, NULL_PF1);
    CHECK_EQ(m->cpu->a[7], user_sp + 4);
    CHECK_EQ(m->cpu->ssp, MACH_STACK);
    mach_destroy(m);
}

/* A-B47: format 9 の再開はどの cpID（1..7）でも行う。フレームの cpID のコプロセッサだけを見る
 */
static void test_rte_format9_resume_all_cpids(mc68k_model_t model) {
    for (unsigned id = 1; id < 8; id++) {
        mach_t *m = host_machine(model);

        put16(m, MACH_HANDLER(0x32), OP_RTE);
        mach_response(m, TAKE_MID(0x32));
        mach_response(m, NULL_PF1);
        const uint32_t pc = PROG(m, MACH_START, OP_CP(id, T_GEN, 0), 0x0000);
        run(m, pc);
        expect_log(m, 0, true, id, CIR_COMMAND, 2, 0);
        expect_log(m, 3, false, id, CIR_RESPONSE, 2,
                   NULL_PF1); /* 再開した対話は cpID=id の応答 CIR を読む */
        expect_nlog(m, 4);
        mach_destroy(m);
    }

    /* 内蔵 FPU（cpID=1）だけでフックが無いときの cpID=2 のフレームは再開せず、復帰だけをする */
    mach_t *m = mach_create(model, MC68K_FPU_68881, false);

    m->cpu->a[7] = 0x5000;
    put16(m, 0x5000, 0x2000);
    put32(m, 0x5002, 0x1234);
    put16(m, 0x5006, 0x9000);
    put32(m, 0x5008, 0x7777);
    put16(m, 0x500E, OP_CP(2, T_GEN, 0));
    put16(m, 0x1234, 0x4E71);
    PROG(m, MACH_START, OP_RTE);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), 0x1234);
    CHECK_EQ(m->cpu->a[7], 0x5000 + 20);
    mach_destroy(m);
}

/* A-B49: RTE が読む・例外が積む long 欄（PC、instruction
 * address、EA）の上位ワードを落とさない。format 9 の IA=$00ABCDE0、EA=$12345678、PC=$00AB1236
 * から RTE し、再度 mid 例外にすると、新しいフレームが同じ値を引き継ぐ。
 * 新しいフレームは古いフレームと同じアドレスに重なるので、書込みを word
 * 幅にする変異は値が残ってこの試験を通り抜ける。 この試験が検出するのは値の切り詰め（上位 word
 * を落とす）で、書込み幅は test_exception_frame_extent が検出する（A-N63） */
static void test_rte_format9_long_fields(mc68k_model_t model) {
    mach_t *m = host_machine(model);
    const uint32_t frame = 0x5000;

    mach_response(m, TAKE_MID(0x32));
    m->cpu->a[7] = frame;
    put16(m, frame, 0x2000);
    put32(m, frame + 2, 0x00AB1236u);
    put16(m, frame + 6, 0x9000 | (0x32 * 4));
    put32(m, frame + 8, 0x00ABCDE0u);
    put16(m, frame + 14, OP_CP(CPID, T_GEN, 0));
    put32(m, frame + 16, 0x12345678u);
    PROG(m, MACH_START, OP_RTE);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(0x32));
    const uint32_t f = m->cpu->a[7];
    CHECK_EQ(f, frame);                     /* 20 バイト pop して 20 バイト push */
    CHECK_EQ(get32(m, f + 2), 0x00AB1236u); /* scanPC は RTE が復元した PC */
    CHECK_EQ(get32(m, f + 8), 0x00ABCDE0u);
    CHECK_EQ(get32(m, f + 16), 0x12345678u);
    mach_destroy(m);
}

/* A-B49、A-B50: スタックを $A5A5 で埋めておき、pre・post・mid のフレームの long
 * 欄の全体と、フレームの外 （上は SP+8/12/20 から MACH_STACK+16
 * まで、下はフレームの先頭の手前）を書かないことを確かめる */
#define FILL_BELOW 32
#define FILL_ABOVE 16
static void test_exception_frame_extent(mc68k_model_t model) {
    static const struct {
        uint16_t resp;
        unsigned vec;
        unsigned frame_bytes;
    } kinds[] = {
        {TAKE_PRE(0x30), 0x30, 8}, {TAKE_POST(0x31), 0x31, 12}, {TAKE_MID(0x32), 0x32, 20}};

    for (unsigned k = 0; k < 3; k++) {
        mach_t *m = host_machine(model);

        for (uint32_t a = MACH_STACK - FILL_BELOW; a < MACH_STACK + FILL_ABOVE; a += 2)
            put16(m, a, 0xA5A5);
        mach_response(m, EA_TO_MPU(VEA_MEM_ALT, 4));
        mach_response(m, kinds[k].resp);
        m->operand_in[0] = 1;
        m->n_operand_in = 1;
        m->cpu->a[0] = 0x4000;
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6000);
        mach_run_until(m, MACH_HANDLER(kinds[k].vec), 10);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(kinds[k].vec));
        const uint32_t f = m->cpu->a[7];
        CHECK_EQ(f, MACH_STACK - kinds[k].frame_bytes);
        CHECK_EQ(get32(m, f + 2), kinds[k].frame_bytes == 8 ? MACH_START : MACH_START + 4);
        if (kinds[k].frame_bytes >= 12)
            CHECK_EQ(get32(m, f + 8), MACH_START);
        if (kinds[k].frame_bytes == 20) {
            CHECK_EQ(get16(m, f + 12), 0); /* 設計値（§3.8 の表。UM20 の値ではない）。$A5A5
                                              が残っていないことの確認を兼ねる */
            CHECK_EQ(get16(m, f + 14), OP_CP(CPID, T_GEN, EA_AN_IND(0)));
            CHECK_EQ(get32(m, f + 16), 0x4000);
        }
        for (uint32_t a = MACH_STACK - FILL_BELOW; a < f; a += 2)
            CHECK_EQ(get16(m, a), 0xA5A5);
        for (uint32_t a = MACH_STACK; a < MACH_STACK + FILL_ABOVE; a += 2)
            CHECK_EQ(get16(m, a), 0xA5A5);
        mach_destroy(m);
    }
}

/* A-B51: take primitive のベクタ番号は 8 bit 全部（$80、$FF）。フレームの format word
 * とベクタ読出しの両方 */
static void test_take_vector_8bit(mc68k_model_t model) {
    static const unsigned vectors[] = {0x80, 0xFF};
    const uint32_t handler = 0x3000;

    for (unsigned v = 0; v < 2; v++) {
        for (unsigned mid = 0; mid < 2; mid++) {
            mach_t *m = host_machine(model);

            put32(m, vectors[v] * 4u, handler + 0x10 * v);
            put16(m, handler + 0x10 * v, 0x4E71);
            mach_response(m, mid ? TAKE_MID(vectors[v]) : TAKE_PRE(vectors[v]));
            PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
            mc68k_execute(m->cpu, 1);
            CHECK_EQ(mc68k_get_pc(m->cpu), handler + 0x10 * v);
            CHECK_EQ(get16(m, m->cpu->a[7] + 6), (mid ? 0x9000 : 0x0000) | (vectors[v] * 4u));
            CHECK_EQ(m->cpu->last_exception_vector, vectors[v]);
            mach_destroy(m);
        }
    }
}

/* ===== Opus 10 回目レビュー（A-B52〜A-B63）の指摘 ===== */

/* ホストのメモリアクセス回数（命令フェッチを含む）。失敗した最初のバスサイクルの後に後半を出さないことの確認用
 */
static int mem_reads, mem_writes;
static bool cnt_rb(void *p, uint32_t a, uint8_t *v) {
    mem_reads++;
    return mach_rb(p, a, v);
}
static bool cnt_rw(void *p, uint32_t a, uint16_t *v) {
    mem_reads++;
    return mach_rw(p, a, v);
}
static bool cnt_rl(void *p, uint32_t a, uint32_t *v) {
    mem_reads++;
    return mach_rl(p, a, v);
}
static bool cnt_wb(void *p, uint32_t a, uint8_t v) {
    mem_writes++;
    return mach_wb(p, a, v);
}
static bool cnt_ww(void *p, uint32_t a, uint16_t v) {
    mem_writes++;
    return mach_ww(p, a, v);
}
static void count_memory(mach_t *m) {
    m->cpu->bus.read_byte = cnt_rb;
    m->cpu->bus.read_word = cnt_rw;
    m->cpu->bus.read_long = cnt_rl;
    m->cpu->bus.write_byte = cnt_wb;
    m->cpu->bus.write_word = cnt_ww;
    mem_reads = mem_writes = 0;
}

/* A-B52: コプロセッサの存在は read と write の両方のフックがあるときだけ。write だけ（read が
 * NULL）なら CIR に触れず F-line（bus_hooks.h、設計書 §2） */
static void test_port_requires_both_hooks(mc68k_model_t model) {
    mach_t *m = mach_create(model, MC68K_FPU_NONE, false);

    m->cpu->bus.cpu_space_write = mach_cs_write;
    m->cpu->a[7] = MACH_STACK;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FLINE));
    expect_nlog(m, 0);
    mach_destroy(m);
}

/* A-B53: 最初の CIR アクセスより後の CIR フォルトは、すべて通常のバスエラー（vector
 * 2）で、最後に AB を書く。 F-line になるのは命令を始める最初のアクセスだけ（[UM20 §7.5.2.2
 * p.7-55]、設計書 §3.9） */
static void cir_fault_case(mc68k_model_t model, int which) {
    mach_t *m = host_machine(model);
    uint16_t ea = 0;

    m->cpu->a[0] = 0x4000;
    switch (which) {
    case 0:
        mach_response(m, EA_TO_CP(VEA_ANY, 4));
        m->fail_cir = CIR_OPERAND;
        ea = EA_AN_IND(0);
        break;
    case 1:
        mach_response(m, EA_TO_MPU(VEA_ANY, 4));
        m->fail_cir = CIR_OPERAND;
        ea = EA_AN_IND(0);
        break;
    case 2:
        mach_response(m, EA_TO_CP(VEA_ANY, 4));
        m->fail_cir = CIR_OPERAND;
        ea = EA_DN(1);
        break;
    case 3:
        mach_response(m, EA_TO_MPU(VEA_ANY, 4));
        m->fail_cir = CIR_OPERAND;
        ea = EA_DN(1);
        break;
    case 4:
        mach_response(m, EA_TO_CP(VEA_ANY, 1));
        m->fail_cir = CIR_OPERAND;
        ea = EA_IMM;
        break;
    case 5:
        mach_response(m, FPREGS_TO_CP(4));
        m->fail_cir = CIR_REGSEL;
        ea = EA_AN_IND(0);
        break;
    case 6:
        mach_response(m, MPREG_TO_CP(1));
        m->fail_cir = CIR_OPERAND;
        break;
    case 7:
        mach_response(m, MPREG_TO_MPU(1));
        m->fail_cir = CIR_OPERAND;
        break;
    case 8:
        mach_response(m, NULL_PC_FIN);
        m->fail_cir = CIR_IA;
        break;
    case 9:
        mach_response(m, TAKE_PRE(0x30));
        m->fail_cir = CIR_CONTROL;
        break;
    case 10:
        mach_response(m, XFER_OPWORD);
        m->fail_cir = CIR_OPWORD;
        break;
    case 11:
        mach_response(m, XFER_STREAM(4));
        m->fail_cir = CIR_OPERAND;
        break;
    }
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, ea), 0x0000, 0x0012);
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 3);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    CHECK_EQ(m->cpu->last_exception_vector, VEC_BUS_ERROR);
    expect_log(m, m->nlog - 1, true, CPID, CIR_CONTROL, 2,
               1); /* 最後は AB。そのあと対話を続けない */
    mach_destroy(m);
}

static void test_cir_fault_after_first_access(mc68k_model_t model) {
    for (int which = 0; which <= 11; which++)
        cir_fault_case(model, which);

    mach_t *m = host_machine(model); /* cpSAVE のオペランド CIR の読出し */
    m->save_format = 0x1F04;
    m->fail_cir = CIR_OPERAND;
    m->cpu->a[1] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 3);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    mach_destroy(m);

    m = host_machine(model); /* cpRESTORE のオペランド CIR の書込み */
    m->restore_readback = 0x1F04;
    m->fail_cir = CIR_OPERAND;
    m->cpu->a[1] = 0x4000;
    put16(m, 0x4000, 0x1F04);
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 3);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    mach_destroy(m);
}

/* A-B54: CIR バスエラーのフレームの SSW と fault address（ゲストに見える欄）。UM20 §6.2.1
 * Figure 6-8 p.6-23（SSW）、RW・SIZE は p.6-24、SIZE の符号化は Table 5-2、fault address は
 * SP+$10（p.6-25）。 4 バイトのオペランド CIR 書込み:
 * DF=1、RW=0（write）、SIZE=00（long）、FC=7。応答 CIR（2 バイト）の読出し:
 * RW=1、SIZE=10（word）。値はどちらも、実測してフレームの欄の組合せを固定したもの */
static void test_cir_fault_frame_attributes(mc68k_model_t model) {
    for (int read = 0; read < 2; read++) {
        mach_t *m = host_machine(model);

        mach_response(m, EA_TO_CP(VEA_DATA, 4));
        mach_response(m, NULL_PF1);
        m->fail_cir = read ? CIR_RESPONSE : CIR_OPERAND;
        m->fail_after = read ? 1 : 0; /* 応答 CIR は 2 回目のアクセスで失敗させる */
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0x1234, 0x5678);
        mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
        CHECK_EQ(get16(m, m->cpu->a[7] + 0x0A) & 0x0177u, read ? 0x0167u : 0x0107u);
        mach_destroy(m);
    }
    /* fault address は cpID を含む（cpID=3 は $26010） */
    for (unsigned cpid = 1; cpid < 8; cpid++) {
        mach_t *m = host_machine(model);

        mach_response(m, EA_TO_CP(VEA_DATA, 4));
        mach_response(m, NULL_PF1);
        m->fail_cir = CIR_OPERAND;
        PROG(m, MACH_START, OP_CP(cpid, T_GEN, EA_IMM), 0x4000, 0, 3);
        mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
        CHECK_EQ(get32(m, m->cpu->a[7] + 0x10), 0x20000u + (cpid << 13) + CIR_OPERAND);
        mach_destroy(m);
    }
}

/* A-B55: メモリのフォルトの後に CIR へ触れない（AB
 * だけ）、フォルトした最初のバスサイクルの後に後半を出さない */
static void test_memory_fault_stops_cir_traffic(mc68k_model_t model) {
    mach_t *m;

    /* コマンド word・条件 word のフェッチが RAM の外 */
    m = host_machine(model);
    mach_response(m, NULL_PF1);
    put16(m, MACH_RAM - 2, OP_CP(CPID, T_GEN, 0));
    m->cpu->pc = MACH_RAM - 2;
    mc68k_execute(m->cpu, 1);
    expect_nlog(m, 0);
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, NULL_TF1);
    put16(m, MACH_RAM - 2, OP_CP(CPID, T_COND, 0));
    m->cpu->pc = MACH_RAM - 2;
    mc68k_execute(m->cpu, 1);
    expect_nlog(m, 0);
    mach_destroy(m);

    /* 命令ストリーム（長さ 4）と即値 1 バイトが RAM の外: コマンド、応答、AB の 3 回 */
    for (int imm = 0; imm < 2; imm++) {
        m = host_machine(model);
        mach_response(m, imm ? EA_TO_CP(VEA_ANY, 1) : XFER_STREAM(4));
        mach_response(m, NULL_PF1);
        put16(m, MACH_RAM - 4, OP_CP(CPID, T_GEN, imm ? EA_IMM : 0));
        put16(m, MACH_RAM - 2, 0x0000);
        m->cpu->pc = MACH_RAM - 4;
        mc68k_execute(m->cpu, 1);
        expect_nlog(m, 3);
        expect_log(m, 2, true, CPID, CIR_CONTROL, 2, 1);
        mach_destroy(m);
    }

    /* cpSAVE のヘッダ書込みが失敗: format word 読出しと AB だけ（オペランドを読まない） */
    m = host_machine(model);
    m->save_format = 0x1F04;
    m->operand_in[0] = 1;
    m->n_operand_in = 1;
    m->cpu->a[1] = MACH_RAM;
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    mc68k_execute(m->cpu, 1);
    expect_nlog(m, 2);
    mach_destroy(m);

    /* cpRESTORE の body 読出しが失敗: restore CIR の書込みと読返し、AB */
    m = host_machine(model);
    m->restore_readback = 0x1F08;
    m->cpu->a[1] = MACH_RAM - 4;
    put16(m, MACH_RAM - 4, 0x1F08);
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    mc68k_execute(m->cpu, 1);
    expect_nlog(m, 3);
    mach_destroy(m);

    /* 3 バイトの読出し・書込みは、失敗した word の後に byte を出さない（2 バイトと同じ回数） */
    int reads[2], writes[2];
    for (int len = 2; len <= 3; len++) {
        m = host_machine(model);
        mach_response(m, EA_TO_CP(VEA_ANY, (unsigned)len));
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = MACH_RAM;
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x0000);
        count_memory(m);
        mc68k_execute(m->cpu, 1);
        reads[len - 2] = mem_reads;
        mach_destroy(m);

        m = host_machine(model);
        mach_response(m, EA_TO_MPU(VEA_ANY, (unsigned)len));
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = MACH_RAM;
        m->operand_in[0] = 0x123456;
        m->n_operand_in = 1;
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x0000);
        count_memory(m);
        mc68k_execute(m->cpu, 1);
        writes[len - 2] = mem_writes;
        mach_destroy(m);
    }
    CHECK_EQ(reads[0], reads[1]);
    CHECK(reads[0] > 0);
    CHECK_EQ(writes[0], writes[1]);
    CHECK(writes[0] > 0);

    /* 命令ストリームの long と cpBcc.L の変位: 最初の word が RAM の外なら、2 番目の word
     * を読まない */
    for (int k = 0; k < 2; k++) {
        int r[2];

        for (int wide = 1; wide >= 0; wide--) {
            m = host_machine(model);
            if (k == 0) {
                mach_response(m, XFER_STREAM(wide ? 4 : 2));
                mach_response(m, NULL_PF1);
                put16(m, MACH_RAM - 4, OP_CP(CPID, T_GEN, 0));
                put16(m, MACH_RAM - 2, 0x0000);
                m->cpu->pc = MACH_RAM - 4;
            } else {
                mach_response(m, NULL_TF1);
                put16(m, MACH_RAM - 2, OP_CP(CPID, wide ? T_BCC_L : T_BCC_W, 1));
                m->cpu->pc = MACH_RAM - 2;
            }
            count_memory(m);
            mc68k_execute(m->cpu, 1);
            r[wide] = mem_reads;
            mach_destroy(m);
        }
        CHECK_EQ(r[0], r[1]);
    }

    /* 1 バイトのメモリオペランドは byte で読み書きする: RAM の最終バイトは成功する */
    m = host_machine(model);
    mach_response(m, EA_TO_CP(VEA_ANY, 1));
    mach_response(m, NULL_PF1);
    m->ram[MACH_RAM - 1] = 0x5A;
    m->cpu->a[0] = MACH_RAM - 1;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x0000);
    mc68k_execute(m->cpu, 1);
    expect_nlog(m, 4);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_START + 4);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 1, 0x5A);
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, EA_TO_MPU(VEA_ANY, 1));
    mach_response(m, NULL_PF1);
    m->operand_in[0] = 0xA7;
    m->n_operand_in = 1;
    m->cpu->a[0] = MACH_RAM - 1;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x0000);
    mc68k_execute(m->cpu, 1);
    expect_nlog(m, 4);
    CHECK_EQ(mc68k_get_pc(m->cpu),
             MACH_START +
                 4); /* 例外なく完了（word で書くと奇数アドレスの 2 番目の byte が RAM の外） */
    CHECK_EQ(m->ram[MACH_RAM - 1], 0xA7);
    mach_destroy(m);
}

/* A-B56: protocol violation の format 9 フレームの EA 欄は、評価済みの実効アドレス（[UM20
 * §7.5.2.1 p.7-55]、 Figure 7-43 p.7-48）。EA を評価していないときだけ undefined（p.7-49） */
static void test_violation_frame_ea(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(m, EA_TO_CP(VEA_ANY, 4));
    mach_response(m, 0x8600u); /* CA=1 で未定義の機能 */
    m->cpu->a[0] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x0000);
    mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
    CHECK_EQ(get32(m, m->cpu->a[7] + 16), 0x4000);
    mach_destroy(m);
}

/* A-B57: コプロセッサ例外のハンドラ内の SR。S をセットし T1・T0 を消すだけで、M・IPL・CCR
 * は保つ （[UM20 §6.1 p.6-1]。IPL を更新するのは reset と割込みだけ） */
static void test_exception_keeps_sr_bits(mc68k_model_t model) {
    static const struct {
        uint16_t resp;
        uint16_t vec;
        unsigned bytes;
    } kinds[] = {{TAKE_PRE(0x30), 0x30, 8},
                 {TAKE_POST(0x31), 0x31, 12},
                 {TAKE_MID(0x32), 0x32, 20},
                 {0x8600u, VEC_VIOLATION, 20}};

    for (unsigned i = 0; i < 4; i++) {
        mach_t *m = host_machine(model);

        mach_response(m, kinds[i].resp);
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
        cpu_set_sr(m->cpu, 0x3315); /* S=1、M=1、IPL=3、CCR=X0 の 10101 */
        m->cpu->a[7] = 0x7000;
        mc68k_execute(m->cpu, 1);
        CHECK_EQ(m->cpu->sr, 0x3315);
        CHECK_EQ(m->cpu->a[7], 0x7000u - kinds[i].bytes);
        CHECK_EQ(get16(m, m->cpu->a[7]), 0x3315);
        mach_destroy(m);
    }
}

/* A-B58: cpGEN の終了条件は T1（トレース保留）だけ。T0 ではトレース保留にならない（[UM20
 * §7.5.2.5 p.7-57]） */
static void test_t0_terminates_cpgen(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(m, 0x0900u); /* CA=0 の Null（PF=0） */
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    cpu_set_sr(m->cpu, 0x6700); /* T0=1 */
    mc68k_execute(m->cpu, 1);
    expect_nlog(m, 2); /* コマンド、応答。読み続けない */
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, EA_TO_CP_LAST(VEA_DATA, 2)); /* CA=0 の EA 転送 */
    mach_response(m, NULL_PF1);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x0000, 0x00AB);
    cpu_set_sr(m->cpu, 0x6700);
    mc68k_execute(m->cpu, 1);
    expect_nlog(m, 3); /* コマンド、応答、オペランド */
    mach_destroy(m);
}

/* A-B59・A-B64: 図で固定されたビットが違う primitive は未定義（protocol violation）。設計書
 * §3.3 の表のとおり （[UM20 Figure 7-23〜7-27、7-33、7-40、7-42、7-44]）。supervisor check で
 * DR=1 の形は Busy と同じ符号（$24）なので、 Busy の規則に従う */
static void test_primitive_fixed_bits(mc68k_model_t model) {
    static const uint16_t undefined[] = {
        0x2400, /* Busy で CA=0 */
        0x3C30,
        0x2700,
        0x2F02, /* DR=1 の take pre、opword、stream（CA=0） */
        0xBC30,
        0xA700,
        0xAF02, /* 同じ（CA=1） */
        0x3C00,
        0xBC00, /* DR=1 の 0x1C。func を bit11-8 だけで見ると MP レジスタ転送になる */
        0x2800,
        0x2900, /* DR=1 の Null */
        /* 下位の固定ビット（A-B64）: Busy・supervisor check・opword は bit7-0=0、Null は
           bit7-2=0、MP レジスタは bit7-4=0 */
        0xA401,
        0xA480,
        0x8401,
        0x0480,
        0x8701,
        0x0780,
        0x0804,
        0x0880,
        0x8804,
        0x08FC,
        0x8C10,
        0x8C80,
        0xAC40,
        0xACF0,
    };
    /* A-B65: 固定ビットを 1 ビットずつ立てた値（マスクの中間のビットを落とす変異を検出する） */
    uint16_t single_bit[8 * 3 + 6 + 4], nsingle = 0;
    for (unsigned b = 0; b < 8; b++) {
        single_bit[nsingle++] = (uint16_t)(0xA400u | (1u << b)); /* Busy */
        single_bit[nsingle++] = (uint16_t)(0x8400u | (1u << b)); /* supervisor check */
        single_bit[nsingle++] = (uint16_t)(0x8700u | (1u << b)); /* transfer operation word */
    }
    for (unsigned b = 2; b < 8; b++)
        single_bit[nsingle++] = (uint16_t)(0x0800u | (1u << b)); /* Null の bit7-2 */
    for (unsigned b = 4; b < 8; b++)
        single_bit[nsingle++] = (uint16_t)(0x8C00u | (1u << b)); /* MP レジスタの bit7-4 */
    CHECK_EQ(nsingle, sizeof(single_bit) / sizeof(single_bit[0]));

    for (unsigned k = 0; k < sizeof(undefined) / sizeof(undefined[0]) + nsingle; k++) {
        mach_t *m = host_machine(model);
        const uint16_t resp = k < sizeof(undefined) / sizeof(undefined[0])
                                  ? undefined[k]
                                  : single_bit[k - sizeof(undefined) / sizeof(undefined[0])];

        mach_response(m, resp);
        mach_response(m, NULL_PF1); /* 受理されたら終わる（最後の応答の繰返しが 65536 回目の
                                       violation に化けない） */
        m->operand_in[0] = 0;
        m->n_operand_in = 1;
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000, 0x1111);
        mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 3);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
        mach_destroy(m);
    }
    /* 条件系でも violation: Busy の CA=0（$2400）、Null の固定ビット（$0804）、A-B66: CA=1 の
     * Busy・supervisor check・ opword・MP
     * レジスタの下位固定ビット違反（$A401・$8401・$8701・$8C10） */
    static const uint16_t cond_undefined[] = {0x2400, 0x0804, 0xA401, 0x8401, 0x8701, 0x8C10};
    for (unsigned k = 0; k < sizeof(cond_undefined) / sizeof(cond_undefined[0]); k++) {
        mach_t *m = host_machine(model);
        mach_response(m, cond_undefined[k]);
        mach_response(m, NULL_TF0);
        PROG(m, MACH_START, OP_CP(CPID, T_BCC_W, 1), 0x0004);
        mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 3);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
        mach_destroy(m);
    }
}

/* A-B67: PC ビット（bit14）は全 primitive で可変（[UM20 §7.4.2]）。固定ビットの検査が PC=1
 * の正規形を 弾かない。Busy・supervisor check・opword・MP レジスタの PC=1 は、命令アドレス CIR
 * へ書いてから通常の処理 */
static void test_pc_bit_in_fixed_bit_primitives(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    m = host_machine(model); /* Busy: IA を渡して命令をやり直す */
    mach_response(m, 0xE400);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_START);
    CHECK_EQ(m->cpu->last_exception_vector, 0);
    expect_nlog(m, 3);
    expect_log(m, 2, true, CPID, CIR_IA, 4, MACH_START);
    mach_destroy(m);

    m = host_machine(model); /* supervisor check（S=1） */
    mach_response(m, 0xC400);
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    run(m, pc);
    expect_nlog(m, 4);
    expect_log(m, 2, true, CPID, CIR_IA, 4, MACH_START);
    mach_destroy(m);

    m = host_machine(model); /* transfer operation word */
    mach_response(m, 0xC700);
    mach_response(m, NULL_PF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    run(m, pc);
    expect_nlog(m, 5);
    expect_log(m, 2, true, CPID, CIR_IA, 4, MACH_START);
    expect_log(m, 3, true, CPID, CIR_OPWORD, 2, OP_CP(CPID, T_GEN, 0));
    mach_destroy(m);

    m = host_machine(model); /* transfer single MP register（D0 → オペランド CIR） */
    mach_response(m, 0xCC00);
    mach_response(m, NULL_PF1);
    m->cpu->d[0] = 0x12345678;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    run(m, pc);
    expect_nlog(m, 5);
    expect_log(m, 2, true, CPID, CIR_IA, 4, MACH_START);
    expect_log(m, 3, true, CPID, CIR_OPERAND, 4, 0x12345678);
    mach_destroy(m);
}

/* A-B60: 条件系でも CA=1 の transfer operation word・transfer from instruction stream は有効
 * （[UM20 §7.4.6 p.7-34、§7.4.7 p.7-35]、Table 7-6 p.7-54 の注） */
static void test_conditional_ca1_transfers(mc68k_model_t model) {
    mach_t *m = host_machine(model);
    uint32_t pc;

    mach_response(m, XFER_OPWORD);
    mach_response(m, NULL_TF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_BCC_W, 0x01), 0x0004, MOVEQ(1, 0), MOVEQ(2, 1));
    run(m, pc);
    CHECK_EQ(m->cpu->last_exception_vector, 0);
    CHECK_EQ(m->cpu->d[0], 0); /* TF=1 で分岐 */
    expect_log(m, 2, true, CPID, CIR_OPWORD, 2, OP_CP(CPID, T_BCC_W, 0x01));
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, XFER_STREAM(2));
    mach_response(m, NULL_TF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, 0), 0x0001, 0xABCD);
    run(m, pc);
    CHECK_EQ(m->cpu->last_exception_vector, 0);
    CHECK_EQ(m->cpu->d[0] & 0xFF, 0xFF);
    expect_log(m, 2, true, CPID, CIR_OPERAND, 2, 0xABCD);
    mach_destroy(m);
}

/* A-B61: EA・符号化の境界 */
static void test_ea_encoding_boundaries(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* (a) cpScc (xxx).L（mode 7 reg 1）は cpTRAPcc ではなく cpScc。[UM20 §7.2.2.2 p.7-14、Table
     * 7-1 p.7-16] */
    m = host_machine(model);
    mach_response(m, NULL_TF1);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, 0x39), 0x0001, 0x0000, 0x4000);
    run(m, pc);
    CHECK_EQ(m->ram[0x4000], 0xFF);
    mach_destroy(m);

    /* (b) メモリ→コプロセッサの FP レジスタ群は制御モード（(d16,PC) を含む）。[UM20 §7.4.16
     * p.7-44] */
    m = host_machine(model);
    mach_response(m, FPREGS_TO_CP(4));
    mach_response(m, NULL_PF1);
    m->regsel = 0x8000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0x3A), 0x0000, 0x0010);
    put32(m, MACH_START + 4 + 0x10, 0xCAFEBABE);
    run(m, pc);
    expect_log(m, 3, true, CPID, CIR_OPERAND, 4, 0xCAFEBABE);
    mach_destroy(m);

    /* (c) cpBcc.W の条件は 6 bit（$3F まで）。[UM20 Figure 7-9、7-10] */
    m = host_machine(model);
    mach_response(m, NULL_TF0);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_BCC_W, 0x3F), 0x0004);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), pc);
    expect_nlog(m, 2);
    expect_log(m, 0, true, CPID, CIR_CONDITION, 2, OP_CP(CPID, T_BCC_W, 0x3F));
    mach_destroy(m);
}

/* A-B62: format word の値の幅と境界（[UM20 Table 7-2 p.7-19、§7.5.2.7]） */
static void test_format_word_widths(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* cpSAVE: format code と length は 8 bit 全体。$8004 は valid で 4 バイトの body */
    m = host_machine(model);
    m->save_format = 0x8004;
    m->operand_in[0] = 0x11112222;
    m->n_operand_in = 1;
    m->cpu->a[1] = 0x4000;
    pc = PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    run(m, pc);
    CHECK_EQ(get32(m, 0x4000), 0x8004FFFFu);
    CHECK_EQ(get32(m, 0x4004), 0x11112222u);
    mach_destroy(m);

    /* cpRESTORE: length $84 は 33 個の long */
    m = host_machine(model);
    m->restore_readback = 0x1F84;
    m->cpu->a[1] = 0x4000;
    put16(m, 0x4000, 0x1F84);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    run(m, pc);
    expect_nlog(m, 2 + 0x84 / 4);
    mach_destroy(m);

    /* メモリの format word の長さが 4 の倍数でない valid は format error（code $1F・$10
     * の境界） */
    for (int k = 0; k < 2; k++) {
        m = host_machine(model);
        m->restore_readback = 0x1F04;
        m->cpu->a[1] = 0x4000;
        put16(m, 0x4000, k ? 0x1002 : 0x1F01);
        PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
        mach_run_until(m, MACH_HANDLER(VEC_FORMAT), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FORMAT));
        mach_destroy(m);
    }
    /* cpSAVE でも同じ（長さ 1、2 は format error） */
    for (unsigned len = 1; len <= 3; len++) {
        m = host_machine(model);
        m->save_format = (uint16_t)(0x1F00u | len);
        m->cpu->a[1] = 0x4000;
        PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
        mach_run_until(m, MACH_HANDLER(VEC_FORMAT), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FORMAT));
        mach_destroy(m);
    }

    /* empty の format word は length が 0 でなくても empty: header だけ書き、body は読まない */
    for (int k = 0; k < 2; k++) {
        m = host_machine(model);
        m->save_format = k ? 0x0003 : 0x0004;
        m->cpu->a[1] = 0x4000;
        put32(m, 0x4004, 0x55555555);
        pc = PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
        mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu), pc);
        CHECK_EQ(get32(m, 0x4000),
                 (uint32_t)m->save_format << 16); /* format word と予約 word 0 */
        CHECK_EQ(get32(m, 0x4004), 0x55555555u);
        expect_nlog(m, 1);
        mach_destroy(m);
    }
}

/* A-B63: レジスタ番号と type の値域 */
static void test_register_and_type_ranges(mc68k_model_t model) {
    mach_t *m;
    uint32_t pc;

    /* (a) cpRESTORE (A0)+ は A0（reg 0）も更新する: format word + body 4 バイト */
    m = host_machine(model);
    m->restore_readback = 0x1F04;
    m->cpu->a[0] = 0x4000;
    put16(m, 0x4000, 0x1F04);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_POST(0)));
    run(m, pc);
    CHECK_EQ(m->cpu->a[0], 0x4008);
    mach_destroy(m);

    /* (b) ユーザモードの cpBcc.L は特権違反ではない（特権は cpSAVE/cpRESTORE だけ。[UM20
     * §7.5.2.3 p.7-56]） */
    m = host_machine(model);
    mach_response(m, NULL_TF0);
    pc = PROG(m, MACH_START, OP_CP(CPID, T_BCC_L, 0x01), 0x0000, 0x0006);
    cpu_set_sr(m->cpu, 0x0000);
    m->cpu->a[7] = 0x5000;
    m->cpu->ssp = MACH_STACK;
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), pc);
    mach_destroy(m);

    /* (c) 内蔵モデルの MP レジスタ番号は 3 bit: 動的な FMOVEM の D5 */
    m = mach_create(model, MC68K_FPU_68881, false);
    m->cpu->a[7] = MACH_STACK;
    m->cpu->a[1] = 0x4100;
    m->cpu->d[5] = 0xC0; /* 2 本のレジスタ（FP6、FP7） */
    pc = PROG(m, MACH_START, (uint16_t)(0xF200u | EA_AN_POST(1)),
              (uint16_t)(0xD800u | (5u << 4)));
    mach_run_until(m, pc, 20);
    CHECK_EQ(m->cpu->a[1], 0x4100u + 24);
    mach_destroy(m);
}

/* Dn/An への転送（コプロセッサ→MPU）を 1 ビットずつ突く（#68 backlog: operand_to_register）。
 * 期待値はビット単位の規則から組む: Dn は下位 len バイトだけ operand、残りは元のレジスタ値。
 * An は operand の下位 len バイトを符号拡張した long。operand の上位ビットは garbage
 * として立てる。 */
#define REG_BITS 32u

static uint32_t expected_dn_update(uint32_t before, uint32_t operand, unsigned len) {
    uint32_t r = before;

    for (unsigned b = 0; b < 8u * len; b++)
        r = (r & ~(UINT32_C(1) << b)) | (operand & (UINT32_C(1) << b));
    return r;
}

static uint32_t expected_an_update(uint32_t operand, unsigned len) {
    const unsigned bits = 8u * len;
    uint32_t r = operand & ((UINT32_C(1) << bits) - 1u);

    if (r & (UINT32_C(1) << (bits - 1u)))
        for (unsigned b = bits; b < REG_BITS; b++)
            r |= UINT32_C(1) << b;
    return r;
}

static void test_register_transfer_bits(mc68k_model_t model) {
    static const unsigned lens[] = {1, 2, 4};
    unsigned cases = 0;

    for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        for (unsigned addr = 0; addr < 2; addr++) {
            for (unsigned b = 0; b < REG_BITS; b++) {
                const uint32_t one = UINT32_C(1) << b;
                /* (before, operand): 1 ビットだけ立てる／1 ビットだけ落とす × 互いに反転 */
                const uint32_t pairs[4][2] = {
                    {one, ~one}, {~one, one}, {one, one}, {~one, ~one}};

                for (unsigned p = 0; p < 4; p++) {
                    mach_t *m = host_machine(model);
                    uint32_t pc;
                    uint32_t after;

                    m->operand_in[0] = pairs[p][1];
                    m->n_operand_in = 1;
                    mach_response(m, EA_TO_MPU(VEA_ALT, lens[li]));
                    mach_response(m, NULL_PF1);
                    if (addr)
                        m->cpu->a[1] = pairs[p][0];
                    else
                        m->cpu->d[1] = pairs[p][0];
                    pc =
                        PROG(m, MACH_START, OP_CP(CPID, T_GEN, addr ? 0x09 : EA_DN(1)), 0x6000);
                    run(m, pc);
                    after = addr ? m->cpu->a[1] : m->cpu->d[1];
                    if (lens[li] == 4)
                        CHECK_EQ(after, pairs[p][1]);
                    else if (addr)
                        CHECK_EQ(after, expected_an_update(pairs[p][1], lens[li]));
                    else
                        CHECK_EQ(after, expected_dn_update(pairs[p][0], pairs[p][1], lens[li]));
                    mach_destroy(m);
                    cases++;
                }
            }
        }
    }
    CHECK_EQ(cases, 3u * 2u * REG_BITS * 4u);
}

/* 条件系（cpScc）の supervisor check は CA=1 なら protocol violation でなく、S=1 なら続行する
 * （条件系の CA=0 だけが violation。[UM20 Table 7-6]）。#68: cp_primitive の `conditional &&
 * !ca` の観測 */
static void test_supervisor_check_conditional(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(m, SUPERVISOR_CHECK);
    mach_response(m, NULL_TF1);
    m->cpu->d[3] = 0x12345600u;
    const uint32_t pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(3)), 0x0001);
    run(m, pc);
    CHECK_EQ(m->cpu->d[3], 0x123456FFu);
    expect_log(m, 1, false, CPID, CIR_RESPONSE, 2, SUPERVISOR_CHECK);
    expect_log(m, 2, false, CPID, CIR_RESPONSE, 2, NULL_TF1);
    mach_destroy(m);
}

/* コプロセッサの特権違反・cpTRAPcc の例外のサイクルは、通常命令の同じ例外（特権命令の違反、TRAP
 * #n）と同じ （#68: `CYC(cpu, CYC_EXC_PRIVILEGE)`・`CYC(cpu, CYC_EXC_TRAP)` の
 * +1。値そのものは書かず、既存の例外との一致で見る） */
static void test_cp_exception_cycles_match_legacy(mc68k_model_t model) {
    mach_t *m;
    int ref, cycles;

    m = host_machine(model);
    cpu_set_sr(m->cpu, 0x0000);
    m->cpu->ssp = MACH_STACK;
    PROG(m, MACH_START, 0x46FC /* MOVE #imm,SR（特権） */, 0x0000);
    ref = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_PRIV));
    mach_destroy(m);

    m = host_machine(model);
    cpu_set_sr(m->cpu, 0x0000);
    m->cpu->ssp = MACH_STACK;
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    cycles = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_PRIV));
    CHECK_EQ(cycles, ref);
    mach_destroy(m);

    m = host_machine(model);
    PROG(m, MACH_START, 0x4E40 /* TRAP #0 */);
    ref = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(32));
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, NULL_TF1);
    PROG(m, MACH_START, OP_CP(CPID, T_COND, 0x3C /* cpTRAPcc、オペランド無し */), 0x0001);
    cycles = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_TRAPCC));
    CHECK_EQ(cycles, ref);
    mach_destroy(m);
}

/* RTE の format 9 以外のフレームは、スタックの先（+8
 * 以降）を読まず、コプロセッサの対話を再開しない （#68: op_rte() の `format == 9`）。フレームを
 * RAM の末尾に置いて、先を読めば失敗するようにする */
static void test_rte_non_format9_reads_only_its_frame(mc68k_model_t model) {
    static const struct {
        unsigned format, bytes;
    } frames[] = {{0, 8}, {2, 12}};

    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        mach_t *m = host_machine(model);
        const uint32_t frame = MACH_RAM - frames[i].bytes;
        const uint32_t target = MACH_START + 0x100;

        PROG(m, MACH_START, 0x4E73 /* RTE */);
        PROG(m, target, 0x4E71);
        m->cpu->a[7] = frame;
        put16(m, frame, 0x2000);
        put32(m, frame + 2, target);
        put16(m, frame + 6, (uint16_t)(frames[i].format << 12));
        mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu), target);
        CHECK_EQ(m->cpu->a[7], MACH_RAM);
        CHECK_EQ(m->nlog_total, 0);
        mach_destroy(m);
    }
}

/* 例外ベクタの取得に失敗した（二重フォルト）ときは停止し、ハンドラへ移った状態（PC・ベクタ番号）にしない
 * （#68: cpu_dispatch_coproc_exception() の `return 12` の脱落） */
static void test_coproc_exception_vector_fault_halts(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    mach_response(m, TAKE_PRE(0x30));
    m->cpu->vbr = MACH_RAM; /* どのベクタも RAM の外 */
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    mc68k_execute(m->cpu, 1);
    CHECK_EQ(m->cpu->exec_state, CPU_HALTED);
    CHECK_EQ(m->cpu->last_exception_vector, 0);
    mach_destroy(m);
}

/* cpDBcc のカウンタと cpScc の Dn への書込みを 1 ビットずつ突く（#68 backlog:
 * cp_complete_conditional）。 期待値は規則から組む: DBcc は下位 word だけ 1
 * 減らし（上位は保つ）、減らした値が 0xFFFF でなければ分岐する。 cpScc は下位 byte だけを 0xFF
 * / 0x00 に置き換え、残りの 24 ビットは保つ。 */
static void test_conditional_register_bits(mc68k_model_t model) {
    unsigned cases = 0;

    for (unsigned b = 0; b < REG_BITS; b++) {
        const uint32_t one = UINT32_C(1) << b;
        const uint32_t befores[2] = {one, ~one};

        for (unsigned p = 0; p < 2; p++) {
            const uint32_t before = befores[p];
            const uint16_t low = (uint16_t)(before & 0xFFFFu);
            const uint16_t dec = (uint16_t)(low - 1u);
            mach_t *m = host_machine(model);
            uint32_t pc;

            /* cpDBcc D1: 分岐すれば直後の MOVEQ #1,D0 を飛ばす（変位の基点は変位ワード） */
            mach_response(m, NULL_TF0);
            m->cpu->d[0] = 0;
            m->cpu->d[1] = before;
            pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, 0x08 | 1), 0x0001, 0x0004, MOVEQ(1, 0),
                      MOVEQ(2, 2));
            run(m, pc);
            CHECK_EQ(m->cpu->d[1], (before & 0xFFFF0000u) | dec);
            CHECK_EQ(m->cpu->d[0], dec != 0xFFFFu ? 0u : 1u);
            mach_destroy(m);
            cases++;

            for (unsigned tf = 0; tf < 2; tf++) {
                m = host_machine(model);
                mach_response(m, tf ? NULL_TF1 : NULL_TF0);
                m->cpu->d[3] = before;
                pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(3)), 0x0001);
                run(m, pc);
                CHECK_EQ(m->cpu->d[3], expected_dn_update(before, tf ? 0xFFu : 0x00u, 1));
                mach_destroy(m);
                cases++;
            }
        }
    }
    CHECK_EQ(cases, REG_BITS * 2u * 3u);
}

/* cpScc (A0)+ の書込みが失敗したら A0 は更新しない（EA 転送の (A0)+ と同じ規則。#68 backlog）
 */
static void test_scc_memory_fault_keeps_an(mc68k_model_t model) {
    const uint32_t outside = MACH_RAM + 0x100;
    mach_t *m = host_machine(model);

    mach_response(m, NULL_TF1);
    m->cpu->a[0] = outside;
    PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_AN_POST(0)), 0x0001);
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    CHECK_EQ(m->cpu->a[0], outside);
    mach_destroy(m);
}

/* cpScc の書込み失敗で消費するサイクルは、同じ位置で失敗する通常の Scc.B と同じ（ハンドラの早期
 * return 値） */
static void test_scc_memory_fault_cycles(mc68k_model_t model) {
    const uint32_t outside = MACH_RAM + 0x100;
    int cycles_cp, cycles_scc;
    mach_t *m = host_machine(model);

    mach_response(m, NULL_TF1);
    m->cpu->a[0] = outside;
    PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_AN_IND(0)), 0x0001);
    cycles_cp = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    mach_destroy(m);

    m = host_machine(model);
    m->cpu->a[0] = outside;
    PROG(m, MACH_START, 0x50D0 /* ST (A0) */);
    cycles_scc = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    mach_destroy(m);
    CHECK_EQ(cycles_cp, cycles_scc);
}

/* メモリ→オペランド CIR を長さ 1〜12 の全部で突く（#68 backlog: cp_memory_to_operand）。
 * 4 バイトずつ昇順に書き、端数（1〜3
 * バイト）はその大きさ。値はメモリのバイト列を上位から詰めた数値。
 * メモリのバイトは全部異なる値にし、アドレスの組立て（off
 * の加減）と端数の位置が値に出るようにする。 */
#define MEM_TO_CP_MAX_LEN 12u

static void test_memory_to_operand_lengths(mc68k_model_t model) {
    for (unsigned len = 1; len <= MEM_TO_CP_MAX_LEN; len++) {
        mach_t *m = host_machine(model);
        uint32_t pc;
        unsigned writes = 0;

        mach_response(m, EA_TO_CP(VEA_MEM, len));
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = 0x4000;
        for (unsigned i = 0; i < MEM_TO_CP_MAX_LEN + 4u; i++)
            m->ram[0x4000 + i] = (uint8_t)(0x10u * (i % 15u) + i + 1u);
        pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x4800);
        run(m, pc);
        for (unsigned off = 0; off < len; off += 4, writes++) {
            const unsigned n = len - off < 4u ? len - off : 4u;
            uint32_t v = 0;

            for (unsigned k = 0; k < n; k++)
                v = (v << 8) | m->ram[0x4000 + off + k];
            expect_log(m, 2 + (int)writes, true, CPID, CIR_OPERAND, n, v);
        }
        CHECK_EQ(writes, (len + 3u) / 4u);
        mach_destroy(m);
    }
}

/* オペランド CIR→メモリを長さ 1〜12 の全部で突く（#68 backlog: cp_operand_to_memory）。
 * 4 バイトずつ昇順に読み、端数（1〜3 バイト）は operand の下位 n
 * バイトをメモリへ上位から並べる。 operand の上位（端数の外）には garbage
 * を立て、書込みの末尾の次のバイトは触らないことも見る。 */
static void test_operand_to_memory_lengths(mc68k_model_t model) {
    static const uint8_t untouched = 0xEE;
    static const uint32_t operands[3] = {0x91A2B3C4u, 0x85D6E7F8u, 0xF1E2D3C4u};

    for (unsigned len = 1; len <= MEM_TO_CP_MAX_LEN; len++) {
        mach_t *m = host_machine(model);
        uint32_t pc;

        set_operands(m, operands, 3);
        mach_response(m, EA_TO_MPU(VEA_MEM_ALT, len));
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = 0x4000;
        for (unsigned i = 0; i < MEM_TO_CP_MAX_LEN + 4u; i++)
            m->ram[0x4000 + i] = untouched;
        pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6800);
        run(m, pc);
        for (unsigned i = 0; i < MEM_TO_CP_MAX_LEN + 4u; i++) {
            const unsigned chunk = i / 4u, n = len - chunk * 4u < 4u ? len - chunk * 4u : 4u;
            uint8_t want = untouched;

            if (i < len) /* チャンク内の位置 k のバイト = operand の下位 n バイトの上位から k
                            番目 */
                want = (uint8_t)(operands[chunk] >> (8u * (n - 1u - i % 4u)));
            CHECK_EQ(m->ram[0x4000 + i], want);
        }
        mach_destroy(m);
    }
}

/* オペランド→メモリの書込みが失敗したときの消費サイクルは、同じ位置で失敗する通常の命令と同じ
 * （ハンドラの早期 return 値。cp_memory_ok() の戻り値が false であることの観測。68020+ では An
 * の巻き戻しで レジスタには出ない）。#68 backlog: cp_operand_to_memory */
static void test_operand_to_memory_fault_cycles(mc68k_model_t model) {
    static const unsigned lens[] = {1, 2, 3, 4};
    const uint32_t outside = MACH_RAM + 0x100;

    for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        int cycles_cp, cycles_ref;
        mach_t *m = host_machine(model);

        m->operand_in[0] = 0x01020304u;
        m->n_operand_in = 1;
        mach_response(m, EA_TO_MPU(VEA_MEM_ALT, lens[i]));
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = outside;
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6800);
        cycles_cp = mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
        mach_destroy(m);

        m = host_machine(model);
        m->cpu->a[0] = outside;
        PROG(m, MACH_START, 0x50D0 /* ST (A0) */);
        cycles_ref = mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
        mach_destroy(m);
        CHECK_EQ(cycles_cp, cycles_ref);
    }
}

/* cp_ea_transfer の即値・EA 判定・拡張語の取得失敗（#68 backlog: cp_ea_transfer） */
#define IMM_BYTE_BITS 8u

/* 長さ 1 の即値は拡張 word の下位 byte だけを渡す。上位 byte は garbage として立て、8
 * ビット全部を突く */
static void test_immediate_byte_bits(mc68k_model_t model) {
    unsigned cases = 0;

    for (unsigned b = 0; b < IMM_BYTE_BITS; b++) {
        const uint16_t one = (uint16_t)(1u << b);
        const uint16_t exts[4] = {(uint16_t)(0xFF00u | one),
                                  (uint16_t)(0xFF00u | (~one & 0xFFu)), one,
                                  (uint16_t)(~one & 0xFFu)};

        for (unsigned p = 0; p < 4; p++) {
            mach_t *m = host_machine(model);
            uint32_t pc;

            mach_response(m, EA_TO_CP(VEA_ANY, 1));
            mach_response(m, NULL_PF1);
            pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, exts[p]);
            run(m, pc);
            expect_log(m, 2, true, CPID, CIR_OPERAND, 1, exts[p] & 0xFFu);
            mach_destroy(m);
            cases++;
        }
    }
    CHECK_EQ(cases, IMM_BYTE_BITS * 4u);
}

/* reg == 4 のメモリ EA（(A4)、(A4)+、-(A4)、d16(A4)）は即値（mode 7, reg 4）として扱わない */
static void test_reg4_memory_modes_not_immediate(mc68k_model_t model) {
    static const struct {
        uint16_t ea;
        uint32_t a4;
        uint32_t a4_after;
        bool ext;
    } modes[] = {
        {EA_AN_IND(4), 0x4000, 0x4000, false},
        {EA_AN_POST(4), 0x4000, 0x4004, false},
        {EA_AN_PRE(4), 0x4004, 0x4000, false},
        {0x2C /* d16(A4) */, 0x3FF0, 0x3FF0, true},
    };

    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        mach_t *m = host_machine(model);
        uint32_t pc;

        mach_response(m, EA_TO_CP(VEA_MEM, 4));
        mach_response(m, NULL_PF1);
        m->cpu->a[4] = modes[i].a4;
        put32(m, 0x4000, 0xA1B2C3D4u);
        if (modes[i].ext)
            pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, modes[i].ea), 0x6800, 0x0010);
        else
            pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, modes[i].ea), 0x6800);
        run(m, pc);
        expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0xA1B2C3D4u);
        CHECK_EQ(m->cpu->a[4], modes[i].a4_after);
        mach_destroy(m);
    }
}

/* EA の拡張語の取得に失敗したときの消費サイクルは、同じ位置で失敗する通常の命令（MOVE.B
 * d16(A0),D0）と同じ （cp_resolve_memory() の失敗が cp_ea_transfer() の戻り値 false
 * になることの観測） */
static void test_ea_extension_fetch_fault_cycles(mc68k_model_t model) {
    const uint32_t jump_to = MACH_RAM - 4;
    int cycles_cp, cycles_ref;
    mach_t *m = host_machine(model);

    mach_response(m, EA_TO_CP(VEA_MEM, 4));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    PROG(m, MACH_START, 0x4EF9 /* JMP abs.l */, (uint16_t)(jump_to >> 16), (uint16_t)jump_to);
    PROG(m, jump_to, OP_CP(CPID, T_GEN, 0x28 /* d16(A0) */), 0x6800); /* 拡張語は RAM の外 */
    mc68k_execute(m->cpu, 1);
    cycles_cp = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    mach_destroy(m);

    m = host_machine(model);
    m->cpu->a[0] = 0x4000;
    PROG(m, MACH_START, 0x4EF9, (uint16_t)((MACH_RAM - 2) >> 16), (uint16_t)(MACH_RAM - 2));
    PROG(m, MACH_RAM - 2, 0x1028 /* MOVE.B d16(A0),D0 */);
    mc68k_execute(m->cpu, 1);
    cycles_ref = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    mach_destroy(m);
    CHECK_EQ(cycles_cp, cycles_ref);
}

/* (An)+ / -(An) の更新が An の 32 ビット全部に効くこと（#68 backlog: cp_commit_memory）。
 * 試験機の RAM は 64KB なので、上位 16 ビットを無視して RAM に折り返すバスフックを使い、An を
 * 64KB 以上にする。 */
#define ALIAS_ADDR_MASK 0xFFFFu

static bool alias_rb(void *p, uint32_t a, uint8_t *v) {
    return mach_rb(p, a & ALIAS_ADDR_MASK, v);
}
static bool alias_rw(void *p, uint32_t a, uint16_t *v) {
    return mach_rw(p, a & ALIAS_ADDR_MASK, v);
}
static bool alias_rl(void *p, uint32_t a, uint32_t *v) {
    return mach_rl(p, a & ALIAS_ADDR_MASK, v);
}
static bool alias_wb(void *p, uint32_t a, uint8_t v) {
    return mach_wb(p, a & ALIAS_ADDR_MASK, v);
}
static bool alias_ww(void *p, uint32_t a, uint16_t v) {
    return mach_ww(p, a & ALIAS_ADDR_MASK, v);
}

static void test_autoinc_updates_all_32_bits(mc68k_model_t model) {
    static const uint32_t bases[] = {0x00014000u, 0x12344000u, 0xA5C34000u, 0xFFFF4000u};

    for (unsigned i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        for (unsigned pre = 0; pre < 2; pre++) {
            mach_t *m = host_machine(model);
            uint32_t pc;

            m->cpu->bus.read_byte = alias_rb;
            m->cpu->bus.read_word = alias_rw;
            m->cpu->bus.read_long = alias_rl;
            m->cpu->bus.write_byte = alias_wb;
            m->cpu->bus.write_word = alias_ww;
            mach_response(m, EA_TO_CP(VEA_MEM, 4));
            mach_response(m, NULL_PF1);
            put32(m, 0x4000, 0xA1B2C3D4u);
            m->cpu->a[0] = pre ? bases[i] + 4u : bases[i];
            pc = PROG(m, MACH_START, OP_CP(CPID, T_GEN, pre ? EA_AN_PRE(0) : EA_AN_POST(0)),
                      0x6800);
            run(m, pc);
            expect_log(m, 2, true, CPID, CIR_OPERAND, 4, 0xA1B2C3D4u);
            CHECK_EQ(m->cpu->a[0], pre ? bases[i] : bases[i] + 4u);
            mach_destroy(m);
        }
    }
}

/* Dn/An からコプロセッサへの転送（MPU→コプロセッサ）を 1 ビットずつ突く（#68 backlog:
 * register_to_operand）。 operand CIR へ渡す値は、レジスタの下位 len
 * バイトを下位詰めにした数値（len = 4 はそのまま）。 */
static void test_register_to_operand_bits(mc68k_model_t model) {
    static const unsigned lens[] = {1, 2, 4};
    unsigned cases = 0;

    for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        for (unsigned addr = 0; addr < 2; addr++) {
            for (unsigned b = 0; b < REG_BITS; b++) {
                const uint32_t one = UINT32_C(1) << b;
                const uint32_t regs[2] = {one, ~one};

                for (unsigned p = 0; p < 2; p++) {
                    mach_t *m = host_machine(model);
                    uint32_t pc, want = 0;

                    for (unsigned k = 0; k < 8u * lens[li]; k++)
                        want |= regs[p] & (UINT32_C(1) << k);
                    mach_response(m, EA_TO_CP(VEA_ANY, lens[li]));
                    mach_response(m, NULL_PF1);
                    if (addr)
                        m->cpu->a[1] = regs[p];
                    else
                        m->cpu->d[1] = regs[p];
                    pc =
                        PROG(m, MACH_START, OP_CP(CPID, T_GEN, addr ? 0x09 : EA_DN(1)), 0x4000);
                    run(m, pc);
                    expect_log(m, 2, true, CPID, CIR_OPERAND, lens[li], want);
                    mach_destroy(m);
                    cases++;
                }
            }
        }
    }
    CHECK_EQ(cases, 3u * 2u * REG_BITS * 2u);
}

/* format word の code（上位 byte）と length（下位 byte）を 1 ビットずつ突く（#68 backlog:
 * FORMAT_CODE / FORMAT_LENGTH）。 code: 0x02〜0x0F は予約（format error）、0x10 以上は
 * valid。0x01 は not ready で再実行するので除く。 */
#define FORMAT_CODE_BITS      8u
#define FORMAT_VALID_CODE_MIN 0x10u

static void test_format_code_bits(mc68k_model_t model) {
    for (unsigned b = 1; b < FORMAT_CODE_BITS; b++) {
        const uint16_t fw = (uint16_t)((1u << b) << 8);
        const bool valid = (1u << b) >= FORMAT_VALID_CODE_MIN;

        for (unsigned restore = 0; restore < 2; restore++) {
            mach_t *m = host_machine(model);
            uint32_t pc;

            m->cpu->a[1] = 0x4000;
            put32(m, 0x4000, restore ? ((uint32_t)fw << 16) | 0xFFFFu : 0xAAAAAAAAu);
            if (restore)
                m->restore_readback = fw;
            else
                m->save_format = fw;
            pc = PROG(m, MACH_START, OP_CP(CPID, restore ? T_RESTORE : T_SAVE, EA_AN_IND(1)));
            if (valid) {
                run(m, pc);
                if (!restore)
                    CHECK_EQ(get32(m, 0x4000), ((uint32_t)fw << 16) | 0xFFFFu);
            } else {
                mach_run_until(m, MACH_HANDLER(VEC_FORMAT), 5);
                CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FORMAT));
            }
            mach_destroy(m);
        }
    }
}

static void test_format_length_bits(mc68k_model_t model) {
    static const unsigned lens[] = {4, 8, 16, 32, 64}; /* body は operand_in の 16 long まで */

    for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        const unsigned len = lens[li], longs = len / 4u;

        for (unsigned restore = 0; restore < 2; restore++) {
            mach_t *m = host_machine(model);
            uint32_t pc;

            m->cpu->a[1] = 0x4000;
            if (restore) {
                put32(m, 0x4000, (0x1F00u + len) << 16 | 0xFFFFu);
                for (unsigned i = 0; i < longs; i++)
                    put32(m, 0x4004 + 4 * i, 0x01010101u * (i + 1u));
                m->restore_readback = (uint16_t)(0x1F00u + len);
            } else {
                m->save_format = (uint16_t)(0x1F00u + len);
                for (unsigned i = 0; i < longs; i++)
                    m->operand_in[i] = 0x01010101u * (i + 1u);
                m->n_operand_in = (int)longs;
            }
            pc = PROG(m, MACH_START, OP_CP(CPID, restore ? T_RESTORE : T_SAVE, EA_AN_IND(1)));
            run(m, pc);
            for (unsigned i = 0; i < longs; i++) {
                if (restore)
                    expect_log(m, 2 + (int)i, true, CPID, CIR_OPERAND, 4,
                               0x01010101u * (i + 1u));
                else /* 最初に読んだ long が最高アドレス（+len）、以後降順 */
                    CHECK_EQ(get32(m, 0x4000 + len - 4 * i), 0x01010101u * (i + 1u));
            }
            expect_nlog(m, restore ? 2 + (int)longs : 1 + (int)longs);
            mach_destroy(m);
        }
    }
}

/* CA=0 の NULL 応答の IA（bit8）・PF（bit1）・TF（bit0）の 8 通りを全部突く（#68 backlog:
 * NULL_PF / NULL_TF）。 条件系は TF だけで真偽が決まる。cpGEN はトレース保留中、PF=0 の間は応答
 * CIR を読み続け、PF=1 で終わる。 */
#define NULL_RESP_BASE   0x0800u
#define NULL_RESP_IA     0x0100u
#define NULL_RESP_PF     0x0002u
#define NULL_RESP_TF     0x0001u
#define NULL_RESP_COMBOS 8u

static uint16_t null_resp_combo(unsigned c) {
    return (uint16_t)(NULL_RESP_BASE | ((c & 4u) ? NULL_RESP_IA : 0u) |
                      ((c & 2u) ? NULL_RESP_PF : 0u) | ((c & 1u) ? NULL_RESP_TF : 0u));
}

static void test_null_response_bits(mc68k_model_t model) {
    for (unsigned c = 0; c < NULL_RESP_COMBOS; c++) {
        const bool pf = (c & 2u) != 0, tf = (c & 1u) != 0;
        mach_t *m = host_machine(model);
        uint32_t pc;

        mach_response(m, null_resp_combo(c));
        m->cpu->d[3] = 0x123456AAu;
        pc = PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(3)), 0x0001);
        run(m, pc);
        CHECK_EQ(m->cpu->d[3], tf ? 0x123456FFu : 0x12345600u);
        mach_destroy(m);

        m = host_machine(model);
        mach_response(m, null_resp_combo(c));
        mach_response(m, NULL_PF1);
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_DN(0)), 0x0000);
        m->cpu->sr |= SR_T1;
        mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(9)); /* トレース */
        int reads = 0;
        for (int i = 0; i < m->nlog; i++)
            reads += !m->log[i].write && (m->log[i].addr & 0x1F) == CIR_RESPONSE;
        CHECK_EQ(reads, pf ? 1 : 2);
        mach_destroy(m);
    }
}

/* 例外へ移った後は対話を続けない（#68 backlog: cp_take_exception / cp_violation の戻り値）。
 * protocol violation の後にも終了応答（NULL_PF1）を用意しておき、読まれなければ CIR のログは
 * command 書込みと 応答読出しの 2 件だけ。 */
static void test_violation_stops_dialog(mc68k_model_t model) {
    static const struct {
        uint16_t resp;
        uint16_t ea;
    } sites[] = {
        {EA_TO_CP(VEA_ALT, 3), EA_DN(1)}, /* Dn への転送で長さが 1/2/4 でない */
        {EA_TO_MPU(VEA_ANY, 4), 0x3A},    /* 変更できない EA（PC 相対）への書込み */
        {EA_TO_CP(VEA_ANY, 3), EA_IMM},   /* 即値で長さが奇数（1 以外） */
        {FPREGS_TO_MPU(3), EA_AN_PRE(0)}, /* FP レジスタ群で長さが奇数 */
    };

    for (unsigned i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
        mach_t *m = host_machine(model);

        mach_response(m, sites[i].resp);
        mach_response(m, NULL_PF1);
        m->cpu->a[0] = 0x4000;
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, sites[i].ea), 0x4000, 0x0000, 0x0000);
        mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
        expect_nlog(m, 2);
        mach_destroy(m);
    }

    /* 応答の読み直しが上限を超えたら protocol violation。cpScc の完了処理（Dn
     * の書込み）へは進まない */
    mach_t *m = host_machine(model);
    mach_response(m, NULL_AGAIN);
    m->cpu->d[3] = 0x123456AAu;
    PROG(m, MACH_START, OP_CP(CPID, T_COND, EA_DN(3)), 0x0001);
    mach_run_until(m, MACH_HANDLER(VEC_VIOLATION), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_VIOLATION));
    CHECK_EQ(m->cpu->d[3], 0x123456AAu);
    mach_destroy(m);
}

/* CIR・メモリ・命令ストリームの失敗で消費するサイクルは、同じ種類のフォルトを起こす通常の命令と同じ
 * （ハンドラの早期 return 値と、戻り値 false で完了処理へ進まないことの観測。68020+ では An
 * の巻き戻しで レジスタには出ない）。#68 backlog: cp_dialog / cp_fp_registers /
 * cp_fetch_branch_target / cp_cir_fault */
typedef enum { FAULT_REF_WRITE, FAULT_REF_FETCH } fault_ref_t;

static int fault_ref_cycles(mc68k_model_t model, fault_ref_t kind) {
    mach_t *m = host_machine(model);
    int cycles;

    m->cpu->a[0] = MACH_RAM + 0x100;
    if (kind == FAULT_REF_WRITE) {
        PROG(m, MACH_START, 0x50D0 /* ST (A0) */);
    } else {
        PROG(m, MACH_START, 0x4EF9 /* JMP abs.l */, (uint16_t)((MACH_RAM - 2) >> 16),
             (uint16_t)(MACH_RAM - 2));
        PROG(m, MACH_RAM - 2, 0x1028 /* MOVE.B d16(A0),D0（拡張語が RAM の外） */);
        mc68k_execute(m->cpu, 1);
    }
    cycles = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    mach_destroy(m);
    return cycles;
}

static int fault_cp_cycles(mach_t *m, uint32_t at) {
    int cycles;

    if (at != MACH_START) {
        PROG(m, MACH_START, 0x4EF9, (uint16_t)(at >> 16), (uint16_t)at);
        mc68k_execute(m->cpu, 1);
    }
    cycles = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    mach_destroy(m);
    return cycles;
}

static void test_cp_fault_cycles(mc68k_model_t model) {
    const int ref_write = fault_ref_cycles(model, FAULT_REF_WRITE);
    const int ref_fetch = fault_ref_cycles(model, FAULT_REF_FETCH);
    const uint32_t outside = MACH_RAM + 0x100;
    mach_t *m;

    /* 応答 CIR の読出し失敗（対話の最初の読出し） */
    m = host_machine(model);
    mach_response(m, NULL_PF1);
    m->fail_cir = CIR_RESPONSE;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_DN(0)), 0x0000);
    CHECK_EQ(fault_cp_cycles(m, MACH_START), ref_write);

    /* FP レジスタ群: 拡張語の取得失敗、レジスタセレクト CIR の読出し失敗、メモリの読出し失敗 */
    m = host_machine(model);
    mach_response(m, FPREGS_TO_CP(2));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = 0x4000;
    PROG(m, MACH_RAM - 4, OP_CP(CPID, T_GEN, 0x28 /* d16(A0) */), 0x6800);
    CHECK_EQ(fault_cp_cycles(m, MACH_RAM - 4), ref_fetch);

    m = host_machine(model);
    mach_response(m, FPREGS_TO_CP(4));
    mach_response(m, NULL_PF1);
    m->regsel = 1;
    m->fail_cir = CIR_REGSEL;
    m->cpu->a[0] = 0x4000;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6800);
    CHECK_EQ(fault_cp_cycles(m, MACH_START), ref_write);

    m = host_machine(model);
    mach_response(m, FPREGS_TO_CP(4));
    mach_response(m, NULL_PF1);
    m->regsel = 1;
    m->cpu->a[0] = outside;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x6800);
    CHECK_EQ(fault_cp_cycles(m, MACH_START), ref_write);

    /* cpBcc.W: 変位ワードの取得失敗 */
    m = host_machine(model);
    mach_response(m, NULL_TF1);
    PROG(m, MACH_RAM - 2, OP_CP(CPID, T_BCC_W, 0x2A));
    CHECK_EQ(fault_cp_cycles(m, MACH_RAM - 2), ref_fetch);
}

/* 失敗した命令は、その後に CIR にもメモリにも触れない（#68: 失敗後の `return`
 * の脱落を検出する）。 失敗させる箇所ごとに、(1) 通常命令の同種のフォルトと同じサイクル、(2)
 * 対話中なら最後のログが AB の書込み、 (3)
 * 失敗の後に書かれるはずのない値（メモリの目印、Dn）が変わっていない、を確かめる */
static int fault_run(mach_t *m, uint32_t at) {
    if (at != MACH_START) {
        PROG(m, MACH_START, 0x4EF9, (uint16_t)(at >> 16), (uint16_t)at);
        mc68k_execute(m->cpu, 1);
    }
    const int cycles = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    return cycles;
}

static void expect_aborted_last(const mach_t *m) {
    CHECK(m->nlog > 0);
    CHECK_EQ(m->nlog, m->nlog_total);
    const cs_log_t *last = &m->log[m->nlog - 1];
    CHECK(last->write);
    CHECK_EQ(last->addr & 0x1Fu, (uint32_t)CIR_CONTROL);
    CHECK_EQ(last->val, 1u); /* CP_CONTROL_ABORT */
}

#define FAULT_MARK 0xA5u
static void mark_ram(mach_t *m, uint32_t addr, unsigned n) {
    memset(&m->ram[addr], FAULT_MARK, n);
}

static void expect_marked(const mach_t *m, uint32_t addr, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        CHECK_EQ(m->ram[addr + i], FAULT_MARK);
}

static void test_fault_stops_dialog(mc68k_model_t model) {
    const int ref_write = fault_ref_cycles(model, FAULT_REF_WRITE);
    const int ref_fetch = fault_ref_cycles(model, FAULT_REF_FETCH);
    const uint32_t data = 0x4000;
    mach_t *m;

    /* cpGEN EA→MPU（(A0)、4 バイト）のオペランド CIR の読出し失敗: メモリへ何も書かない */
    m = host_machine(model);
    mach_response(m, EA_TO_MPU(VEA_ANY, 4));
    mach_response(m, NULL_PF1);
    m->fail_cir = CIR_OPERAND;
    m->cpu->a[0] = data;
    mark_ram(m, data, 16);
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_AN_IND(0)), 0x0000);
    CHECK_EQ(fault_run(m, MACH_START), ref_write);
    expect_aborted_last(m);
    expect_marked(m, data, 16);
    mach_destroy(m);

    /* cpGEN EA→cp（d16(A0)）の拡張語の取得失敗: オペランド CIR へ書かない */
    m = host_machine(model);
    mach_response(m, EA_TO_CP(VEA_ANY, 4));
    mach_response(m, NULL_PF1);
    m->cpu->a[0] = data;
    PROG(m, MACH_RAM - 4, OP_CP(CPID, T_GEN, 0x28 /* d16(A0) */), 0x4000);
    CHECK_EQ(fault_run(m, MACH_RAM - 4), ref_fetch);
    expect_aborted_last(m);
    CHECK_EQ(m->nlog_total, 3); /* command、応答、AB。失敗の後にもう一度 AB を書かない */
    mach_destroy(m);

    /* 命令アドレス CIR の書込み失敗: 応答の読出しへ進まない */
    m = host_machine(model);
    mach_response(m, NULL_PC_FIN);
    m->fail_cir = CIR_IA;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    CHECK_EQ(fault_run(m, MACH_START), ref_write);
    expect_aborted_last(m);
    mach_destroy(m);

    /* cpDBcc の変位の取得失敗: カウンタを減らさない */
    m = host_machine(model);
    mach_response(m, NULL_TF0);
    m->cpu->d[2] = 5;
    PROG(m, MACH_RAM - 4, OP_CP(CPID, T_COND, 0x08 | 2), 0x0001);
    CHECK_EQ(fault_run(m, MACH_RAM - 4), ref_fetch);
    CHECK_EQ(m->cpu->d[2], 5u);
    mach_destroy(m);

    /* cpScc d16(A0) の拡張語の取得失敗 */
    m = host_machine(model);
    mach_response(m, NULL_TF1);
    m->cpu->a[0] = data;
    mark_ram(m, 0, 4);
    PROG(m, MACH_RAM - 4, OP_CP(CPID, T_COND, 0x28 /* d16(A0) */), 0x0001);
    CHECK_EQ(fault_run(m, MACH_RAM - 4), ref_fetch);
    expect_marked(m, 0, 4); /* 失敗後のアドレス 0 への書込みが無い */
    mach_destroy(m);

    /* cpSAVE: save CIR の読出し失敗（F-line。メモリへ書かず、CIR にはその 1 回しか触れない） */
    m = host_machine(model);
    m->fail_cir = CIR_SAVE;
    m->cpu->a[1] = data;
    mark_ram(m, data, 16);
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(1)));
    mach_run_until(m, MACH_HANDLER(VEC_FLINE), 5);
    CHECK_EQ(m->nlog_total, 1);
    expect_marked(m, data, 16);
    mach_destroy(m);

    /* cpSAVE d16(A0): 拡張語の取得失敗 */
    m = host_machine(model);
    m->save_format = 0x1804;
    m->cpu->a[0] = data;
    PROG(m, MACH_RAM - 2, OP_CP(CPID, T_SAVE, 0x28));
    CHECK_EQ(fault_run(m, MACH_RAM - 2), ref_fetch);
    expect_aborted_last(m);
    CHECK_EQ(m->nlog_total, 2); /* save CIR の読出しと AB だけ */
    mach_destroy(m);

    /* cpSAVE: body のオペランド CIR の読出し失敗（header は書いたあと。body
     * の位置へは書かない） */
    m = host_machine(model);
    m->save_format = 0x1804;
    m->fail_cir = CIR_OPERAND;
    m->cpu->a[0] = data;
    mark_ram(m, data, 16);
    PROG(m, MACH_START, OP_CP(CPID, T_SAVE, EA_AN_IND(0)));
    CHECK_EQ(fault_run(m, MACH_START), ref_write);
    expect_aborted_last(m);
    expect_marked(m, data + 4, 12);
    mach_destroy(m);

    /* cpRESTORE d16(A0): 拡張語の取得失敗（CIR には触れない） */
    m = host_machine(model);
    m->cpu->a[0] = data;
    PROG(m, MACH_RAM - 2, OP_CP(CPID, T_RESTORE, 0x28));
    CHECK_EQ(fault_run(m, MACH_RAM - 2), ref_fetch);
    CHECK_EQ(m->nlog_total, 0);
    mach_destroy(m);

    /* cpRESTORE: restore CIR の読返しの失敗（書込みは成功したあと） */
    m = host_machine(model);
    m->fail_cir = CIR_RESTORE;
    m->fail_after = 1;
    m->cpu->a[1] = data;
    put16(m, data, 0x1804);
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    CHECK_EQ(fault_run(m, MACH_START), ref_write);
    CHECK_EQ(m->nlog_total, 3);
    expect_aborted_last(m);
    mach_destroy(m);

    /* cpRESTORE: body のオペランド CIR の書込み失敗 */
    m = host_machine(model);
    m->restore_readback = 0x1804;
    m->fail_cir = CIR_OPERAND;
    m->cpu->a[1] = data;
    put16(m, data, 0x1804);
    PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, EA_AN_IND(1)));
    CHECK_EQ(fault_run(m, MACH_START), ref_write);
    expect_aborted_last(m);
    mach_destroy(m);
}

/* 例外フレームの各バイトを、ちょうど 1 回ずつ書く（#68 backlog:
 * cpu_dispatch_coproc_exception）。 値が同じでも、書込みの範囲を重ねる変異（内部レジスタ word
 * を long で書いて次の word に重ねる、など）は、
 * 同じバイトへの書込みの重複としてバスアクセス（アドレスと size）に出る。long
 * のバス分割は境界で変わるので、 アクセスの個数でなくバイトごとの書込み回数を数える。 */
#define FRAME_BYTES_MAX 20u
static unsigned frame_byte_writes[FRAME_BYTES_MAX];
static uint32_t frame_lo, frame_hi;

static void frame_write_recorder(void *ctx, const mc68k_bus_access_t *a,
                                 mc68k_bus_timing_t *out) {
    (void)ctx;
    (void)out;
    if (!a->read && a->fc != 7 && a->physical >= frame_lo && a->physical < frame_hi)
        for (unsigned k = 0; k < a->size && a->physical + k < frame_hi; k++)
            frame_byte_writes[a->physical + k - frame_lo]++;
}

static void test_exception_frame_write_once(mc68k_model_t model) {
    static const struct {
        uint16_t resp;
        unsigned vec;
        unsigned frame_bytes;
    } kinds[] = {
        {TAKE_PRE(0x30), 0x30, 8}, {TAKE_POST(0x31), 0x31, 12}, {TAKE_MID(0x32), 0x32, 20}};

    for (unsigned k = 0; k < 3; k++) {
        mach_t *m = host_machine(model);

        mach_response(m, kinds[k].resp);
        m->cpu->bus.access_timing = frame_write_recorder;
        memset(frame_byte_writes, 0, sizeof(frame_byte_writes));
        frame_lo = MACH_STACK - kinds[k].frame_bytes;
        frame_hi = MACH_STACK;
        PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_DN(0)), 0x0000);
        mach_run_until(m, MACH_HANDLER(kinds[k].vec), 10);
        CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(kinds[k].vec));
        for (unsigned i = 0; i < kinds[k].frame_bytes; i++)
            CHECK_EQ(frame_byte_writes[i], 1u);
        mach_destroy(m);
    }
}

/* format 9 の再開は、フレームのオペレーションワードの EA 欄（mode 0〜7
 * の全部、bit5〜0）によらず行う （#68 backlog: coproc_resume の F-line 判定のマスク）。cpGEN は
 * EA を使わない応答（NULL）なら EA 欄は任意。 */
static void test_rte_format9_resume_all_ea_fields(mc68k_model_t model) {
    static const uint16_t eas[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x3C};

    for (unsigned i = 0; i < sizeof(eas) / sizeof(eas[0]); i++) {
        mach_t *m = host_machine(model);

        put16(m, MACH_HANDLER(0x32), OP_RTE);
        mach_response(m, TAKE_MID(0x32));
        mach_response(m, NULL_PF1);
        const uint32_t pc =
            PROG(m, MACH_START, OP_CP(CPID, T_GEN, eas[i]), 0x0000, 0x0000, 0x0000);
        mach_run_until(m, pc - 4,
                       200); /* EA 欄に拡張語を要する mode でも、この試験は拡張語を使わない */
        expect_log(m, 0, true, CPID, CIR_COMMAND, 2, 0);
        expect_log(m, 3, false, CPID, CIR_RESPONSE, 2,
                   NULL_PF1); /* 再開した対話が応答 CIR を読む */
        expect_nlog(m, 4);
        mach_destroy(m);
    }
}

/* cpRESTORE は、EA の基準レジスタ以外のアドレスレジスタを更新しない（#68 backlog:
 * cp_resolve_memory の m->reg の初期値）。 (An)+ 以外の EA では、どの An も更新しない。EA
 * の基準レジスタを A1 にして、A0 と A1 を見る */
static void test_restore_updates_no_register_for_plain_ea(mc68k_model_t model) {
    static const struct {
        uint16_t ea;
        bool ext;
    } eas[] = {{EA_AN_IND(1), false}, {0x29 /* d16(A1) */, true}};

    for (unsigned i = 0; i < sizeof(eas) / sizeof(eas[0]); i++) {
        mach_t *m = host_machine(model);
        uint32_t pc;

        m->restore_readback = 0x1008;
        m->cpu->a[0] = 0x00005A5Au;
        m->cpu->a[1] = 0x4000;
        put32(m, 0x4000, 0x1008FFFFu);
        put32(m, 0x4004, 0x11111111u);
        put32(m, 0x4008, 0x22222222u);
        if (eas[i].ext)
            pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, eas[i].ea), 0x0000);
        else
            pc = PROG(m, MACH_START, OP_CP(CPID, T_RESTORE, eas[i].ea));
        run(m, pc);
        CHECK_EQ(m->cpu->a[0], 0x00005A5Au);
        CHECK_EQ(m->cpu->a[1], 0x4000u);
        mach_destroy(m);
    }
}

/* type 6・7 の F-line オペレーションワード（cpID 1〜7）は、従来の F-line（cpID
 * 0）と同じサイクル数・同じ例外になる （#68 backlog: register_coproc_ops。type 6・7 を
 * op_coproc に登録しても挙動が変わらないことの確認） */
static void test_reserved_types_match_legacy_fline(mc68k_model_t model) {
    int legacy;
    mach_t *m = mach_create(model, MC68K_FPU_NONE, false);

    m->cpu->a[7] = MACH_STACK;
    PROG(m, MACH_START, 0xF040 /* cpID=0 */, 0x0000);
    legacy = mc68k_execute(m->cpu, 1);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FLINE));
    mach_destroy(m);

    for (unsigned cpid = 1; cpid < 8; cpid++) {
        for (unsigned type = 6; type < 8; type++) {
            for (unsigned ea = 0; ea < 64; ea += 9) {
                m = mach_create(model, MC68K_FPU_NONE, false);
                m->cpu->a[7] = MACH_STACK;
                PROG(m, MACH_START, OP_CP(cpid, type, ea), 0x0000);
                CHECK_EQ(mc68k_execute(m->cpu, 1), legacy);
                CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_FLINE));
                mach_destroy(m);
            }
        }
    }
}

/* 再開しない format 9 の RTE も、format 0 の RTE と同じく変更フロートレース（T0）の対象（#68
 * backlog: op_rte の flow_changed）。 同じ手順で format 0 と format 9（オペレーションワードが
 * cpSAVE で再開しないフレーム）の RTE を実行し、
 * トレース例外になるかどうかが一致することを見る。 */
static bool rte_traced_by_t0(mc68k_model_t model, bool format9) {
    mach_t *m = host_machine(model);
    const uint32_t frame = 0x5000, ret = 0x1234;
    bool traced;

    put16(m, frame, 0x2000);
    put32(m, frame + 2, ret);
    put16(m, frame + 6, format9 ? 0x9000 : 0x0000);
    if (format9) {
        put32(m, frame + 8, 0x1000);
        put16(m, frame + 14, OP_CP(CPID, T_SAVE, EA_AN_IND(0)));
        put32(m, frame + 16, 0);
    }
    put16(m, ret, 0x4E71);
    PROG(m, MACH_START, OP_RTE);
    m->cpu->a[7] = frame;
    m->cpu->sr |= SR_T0;
    mc68k_execute(m->cpu, 1);
    traced = mc68k_get_pc(m->cpu) == MACH_HANDLER(9);
    mach_destroy(m);
    return traced;
}

static void test_rte_format9_not_resumed_traces_like_format0(mc68k_model_t model) {
    const bool traced0 = rte_traced_by_t0(model, false);

    CHECK(traced0); /* 前提: format 0 の RTE は変更フロートレースになる */
    CHECK_EQ(rte_traced_by_t0(model, true), traced0);
}

/* cpu_dispatch_coproc_exception() のフレーム書込み。フレームの欄は [UM20 §7.4.18〜§7.4.20 の
 * Figure 7-41（PRE = format 0 の 4 word）、Figure 7-45（POST = format 2 の 6 word）、Figure
 * 7-43 （MID = format 9 の 10 word）] から手で組み立てる。ベクタ番号の欄は番号の 4 倍（[UM20
 * §6.1]）。 MID の +0C（INTERNAL REGISTER）は図に値が無く、0 は設計書 §3.8
 * の実装上の選択（推論）。 欄の書込みが 1
 * つ失敗したら、続きの欄もベクタの読出しもしない。失敗した書込みだけが失敗する
 * 機械（fail_write_addr）で、続きの書込みが成功する状況を作って確かめる。
 *
 * ベクタの読出しをしていないことは、PMMU（68030）を有効にして確かめる。ベクタ表のページの記述子は、
 * 表の探索で U ビット（0x8）が立つので、読出しをしていればそのビットで分かる。 */
/* 68030 の PMMU（識別マッピング、4 KiB ページ、2 段の表）。A 表の 0 番に B 表が付き、B 表の
 * i 番が i 番のページを同じアドレスへ写す。表の探索は記述子の U ビット（0x8）を立てる。 */
enum {
    PT_A = 0xC000,
    PT_B = 0xD000,
    PT_PAGE_SHIFT = 12,
    PT_PAGES = 16,
    PD_PAGE_DESC = 1,
    PD_U = 8
};

static void enable_pmmu(mach_t *m) {
    put32(m, PT_A, PT_B | 2u); /* DT=2: 4 バイトの記述子の表 */
    for (uint32_t i = 0; i < PT_PAGES; i++)
        put32(m, PT_B + 4 * i, (i << PT_PAGE_SHIFT) | PD_PAGE_DESC);
    m->cpu->mmu_tc = 0x82C0AA00u; /* E=1、SRE=1、PS=4 KiB、IS=0、TIA=TIB=10 bit */
    m->cpu->mmu_crp = m->cpu->mmu_srp = UINT64_C(0x0000000200000000) | PT_A;
}

static uint32_t pmmu_descriptor(const mach_t *m, uint32_t page) {
    return get32(m, PT_B + 4 * page);
}

typedef struct {
    uint32_t off;
    unsigned size; /* 2 = word、4 = long */
} frame_field_t;

typedef struct {
    cp_exc_kind_t kind;
    uint32_t bytes;
    size_t nfields;
    frame_field_t f[7];
} frame_layout_t;

/* PMMU 版の試験が実際に走った回数。68030 以外では黙って return するので、main で件数を assert
 * する */
static unsigned pmmu_tests_run;
#define PMMU_TESTS_PER_MODEL                                       \
    3 /* frame_write_fault(pmmu), rte_format9_fault_stops_reading, \
         restore_ea_fault_stops_reading */

static void test_coproc_frame_write_fault(mc68k_model_t model, bool pmmu) {
    if (pmmu && model != MC68K_MODEL_68030) /* PMMU があるのは 68030 だけ（68EC030 に無い） */
        return;
    if (pmmu)
        pmmu_tests_run++;
    static const frame_layout_t layouts[] = {
        {CP_EXC_PRE, 8, 3, {{0, 2}, {2, 4}, {6, 2}}},
        {CP_EXC_POST, 12, 4, {{0, 2}, {2, 4}, {6, 2}, {8, 4}}},
        {CP_EXC_MID, 20, 7, {{0, 2}, {2, 4}, {6, 2}, {8, 4}, {12, 2}, {14, 2}, {16, 4}}},
    };
    enum { VECTOR = 13, NOMINAL = 77, SENTINEL = 0xA5, SPAN = 32, VBR_PAGE = 3 };
    const uint32_t vbr = pmmu ? VBR_PAGE << PT_PAGE_SHIFT : 0;
    const uint32_t instr_pc = 0x1230, scan_pc = 0x1238, ea = 0x00005678;
    const uint16_t opword = 0xF234;

    for (size_t li = 0; li < sizeof(layouts) / sizeof(layouts[0]); li++) {
        const frame_layout_t *l = &layouts[li];
        const uint32_t base = MACH_STACK - l->bytes;
        const uint32_t pc_field = l->kind == CP_EXC_PRE ? instr_pc : scan_pc;
        const uint16_t format = l->kind == CP_EXC_PRE    ? 0x0000u
                                : l->kind == CP_EXC_POST ? 0x2000u
                                                         : 0x9000u;

        /* failing = -1: 失敗なし。それ以外は、その番号の欄の書込みだけが失敗する */
        for (int failing = -1; failing < (int)l->nfields; failing++) {
            mach_t *m = host_machine(model);
            mc68k_state_t *cpu = m->cpu;

            memset(&m->ram[MACH_STACK - SPAN], SENTINEL, SPAN);
            cpu->a[7] = MACH_STACK;
            if (pmmu) { /* ベクタ表をほかのページへ移し、そのページには例外以外で触れない */
                put32(m, vbr + VECTOR * 4u, MACH_HANDLER(VECTOR));
                cpu->vbr = vbr;
                enable_pmmu(m);
            }
            cpu->instr_pc = instr_pc;
            cpu->pc = instr_pc + 2;
            const uint16_t saved_sr = cpu->sr;
            if (failing >= 0) {
                m->fail_write_en = true;
                m->fail_write_addr = base + l->f[failing].off;
            }
            const int cycles = cpu_dispatch_coproc_exception(cpu, l->kind, VECTOR, scan_pc,
                                                             opword, ea, NOMINAL);

            const uint32_t expect[7] = {saved_sr, pc_field, (uint32_t)(format | (VECTOR * 4u)),
                                        instr_pc, 0,        opword,
                                        ea};
            for (size_t i = 0; i < l->nfields; i++) {
                const uint32_t at = base + l->f[i].off;
                const bool written = failing < 0 || (int)i < failing;
                for (unsigned b = 0; b < l->f[i].size; b++) {
                    const uint8_t want =
                        written ? (uint8_t)(expect[i] >> (8u * (l->f[i].size - 1u - b)))
                                : (uint8_t)SENTINEL;
                    CHECK_EQ(m->ram[at + b], want);
                }
            }
            if (failing < 0) {
                CHECK_EQ(cycles, NOMINAL);
                CHECK_EQ(cpu->exec_state, CPU_NORMAL);
                CHECK_EQ(cpu->pc, MACH_HANDLER(VECTOR));
                CHECK(cpu->exception_frame_origins == NULL); /* 記録を残さない */
                if (pmmu) /* ベクタを読んだ（この検査が、下の「読んでいない」の対照になる） */
                    CHECK_EQ(pmmu_descriptor(m, VBR_PAGE),
                             (VBR_PAGE << PT_PAGE_SHIFT) | PD_PAGE_DESC | PD_U);
            } else {
                CHECK(cpu->exec_state != CPU_NORMAL);
                CHECK(cpu->pc != MACH_HANDLER(VECTOR)); /* ベクタへ飛ばない */
                /* フレームの書込みに失敗したときの group 0 フレームが使う、元の例外の SR と PC
                 */
                const mc68k_exception_frame_origin_t *o = cpu->exception_frame_origins;
                CHECK(o != NULL && !o->bound);
                CHECK_EQ(o->saved_sr, saved_sr);
                CHECK_EQ(o->saved_pc, pc_field);
                if (pmmu) /* ベクタを読んでいない */
                    CHECK_EQ(pmmu_descriptor(m, VBR_PAGE),
                             (VBR_PAGE << PT_PAGE_SHIFT) | PD_PAGE_DESC);
            }
            mach_destroy(m);
        }
    }
}

/* RTE の format 9 フレームの +8（PC）、+14（OPERATION WORD）、+16（EFFECTIVE
 * ADDRESS）の読出しが 1 つ失敗したら、続きの欄を読まず HALT する。続きの読出しは PMMU
 * の表の探索を起こし、次のページの 記述子の U ビットを立てるので、立っていないことで確かめる。
 *   case 0: フレームを 0x4FF4 に置く。+8 の long（0x4FFC）が失敗し、続きの
 * +14（0x5002）は次のページ case 1: フレームを 0x4FF0 に置く。+14 の
 * word（0x4FFE）が失敗し、続きの +16（0x5000）は次のページ
 */
static void test_rte_format9_fault_stops_reading(mc68k_model_t model) {
    static const struct {
        uint32_t frame;
        uint32_t fail_off;
    } cases[] = {{0x4FF4, 8}, {0x4FF0, 14}};
    enum { NEXT_PAGE = 5 };

    if (model != MC68K_MODEL_68030)
        return;
    pmmu_tests_run++;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mach_t *m = host_machine(model);
        const uint32_t frame = cases[i].frame;

        m->cpu->a[7] = frame;
        put16(m, frame, 0x2000);
        put32(m, frame + 2, 0x1234);
        put16(m, frame + 6, 0x9000);
        PROG(m, MACH_START, OP_RTE);
        enable_pmmu(m);
        m->fail_read_en = true;
        m->fail_read_addr = frame + cases[i].fail_off;
        mc68k_execute(m->cpu, 1);
        CHECK_EQ(mc68k_get_exec_state(m->cpu), CPU_HALTED);
        CHECK_EQ(m->cpu->a[7], frame); /* フレームを pop していない */
        CHECK_EQ(m->cpu->addr_err_addr, frame + cases[i].fail_off); /* 最初の失敗のまま */
        CHECK_EQ(pmmu_descriptor(m, NEXT_PAGE), (NEXT_PAGE << PT_PAGE_SHIFT) | PD_PAGE_DESC);
        mach_destroy(m);
    }
}

/* cpRESTORE d16(A0) の拡張語の取得が失敗したら、続きのメモリ読出しをしない。失敗した EA の
 * アドレスは 0 で返るので、続きを読むとページ 0 の表の探索が起き、その記述子の U ビットが立つ。
 * ベクタ表は別のページへ移してあるので、ページ 0 の U ビットが立たないことで確かめる。 */
static void test_restore_ea_fault_stops_reading(mc68k_model_t model) {
    enum { VBR_PAGE = 3, ZERO_PAGE = 0 };
    const uint32_t vbr = VBR_PAGE << PT_PAGE_SHIFT;

    if (model != MC68K_MODEL_68030)
        return;
    pmmu_tests_run++;
    mach_t *m = host_machine(model);

    m->cpu->a[0] = 0x4000;
    PROG(m, MACH_RAM - 2, OP_CP(CPID, T_RESTORE, 0x28 /* d16(A0) */));
    put32(m, vbr + VEC_BUS_ERROR * 4u, MACH_HANDLER(VEC_BUS_ERROR));
    m->cpu->vbr = vbr;
    enable_pmmu(m);
    /* 拡張語の取得の失敗そのものを固定する（取得が成功すると、別の理由で止まっても U
     * ビットの検査が通る） */
    CHECK_EQ(fault_run(m, MACH_RAM - 2), fault_ref_cycles(model, FAULT_REF_FETCH));
    CHECK_EQ(m->nlog_total, 0); /* CIR に触れない */
    CHECK_EQ(pmmu_descriptor(m, VBR_PAGE), (VBR_PAGE << PT_PAGE_SHIFT) | PD_PAGE_DESC | PD_U);
    CHECK_EQ(pmmu_descriptor(m, ZERO_PAGE), (ZERO_PAGE << PT_PAGE_SHIFT) | PD_PAGE_DESC);
    mach_destroy(m);
}

/* take 系 primitive の例外受理（コントロール CIR
 * への書込み）に失敗したら、例外フレームを積まず、 通常のバスエラーになる。バスエラーの A7
 * は、別の CIR（オペランド CIR）の失敗による同じ
 * 種類のバスエラーと等しい（フレームの大きさは機種で決まり、失敗の原因によらない）。 */
static void test_take_ack_write_fault(mc68k_model_t model) {
    mach_t *m = host_machine(model);

    m->fail_cir = CIR_CONTROL;
    mach_response(m, TAKE_PRE(0x30));
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, 0), 0x0000);
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    CHECK_EQ(m->cpu->last_exception_vector, VEC_BUS_ERROR);
    const uint32_t a7_take_ack = m->cpu->a[7];
    mach_destroy(m);

    m = host_machine(model);
    mach_response(m, EA_TO_CP(VEA_DATA, 4));
    mach_response(m, NULL_PF1);
    m->fail_cir = CIR_OPERAND;
    PROG(m, MACH_START, OP_CP(CPID, T_GEN, EA_IMM), 0x4000, 0, 3);
    mach_run_until(m, MACH_HANDLER(VEC_BUS_ERROR), 5);
    CHECK_EQ(mc68k_get_pc(m->cpu), MACH_HANDLER(VEC_BUS_ERROR));
    CHECK_EQ(m->cpu->a[7], a7_take_ack);
    mach_destroy(m);
}

int main(void) {
    for (size_t i = 0; i < NMODELS; i++) {
        test_register_transfer_bits(models[i]);
        test_conditional_register_bits(models[i]);
        test_supervisor_check_conditional(models[i]);
        test_scc_memory_fault_keeps_an(models[i]);
        test_scc_memory_fault_cycles(models[i]);
        test_memory_to_operand_lengths(models[i]);
        test_operand_to_memory_lengths(models[i]);
        test_operand_to_memory_fault_cycles(models[i]);
        test_immediate_byte_bits(models[i]);
        test_reg4_memory_modes_not_immediate(models[i]);
        test_ea_extension_fetch_fault_cycles(models[i]);
        test_autoinc_updates_all_32_bits(models[i]);
        test_register_to_operand_bits(models[i]);
        test_format_code_bits(models[i]);
        test_format_length_bits(models[i]);
        test_null_response_bits(models[i]);
        test_violation_stops_dialog(models[i]);
        test_cp_fault_cycles(models[i]);
        test_fault_stops_dialog(models[i]);
        test_cp_exception_cycles_match_legacy(models[i]);
        test_rte_non_format9_reads_only_its_frame(models[i]);
        test_coproc_exception_vector_fault_halts(models[i]);
        test_exception_frame_write_once(models[i]);
        test_rte_format9_resume_all_ea_fields(models[i]);
        test_restore_updates_no_register_for_plain_ea(models[i]);
        test_reserved_types_match_legacy_fline(models[i]);
        test_rte_format9_not_resumed_traces_like_format0(models[i]);
        fprintf(stderr, "model %d\n", (int)models[i]);
        for (unsigned cpid = 1; cpid < 8; cpid++)
            test_general_cir_cycles(models[i], cpid);
        test_pass_pc(models[i]);
        test_absent_coprocessor(models[i]);
        test_cir_bus_errors(models[i]);
        test_conditional(models[i]);
        test_save(models[i]);
        test_restore(models[i]);
        test_ea_transfer_to_mpu(models[i]);
        test_ea_transfer_to_coprocessor(models[i]);
        test_fp_registers(models[i]);
        test_mp_register(models[i]);
        test_control_primitives(models[i]);
        test_take_exceptions(models[i]);
        test_protocol_violation(models[i]);
        test_bus_timing(models[i]);
        test_memory_bus_errors(models[i]);
        test_supervisor_check_ca0(models[i]);
        test_trace_pending_non_null(models[i]);
        test_rte_format9_keeps_instr_pc(models[i]);
        test_privilege_before_presence(models[i]);
        test_fp_registers_order_and_mask(models[i]);
        test_fp_registers_ea_before_regsel(models[i]);
        test_misc_boundaries(models[i]);
        test_resume_details(models[i]);
        test_ca0_terminates_without_trace(models[i]);
        test_rte_format9_not_resumed(models[i]);
        test_invalid_ea_before_privilege(models[i]);
        test_restore_length_from_memory(models[i]);
        test_restore_transfer_follows_readback(models[i]);
        test_scc_invalid_ea(models[i]);
        test_trace_on_change_of_flow(models[i]);
        test_conditional_termination(models[i]);
        test_valid_ea_classes(models[i]);
        test_rte_format9_not_resumed_more(models[i]);
        test_take_with_ca(models[i]);
        test_length_boundaries(models[i]);
        test_bus_timing_size_and_direction(models[i]);
        test_autoinc_not_committed_on_fault(models[i]);
        test_exception_attributes(models[i]);
        test_restore_memory_not_valid_readback_valid(models[i]);
        test_cpid_routing_with_internal_fpu(models[i]);
        test_exception_clears_trace_bits(models[i]);
        test_vbr_relative_vectors(models[i]);
        test_rte_format9_extra_fields_fault(models[i]);
        test_absent_fline_cycles_match_legacy(models[i]);
        test_rte_format9_ea_field_fault(models[i]);
        test_coproc_frame_write_fault(models[i], false);
        test_coproc_frame_write_fault(models[i], true);
        test_rte_format9_fault_stops_reading(models[i]);
        test_restore_ea_fault_stops_reading(models[i]);
        test_take_ack_write_fault(models[i]);
        test_restore_reserved_readback(models[i]);
        test_restore_readback_bus_error(models[i]);
        test_coproc_exception_double_fault(models[i]);
        test_rte_format9_resume_in_user_mode(models[i]);
        test_rte_format9_resume_all_cpids(models[i]);
        test_rte_format9_long_fields(models[i]);
        test_exception_frame_extent(models[i]);
        test_take_vector_8bit(models[i]);
        test_port_requires_both_hooks(models[i]);
        test_cir_fault_after_first_access(models[i]);
        test_cir_fault_frame_attributes(models[i]);
        test_memory_fault_stops_cir_traffic(models[i]);
        test_violation_frame_ea(models[i]);
        test_exception_keeps_sr_bits(models[i]);
        test_t0_terminates_cpgen(models[i]);
        test_primitive_fixed_bits(models[i]);
        test_conditional_ca1_transfers(models[i]);
        test_ea_encoding_boundaries(models[i]);
        test_format_word_widths(models[i]);
        test_register_and_type_ranges(models[i]);
        test_pc_bit_in_fixed_bit_primitives(models[i]);
    }
    unsigned n_pmmu_models = 0;
    for (size_t i = 0; i < NMODELS; i++)
        n_pmmu_models += models[i] == MC68K_MODEL_68030;
    CHECK(n_pmmu_models > 0);
    CHECK_EQ(pmmu_tests_run, n_pmmu_models * PMMU_TESTS_PER_MODEL);
    test_old_models_unaffected();
    test_68040_has_no_cir_interface();
    return 0;
}
