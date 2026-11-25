/*
 * fpu_exec.c — command の実行（共通コアの呼出し、結果の書戻し、条件評価）。
 * 設計は docs/mc68000-internals/reference/fpu.md。
 *
 * ダイアログは fpu_proto.c が進め、必要な入力が揃った時点でここの関数を呼ぶ。
 * opmode→共通コア関数の対応は本ファイルの実装を参照。
 */
#include <string.h>
#include "fpu_internal.h"
#include "fpu_model.h"
#include "fpu_exec.h"

#define OP_FMOVE        0x00u
#define OP_FINT         0x01u
#define OP_FSINH        0x02u
#define OP_FINTRZ       0x03u
#define OP_FSQRT        0x04u
#define OP_FLOGNP1      0x06u
#define OP_FETOXM1      0x08u
#define OP_FTANH        0x09u
#define OP_FATAN        0x0Au
#define OP_FASIN        0x0Cu
#define OP_FATANH       0x0Du
#define OP_FSIN         0x0Eu
#define OP_FTAN         0x0Fu
#define OP_FETOX        0x10u
#define OP_FTWOTOX      0x11u
#define OP_FTENTOX      0x12u
#define OP_FLOGN        0x14u
#define OP_FLOG10       0x15u
#define OP_FLOG2        0x16u
#define OP_FABS         0x18u
#define OP_FCOSH        0x19u
#define OP_FNEG         0x1Au
#define OP_FACOS        0x1Cu
#define OP_FCOS         0x1Du
#define OP_FGETEXP      0x1Eu
#define OP_FGETMAN      0x1Fu
#define OP_FDIV         0x20u
#define OP_FMOD         0x21u
#define OP_FADD         0x22u
#define OP_FMUL         0x23u
#define OP_FSGLDIV      0x24u
#define OP_FREM         0x25u
#define OP_FSCALE       0x26u
#define OP_FSGLMUL      0x27u
#define OP_FSUB         0x28u
#define OP_FSINCOS_BASE 0x30u
#define OP_FSINCOS_MASK 0x78u
#define OP_FCMP         0x38u
#define OP_FTST         0x3Au

#define WORD_SHIFT_HI 32
#define HI16_SHIFT    16
#define BYTE_SHIFT    24
#define PACKED_WORDS  3u

typedef mc68k_fp80_t (*monadic_fn)(mc68k_fp80_t, mc68k_fpu_state_t *);
typedef mc68k_fp80_t (*dyadic_fn)(mc68k_fp80_t, mc68k_fp80_t, mc68k_fpu_state_t *);

static const struct {
    uint8_t op;
    monadic_fn fn;
} monadic_table[] = {
    {OP_FINT, mc68k_fpu_int},       {OP_FSINH, mc68k_fpu_sinh},
    {OP_FINTRZ, mc68k_fpu_intrz},   {OP_FSQRT, mc68k_fpu_sqrt},
    {OP_FLOGNP1, mc68k_fpu_lognp1}, {OP_FETOXM1, mc68k_fpu_etoxm1},
    {OP_FTANH, mc68k_fpu_tanh},     {OP_FATAN, mc68k_fpu_atan},
    {OP_FASIN, mc68k_fpu_asin},     {OP_FATANH, mc68k_fpu_atanh},
    {OP_FSIN, mc68k_fpu_sin},       {OP_FTAN, mc68k_fpu_tan},
    {OP_FETOX, mc68k_fpu_etox},     {OP_FTWOTOX, mc68k_fpu_twotox},
    {OP_FTENTOX, mc68k_fpu_tentox}, {OP_FLOGN, mc68k_fpu_logn},
    {OP_FLOG10, mc68k_fpu_log10},   {OP_FLOG2, mc68k_fpu_log2},
    {OP_FABS, mc68k_fpu_abs},       {OP_FCOSH, mc68k_fpu_cosh},
    {OP_FNEG, mc68k_fpu_neg},       {OP_FACOS, mc68k_fpu_acos},
    {OP_FCOS, mc68k_fpu_cos},       {OP_FGETEXP, mc68k_fpu_getexp},
    {OP_FGETMAN, mc68k_fpu_getman},
};

static const struct {
    uint8_t op;
    dyadic_fn fn;
} dyadic_table[] = {
    {OP_FDIV, mc68k_fpu_div},     {OP_FMOD, mc68k_fpu_mod},       {OP_FADD, mc68k_fpu_add},
    {OP_FMUL, mc68k_fpu_mul},     {OP_FSGLDIV, mc68k_fpu_sgldiv}, {OP_FREM, mc68k_fpu_rem},
    {OP_FSCALE, mc68k_fpu_scale}, {OP_FSGLMUL, mc68k_fpu_sglmul}, {OP_FSUB, mc68k_fpu_sub},
};

/* §8.1 の優先順位（高い方から）。ベクタは [881UM Table 6-1] */
static const struct {
    uint8_t bit;
    uint8_t vector;
} exception_order[] = {
    {MC68K_FPU_EXC_BSUN, FPU_VECTOR_BSUN},   {MC68K_FPU_EXC_SNAN, FPU_VECTOR_SNAN},
    {MC68K_FPU_EXC_OPERR, FPU_VECTOR_OPERR}, {MC68K_FPU_EXC_OVFL, FPU_VECTOR_OVFL},
    {MC68K_FPU_EXC_UNFL, FPU_VECTOR_UNFL},   {MC68K_FPU_EXC_DZ, FPU_VECTOR_DZ},
    {MC68K_FPU_EXC_INEX2, FPU_VECTOR_INEX},  {MC68K_FPU_EXC_INEX1, FPU_VECTOR_INEX},
};

uint8_t fpu_select_exception(uint8_t exc, uint8_t enable, uint8_t *exc_bit) {
    uint8_t active = (uint8_t)(exc & enable);
    size_t i;

    /* [881UM §6.1.10] INEX の trap 条件: OVFL 起因なら EXC.INEX2 が無くても ENABLE.INEX2
     * で候補に加える */
    if ((exc & MC68K_FPU_EXC_OVFL) && (enable & MC68K_FPU_EXC_INEX2))
        active |= MC68K_FPU_EXC_INEX2;
    for (i = 0; i < sizeof(exception_order) / sizeof(exception_order[0]); i++) {
        if (active & exception_order[i].bit) {
            if (exc_bit)
                *exc_bit = exception_order[i].bit;
            return exception_order[i].vector;
        }
    }
    if (exc_bit)
        *exc_bit = 0;
    return 0;
}

/* EXC byte のクリアは命令の開始時（設計書 §2）。条件命令・算術・FMOVE が呼ぶ */
static void begin_instruction(mc68k_fpu_state_t *st) {
    st->fpsr_exc = 0;
}

/* ---- 条件評価 ---------------------------------------------------------- */

#define PRED_IEEE_NONAWARE_BIT 0x10u
#define PRED_LOW_MASK          0x0Fu

void fpu_eval_predicate(mc68k_fpu_state_t *st, uint8_t predicate, bool *tf, bool *bsun) {
    unsigned p =
        (unsigned)(predicate & 0x1Fu); /* $20〜$3F は下位5bitと同じ（Table 4-20 Note 3） */
    unsigned cc = st->fpsr_cc;
    bool n = (cc & MC68K_FPU_CC_N) != 0;
    bool z = (cc & MC68K_FPU_CC_Z) != 0;
    bool nan = (cc & MC68K_FPU_CC_NAN) != 0;
    bool r;

    switch (p & PRED_LOW_MASK) {
    case 0x0:
        r = false;
        break; /* F / SF */
    case 0x1:
        r = z;
        break; /* EQ / SEQ */
    case 0x2:
        r = !(nan || z || n);
        break; /* OGT / GT */
    case 0x3:
        r = z || !(nan || n);
        break; /* OGE / GE */
    case 0x4:
        r = n && !(nan || z);
        break; /* OLT / LT */
    case 0x5:
        r = z || (n && !nan);
        break; /* OLE / LE */
    case 0x6:
        r = !(nan || z);
        break; /* OGL / GL */
    case 0x7:
        r = !nan;
        break; /* OR / GLE */
    case 0x8:
        r = nan;
        break; /* UN / NGLE */
    case 0x9:
        r = nan || z;
        break; /* UEQ / NGL */
    case 0xA:
        r = nan || !(n || z);
        break; /* UGT / NLE */
    case 0xB:
        r = nan || z || !n;
        break; /* UGE / NLT */
    case 0xC:
        r = nan || (n && !z);
        break; /* ULT / NGE */
    case 0xD:
        r = nan || z || n;
        break; /* ULE / NGT */
    case 0xE:
        r = !z;
        break; /* NE / SNE */
    default:
        r = true;
        break; /* T / ST */
    }
    *tf = r;
    /* IEEE nonaware predicate（bit4=1）で NAN が立っていれば BSUN（[881UM] Table 4-20 の Note
     * 2、§4.4.1） */
    *bsun = (p & PRED_IEEE_NONAWARE_BIT) && nan;
}

/* 条件命令の開始: EXC をクリアし、評価し、BSUN なら EXC.BSUN を立てる。
 * 戻り値は BSUN trap（ENABLE.BSUN が立っている）かどうか */
bool fpu_evaluate_condition(struct mc68k_fpu *f, uint8_t predicate, bool *tf) {
    mc68k_fpu_state_t *st = &f->state;
    bool bsun = false;

    begin_instruction(st);
    fpu_eval_predicate(st, predicate, tf, &bsun);
    if (bsun) {
        fpu_commit_exc(st, MC68K_FPU_EXC_BSUN);
        if (st->fpcr_enable & MC68K_FPU_EXC_BSUN)
            return true;
    }
    return false;
}

/* ---- 外部形式 ---------------------------------------------------------- */

unsigned fpu_format_words(mc68k_fpu_format_t fmt) {
    switch (fmt) {
    case MC68K_FPU_FORMAT_D:
        return 2;
    case MC68K_FPU_FORMAT_X:
    case MC68K_FPU_FORMAT_P:
        return 3;
    default:
        return 1;
    }
}

unsigned fpu_format_bytes(mc68k_fpu_format_t fmt) {
    switch (fmt) {
    case MC68K_FPU_FORMAT_B:
        return 1;
    case MC68K_FPU_FORMAT_W:
        return 2;
    case MC68K_FPU_FORMAT_L:
    case MC68K_FPU_FORMAT_S:
        return 4;
    case MC68K_FPU_FORMAT_D:
        return 8;
    default:
        return 12;
    }
}

static mc68k_fp80_t x_from_words(const uint32_t *w) {
    mc68k_fp80_t v;

    v.sign_exp = (uint16_t)(w[0] >> HI16_SHIFT);
    v.signif = ((uint64_t)w[1] << WORD_SHIFT_HI) | w[2];
    return v;
}

static void x_to_words(mc68k_fp80_t v, uint32_t *w) {
    w[0] = (uint32_t)v.sign_exp << HI16_SHIFT;
    w[1] = (uint32_t)(v.signif >> WORD_SHIFT_HI);
    w[2] = (uint32_t)v.signif;
}

static mc68k_fp80_t load_external(const struct fpu_dialog *d, mc68k_fpu_state_t *st) {
    const uint32_t *w = d->buf;

    switch (d->cmd.format) {
    case MC68K_FPU_FORMAT_B:
        return mc68k_fpu_from_b((int8_t)(w[0] >> BYTE_SHIFT));
    case MC68K_FPU_FORMAT_W:
        return mc68k_fpu_from_w((int16_t)(w[0] >> HI16_SHIFT));
    case MC68K_FPU_FORMAT_L:
        return mc68k_fpu_from_l((int32_t)w[0]);
    case MC68K_FPU_FORMAT_S:
        return mc68k_fpu_from_s(w[0], st);
    case MC68K_FPU_FORMAT_D:
        return mc68k_fpu_from_d(((uint64_t)w[0] << WORD_SHIFT_HI) | w[1], st);
    case MC68K_FPU_FORMAT_X:
        return mc68k_fpu_from_x(x_from_words(w), st);
    default:
        break;
    }
    return mc68k_fpu_from_p(w, st);
}

/* ---- 演算 -------------------------------------------------------------- */

/* §8.3: これらの例外では FPn を書き戻さない（OVFL/UNFL/INEX2/INEX1 は書き戻す）。
 * BSUN は条件命令だけが立て、演算では立たないので、ここでは到達しない（冗長だが §8.3
 * の表を写して残す） */
static bool exception_suppresses_writeback(uint8_t exc_bit) {
    return exc_bit == MC68K_FPU_EXC_SNAN || exc_bit == MC68K_FPU_EXC_OPERR ||
           exc_bit == MC68K_FPU_EXC_DZ || exc_bit == MC68K_FPU_EXC_BSUN;
}

void fpu_exec_operation(struct mc68k_fpu *f) {
    mc68k_fpu_state_t *st = &f->state;
    const mc68k_fpu_command_t *cmd = &f->dlg.cmd;
    mc68k_fp80_t result = {0, 0};
    mc68k_fp80_t cos_result = {0, 0};
    bool write_result = true;
    bool write_cos = false;
    uint8_t exc_bit = 0;
    uint8_t op = cmd->opmode;
    size_t i;

    begin_instruction(st);
    if (cmd->source_kind == MC68K_FPU_SOURCE_IMMEDIATE) {
        result = mc68k_fpu_movecr(cmd->movecr_offset, st);
    } else {
        mc68k_fp80_t src = (cmd->source_kind == MC68K_FPU_SOURCE_EXTERNAL)
                               ? load_external(&f->dlg, st)
                               : st->fp[cmd->src_fp];
        mc68k_fp80_t dst = st->fp[cmd->dst_fp];
        bool found = false;

        if (op == OP_FMOVE) {
            result = mc68k_fpu_move(src, st);
            fpu_set_cc(st, result);
            found = true;
        } else if ((op & OP_FSINCOS_MASK) == OP_FSINCOS_BASE) {
            result = mc68k_fpu_sincos(src, &cos_result, st);
            write_cos = true;
            found = true;
        } else if (op == OP_FCMP) {
            mc68k_fpu_cmp(dst, src, st);
            write_result = false;
            found = true;
        } else if (op == OP_FTST) {
            mc68k_fpu_tst(src, st);
            write_result = false;
            found = true;
        }
        if (op == OP_FMOD || op == OP_FREM) {
            /* 冗長（残す）: fpu_mod.c の remainder_op() も先頭で商 byte
             * をクリアする。ここは、その実装が 変わっても「開始時にクリア」を呼出し側で保証する
             */
            st->fpsr_quotient = 0; /* 商 byte は FMOD/FREM 自身の開始時だけクリア（§2） */
            st->fpsr_quotient_sign = 0;
        }
        for (i = 0; !found && i < sizeof(monadic_table) / sizeof(monadic_table[0]); i++) {
            if (monadic_table[i].op == op) {
                result = monadic_table[i].fn(src, st);
                found = true;
            }
        }
        for (i = 0; !found && i < sizeof(dyadic_table) / sizeof(dyadic_table[0]); i++) {
            if (dyadic_table[i].op == op) {
                result = dyadic_table[i].fn(dst, src, st);
                found = true;
            }
        }
        if (!found)
            write_result = false; /* デコーダが弾くので到達しない */
    }

    /* trap enabled のとき書戻しを例外別に決め、保留算術例外にする（§8.2、§8.3） */
    if (fpu_select_exception(st->fpsr_exc, st->fpcr_enable, &exc_bit) != 0) {
        f->arith = FPU_ARITH_PENDING;
        if (exception_suppresses_writeback(exc_bit))
            write_result = false;
    }
    if (write_result) {
        if (write_cos)
            st->fp[cmd->secondary_dst_fp] = cos_result; /* FPs==FPc なら sin が残る */
        st->fp[cmd->dst_fp] = result;
    }
}

/* 外部宛て FMOVE の変換。EXC をクリアして変換し、出力 word 列を dlg.buf に置く。
 * enabled exception があれば転送後に TAKE_MID を返す（mid_exc。§8.3） */
void fpu_prepare_ext_output(struct mc68k_fpu *f) {
    mc68k_fpu_state_t *st = &f->state;
    struct fpu_dialog *d = &f->dlg;
    mc68k_fp80_t v = st->fp[d->cmd.src_fp];

    begin_instruction(st);
    memset(d->buf, 0, sizeof(d->buf));
    switch (d->cmd.format) {
    case MC68K_FPU_FORMAT_B:
        d->buf[0] = (uint32_t)(uint8_t)mc68k_fpu_to_b(v, st) << BYTE_SHIFT;
        break;
    case MC68K_FPU_FORMAT_W:
        d->buf[0] = (uint32_t)(uint16_t)mc68k_fpu_to_w(v, st) << HI16_SHIFT;
        break;
    case MC68K_FPU_FORMAT_L:
        d->buf[0] = (uint32_t)mc68k_fpu_to_l(v, st);
        break;
    case MC68K_FPU_FORMAT_S:
        d->buf[0] = mc68k_fpu_to_s(v, st);
        break;
    case MC68K_FPU_FORMAT_D: {
        uint64_t bits = mc68k_fpu_to_d(v, st);

        d->buf[0] = (uint32_t)(bits >> WORD_SHIFT_HI);
        d->buf[1] = (uint32_t)bits;
        break;
    }
    case MC68K_FPU_FORMAT_X:
        x_to_words(mc68k_fpu_to_x(v, st), d->buf);
        break;
    default: {
        int8_t k =
            d->cmd.dynamic_packed_k_factor ? (int8_t)d->dn_value : d->cmd.packed_k_factor;

        mc68k_fpu_to_p(v, k, d->buf, st);
        break;
    }
    }
    d->total = (uint8_t)fpu_format_words(d->cmd.format);
    d->cnt = 0;
    d->mid_exc = fpu_select_exception(st->fpsr_exc, st->fpcr_enable, NULL) != 0;
}

/* ---- 制御レジスタ／FMOVEM --------------------------------------------- */

/* control register の転送順は FPCR、FPSR、FPIAR（アドレスの昇順） */
static const struct {
    uint8_t bit;
} control_order[] = {
    {MC68K_FPU_CONTROL_FPCR},
    {MC68K_FPU_CONTROL_FPSR},
    {MC68K_FPU_CONTROL_FPIAR},
};

void fpu_exec_control_in(struct mc68k_fpu *f) {
    mc68k_fpu_state_t *st = &f->state;
    const struct fpu_dialog *d = &f->dlg;
    unsigned idx = 0;
    size_t i;

    for (i = 0; i < sizeof(control_order) / sizeof(control_order[0]); i++) {
        uint8_t bit = control_order[i].bit;

        if (!(d->cmd.control_mask & bit))
            continue;
        if (bit == MC68K_FPU_CONTROL_FPCR)
            fpu_unpack_fpcr(st, d->buf[idx]);
        else if (bit == MC68K_FPU_CONTROL_FPSR)
            fpu_unpack_fpsr(st, d->buf[idx]);
        else
            st->fpiar = d->buf[idx];
        idx++;
    }
}

void fpu_prepare_control_out(struct mc68k_fpu *f) {
    const mc68k_fpu_state_t *st = &f->state;
    struct fpu_dialog *d = &f->dlg;
    unsigned idx = 0;
    size_t i;

    memset(d->buf, 0, sizeof(d->buf));
    for (i = 0; i < sizeof(control_order) / sizeof(control_order[0]); i++) {
        uint8_t bit = control_order[i].bit;

        if (!(d->cmd.control_mask & bit))
            continue;
        if (bit == MC68K_FPU_CONTROL_FPCR)
            d->buf[idx] = fpu_pack_fpcr(st);
        else if (bit == MC68K_FPU_CONTROL_FPSR)
            d->buf[idx] = fpu_pack_fpsr(st);
        else
            d->buf[idx] = st->fpiar;
        idx++;
    }
    d->total = (uint8_t)idx;
    d->cnt = 0;
}

/* FMOVEM の register list を走査順（MSB から）に FP 番号へ展開する。
 * predecrement は MSB=FP7、postincrement/control は MSB=FP0（[881UM] p.4-132） */
unsigned fpu_movem_registers(const struct fpu_dialog *d, uint8_t regs[8]) {
    unsigned n = 0;
    int bit;

    for (bit = 7; bit >= 0; bit--) {
        if (!(d->mask & (1u << bit)))
            continue;
        regs[n++] = (uint8_t)(d->cmd.predecrement ? bit : 7 - bit);
    }
    return n;
}

void fpu_exec_movem_in(struct mc68k_fpu *f) {
    const struct fpu_dialog *d = &f->dlg;
    uint8_t regs[8];
    unsigned n = fpu_movem_registers(d, regs);
    unsigned i;

    for (i = 0; i < n; i++)
        f->state.fp[regs[i]] = x_from_words(&d->buf[i * PACKED_WORDS]);
}

void fpu_prepare_movem_out(struct mc68k_fpu *f) {
    struct fpu_dialog *d = &f->dlg;
    uint8_t regs[8];
    unsigned n = fpu_movem_registers(d, regs);
    unsigned i;

    memset(d->buf, 0, sizeof(d->buf));
    for (i = 0; i < n; i++)
        x_to_words(f->state.fp[regs[i]], &d->buf[i * PACKED_WORDS]);
    d->total = (uint8_t)(n * PACKED_WORDS);
    d->cnt = 0;
}
