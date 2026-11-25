/*
 * timing_060.c — MC68060 の命令時間（[060UM §10] Table 10-5〜10-26）。
 *
 * 060 は 1 命令の時間が EA 形式でほぼ決まるため、命令ごとの handler ではなく、実行後に
 * opcode と実行時の情報から合計時間を決める。mc68k_execute() が handler の戻り値をこの値で
 * 置き換える（例外を積んだ命令は cyc060_exception() の値をそのまま使う）。
 *
 * 前提（[060UM §10.2]）: 命令ごとの単独実行時間。dual issue の重なり、change/use stall、
 * cache miss、ATC miss、バス wait は含めない（加算境界は docs/mc68000-internals/reference/cpu_68060.md §10）。
 * 分岐は簡略化した branch cache（256 entry の direct-mapped。taken で登録し、not taken で
 * 削除する 2 状態）で「予測成功/失敗/未登録」の列（Table 10-17、10-18）を選ぶ。060 の
 * 5 状態予測や 4-way 構成は模さない。
 *
 * EA 計算時間（Table 10-5）は、brief format までが 0、full format が 1、memory indirect が
 * 3 で、cpu->t060_ea_extra に ea_helpers.c の full_ext_ea() が積む。この値は EA を計算する
 * 命令の時間へ加算する。
 */
#include "cpu_internal.h"

enum {
    T060_MODE_AN_DIRECT = 1,
    T060_MODE_POSTINC = 3,
    T060_MODE_PREDEC = 4,
    T060_MODE_D16_AN = 5,
    T060_MODE_INDEX = 6,
    T060_MODE_OTHER = 7,
    T060_REG_ABS_W = 0,
    T060_REG_ABS_L = 1,
    T060_REG_PC_D16 = 2,
    T060_REG_PC_INDEX = 3,
    T060_REG_IMM = 4,
};

/* 例外処理時間（Table 10-26）。EA を計算する divide by zero と CHK は EA 時間を足す。 */
enum {
    T060_EXC_COMMON = 19,
    T060_EXC_DIVZERO_CHK = 20,
    T060_EXC_BSUN = 21,
    T060_EXC_FORMAT_IRQ = 23,
};

/* 整数命令（Table 10-9〜10-24）。 */
enum {
    T060_BASE = 1,
    T060_MUL = 2,
    T060_DIVW = 22, /* 上限。word divide は途中で終わる（Table 10-9 注 2） */
    T060_DIVL = 38,
    T060_CHK = 2,
    T060_LINK = 2,
    T060_CAS = 19,
    T060_TAS_MEM = 17,
    T060_MOVE_TO_SR = 12,
    T060_ANDI_EORI_SR = 12,
    T060_ORI_SR = 5,
    T060_NOP = 9,
    T060_STOP = 8,
    T060_RESET = 520,
    T060_RTE = 17,
    T060_RTD_RTS = 7,
    T060_RTR = 8,
    T060_LPSTOP = 15,
    T060_MOVE16 = 18, /* read も write も cache miss の値。read hit は 11（Table 10-8 注） */
    T060_MOVEC_SIMPLE_TO = 12,
    T060_MOVEC_SIMPLE_FROM = 11,
    T060_MOVEC_MMU_TO = 15,
    T060_MOVEC_MMU_FROM = 14,
    T060_JMP_SLOW = 5,
    T060_PFLUSH = 18,
    T060_PFLUSHA = 33,
    T060_PLPA_HIT = 15,
    T060_CINVA = 17,
    T060_CINVL = 18,
    T060_CINVP = 274,
    /* CPUSH は「走査の基準時間 + dirty line 1 本あたりの書き戻し時間 × 書き戻した line 数」で
     * 数える。Table 10-24 は上限値（CPUSHA ≦5394(0/512)、CPUSHP ≦2838(0/256)、CPUSHL ≦26(0/1)）
     * だけを印字しており、基準時間と line 単価は本実装がその 3 点から逆算した推定値である
     * （CPUSHA: 274 + 512×10 = 5394、CPUSHP: 278 + 256×10 = 2838、CPUSHL: 16 + 1×10 = 26）。
     * 表の line 数（512/256/1）の dirty line を書き戻したとき、表の上限値に一致する。 */
    T060_CPUSH_LINE = 10,
    T060_CPUSHA_BASE = 274,
    T060_CPUSHL_BASE = 16,
    T060_CPUSHP_BASE = 278,
};

enum { T060_CACHE_SCOPE_LINE = 1, T060_CACHE_SCOPE_PAGE = 2 };

/* MOVEC の CACR 番号と CACR のビット（[060UM §5.2.1]）。 */
#define T060_CREG_CACR UINT32_C(0x002)
#define T060_CACR_EBC  UINT32_C(0x00800000)
#define T060_CACR_CABC UINT32_C(0x00400000)
#define T060_CACR_CUBC UINT32_C(0x00200000)

/* Table 10-17、10-18 の列。fwd/back は静的予測（branch cache 未登録）、pred は登録済みで
 * 予測どおり taken、wrong は登録済みなのに not taken。 */
typedef struct {
    int8_t fwd_taken, fwd_not, back_taken, back_not, pred_taken, wrong;
} t060_branch_t;

static const t060_branch_t BR_BCC = {7, 1, 3, 7, 0, 7};
static const t060_branch_t BR_BRA = {3, 0, 3, 0, 0, 0};
static const t060_branch_t BR_BSR = {3, 0, 3, 0, 1, 0};
static const t060_branch_t BR_DBCC = {3, 8, 3, 8, 2, 8};
static const t060_branch_t BR_DBRA = {3, 7, 3, 7, 1, 7};
static const t060_branch_t BR_FBCC = {8, 2, 8, 2, 2, 8};
static const t060_branch_t BR_JMP = {3, 0, 3, 0, 0, 0};
static const t060_branch_t BR_JSR = {3, 0, 3, 0, 1, 0};

static void branch_cache_clear(mc68k_state_t *cpu, bool user_only) {
    for (unsigned i = 0; i < MC68K_060_BRANCH_CACHE_ENTRIES; i++)
        if (!user_only || cpu->t060_bc_user[i])
            cpu->t060_bc_tag[i] = 0;
}

static int branch_time(mc68k_state_t *cpu, uint32_t pc, bool taken, bool backward,
                       const t060_branch_t *t) {
    const bool enabled = (cpu->cacr & T060_CACR_EBC) != 0;
    const unsigned idx = (pc >> 1) & (MC68K_060_BRANCH_CACHE_ENTRIES - 1u);
    const uint32_t tag = pc | 1u;
    const bool hit = enabled && cpu->t060_bc_tag[idx] == tag;
    int cycles;
    if (hit)
        cycles = taken ? t->pred_taken : t->wrong;
    else if (taken)
        cycles = backward ? t->back_taken : t->fwd_taken;
    else
        cycles = backward ? t->back_not : t->fwd_not;
    if (enabled && taken) {
        cpu->t060_bc_tag[idx] = tag;
        cpu->t060_bc_user[idx] = (cpu->sr & SR_S) == 0;
    } else if (hit) {
        cpu->t060_bc_tag[idx] = 0;
    }
    return cycles;
}

static bool ea_is_memory(int mode, int reg) {
    return mode >= 2 && !(mode == T060_MODE_OTHER && reg == T060_REG_IMM);
}

static bool ea_is_immediate(int mode, int reg) {
    return mode == T060_MODE_OTHER && reg == T060_REG_IMM;
}

/* (d8,An,Xi)、absolute、(d8,PC,Xi) を表で 1 cycle 重くする命令用。 */
static bool ea_is_index_or_absolute(int mode, int reg) {
    return mode == T060_MODE_INDEX ||
           (mode == T060_MODE_OTHER && reg != T060_REG_PC_D16 && reg != T060_REG_IMM);
}

static int popcount16(uint16_t v) {
    int n = 0;
    for (; v; v >>= 1)
        n += v & 1;
    return n;
}

static int cyc060_move(const mc68k_state_t *cpu, uint16_t op) {
    const int src_mode = (op >> 3) & 7, src_reg = op & 7;
    const int dst_mode = (op >> 6) & 7;
    int cycles = T060_BASE;
    if (ea_is_memory(src_mode, src_reg) && dst_mode >= 2)
        cycles++;
    if (ea_is_immediate(src_mode, src_reg) && dst_mode >= T060_MODE_D16_AN)
        cycles++;
    return cycles + cpu->t060_ea_extra;
}

static int cyc060_group0(const mc68k_state_t *cpu, uint16_t op) {
    const int mode = (op >> 3) & 7, reg = op & 7, x = cpu->t060_ea_extra;
    const int kind = (op >> 9) & 7;
    if (op & 0x0100) /* BTST/BCHG/BCLR/BSET Dn,<ea>。MOVEP は vector 61 で来ない */
        return T060_BASE + x;
    if ((op & 0x00FF) == 0x003C && kind != 2 && kind != 3 && kind != 6)
        return T060_BASE; /* ORI/ANDI/EORI to CCR */
    if ((op & 0x00FF) == 0x007C && kind != 2 && kind != 3 && kind != 6)
        return kind == 0 ? T060_ORI_SR : T060_ANDI_EORI_SR;
    /* 静的ビット数の BTST/BCHG/BCLR/BSET。BSET は下位 2 bit が 11 なので CAS より先に判定する
     */
    if (kind == 4)
        return T060_BASE + (ea_is_memory(mode, reg) && mode >= T060_MODE_D16_AN ? 1 : 0) + x;
    if ((op & 0x00C0) == 0x00C0) /* CAS（CMP2/CHK2 は vector 61） */
        return T060_CAS + (mode == T060_MODE_INDEX || mode == T060_MODE_OTHER ? 1 : 0) + x;
    if (kind == 7) /* MOVES */
        return T060_BASE + (ea_is_index_or_absolute(mode, reg) ? 1 : 0) + x;
    /* ORI/ANDI/SUBI/ADDI/EORI/CMPI #imm,<ea> */
    return T060_BASE + (mode >= T060_MODE_D16_AN ? 1 : 0) + x;
}

static int cyc060_movec(mc68k_state_t *cpu, uint16_t op) {
    const bool to_control = (op & 1) != 0;
    if (to_control && (cpu->t060_aux & 0xFFFu) == T060_CREG_CACR) {
        if (cpu->cacr & T060_CACR_CABC)
            branch_cache_clear(cpu, false);
        else if (cpu->cacr & T060_CACR_CUBC)
            branch_cache_clear(cpu, true);
    }
    switch (cpu->t060_aux & 0xFFFu) {
    case 0x000: /* SFC */
    case 0x001: /* DFC */
    case 0x800: /* USP */
    case 0x801: /* VBR */
    case 0x808: /* PCR */
        return to_control ? T060_MOVEC_SIMPLE_TO : T060_MOVEC_SIMPLE_FROM;
    default:
        return to_control ? T060_MOVEC_MMU_TO : T060_MOVEC_MMU_FROM;
    }
}

static int cyc060_movem(const mc68k_state_t *cpu, uint16_t op) {
    const int mode = (op >> 3) & 7, reg = op & 7;
    int n = popcount16((uint16_t)cpu->t060_aux);
    if (n == 0)
        n = 1;
    const bool extra =
        mode == T060_MODE_INDEX || (mode == T060_MODE_OTHER && reg != T060_REG_PC_D16);
    return n + (extra ? 1 : 0) + cpu->t060_ea_extra;
}

static int cyc060_jump(mc68k_state_t *cpu, uint16_t op, uint32_t instr_pc) {
    const int mode = (op >> 3) & 7, reg = op & 7;
    const bool fast = mode == T060_MODE_OTHER && reg <= T060_REG_PC_D16;
    if (!fast)
        return T060_JMP_SLOW + cpu->t060_ea_extra;
    return branch_time(cpu, instr_pc, true, false, (op & 0x0040) ? &BR_JMP : &BR_JSR);
}

static int cyc060_group4(mc68k_state_t *cpu, uint16_t op, uint32_t instr_pc) {
    const int mode = (op >> 3) & 7, reg = op & 7, x = cpu->t060_ea_extra;
    switch (op) {
    case 0x4E70:
        return T060_RESET;
    case 0x4E71:
        return T060_NOP;
    case 0x4E72:
        return T060_STOP;
    case 0x4E73:
        return T060_RTE;
    case 0x4E74:
    case 0x4E75:
        return T060_RTD_RTS;
    case 0x4E76:
        return T060_BASE; /* TRAPV（V=1 は例外経路） */
    case 0x4E77:
        return T060_RTR;
    case 0x4E7A:
    case 0x4E7B:
        return cyc060_movec(cpu, op);
    default:
        break;
    }
    if ((op & 0xFFF8) == 0x4E50 || (op & 0xFFF8) == 0x4808)
        return T060_LINK;
    if ((op & 0xFFF8) == 0x4E58)
        return T060_BASE; /* UNLK */
    if ((op & 0xFFF8) == 0x4E60)
        return 2; /* MOVE An,USP */
    if ((op & 0xFFF8) == 0x4E68)
        return T060_BASE; /* MOVE USP,An */
    if ((op & 0xFF80) == 0x4E80)
        return cyc060_jump(cpu, op, instr_pc); /* JSR/JMP */
    if ((op & 0xFFF8) == 0x4840)
        return T060_BASE;          /* SWAP */
    if ((op & 0xFFC0) == 0x4840) { /* PEA */
        if (mode == T060_MODE_OTHER && reg == T060_REG_PC_INDEX)
            /* Table 10-20: (d8,PC,Xi)/(bd,PC,Xi) は 2、注 1 でメモリ間接 +2 */
            return 2 + (x > 1 ? x - 1 : 0);
        return T060_BASE + ((mode == T060_MODE_D16_AN || mode == T060_MODE_INDEX) ? 1 : 0) + x;
    }
    if (mode == 0 &&
        ((op & 0xFFC0) == 0x4880 || (op & 0xFFC0) == 0x48C0 || (op & 0xFFC0) == 0x49C0))
        return T060_BASE; /* EXT/EXTB */
    if ((op & 0xFB80) == 0x4880)
        return cyc060_movem(cpu, op);
    if ((op & 0xFFC0) == 0x4C00 || (op & 0xFFC0) == 0x4C40) {
        const int base = (op & 0x0040) ? T060_DIVL : T060_MUL;
        return base +
               (mode == T060_MODE_INDEX || (mode == T060_MODE_OTHER && reg != T060_REG_PC_D16)
                    ? 1
                    : 0) +
               x;
    }
    if ((op & 0xF1C0) == 0x41C0)
        return T060_BASE + x; /* LEA */
    if ((op & 0xF140) == 0x4100)
        return T060_CHK + x;
    if ((op & 0xFFC0) == 0x40C0 || (op & 0xFFC0) == 0x42C0 || (op & 0xFFC0) == 0x44C0)
        return T060_BASE + x; /* MOVE from SR/from CCR/to CCR */
    if ((op & 0xFFC0) == 0x46C0)
        return T060_MOVE_TO_SR + x;
    if ((op & 0xFFC0) == 0x4AC0)
        return (mode == 0 ? T060_BASE : T060_TAS_MEM) + x;
    return T060_BASE + x; /* NEGX/CLR/NEG/NOT/NBCD/TST */
}

static int cyc060_bcc(mc68k_state_t *cpu, uint16_t op, uint32_t instr_pc) {
    const int cond = (op >> 8) & 0xF;
    /* 変位が次の命令を指す taken は PC では判定できない */
    const bool taken = cpu->flow_changed;
    const bool backward = cpu->t060_aux != 0;
    return branch_time(cpu, instr_pc, taken, backward,
                       cond == 0   ? &BR_BRA
                       : cond == 1 ? &BR_BSR
                                   : &BR_BCC);
}

static int cyc060_group5(mc68k_state_t *cpu, uint16_t op, uint32_t instr_pc) {
    const int mode = (op >> 3) & 7, x = cpu->t060_ea_extra;
    if ((op & 0x00C0) != 0x00C0) /* ADDQ/SUBQ */
        return T060_BASE + x;
    if (mode == T060_MODE_AN_DIRECT) { /* DBcc */
        const bool taken = cpu->flow_changed;
        return branch_time(cpu, instr_pc, taken, true,
                           (op & 0x0F00) == 0x0100 ? &BR_DBRA : &BR_DBCC);
    }
    return T060_BASE + x; /* Scc、TRAPcc（成立時は例外経路） */
}

static int cyc060_group8_d(const mc68k_state_t *cpu, uint16_t op, bool add_sub) {
    const int mode = (op >> 3) & 7, reg = op & 7, x = cpu->t060_ea_extra;
    if (add_sub) {
        if ((op & 0x00C0) == 0x00C0) { /* ADDA/SUBA */
            const bool same = (mode == T060_MODE_POSTINC || mode == T060_MODE_PREDEC) &&
                              reg == ((op >> 9) & 7);
            return T060_BASE + (same ? 1 : 0) + x;
        }
        if ((op & 0x0130) == 0x0100) /* ADDX/SUBX */
            return (op & 0x0008) ? 2 : T060_BASE;
        return T060_BASE + x;
    }
    if ((op & 0x01F0) == 0x0100) /* SBCD */
        return (op & 0x0008) ? 2 : T060_BASE;
    if ((op & 0x01F0) == 0x0140 || (op & 0x01F0) == 0x0180) /* PACK/UNPK */
        return 2;
    if ((op & 0x00C0) == 0x00C0)
        return T060_DIVW + x;
    return T060_BASE + x;
}

static int cyc060_groupb(const mc68k_state_t *cpu, uint16_t op) {
    const int mode = (op >> 3) & 7, reg = op & 7;
    if ((op & 0x0138) == 0x0108 && (op & 0x00C0) != 0x00C0)
        return 2;                  /* CMPM */
    if ((op & 0x00C0) == 0x00C0) { /* CMPA。Table 10-9 注 1: (Ay)+/-(Ay) で Ay = An なら +1 */
        const bool same =
            (mode == T060_MODE_POSTINC || mode == T060_MODE_PREDEC) && reg == ((op >> 9) & 7);
        return T060_BASE + (same ? 1 : 0) + cpu->t060_ea_extra;
    }
    return T060_BASE + cpu->t060_ea_extra;
}

static int cyc060_groupc(const mc68k_state_t *cpu, uint16_t op) {
    if ((op & 0x00C0) == 0x00C0)
        return T060_MUL + cpu->t060_ea_extra;
    if ((op & 0x01F0) == 0x0100) /* ABCD */
        return (op & 0x0008) ? 2 : T060_BASE;
    if ((op & 0x0130) == 0x0100) /* EXG */
        return T060_BASE;
    return T060_BASE + cpu->t060_ea_extra;
}

/* Table 10-16。t060_aux は op_bf() が積む「field が 5 byte にまたがるか」。 */
static int cyc060_bitfield(const mc68k_state_t *cpu, uint16_t op) {
    static const uint8_t base4[8] = {6, 6, 8, 6, 8, 9, 8, 6};
    static const uint8_t base5[8] = {8, 8, 12, 8, 12, 11, 12, 6};
    const int kind = (op >> 8) & 7, mode = (op >> 3) & 7, reg = op & 7;
    const int base = cpu->t060_aux ? base5[kind] : base4[kind];
    if (mode == 0)
        return base;
    return base + (ea_is_index_or_absolute(mode, reg) ? 1 : 0) + cpu->t060_ea_extra;
}

static int cyc060_groupe(const mc68k_state_t *cpu, uint16_t op) {
    if ((op & 0xF8C0) == 0xE8C0)
        return cyc060_bitfield(cpu, op);
    return T060_BASE + ((op & 0x00C0) == 0x00C0 ? cpu->t060_ea_extra : 0);
}

/* CINV/CPUSH（Table 10-24）。CPUSH の t060_aux は op_cache060() が積んだ「書き戻した dirty line 数」。 */
static int cyc060_cache_op(const mc68k_state_t *cpu, uint16_t op) {
    const bool push = (op & 0x0020) != 0;
    const int scope = (op >> 3) & 3;
    const int lines = (int)cpu->t060_aux * T060_CPUSH_LINE;
    if (scope == T060_CACHE_SCOPE_LINE)
        return push ? T060_CPUSHL_BASE + lines : T060_CINVL;
    if (scope == T060_CACHE_SCOPE_PAGE)
        return push ? T060_CPUSHP_BASE + lines : T060_CINVP;
    return push ? T060_CPUSHA_BASE + lines : T060_CINVA;
}

/* FPU（Table 10-25）。t060_aux は命令の拡張語。 */
enum {
    FPU_OPMODE_MASK = 0x7F,
    FPU_OPMODE_ROUNDED = 0x40, /* FSxxx/FDxxx（丸め精度つき）は bit6 が 1 */
    FPU_OPMODE_DOUBLE = 0x04,  /* 丸め精度つきで FD（倍精度）なら 1 */
    FPU_OPMODE_SSQRT = 0x41,   /* FSSQRT。FDSQRT と合わせ、FSQRT の 0x04 に戻す */
    FPU_OPMODE_FSQRT = 0x04,
    FPU_OPMODE_BASE_MASK = 0x3F,
    FPU_OPMODE_FMOVE = 0x00,
    FPU_OPMODE_FINT = 0x01,
    FPU_OPMODE_FINTRZ = 0x03,
    FPU_OPMODE_FSUB = 0x28,
    FPU_OPMODE_FTST = 0x3A,
    FPU_FORMAT_DOUBLE = 5,
    FPU_DN_EXTRA = 2,
    FPU_INT_DN_EXTRA = 1,
    FPU_CMD_FMOVE_CR_LOAD = 4,
    FPU_CMD_FMOVE_CR_STORE = 5,
};

/* 丸め精度つきの opmode（FSADD、FDMUL など）を基本演算の opmode に戻す。 */
static unsigned fpu_base_opmode(unsigned opmode) {
    opmode &= FPU_OPMODE_MASK;
    if (opmode & FPU_OPMODE_ROUNDED) {
        opmode &= ~(unsigned)FPU_OPMODE_DOUBLE;
        if (opmode == FPU_OPMODE_SSQRT)
            return FPU_OPMODE_FSQRT;
    }
    return opmode & FPU_OPMODE_BASE_MASK;
}

static int fpu_opmode_base(unsigned opmode) {
    switch (fpu_base_opmode(opmode)) {
    case 0x01: /* FINT */
    case 0x03: /* FINTRZ */
    case 0x22: /* FADD */
    case 0x23: /* FMUL */
    case 0x27: /* FSGLMUL */
    case 0x28: /* FSUB */
        return 3;
    case 0x04: /* FSQRT */
        return 68;
    case 0x20: /* FDIV */
    case 0x24: /* FSGLDIV */
        return 37;
    default: /* FMOVE、FABS、FNEG、FCMP、FTST */
        return 1;
    }
}

static int fpu_format_extra(unsigned format) {
    switch (format) {
    case 0: /* long */
    case 4: /* word */
    case 6: /* byte */
        return 3;
    case 2: /* extended */
        return 2;
    default:
        return 0;
    }
}

/* <ea> を読む／書く FPU 命令の EA 加算。opmode は FMOVE FPcr など opmode を持たない命令では
 * FPU_OPMODE_FMOVE を渡す。Table 10-25 は FINT/FINTRZ の (d8,An,Xi)・absolute・#<data> と
 * Dn、FSUB 系・FTST の #<data> だけ他の命令と加算が異なる（印字値に合わせる）。 */
static int fpu_ea_extra(const mc68k_state_t *cpu, int mode, int reg, unsigned opmode,
                        bool immediate_adds) {
    const unsigned base = fpu_base_opmode(opmode);
    const bool integer_round = base == FPU_OPMODE_FINT || base == FPU_OPMODE_FINTRZ;
    if (mode == 0)
        return integer_round ? FPU_INT_DN_EXTRA : FPU_DN_EXTRA;
    int n = cpu->t060_ea_extra;
    if (integer_round)
        return n > 0 ? n + 1 : 0; /* full format／memory indirect だけ index 分が 1 足される */
    if (ea_is_index_or_absolute(mode, reg))
        n++;
    if (immediate_adds && ea_is_immediate(mode, reg) && base != FPU_OPMODE_FSUB &&
        base != FPU_OPMODE_FTST)
        n++;
    return n;
}

static int cyc060_fpu_movem(const mc68k_state_t *cpu, uint16_t ext, int mode, int reg) {
    int n;
    if (ext & 0x0800)
        n = popcount16((uint16_t)(cpu->d[(ext >> 4) & 7] & 0xFF));
    else
        n = popcount16(ext & 0xFF);
    return 1 + 3 * n + (ea_is_index_or_absolute(mode, reg) ? 1 : 0) + cpu->t060_ea_extra;
}

static int cyc060_fpu(mc68k_state_t *cpu, uint16_t op, uint32_t instr_pc) {
    const unsigned type = (op >> 6) & 7u;
    const int mode = (op >> 3) & 7, reg = op & 7;
    const uint16_t ext = (uint16_t)cpu->t060_aux;
    if (type == 2 || type == 3) { /* FBcc */
        const bool taken = cpu->flow_changed;
        return branch_time(cpu, instr_pc, taken, false, &BR_FBCC);
    }
    if (type == 4)
        return 3; /* FSAVE */
    if (type == 5)
        return 6; /* FRESTORE */
    if (type != 0)
        return T060_BASE; /* FDBcc/FScc/FTRAPcc は未実装 FP 命令の例外経路 */
    const unsigned form = (ext >> 13) & 7u;
    const unsigned format = (ext >> 10) & 7u;
    if (form == 0 || form == 2) { /* FPm,FPn / <ea>,FPn */
        int cycles = fpu_opmode_base(ext & FPU_OPMODE_MASK);
        if (form == 2) {
            cycles += fpu_ea_extra(cpu, mode, reg, ext & FPU_OPMODE_MASK,
                                   fpu_base_opmode(ext) != FPU_OPMODE_FMOVE);
            cycles += fpu_format_extra(format);
            if (ea_is_immediate(mode, reg) && format == FPU_FORMAT_DOUBLE)
                cycles++;
        }
        return cycles;
    }
    if (form == 3) { /* FMOVE FPn,<ea> */
        return T060_BASE + fpu_ea_extra(cpu, mode, reg, FPU_OPMODE_FMOVE, false) +
               fpu_format_extra(format);
    }
    if (form == 4 || form == 5) { /* FMOVE/FMOVEM FPcr */
        const int count = popcount16((uint16_t)((ext >> 10) & 7u));
        if (count != 1)
            return form == 4 ? 7 : 5;
        if (form == FPU_CMD_FMOVE_CR_LOAD)
            return 6 + fpu_ea_extra(cpu, mode, reg, FPU_OPMODE_FMOVE, true);
        return 2 + fpu_ea_extra(cpu, mode, reg, FPU_OPMODE_FMOVE, false);
    }
    return cyc060_fpu_movem(cpu, ext, mode, reg); /* FMOVEM FPn */
}

static int cyc060_linef(mc68k_state_t *cpu, uint16_t op, uint32_t instr_pc) {
    if ((op & 0xFF00) == 0xF400) {
        if (op & 0x0080) /* instruction cache を対象にすると branch cache も消える */
            branch_cache_clear(cpu, false);
        return cyc060_cache_op(cpu, op);
    }
    if ((op & 0xFFE0) == 0xF500) /* opmode（bit4-3）が 2/3 なら全 entry */
        return (op & 0x0010) ? T060_PFLUSHA : T060_PFLUSH;
    if ((op & 0xFFB8) == 0xF588)
        return T060_PLPA_HIT;
    if ((op & 0xFFC0) == 0xF600)
        return T060_MOVE16;
    if (op == 0xF800)
        return T060_LPSTOP;
    if ((op & 0xFE00) == 0xF200)
        return cyc060_fpu(cpu, op, instr_pc);
    return T060_BASE;
}

static int cyc060_instruction_raw(mc68k_state_t *cpu, uint16_t op, uint32_t instr_pc);

/* 予測成功の分岐は表が 0 cycle（前の命令に畳まれる）。mc68k_execute() は 0 cycle の命令では
 * 進まなくなるため、1 cycle に切り上げる。 */
int cyc060_instruction(mc68k_state_t *cpu, uint16_t op, uint32_t instr_pc) {
    const int cycles = cyc060_instruction_raw(cpu, op, instr_pc);
    return cycles < 1 ? 1 : cycles;
}

static int cyc060_instruction_raw(mc68k_state_t *cpu, uint16_t op, uint32_t instr_pc) {
    switch (op >> 12) {
    case 0x0:
        return cyc060_group0(cpu, op);
    case 0x1:
    case 0x2:
    case 0x3:
        return cyc060_move(cpu, op);
    case 0x4:
        return cyc060_group4(cpu, op, instr_pc);
    case 0x5:
        return cyc060_group5(cpu, op, instr_pc);
    case 0x6:
        return cyc060_bcc(cpu, op, instr_pc);
    case 0x7:
        return T060_BASE;
    case 0x8:
        return cyc060_group8_d(cpu, op, false);
    case 0x9:
    case 0xD:
        return cyc060_group8_d(cpu, op, true);
    case 0xB:
        return cyc060_groupb(cpu, op);
    case 0xC:
        return cyc060_groupc(cpu, op);
    case 0xE:
        return cyc060_groupe(cpu, op);
    case 0xF:
        return cyc060_linef(cpu, op, instr_pc);
    default:
        return T060_BASE;
    }
}

int cyc060_exception(uint16_t vector, int ea_extra) {
    switch (vector) {
    case 5: /* divide by zero */
    case 6: /* CHK */
        return T060_EXC_DIVZERO_CHK + ea_extra;
    case 14: /* format error */
        return T060_EXC_FORMAT_IRQ;
    case 48: /* FP branch on unordered condition */
        return T060_EXC_BSUN;
    default:
        return T060_EXC_COMMON;
    }
}

int cyc060_interrupt(void) {
    /* Table 10-26 注 3: オートベクタ・外部ベクタとも同じ値（ウェイト 0）。
     * ベクタ番号に依存させない */
    return T060_EXC_FORMAT_IRQ;
}
