/*
 * fpu_model.h — MC68881/MC68882 コプロセッサモデルの内部定義。
 * 公開しない。
 *
 * struct mc68k_fpu は設計書 §4 の内部状態。表 A の状態 S1〜S20 は (phase, arith,
 * unstarted, violation_pending, context_valid) から fpu_state_id() で導く。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mc68000/fpu.h"

/* §4.1 の protocol phase（10種） */
typedef enum {
    FPU_PH_IDLE,
    FPU_PH_COMMAND_PENDING,
    FPU_PH_CONDITION_PENDING,
    FPU_PH_RESPONSE_NEXT,
    FPU_PH_OPERAND_IN,
    FPU_PH_OPERAND_OUT,
    FPU_PH_REGISTER_SELECT,
    FPU_PH_PC,
    FPU_PH_SAVE_BODY,
    FPU_PH_RESTORE_BODY,
} fpu_phase_t;

/* §4.2 の保留算術例外 */
typedef enum {
    FPU_ARITH_NONE,
    FPU_ARITH_PENDING,
    FPU_ARITH_PRESENTED_PRE,
    FPU_ARITH_PRESENTED_MID,
} fpu_arith_t;

/* UNSTARTED の TAKE の出所（§4.2） */
typedef enum {
    FPU_ORIGIN_ARITH,
    FPU_ORIGIN_FLINE,
    FPU_ORIGIN_BSUN,
} fpu_origin_t;

/* ダイアログの内訳（phase の中の細かい待ち）。phase は fpu_set_step() が決める */
typedef enum {
    FPU_STEP_NONE,
    FPU_STEP_FIRST,       /* COMMAND_PENDING: 最初の primitive が未読 */
    FPU_STEP_COND,        /* CONDITION_PENDING: TF の NULL が未読 */
    FPU_STEP_NEXT_EA_OUT, /* RESPONSE_NEXT: 後続が DR=1 の EA_TRANSFER */
    FPU_STEP_NEXT_FPREGS, /* RESPONSE_NEXT: 後続が FP_REGISTERS */
    FPU_STEP_IN_EA,       /* OPERAND_IN: <ea> からの operand（演算入力、control 入力） */
    FPU_STEP_IN_DN,       /* OPERAND_IN: MP_REGISTER（動的 k-factor / 動的 list の Dn） */
    FPU_STEP_IN_REGS,     /* OPERAND_IN: FMOVEM の FP レジスタ入力 */
    FPU_STEP_OUT,         /* OPERAND_OUT: 外部への出力 operand */
    FPU_STEP_REGSEL,      /* REGISTER_SELECT: register select 読出し待ち */
    FPU_STEP_PC,          /* PC: IA 書込み待ち（68882）。BSUN の PC も含む */
} fpu_step_t;

/* command の対話の型（command descriptor から決まる） */
typedef enum {
    FPU_DK_NOREQ,      /* register-to-register、FMOVECR。転送なし */
    FPU_DK_EA_IN,      /* <ea>,FPn の演算・FMOVE */
    FPU_DK_EA_OUT,     /* FMOVE FPn,<ea>（静的 k-factor / 非 P） */
    FPU_DK_PK_OUT_DYN, /* FMOVE.P FPn,<ea>{Dn}（動的 k-factor） */
    FPU_DK_CTRL_IN,
    FPU_DK_CTRL_OUT,
    FPU_DK_MOVEM_IN_STATIC,
    FPU_DK_MOVEM_OUT_STATIC,
    FPU_DK_MOVEM_IN_DYN,
    FPU_DK_MOVEM_OUT_DYN,
} fpu_dk_t;

#define FPU_BUF_WORDS 24u /* FP レジスタ 8本 × 3 longword */
#define FPU_BODY_WORDS \
    64u /* FSAVE/FRESTORE の body の最大 longword 数（68882 busy の 53 を含む） */
/* 例外ベクタ（[881UM Table 7-6]） */
#define FPU_VECTOR_F_LINE   11u
#define FPU_VECTOR_PROTOCOL 13u
#define FPU_VECTOR_BSUN     48u
#define FPU_VECTOR_INEX     49u
#define FPU_VECTOR_DZ       50u
#define FPU_VECTOR_UNFL     51u
#define FPU_VECTOR_OPERR    52u
#define FPU_VECTOR_OVFL     53u
#define FPU_VECTOR_SNAN     54u
/* FPCR の PREC 欄の予約値（11）。共通コアは 0〜2 だけを扱う */
#define FPU_PREC_RESERVED   3u

/* 表 A の状態番号（設計書 §4.2） */
#define STATE_S1           1
#define STATE_S2           2
#define STATE_S3           3
#define STATE_S4           4
#define STATE_S5           5
#define STATE_S6           6
#define STATE_S7           7
#define STATE_S8           8
#define STATE_S9           9
#define STATE_S10          10
#define STATE_S11          11
#define STATE_S12          12
#define STATE_S13          13
#define STATE_S14          14
#define STATE_S15          15
#define STATE_S16          16
#define STATE_S17          17
#define STATE_S18          18
#define STATE_S19          19
#define STATE_S20          20
#define FPU_PREDICATE_MASK 0x3Fu /* condition は下位6bitだけを使う */

struct fpu_dialog {
    mc68k_fpu_command_t cmd;
    fpu_dk_t dk;
    bool pc_flag;      /* 最初の primitive が PC=1 */
    bool ia_pre;       /* 最初の primitive を読む前に IA を書いた（保存しない） */
    bool mask_known;   /* register mask が確定した */
    bool mid_exc;      /* 外部宛て FMOVE の転送後に TAKE_MID を返す */
    uint8_t dn_value;  /* MP_REGISTER で受けた Dn の下位 8bit（list または k-factor） */
    uint8_t mask;      /* FMOVEM の register mask（上位 byte 側の生値） */
    uint8_t cnt;       /* buf の受信済み／読出し済み word 数 */
    uint8_t total;     /* buf の word 数 */
    uint8_t predicate; /* CONDITION_PENDING の predicate */
    bool tf;           /* CONDITION_PENDING の評価結果 */
    uint32_t buf[FPU_BUF_WORDS];
};

struct fpu_unstarted {
    bool active;
    bool is_condition; /* condition（predicate）なら true、command なら false */
    fpu_origin_t origin;
    uint16_t word;     /* raw command word、または predicate（下位6bit） */
    bool ia_satisfied; /* 68882: BSUN の IA を先に書いた */
};

/* S2 の応答読出しが返し続ける条件命令の結果の NULL（IA=0、PF=0、TF。§5.1「応答値の保持」） */
struct fpu_hold {
    bool active;
    bool tf;
};

struct mc68k_fpu {
    mc68k_fpu_config_t config;
    mc68k_fpu_state_t state;
    fpu_phase_t phase;
    fpu_step_t step;
    struct fpu_dialog dlg;
    fpu_arith_t arith;
    struct fpu_unstarted unstarted;
    bool violation_pending;
    bool context_valid;
    struct fpu_hold hold;

    /* SAVE_BODY / RESTORE_BODY */
    uint32_t body[FPU_BODY_WORDS];
    size_t body_words;
    size_t body_pos;
    bool restore_busy; /* RESTORE_BODY の frame が busy か idle か */
};

/* 表 A の状態番号 1〜20 */
int fpu_state_id(const struct mc68k_fpu *f);

/* reset（§3.1）: 共通コアの reset、context_valid=false、TAKE の保存先3つのクリア、phase=idle */
void mc68k_fpu_model_reset(struct mc68k_fpu *f);

/* fpu_frame.c */
size_t fpu_idle_body_words(mc68k_fpu_config_t config);
size_t fpu_busy_body_words(mc68k_fpu_config_t config);
uint16_t fpu_idle_format_word(mc68k_fpu_config_t config);
uint16_t fpu_busy_format_word(mc68k_fpu_config_t config);
void fpu_build_save_frame(struct mc68k_fpu *f, uint16_t *format_word, size_t *body_bytes);
void fpu_apply_restore_frame(struct mc68k_fpu *f);

/* fpu_proto.c の共有ヘルパー（fpu_frame.c が使う） */
void fpu_set_step(struct mc68k_fpu *f, fpu_step_t step);
void fpu_to_idle(struct mc68k_fpu *f);
bool fpu_violation(struct mc68k_fpu *f);
void fpu_start_command(struct mc68k_fpu *f, uint16_t word);
void fpu_start_condition(struct mc68k_fpu *f, uint8_t predicate);
void fpu_start_command_pending_rule(struct mc68k_fpu *f, uint16_t word);
void fpu_start_condition_pending_rule(struct mc68k_fpu *f, uint8_t predicate);
fpu_dk_t fpu_dialog_kind(const mc68k_fpu_command_t *cmd);
unsigned fpu_expected_in_words(const struct fpu_dialog *d);
unsigned fpu_expected_out_words(const struct fpu_dialog *d);
bool fpu_command_passes_pending(const mc68k_fpu_command_t *cmd);
unsigned fpu_popcount8(unsigned v);

/* fpu_model.c: FPCR/FPSR の32bit パッキング（control register move と観測 API で共通。§6） */
uint32_t fpu_pack_fpcr(const mc68k_fpu_state_t *st);
uint32_t fpu_pack_fpsr(const mc68k_fpu_state_t *st);
void fpu_unpack_fpcr(mc68k_fpu_state_t *st, uint32_t v);
void fpu_unpack_fpsr(mc68k_fpu_state_t *st, uint32_t v);
