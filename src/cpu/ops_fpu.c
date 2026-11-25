/*
 * ops_fpu.c — MC68040/MC68060 内蔵 FPU の F-line 入口、実装済み命令の実行、state frame。
 *
 * cpID 1 の命令語を分類し、不正な F-line（format $0）、未実装 FP 命令（format $2）、
 * FPU 非搭載 040 の F-line（format $4）の例外フレームを生成する。実装済み命令は共通コアの
 * 値演算 API で実行し、例外の報告時期・書戻し・FPIAR をこの層が決める。分類と例外の根拠は
 * docs/mc68000-internals/reference/fpu.md。
 */
#include <string.h>

#include "cpu_internal.h"
#include "fpu/fpu_exec.h"
#include "fpu/fpu_internal.h"

enum {
    OPCODE_CPID_BASE = 0xF200, /* 1111 001x xxxxxxxx: cpID 1 */
    OPCODE_CPID_COUNT = 0x200,
    FPU040_EXC_CYCLES = 34,  /* op_line_f と同じ公称値（命令別 timing は範囲外） */
    FPU040_OP_CYCLES = 8,    /* 命令別 timing は範囲外。固定の公称値 */
    FPU040_FAULT_CYCLES = 8, /* バスエラー等でメモリアクセスが失敗したときの公称値 */
    EA_IMMEDIATE = 074,      /* mode 7, reg 4 */
    VEC_FLINE = 11,
    VEC_TRAPCC = 7,
    VEC_UNSUPPORTED_DATA = 55,
    VEC_FORMAT_ERROR = 14,
    FP_REG_COUNT = 8,
    LONG_BYTES = 4,
    EXT_BYTES = 12, /* FP レジスタ 1 本のメモリ表現（96 bit） */
};

/* 実装済みの opmode（拡張語 bits 6-0）。0x40 以降は強制精度命令で、base_opmode()
 * が基本命令に写す。 FMOD/FREM/FSCALE は未実装演算（format
 * $2）だが、宛先を読むので名前を付ける。 */
enum {
    OPMODE_MASK = 0x7F,
    OP_FMOVE = 0x00,
    OP_FINT = 0x01,   /* 060 のみ実装（040 では未実装演算） */
    OP_FINTRZ = 0x03, /* 同上 */
    OP_FSQRT = 0x04,
    OP_FABS = 0x18,
    OP_FNEG = 0x1A,
    OP_FDIV = 0x20,
    OP_FADD = 0x22,
    OP_FMUL = 0x23,
    OP_FSGLDIV = 0x24,
    OP_FSGLMUL = 0x27,
    OP_FSUB = 0x28,
    OP_FCMP = 0x38,
    OP_FTST = 0x3A,
    OP_FMOD = 0x21,
    OP_FREM = 0x25,
    OP_FSCALE = 0x26,
    OP_FORCED_PRECISION_MIN = 0x40,
};

/* state frame。先頭 long は version（byte 0）と本文のバイト数（byte
 * 1）。 */
enum {
    FRAME_VERSION = 0x41,
    FRAME_BUSY_SIZE = 0x60,
    FRAME_UNIMPL_SIZE = 0x30,
    FRAME_HEADER_BYTES = 4,
    FRAME_MAX_WORDS = (FRAME_HEADER_BYTES + FRAME_BUSY_SIZE) / 4,
    /* busy frame のオフセット。unimplemented frame は CMDREG3B 以降が 0x30 だけ前にずれる。 */
    FRAME_UNIMPL_SHIFT = 0x30,
    FRAME_FPIARCU = 0x28,
    FRAME_CMDREG3B = 0x34,
    FRAME_STAG = 0x3C,
    FRAME_CMDREG1B = 0x40,
    FRAME_DTAG = 0x44,
    FRAME_FLAGS = 0x48,
    FRAME_FPTEMP = 0x4C,
    FRAME_ETEMP = 0x58,
    /* long 内の位置は M68040FPSP の fpsp.h による。E_BYTE の
     * E1/E3、T_BYTE の TFLAG、CMDREG1B（上位語）。 */
    FRAME_TAG_SHIFT = 29,
    FRAME_CMDREG1B_SHIFT = 16,
    FRAME_FLAG_E1 = 0x04000000,
    FRAME_FLAG_E3 = 0x02000000,
    FRAME_FLAG_T = 0x00100000,
    /* E3 の WBTEMP（`$18` から。WBTS = bit31、WBTE[14-0] = bits 30-16、続く 2 long が
     * WBTM[63-0]）と、STAG の long の guard/round/SBIT（M66 は常に 0）、DTAG の long の
     * WBTE15、CMDREG3B（上位語）。位置は fpsp.h。 */
    FRAME_DIRTY = 0x08,
    FRAME_DIRTY_SHIFT = 16,
    FRAME_CU_SAVEPC_SHIFT = 24,
    FRAME_TAG_E15 = 0x10000000,
    /* NMNEXC（`$38` の byte 0）の nmn_unsup_bit と NMCEXC（`$39`）の
       nmc_unsup_bit（fpsp.h。推論 25）。 */
    FRAME_NMEXC = 0x38,
    FRAME_NMN_UNSUP = 0x02000000,
    FRAME_NMC_UNSUP = 0x00100000,
    FRAME_WBTEMP = 0x18,
    FRAME_WBTE_SHIFT = 16,
    FRAME_CMDREG3B_SHIFT = 16,
    FRAME_WBTM_GUARD = 0x02000000,
    FRAME_WBTM_ROUND = 0x01000000,
    FRAME_SBIT = 0x00800000,
    FRAME_WBTE15 = 0x00100000,
};

/* 指数の下位 15 bit（WBTE[14-0]）と WBTS（符号）。 */
enum { EXP15_MASK = 0x7FFF };
static const uint32_t FRAME_WBTS = UINT32_C(0x80000000);

/* STAG/DTAG（[040UM §9.8]）。 */
enum {
    TAG_NORMAL = 0,
    TAG_ZERO = 1,
    TAG_INF = 2,
    TAG_NAN = 3,
    TAG_EXT_DENORM = 4,
    TAG_SD_DENORM = 5
};

/* 無条件に報告する算術例外。整数宛ての FMOVE では SNAN/OPERR
 * も。 */
enum {
    NONMASKABLE_EXC = MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_UNFL,
    NONMASKABLE_INT_DEST_EXC = NONMASKABLE_EXC | MC68K_FPU_EXC_SNAN | MC68K_FPU_EXC_OPERR,
};

/* オペレーションワードの type（bits 8-6）。 */
enum {
    TYPE_GEN = 0,
    TYPE_COND = 1, /* FDBcc/FScc/FTRAPcc */
    TYPE_BCC_W = 2,
    TYPE_BCC_L = 3,
    TYPE_SAVE = 4,
    TYPE_RESTORE = 5,
};

/* 汎用命令の拡張語 bits 15-13（opclass）。 */
enum {
    OPCLASS_REG_REG = 0,
    OPCLASS_EA_TO_REG = 2,
    OPCLASS_REG_TO_EA = 3,
    OPCLASS_EA_TO_CTRL = 4,
    OPCLASS_CTRL_TO_EA = 5,
    OPCLASS_EA_TO_MULTI = 6,
    OPCLASS_MULTI_TO_EA = 7,
};

enum {
    FMT_L = 0,
    FMT_S = 1,
    FMT_X = 2,
    FMT_P = 3,
    FMT_W = 4,
    FMT_D = 5,
    FMT_B = 6,
    FMT_P_DYNAMIC = 7, /* opclass 011 の P{Dn}。040 では未対応データ型 */
    SRC_FMOVECR = 7,   /* opclass 010 の source specifier 7 = FMOVECR */
};

/* opmode（拡張語 bits 6-0）の分類。 */
typedef enum { OPMODE_ILLEGAL, OPMODE_UNIMPL, OPMODE_IMPL } opmode_class_t;

/* Table 9-10 の未実装演算に FLOG2 を加える（Table E-2: FPSP が実装する）。FMOVECR は opmode
 * でなく source specifier で判定する。強制精度命令（0x40 以降）の opmode は設計書 §3
 * の「推論」。 */
static opmode_class_t classify_opmode(unsigned opmode) {
    switch (opmode) {
    case 0x00:
    case 0x04:
    case 0x18:
    case 0x1A: /* FMOVE FSQRT FABS FNEG */
    case 0x20:
    case 0x22:
    case 0x23:
    case 0x24: /* FDIV FADD FMUL FSGLDIV */
    case 0x27:
    case 0x28:
    case 0x38:
    case 0x3A: /* FSGLMUL FSUB FCMP FTST */
    case 0x40:
    case 0x41:
    case 0x44:
    case 0x45: /* FSMOVE FSSQRT FDMOVE FDSQRT */
    case 0x58:
    case 0x5A:
    case 0x5C:
    case 0x5E: /* FSABS FSNEG FDABS FDNEG */
    case 0x60:
    case 0x62:
    case 0x63:
    case 0x64: /* FSDIV FSADD FSMUL FDDIV */
    case 0x66:
    case 0x67:
    case 0x68:
    case 0x6C: /* FDADD FDMUL FSSUB FDSUB */
        return OPMODE_IMPL;
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x06: /* FINT FSINH FINTRZ FLOGNP1 */
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0C: /* FETOXM1 FTANH FATAN FASIN */
    case 0x0D:
    case 0x0E:
    case 0x0F:
    case 0x10: /* FATANH FSIN FTAN FETOX */
    case 0x11:
    case 0x12:
    case 0x14:
    case 0x15: /* FTWOTOX FTENTOX FLOGN FLOG10 */
    case 0x16:
    case 0x19:
    case 0x1C:
    case 0x1D: /* FLOG2 FCOSH FACOS FCOS */
    case 0x1E:
    case 0x1F:
    case 0x21:
    case 0x25: /* FGETEXP FGETMAN FMOD FREM */
    case 0x26: /* FSCALE */
        return OPMODE_UNIMPL;
    default:
        return opmode >= 0x30 && opmode <= 0x37 ? OPMODE_UNIMPL /* FSINCOS */ : OPMODE_ILLEGAL;
    }
}

static bool format_allows_dn(unsigned fmt) {
    return fmt == FMT_L || fmt == FMT_S || fmt == FMT_W || fmt == FMT_B;
}

static unsigned popcount8(unsigned v) {
    unsigned n = 0;
    for (v &= 0xFFu; v; v >>= 1)
        n += v & 1u;
    return n;
}

/* FMOVEM の拡張語 bits 12-11 のうち、predecrement 形式（FPn→-(An)）を示す値。 */
static bool multi_mode_is_predecrement(unsigned mode_bits) {
    return mode_bits <= 1;
}

/* TYPE_COND の EA 欄が FDBcc（mode 1）/ FTRAPcc（mode 7、reg 2〜4）か。 */
static bool is_fdbcc(unsigned ea) {
    return (ea >> 3) == 1;
}

static bool is_ftrapcc(unsigned ea) {
    return (ea >> 3) == 7 && (ea & 7u) >= 2 && (ea & 7u) <= 4;
}

static fpu040_class_t classify_general(unsigned ea, uint16_t ext) {
    const unsigned mode = ea >> 3, opclass = ext >> 13, fmt = (ext >> 10) & 7u;
    const unsigned regsel = (ext >> 10) & 7u, multi_mode = (ext >> 11) & 3u;

    switch (opclass) {
    case OPCLASS_REG_REG:
        if (ea != 0)
            return FPU040_ILLEGAL;
        break;
    case OPCLASS_EA_TO_REG:
        if (fmt == SRC_FMOVECR)
            return ea == 0 ? FPU040_UNIMPL : FPU040_ILLEGAL;
        if (!ea_is_data(ea) || (mode == 0 && !format_allows_dn(fmt)))
            return FPU040_ILLEGAL;
        break;
    case OPCLASS_REG_TO_EA:
        if (!ea_is_data_alterable(ea) || (mode == 0 && !format_allows_dn(fmt)))
            return FPU040_ILLEGAL;
        return FPU040_IMPL;
    case OPCLASS_EA_TO_CTRL: /* 単一レジスタは -(An) も可（PRM FMOVE System Control Register）
                              */
        return ((mode == 0 && popcount8(regsel) <= 1) || (mode == 1 && regsel == 1) ||
                ea_is_control(ea) || mode == 3 || ea == EA_IMMEDIATE ||
                (mode == 4 && popcount8(regsel) == 1))
                   ? FPU040_IMPL
                   : FPU040_ILLEGAL;
    case OPCLASS_CTRL_TO_EA: /* 単一レジスタは (An)+ も可 */
        return ((mode == 0 && popcount8(regsel) <= 1) || (mode == 1 && regsel == 1) ||
                ea_is_control_alterable(ea) || mode == 4 ||
                (mode == 3 && popcount8(regsel) == 1))
                   ? FPU040_IMPL
                   : FPU040_ILLEGAL;
    case OPCLASS_EA_TO_MULTI:
        return (multi_mode >= 2 && (ea_is_control(ea) || mode == 3)) ? FPU040_IMPL
                                                                     : FPU040_ILLEGAL;
    case OPCLASS_MULTI_TO_EA:
        if (mode == 4)
            return multi_mode_is_predecrement(multi_mode) ? FPU040_IMPL : FPU040_ILLEGAL;
        return (multi_mode >= 2 && ea_is_control_alterable(ea)) ? FPU040_IMPL : FPU040_ILLEGAL;
    default:
        return FPU040_ILLEGAL;
    }

    switch (classify_opmode(ext & OPMODE_MASK)) {
    case OPMODE_IMPL:
        return FPU040_IMPL;
    case OPMODE_UNIMPL:
        return FPU040_UNIMPL;
    default:
        return FPU040_ILLEGAL;
    }
}

fpu040_class_t fpu040_classify(uint16_t opcode, uint16_t ext) {
    const unsigned ea = opcode & 077u, mode = ea >> 3;

    switch ((opcode >> 6) & 7u) {
    case TYPE_GEN:
        return classify_general(ea, ext);
    case TYPE_COND:
        if (is_fdbcc(ea) || is_ftrapcc(ea))
            return FPU040_IMPL;
        return ea_is_data_alterable((int)ea) ? FPU040_IMPL : FPU040_ILLEGAL; /* FScc */
    case TYPE_BCC_W:
    case TYPE_BCC_L:
        return FPU040_IMPL;
    case TYPE_SAVE:
        return (ea_is_control_alterable((int)ea) || mode == 4) ? FPU040_IMPL : FPU040_ILLEGAL;
    case TYPE_RESTORE:
        return (ea_is_control((int)ea) || mode == 3) ? FPU040_IMPL : FPU040_ILLEGAL;
    default:
        return FPU040_ILLEGAL;
    }
}

/* 外部形式（FMT_*）ごとのメモリ上のバイト数。添字 7 は opclass 011 の P{Dn}（12）。opclass 010
 * の FMOVECR は EA を持たないので引かない。 */
static const unsigned fmt_bytes[8] = {4, 4, 12, 12, 2, 8, 1, 12};

/* メモリオペランドのバイト数（(An)+/-(An) の更新幅と即値の長さ）。0 = EA を持たない形式。 */
static unsigned operand_bytes(const mc68k_state_t *cpu, uint16_t opcode, uint16_t ext) {
    const unsigned type = (opcode >> 6) & 7u, opclass = ext >> 13;

    if (type == TYPE_COND)
        return 1; /* FScc */
    if (type != TYPE_GEN)
        return 0; /* FSAVE/FRESTORE（FPU 非搭載 040 の format $4 は EA を持たない扱い） */
    switch (opclass) {
    case OPCLASS_EA_TO_REG:
        return ((ext >> 10) & 7u) == SRC_FMOVECR ? 0 : fmt_bytes[(ext >> 10) & 7u];
    case OPCLASS_REG_TO_EA:
        return fmt_bytes[(ext >> 10) & 7u];
    case OPCLASS_EA_TO_CTRL:
    case OPCLASS_CTRL_TO_EA:
        return 4u * popcount8((ext >> 10) & 7u);
    case OPCLASS_EA_TO_MULTI:
    case OPCLASS_MULTI_TO_EA:
        return EXT_BYTES * popcount8((ext & 0x0800u) ? (unsigned)cpu->d[(ext >> 4) & 7u] : ext);
    default:
        return 0;
    }
}

/* 実効アドレスを計算して返す。Dn/An/即値は 0。(An)+/-(An) は An
 * を更新する （FPU 非搭載 040 の呼び出し側は元へ戻す。設計書 推論 3）。拡張語のフェッチ失敗は
 * exec_state に残る。 */
static uint32_t calc_operand_ea(mc68k_state_t *cpu, unsigned ea, unsigned bytes) {
    const unsigned mode = ea >> 3, reg = ea & 7u;

    switch (mode) {
    case 0:
    case 1:
        return 0;
    case 2:
        return cpu->a[reg];
    case 3: {
        uint32_t addr = cpu->a[reg];
        cpu->a[reg] += (reg == 7 && bytes == 1) ? 2u : bytes;
        return addr;
    }
    case 4:
        cpu->a[reg] -= (reg == 7 && bytes == 1) ? 2u : bytes;
        return cpu->a[reg];
    default:
        break;
    }
    if (ea == EA_IMMEDIATE) { /* 即値: 拡張語を読み飛ばすだけ（EA = 0） */
        for (unsigned left = bytes < 2 ? 2 : bytes; left > 0 && cpu->exec_state == CPU_NORMAL;
             left -= 2)
            (void)cpu_fetch_word(cpu);
        return 0;
    }
    return ea_get_addr(cpu, (int)mode, (int)reg);
}

/* 拡張語のうち EA 以外のもの（FBcc の変位、FDBcc の変位、FTRAPcc のオペランド）を読み飛ばす。
 */
static void skip_trailing_words(mc68k_state_t *cpu, uint16_t opcode) {
    unsigned words = 0;
    const unsigned type = (opcode >> 6) & 7u, ea = opcode & 077u;

    if (type == TYPE_BCC_W)
        words = 1;
    else if (type == TYPE_BCC_L)
        words = 2;
    else if (type == TYPE_COND && is_fdbcc(ea))
        words = 1;
    else if (type == TYPE_COND && is_ftrapcc(ea))
        words = ea == 072 ? 1 : ea == 073 ? 2 : 0;
    while (words-- > 0 && cpu->exec_state == CPU_NORMAL)
        (void)cpu_fetch_word(cpu);
}

static int fpu040_fline(mc68k_state_t *cpu) {
    return cpu_dispatch_exception(cpu, cpu->instr_pc, VEC_FLINE, FPU040_EXC_CYCLES);
}

/* ---- 実装済み命令の実行 ------------------------------------------------ */

static mc68k_fp80_t x_from_words(const uint32_t *w) {
    mc68k_fp80_t v;
    v.sign_exp = (uint16_t)(w[0] >> 16);
    v.signif = ((uint64_t)w[1] << 32) | w[2];
    return v;
}

static void x_to_words(mc68k_fp80_t v, uint32_t *w) {
    w[0] = (uint32_t)v.sign_exp << 16;
    w[1] = (uint32_t)(v.signif >> 32);
    w[2] = (uint32_t)v.signif;
}

/* 未対応データ型: 拡張精度の denormal（指数 0 で仮数が非 0）と
 * unnormalized（指数が 1〜$7FFE で整数部ビットが 0。推論 5）。 */
static bool x_is_unsupported(mc68k_fp80_t v) {
    const uint16_t exp = v.sign_exp & 0x7FFFu;

    if (exp == 0x7FFFu)
        return false;
    if (exp == 0)
        return v.signif != 0;
    return (v.signif >> 63) == 0;
}

static bool operand_is_unsupported(unsigned fmt, const uint32_t *w) {
    switch (fmt) {
    case FMT_P:
        return true;
    case FMT_S:
        return ((w[0] >> 23) & 0xFFu) == 0 && (w[0] & 0x7FFFFFu) != 0;
    case FMT_D:
        return ((w[0] >> 20) & 0x7FFu) == 0 && ((uint64_t)(w[0] & 0xFFFFFu) << 32 | w[1]) != 0;
    case FMT_X:
        return x_is_unsupported(x_from_words(w));
    default:
        return false;
    }
}

/* 外部形式のオペランドを内部拡張精度にする。FMT_P
 * は呼ばれない（未対応データ型で先に報告する）。 */
static mc68k_fp80_t load_operand(unsigned fmt, const uint32_t *w, mc68k_fpu_state_t *st) {
    switch (fmt) {
    case FMT_B:
        return mc68k_fpu_from_b((int8_t)(w[0] >> 24));
    case FMT_W:
        return mc68k_fpu_from_w((int16_t)(w[0] >> 16));
    case FMT_L:
        return mc68k_fpu_from_l((int32_t)w[0]);
    case FMT_S:
        return mc68k_fpu_from_s(w[0], st);
    case FMT_D:
        return mc68k_fpu_from_d(((uint64_t)w[0] << 32) | w[1], st);
    default:
        return mc68k_fpu_from_x(x_from_words(w), st);
    }
}

static bool uses_destination(unsigned opmode) {
    switch (opmode) {
    case OP_FDIV:
    case OP_FADD:
    case OP_FMUL:
    case OP_FSGLDIV:
    case OP_FSGLMUL:
    case OP_FSUB:
    case OP_FCMP:
        return true;
    default:
        return false;
    }
}

/* 共通コアで 1 命令を実行する。FPn へ書き戻す結果があれば true。 */
static bool run_opmode(mc68k_fpu_state_t *st, unsigned opmode, mc68k_fp80_t src,
                       mc68k_fp80_t dst, mc68k_fp80_t *result) {
    switch (opmode) {
    case OP_FMOVE:
        *result = mc68k_fpu_move(src, st);
        fpu_set_cc(st, *result);
        return true;
    case OP_FINT:
        *result = mc68k_fpu_int(src, st);
        return true;
    case OP_FINTRZ:
        *result = mc68k_fpu_intrz(src, st);
        return true;
    case OP_FSQRT:
        *result = mc68k_fpu_sqrt(src, st);
        return true;
    case OP_FABS:
        *result = mc68k_fpu_abs(src, st);
        return true;
    case OP_FNEG:
        *result = mc68k_fpu_neg(src, st);
        return true;
    case OP_FDIV:
        *result = mc68k_fpu_div(dst, src, st);
        return true;
    case OP_FADD:
        *result = mc68k_fpu_add(dst, src, st);
        return true;
    case OP_FMUL:
        *result = mc68k_fpu_mul(dst, src, st);
        return true;
    case OP_FSGLDIV:
        *result = mc68k_fpu_sgldiv(dst, src, st);
        return true;
    case OP_FSGLMUL:
        *result = mc68k_fpu_sglmul(dst, src, st);
        return true;
    case OP_FSUB:
        *result = mc68k_fpu_sub(dst, src, st);
        return true;
    case OP_FCMP:
        mc68k_fpu_cmp(dst, src, st);
        return false;
    default:
        mc68k_fpu_tst(src, st);
        return false; /* OP_FTST */
    }
}

/* 報告する算術例外のベクタ（無ければ 0）。*bit に選んだ EXC ビットを返す。 */
static uint8_t select_exception(const mc68k_fpu_state_t *st, uint8_t nonmaskable,
                                uint8_t *bit) {
    return fpu_select_exception(st->fpsr_exc, (uint8_t)(st->fpcr_enable | nonmaskable), bit);
}

/* 報告する例外があっても宛先へ結果を書くのは INEX だけ（UM §9.7.7。ほかは宛先を変えない）。 */
static bool result_is_written(uint8_t vector, uint8_t bit) {
    return vector == 0 || bit == MC68K_FPU_EXC_INEX2 || bit == MC68K_FPU_EXC_INEX1;
}

static int fpu040_post_exception(mc68k_state_t *cpu, uint8_t vector, uint32_t ea) {
    return cpu_dispatch_fpu040_exception(cpu, FPU040_FRAME_POST, vector, cpu->pc, ea,
                                         cpu->instr_pc, FPU040_EXC_CYCLES);
}

/* pre-instruction の FP 例外は Table 8-4 の priority 2 で、trace（priority
 * 6）より先に処理される。命令は handler の RTE で再実行され、同時に保留された trace
 * は再実行のときに生じるので、ここでは報告しない （[040UM §8.3] 「the remaining exceptions are
 * regenerated when the current instruction restarts」）。 */
static int fpu040_pre_exception(mc68k_state_t *cpu, uint8_t vector) {
    const int cycles = cpu_dispatch_exception(cpu, cpu->instr_pc, vector, FPU040_EXC_CYCLES);
    if (cpu->exec_state == CPU_NORMAL)
        cpu->exc_suppress_trace = true;
    return cycles;
}

/* Dn の下位を外部形式の先頭語に置く（B は bits 31-24、W は bits 31-16）。 */
static void register_to_words(uint32_t value, unsigned fmt, uint32_t *w) {
    w[0] = fmt == FMT_B ? value << 24 : fmt == FMT_W ? value << 16 : value;
}

static void fetch_immediate(mc68k_state_t *cpu, unsigned bytes, uint32_t *w) {
    if (bytes <= 2) {
        const uint16_t v = cpu_fetch_word(cpu);
        w[0] = bytes == 1 ? (uint32_t)(v & 0xFFu) << 24 : (uint32_t)v << 16;
        return;
    }
    for (unsigned i = 0; i < bytes / LONG_BYTES && cpu->exec_state == CPU_NORMAL; i++) {
        const uint32_t hi = cpu_fetch_word(cpu);
        if (cpu->exec_state != CPU_NORMAL)
            return;
        w[i] = hi << 16 | cpu_fetch_word(cpu);
    }
}

static void read_memory_words(mc68k_state_t *cpu, uint32_t addr, unsigned bytes, uint32_t *w) {
    if (bytes == 1) {
        w[0] = (uint32_t)cpu_read_byte(cpu, addr) << 24;
    } else if (bytes == 2) {
        w[0] = (uint32_t)cpu_read_word(cpu, addr) << 16;
    } else {
        for (unsigned i = 0; i < bytes / LONG_BYTES && cpu->exec_state == CPU_NORMAL; i++)
            w[i] = cpu_read_long(cpu, addr + i * LONG_BYTES);
    }
}

static void write_memory_words(mc68k_state_t *cpu, uint32_t addr, unsigned bytes,
                               const uint32_t *w) {
    if (bytes == 1) {
        cpu_write_byte(cpu, addr, (uint8_t)(w[0] >> 24));
    } else if (bytes == 2) {
        cpu_write_word(cpu, addr, (uint16_t)(w[0] >> 16));
    } else {
        for (unsigned i = 0; i < bytes / LONG_BYTES && cpu->exec_state == CPU_NORMAL; i++)
            cpu_write_long(cpu, addr + i * LONG_BYTES, w[i]);
    }
}

/* ソースオペランド（opclass 010）を words に読む。*ea_value は EA（Dn/即値は 0）。 */
static bool load_source_words(mc68k_state_t *cpu, uint16_t opcode, unsigned fmt, uint32_t *w,
                              uint32_t *ea_value) {
    const unsigned ea = opcode & 077u;

    *ea_value = 0;
    if ((ea >> 3) == 0) {
        register_to_words(cpu->d[ea & 7u], fmt, w);
    } else if (ea == EA_IMMEDIATE) {
        fetch_immediate(cpu, fmt_bytes[fmt], w);
    } else {
        *ea_value = calc_operand_ea(cpu, ea, fmt_bytes[fmt]);
        if (cpu->exec_state == CPU_NORMAL)
            read_memory_words(cpu, *ea_value, fmt_bytes[fmt], w);
    }
    return cpu->exec_state == CPU_NORMAL;
}

static uint8_t x_tag(mc68k_fp80_t v) {
    if ((v.sign_exp & 0x7FFFu) == 0x7FFFu)
        return (v.signif & FPU_SIG_FRAC) == 0 ? TAG_INF : TAG_NAN;
    if (x_is_unsupported(v))
        return TAG_EXT_DENORM;
    return v.signif == 0 ? TAG_ZERO : TAG_NORMAL;
}

/* FSQRT は CMDREG1B の opmode が $4 → $5 に写る（[040UM §9.8 CMDREG1B]）。opclass 011 の bits
 * 6-0 は opmode でなく k-factor なので、写すのは opclass 000/010 だけ。 */
static uint16_t cmdreg1b_of(uint16_t ext) {
    const unsigned opclass = ext >> 13;
    return (opclass == OPCLASS_REG_REG || opclass == OPCLASS_EA_TO_REG) &&
                   (ext & OPMODE_MASK) == OP_FSQRT
               ? (uint16_t)((ext & ~OPMODE_MASK) | 0x05u)
               : ext;
}

/* 例外状態を作る。オペランドは続けて capture_* で入れる。 */
static mc68k_fpu040_exc_t *begin_exception(mc68k_fpu040_t *fpu, uint8_t vector, bool unimpl,
                                           bool t, uint16_t ext) {
    mc68k_fpu040_exc_t *e = &fpu->exc;

    memset(e, 0, sizeof(*e));
    e->valid = true;
    e->vector = vector;
    e->unimpl = unimpl;
    e->e1 = true; /* unimplemented frame の E1 は packed ソースで、exec_unimplemented()
                     が上書きする */
    e->t = t;
    e->cmdreg1b = cmdreg1b_of(ext);
    return e;
}

/* 外部形式のソースを ETEMP に入れる。packed は ETM[63-0] に下位 64 bit、FPTM[31-0] に上位 32
 * bit （[040UM §9.6.2]）。single/double の denormal は図 9-9 の形（指数 0、仮数を左詰め、整数部
 * 0）。 packed なら true（FPTEMP が埋まるので宛先は入れない）。 */
static bool capture_source(mc68k_fpu040_exc_t *e, unsigned fmt, const uint32_t *w) {
    if (fmt == FMT_P) {
        e->etemp.signif = ((uint64_t)w[1] << 32) | w[2];
        e->fptemp.signif = w[0];
        return true;
    }
    if (fmt == FMT_X) {
        e->etemp = x_from_words(w);
        e->stag = x_tag(e->etemp);
    } else if (operand_is_unsupported(fmt, w)) { /* single/double の denormal */
        const bool single = fmt == FMT_S;
        const uint64_t frac =
            single ? w[0] & 0x7FFFFFu : ((uint64_t)(w[0] & 0xFFFFFu) << 32) | w[1];
        e->etemp.sign_exp = (uint16_t)((w[0] >> 31) << 15);
        e->etemp.signif = frac << (single ? 40 : 11);
        e->stag = TAG_SD_DENORM;
    } else {
        mc68k_fpu_state_t scratch; /* 変換が FPSR を汚さないように */
        mc68k_fpu_reset(&scratch);
        e->etemp = load_operand(fmt, w, &scratch);
        e->stag = x_tag(e->etemp);
    }
    return false;
}

static void capture_register_source(mc68k_fpu040_exc_t *e, mc68k_fp80_t v) {
    e->etemp = v;
    e->stag = x_tag(v);
}

static void capture_destination(mc68k_fpu040_exc_t *e, mc68k_fp80_t v) {
    e->fptemp = v;
    e->dtag = x_tag(v);
}

/* UNFL/INEX の E1 frame は FPTEMP に丸め前の中間結果を持つ（[040UM §9.8 Table 9-16]。FMOVE の
 * レジスタ宛てと FMOVE out）。FMOVE の丸め前の値はソースそのもの。FABS/FNEG は共通コアが
 * UNFL/INEX2 を 検出しないので、ここには来ない。 */
static bool reports_unrounded_result(uint8_t bit) {
    return bit == MC68K_FPU_EXC_UNFL || bit == MC68K_FPU_EXC_INEX2 ||
           bit == MC68K_FPU_EXC_INEX1;
}

/* E3 frame になる命令か（[040UM] Table 9-16 の FADD/FSUB/FMUL/FDIV/FSQRT と、FPSP が E3
 * として扱う FSGLMUL/FSGLDIV。強制精度命令は base_opmode() で基本命令に写った後の値。推論
 * 17）。 */
static bool is_e3_opmode(unsigned opmode) {
    switch (opmode) {
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV:
    case OP_FSQRT:
    case OP_FSGLMUL:
    case OP_FSGLDIV:
        return true;
    default:
        return false;
    }
}

static bool is_e3_exception(uint8_t bit) {
    return bit == MC68K_FPU_EXC_OVFL || bit == MC68K_FPU_EXC_UNFL || bit == MC68K_FPU_EXC_INEX2;
}

/* CMDREG1B → CMDREG3B（推論 19）。bits 9-6 と 1-0 はそのまま、CMD bit 2 を bit 5 へ、CMD bits
 * 5-3 を bits 4-2 へ。 */
static uint16_t cmdreg3b_of(uint16_t ext) {
    const uint16_t cmd = cmdreg1b_of(ext);
    return (uint16_t)((cmd & 0x3C3u) | (((cmd >> 2) & 1u) << 5) | (((cmd >> 3) & 7u) << 2));
}

static mc68k_fp80_t fp_from_unrounded(const mc68k_fpu_unrounded_t *u) {
    mc68k_fp80_t v;

    v.sign_exp = (uint16_t)((u->sign ? 0x8000u : 0u) | ((uint32_t)u->exp & EXP15_MASK));
    v.signif = u->sig;
    return v;
}

/* E3 frame を作る（Table 9-16）。OVFL は仮数を丸めた中間結果・WBTE15 = 0、UNFL/INEX2
 * は丸め前の符号・指数・ 64 bit 仮数と guard/round/sticky（UNFL の WBTE15 = 1、INEX2 は 0）。E1
 * は立てず、T は 0（推論 18〜20）。 */
static void capture_e3(mc68k_fpu040_exc_t *e, uint8_t bit, const mc68k_fpu_unrounded_t *u,
                       uint16_t ext) {
    mc68k_fpu_unrounded_t rounded;

    e->e1 = false;
    e->e3 = true;
    e->cmdreg3b = cmdreg3b_of(ext);
    if (bit == MC68K_FPU_EXC_OVFL) {
        mc68k_fpu_unrounded_round_mantissa(u, &rounded);
        e->wbtemp = fp_from_unrounded(&rounded);
        return;
    }
    e->wbtemp = fp_from_unrounded(u);
    e->wbte15 = bit == MC68K_FPU_EXC_UNFL;
    e->wbtm_guard = (u->sig_extra >> 63) != 0;
    e->wbtm_round = ((u->sig_extra >> 62) & 1u) != 0;
    e->wbtm_sticky = (u->sig_extra & ~(UINT64_C(3) << 62)) != 0;
}

/* FMOVE out の OVFL の E1 は、仮数を丸めた中間結果を ETEMP に置き、UM どおり FPTEMP にも
 * 置く（推論 21）。STAG は normalized。 */
static void capture_overflow_e1(mc68k_fpu040_exc_t *e, const mc68k_fpu_unrounded_t *u) {
    mc68k_fpu_unrounded_t rounded;

    mc68k_fpu_unrounded_round_mantissa(u, &rounded);
    e->etemp = e->fptemp = fp_from_unrounded(&rounded);
    e->stag = TAG_NORMAL;
}

/* 実装済みの強制精度命令は基本命令と精度に分ける。S は single、D は double（opmode bit 2）。 */
static unsigned base_opmode(unsigned opmode) {
    if (opmode < OP_FORCED_PRECISION_MIN)
        return opmode;
    switch (opmode & ~0x04u) {
    case 0x40:
        return OP_FMOVE;
    case 0x41:
        return OP_FSQRT;
    case 0x58:
        return OP_FABS;
    case 0x5A:
        return OP_FNEG;
    case 0x60:
        return OP_FDIV;
    case 0x62:
        return OP_FADD;
    case 0x63:
        return OP_FMUL;
    default:
        return OP_FSUB; /* 0x68 */
    }
}

static bool is_dyadic_opmode(unsigned opmode) {
    return uses_destination(opmode) || opmode == OP_FMOD || opmode == OP_FREM ||
           opmode == OP_FSCALE;
}

/* 解決済みのソースと宛先の値で 1 命令（opclass 000/010）を実行し、結果を FPn
 * へ書く。例外は保留に積む（報告は 次の FP 命令。推論 7）。FRESTORE の再実行（推論
 * 10）からも使う。 */
static void run_register_operation(mc68k_fpu040_t *fpu, uint16_t ext, mc68k_fp80_t src,
                                   mc68k_fp80_t dst_value) {
    mc68k_fpu_state_t *st = &fpu->state;
    const unsigned raw_opmode = ext & OPMODE_MASK, opmode = base_opmode(raw_opmode);
    const unsigned dst = (ext >> 7) & 7u;
    mc68k_fp80_t result = {0, 0};
    const uint8_t saved_prec = st->fpcr_round_prec;

    if (raw_opmode >= OP_FORCED_PRECISION_MIN) /* 強制精度命令は FPCR の PREC を使わない */
        st->fpcr_round_prec =
            (raw_opmode & 0x04u) ? MC68K_FPU_PREC_DOUBLE : MC68K_FPU_PREC_SINGLE;
    mc68k_fpu_unrounded_reset();
    const bool writes = run_opmode(st, opmode, src, dst_value, &result);
    st->fpcr_round_prec = saved_prec;
    uint8_t bit = 0;
    const uint8_t vector = select_exception(st, NONMASKABLE_EXC, &bit);
    if (vector) {
        mc68k_fpu040_exc_t *e = begin_exception(fpu, vector, false, false, ext);
        mc68k_fpu_unrounded_t u;
        const bool have_u = mc68k_fpu_unrounded_get(&u);
        capture_register_source(e, src);
        if (uses_destination(opmode))
            capture_destination(e, dst_value);
        else if (reports_unrounded_result(bit) && opmode == OP_FMOVE)
            capture_destination(e, src);
        if (have_u && is_e3_opmode(opmode) && is_e3_exception(bit))
            capture_e3(e, bit, &u, ext);
    }
    if (writes && result_is_written(vector, bit))
        st->fp[dst] = result;
}

/* opclass 000/010: FMOVE/算術/FCMP/FTST（宛先は FPn）。 */
static int exec_to_register(mc68k_state_t *cpu, uint16_t opcode, uint16_t ext) {
    mc68k_fpu040_t *fpu = cpu->fpu040;
    mc68k_fpu_state_t *st = &fpu->state;
    const unsigned opclass = ext >> 13, opmode = base_opmode(ext & OPMODE_MASK);
    const unsigned dst = (ext >> 7) & 7u,
                   fmt = opclass == OPCLASS_REG_REG ? FMT_X : (ext >> 10) & 7u;
    uint32_t w[3] = {0, 0, 0}, ea_value;
    mc68k_fp80_t src;
    bool unsupported;

    if (opclass == OPCLASS_REG_REG) {
        src = st->fp[(ext >> 10) & 7u];
        unsupported = x_is_unsupported(src);
    } else {
        if (!load_source_words(cpu, opcode, fmt, w, &ea_value))
            return FPU040_FAULT_CYCLES;
        unsupported = operand_is_unsupported(fmt, w);
    }
    if (!unsupported && uses_destination(opmode))
        unsupported = x_is_unsupported(st->fp[dst]);
    if (unsupported) { /* 命令は受理済み。算術例外と同じく次の FP 命令の開始で報告する（推論 7）
                        */
        mc68k_fpu040_exc_t *e = begin_exception(fpu, VEC_UNSUPPORTED_DATA, false, false, ext);
        const bool packed = opclass == OPCLASS_REG_REG
                                ? (capture_register_source(e, src), false)
                                : capture_source(e, fmt, w);
        if (uses_destination(opmode) && !packed)
            capture_destination(e, st->fp[dst]);
        return FPU040_OP_CYCLES;
    }

    st->fpsr_exc = 0;
    if (opclass != OPCLASS_REG_REG)
        src = load_operand(fmt, w, st);
    run_register_operation(fpu, ext, src, st->fp[dst]);
    return FPU040_OP_CYCLES;
}

/* 整数宛ての FMOVE で、変換結果がその形式の最小値か（040 の誤った OPERR 条件。推論 6）。 */
static bool int_result_is_minimum(unsigned fmt, const uint32_t *w) {
    return fmt == FMT_B   ? (w[0] >> 24) == 0x80u
           : fmt == FMT_W ? (w[0] >> 16) == 0x8000u
                          : w[0] == 0x80000000u;
}

static void convert_to_words(unsigned fmt, mc68k_fp80_t v, mc68k_fpu_state_t *st, uint32_t *w) {
    switch (fmt) {
    case FMT_B:
        w[0] = (uint32_t)(uint8_t)mc68k_fpu_to_b(v, st) << 24;
        break;
    case FMT_W:
        w[0] = (uint32_t)(uint16_t)mc68k_fpu_to_w(v, st) << 16;
        break;
    case FMT_L:
        w[0] = (uint32_t)mc68k_fpu_to_l(v, st);
        break;
    case FMT_S:
        w[0] = mc68k_fpu_to_s(v, st);
        break;
    case FMT_D: {
        const uint64_t bits = mc68k_fpu_to_d(v, st);
        w[0] = (uint32_t)(bits >> 32);
        w[1] = (uint32_t)bits;
        break;
    }
    default:
        x_to_words(mc68k_fpu_to_x(v, st), w);
        break;
    }
}

/* opclass 011: FMOVE FPn,<ea>。例外は常に実行後（format $3）で、報告するときは宛先を変えない。
 */
static int exec_to_ea(mc68k_state_t *cpu, uint16_t opcode, uint16_t ext) {
    mc68k_fpu_state_t *st = &cpu->fpu040->state;
    const unsigned ea = opcode & 077u, mode = ea >> 3, fmt = (ext >> 10) & 7u;
    const mc68k_fp80_t v = st->fp[(ext >> 7) & 7u];
    const bool int_dest = fmt == FMT_L || fmt == FMT_W || fmt == FMT_B;
    uint32_t addr = 0, w[3] = {0, 0, 0};

    if (mode != 0)
        addr = calc_operand_ea(cpu, ea, fmt_bytes[fmt]);
    if (cpu->exec_state != CPU_NORMAL) /* 拡張語のフェッチ失敗。(An)+/-(An) は更新していない */
        return FPU040_FAULT_CYCLES;
    if (fmt == FMT_P || fmt == FMT_P_DYNAMIC || x_is_unsupported(v)) {
        capture_register_source(
            begin_exception(cpu->fpu040, VEC_UNSUPPORTED_DATA, false, true, ext), v);
        return fpu040_post_exception(cpu, VEC_UNSUPPORTED_DATA, addr);
    }

    st->fpsr_exc = 0;
    mc68k_fpu_unrounded_reset();
    convert_to_words(fmt, v, st, w);
    if (int_dest && int_result_is_minimum(fmt, w) && !(st->fpsr_exc & MC68K_FPU_EXC_OPERR))
        fpu_commit_exc(st, MC68K_FPU_EXC_OPERR);
    uint8_t bit = 0;
    const uint8_t vector =
        select_exception(st, int_dest ? NONMASKABLE_INT_DEST_EXC : NONMASKABLE_EXC, &bit);
    if (result_is_written(vector, bit)) {
        if (mode == 0)
            cpu->d[ea & 7u] = fmt == FMT_B   ? (cpu->d[ea & 7u] & 0xFFFFFF00u) | (w[0] >> 24)
                              : fmt == FMT_W ? (cpu->d[ea & 7u] & 0xFFFF0000u) | (w[0] >> 16)
                                             : w[0];
        else
            write_memory_words(cpu, addr, fmt_bytes[fmt], w);
        if (cpu->exec_state != CPU_NORMAL)
            return FPU040_FAULT_CYCLES;
    }
    if (!vector)
        return FPU040_OP_CYCLES;
    /* 保留ストア（WB2）を残したまま post 例外を起こすと、再実行で例外が保留済みとして扱われて
     * ストアが行われない。例外の記録より前に保留を捨て、命令全体を再実行する。 */
    if (cpu->wb2.valid) {
        cpu_040_wb2_escalate(cpu);
        return FPU040_FAULT_CYCLES;
    }
    mc68k_fpu040_exc_t *e = begin_exception(cpu->fpu040, vector, false, true, ext);
    capture_register_source(e, v);
    if (reports_unrounded_result(bit))
        capture_destination(e, v); /* FMOVE out の丸め前の値はレジスタの値そのもの */
    mc68k_fpu_unrounded_t u;
    if (bit == MC68K_FPU_EXC_OVFL && mc68k_fpu_unrounded_get(&u))
        capture_overflow_e1(e, &u);
    return fpu040_post_exception(cpu, vector, addr);
}

/* opclass 100/101: FMOVE <ea>,FPcr / FMOVE FPcr,<ea>。例外を起こさず FPIAR も変えない。 */
static int exec_control(mc68k_state_t *cpu, uint16_t opcode, uint16_t ext) {
    mc68k_fpu_state_t *st = &cpu->fpu040->state;
    const unsigned ea = opcode & 077u, mode = ea >> 3, regsel = (ext >> 10) & 7u;
    const bool to_fpu = (ext >> 13) == OPCLASS_EA_TO_CTRL;
    const unsigned count = popcount8(regsel), bytes = count * LONG_BYTES;
    uint32_t w[3] = {0, 0, 0};
    uint32_t addr = 0;

    if (to_fpu) {
        if (mode == 0) {
            w[0] = cpu->d[ea & 7u];
        } else if (mode == 1) {
            w[0] = cpu->a[ea & 7u];
        } else if (ea == EA_IMMEDIATE) {
            if (bytes > 0)
                fetch_immediate(cpu, bytes, w);
        } else {
            addr = calc_operand_ea(cpu, ea, bytes);
            if (cpu->exec_state == CPU_NORMAL)
                read_memory_words(cpu, addr, bytes, w);
        }
        if (cpu->exec_state != CPU_NORMAL)
            return FPU040_FAULT_CYCLES;
    }
    /* 転送順は FPCR、FPSR、FPIAR（アドレスの昇順）。regsel のビットは 4/2/1 がその順。 */
    static const unsigned order[3] = {MC68K_FPU_CONTROL_FPCR, MC68K_FPU_CONTROL_FPSR,
                                      MC68K_FPU_CONTROL_FPIAR};
    unsigned idx = 0;
    for (unsigned i = 0; i < 3; i++) {
        if (!(regsel & order[i]))
            continue;
        if (to_fpu) {
            if (order[i] == MC68K_FPU_CONTROL_FPCR)
                fpu_unpack_fpcr(st, w[idx]);
            else if (order[i] == MC68K_FPU_CONTROL_FPSR)
                fpu_unpack_fpsr(st, w[idx]);
            else
                st->fpiar = w[idx];
        } else {
            w[idx] = order[i] == MC68K_FPU_CONTROL_FPCR   ? fpu_pack_fpcr(st)
                     : order[i] == MC68K_FPU_CONTROL_FPSR ? fpu_pack_fpsr(st)
                                                          : st->fpiar;
        }
        idx++;
    }
    if (!to_fpu && count > 0) {
        if (mode == 0) {
            cpu->d[ea & 7u] = w[0];
        } else if (mode == 1) {
            cpu->a[ea & 7u] = w[0];
        } else {
            addr = calc_operand_ea(cpu, ea, bytes);
            if (cpu->exec_state == CPU_NORMAL)
                write_memory_words(cpu, addr, bytes, w);
            if (cpu->exec_state != CPU_NORMAL)
                return FPU040_FAULT_CYCLES;
        }
    }
    return FPU040_OP_CYCLES;
}

/* opclass 110/111: FMOVEM <ea>,<list> / FMOVEM <list>,<ea>。例外を起こさず FPIAR も変えない。
 * メモリでは、選んだレジスタのうち番号の小さいものが低いアドレスに並ぶ（predecrement
 * でも同じ）。 */
static int exec_movem(mc68k_state_t *cpu, uint16_t opcode, uint16_t ext) {
    mc68k_fpu_state_t *st = &cpu->fpu040->state;
    const unsigned ea = opcode & 077u;
    const bool to_fpu = (ext >> 13) == OPCLASS_EA_TO_MULTI;
    const bool predecrement = !to_fpu && multi_mode_is_predecrement((ext >> 11) & 3u);
    const unsigned list =
        (ext & 0x0800u) ? (unsigned)(cpu->d[(ext >> 4) & 7u] & 0xFFu) : (ext & 0xFFu);
    const unsigned bytes = EXT_BYTES * popcount8(list);
    uint32_t words[FP_REG_COUNT * 3] = {0};
    unsigned regs[FP_REG_COUNT], n = 0;

    /* リストの bit: predecrement は bit r が FPr、それ以外は bit 7-r が FPr。 */
    for (unsigned r = 0; r < FP_REG_COUNT; r++)
        if (list & (1u << (predecrement ? r : FP_REG_COUNT - 1 - r)))
            regs[n++] = r;
    const uint32_t addr = calc_operand_ea(cpu, ea, bytes);
    if (cpu->exec_state != CPU_NORMAL) /* 拡張語のフェッチ失敗。(An)+/-(An) は更新していない */
        return FPU040_FAULT_CYCLES;
    if (to_fpu) {
        read_memory_words(cpu, addr, bytes, words);
    } else {
        for (unsigned i = 0; i < n; i++)
            x_to_words(st->fp[regs[i]], &words[i * 3]);
        write_memory_words(cpu, addr, bytes, words);
    }
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    if (to_fpu)
        for (unsigned i = 0; i < n; i++)
            st->fp[regs[i]] = x_from_words(&words[i * 3]);
    return FPU040_OP_CYCLES;
}

/* FBcc/FDBcc/FScc/FTRAPcc の共通部。BSUN の判定まで行い、条件の真偽を *tf に返す。
 * 戻り値 true は BSUN の例外を報告する（pre-instruction。評価前に取る。UM §9.7.1）。 */
static bool evaluate_condition(mc68k_state_t *cpu, uint8_t predicate, bool *tf) {
    mc68k_fpu_state_t *st = &cpu->fpu040->state;
    bool bsun = false;

    st->fpsr_exc = 0;
    fpu_eval_predicate(st, predicate, tf, &bsun);
    if (!bsun)
        return false;
    fpu_commit_exc(st, MC68K_FPU_EXC_BSUN);
    return (st->fpcr_enable & MC68K_FPU_EXC_BSUN) != 0;
}

/* 変位（word または long）を読み、変位の先頭アドレスを基点にした分岐先を返す。 */
static uint32_t fetch_branch_target(mc68k_state_t *cpu, bool long_displacement) {
    const uint32_t base = cpu->pc;
    uint32_t disp;

    if (long_displacement) {
        disp = (uint32_t)cpu_fetch_word(cpu) << 16;
        if (cpu->exec_state == CPU_NORMAL)
            disp |= cpu_fetch_word(cpu);
    } else {
        disp = (uint32_t)(int32_t)(int16_t)cpu_fetch_word(cpu);
    }
    return base + disp;
}

static int exec_conditional(mc68k_state_t *cpu, uint16_t opcode, uint16_t ext) {
    const unsigned type = (opcode >> 6) & 7u, ea = opcode & 077u, mode = ea >> 3;
    const bool is_bcc = type == TYPE_BCC_W || type == TYPE_BCC_L;
    bool tf = false;

    if (evaluate_condition(cpu, (uint8_t)((is_bcc ? opcode : ext) & FPU_PREDICATE_MASK), &tf)) {
        begin_exception(cpu->fpu040, FPU_VECTOR_BSUN, false, false, 0);
        return fpu040_pre_exception(cpu, FPU_VECTOR_BSUN);
    }
    if (is_bcc || is_fdbcc(ea)) { /* FBcc / FDBcc */
        const uint32_t target = fetch_branch_target(cpu, type == TYPE_BCC_L);
        if (cpu->exec_state != CPU_NORMAL)
            return FPU040_FAULT_CYCLES;
        if (is_bcc ? tf : !tf) {
            if (!is_bcc) {
                const unsigned reg = ea & 7u;
                const uint16_t counter = (uint16_t)((cpu->d[reg] & 0xFFFFu) - 1u);
                cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000u) | counter;
                if (counter == 0xFFFFu)
                    return FPU040_OP_CYCLES;
            }
            cpu->pc = target;
            cpu->flow_changed = true;
        }
        return FPU040_OP_CYCLES;
    }
    if (is_ftrapcc(ea)) {
        skip_trailing_words(cpu, opcode);
        if (cpu->exec_state != CPU_NORMAL)
            return FPU040_FAULT_CYCLES;
        if (!tf)
            return FPU040_OP_CYCLES;
        cpu->flow_changed = true;
        const int cycles = cpu_dispatch_exception(cpu, cpu->pc, VEC_TRAPCC, FPU040_EXC_CYCLES);
        if (cpu->exec_state ==
            CPU_NORMAL) /* level 3 の例外と同時のトレースは報告しない（[040UM §8.3]） */
            cpu->exc_suppress_trace = true;
        return cycles;
    }
    /* FScc */
    const uint8_t value = tf ? 0xFFu : 0x00u;
    if (mode == 0) {
        cpu->d[ea & 7u] = (cpu->d[ea & 7u] & 0xFFFFFF00u) | value;
        return FPU040_OP_CYCLES;
    }
    const uint32_t addr = calc_operand_ea(cpu, ea, 1);
    if (cpu->exec_state == CPU_NORMAL)
        cpu_write_byte(cpu, addr, value);
    return cpu->exec_state == CPU_NORMAL ? FPU040_OP_CYCLES : FPU040_FAULT_CYCLES;
}

/* FSAVE の frame を作る。戻り値は全体のバイト数。 */
static unsigned build_frame(const mc68k_fpu040_t *fpu, uint32_t *w) {
    const mc68k_fpu040_exc_t *e = &fpu->exc;

    memset(w, 0, FRAME_MAX_WORDS * sizeof(uint32_t));
    if (!e->valid) {
        w[0] =
            fpu->idle ? (uint32_t)FRAME_VERSION << 24 : 0; /* idle: version のみ、null: 全 0 */
        return FRAME_HEADER_BYTES;
    }
    const unsigned size = e->unimpl ? FRAME_UNIMPL_SIZE : FRAME_BUSY_SIZE;
    const unsigned shift = e->unimpl ? FRAME_UNIMPL_SHIFT : 0;

    w[0] = (uint32_t)FRAME_VERSION << 24 | (uint32_t)size << 16;
    if (!e->unimpl)
        w[FRAME_FPIARCU / 4] = fpu->state.fpiar;
    w[(FRAME_STAG - shift) / 4] = (uint32_t)e->stag << FRAME_TAG_SHIFT;
    w[(FRAME_CMDREG1B - shift) / 4] = (uint32_t)e->cmdreg1b << FRAME_CMDREG1B_SHIFT;
    w[(FRAME_DTAG - shift) / 4] = (uint32_t)e->dtag << FRAME_TAG_SHIFT;
    w[(FRAME_FLAGS - shift) / 4] =
        (e->e1 ? FRAME_FLAG_E1 : 0) | (e->e3 ? FRAME_FLAG_E3 : 0) | (e->t ? FRAME_FLAG_T : 0);
    if (e->e3) {
        w[(FRAME_CMDREG3B - shift) / 4] = (uint32_t)e->cmdreg3b << FRAME_CMDREG3B_SHIFT;
        /* 書戻しが残っている宛先の dirty bit（bit16 = FP0。bugfix.sa。推論 23） */
        w[FRAME_DIRTY / 4] |= UINT32_C(1) << (FRAME_DIRTY_SHIFT + ((e->cmdreg3b >> 7) & 7u));
        w[FRAME_WBTEMP / 4] = (e->wbtemp.sign_exp & 0x8000u ? FRAME_WBTS : 0u) |
                              (uint32_t)(e->wbtemp.sign_exp & EXP15_MASK) << FRAME_WBTE_SHIFT;
        w[FRAME_WBTEMP / 4 + 1] = (uint32_t)(e->wbtemp.signif >> 32);
        w[FRAME_WBTEMP / 4 + 2] = (uint32_t)e->wbtemp.signif;
        w[(FRAME_STAG - shift) / 4] |= (e->wbtm_guard ? FRAME_WBTM_GUARD : 0u) |
                                       (e->wbtm_round ? FRAME_WBTM_ROUND : 0u) |
                                       (e->wbtm_sticky ? FRAME_SBIT : 0u);
        w[(FRAME_DTAG - shift) / 4] |= e->wbte15 ? FRAME_WBTE15 : 0u;
    }
    if (e->vector == VEC_UNSUPPORTED_DATA)
        w[FRAME_NMEXC / 4] |= FRAME_NMN_UNSUP;
    x_to_words(e->fptemp, &w[(FRAME_FPTEMP - shift) / 4]);
    x_to_words(e->etemp, &w[(FRAME_ETEMP - shift) / 4]);
    return FRAME_HEADER_BYTES + size;
}

static int exec_save(mc68k_state_t *cpu, uint16_t opcode) {
    mc68k_fpu040_t *fpu = cpu->fpu040;
    const unsigned ea = opcode & 077u;
    uint32_t w[FRAME_MAX_WORDS];

    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    const unsigned bytes = build_frame(fpu, w);
    const uint32_t addr = calc_operand_ea(cpu, ea, bytes);
    if (cpu->exec_state == CPU_NORMAL)
        write_memory_words(cpu, addr, bytes, w);
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    memset(&fpu->exc, 0, sizeof(fpu->exc)); /* FSAVE が例外状態を消す */
    return FPU040_OP_CYCLES;
}

/* frame の例外状態が未対応データ型か。frame の NMNEXC の unsup bit（FSAVE が vector 55
 * のときに立てる。FPSP は 処理済みの frame で `clr.w NMNEXC` する）で決める。タグ（denormal）や
 * packed のソース形式では決めない: FPSP が frestore する frame は、タグが denormal のまま UNFL
 * などの例外になることがある（推論 25）。 */
static bool frame_is_unsupported(const uint32_t *w) {
    return (w[FRAME_NMEXC / 4] & (FRAME_NMN_UNSUP | FRAME_NMC_UNSUP)) != 0;
}

/* busy frame の例外状態を戻す（E1/E3/T が立つ frame。推論 10）。frame に vector
 * は無いので、未対応データ型は NMNEXC の unsup bit から、算術例外は FRESTORE 時の FPSR の EXC
 * と FPCR の ENABLE（nonmaskable を含む）から決める（FPSP は frestore の前に FPSR/FPCR
 * を戻す）。決められなければ（EXC
 * が立っていない等）何も戻さない。宛先は書かない。次に例外を報告できる FP 命令が、元と同じ
 * vector で pre-instruction（format $0）として報告する（元が T = 1 の post-instruction でも）。
 * 再度の FSAVE は同じ frame になる。force_unsupported:
 * 完了の再実行でオペランドが未対応データ型だった場合。 */
static void restore_exception_state(mc68k_fpu040_t *fpu, const uint32_t *w,
                                    bool force_unsupported) {
    const uint32_t flags = w[FRAME_FLAGS / 4];
    const uint16_t cmdreg1b = (uint16_t)(w[FRAME_CMDREG1B / 4] >> FRAME_CMDREG1B_SHIFT);
    const unsigned stag = w[FRAME_STAG / 4] >> FRAME_TAG_SHIFT,
                   dtag = w[FRAME_DTAG / 4] >> FRAME_TAG_SHIFT;
    const unsigned fmt = (cmdreg1b >> 10) & 7u;
    const bool t = (flags & FRAME_FLAG_T) != 0,
               int_dest = t && (fmt == FMT_L || fmt == FMT_W || fmt == FMT_B);
    uint8_t bit = 0, vector = VEC_UNSUPPORTED_DATA;

    if (!force_unsupported && !frame_is_unsupported(w))
        vector = select_exception(&fpu->state,
                                  int_dest ? NONMASKABLE_INT_DEST_EXC : NONMASKABLE_EXC, &bit);
    if (!vector)
        return;
    mc68k_fpu040_exc_t *e = begin_exception(fpu, vector, false, t, 0);
    const uint32_t wbt = w[FRAME_WBTEMP / 4], stag_long = w[FRAME_STAG / 4];

    e->e1 = (flags & FRAME_FLAG_E1) != 0 ||
            force_unsupported; /* 未対応データ型の frame の E1 は 1 */
    e->e3 = (flags & FRAME_FLAG_E3) != 0;
    e->cmdreg1b = cmdreg1b;
    e->cmdreg3b = (uint16_t)(w[FRAME_CMDREG3B / 4] >> FRAME_CMDREG3B_SHIFT);
    e->stag = (uint8_t)stag;
    e->dtag = (uint8_t)dtag;
    e->etemp = x_from_words(&w[FRAME_ETEMP / 4]);
    e->fptemp = x_from_words(&w[FRAME_FPTEMP / 4]);
    e->wbtemp.sign_exp = (uint16_t)((wbt & FRAME_WBTS ? 0x8000u : 0u) |
                                    ((wbt >> FRAME_WBTE_SHIFT) & EXP15_MASK));
    e->wbtemp.signif = ((uint64_t)w[FRAME_WBTEMP / 4 + 1] << 32) | w[FRAME_WBTEMP / 4 + 2];
    e->wbte15 = (w[FRAME_DTAG / 4] & FRAME_WBTE15) != 0;
    e->wbtm_guard = (stag_long & FRAME_WBTM_GUARD) != 0;
    e->wbtm_round = (stag_long & FRAME_WBTM_ROUND) != 0;
    e->wbtm_sticky = (stag_long & FRAME_SBIT) != 0;
}

/* FPSP が正規化して負の指数にしたオペランド（ETE15/FPTE15 = 1 かつ指数 bit 14 = 1。res_func の
 * nrm_set）を、 実行用の拡張精度の非正規化数にする。指数は 16 bit の二の補数（$8000
 * 引く）で、-指数 だけ仮数を右へ寄せる。 仮数が全部消えて非 0
 * だった場合は最小の非正規化数にする（元の値が非 0
 * であることを保つ。一部の端数は切り捨てる。推論 24）。 */
static mc68k_fp80_t frame_operand(mc68k_fp80_t v, bool e15) {
    mc68k_fp80_t r = v;

    if (!e15 || !(v.sign_exp & 0x4000u))
        return v;
    const unsigned shift = 0x8000u - (v.sign_exp & EXP15_MASK);
    const uint64_t lost = shift >= 64 ? v.signif : v.signif & ((UINT64_C(1) << shift) - 1);

    r.sign_exp = (uint16_t)(v.sign_exp & 0x8000u);
    r.signif = shift >= 64 ? 0 : v.signif >> shift;
    if (lost != 0 && r.signif == 0)
        r.signif = 1;
    return r;
}

/* FPSP が frame を書き直して frestore する経路（CU_SAVEPC != 0、E1/E3/T = 0。res_func の
 * `fix_stk`）: CMDREG1B の 命令を frame の
 * ETEMP（ソース）・FPTEMP（宛先）で実行して宛先へ書く。完了時の例外は保留に積む。 */
static void complete_instruction(mc68k_fpu040_t *fpu, const uint32_t *w) {
    const uint16_t cmdreg1b = (uint16_t)(w[FRAME_CMDREG1B / 4] >> FRAME_CMDREG1B_SHIFT);
    const unsigned opclass = cmdreg1b >> 13, fmt = (cmdreg1b >> 10) & 7u;
    /* FSQRT の CMDREG1B の opmode は $5（cmdreg1b_of() の逆）。 */
    const uint16_t ext = (cmdreg1b & OPMODE_MASK) == 0x05u
                             ? (uint16_t)((cmdreg1b & ~OPMODE_MASK) | OP_FSQRT)
                             : cmdreg1b;
    const unsigned opmode = base_opmode(ext & OPMODE_MASK);
    const bool ete15 = (w[FRAME_STAG / 4] & FRAME_TAG_E15) != 0,
               fpte15 = (w[FRAME_DTAG / 4] & FRAME_TAG_E15) != 0;

    if ((opclass != OPCLASS_REG_REG && opclass != OPCLASS_EA_TO_REG) ||
        (opclass == OPCLASS_EA_TO_REG && fmt == SRC_FMOVECR) ||
        classify_opmode(ext & OPMODE_MASK) != OPMODE_IMPL)
        return;
    const mc68k_fp80_t etemp = x_from_words(&w[FRAME_ETEMP / 4]),
                       fptemp = x_from_words(&w[FRAME_FPTEMP / 4]);

    /* ETE15/FPTE15 = 1 のオペランドは FPSP
     * が処理済みで、未対応データ型の検査をしない（`get_op.sa`: 非正規化数は do_func/res_func
     * が扱う） */
    if ((opclass == OPCLASS_EA_TO_REG && fmt == FMT_P) || (!ete15 && x_is_unsupported(etemp)) ||
        (uses_destination(opmode) && !fpte15 && x_is_unsupported(fptemp))) {
        restore_exception_state(fpu, w, true);
        return;
    }
    fpu->state.fpsr_exc = 0;
    run_register_operation(fpu, ext, frame_operand(etemp, ete15),
                           frame_operand(fptemp, fpte15));
}

/* busy frame の FRESTORE（推論 10）。frame は FSAVE が作る位置から読む。
 * - E1/E3/T が立つ frame は、例外状態を戻す（restore_exception_state）。宛先は書かない。
 * - それ以外で CU_SAVEPC != 0 の frame は、命令を完了させる（complete_instruction）。
 * - CU_SAVEPC = 0 の frame（CU に命令が無い。bugfix.sa）は何もしない。 */
static void restore_busy_frame(mc68k_fpu040_t *fpu, const uint32_t *w) {
    if (w[FRAME_FLAGS / 4] & (FRAME_FLAG_E1 | FRAME_FLAG_E3 | FRAME_FLAG_T))
        restore_exception_state(fpu, w, false);
    else if ((w[FRAME_DIRTY / 4] >> FRAME_CU_SAVEPC_SHIFT) != 0)
        complete_instruction(fpu, w);
}

/* FRESTORE: null は reset、idle は idle へ戻す。unimplemented frame は idle
 * へ戻すだけ（ハードウェアが完了できない 命令を持つ）、busy frame は restore_busy_frame()（推論
 * 10）。version/size が不正なら format error。 */
static int exec_restore(mc68k_state_t *cpu, uint16_t opcode) {
    mc68k_fpu040_t *fpu = cpu->fpu040;
    const unsigned ea = opcode & 077u;
    uint32_t w[FRAME_MAX_WORDS] = {0};

    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    const uint32_t addr = calc_operand_ea(cpu, ea, 0);
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    read_memory_words(cpu, addr, FRAME_HEADER_BYTES, w);
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    const unsigned version = w[0] >> 24, size = (w[0] >> 16) & 0xFFu;
    const bool null_frame = version == 0;
    if (!null_frame && (version != FRAME_VERSION ||
                        (size != 0 && size != FRAME_UNIMPL_SIZE && size != FRAME_BUSY_SIZE)))
        return cpu_dispatch_exception(cpu, cpu->instr_pc, VEC_FORMAT_ERROR, FPU040_EXC_CYCLES);
    const unsigned bytes = null_frame ? FRAME_HEADER_BYTES : FRAME_HEADER_BYTES + size;
    if (bytes > FRAME_HEADER_BYTES) { /* 本文を読む（バスエラーを起こしうる） */
        read_memory_words(cpu, addr + FRAME_HEADER_BYTES, size, &w[1]);
        if (cpu->exec_state != CPU_NORMAL)
            return FPU040_FAULT_CYCLES;
    }
    if ((ea >> 3) == 3)
        cpu->a[ea & 7u] += bytes;
    if (null_frame) {
        mc68k_fpu040_reset(fpu);
    } else {
        memset(&fpu->exc, 0, sizeof(fpu->exc));
        fpu->idle = true;
        if (size == FRAME_BUSY_SIZE)
            restore_busy_frame(fpu, w);
    }
    return FPU040_OP_CYCLES;
}

/* 保留中の例外を報告できる命令か。FMOVE FPcr/FMOVEM は例外を起こさず（UM §9.2.4）、報告もしない
 * 。FSAVE は報告しない（UM §9.8）。FRESTORE
 * も同じ扱い（推論）。 */
static bool reports_pending_exception(unsigned type, unsigned opclass) {
    return type != TYPE_GEN || opclass == OPCLASS_REG_REG || opclass == OPCLASS_EA_TO_REG ||
           opclass == OPCLASS_REG_TO_EA;
}

/* 報告すべき保留中の例外があるか。未実装命令の状態（unimpl）は保留中の例外ではなく、次に例外を
 * 報告できる FP 命令（未実装命令自身を含む）が実行を始める時点で捨てる（F-line handler が FSAVE
 * せず RTE した場合に、同じ命令が無限に再実行されるのを防ぐ）。 */
static bool has_pending_exception(mc68k_fpu040_t *fpu) {
    if (fpu->exc.valid && fpu->exc.unimpl)
        memset(&fpu->exc, 0, sizeof(fpu->exc));
    return fpu->exc.valid;
}

/* 実装済み命令（FPU040_IMPL）の実行。 */
static int fpu040_execute(mc68k_state_t *cpu, uint16_t opcode, uint16_t ext) {
    mc68k_fpu040_t *fpu = cpu->fpu040;
    const unsigned type = (opcode >> 6) & 7u, opclass = ext >> 13;
    const bool has_ext = type == TYPE_GEN || type == TYPE_COND;

    if (type == TYPE_SAVE)
        return exec_save(cpu, opcode);
    if (type == TYPE_RESTORE)
        return exec_restore(cpu, opcode);
    if (reports_pending_exception(type, has_ext ? opclass : 0)) {
        if (has_pending_exception(fpu)) /* FSAVE まで同じ例外を報告し続ける（[040UM §9.8]） */
            return fpu040_pre_exception(cpu, fpu->exc.vector);
        fpu->state.fpiar = cpu->instr_pc;
    }
    if (type == TYPE_GEN)
        fpu->idle = true;
    if (type != TYPE_GEN)
        return exec_conditional(cpu, opcode, ext);
    switch (opclass) {
    case OPCLASS_REG_REG:
    case OPCLASS_EA_TO_REG:
        return exec_to_register(cpu, opcode, ext);
    case OPCLASS_REG_TO_EA:
        return exec_to_ea(cpu, opcode, ext);
    case OPCLASS_EA_TO_CTRL:
    case OPCLASS_CTRL_TO_EA:
        return exec_control(cpu, opcode, ext);
    default:
        return exec_movem(cpu, opcode, ext);
    }
}

/* 040 の未実装 FP 命令（format $2）。ソースと宛先を ETEMP/FPTEMP に取り、FSAVE の unimplemented
 * frame に載せる。FPSP はこれを使って命令を代行する（[040UM §9.6.1]）。 */
static int exec_unimplemented(mc68k_state_t *cpu, uint16_t opcode, uint16_t ext) {
    mc68k_fpu040_t *fpu = cpu->fpu040;
    const unsigned opclass = ext >> 13, opmode = ext & OPMODE_MASK, fmt = (ext >> 10) & 7u;
    uint32_t w[3] = {0, 0, 0}, ea_value = 0;

    if (has_pending_exception(fpu)) /* 保留中の例外が先に報告される（[040UM §9.6.1]） */
        return fpu040_pre_exception(cpu, fpu->exc.vector);
    if (opclass == OPCLASS_EA_TO_REG && fmt != SRC_FMOVECR &&
        !load_source_words(cpu, opcode, fmt, w, &ea_value))
        return FPU040_FAULT_CYCLES;
    mc68k_fpu040_exc_t *e = begin_exception(fpu, VEC_FLINE, true, false, ext);
    bool packed = false;
    if (opclass == OPCLASS_REG_REG)
        capture_register_source(e, fpu->state.fp[fmt]);
    else if (fmt != SRC_FMOVECR)
        packed = capture_source(e, fmt, w);
    e->e1 = packed;
    if (is_dyadic_opmode(opmode) && !packed)
        capture_destination(e, fpu->state.fp[(ext >> 7) & 7u]);
    fpu->idle = true;
    return cpu_dispatch_fpu040_exception(cpu, FPU040_FRAME_UNIMPL, VEC_FLINE, cpu->pc, ea_value,
                                         cpu->instr_pc, FPU040_EXC_CYCLES);
}

static int op_fpu040(mc68k_state_t *cpu, uint16_t opcode) {
    const unsigned type = (opcode >> 6) & 7u, ea = opcode & 077u;
    uint16_t ext = 0;

    /* FPU を構成しない 040（NONE）は、cpID 1 も通常の F-line。 */
    if (cpu->fpu_config != MC68K_FPU_68040 && cpu->fpu_config != MC68K_FPU_NONE_040)
        return fpu040_fline(cpu);

    if (type == TYPE_GEN || type == TYPE_COND) {
        ext = cpu_fetch_word(cpu);
        if (cpu->exec_state != CPU_NORMAL)
            return FPU040_FAULT_CYCLES;
    }
    const fpu040_class_t cls = fpu040_classify(opcode, ext);
    if (cls == FPU040_ILLEGAL)
        return fpu040_fline(cpu);

    const bool no_fpu = cpu->fpu_config == MC68K_FPU_NONE_040;
    if (!no_fpu && cls == FPU040_IMPL)
        return fpu040_execute(cpu, opcode, ext);
    if (!no_fpu)
        return exec_unimplemented(cpu, opcode, ext);

    /* 未実装 FP 命令、または FPU 非搭載 040 の全 FP 命令: EA を計算して format $2/$4。 */
    uint32_t ea_value = 0;
    const uint32_t an_before = cpu->a[ea & 7u];
    if (type == TYPE_GEN || type == TYPE_SAVE || type == TYPE_RESTORE ||
        (type == TYPE_COND && !is_ftrapcc(ea) && !is_fdbcc(ea)))
        ea_value = calc_operand_ea(cpu, ea, operand_bytes(cpu, opcode, ext));
    /* FPU 非搭載 040 は命令を完了させずにトラップするので、An は更新しない（設計書 推論 3）。
     */
    if (no_fpu && ((ea >> 3) == 3 || (ea >> 3) == 4))
        cpu->a[ea & 7u] = an_before;
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    skip_trailing_words(cpu, opcode);
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;

    return cpu_dispatch_fpu040_exception(
        cpu, no_fpu ? FPU040_FRAME_UNIMPL_NOFPU : FPU040_FRAME_UNIMPL, VEC_FLINE, cpu->pc,
        ea_value, cpu->instr_pc, FPU040_EXC_CYCLES);
}

void register_68040_fpu_ops(op_handler_t *table) {
    for (unsigned i = 0; i < OPCODE_CPID_COUNT; i++)
        table[OPCODE_CPID_BASE + i] = op_fpu040;
}

/* ---- MC68060 --------------------------------------------------------------------------
 * 実行部（共通コアの値演算、FBcc、FMOVE/FMOVEM の実装済み形）は 040 と共有し、060
 * が実装しない命令 だけを trap にする（設計書 §8、[060UM Table 6-11, §8.2.4]）。FPU 状態は 040
 * と同じ cpu->fpu040。 */
enum {
    VEC_UNIMPL_EA = 60,
    PCR_DFP = 0x00000002u, /* PCR bit 1: FPU 無効（[060UM §3.2.2.5]） */
    /* FSAVE/FRESTORE の frame は NULL を含め 3 long word。format は long 0 の bits 15-8 */
    FRAME060_BYTES = 12,
    FRAME060_FORMAT_SHIFT = 8,
    FRAME060_FORMAT_MASK = 0xFF,
    FRAME060_NULL = 0x00,
    FRAME060_IDLE = 0x60,
    FRAME060_EXCP = 0xE0,
    FRAME060_VECTOR_MASK = 7,
};

/* 060 の分類。FPU040_* に UNIMPL_EA（vector 60）を加えたもの。 */
typedef enum { FPU060_ILLEGAL, FPU060_IMPL, FPU060_UNIMPL, FPU060_UNIMPL_EA } fpu060_class_t;

/* FMOVEM の拡張語 bit 11: レジスタリストを Dn で指定する（動的リスト）。 */
static bool is_dynamic_list(uint16_t ext) {
    return (ext & 0x0800u) != 0;
}

static bool is_060_integer_round_opmode(unsigned opmode) {
    return opmode == OP_FINT || opmode == OP_FINTRZ;
}

/* 040 の分類を 060 に写す。040 が実装済みとする FDBcc/FScc/FTRAPcc は 060 で未実装、040
 * が未実装とする FINT/FINTRZ は 060 で実装済み。未実装 EA（動的リスト、2〜3
 * 個の制御レジスタへの即値、X/P の 即値）は演算の種類（Table 6-11
 * の命令を含む）に優先する（Table 8-3 の 2.4 > 2.6）。 */
static fpu060_class_t fpu060_classify(uint16_t opcode, uint16_t ext) {
    const unsigned type = (opcode >> 6) & 7u, ea = opcode & 077u;
    const fpu040_class_t base = fpu040_classify(opcode, ext);

    if (base == FPU040_ILLEGAL)
        return FPU060_ILLEGAL;
    if (type == TYPE_COND)
        return FPU060_UNIMPL;
    if (type != TYPE_GEN)
        return FPU060_IMPL;

    const unsigned opclass = ext >> 13, fmt = (ext >> 10) & 7u;
    switch (opclass) {
    case OPCLASS_EA_TO_CTRL:
        return ea == EA_IMMEDIATE && popcount8(fmt) >= 2 ? FPU060_UNIMPL_EA : FPU060_IMPL;
    case OPCLASS_EA_TO_MULTI:
    case OPCLASS_MULTI_TO_EA:
        return is_dynamic_list(ext) ? FPU060_UNIMPL_EA : FPU060_IMPL;
    case OPCLASS_EA_TO_REG:
        if (fmt == SRC_FMOVECR)
            return FPU060_UNIMPL;
        if (ea == EA_IMMEDIATE && (fmt == FMT_X || fmt == FMT_P))
            return FPU060_UNIMPL_EA;
        break;
    default:
        break;
    }
    if (base == FPU040_IMPL || is_060_integer_round_opmode(ext & OPMODE_MASK))
        return FPU060_IMPL;
    return FPU060_UNIMPL;
}

/* 060 の trap（FP 未実装命令 $2 / FP 無効 $4）。命令を完了させないので (An)+/-(An) の An は
 * 更新しない。stacked PC は次の命令、SP+8 は最初のメモリオペランドの EA（レジスタ・即値は 0）。
 */
static int fpu060_trap(mc68k_state_t *cpu, uint16_t opcode, uint16_t ext, uint8_t format) {
    const unsigned type = (opcode >> 6) & 7u, ea = opcode & 077u, mode = ea >> 3;
    const uint32_t an_before = cpu->a[ea & 7u];
    uint32_t ea_value = 0;

    if (type == TYPE_GEN || type == TYPE_SAVE || type == TYPE_RESTORE ||
        (type == TYPE_COND && !is_ftrapcc(ea) && !is_fdbcc(ea)))
        ea_value = calc_operand_ea(cpu, ea, operand_bytes(cpu, opcode, ext));
    if (mode == 3 || mode == 4) {
        cpu->a[ea & 7u] = an_before;
        /* FP 無効の -(An) は、拡張精度だけ An-12 でなく An-4（[060UM §8.2.4]） */
        if (mode == 4 && format == FPU040_FRAME_UNIMPL_NOFPU && type == TYPE_GEN &&
            ((ext >> 13) == OPCLASS_EA_TO_REG || (ext >> 13) == OPCLASS_REG_TO_EA) &&
            ((ext >> 10) & 7u) == FMT_X)
            ea_value = an_before - LONG_BYTES;
    }
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    skip_trailing_words(cpu, opcode);
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    if (format == FPU040_FRAME_UNIMPL) /* FPIAR は FP 命令のアドレス（FPSP が命令を特定する） */
        cpu->fpu040->state.fpiar = cpu->instr_pc;
    return cpu_dispatch_fpu040_exception(cpu, format, VEC_FLINE, cpu->pc, ea_value,
                                         cpu->instr_pc, FPU040_EXC_CYCLES);
}

/* FSAVE: 3 long word の frame。format は NULL/IDLE/EXCP（EXCP は保留中の FP 例外がある場合）。
 */
static int exec_save060(mc68k_state_t *cpu, uint16_t opcode) {
    mc68k_fpu040_t *fpu = cpu->fpu040;
    uint32_t w[FRAME060_BYTES / LONG_BYTES] = {0};

    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    const unsigned format = fpu->exc.valid ? FRAME060_EXCP
                            : fpu->idle    ? FRAME060_IDLE
                                           : FRAME060_NULL;
    w[0] = (uint32_t)format << FRAME060_FORMAT_SHIFT;
    if (format ==
        FRAME060_EXCP) { /* 状態語の V2-V0 と exception operand（[060UM §6.7、Figure 6-11]） */
        w[0] |= ((uint32_t)fpu->exc.operand060.sign_exp << 16) |
                ((uint32_t)(fpu->exc.vector - FPU_VECTOR_BSUN) & FRAME060_VECTOR_MASK);
        w[1] = (uint32_t)(fpu->exc.operand060.signif >> 32);
        w[2] = (uint32_t)fpu->exc.operand060.signif;
    }
    const uint32_t addr = calc_operand_ea(cpu, opcode & 077u, FRAME060_BYTES);
    if (cpu->exec_state == CPU_NORMAL)
        write_memory_words(cpu, addr, FRAME060_BYTES, w);
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    memset(&fpu->exc, 0, sizeof(fpu->exc)); /* FSAVE が例外状態を消す */
    return FPU040_OP_CYCLES;
}

/* FRESTORE: NULL は reset、IDLE/EXCP は idle へ戻す。それ以外の format は format error。 */
static int exec_restore060(mc68k_state_t *cpu, uint16_t opcode) {
    mc68k_fpu040_t *fpu = cpu->fpu040;
    const unsigned ea = opcode & 077u;
    uint32_t w[FRAME060_BYTES / LONG_BYTES] = {0};

    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    const uint32_t addr = calc_operand_ea(cpu, ea, 0);
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    read_memory_words(cpu, addr, FRAME060_BYTES, w);
    if (cpu->exec_state != CPU_NORMAL)
        return FPU040_FAULT_CYCLES;
    const unsigned format = (w[0] >> FRAME060_FORMAT_SHIFT) & FRAME060_FORMAT_MASK;
    if (format != FRAME060_NULL && format != FRAME060_IDLE && format != FRAME060_EXCP)
        return cpu_dispatch_exception(cpu, cpu->instr_pc, VEC_FORMAT_ERROR, FPU040_EXC_CYCLES);
    if ((ea >> 3) == 3)
        cpu->a[ea & 7u] += FRAME060_BYTES;
    if (format == FRAME060_NULL) {
        mc68k_fpu040_reset(fpu);
    } else {
        memset(&fpu->exc, 0, sizeof(fpu->exc));
        fpu->idle = true;
        if (format ==
            FRAME060_EXCP) { /* FPU を exception 状態へ戻す。operand は後の FSAVE が書き出す */
            fpu->exc.valid = true;
            fpu->exc.vector = (uint8_t)(FPU_VECTOR_BSUN + (w[0] & FRAME060_VECTOR_MASK));
            fpu->exc.operand060.sign_exp = (uint16_t)(w[0] >> 16);
            fpu->exc.operand060.signif = ((uint64_t)w[1] << 32) | w[2];
        }
    }
    return FPU040_OP_CYCLES;
}

static int op_fpu060(mc68k_state_t *cpu, uint16_t opcode) {
    const unsigned type = (opcode >> 6) & 7u;
    uint16_t ext = 0;

    /* FPU を構成しない 060（NONE）は、cpID 1 も通常の F-line。 */
    if (!cpu_has_060_fpu(cpu))
        return fpu040_fline(cpu);
    if (type == TYPE_GEN || type == TYPE_COND) {
        ext = cpu_fetch_word(cpu);
        if (cpu->exec_state != CPU_NORMAL)
            return FPU040_FAULT_CYCLES;
    }
    cpu->t060_aux = ext;
    const fpu060_class_t cls = fpu060_classify(opcode, ext);
    if (cls == FPU060_ILLEGAL)
        return fpu040_fline(cpu);
    /* FP 無効（PCR.DFP）は未実装の判定より先。Table 6-11 の命令も $4 になる。 */
    if (cpu->pcr & PCR_DFP)
        return fpu060_trap(cpu, opcode, ext, FPU040_FRAME_UNIMPL_NOFPU);
    switch (cls) {
    case FPU060_UNIMPL_EA: /* fault として FP 命令を積む。FPIAR は不変 */
        return cpu_dispatch_exception(cpu, cpu->instr_pc, VEC_UNIMPL_EA, FPU040_EXC_CYCLES);
    case FPU060_UNIMPL:
        return fpu060_trap(cpu, opcode, ext, FPU040_FRAME_UNIMPL);
    default:
        break;
    }
    if (type == TYPE_SAVE)
        return exec_save060(cpu, opcode);
    if (type == TYPE_RESTORE)
        return exec_restore060(cpu, opcode);
    return fpu040_execute(cpu, opcode, ext);
}

void register_68060_fpu_ops(op_handler_t *table) {
    for (unsigned i = 0; i < OPCODE_CPID_COUNT; i++)
        table[OPCODE_CPID_BASE + i] = op_fpu060;
}
