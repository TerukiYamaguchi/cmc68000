/*
 * MC68060-G (#102) T-G1。設計書 docs/mc68000-internals/reference/cpu_68060.md §3.0「060 の命令時間」、§10.1。
 * [060UM §10] Table 10-5〜10-26 の代表値を、1 命令を実行した戻り値で固定する。
 * 期待値はすべて 060UM の印字値（キャッシュ 100% ヒット、単独実行）。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define RAM_SIZE        0x80000u
#define SSP_INIT        0x8000u
#define RESET_PC        0x100u
#define TEST_PC         0x400u
#define OPERAND         0x6000u
#define HANDLER         0x3000u
#define SR_SUP          0x2700
#define SR_SUP_Z        0x2704
#define RTE_OPCODE      0x4e73
#define CACR_EBC        0x00800000u
#define CACR_CABC       0x00400000u
#define CACR_CUBC       0x00200000u
#define SR_USER         0x0000
#define CINVA_IC        0xf498
#define CINVA_DC        0xf458
#define CPUSHA_IC       0xf4b8
#define MOVEC_FROM_CACR 0x4e7a
#define MOVEC_CACR      0x4e7b
#define CACR_REG        0x0002
#define VBR_REG         0x0801
#define TC_REG          0x0003
#define BNE_SELF        0x66fe
#define CODE_NEXT       (TEST_PC + 2u)

static uint8_t ram[RAM_SIZE];
static mc68k_state_t *cpu_g;

static bool rb(void *c, uint32_t a, uint8_t *v) {
    (void)c;
    if (a >= RAM_SIZE)
        return false;
    *v = ram[a];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    (void)c;
    if (a + 2 > RAM_SIZE)
        return false;
    *v = (uint16_t)(ram[a] << 8 | ram[a + 1]);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    (void)c;
    if (a + 4 > RAM_SIZE)
        return false;
    *v = (uint32_t)ram[a] << 24 | (uint32_t)ram[a + 1] << 16 | (uint32_t)ram[a + 2] << 8 |
         ram[a + 3];
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    (void)c;
    if (a >= RAM_SIZE)
        return false;
    ram[a] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    (void)c;
    if (a + 2 > RAM_SIZE)
        return false;
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    (void)c;
    if (a + 4 > RAM_SIZE)
        return false;
    ram[a] = (uint8_t)(v >> 24);
    ram[a + 1] = (uint8_t)(v >> 16);
    ram[a + 2] = (uint8_t)(v >> 8);
    ram[a + 3] = (uint8_t)v;
    return true;
}
static void reset_peripheral(void *c) {
    (void)c;
}

static void put_word(uint32_t a, uint16_t v) {
    if (!ww(NULL, a, v))
        abort();
}
static void put_long(uint32_t a, uint32_t v) {
    if (!wl(NULL, a, v))
        abort();
}

static void init_state(void) {
    memset(ram, 0, sizeof ram);
    put_long(0, SSP_INIT);
    put_long(4, RESET_PC);
    for (int v = 2; v < 256; ++v) {
        put_long((uint32_t)v * 4u, HANDLER + (uint32_t)v * 8u);
        put_word(HANDLER + (uint32_t)v * 8u, RTE_OPCODE);
    }
    mc68k_reset(cpu_g);
    cpu_g->usp = 0x7000u;
    mc68k_set_irq(cpu_g, 0);
    cpu_g->irq_defer = false;
    cpu_g->pcr = 0;
}

/* words を TEST_PC に置いて 1 命令実行し、戻り値（消費 cycle）を返す。 */
static int run_words(uint16_t sr, const uint16_t *w, int n, uint32_t d0) {
    init_state();
    for (int i = 0; i < n; ++i)
        put_word(TEST_PC + (uint32_t)i * 2u, w[i]);
    for (int i = 0; i < 8; ++i)
        cpu_g->d[i] = 0;
    for (int i = 0; i < 7; ++i)
        cpu_g->a[i] = OPERAND;
    cpu_g->d[0] = d0;
    cpu_g->d[1] = 100;
    cpu_g->a[7] = SSP_INIT;
    cpu_g->pc = TEST_PC;
    cpu_set_sr(cpu_g, sr);
    return mc68k_execute(cpu_g, 1);
}

#define CHECK(label, expected, sr, d0, ...)                                                 \
    do {                                                                                    \
        const uint16_t w_[] = {__VA_ARGS__};                                                \
        const int got_ = run_words(sr, w_, (int)(sizeof w_ / sizeof w_[0]), d0);            \
        if (got_ != (expected)) {                                                           \
            fprintf(stderr, "FAIL %s: expected %d got %d\n", label, (int)(expected), got_); \
            failures++;                                                                     \
        }                                                                                   \
    } while (0)

static int failures;

/* Table 10-6/10-7（MOVE）、10-10（immediate）、10-9（standard）、10-20（LEA/PEA/MOVEM）。 */
static void t_g1_integer(void) {
    CHECK("MOVEQ", 1, SR_SUP, 3, 0x7001);
    CHECK("MOVE.L Dn,Dn", 1, SR_SUP, 3, 0x2200);
    CHECK("MOVE.L (An),Dn", 1, SR_SUP, 3, 0x2210);
    CHECK("MOVE.L (An),(An)", 2, SR_SUP, 3, 0x2290);
    CHECK("MOVE.L #imm,(An)", 1, SR_SUP, 3, 0x20bc, 0x0000, 0x0001);
    CHECK("MOVE.L #imm,(d16,An)", 2, SR_SUP, 3, 0x217c, 0x0000, 0x0001, 0x0010);
    CHECK("MOVE.L (d16,An),(An)", 2, SR_SUP, 3, 0x22a8, 0x0010);
    CHECK("MOVE.L Dn,(bd,An,Xi)", 2, SR_SUP, 3, 0x2180, 0x1930, 0x0000, 0x0010);
    CHECK("MOVE.L (bd,An,Xi),Dn", 2, SR_SUP, 3, 0x2230, 0x1930, 0x0000, 0x0010);
    CHECK("MOVE.L Dn,([bd,An,Xi],od)", 4, SR_SUP, 3, 0x2180, 0x1932, 0x0000, 0x0010, 0x0004);
    CHECK("ADD.L Dn,Dn", 1, SR_SUP, 3, 0xd280);
    CHECK("ADD.L (An),Dn", 1, SR_SUP, 3, 0xd290);
    CHECK("ADDQ.L #1,(An)", 1, SR_SUP, 3, 0x5290);
    CHECK("ADDI.L #imm,(d16,An)", 2, SR_SUP, 3, 0x06a8, 0x0000, 0x0001, 0x0010);
    CHECK("MULU.W (An),Dn", 2, SR_SUP, 3, 0xc2d0);
    CHECK("DIVU.W Dn,Dn", 22, SR_SUP, 3, 0x82c0);
    CHECK("MULS.L Dn,Dn", 2, SR_SUP, 3, 0x4c00, 0x1800);
    CHECK("DIVU.L Dn,Dn", 38, SR_SUP, 3, 0x4c40, 0x2002);
    CHECK("LEA (An),An", 1, SR_SUP, 3, 0x43d0);
    CHECK("PEA (An)", 1, SR_SUP, 3, 0x4850);
    CHECK("PEA (d16,An)", 2, SR_SUP, 3, 0x4868, 0x0010);
    CHECK("MOVEM.L Dn-Dn3,(An)", 4, SR_SUP, 3, 0x48d0, 0x000f);
    CHECK("MOVEM.L (An),D0-D3", 4, SR_SUP, 3, 0x4cd0, 0x000f);
    CHECK("MOVEM.L (d8,An,Xi),D0-D3", 5, SR_SUP, 3, 0x4cf0, 0x000f, 0x1800);
    CHECK("LSL.L #1,Dn", 1, SR_SUP, 3, 0xe388);
    CHECK("BTST #3,Dn", 1, SR_SUP, 3, 0x0800, 0x0003);
    CHECK("BCHG Dn,(An)", 1, SR_SUP, 3, 0x0150);
    CHECK("TAS (An)", 17, SR_SUP, 3, 0x4ad0);
    CHECK("CAS.L Dc,Du,(An)", 19, SR_SUP, 3, 0x0ed0, 0x0001);
    CHECK("CMPM.L (Ay)+,(Ax)+", 2, SR_SUP, 3, 0xb389 | 0x0000);
    CHECK("ADDX.L -(Ay),-(Ax)", 2, SR_SUP, 3, 0xd388 | 0x0000);
    CHECK("EXT.W Dn", 1, SR_SUP, 3, 0x4880);
    CHECK("LINK.W An,#0", 2, SR_SUP, 3, 0x4e51, 0x0000);
    CHECK("UNLK An", 1, SR_SUP, 3, 0x4e59);
}

/* Table 10-22〜10-24（SR、MOVEC、その他）。 */
static void t_g1_system(void) {
    CHECK("NOP", 9, SR_SUP, 3, 0x4e71);
    CHECK("ORI to SR", 5, SR_SUP, 3, 0x007c, 0x0000);
    CHECK("ANDI to SR", 12, SR_SUP, 3, 0x027c, 0x2700);
    CHECK("ORI to CCR", 1, SR_SUP, 3, 0x003c, 0x0000);
    CHECK("MOVEC Rn,SFC", 12, SR_SUP, 3, 0x4e7b, 0x0000);
    CHECK("MOVEC USP,Rn", 11, SR_SUP, 3, 0x4e7a, 0x0800);
    CHECK("MOVEC Rn,CACR", 15, SR_SUP, 3, 0x4e7b, 0x0002);
    CHECK("MOVEC CACR,Rn", 14, SR_SUP, 3, 0x4e7a, 0x0002);
    CHECK("MOVE16 (An)+,(xxx).L", 18, SR_SUP, 3, 0xf600, 0x0000, 0x6100);
    CHECK("CINVA", 17, SR_SUP, 3, 0xf4d8);
    CHECK("CPUSHL (An)", 16, SR_SUP, 3, 0xf4e8); /* dirty line なし。dirty 時は期待値 CSV で検査 */
    CHECK("PFLUSHA", 33, SR_SUP, 3, 0xf518);
}

/* Table 10-16（bit field）。 */
static void t_g1_bitfield(void) {
    CHECK("BFTST Dn{0:8}", 6, SR_SUP, 3, 0xe8c0, 0x0008);
    CHECK("BFEXTU (An){0:8},Dn", 6, SR_SUP, 3, 0xe9d0, 0x1008);
    CHECK("BFEXTU (An){4:32},Dn (5 bytes)", 8, SR_SUP, 3, 0xe9d0, 0x1100);
    CHECK("BFFFO (An){0:8},Dn", 9, SR_SUP, 3, 0xedd0, 0x1008);
    CHECK("BFCHG (An){0:8}", 8, SR_SUP, 3, 0xeaD0, 0x0008);
    CHECK("BFINS Dn,(An){0:8}", 6, SR_SUP, 3, 0xefd0, 0x1008);
}

/* Table 10-25（FPU）。 */
static void t_g1_fpu(void) {
    CHECK("FMOVE FPm,FPn", 1, SR_SUP, 3, 0xf200, 0x0080);
    CHECK("FADD FPm,FPn", 3, SR_SUP, 3, 0xf200, 0x00a2);
    CHECK("FDIV FPm,FPn", 37, SR_SUP, 3, 0xf200, 0x00a0 | 0x0000);
    CHECK("FSQRT FPm,FPn", 68, SR_SUP, 3, 0xf200, 0x0084);
    CHECK("FMOVE.L Dn,FPn", 6, SR_SUP, 3, 0xf200, 0x4000);
    CHECK("FMOVE.X (An),FPn", 3, SR_SUP, 3, 0xf210, 0x4800);
    CHECK("FMOVE.S (An),FPn", 1, SR_SUP, 3, 0xf210, 0x4400);
    CHECK("FADD.D (An),FPn", 3, SR_SUP, 3, 0xf210, 0x5422);
    CHECK("FMOVEM (An),FP0-FP1", 7, SR_SUP, 3, 0xf210, 0xd003);
}

/* Table 10-17/10-18（分岐）。branch cache は EBC=0 なら静的予測列。 */
static void t_g1_branch(void) {
    CHECK("BEQ.B forward taken", 7, SR_SUP_Z, 3, 0x6702);
    CHECK("BEQ.B forward not taken", 1, SR_SUP, 3, 0x6702);
    CHECK("BNE.B backward taken", 3, SR_SUP, 3, 0x66fe);
    CHECK("BNE.B backward not taken", 7, SR_SUP_Z, 3, 0x66fe);
    CHECK("BRA.B", 3, SR_SUP, 3, 0x6002);
    CHECK("BSR.B", 3, SR_SUP, 3, 0x6102);
    CHECK("DBRA taken", 3, SR_SUP, 3, 0x51c8, 0xfffe);
    CHECK("DBRA exit", 7, SR_SUP, 0, 0x51c8, 0xfffe);
    CHECK("DBNE exit (cc true)", 8, SR_SUP, 3, 0x56c8, 0xfffe);
    CHECK("JMP (d16,PC)", 3, SR_SUP, 3, 0x4efa, 0x0002);
    CHECK("JMP (An)", 5, SR_SUP, 3, 0x4ed0);
    CHECK("JSR (An)", 5, SR_SUP, 3, 0x4e90);
    CHECK("FBF forward not taken", 2, SR_SUP, 3, 0xf280, 0x0004);
    CHECK("FBT forward taken", 8, SR_SUP, 3, 0xf28f, 0x0004);
}

/* EBC=1 の branch cache: 2 回目の taken は予測成功列（表は 0、実装は 1 に切り上げ）、not taken
 * は予測失敗列（7）。 */
static void t_g1_branch_cache(void) {
    init_state();
    put_word(TEST_PC, 0x66fe); /* BNE.B * */
    for (int i = 0; i < 8; ++i)
        cpu_g->d[i] = 0;
    cpu_g->a[7] = SSP_INIT;
    cpu_g->cacr |= CACR_EBC;
    cpu_set_sr(cpu_g, SR_SUP);
    cpu_g->pc = TEST_PC;
    int first = mc68k_execute(cpu_g, 1);
    cpu_g->pc = TEST_PC;
    int second = mc68k_execute(cpu_g, 1);
    cpu_set_sr(cpu_g, SR_SUP_Z);
    cpu_g->pc = TEST_PC;
    int wrong = mc68k_execute(cpu_g, 1);
    cpu_g->pc = TEST_PC;
    int unregistered = mc68k_execute(cpu_g, 1);
    if (first != 3 || second != 1 || wrong != 7 || unregistered != 7) {
        fprintf(stderr, "FAIL branch cache: %d %d %d %d\n", first, second, wrong, unregistered);
        failures++;
    }
    /* CABC で登録を消すと、同じ分岐が再び静的予測列に戻る。 */
    cpu_set_sr(cpu_g, SR_SUP);
    cpu_g->pc = TEST_PC;
    (void)mc68k_execute(cpu_g, 1);
    put_word(TEST_PC + 2, 0x4e7b);
    put_word(TEST_PC + 4, 0x0002);
    cpu_g->d[0] = CACR_EBC | CACR_CABC;
    cpu_g->pc = TEST_PC + 2;
    (void)mc68k_execute(cpu_g, 1);
    cpu_g->pc = TEST_PC;
    int after_clear = mc68k_execute(cpu_g, 1);
    if (after_clear != 3) {
        fprintf(stderr, "FAIL branch cache CABC: %d\n", after_clear);
        failures++;
    }
}

/* branch cache の状態遷移（[060UM §10.11] の予測列、§5.2.1・§5.11 の消去規則、設計書
 * §10.2）。BNE.B * を prime で登録し、操作の後に
 * 同じ分岐を測る。登録済みなら予測成功列（1）、消えていれば静的予測列（backward taken は 3）。
 */
static void bc_start_cacr(uint16_t prime_sr, uint32_t cacr_bits) {
    init_state();
    put_word(TEST_PC, BNE_SELF);
    for (int i = 0; i < 8; ++i)
        cpu_g->d[i] = 0;
    cpu_g->a[7] = SSP_INIT;
    cpu_g->cacr |= cacr_bits;
    cpu_set_sr(cpu_g, prime_sr);
    cpu_g->pc = TEST_PC;
    (void)mc68k_execute(cpu_g, 1);
}

static void bc_start(uint16_t prime_sr) {
    bc_start_cacr(prime_sr, CACR_EBC);
}

static void bc_supervisor_op(uint16_t w0, uint16_t w1, uint32_t d0) {
    put_word(CODE_NEXT, w0);
    put_word(CODE_NEXT + 2u, w1);
    cpu_g->d[0] = d0;
    cpu_set_sr(cpu_g, SR_SUP);
    cpu_g->pc = CODE_NEXT;
    (void)mc68k_execute(cpu_g, 1);
}

static int bc_measure(uint16_t sr) {
    cpu_set_sr(cpu_g, sr);
    cpu_g->pc = TEST_PC;
    return mc68k_execute(cpu_g, 1);
}

static void bc_expect(const char *label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL branch cache %s: expected %d got %d\n", label, want, got);
        failures++;
    }
}

static void t_g1_branch_cache_clear(void) {
    enum { PREDICTED = 1, STATIC_BACKWARD_TAKEN = 3, WRONG = 7 };
    /* CUBC は user の entry だけを消す */
    bc_start(SR_SUP);
    bc_supervisor_op(MOVEC_CACR, CACR_REG, CACR_EBC | CACR_CUBC);
    bc_expect("CUBC keeps a supervisor entry", bc_measure(SR_SUP), PREDICTED);
    bc_start(SR_USER);
    bc_supervisor_op(MOVEC_CACR, CACR_REG, CACR_EBC | CACR_CUBC);
    bc_expect("CUBC clears a user entry", bc_measure(SR_USER), STATIC_BACKWARD_TAKEN);
    bc_start(SR_USER);
    bc_supervisor_op(MOVEC_CACR, CACR_REG, CACR_EBC | CACR_CABC);
    bc_expect("CABC clears a user entry", bc_measure(SR_USER), STATIC_BACKWARD_TAKEN);
    /* instruction cache を対象にする CINV は branch cache も消す */
    bc_start(SR_SUP);
    bc_supervisor_op(CINVA_IC, 0x4e71, 0);
    bc_expect("CINVA IC clears the branch cache", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    /* data cache だけを対象にする CINV は消さず、instruction cache を対象にする CPUSH は消す
     * （[060UM §5.2.1、§5.11]） */
    bc_start(SR_SUP);
    bc_supervisor_op(CINVA_DC, 0x4e71, 0);
    bc_expect("CINVA DC keeps the branch cache", bc_measure(SR_SUP), PREDICTED);
    bc_start(SR_SUP);
    bc_supervisor_op(CPUSHA_IC, 0x4e71, 0);
    bc_expect("CPUSHA IC clears the branch cache", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    /* CACR を読んでも消えない。CABC と CUBC を同時に書くと CABC が優先して全 entry を消す */
    bc_start(SR_SUP);
    bc_supervisor_op(MOVEC_CACR, CACR_REG, CACR_EBC | CACR_CABC);
    bc_expect("CABC write clears", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    bc_supervisor_op(MOVEC_FROM_CACR, CACR_REG, 0);
    bc_expect("CACR read keeps the entry", bc_measure(SR_SUP), PREDICTED);
    bc_start(SR_SUP);
    bc_supervisor_op(MOVEC_CACR, CACR_REG, CACR_EBC | CACR_CABC | CACR_CUBC);
    bc_expect("CABC takes precedence over CUBC", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    /* 予測が外れた not taken は entry を消す */
    bc_start(SR_SUP);
    bc_expect("wrong prediction", bc_measure(SR_SUP_Z), WRONG);
    bc_expect("entry removed by not taken", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    /* EBC=0 では branch cache の情報を使わず、登録もしない（[060UM §5.2.1]） */
    bc_start(SR_SUP);
    bc_supervisor_op(MOVEC_CACR, CACR_REG, 0);
    bc_expect("EBC=0 ignores a registered entry", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    bc_start_cacr(SR_SUP, 0);
    bc_expect("EBC=0 first run", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    bc_expect("EBC=0 second run", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    bc_supervisor_op(MOVEC_CACR, CACR_REG, CACR_EBC);
    bc_expect("EBC=0 registered nothing", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    bc_expect("registered once EBC=1", bc_measure(SR_SUP), PREDICTED);
    /* CACR 以外の MOVEC（VBR、TC）では消えない */
    bc_start(SR_SUP);
    bc_supervisor_op(MOVEC_CACR, CACR_REG, CACR_EBC | CACR_CABC);
    bc_expect("CABC clears, then re-registers", bc_measure(SR_SUP), STATIC_BACKWARD_TAKEN);
    bc_supervisor_op(MOVEC_CACR, VBR_REG, 0);
    bc_expect("MOVEC D0,VBR keeps the entry", bc_measure(SR_SUP), PREDICTED);
    bc_supervisor_op(MOVEC_CACR, TC_REG, 0);
    bc_expect("MOVEC D0,TC keeps the entry", bc_measure(SR_SUP), PREDICTED);
    /* CUBC=0 の CACR 書込みは user の entry を消さない */
    bc_start(SR_USER);
    bc_supervisor_op(MOVEC_CACR, CACR_REG, CACR_EBC);
    bc_expect("CACR write without CUBC keeps a user entry", bc_measure(SR_USER), PREDICTED);
}

/* Table 10-26（例外処理）。 */
static void t_g1_exception(void) {
    CHECK("TRAP #0", 19, SR_SUP, 3, 0x4e40);
    CHECK("ILLEGAL", 19, SR_SUP, 3, 0x4afc);
    CHECK("divide by zero DIVU.W Dn", 20, SR_SUP, 3, 0x82c2);
    CHECK("divide by zero DIVU.W (bd,An,Xi)", 21, SR_SUP, 3, 0x82f0, 0x1930, 0x0000, 0x0010);
    CHECK("MOVEP (vector 61)", 19, SR_SUP, 3, 0x0188, 0x0000);
    CHECK("line A", 19, SR_SUP, 3, 0xa000);
    CHECK("privilege violation", 19, 0x0700, 3, 0x4e72, 0x2700);
}

int main(void) {
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral};
    cpu_g = mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68060, MC68K_FPU_68060);
    assert(cpu_g);
    t_g1_integer();
    t_g1_system();
    t_g1_bitfield();
    t_g1_fpu();
    t_g1_branch();
    t_g1_branch_cache();
    t_g1_branch_cache_clear();
    t_g1_exception();
    if (failures) {
        fprintf(stderr, "test_68060_timing: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_68060_timing: OK");
    return 0;
}
