/*
 * fpu_proto.c — 応答プリミティブ API と protocol の状態遷移。
 * 設計は docs/mc68000-internals/reference/fpu.md。
 *
 * 遷移の正本は設計書 §4.2 の表 A・表 B。表 A の状態 S1〜S20 は fpu_state_id() で
 * (phase, arith, unstarted, violation_pending, context_valid) から導く。各関数のコメントの
 * 「Sn」は表 A の状態番号。
 *
 * 設計書が Phase B で確定すると定めた項目の選択（§12）:
 *  - hold は条件命令の結果の NULL（TF）だけで、S2 で返し続ける。PC=1 の
 * NULL（要求の無い命令の最初の primitive）は命令の完了で idle の NULL に符号化し直す（Table 7-3
 * は CA=0・PC=1 の NULL を ダイアログの最初の応答としてだけ返すとする。IDLE
 * で返し続けると、PF=1 の NULL を読むまで応答 CIR を 読み続ける MPU が抜けられない。設計書
 * §5.1）。 68882 は S18 で IA 書込みまで同じ NULL を返し、68881 と IA
 * 先書きは読出しで完了する。
 *  - S18 の応答は dialog の種類から導く（NOREQ は PC=1 の NULL、外部宛て FMOVE は
 *    CA=1・PC=1 の NULL、それ以外は消費済みの NULL（CA=1、PC=0））。
 *  - 演算の実行（共通コアの呼出し）は、ダイアログの入力が揃った時点で行う。arith は
 *    ダイアログ中は NONE/PENDING に保つ（不変条件4）。
 */
#include <string.h>
#include "fpu_exec.h"
#include "fpu_internal.h"
#include "fpu_model.h"

#define LEN_CONTROL_REGISTER  4u
#define LEN_FP_REGISTER       12u
#define WORDS_PER_FP_REGISTER 3u
#define MASK_SHIFT            8
#define VEC_NONE              0u

/* Table 7-4 の valid EA コード */
#define VALID_EA_CONTROL_ALTERABLE 0u
#define VALID_EA_DATA_ALTERABLE    1u
#define VALID_EA_MEMORY_ALTERABLE  2u
#define VALID_EA_ALTERABLE         3u
#define VALID_EA_DATA              5u
#define VALID_EA_MEMORY            6u
#define VALID_EA_ANY               7u

/* ---- 小さな道具 --------------------------------------------------------- */

unsigned fpu_popcount8(unsigned v) {
    unsigned n = 0;

    for (v &= 0xFFu; v; v >>= 1)
        n += v & 1u;
    return n;
}

static bool is_82(const struct mc68k_fpu *f) {
    return f->config == MC68K_FPU_68882;
}

bool fpu_command_passes_pending(const mc68k_fpu_command_t *cmd) {
    return cmd->valid && (cmd->command_class == MC68K_FPU_COMMAND_MOVEM ||
                          cmd->command_class == MC68K_FPU_COMMAND_MOVE_CONTROL);
}

fpu_dk_t fpu_dialog_kind(const mc68k_fpu_command_t *cmd) {
    switch (cmd->command_class) {
    case MC68K_FPU_COMMAND_MOVE_CONTROL:
        return cmd->transfer_to_external ? FPU_DK_CTRL_OUT : FPU_DK_CTRL_IN;
    case MC68K_FPU_COMMAND_MOVEM:
        if (cmd->dynamic_register_mask)
            return cmd->transfer_to_external ? FPU_DK_MOVEM_OUT_DYN : FPU_DK_MOVEM_IN_DYN;
        return cmd->transfer_to_external ? FPU_DK_MOVEM_OUT_STATIC : FPU_DK_MOVEM_IN_STATIC;
    default:
        break;
    }
    if (cmd->transfer_to_external)
        return cmd->dynamic_packed_k_factor ? FPU_DK_PK_OUT_DYN : FPU_DK_EA_OUT;
    if (cmd->source_kind == MC68K_FPU_SOURCE_EXTERNAL)
        return FPU_DK_EA_IN;
    return FPU_DK_NOREQ;
}

unsigned fpu_expected_in_words(const struct fpu_dialog *d) {
    switch (d->dk) {
    case FPU_DK_EA_IN:
        return fpu_format_words(d->cmd.format);
    case FPU_DK_CTRL_IN:
        return fpu_popcount8(d->cmd.control_mask);
    case FPU_DK_MOVEM_IN_STATIC:
    case FPU_DK_MOVEM_IN_DYN:
        return fpu_popcount8(d->mask) * WORDS_PER_FP_REGISTER;
    default:
        return 0;
    }
}

unsigned fpu_expected_out_words(const struct fpu_dialog *d) {
    switch (d->dk) {
    case FPU_DK_EA_OUT:
    case FPU_DK_PK_OUT_DYN:
        return fpu_format_words(d->cmd.format);
    case FPU_DK_CTRL_OUT:
        return fpu_popcount8(d->cmd.control_mask);
    case FPU_DK_MOVEM_OUT_STATIC:
    case FPU_DK_MOVEM_OUT_DYN:
        return fpu_popcount8(d->mask) * WORDS_PER_FP_REGISTER;
    default:
        return 0;
    }
}

/* ---- 状態 --------------------------------------------------------------- */

int fpu_state_id(const struct mc68k_fpu *f) {
    switch (f->phase) {
    case FPU_PH_COMMAND_PENDING:
        return STATE_S12;
    case FPU_PH_CONDITION_PENDING:
        return STATE_S13;
    case FPU_PH_RESPONSE_NEXT:
        return STATE_S14;
    case FPU_PH_OPERAND_IN:
        return STATE_S15;
    case FPU_PH_OPERAND_OUT:
        return STATE_S16;
    case FPU_PH_REGISTER_SELECT:
        return STATE_S17;
    case FPU_PH_PC:
        return f->unstarted.active ? STATE_S8 : STATE_S18;
    case FPU_PH_SAVE_BODY:
        return STATE_S19;
    case FPU_PH_RESTORE_BODY:
        return STATE_S20;
    case FPU_PH_IDLE:
        break;
    }
    if (f->violation_pending)
        return f->context_valid ? STATE_S10 : STATE_S11;
    if (f->unstarted.active) {
        switch (f->unstarted.origin) {
        case FPU_ORIGIN_ARITH:
            return STATE_S4;
        case FPU_ORIGIN_FLINE:
            return STATE_S5;
        case FPU_ORIGIN_BSUN:
            return f->unstarted.ia_satisfied ? STATE_S7 : STATE_S6;
        }
    }
    if (!f->context_valid)
        return STATE_S1;
    return f->arith == FPU_ARITH_NONE ? STATE_S2
                                      : (f->arith == FPU_ARITH_PENDING ? STATE_S3 : STATE_S9);
}

void fpu_set_step(struct mc68k_fpu *f, fpu_step_t step) {
    f->step = step;
    switch (step) {
    case FPU_STEP_NONE:
        f->phase = FPU_PH_IDLE;
        break;
    case FPU_STEP_FIRST:
        f->phase = FPU_PH_COMMAND_PENDING;
        break;
    case FPU_STEP_COND:
        f->phase = FPU_PH_CONDITION_PENDING;
        break;
    case FPU_STEP_NEXT_EA_OUT:
    case FPU_STEP_NEXT_FPREGS:
        f->phase = FPU_PH_RESPONSE_NEXT;
        break;
    case FPU_STEP_IN_EA:
    case FPU_STEP_IN_DN:
    case FPU_STEP_IN_REGS:
        f->phase = FPU_PH_OPERAND_IN;
        break;
    case FPU_STEP_OUT:
        f->phase = FPU_PH_OPERAND_OUT;
        break;
    case FPU_STEP_REGSEL:
        f->phase = FPU_PH_REGISTER_SELECT;
        break;
    case FPU_STEP_PC:
        f->phase = FPU_PH_PC;
        break;
    }
}

/* ダイアログと進行中の転送を捨てて IDLE に戻る。arith・UNSTARTED・violation・hold は変えない */
void fpu_to_idle(struct mc68k_fpu *f) {
    memset(&f->dlg, 0, sizeof(f->dlg));
    f->phase = FPU_PH_IDLE;
    f->step = FPU_STEP_NONE;
    f->body_words = 0;
    f->body_pos = 0;
    f->restore_busy = false;
}

/* 表 A の記法「V」: ダイアログと UNSTARTED を捨て、arith は変えず、violation 保留にする。
 * 既に violation 保留中（S10・S11）なら何も変えない。戻り値は常に false */
bool fpu_violation(struct mc68k_fpu *f) {
    if (f->violation_pending)
        return false;
    fpu_to_idle(f);
    memset(&f->unstarted, 0, sizeof(f->unstarted));
    memset(&f->hold, 0, sizeof(f->hold));
    f->violation_pending = true;
    return false;
}

/* ---- 応答の組立て -------------------------------------------------------- */

/* NULL primitive の符号化（[881UM Table 7-3] の7種のうち使うもの。processing は !PF） */
static mc68k_fpu_response_t resp_null_raw(bool ca, bool pc, bool ia, bool pf, bool tf) {
    mc68k_fpu_response_t r;

    memset(&r, 0, sizeof(r));
    r.kind = MC68K_FPU_RESPONSE_NULL;
    r.coprocessor_active = ca;
    r.request_pc = pc;
    r.interruptible = ia;
    r.processing = !pf;
    r.condition_true = tf ? 1u : 0u;
    return r;
}

/* idle の NULL: CA=0、PC=0、IA=0、PF=1、TF=0 */
static mc68k_fpu_response_t resp_null_idle(void) {
    return resp_null_raw(false, false, false, true, false);
}

/* 条件命令の結果: CA=0、PC=0、IA=0、PF=0、TF=結果 */
static mc68k_fpu_response_t resp_null_cond(bool tf) {
    return resp_null_raw(false, false, false, false, tf);
}

/* 命令の途中/終了段階の NULL: IA=1、PF=0（CA=0 の PC=0/1、CA=1 の PC=0/1 の4種） */
static mc68k_fpu_response_t resp_null_phase(bool ca, bool pc) {
    return resp_null_raw(ca, pc, true, false, false);
}

static mc68k_fpu_response_t resp_ea(bool dr, unsigned length, uint8_t valid_ea, bool pc) {
    mc68k_fpu_response_t r;

    memset(&r, 0, sizeof(r));
    r.kind = MC68K_FPU_RESPONSE_EA_TRANSFER;
    r.coprocessor_active = true;
    r.request_pc = pc;
    r.transfer_to_host = dr;
    r.length = (uint8_t)length;
    r.valid_ea = valid_ea;
    return r;
}

static mc68k_fpu_response_t resp_mp(uint8_t dn) {
    mc68k_fpu_response_t r;

    memset(&r, 0, sizeof(r));
    r.kind = MC68K_FPU_RESPONSE_MP_REGISTER;
    r.coprocessor_active = true;
    r.data_register = dn;
    return r;
}

static mc68k_fpu_response_t resp_fpregs(bool dr) {
    mc68k_fpu_response_t r;

    memset(&r, 0, sizeof(r));
    r.kind = MC68K_FPU_RESPONSE_FP_REGISTERS;
    r.coprocessor_active = true;
    r.transfer_to_host = dr;
    r.length = (uint8_t)LEN_FP_REGISTER;
    return r;
}

static mc68k_fpu_response_t resp_take(mc68k_fpu_response_kind_t kind, uint8_t vector, bool pc) {
    mc68k_fpu_response_t r;

    memset(&r, 0, sizeof(r));
    r.kind = kind;
    r.request_pc = pc;
    r.vector = vector;
    return r;
}

/* control register 転送の valid EA（Table 7-5）。入力は FPIAR 単独なら any、FPCR/FPSR 単独なら
 * data、 複数なら memory。出力は alterable 側の同じ分類 */
static uint8_t control_valid_ea(const mc68k_fpu_command_t *cmd) {
    unsigned n = fpu_popcount8(cmd->control_mask);

    if (n >= 2)
        return cmd->transfer_to_external ? VALID_EA_MEMORY_ALTERABLE : VALID_EA_MEMORY;
    if (cmd->control_mask == MC68K_FPU_CONTROL_FPIAR)
        return cmd->transfer_to_external ? VALID_EA_ALTERABLE : VALID_EA_ANY;
    return cmd->transfer_to_external ? VALID_EA_DATA_ALTERABLE : VALID_EA_DATA;
}

static uint8_t format_valid_ea(mc68k_fpu_format_t fmt, bool to_external) {
    bool memory_only =
        fmt == MC68K_FPU_FORMAT_D || fmt == MC68K_FPU_FORMAT_X || fmt == MC68K_FPU_FORMAT_P;

    if (to_external)
        return memory_only ? VALID_EA_MEMORY_ALTERABLE : VALID_EA_DATA_ALTERABLE;
    return memory_only ? VALID_EA_MEMORY : VALID_EA_DATA;
}

static mc68k_fpu_response_t first_primitive(const struct mc68k_fpu *f) {
    const struct fpu_dialog *d = &f->dlg;
    const mc68k_fpu_command_t *c = &d->cmd;

    switch (d->dk) {
    case FPU_DK_NOREQ:
        return resp_null_phase(false, d->pc_flag);
    case FPU_DK_EA_IN:
        return resp_ea(false, fpu_format_bytes(c->format), format_valid_ea(c->format, false),
                       d->pc_flag);
    case FPU_DK_EA_OUT:
        return resp_null_phase(true, d->pc_flag);
    case FPU_DK_PK_OUT_DYN: {
        mc68k_fpu_response_t r = resp_mp(c->packed_k_factor_data_register);

        r.request_pc = d->pc_flag;
        return r;
    }
    case FPU_DK_CTRL_IN:
    case FPU_DK_CTRL_OUT:
        return resp_ea(c->transfer_to_external,
                       LEN_CONTROL_REGISTER * fpu_popcount8(c->control_mask),
                       control_valid_ea(c), false);
    case FPU_DK_MOVEM_IN_STATIC:
    case FPU_DK_MOVEM_OUT_STATIC:
        return resp_fpregs(c->transfer_to_external);
    default:
        break;
    }
    return resp_mp(c->register_mask_data_register);
}

static mc68k_fpu_response_t next_primitive(const struct mc68k_fpu *f) {
    const mc68k_fpu_command_t *c = &f->dlg.cmd;

    if (f->step == FPU_STEP_NEXT_FPREGS)
        return resp_fpregs(c->transfer_to_external);
    return resp_ea(true, fpu_format_bytes(c->format), format_valid_ea(c->format, true), false);
}

/* S18 の応答（hold を持たず dialog から導く） */
static mc68k_fpu_response_t pc_state_response(const struct mc68k_fpu *f) {
    switch (f->dlg.dk) {
    case FPU_DK_NOREQ:
        return resp_null_phase(false, true);
    case FPU_DK_EA_OUT:
        return resp_null_phase(true, true);
    default:
        break;
    }
    return resp_null_phase(true, false); /* 消費済みの NULL（§5.1） */
}

static uint8_t live_vector(const struct mc68k_fpu *f) {
    return fpu_select_exception(f->state.fpsr_exc, f->state.fpcr_enable, NULL);
}

/* ---- command / condition の開始 ------------------------------------------ */

/* 表 A の「開始」。先に UNSTARTED を捨て、結果は S2 の command 書込みと同じ。
 * arith は呼出し側が決めておく（FMOVEM・control move は PENDING のまま来る） */
void fpu_start_command(struct mc68k_fpu *f, uint16_t word) {
    mc68k_fpu_command_t cmd;

    f->context_valid = true;
    memset(&f->unstarted, 0, sizeof(f->unstarted));
    memset(&f->hold, 0, sizeof(f->hold));
    fpu_to_idle(f);
    if (!mc68k_fpu_decode_command_word(f->config, word, &cmd)) {
        /* 不正 word: F-line（vector 11、PC=0。§5.2）。S5 */
        f->unstarted.active = true;
        f->unstarted.is_condition = false;
        f->unstarted.origin = FPU_ORIGIN_FLINE;
        f->unstarted.word = word;
        return;
    }
    f->dlg.cmd = cmd;
    f->dlg.dk = fpu_dialog_kind(&cmd);
    f->dlg.pc_flag = (f->dlg.dk == FPU_DK_NOREQ || f->dlg.dk == FPU_DK_EA_IN ||
                      f->dlg.dk == FPU_DK_EA_OUT || f->dlg.dk == FPU_DK_PK_OUT_DYN) &&
                     (f->state.fpcr_enable & (uint8_t)~MC68K_FPU_EXC_BSUN) != 0;
    fpu_set_step(f, FPU_STEP_FIRST);
}

/* 保留算術例外による先取り: arith=PRESENTED_PRE、UNSTARTED=word、TAKE_PRE（PC=0）。S4 */
static void present_pre(struct mc68k_fpu *f, uint16_t word, bool is_condition) {
    f->context_valid = true;
    f->arith = FPU_ARITH_PRESENTED_PRE;
    memset(&f->unstarted, 0, sizeof(f->unstarted));
    memset(&f->hold, 0, sizeof(f->hold));
    fpu_to_idle(f);
    f->unstarted.active = true;
    f->unstarted.is_condition = is_condition;
    f->unstarted.origin = FPU_ORIGIN_ARITH;
    f->unstarted.word = is_condition ? (uint16_t)(word & FPU_PREDICATE_MASK) : word;
}

void fpu_start_condition(struct mc68k_fpu *f, uint8_t predicate) {
    bool tf = false;
    bool trap;

    f->context_valid = true;
    memset(&f->unstarted, 0, sizeof(f->unstarted));
    memset(&f->hold, 0, sizeof(f->hold));
    fpu_to_idle(f);
    /* mc68k_fpu_write_condition() は mask 済みでない値を渡す。fpu_frame.c の呼出しは mask
     * 済みなので、 そちらでは冗長（残す。どの呼出し元でも predicate を6bitに保つ） */
    predicate &= FPU_PREDICATE_MASK;
    trap = fpu_evaluate_condition(f, predicate, &tf);
    if (trap) {
        f->unstarted.active = true;
        f->unstarted.is_condition = true;
        f->unstarted.origin = FPU_ORIGIN_BSUN;
        f->unstarted.word = predicate;
        return;
    }
    f->dlg.predicate = predicate;
    f->dlg.tf = tf;
    fpu_set_step(f, FPU_STEP_COND);
}

/* arith が PENDING（S3）または提示済み（S4・S9）のときの command 書込み（表 A の S3・S4・S9）。
 * （PENDING では live の EXC&ENABLE の候補で TAKE か解消かを決める） */
void fpu_start_command_pending_rule(struct mc68k_fpu *f, uint16_t word) {
    mc68k_fpu_command_t cmd;
    bool decoded = mc68k_fpu_decode_command_word(f->config, word, &cmd);
    bool passes = decoded && fpu_command_passes_pending(&cmd);

    if (f->arith == FPU_ARITH_PENDING) {
        if (passes) {
            fpu_start_command(f, word); /* PENDING のまま開始 */
        } else if (live_vector(f) != VEC_NONE) {
            present_pre(f, word, false);
        } else {
            f->arith = FPU_ARITH_NONE; /* 候補が無ければ保留を解消する（§7.2） */
            fpu_start_command(f, word);
        }
        return;
    }
    /* PRESENTED_PRE / PRESENTED_MID（S4・S9） */
    if (passes && !is_82(f)) {
        f->arith = FPU_ARITH_PENDING;
        fpu_start_command(f, word);
        return;
    }
    present_pre(f, word, false);
}

void fpu_start_condition_pending_rule(struct mc68k_fpu *f, uint8_t predicate) {
    if (f->arith == FPU_ARITH_PENDING) {
        if (live_vector(f) != VEC_NONE) {
            present_pre(f, predicate, true);
            return;
        }
        f->arith = FPU_ARITH_NONE;
        fpu_start_condition(f, predicate);
        return;
    }
    present_pre(f, predicate, true);
}

bool mc68k_fpu_write_command_word(mc68k_fpu_t *f, uint16_t word) {
    if (!f)
        return false;
    switch (fpu_state_id(f)) {
    case STATE_S1:
    case STATE_S2:
    case STATE_S5:
    case STATE_S6:
    case STATE_S7:
        fpu_start_command(f, word);
        return true;
    case STATE_S3:
    case STATE_S4:
    case STATE_S9:
        fpu_start_command_pending_rule(f, word);
        return true;
    case STATE_S10:
    case STATE_S11:
        return false;
    default:
        return fpu_violation(f);
    }
}

bool mc68k_fpu_write_condition(mc68k_fpu_t *f, uint8_t predicate) {
    if (!f)
        return false;
    switch (fpu_state_id(f)) {
    case STATE_S1:
    case STATE_S2:
    case STATE_S5:
    case STATE_S6:
    case STATE_S7:
        fpu_start_condition(f, predicate);
        return true;
    case STATE_S3:
    case STATE_S4:
    case STATE_S9:
        fpu_start_condition_pending_rule(f, predicate);
        return true;
    case STATE_S10:
    case STATE_S11:
        return false;
    default:
        return fpu_violation(f);
    }
}

/* ---- ダイアログの完了 ----------------------------------------------------- */

/* 転送が全て済んだ。最終応答は NULL（CA=0）で IDLE へ戻る。mid_exc なら S9（TAKE_MID） */
static void finish_dialog(struct mc68k_fpu *f) {
    bool mid = f->dlg.mid_exc;

    fpu_to_idle(f);
    if (mid)
        f->arith = FPU_ARITH_PRESENTED_MID;
}

/* 要求の無い命令の完了: 実行して S2/S3 へ。以後の応答は idle の NULL（PC=1 の NULL
 * は返し続けない） */
static void finish_noreq(struct mc68k_fpu *f) {
    fpu_exec_operation(f);
    fpu_to_idle(f);
    memset(&f->hold, 0, sizeof(f->hold));
}

static void enter_in_ea(struct mc68k_fpu *f) {
    f->dlg.cnt = 0;
    f->dlg.total = (uint8_t)fpu_expected_in_words(&f->dlg);
    fpu_set_step(f, FPU_STEP_IN_EA);
}

/* S12 の最初の primitive を読んだ後の遷移。PC=1 の primitive を読んだ 68882 は S18
 * へ（§5.2、§5.5） */
static void after_first_primitive(struct mc68k_fpu *f) {
    struct fpu_dialog *d = &f->dlg;
    bool to_pc = d->pc_flag && !d->ia_pre && is_82(f);

    switch (d->dk) {
    case FPU_DK_NOREQ:
        if (to_pc)
            fpu_set_step(f, FPU_STEP_PC);
        else
            finish_noreq(f);
        break;
    case FPU_DK_EA_IN:
        if (to_pc)
            fpu_set_step(f, FPU_STEP_PC);
        else
            enter_in_ea(f);
        break;
    case FPU_DK_EA_OUT:
        if (to_pc) {
            fpu_set_step(f, FPU_STEP_PC);
        } else {
            fpu_prepare_ext_output(f);
            fpu_set_step(f, FPU_STEP_NEXT_EA_OUT);
        }
        break;
    case FPU_DK_PK_OUT_DYN:
        if (to_pc)
            fpu_set_step(f, FPU_STEP_PC);
        else
            fpu_set_step(f, FPU_STEP_IN_DN);
        break;
    case FPU_DK_CTRL_IN:
        enter_in_ea(f);
        break;
    case FPU_DK_CTRL_OUT:
        fpu_prepare_control_out(f);
        fpu_set_step(f, FPU_STEP_OUT);
        break;
    case FPU_DK_MOVEM_IN_STATIC:
    case FPU_DK_MOVEM_OUT_STATIC:
        d->mask = (uint8_t)d->cmd.register_mask;
        d->mask_known = true;
        fpu_set_step(f, FPU_STEP_REGSEL);
        break;
    default:
        fpu_set_step(f, FPU_STEP_IN_DN);
        break;
    }
}

/* ---- 応答読出し ----------------------------------------------------------- */

mc68k_fpu_response_t mc68k_fpu_read_response(mc68k_fpu_t *f) {
    mc68k_fpu_response_t r;

    if (!f)
        return resp_null_idle();
    switch (fpu_state_id(f)) {
    case STATE_S2:
        if (f->hold.active)
            return resp_null_cond(f->hold.tf);
        return resp_null_idle();
    case STATE_S3: /* hold を持たない（§5.1。command 書込みと begin_restore が hold を空にする）
                    */
        return resp_null_idle();
    case STATE_S4:
    case STATE_S9:
        return resp_take(f->arith == FPU_ARITH_PRESENTED_MID
                             ? MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION
                             : MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION,
                         live_vector(f), false);
    case STATE_S5:
        return resp_take(MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, FPU_VECTOR_F_LINE, false);
    case STATE_S6:
        if (is_82(f))
            fpu_set_step(f, FPU_STEP_PC); /* S8 */
        return resp_take(MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, FPU_VECTOR_BSUN, true);
    case STATE_S7:
    case STATE_S8:
        return resp_take(MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, FPU_VECTOR_BSUN, true);
    case STATE_S10:
    case STATE_S11:
        return resp_take(MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION, FPU_VECTOR_PROTOCOL, false);
    case STATE_S12:
        r = first_primitive(f);
        after_first_primitive(f);
        return r;
    case STATE_S13:
        r = resp_null_cond(f->dlg.tf);
        memset(&f->hold, 0, sizeof(f->hold));
        f->hold.active = true;
        f->hold.tf = f->dlg.tf;
        fpu_to_idle(f);
        return r;
    case STATE_S14:
        r = next_primitive(f);
        if (f->step == FPU_STEP_NEXT_FPREGS)
            fpu_set_step(f, FPU_STEP_REGSEL);
        else
            fpu_set_step(f, FPU_STEP_OUT);
        return r;
    case STATE_S15:
    case STATE_S16:
    case STATE_S17:
        return resp_null_phase(true, false);
    case STATE_S18:
        return pc_state_response(f);
    default:
        break;
    }
    return resp_null_idle();
}

/* ---- オペランド CIR／レジスタ選択 CIR ------------------------------------ */

static void complete_in_ea(struct mc68k_fpu *f) {
    struct fpu_dialog *d = &f->dlg;

    if (d->dk == FPU_DK_EA_IN)
        fpu_exec_operation(f);
    else
        fpu_exec_control_in(f);
    finish_dialog(f);
}

static void received_dn(struct mc68k_fpu *f, uint32_t word) {
    struct fpu_dialog *d = &f->dlg;

    d->dn_value = (uint8_t)word;
    if (d->dk == FPU_DK_PK_OUT_DYN) {
        fpu_prepare_ext_output(f);
        fpu_set_step(f, FPU_STEP_NEXT_EA_OUT);
        return;
    }
    d->mask = d->dn_value;
    d->mask_known = true;
    fpu_set_step(f, FPU_STEP_NEXT_FPREGS);
}

static void apply_restore_if_complete(struct mc68k_fpu *f) {
    if (f->body_pos == f->body_words)
        fpu_apply_restore_frame(f);
}

bool mc68k_fpu_write_operand(mc68k_fpu_t *f, uint32_t word) {
    struct fpu_dialog *d;

    if (!f)
        return false;
    /* 冗長（残す）: violation 保留中は phase が IDLE で、下の default も fpu_violation() が
     * false を返す。 保留中の operand 書込みを受けないことを、phase に依らず先に示す */
    if (f->violation_pending)
        return false;
    d = &f->dlg;
    switch (f->phase) {
    case FPU_PH_RESTORE_BODY:
        f->body[f->body_pos++] = word;
        apply_restore_if_complete(f);
        return true;
    case FPU_PH_OPERAND_IN:
        if (f->step == FPU_STEP_IN_DN) {
            received_dn(f, word);
            return true;
        }
        d->buf[d->cnt++] = word;
        if (d->cnt < d->total)
            return true;
        if (f->step == FPU_STEP_IN_REGS) {
            fpu_exec_movem_in(f);
            finish_dialog(f);
        } else {
            complete_in_ea(f);
        }
        return true;
    default:
        break;
    }
    return fpu_violation(f);
}

bool mc68k_fpu_read_operand(mc68k_fpu_t *f, uint32_t *word) {
    struct fpu_dialog *d;

    if (!f || !word)
        return false;
    if (f->violation_pending)
        return false;
    d = &f->dlg;
    switch (f->phase) {
    case FPU_PH_SAVE_BODY:
        *word = f->body[f->body_pos++];
        if (f->body_pos == f->body_words)
            fpu_to_idle(f);
        return true;
    case FPU_PH_OPERAND_OUT:
        *word = d->buf[d->cnt++];
        if (d->cnt == d->total)
            finish_dialog(f);
        return true;
    default:
        break;
    }
    return fpu_violation(f);
}

bool mc68k_fpu_read_register_select(mc68k_fpu_t *f, uint16_t *mask) {
    struct fpu_dialog *d;
    unsigned n;

    if (!f || !mask)
        return false;
    if (f->violation_pending)
        return false;
    if (f->phase != FPU_PH_REGISTER_SELECT)
        return fpu_violation(f);
    d = &f->dlg;
    *mask = (uint16_t)((unsigned)d->mask << MASK_SHIFT);
    n = fpu_popcount8(d->mask);
    if (n == 0) {
        finish_dialog(f); /* 転送する register が無い。最後の要求を満たした（§4.1） */
    } else if (d->dk == FPU_DK_MOVEM_IN_STATIC || d->dk == FPU_DK_MOVEM_IN_DYN) {
        d->cnt = 0;
        d->total = (uint8_t)(n * WORDS_PER_FP_REGISTER);
        fpu_set_step(f, FPU_STEP_IN_REGS);
    } else {
        fpu_prepare_movem_out(f);
        fpu_set_step(f, FPU_STEP_OUT);
    }
    return true;
}

bool mc68k_fpu_write_register_select(mc68k_fpu_t *f) {
    if (!f)
        return false;
    return fpu_violation(f);
}

/* ---- 命令アドレス CIR ---------------------------------------------------- */

/* S12 の IA 書込みが violation になる最初の primitive（§5.5 の表） */
static bool ia_illegal_in_command_pending(const struct fpu_dialog *d) {
    switch (d->dk) {
    case FPU_DK_MOVEM_IN_STATIC:
    case FPU_DK_MOVEM_OUT_STATIC:
    case FPU_DK_CTRL_OUT:
        return true;
    case FPU_DK_EA_OUT:
        return !d->pc_flag;
    default:
        return false;
    }
}

bool mc68k_fpu_write_instruction_address(mc68k_fpu_t *f, uint32_t pc) {
    if (!f)
        return false;
    if (!is_82(f)) {
        f->state.fpiar = pc; /* 68881 は phase に関係なく常に合法（§5.2） */
        return true;
    }
    switch (fpu_state_id(f)) {
    case STATE_S6:
    case STATE_S8:
        f->state.fpiar = pc;
        f->unstarted.ia_satisfied = true;
        fpu_to_idle(f); /* S7 */
        return true;
    case STATE_S10:
    case STATE_S11:
        return false;
    case STATE_S12:
        if (ia_illegal_in_command_pending(&f->dlg))
            return fpu_violation(f);
        f->state.fpiar = pc;
        if (f->dlg.pc_flag)
            f->dlg.ia_pre = true;
        return true;
    case STATE_S13:
    case STATE_S15:
    case STATE_S20:
        f->state.fpiar = pc;
        return true;
    case STATE_S18:
        f->state.fpiar = pc;
        /* 以下の hold の消去は冗長（残す）: S18 は hold を持たず、command
         * 開始（fpu_start_command）が 既に空にしている。S2 の hold が S18
         * の完了後へ残らないことを、この場所でも保証する */
        switch (f->dlg.dk) {
        case FPU_DK_NOREQ:
            finish_noreq(f);
            break;
        case FPU_DK_EA_OUT:
            memset(&f->hold, 0, sizeof(f->hold));
            fpu_prepare_ext_output(f);
            fpu_set_step(f, FPU_STEP_NEXT_EA_OUT);
            break;
        case FPU_DK_EA_IN:
            memset(&f->hold, 0, sizeof(f->hold));
            enter_in_ea(f);
            break;
        default:
            memset(&f->hold, 0, sizeof(f->hold));
            fpu_set_step(f, FPU_STEP_IN_DN);
            break;
        }
        return true;
    default:
        return fpu_violation(f);
    }
}

/* ---- 制御 CIR ------------------------------------------------------------- */

/* 全てを捨てて IDLE に戻る（arith は呼出し側が決める）。context_valid は変えない */
static void drop_everything(struct mc68k_fpu *f) {
    fpu_to_idle(f);
    memset(&f->unstarted, 0, sizeof(f->unstarted));
    memset(&f->hold, 0, sizeof(f->hold));
    f->violation_pending = false;
}

static void control_81(struct mc68k_fpu *f) {
    int s = fpu_state_id(f);

    /* ACK と ABORT は同一（p.7-4）。保留例外もクリアする。S1・S2 は不変（表 A） */
    if (s == STATE_S1 || s == STATE_S2)
        return;
    drop_everything(f);
    f->arith = FPU_ARITH_NONE;
}

static void control_82_ack(struct mc68k_fpu *f) {
    switch (fpu_state_id(f)) {
    case STATE_S4:
        /* UNSTARTED を捨てる→S9（arith は PRESENTED_PRE のまま） */
        memset(&f->unstarted, 0, sizeof(f->unstarted));
        memset(&f->hold, 0, sizeof(f->hold));
        break;
    case STATE_S5:
    case STATE_S6:
    case STATE_S7:
    case STATE_S8:
        drop_everything(f);
        break;
    case STATE_S10:
    case STATE_S11:
        drop_everything(f);
        f->arith = FPU_ARITH_NONE;
        break;
    default:
        break; /* 不変 */
    }
}

static void control_82_abort(struct mc68k_fpu *f) {
    switch (fpu_state_id(f)) {
    case STATE_S4:
        if (!f->unstarted.is_condition) {
            memset(&f->unstarted, 0, sizeof(f->unstarted));
            memset(&f->hold, 0, sizeof(f->hold));
        }
        break;
    case STATE_S5:
        drop_everything(f);
        break;
    case STATE_S10:
    case STATE_S11:
        /* violation だけ解消し、arith は変えない */
        drop_everything(f);
        break;
    case STATE_S12:
    case STATE_S14:
    case STATE_S15:
    case STATE_S16:
    case STATE_S17:
    case STATE_S18:
    case STATE_S19:
    case STATE_S20:
        fpu_to_idle(f);
        memset(&f->hold, 0, sizeof(f->hold));
        break;
    default:
        break; /* 不変 */
    }
}

void mc68k_fpu_control(mc68k_fpu_t *f, mc68k_fpu_control_t control) {
    if (!f)
        return;
    switch (control) {
    case MC68K_FPU_CONTROL_RESET:
        mc68k_fpu_model_reset(f);
        break;
    case MC68K_FPU_CONTROL_EXCEPTION_ACK:
        if (is_82(f))
            control_82_ack(f);
        else
            control_81(f);
        break;
    case MC68K_FPU_CONTROL_ABORT:
        if (is_82(f))
            control_82_abort(f);
        else
            control_81(f);
        break;
    }
}
