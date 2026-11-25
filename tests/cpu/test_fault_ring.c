/* group0 フォールトリング（fault ring）の記録内容（ Phase B、
 * 現行仕様 の B1〜B21）。
 * PMMU レジスタの設定と、VECTORED の後の PC/SP の戻しには内部ヘッダを使うが、
 * 検証は公開 API（mc68k_read_fault_ring() /
 * mc68k_read_pc_ring()）とスタック上のフレームで行う。 */
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RAM_SIZE         0x10000u
#define RESET_SSP        0x00008000u
#define PROG             0x00002000u
#define BUS_HANDLER      0x00003000u /* ベクタ2 */
#define ADDR_HANDLER     0x00003100u /* ベクタ3 */
#define AUTOVEC1_HANDLER 0x00003200u /* ベクタ25（レベル1 オートベクタ） */
#define OUT_OF_RAM       0x00020000u
#define ODD_ADDR         0x00004001u
#define NO_FAIL          UINT32_MAX
#define MAX_STEPS        64

#define OP_MOVEA_L_IMM_A0 0x207Cu
#define OP_MOVEA_L_IMM_A7 0x2E7Cu
#define OP_MOVE_L_IMM_D0  0x203Cu
#define OP_MOVE_W_A0_D0   0x3010u
#define OP_MOVE_W_D0_A0   0x3080u
#define OP_MOVE_W_IMM_D0  0x303Cu
#define OP_MOVE_L_A0_D0   0x2010u
#define OP_TAS_A0         0x4AD0u
#define OP_BFTST_A0       0xE8D0u
#define EXT_BF_0_16       0x0010u /* {#0:#16} */
#define OP_JMP_ABS_L      0x4EF9u
#define OP_MOVEC_TO_CTRL  0x4E7Bu
#define EXT_MOVEC_D0_MSP  0x0803u
#define OP_MOVE_IMM_SR    0x46FCu
#define OP_NOP            0x4E71u
#define OP_RTE            0x4E73u
#define OP_BRA_SELF       0x60FEu

/* PMMU の構成（test_68030_fault.c の main() 冒頭と map() と同じ）。
 * TC: E=1、SRE=1、PS=4KB、IS=0、TIA=TIB=10。ルートは長形式 DT=3 で A 表は 0x400。
 * A[0]=0x400 は低位 4MiB の恒等写像、A[1]=0x408 は短形式の B 表 0x800 を指す。 */
#define PMMU_TC       0x82c0aa00u
#define PMMU_ROOT     UINT64_C(0x8000000300000400)
#define A1_ATTRIBUTES 0x0000000au
#define FAULT_VA      0x00403010u
#define UNMAPPED_SSP  0x00406000u
/* FAULT_VA の手計算:
 * - A の index = 0x00403010 >> 22 = 1 → 記述子 0x400 + 1*8 = 0x408（level=1）
 * - B の index = (0x00403010 >> 12) & 0x3FF = 3 → 記述子 0x800 + 3*4 = 0x80C（level=2）
 * B16 のフレーム先頭 0x406000 - 92 = 0x405FA4 の B の index は 5 → 0x814（未設定 = 0 → 無効）。
 */
#define DESC_A1       0x00000408u
#define DESC_B3       0x0000080Cu

/*
 * B2 の SSW の手計算値（各モデル1ケース。§4.9.2）。
 * - 68000: [PM Appendix B Figure B-2]。R/W(bit4)=1（読出し）、
 *   I/N(bit3)=0（命令処理中）、FC=5 → 0x0015。
 * - 68010: [DS10 Figure 5-10]。DF(bit12)=1、RW(bit8)=1、
 *   BY(bit9)=0（ワード）、FC=5 → 0x1105。
 * - 68020/68030: [UM20 Figure 6-8]。DF(bit8)=1、RW(bit6)=1、
 *   SIZE(bit5-4)=10（ワード）、FC=5 → 0x0165。
 */
#define B2_SSW_68000 0x0015u
#define B2_SSW_68010 0x1105u
#define B2_SSW_68020 0x0165u

typedef struct {
    uint8_t m[RAM_SIZE];
    uint32_t fail_read_at, fail_write_at;
} ctx_t;

static ctx_t g;

/* 範囲外（RAM_SIZE 以上にかかる）アクセスと fail_*_at に一致するアクセスは
 * false（ホストバスエラー）。 判定は各コールバックの入口で1回だけ行う。 */
static bool in_ram(uint32_t a, uint32_t n) {
    return a < RAM_SIZE && RAM_SIZE - a >= n;
}
static bool rb(void *p, uint32_t a, uint8_t *v) {
    ctx_t *c = p;
    if (a == c->fail_read_at || !in_ram(a, 1))
        return false;
    *v = c->m[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    if (a == c->fail_read_at || !in_ram(a, 2))
        return false;
    *v = (uint16_t)((c->m[a] << 8) | c->m[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    if (a == c->fail_read_at || !in_ram(a, 4))
        return false;
    *v = ((uint32_t)c->m[a] << 24) | ((uint32_t)c->m[a + 1] << 16) |
         ((uint32_t)c->m[a + 2] << 8) | c->m[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    if (a == c->fail_write_at || !in_ram(a, 1))
        return false;
    c->m[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    if (a == c->fail_write_at || !in_ram(a, 2))
        return false;
    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    if (a == c->fail_write_at || !in_ram(a, 4))
        return false;
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
    return true;
}

static void pw(uint32_t a, uint16_t v) {
    g.m[a] = (uint8_t)(v >> 8);
    g.m[a + 1] = (uint8_t)v;
}
static void pl(uint32_t a, uint32_t v) {
    pw(a, (uint16_t)(v >> 16));
    pw(a + 2, (uint16_t)v);
}
static uint16_t gw(uint32_t a) {
    return (uint16_t)((g.m[a] << 8) | g.m[a + 1]);
}
static uint32_t emit_w(uint32_t p, uint16_t v) {
    pw(p, v);
    return p + 2;
}
static uint32_t emit_l(uint32_t p, uint32_t v) {
    pl(p, v);
    return p + 4;
}

static const mc68k_bus_t bus = {
    .read_byte = rb,
    .read_word = rw,
    .read_long = rl,
    .write_byte = wb,
    .write_word = ww,
    .write_long = wl,
    .ctx = &g,
};

/* ADDR ERR / GROUP0-REFAULT 等の診断ログを捨てる（B20 は自前のハンドラに差し替える）。 */
static void discard_log(void *ctx, const char *msg) {
    (void)ctx;
    (void)msg;
}

/* RAM を初期化してベクタとハンドラ（BRA *）を置き、CPU を作って reset する。
 * fault ring と pc ring は on で指定する。 */
static mc68k_state_t *boot(mc68k_model_t model, int rings_on) {
    memset(&g, 0, sizeof(g));
    g.fail_read_at = g.fail_write_at = NO_FAIL;
    pl(0, RESET_SSP);
    pl(4, PROG);
    pl(2 * 4, BUS_HANDLER);
    pl(3 * 4, ADDR_HANDLER);
    pl(25 * 4, AUTOVEC1_HANDLER);
    pw(BUS_HANDLER, OP_BRA_SELF);
    pw(ADDR_HANDLER, OP_BRA_SELF);
    pw(AUTOVEC1_HANDLER, OP_BRA_SELF);
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    assert(cpu);
    mc68k_set_log_handler(cpu, discard_log, NULL);
    mc68k_reset(cpu);
    if (rings_on) {
        mc68k_set_fault_ring(cpu, 1);
        mc68k_set_pc_ring(cpu, 1);
    }
    return cpu;
}

/* ハンドラ先頭に着く（dispatch 直後）か HALT するまで1反復ずつ進める。 */
static void run_to_fault(mc68k_state_t *cpu) {
    for (int i = 0; i < MAX_STEPS; i++) {
        mc68k_execute(cpu, 1);
        uint32_t pc = mc68k_get_pc(cpu);
        if (pc == BUS_HANDLER || pc == ADDR_HANDLER || mc68k_get_exec_state(cpu) == CPU_HALTED)
            return;
    }
    assert(!"fault not reached");
}

/* VECTORED の後に、次の命令列から続ける（§4.9.1）。 */
static void resume_at(mc68k_state_t *cpu, uint32_t pc) {
    assert(mc68k_get_exec_state(cpu) == CPU_NORMAL);
    cpu->pc = pc;
    cpu->a[7] = RESET_SSP;
}

static mc68k_fault_ring_entry_t ring[MC68K_FAULT_RING_SIZE];

static size_t read_ring(mc68k_state_t *cpu) {
    size_t n = mc68k_read_fault_ring(cpu, ring, MC68K_FAULT_RING_SIZE);
    assert(n == mc68k_read_fault_ring(cpu, NULL, 0));
    return n;
}

/* 共通期待値表（§4.9.3）の1行。 */
typedef struct {
    uint8_t vector, access_kind, fc, size, read, rm, opcode_valid;
    uint16_t opcode;
    uint32_t instr_pc, fault_addr;
    uint8_t frame_format, outcome, origin;
} expect_t;

static void check_entry(const mc68k_fault_ring_entry_t *e, const expect_t *x) {
    assert(e->vector == x->vector);
    assert(e->access_kind == x->access_kind);
    assert(e->fc == x->fc);
    assert(e->size == x->size);
    assert(e->read == x->read);
    assert(e->rm == x->rm);
    assert(e->opcode_valid == x->opcode_valid);
    if (x->opcode_valid)
        assert(e->opcode == x->opcode);
    assert(e->instr_pc == x->instr_pc);
    assert(e->fault_addr == x->fault_addr);
    assert(e->frame_format == x->frame_format);
    assert(e->outcome == x->outcome);
    assert(e->origin == x->origin);
}

/* §4.9.2 の 0 / NONE の規則（PMMU 系フィールド。regs_zero なら tc/crp/srp も 0）。 */
static void check_no_mmu(const mc68k_fault_ring_entry_t *e, int regs_zero) {
    assert(e->mmu_valid == 0);
    assert(e->mmu_source == MC68K_FAULT_MMU_SOURCE_NONE);
    assert(e->mmu_fault == MC68K_DEBUG_MMU_NONE);
    assert(e->walk_valid == 0);
    assert(e->level == 0);
    assert(e->descriptor_physical == 0);
    assert(e->table_bus_error_physical == 0);
    if (regs_zero) {
        assert(e->tc == 0);
        assert(e->crp == 0);
        assert(e->srp == 0);
    }
}

static void check_pmmu_regs(const mc68k_fault_ring_entry_t *e) {
    assert(e->tc == PMMU_TC);
    assert(e->crp == PMMU_ROOT);
    assert(e->srp == PMMU_ROOT);
}

/* 直近の pc ring エントリがこのフォールトの試行であること（seq と pc の一致）。 */
static void check_seq_is_last_attempt(mc68k_state_t *cpu, const mc68k_fault_ring_entry_t *e) {
    mc68k_pc_ring_entry_t pcs[MC68K_PC_RING_SIZE];
    size_t n = mc68k_read_pc_ring(cpu, pcs, MC68K_PC_RING_SIZE);
    assert(n >= 1);
    assert(pcs[n - 1].pc == e->instr_pc);
    assert(pcs[n - 1].seq == e->seq);
}

/* VECTORED のエントリの ssw / frame_format を、スタックに積まれたフレームと突き合わせる。
 * ハンドラは未実行なので A7 がフレームの先頭。スタック上の SSW を返す。 */
static uint16_t check_stack_frame(mc68k_state_t *cpu, mc68k_model_t model,
                                  const mc68k_fault_ring_entry_t *e) {
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    uint32_t sp = r.a[7];
    uint16_t ssw;
    if (model == MC68K_MODEL_68000) {
        assert(e->frame_format == 0); /* フォーマットワードなし */
        ssw = gw(sp);
    } else {
        uint16_t fmt = gw(sp + 6);
        assert((fmt >> 12) == e->frame_format);
        assert((fmt & 0x0FFFu) == (uint16_t)(e->vector * 4u));
        ssw = gw(model == MC68K_MODEL_68010 ? sp + 8 : sp + 0x0A);
    }
    assert(e->ssw == ssw);
    /* SSW の FC 欄（bit 2-0）は fault ring の fc と一致するはず。
     * ssw が e->ssw のコピーであるだけの自己一致に留まらないよう、実際に
     * cpu->fault_access.fc から独立して記録された e->fc と突き合わせる。 */
    assert((ssw & 0x0007u) == e->fc);
    return ssw;
}

static const mc68k_model_t models[4] = {MC68K_MODEL_68000, MC68K_MODEL_68010, MC68K_MODEL_68020,
                                        MC68K_MODEL_68030};
/* B7〜B9 で B2 と突き合わせるため、B2 のスタック上の SSW をモデル別に取っておく。 */
static uint16_t b2_stack_ssw[4];

static int has_alignment(mc68k_model_t model) {
    return model == MC68K_MODEL_68000 || model == MC68K_MODEL_68010;
}

/* B2 のプログラム。68000/68010 は奇数アドレスのアドレスエラー、68020+ は RAM 外のバスエラー。
 */
static void put_b2_program(mc68k_model_t model) {
    uint32_t p = emit_w(PROG, OP_MOVEA_L_IMM_A0);
    p = emit_l(p, has_alignment(model) ? ODD_ADDR : OUT_OF_RAM);
    emit_w(p, OP_MOVE_W_A0_D0);
}

static expect_t b2_expect(mc68k_model_t model, int mi) {
    static const uint8_t fmt[4] = {0, 0x8, 0xB, 0xB};
    return (expect_t){.vector = has_alignment(model) ? 3 : 2,
                      .access_kind = MC68K_FAULT_ACCESS_DATA_READ,
                      .fc = 5,
                      .size = 2,
                      .read = 1,
                      .rm = 0,
                      .opcode_valid = 1,
                      .opcode = OP_MOVE_W_A0_D0,
                      .instr_pc = PROG + 6,
                      .fault_addr = has_alignment(model) ? ODD_ADDR : OUT_OF_RAM,
                      .frame_format = fmt[mi],
                      .outcome = MC68K_FAULT_OUTCOME_VECTORED,
                      .origin = MC68K_FAULT_ORIGIN_INSTRUCTION};
}

/* B1: 既定オフ。オフ中のフォールトは載らず、ON にした後の次のフォールトから載る。 */
static void test_b1_default_off(void) {
    mc68k_state_t *cpu = boot(MC68K_MODEL_68000, 0);
    assert(mc68k_get_fault_ring(cpu) == 0);
    uint32_t p = emit_w(PROG, OP_MOVEA_L_IMM_A0);
    p = emit_l(p, OUT_OF_RAM);
    emit_w(p, OP_MOVE_W_D0_A0);
    run_to_fault(cpu);
    assert(mc68k_get_pc(cpu) == BUS_HANDLER);
    assert(mc68k_read_fault_ring(cpu, NULL, 0) == 0);
    mc68k_set_fault_ring(cpu, 1);
    assert(mc68k_get_fault_ring(cpu) == 1);
    resume_at(cpu, PROG + 6);
    run_to_fault(cpu);
    assert(read_ring(cpu) == 1);
    assert(ring[0].instr_pc == PROG + 6);
    assert(ring[0].vector == 2);
    mc68k_destroy(cpu);
}

/* B2: データ読出しのフォールト（4モデル）。 */
static void test_b2_data_read(void) {
    int ran = 0;
    for (int mi = 0; mi < 4; mi++) {
        mc68k_model_t model = models[mi];
        mc68k_state_t *cpu = boot(model, 1);
        put_b2_program(model);
        run_to_fault(cpu);
        assert(read_ring(cpu) == 1);
        expect_t x = b2_expect(model, mi);
        check_entry(&ring[0], &x);
        check_no_mmu(&ring[0], 1);
        check_seq_is_last_attempt(cpu, &ring[0]);
        b2_stack_ssw[mi] = check_stack_frame(cpu, model, &ring[0]);
        /* 手計算値（上の B2_SSW_* の根拠）。 */
        static const uint16_t hand[4] = {B2_SSW_68000, B2_SSW_68010, B2_SSW_68020,
                                         B2_SSW_68020};
        assert(ring[0].ssw == hand[mi]);
        mc68k_destroy(cpu);
        ran++;
    }
    assert(ran == 4);
}

/* B3: データ書込みのバスエラー（4モデル）。 */
static void test_b3_data_write(void) {
    static const uint8_t fmt[4] = {0, 0x8, 0xB, 0xB};
    int ran = 0;
    for (int mi = 0; mi < 4; mi++) {
        mc68k_model_t model = models[mi];
        mc68k_state_t *cpu = boot(model, 1);
        uint32_t p = emit_w(PROG, OP_MOVEA_L_IMM_A0);
        p = emit_l(p, OUT_OF_RAM);
        emit_w(p, OP_MOVE_W_D0_A0);
        run_to_fault(cpu);
        assert(read_ring(cpu) == 1);
        expect_t x = {.vector = 2,
                      .access_kind = MC68K_FAULT_ACCESS_DATA_WRITE,
                      .fc = 5,
                      .size = 2,
                      .read = 0,
                      .rm = 0,
                      .opcode_valid = 1,
                      .opcode = OP_MOVE_W_D0_A0,
                      .instr_pc = PROG + 6,
                      .fault_addr = OUT_OF_RAM,
                      .frame_format = fmt[mi],
                      .outcome = MC68K_FAULT_OUTCOME_VECTORED,
                      .origin = MC68K_FAULT_ORIGIN_INSTRUCTION};
        check_entry(&ring[0], &x);
        check_no_mmu(&ring[0], 1);
        check_seq_is_last_attempt(cpu, &ring[0]);
        check_stack_frame(cpu, model, &ring[0]);
        mc68k_destroy(cpu);
        ran++;
    }
    assert(ran == 4);
}

/* B4: opcode 自体の fetch のバスエラー（4モデル）。 */
static void test_b4_opcode_fetch(void) {
    static const uint8_t fmt[4] = {0, 0x8, 0xA, 0xA};
    int ran = 0;
    for (int mi = 0; mi < 4; mi++) {
        mc68k_model_t model = models[mi];
        mc68k_state_t *cpu = boot(model, 1);
        emit_l(emit_w(PROG, OP_JMP_ABS_L), OUT_OF_RAM);
        run_to_fault(cpu);
        assert(read_ring(cpu) == 1);
        expect_t x = {.vector = 2,
                      .access_kind = MC68K_FAULT_ACCESS_OPCODE,
                      .fc = 6,
                      .size = 2,
                      .read = 1,
                      .rm = 0,
                      .opcode_valid = 0,
                      .instr_pc = OUT_OF_RAM,
                      .fault_addr = OUT_OF_RAM,
                      .frame_format = fmt[mi],
                      .outcome = MC68K_FAULT_OUTCOME_VECTORED,
                      .origin = MC68K_FAULT_ORIGIN_INSTRUCTION};
        if (model == MC68K_MODEL_68000) {
            /* 68000 は JMP の完了時に分岐先を先読みするため、フォルトは JMP の中で起きる。 */
            x.instr_pc =
                PROG; /* 次命令の opcode 取得の失敗として記録される（opcode_valid=0） */
        }
        check_entry(&ring[0], &x);
        check_no_mmu(&ring[0], 1);
        check_seq_is_last_attempt(cpu, &ring[0]);
        check_stack_frame(cpu, model, &ring[0]);
        mc68k_destroy(cpu);
        ran++;
    }
    assert(ran == 4);
}

/* B5: 拡張語の fetch のバスエラー（4モデル）。0xFFFE の MOVE.W #imm,D0 の即値が RAM 外。 */
static void test_b5_extension_fetch(void) {
    static const uint8_t fmt[4] = {0, 0x8, 0xB, 0xB};
    int ran = 0;
    for (int mi = 0; mi < 4; mi++) {
        mc68k_model_t model = models[mi];
        mc68k_state_t *cpu = boot(model, 1);
        /* 68000 は先読みが2語先行するため、即値の1語先（$10000）の補充が MOVE.W #imm,D0 の中で
         * 失敗するよう、命令を $FFFC に置く（$FFFE の即値は取得できる）。 */
        const uint32_t insn_pc = (model == MC68K_MODEL_68000) ? 0x0000FFFCu : 0x0000FFFEu;
        emit_l(emit_w(PROG, OP_JMP_ABS_L), insn_pc);
        pw((uint16_t)insn_pc, OP_MOVE_W_IMM_D0);
        run_to_fault(cpu);
        assert(read_ring(cpu) == 1);
        expect_t x = {.vector = 2,
                      .access_kind = MC68K_FAULT_ACCESS_EXTENSION,
                      .fc = 6,
                      .size = 2,
                      .read = 1,
                      .rm = 0,
                      .opcode_valid = 1,
                      .opcode = OP_MOVE_W_IMM_D0,
                      .instr_pc = insn_pc,
                      .fault_addr = 0x00010000u,
                      .frame_format = fmt[mi],
                      .outcome = MC68K_FAULT_OUTCOME_VECTORED,
                      .origin = MC68K_FAULT_ORIGIN_INSTRUCTION};
        check_entry(&ring[0], &x);
        check_no_mmu(&ring[0], 1);
        check_seq_is_last_attempt(cpu, &ring[0]);
        check_stack_frame(cpu, model, &ring[0]);
        mc68k_destroy(cpu);
        ran++;
    }
    assert(ran == 4);
}

/* B6: TAS のロックされた RMW の読出しサイクル（68020）。 */
static void test_b6_tas_rm(void) {
    mc68k_state_t *cpu = boot(MC68K_MODEL_68020, 1);
    uint32_t p = emit_w(PROG, OP_MOVEA_L_IMM_A0);
    p = emit_l(p, OUT_OF_RAM);
    emit_w(p, OP_TAS_A0);
    run_to_fault(cpu);
    assert(read_ring(cpu) == 1);
    expect_t x = {.vector = 2,
                  .access_kind = MC68K_FAULT_ACCESS_DATA_READ,
                  .fc = 5,
                  .size = 1,
                  .read = 1,
                  .rm = 1,
                  .opcode_valid = 1,
                  .opcode = OP_TAS_A0,
                  .instr_pc = PROG + 6,
                  .fault_addr = OUT_OF_RAM,
                  .frame_format = 0xB,
                  .outcome = MC68K_FAULT_OUTCOME_VECTORED,
                  .origin = MC68K_FAULT_ORIGIN_INSTRUCTION};
    check_entry(&ring[0], &x);
    check_no_mmu(&ring[0], 1);
    check_seq_is_last_attempt(cpu, &ring[0]);
    uint16_t ssw = check_stack_frame(cpu, MC68K_MODEL_68020, &ring[0]);
    assert(ssw & 0x0080u); /* RM [UM20 Figure 6-8] */
    mc68k_destroy(cpu);
}

/* B7/B8: 68010 のフレーム書込みの失敗。B2 の 68010 と同じプログラム。 */
static void test_b7_b8_frame_write_fault(void) {
    static const uint32_t fail_at[2] = {
        RESET_SSP - 58,        /* B7: フレーム先頭（最初の書込み = SR、SSW より前） */
        RESET_SSP - 58 + 0x0A, /* B8: フォールトアドレス欄の long 書込み（SSW より後） */
    };
    int ran = 0;
    for (int k = 0; k < 2; k++) {
        mc68k_state_t *cpu = boot(MC68K_MODEL_68010, 1);
        put_b2_program(MC68K_MODEL_68010);
        g.fail_write_at = fail_at[k];
        run_to_fault(cpu);
        assert(mc68k_get_exec_state(cpu) == CPU_HALTED);
        assert(read_ring(cpu) == 1);
        expect_t x = b2_expect(MC68K_MODEL_68010, 1);
        x.outcome = MC68K_FAULT_OUTCOME_FRAME_WRITE_FAULT;
        check_entry(&ring[0], &x);
        check_no_mmu(&ring[0], 1);
        check_seq_is_last_attempt(cpu, &ring[0]);
        assert(ring[0].ssw == b2_stack_ssw[1]);
        mc68k_destroy(cpu);
        ran++;
    }
    assert(ran == 2);
}

/* B9: ベクタ読出しの失敗（68000 / 68010 / 68020）。 */
static void test_b9_vector_read_fault(void) {
    int ran = 0;
    for (int mi = 0; mi < 3; mi++) {
        mc68k_model_t model = models[mi];
        mc68k_state_t *cpu = boot(model, 1);
        put_b2_program(model);
        g.fail_read_at = has_alignment(model) ? 3 * 4 : 2 * 4;
        run_to_fault(cpu);
        assert(mc68k_get_exec_state(cpu) == CPU_HALTED);
        assert(read_ring(cpu) == 1);
        expect_t x = b2_expect(model, mi);
        x.outcome = MC68K_FAULT_OUTCOME_VECTOR_READ_FAULT;
        check_entry(&ring[0], &x);
        check_no_mmu(&ring[0], 1);
        check_seq_is_last_attempt(cpu, &ring[0]);
        assert(ring[0].ssw == b2_stack_ssw[mi]);
        mc68k_destroy(cpu);
        ran++;
    }
    assert(ran == 3);
}

/* B10: 68010 の再フォルト安全弁（ハンドラが RTE、test_68010.c の
 * test_group0_refault_safety_valve_halts() と同じ構成）。 */
static void test_b10_refault_safety_valve(void) {
    mc68k_state_t *cpu = boot(MC68K_MODEL_68010, 1);
    put_b2_program(MC68K_MODEL_68010);
    pw(ADDR_HANDLER, OP_RTE);
    run_to_fault(cpu); /* 1回目: ハンドラ先頭（RTE の実行前）で止まる */
    assert(mc68k_get_pc(cpu) == ADDR_HANDLER);
    assert(read_ring(cpu) == 1);
    check_stack_frame(cpu, MC68K_MODEL_68010, &ring[0]);
    for (int i = 0; i < MAX_STEPS && mc68k_get_exec_state(cpu) != CPU_HALTED; i++)
        mc68k_execute(cpu, 1);
    assert(mc68k_get_exec_state(cpu) == CPU_HALTED);
    assert(read_ring(cpu) == 2);
    assert(ring[0].outcome == MC68K_FAULT_OUTCOME_VECTORED);
    assert(ring[1].outcome == MC68K_FAULT_OUTCOME_NOT_BUILT);
    assert(ring[1].ssw == ring[0].ssw);
    assert(ring[1].fault_addr == ring[0].fault_addr);
    assert(ring[1].instr_pc == ring[0].instr_pc);
    assert(ring[1].seq > ring[0].seq);
    mc68k_destroy(cpu);
}

/* ---- PMMU（68030） ---- */

static void map(uint32_t a1_attributes, uint32_t leaf) {
    pl(0x400, 0x00000009);
    pl(0x404, 0x00000000); /* A[0]: identity page */
    pl(0x408, a1_attributes);
    pl(0x40c, 0x00000800); /* A[1]: short B table */
    pl(0x80c, leaf);
}

static void enable_pmmu(mc68k_state_t *cpu) {
    cpu->mmu_tc = PMMU_TC;
    cpu->mmu_crp = cpu->mmu_srp = PMMU_ROOT;
}

/* B11 の共通表の行（instr_pc は呼び出し側で変える）。 */
static expect_t b11_expect(uint32_t instr_pc) {
    return (expect_t){.vector = 2,
                      .access_kind = MC68K_FAULT_ACCESS_DATA_READ,
                      .fc = 5,
                      .size = 4,
                      .read = 1,
                      .rm = 0,
                      .opcode_valid = 1,
                      .opcode = OP_MOVE_L_A0_D0,
                      .instr_pc = instr_pc,
                      .fault_addr = FAULT_VA,
                      .frame_format = 0xB,
                      .outcome = MC68K_FAULT_OUTCOME_VECTORED,
                      .origin = MC68K_FAULT_ORIGIN_INSTRUCTION};
}

/* 68030 で PMMU を有効にし、0x2000 に MOVEA.L #va,A0; MOVE.L (A0),D0 を置く。 */
static mc68k_state_t *boot_pmmu(uint32_t va) {
    mc68k_state_t *cpu = boot(MC68K_MODEL_68030, 1);
    map(A1_ATTRIBUTES, 0); /* 葉 0x80C の DT=0 → 無効 */
    enable_pmmu(cpu);
    uint32_t p = emit_w(PROG, OP_MOVEA_L_IMM_A0);
    p = emit_l(p, va);
    emit_w(p, OP_MOVE_L_A0_D0);
    return cpu;
}

static void check_b11_entry(const mc68k_fault_ring_entry_t *e) {
    expect_t x = b11_expect(PROG + 6);
    check_entry(e, &x);
    assert(e->mmu_valid == 1);
    assert(e->mmu_source == MC68K_FAULT_MMU_SOURCE_WALK);
    assert(e->walk_valid == 1);
    assert(e->mmu_fault == MC68K_DEBUG_MMU_INVALID);
    assert(e->level == 2);
    assert(e->descriptor_physical == DESC_B3);
    assert(e->table_bus_error_physical == 0);
    check_pmmu_regs(e);
}

static void check_b12_entry(const mc68k_fault_ring_entry_t *e) {
    expect_t x = b11_expect(PROG + 6);
    check_entry(e, &x);
    assert(e->mmu_valid == 1);
    assert(e->mmu_source == MC68K_FAULT_MMU_SOURCE_ATC);
    assert(e->walk_valid == 0);
    assert(e->mmu_fault == MC68K_DEBUG_MMU_INVALID);
    assert(e->level == 0);
    assert(e->descriptor_physical == 0);
    assert(e->table_bus_error_physical == 0);
    check_pmmu_regs(e);
}

/* B11: ウォークで無効な葉に当たる。B12: 同じインスタンスで ATC の負エントリにヒットする。 */
static void test_b11_b12_walk_then_atc(void) {
    mc68k_state_t *cpu = boot_pmmu(FAULT_VA);
    run_to_fault(cpu);
    assert(read_ring(cpu) == 1);
    check_b11_entry(&ring[0]);
    check_seq_is_last_attempt(cpu, &ring[0]);
    check_stack_frame(cpu, MC68K_MODEL_68030, &ring[0]);

    resume_at(cpu, PROG + 6); /* ATC は無効化しない */
    run_to_fault(cpu);
    assert(read_ring(cpu) == 2);
    check_b11_entry(&ring[0]);
    check_b12_entry(&ring[1]);
    check_seq_is_last_attempt(cpu, &ring[1]);
    check_stack_frame(cpu, MC68K_MODEL_68030, &ring[1]);
    mc68k_destroy(cpu);
}

/* B13: 第2段の記述子の読出しがバスエラー。 */
static void test_b13_table_bus_error(void) {
    mc68k_state_t *cpu = boot_pmmu(FAULT_VA);
    g.fail_read_at = DESC_B3;
    run_to_fault(cpu);
    assert(read_ring(cpu) == 1);
    const mc68k_fault_ring_entry_t *e = &ring[0];
    expect_t x = b11_expect(PROG + 6);
    check_entry(e, &x);
    assert(e->mmu_valid == 1);
    assert(e->mmu_source == MC68K_FAULT_MMU_SOURCE_WALK);
    assert(e->walk_valid == 1);
    assert(e->mmu_fault == MC68K_DEBUG_MMU_TABLE_BUS_ERROR);
    assert(e->level == 1);
    assert(e->descriptor_physical == DESC_A1);
    assert(e->table_bus_error_physical == DESC_B3);
    check_pmmu_regs(e);
    check_seq_is_last_attempt(cpu, e);
    check_stack_frame(cpu, MC68K_MODEL_68030, e);
    mc68k_destroy(cpu);
}

static void check_b14_entry(const mc68k_fault_ring_entry_t *e, uint32_t instr_pc) {
    expect_t x = b11_expect(instr_pc);
    x.fault_addr = OUT_OF_RAM;
    check_entry(e, &x);
    check_no_mmu(e, 0);
    check_pmmu_regs(e); /* レジスタ値は記録される */
}

/* B14: PMMU 有効のまま、恒等写像ページ内の RAM 外でホストバスエラー。 */
static void test_b14_host_bus_error_with_pmmu(void) {
    mc68k_state_t *cpu = boot_pmmu(OUT_OF_RAM);
    run_to_fault(cpu);
    assert(read_ring(cpu) == 1);
    check_b14_entry(&ring[0], PROG + 6);
    check_seq_is_last_attempt(cpu, &ring[0]);
    check_stack_frame(cpu, MC68K_MODEL_68030, &ring[0]);
    mc68k_destroy(cpu);
}

/* B15: PMMU fault → ホストバス fault → 奇数 opcode fetch
 * のアドレスエラーを1つのインスタンスで。 PMMU のラッチが dispatch で解除されること。 */
static void test_b15_latch_cleared_between_faults(void) {
    mc68k_state_t *cpu = boot_pmmu(FAULT_VA);
    const uint32_t second = PROG + 8, third = PROG + 16;
    uint32_t p = emit_w(second, OP_MOVEA_L_IMM_A0);
    p = emit_l(p, OUT_OF_RAM);
    p = emit_w(p, OP_MOVE_L_A0_D0);
    assert(p == third);
    emit_l(emit_w(third, OP_JMP_ABS_L), PROG + 1);

    run_to_fault(cpu);
    assert(read_ring(cpu) == 1);
    check_stack_frame(cpu, MC68K_MODEL_68030, &ring[0]);
    resume_at(cpu, second);
    run_to_fault(cpu);
    assert(read_ring(cpu) == 2);
    check_stack_frame(cpu, MC68K_MODEL_68030, &ring[1]);
    resume_at(cpu, third);
    run_to_fault(cpu);
    assert(read_ring(cpu) == 3);
    check_b11_entry(&ring[0]);
    check_b14_entry(&ring[1], second + 6);
    expect_t x = {.vector = 3,
                  .access_kind = MC68K_FAULT_ACCESS_OPCODE,
                  .fc = 6,
                  .size = 2,
                  .read = 1,
                  .rm = 0,
                  .opcode_valid = 0,
                  .instr_pc = PROG + 1,
                  .fault_addr = PROG + 1,
                  .frame_format = 0xA,
                  .outcome = MC68K_FAULT_OUTCOME_VECTORED,
                  .origin = MC68K_FAULT_ORIGIN_INSTRUCTION};
    check_entry(&ring[2], &x);
    check_no_mmu(&ring[2], 0);
    check_seq_is_last_attempt(cpu, &ring[2]);
    check_stack_frame(cpu, MC68K_MODEL_68030, &ring[2]);
    assert(ring[0].seq < ring[1].seq && ring[1].seq < ring[2].seq);
    mc68k_destroy(cpu);
}

/* B16: SSP を未マップの VA にして、フレーム書込みでも PMMU fault を起こす。記録されるのは
 * 最初の fault（0x80C）の変換結果で、フレーム書込みの失敗（0x814）で上書きされない。 */
static void test_b16_first_fault_latched(void) {
    mc68k_state_t *cpu = boot(MC68K_MODEL_68030, 1);
    map(A1_ATTRIBUTES, 0);
    enable_pmmu(cpu);
    uint32_t p = emit_w(PROG, OP_MOVEA_L_IMM_A7);
    p = emit_l(p, UNMAPPED_SSP);
    p = emit_w(p, OP_MOVEA_L_IMM_A0);
    p = emit_l(p, FAULT_VA);
    emit_w(p, OP_MOVE_L_A0_D0);
    run_to_fault(cpu);
    assert(mc68k_get_exec_state(cpu) == CPU_HALTED);
    assert(read_ring(cpu) == 1);
    const mc68k_fault_ring_entry_t *e = &ring[0];
    expect_t x = b11_expect(PROG + 12);
    x.outcome = MC68K_FAULT_OUTCOME_FRAME_WRITE_FAULT;
    check_entry(e, &x);
    assert(e->mmu_valid == 1);
    assert(e->mmu_source == MC68K_FAULT_MMU_SOURCE_WALK);
    assert(e->walk_valid == 1);
    assert(e->mmu_fault == MC68K_DEBUG_MMU_INVALID);
    assert(e->level == 2);
    assert(e->descriptor_physical == DESC_B3);
    assert(e->table_bus_error_physical == 0);
    check_pmmu_regs(e);
    check_seq_is_last_attempt(cpu, e);
    mc68k_destroy(cpu);
}

/* B21: 最初の PMMU fault の後も、同じ反復の中でバスサイクルが続く命令。BFTST のメモリ形式は
 * フィールドの各ビットごとにバイトを読み、途中で exec_state を見ない。2回目以降の読出しは
 * 同じページの負 ATC エントリにヒットして失敗する（読出しなので無効化されない）。記録されるのは
 * 最初の fault（ウォーク、0x80C）で、後続の ATC 失敗で置き換えられないこと。
 * （フレーム書込み中の変換失敗は、dispatch がフレームを書く前に記録を確定させるので B16 では
 * 検出できない。cpu_translate_execute_access() のラッチ条件はこのテストで検出する。） */
static void test_b21_same_iteration_refault_keeps_first(void) {
    mc68k_state_t *cpu = boot(MC68K_MODEL_68030, 1);
    map(A1_ATTRIBUTES, 0);
    enable_pmmu(cpu);
    uint32_t p = emit_w(PROG, OP_MOVEA_L_IMM_A0);
    p = emit_l(p, FAULT_VA);
    p = emit_w(p, OP_BFTST_A0);
    emit_w(p, EXT_BF_0_16);
    run_to_fault(cpu);
    assert(read_ring(cpu) == 1);
    const mc68k_fault_ring_entry_t *e = &ring[0];
    expect_t x = b11_expect(PROG + 6);
    x.size = 1;
    x.opcode = OP_BFTST_A0;
    check_entry(e, &x);
    assert(e->mmu_valid == 1);
    assert(e->mmu_source == MC68K_FAULT_MMU_SOURCE_WALK);
    assert(e->walk_valid == 1);
    assert(e->mmu_fault == MC68K_DEBUG_MMU_INVALID);
    assert(e->level == 2);
    assert(e->descriptor_physical == DESC_B3);
    assert(e->table_bus_error_physical == 0);
    check_pmmu_regs(e);
    check_seq_is_last_attempt(cpu, e);
    check_stack_frame(cpu, MC68K_MODEL_68030, e);
    mc68k_destroy(cpu);
}

/* B17: 68020 の M=1 からの割込み受付で、ISP 上の $1 throwaway フレームの書込みが失敗する
 * （現行仕様 の手順）。 */
static void test_b17_irq_accept_origin(void) {
    const uint32_t msp = 0x00007000u;
    mc68k_state_t *cpu = boot(MC68K_MODEL_68020, 1);
    uint32_t p = emit_w(PROG, OP_MOVE_L_IMM_D0);
    p = emit_l(p, msp);
    p = emit_w(p, OP_MOVEC_TO_CTRL);
    p = emit_w(p, EXT_MOVEC_D0_MSP);
    const uint32_t move_sr_pc = p;
    p = emit_w(p, OP_MOVE_IMM_SR);
    p = emit_w(p, 0x3000); /* S=1・M=1・割込みマスク 0 */
    const uint32_t nop_pc = p;
    emit_w(p, OP_NOP);
    assert(move_sr_pc == 0x200Au && nop_pc == 0x200Eu);

    for (int i = 0; i < 3; i++)
        mc68k_execute(cpu, 1);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == nop_pc && r.a[7] == msp && (r.sr & 0x3000u) == 0x3000u);

    g.fail_write_at = RESET_SSP - 8; /* ISP 上の $1 throwaway フレームの先頭 */
    mc68k_set_irq(cpu, 1);
    mc68k_execute(cpu, 1);
    assert(mc68k_get_exec_state(cpu) == CPU_EXCEPTION);
    assert(mc68k_read_fault_ring(cpu, NULL, 0) == 0); /* まだ保留中 */
    mc68k_set_irq(cpu, 0);
    mc68k_execute(cpu, 1);

    assert(read_ring(cpu) == 1);
    const mc68k_fault_ring_entry_t *e = &ring[0];
    expect_t x = {.vector = 2,
                  .access_kind = MC68K_FAULT_ACCESS_DATA_WRITE,
                  .fc = 5,
                  .size = 2,
                  .read = 0,
                  .rm = 0,
                  .opcode_valid = 0,
                  .instr_pc = nop_pc,
                  .fault_addr = RESET_SSP - 8,
                  .frame_format = 0xA,
                  .outcome = MC68K_FAULT_OUTCOME_VECTORED,
                  .origin = MC68K_FAULT_ORIGIN_IRQ_ACCEPT};
    check_entry(e, &x);
    check_no_mmu(e, 1);
    /* seq は直前に採番された試行（MOVE #$3000,SR）のもの。 */
    mc68k_pc_ring_entry_t pcs[MC68K_PC_RING_SIZE];
    size_t n = mc68k_read_pc_ring(cpu, pcs, MC68K_PC_RING_SIZE);
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (pcs[i].pc == move_sr_pc) {
            assert(pcs[i].seq == e->seq);
            found++;
        }
    }
    assert(found == 1);
    mc68k_get_regs(cpu, &r);
    assert(r.pc == BUS_HANDLER);
    assert(r.a[7] == 0x00006FD8u); /* MSP: 実際に積まれたIRQフレームは保持される */
    /* mc68000#57: ISPシャドウ(cpu->ssp)は、throwawayフレーム書込み失敗時点で
     * a[7]が指していた本当のISP（$1フレームの直前=RESET_SSP-8）を保持していなければ
     * ならない。巻き戻しでa[7]を割込み受付前のMSP値(msp=0x00007000)で上書きしてから
     * cpu_set_sr()を呼ぶと、そのMSP値がISPシャドウへ誤って保存される（修正前の挙動）。 */
    assert(cpu->ssp == RESET_SSP - 8);
    check_stack_frame(cpu, MC68K_MODEL_68020, e);
    mc68k_destroy(cpu);
}

/* B18 / B19 のハンドラ: SP を戻して MOVE.W (A0),D0 をやり直す。 */
static void put_refault_handler(void) {
    uint32_t p = emit_w(ADDR_HANDLER, OP_MOVEA_L_IMM_A7);
    p = emit_l(p, RESET_SSP);
    emit_l(emit_w(p, OP_JMP_ABS_L), PROG + 6);
}

/* B18: 折返し。容量 + 3 回のフォールトで、最古の3件が上書きされる。 */
static void test_b18_wraparound(void) {
    enum { FAULTS = MC68K_FAULT_RING_SIZE + 3 };
    mc68k_state_t *cpu = boot(MC68K_MODEL_68000, 1);
    put_b2_program(MC68K_MODEL_68000);
    put_refault_handler();
    uint64_t seqs[FAULTS];
    int faults = 0;
    for (int i = 0; i < FAULTS * 8 && faults < FAULTS; i++) {
        mc68k_execute(cpu, 1);
        if (mc68k_get_pc(cpu) != ADDR_HANDLER)
            continue;
        /* 独立な出どころ: フォールトした試行（pc=0x2006）の pc ring 上の seq。 */
        mc68k_pc_ring_entry_t last;
        size_t n = mc68k_read_pc_ring(cpu, NULL, 0);
        mc68k_pc_ring_entry_t pcs[MC68K_PC_RING_SIZE];
        assert(mc68k_read_pc_ring(cpu, pcs, MC68K_PC_RING_SIZE) == n);
        last = pcs[n - 1];
        assert(last.pc == PROG + 6);
        seqs[faults++] = last.seq;
    }
    assert(faults == FAULTS);
    assert(mc68k_read_fault_ring(cpu, NULL, 0) == MC68K_FAULT_RING_SIZE);
    assert(read_ring(cpu) == MC68K_FAULT_RING_SIZE);
    for (int i = 0; i < MC68K_FAULT_RING_SIZE; i++) {
        assert(ring[i].seq == seqs[i + 3]); /* out[0] が4回目のフォールト */
        if (i > 0)
            assert(ring[i].seq > ring[i - 1].seq);
    }
    mc68k_destroy(cpu);
}

/* B19: OFF 中のフォールトは載らない。reset 後も ON のまま履歴は保持され、seq は戻らない。 */
static void test_b19_off_on_reset(void) {
    mc68k_state_t *cpu = boot(MC68K_MODEL_68000, 1);
    put_b2_program(MC68K_MODEL_68000);
    put_refault_handler();
    run_to_fault(cpu);
    assert(read_ring(cpu) == 1);
    const uint64_t first_seq = ring[0].seq;

    mc68k_set_fault_ring(cpu, 0);
    mc68k_execute(cpu, 1); /* ハンドラの MOVEA.L */
    mc68k_execute(cpu, 1); /* ハンドラの JMP */
    run_to_fault(cpu);
    assert(mc68k_get_pc(cpu) == ADDR_HANDLER);
    assert(read_ring(cpu) == 1);
    assert(ring[0].seq == first_seq);

    mc68k_set_fault_ring(cpu, 1);
    mc68k_reset(cpu);
    assert(mc68k_get_fault_ring(cpu) == 1);
    assert(read_ring(cpu) == 1);
    assert(ring[0].seq == first_seq);
    run_to_fault(cpu);
    assert(read_ring(cpu) == 2);
    assert(ring[0].seq == first_seq);
    assert(ring[1].seq > first_seq);
    assert(ring[1].instr_pc == PROG + 6);
    mc68k_destroy(cpu);
}

/* ---- B20: dump ---- */

typedef struct {
    char text[8192];
    size_t len;
} log_capture_t;

static void capture_log(void *ctx, const char *msg) {
    log_capture_t *c = ctx;
    size_t n = strlen(msg);
    assert(c->len + n < sizeof(c->text));
    memcpy(c->text + c->len, msg, n + 1);
    c->len += n;
}

static void dump_to(mc68k_state_t *cpu, log_capture_t *log) {
    memset(log, 0, sizeof(*log));
    mc68k_set_log_handler(cpu, capture_log, log);
    mc68k_dump_fault_ring(cpu, 0);
    mc68k_set_log_handler(cpu, discard_log, NULL);
}

static void test_b20_dump(void) {
    static log_capture_t log;

    mc68k_state_t *cpu = boot(MC68K_MODEL_68000, 1);
    dump_to(cpu, &log);
    assert(strstr(log.text, "=== fault trace (last 0 faults, oldest first) ===\n"));
    assert(strstr(log.text, "=== end fault trace ===\n"));
    put_b2_program(MC68K_MODEL_68000);
    run_to_fault(cpu);
    dump_to(cpu, &log);
    assert(strstr(log.text, "last 1 faults"));
    assert(strstr(log.text, " vec=3 fmt=0 "));
    assert(strstr(log.text, " out=VECTORED org=INSN\n"));
    assert(!strstr(log.text, "  mmu "));
    mc68k_destroy(cpu);

    cpu = boot_pmmu(FAULT_VA);
    run_to_fault(cpu);
    resume_at(cpu, PROG + 6);
    run_to_fault(cpu);
    dump_to(cpu, &log);
    assert(strstr(log.text, "last 2 faults"));
    const char *walk =
        strstr(log.text, "  mmu src=WALK fault=INVALID level=2 desc=0000080C tberr=-");
    const char *atc = strstr(log.text, "  mmu src=ATC fault=INVALID level=- desc=- tberr=-");
    assert(walk && atc && walk < atc);
    mc68k_destroy(cpu);
}

int main(void) {
    test_b1_default_off();
    test_b2_data_read();
    test_b3_data_write();
    test_b4_opcode_fetch();
    test_b5_extension_fetch();
    test_b6_tas_rm();
    test_b7_b8_frame_write_fault();
    test_b9_vector_read_fault();
    test_b10_refault_safety_valve();
    test_b11_b12_walk_then_atc();
    test_b13_table_bus_error();
    test_b14_host_bus_error_with_pmmu();
    test_b15_latch_cleared_between_faults();
    test_b16_first_fault_latched();
    test_b21_same_iteration_refault_keeps_first();
    test_b17_irq_accept_origin();
    test_b18_wraparound();
    test_b19_off_on_reset();
    test_b20_dump();
    printf("test_fault_ring: all tests passed\n");
    return 0;
}
