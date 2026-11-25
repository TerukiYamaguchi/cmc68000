/*
 * cycles.h — CPUモデル別の命令サイクルタイミング。
 *
 * 型・定数のみを持ち、
 * mc68k_state の完全定義には依存しない（cpu_internal.h との相互include を
 * 避けるため、同ファイル§5.4のファイル分割方針に従う）。
 *
 * - cyc_get()/CYC() の実体は cpu_internal.h（mc68k_state 定義の後）に置く。
 * - g_cycles[][] の実体と cyc_resolve() は cycles.c に置く。
 */
#pragma once

#include <stdint.h>

/* この構造体は ea_helpers.h が定義する。前方宣言により、model timing table が
 * CPU の完全な内部定義へ依存することを避ける。 */
struct ea_desc;
typedef struct ea_desc ea_desc_t;

/* タイミングプロファイル（§5.2）。MC68K_MODEL_68030 と MC68K_MODEL_68EC030 は CYC_PROF_68030 を
 * 共有する（EC030 が030とタイミング同一という仮定の根拠・傍証は§5.2参照。
 * 一次資料の直接の裏付けは無く、PMMU差のみという構成上の仮定）。 */
typedef enum {
    CYC_PROF_68000 = 0,
    CYC_PROF_68010,
    CYC_PROF_68020,
    CYC_PROF_68030, /* MC68030 と MC68EC030 が共有 */
    CYC_PROF_68040,
    CYC_PROF_68060,
    CYC_PROF_COUNT
} cyc_profile_t;

/* 68020/68030 manual が使う5種類の effective-address timing table。
 * NONE は意図的な値であり、一部の命令 timing matrix はすでに EA 処理を含むため、
 * 二重に加算してはならない。 */
typedef enum {
    EA_COST_NONE = 0,
    EA_COST_FETCH,     /* fea */
    EA_COST_CALC,      /* cea */
    EA_COST_FETCH_IMM, /* fiea */
    EA_COST_CALC_IMM,  /* ciea */
    EA_COST_JUMP,      /* jea */
    EA_COST_KIND_COUNT
} ea_cost_kind_t;

/* タイミングクラス（§5.3）。DBcc、68010 固有の命令時間、乗除算、immediate、単一オペランド、
 * bit 操作などの命令時間を区別する。 */
typedef enum {
    /* 条件分岐（[UM30 §11.6.15] / [UM20 §8.2.15] / [DS10 Table 7-18]） */
    CYC_DBCC_BRANCH,  /* cc false, count not expired（分岐する） */
    CYC_DBCC_EXPIRED, /* cc false, count expired（抜ける） */
    CYC_DBCC_CC_TRUE, /* cc true（デクリメントせず通過） */

    /* 68010 固有の命令時間と乗除算の基本時間。 */
    CYC_ADDA_L_EA_REG,
    CYC_SUBA_L_EA_REG,
    CYC_ADD_L_EA_REG,
    CYC_AND_L_EA_REG,
    CYC_OR_L_EA_REG,
    CYC_SUB_L_EA_REG,
    CYC_EOR_L_DNDN,
    CYC_DIVS,
    CYC_DIVU,
    CYC_MULS,
    CYC_MULU,

    /* [DS10 Table 7-11] Immediate */
    CYC_ADDI_L_DN,
    CYC_SUBI_L_DN,
    CYC_EORI_L_DN,
    CYC_ORI_L_DN,
    CYC_CMPI_L_DN,
    CYC_SUBQ_W_AN,

    /* [DS10 Table 7-12] Single Operand */
    CYC_SCC_TRUE,

    /* [DS10 Table 7-17] Bit Manipulation（メモリ形式。レジスタ形式のBCLR非対称は
     * Phase 1aでリテラル直書き済み・クラス化不要） */
    CYC_BCLR_MEM_DYNAMIC,
    CYC_BCLR_MEM_STATIC,

    /* [DS10 Table 7-18] Conditional */
    CYC_BCC_BYTE_NOT_TAKEN,
    CYC_BCC_WORD_NOT_TAKEN,

    /* [DS10 Table 7-20] Multi-Precision */
    CYC_ADDX_L_REGREG,
    CYC_SUBX_L_REGREG,

    /* [DS10 Table 7-21] Miscellaneous */
    CYC_ANDI_CCRSR,
    CYC_EORI_CCRSR,
    CYC_ORI_CCRSR,
    CYC_CHK_NORMAL,
    CYC_MOVE_FROM_SR_REG,
    CYC_MOVE_FROM_CCR_REG, /* 68010新設。68000行はCYC_ABSENT */
    CYC_MOVE_USP,
    CYC_MOVEC_SRC_TO_REG, /* 68010新設（Rc→Rn）。68000行はCYC_ABSENT */
    CYC_MOVES_L,          /* 68010新設。68000行はCYC_ABSENT。値は暫定（Phase 2aでEA依存分解） */
    CYC_RESET_INSTR,
    CYC_RTE_SHORT,
    CYC_RTE_LONG_NORETRY,     /* 68010のみ到達。68000/68020/68030行はCYC_ABSENT */
    CYC_RTE_LONG_RETRY_READ,  /* 68010のみ到達（SSW.RR=0,RW=1）。他行はCYC_ABSENT
                               */
    CYC_RTE_LONG_RETRY_WRITE, /* 68010のみ到達（SSW.RR=0,RW=0）。他行はCYC_ABSENT */

    /* [DS10 Table 7-22] Exception Processing */
    CYC_EXC_CHK,
    CYC_EXC_DIVZERO,
    CYC_EXC_ILLEGAL,
    CYC_EXC_INTERRUPT,
    CYC_EXC_MOVEC_ILLEGAL, /* 68010新設。68000行はCYC_ABSENT */
    CYC_EXC_PRIVILEGE,
    CYC_EXC_TRACE,
    CYC_EXC_TRAP,
    CYC_EXC_TRAPV,
    CYC_EXC_BKPT, /* 68010新設。68000行はCYC_ABSENT */

    /* UM30 §11.6.16 にだけ独立行がある exception-taken 経路。020 の
     * 既存 snapshot 40 とは資料上の親子関係にないため別クラスにする。 */
    CYC_CHK_EXCEPTION_DN,
    CYC_CHK_EXCEPTION_EA,
    CYC_CHK2_EXCEPTION,

    CYC_CLASS_COUNT
} cyc_class_t;

/* 表に書く番兵。実際のサイクル数（1以上）と重ならない負値にする。 */
#define CYC_INHERIT ((int16_t)-1) /* 親プロファイルの値をそのまま使う */
#define CYC_ABSENT  ((int16_t)-2) /* このモデルには存在しない命令のクラス */

struct mc68k_state; /* 前方宣言で足りる。完全定義は cpu_internal.h */

/* モデル確定時（mc68k_create_model() 内、build_op_table() と同じ場所）に
 * 一度だけ呼ぶ。継承チェーンを解決し cpu->cyc[] を確定させる。 */
void cyc_resolve(struct mc68k_state *cpu);

/* テスト専用アクセサ:
 * g_cycles の生エントリをそのまま返す。cyc_resolve() 後の解決済み値
 * （cpu->cyc[]）とは違い、CYC_INHERIT の混入を直接検査できる。
 * 本番コードから呼んではならない。 */
int16_t cyc_raw_entry(cyc_profile_t prof, cyc_class_t cls);

/* timing class の後半、すなわち EA contribution が必要かを返す。
 * g_cycles と同じ profile 継承規則に従う。 */
ea_cost_kind_t cyc_ea_kind(const struct mc68k_state *cpu, cyc_class_t cls);

/* decode 済み effective address に対する model 固有の contribution を返す。 */
int ea_cycles(const struct mc68k_state *cpu, ea_cost_kind_t kind, const ea_desc_t *ea);

/* 一部の68020命令 table は data operand が byte/long でも固定の1-word extension を
 * 使う。この幅を decode 済み EA descriptor と分離し、呼出し側が誤って long immediate
 * row を選ばないようにする（UM20 8.2.2/8.2.4）。 */
int ea_cycles_immediate_word(const struct mc68k_state *cpu, ea_cost_kind_t kind,
                             const ea_desc_t *ea);
