/*
 * fpu_frame.c — FSAVE/FRESTORE の state frame。
 *
 * frame の byte 数・format word は [881UM Table 5-8、Table 6-6] のとおり（機種ごと）。
 * format word（と、null 以外では予約 word）は入口が置くので、このモデルが返す body は
 * format longword の後ろ（+$04 以降）である。
 *
 * 推論（この1か所にまとめる。frame の内容は一次資料が不透明な部分）:
 *  - idle frame の command/condition image は +$04 の上位 word（Figure 6-4・6-5。下位 word
 * は予約）に raw command word または predicate（下位6bit）を置く。68882 の CU の32 byte は
 * +$08〜+$27（p.6-32）。 p.6-30 の「CU state は format word の直後」の記述とは食い違うが、図と
 * p.6-32 を採る。
 *  - busy frame の body は、先頭 longword に BIU flags（idle frame と同じ32bit形式）を置き、
 *    続けてモデル固定表現（下記 BUSY_*）を置く。残りの longword は0。68882 も同じ配置で、
 *    CU の 32 byte を別に持たない。
 *  - busy frame の FRESTORE は、固定表現が妥当で（magic、phase、command word の再デコード、
 *    転送 word 数と register mask の整合、BIU の 30–28/bit 26 が phase から導いた値と一致、
 *    b27=0 は FMOVEM・control move の phase だけ）あるときだけ再開する。それ以外は B11。
 */
#include <string.h>
#include "fpu_exec.h"
#include "fpu_model.h"

#define BIU_B31_SHIFT       31
#define BIU_P_SHIFT         28
#define BIU_P_MASK          7u
#define BIU_B27_SHIFT       27
#define BIU_B26_SHIFT       26
#define CMD_WORD_MAX        0xFFFFu /* command word は16bit */
#define BIU_LOW16           0xFFFFu /* bit15-0 は未定義。全1 */
#define BIU_P_NO_PENDING    7u      /* Table 6-4 */
#define BIU_P_GENERAL       3u      /* 011 */
#define BIU_P_CONDITIONAL   1u      /* 001 */
#define BIU_P_WRITE_OPERAND 4u      /* 100 */
#define BIU_P_READ_OPERAND  6u      /* 110 */

#define FORMAT_BASE_REVISION 0x1Fu
#define FORMAT_881_IDLE      0x18u
#define FORMAT_881_BUSY      0xB4u
#define FORMAT_882_IDLE      0x38u
#define FORMAT_882_BUSY      0xD4u
#define FORMAT_ILLEGAL       0x0200u
#define FORMAT_LOW_MASK      0xFFu
#define FORMAT_VERSION_SHIFT 8

#define IDLE_WORDS_881     6u  /* +$04..+$1B。24 byte */
#define BUSY_WORDS_881     45u /* 180 byte */
#define IDLE_WORDS_882     14u /* 56 byte */
#define BUSY_WORDS_882     53u /* 212 byte */
#define IDLE_BIU_INDEX_881 5u  /* +$18 */
#define IDLE_BIU_INDEX_882 13u /* +$38 */
#define IDLE_IMAGE_INDEX   0u  /* +$04。両機種で同じ（68882 の CU の32 byte は +$08〜+$27） */
#define IMAGE_WORD_SHIFT   16  /* image の16bit は +$04 の上位 word（Figure 6-4・6-5） */
#define BIU_DEFINED_MASK \
    0xFC000000u /* bit31-26。bit25-16 と bit15-0 は restore で無視（p.6-33） */

/* busy frame の固定表現（body の longword 番号） */
#define BUSY_BIU              0u
#define BUSY_MAGIC            1u
#define BUSY_STATE            2u
#define BUSY_WORD             3u
#define BUSY_FLAGS            4u
#define BUSY_COUNTS           5u
#define BUSY_RESERVED_A       6u
#define BUSY_RESERVED_B       7u
#define BUSY_BUF              8u
#define BUSY_MAGIC_VALUE      0x46505342u /* 'FPSB' */
#define BUSY_FLAG_TF          0x1u
#define BUSY_FLAG_PC          0x2u
#define BUSY_FLAG_MID         0x4u
#define BUSY_FLAG_MASK_KNOWN  0x8u
#define BUSY_FLAG_USED        0xFu
#define BUSY_DN_SHIFT         8
#define BUSY_MASK_SHIFT       16
#define BUSY_BYTE             0xFFu
#define BUSY_FLAG_UNUSED_MASK 0xFF000000u
#define COUNT_TOTAL_SHIFT     8

size_t fpu_idle_body_words(mc68k_fpu_config_t config) {
    return config == MC68K_FPU_68882 ? IDLE_WORDS_882 : IDLE_WORDS_881;
}

size_t fpu_busy_body_words(mc68k_fpu_config_t config) {
    return config == MC68K_FPU_68882 ? BUSY_WORDS_882 : BUSY_WORDS_881;
}

uint16_t fpu_idle_format_word(mc68k_fpu_config_t config) {
    return (uint16_t)((FORMAT_BASE_REVISION << FORMAT_VERSION_SHIFT) |
                      (config == MC68K_FPU_68882 ? FORMAT_882_IDLE : FORMAT_881_IDLE));
}

uint16_t fpu_busy_format_word(mc68k_fpu_config_t config) {
    return (uint16_t)((FORMAT_BASE_REVISION << FORMAT_VERSION_SHIFT) |
                      (config == MC68K_FPU_68882 ? FORMAT_882_BUSY : FORMAT_881_BUSY));
}

static uint32_t make_biu(bool b31, unsigned p, bool b27, bool b26) {
    return ((uint32_t)b31 << BIU_B31_SHIFT) | ((uint32_t)p << BIU_P_SHIFT) |
           ((uint32_t)b27 << BIU_B27_SHIFT) | ((uint32_t)b26 << BIU_B26_SHIFT) | BIU_LOW16;
}

/* §4.1 の表: busy の phase 別の 30–28 と bit 26 */
static bool busy_biu_for_state(int state, fpu_dk_t dk, unsigned *p, bool *b26) {
    switch (state) {
    case STATE_S12:
    case STATE_S17:
    case STATE_S18:
        *p = BIU_P_GENERAL;
        *b26 = true;
        return true;
    case STATE_S13:
        *p = BIU_P_CONDITIONAL;
        *b26 = true;
        return true;
    case STATE_S14:
        if (dk == FPU_DK_MOVEM_IN_DYN || dk == FPU_DK_MOVEM_OUT_DYN) {
            *p = BIU_P_GENERAL;
            *b26 = true;
        } else {
            *p = BIU_P_READ_OPERAND;
            *b26 = false;
        }
        return true;
    case STATE_S15:
        *p = BIU_P_WRITE_OPERAND;
        *b26 = true;
        return true;
    case STATE_S16:
        *p = BIU_P_READ_OPERAND;
        *b26 = false;
        return true;
    default:
        return false;
    }
}

static uint32_t busy_state_biu(const struct mc68k_fpu *f, int state) {
    unsigned p = BIU_P_GENERAL;
    bool b26 = true;

    busy_biu_for_state(state, f->dlg.dk, &p, &b26);
    return make_biu(false, p, f->arith == FPU_ARITH_NONE, b26);
}

static void serialize_busy(const struct mc68k_fpu *f, int state, uint32_t *w) {
    const struct fpu_dialog *d = &f->dlg;
    bool keeps_pc = (state == STATE_S12 || state == STATE_S18) && d->pc_flag;
    uint32_t flags =
        (d->tf ? BUSY_FLAG_TF : 0u) | (keeps_pc ? BUSY_FLAG_PC : 0u) |
        (d->mid_exc ? BUSY_FLAG_MID : 0u) | (d->mask_known ? BUSY_FLAG_MASK_KNOWN : 0u) |
        ((uint32_t)d->dn_value << BUSY_DN_SHIFT) | ((uint32_t)d->mask << BUSY_MASK_SHIFT);

    w[BUSY_BIU] = busy_state_biu(f, state);
    w[BUSY_MAGIC] = BUSY_MAGIC_VALUE;
    w[BUSY_STATE] = (uint32_t)state;
    w[BUSY_WORD] = state == STATE_S13 ? d->predicate : d->cmd.command_word;
    w[BUSY_FLAGS] = flags;
    w[BUSY_COUNTS] = (uint32_t)d->cnt | ((uint32_t)d->total << COUNT_TOTAL_SHIFT);
    memcpy(&w[BUSY_BUF], d->buf, sizeof(d->buf));
}

void fpu_build_save_frame(struct mc68k_fpu *f, uint16_t *format_word, size_t *body_bytes) {
    uint32_t body[FPU_BODY_WORDS];
    size_t words = 0;
    int state = fpu_state_id(f);
    uint16_t fmt = 0;
    bool busy = state >= STATE_S12 && state <= STATE_S18;
    bool b27 = f->arith == FPU_ARITH_NONE;
    size_t biu_idx = f->config == MC68K_FPU_68882 ? IDLE_BIU_INDEX_882 : IDLE_BIU_INDEX_881;

    memset(body, 0, sizeof(body));
    if (!f->context_valid) {
        /* null frame。violation 保留は frame に残らない（§7.1） */
        fmt = 0;
    } else if (busy) {
        fmt = fpu_busy_format_word(f->config);
        words = fpu_busy_body_words(f->config);
        serialize_busy(f, state, body);
    } else {
        unsigned p = BIU_P_NO_PENDING;
        uint32_t image = 0;
        bool b31 = f->violation_pending;

        fmt = fpu_idle_format_word(f->config);
        words = fpu_idle_body_words(f->config);
        if (!b31 && f->unstarted.active) {
            p = f->unstarted.is_condition ? BIU_P_CONDITIONAL : BIU_P_GENERAL;
            image = f->unstarted.word;
        }
        body[IDLE_IMAGE_INDEX] = image << IMAGE_WORD_SHIFT;
        body[biu_idx] = make_biu(b31, p, b27, true);
    }
    /* frame を確定した時点で arith・UNSTARTED・violation 保留・進行中のダイアログを捨てる */
    f->arith = FPU_ARITH_NONE;
    f->violation_pending = false;
    memset(&f->unstarted, 0, sizeof(f->unstarted));
    memset(&f->hold, 0, sizeof(f->hold));
    fpu_to_idle(f);
    if (words > 0) {
        memcpy(f->body, body, words * sizeof(body[0]));
        f->body_words = words;
        f->body_pos = 0;
        f->phase = FPU_PH_SAVE_BODY;
    }
    *format_word = fmt;
    *body_bytes = words * sizeof(uint32_t);
}

/* ---- FRESTORE ------------------------------------------------------------ */

static bool dk_is_movem_dyn(fpu_dk_t dk) {
    return dk == FPU_DK_MOVEM_IN_DYN || dk == FPU_DK_MOVEM_OUT_DYN;
}

static bool dk_is_movem(fpu_dk_t dk) {
    return dk == FPU_DK_MOVEM_IN_STATIC || dk == FPU_DK_MOVEM_OUT_STATIC || dk_is_movem_dyn(dk);
}

static bool dk_has_pc(fpu_dk_t dk) {
    return dk == FPU_DK_NOREQ || dk == FPU_DK_EA_IN || dk == FPU_DK_EA_OUT ||
           dk == FPU_DK_PK_OUT_DYN;
}

/* 保存した状態 state で、dlg の転送欄が command と矛盾しないか。矛盾すれば false（B11） */
static bool busy_progress_consistent(const struct mc68k_fpu *f, int state, fpu_step_t *step) {
    const struct fpu_dialog *d = &f->dlg;
    bool movem_mask_ok =
        !dk_is_movem(d->dk) ||
        (d->mask_known &&
         (dk_is_movem_dyn(d->dk) ? d->mask == d->dn_value
                                 : d->mask == (d->cmd.register_mask & BUSY_BYTE)));
    unsigned in_words;
    unsigned out_words;

    if (d->cnt > d->total || d->total > FPU_BUF_WORDS)
        return false;
    switch (state) {
    case STATE_S12:
        *step = FPU_STEP_FIRST;
        return d->cnt == 0 && d->total == 0 && !d->mask_known && !d->mid_exc &&
               d->dn_value == 0 && d->mask == 0;
    case STATE_S14:
        if (dk_is_movem_dyn(d->dk)) {
            *step = FPU_STEP_NEXT_FPREGS;
            return d->cnt == 0 && d->total == 0 && movem_mask_ok && !d->mid_exc;
        }
        if (d->dk != FPU_DK_EA_OUT && d->dk != FPU_DK_PK_OUT_DYN)
            return false;
        *step = FPU_STEP_NEXT_EA_OUT;
        return d->cnt == 0 && d->total == fpu_expected_out_words(d) && !d->mask_known &&
               d->mask == 0 && (d->dk == FPU_DK_PK_OUT_DYN || d->dn_value == 0);
    case STATE_S15:
        in_words = fpu_expected_in_words(d);
        if (d->dk == FPU_DK_EA_IN || d->dk == FPU_DK_CTRL_IN) {
            *step = FPU_STEP_IN_EA;
            return d->cnt < d->total && d->total == in_words && !d->mask_known && !d->mid_exc;
        }
        if (d->dk == FPU_DK_PK_OUT_DYN || (dk_is_movem_dyn(d->dk) && !d->mask_known)) {
            *step = FPU_STEP_IN_DN;
            return d->cnt == 0 && d->total == 0 && !d->mask_known && !d->mid_exc &&
                   d->dn_value == 0 && d->mask == 0;
        }
        if (d->dk == FPU_DK_MOVEM_IN_STATIC || d->dk == FPU_DK_MOVEM_IN_DYN) {
            *step = FPU_STEP_IN_REGS;
            return movem_mask_ok && d->total == in_words && d->total > 0 && d->cnt < d->total &&
                   !d->mid_exc;
        }
        return false;
    case STATE_S16:
        out_words = fpu_expected_out_words(d);
        *step = FPU_STEP_OUT;
        if (d->dk == FPU_DK_EA_OUT || d->dk == FPU_DK_PK_OUT_DYN)
            return d->total == out_words && d->cnt < d->total && !d->mask_known && d->mask == 0;
        if (d->dk == FPU_DK_CTRL_OUT)
            return d->total == out_words && d->cnt < d->total && !d->mask_known &&
                   !d->mid_exc && d->mask == 0;
        if (d->dk == FPU_DK_MOVEM_OUT_STATIC || d->dk == FPU_DK_MOVEM_OUT_DYN)
            return movem_mask_ok && d->total == out_words && d->total > 0 &&
                   d->cnt < d->total && !d->mid_exc;
        return false;
    case STATE_S17:
        *step = FPU_STEP_REGSEL;
        /* cnt==0 は冗長（残す）: S17 は転送の前で、上の cnt > total の検査と total == 0 から
         * cnt は0に決まる */
        return dk_is_movem(d->dk) && movem_mask_ok && d->cnt == 0 && d->total == 0 &&
               !d->mid_exc;
    case STATE_S18:
        *step = FPU_STEP_PC;
        return f->config == MC68K_FPU_68882 && dk_has_pc(d->dk) && d->pc_flag && d->cnt == 0 &&
               d->total == 0 && !d->mask_known && !d->mid_exc && d->dn_value == 0 &&
               d->mask == 0;
    default:
        return false;
    }
}

/* 表 B の B10: 固定表現が妥当なら busy frame の phase を再開する。妥当でなければ false（B11）
 */
static bool restore_busy_image(struct mc68k_fpu *f, const uint32_t *w, bool b27) {
    struct fpu_dialog *d = &f->dlg;
    int state = (int)w[BUSY_STATE];
    uint32_t flags = w[BUSY_FLAGS];
    uint32_t counts = w[BUSY_COUNTS];
    fpu_step_t step = FPU_STEP_NONE;
    unsigned p;
    bool b26;
    size_t i;

    if (w[BUSY_MAGIC] != BUSY_MAGIC_VALUE || w[BUSY_RESERVED_A] != 0 ||
        w[BUSY_RESERVED_B] != 0 || (flags & BUSY_FLAG_UNUSED_MASK) != 0 ||
        (counts >> (2 * COUNT_TOTAL_SHIFT)) != 0)
        return false;
    if (state < STATE_S12 || state > STATE_S18)
        return false;
    memset(d, 0, sizeof(*d));
    d->tf = (flags & BUSY_FLAG_TF) != 0;
    d->pc_flag = (flags & BUSY_FLAG_PC) != 0;
    d->mid_exc = (flags & BUSY_FLAG_MID) != 0;
    d->mask_known = (flags & BUSY_FLAG_MASK_KNOWN) != 0;
    d->dn_value = (uint8_t)((flags >> BUSY_DN_SHIFT) & BUSY_BYTE);
    d->mask = (uint8_t)((flags >> BUSY_MASK_SHIFT) & BUSY_BYTE);
    d->cnt = (uint8_t)(counts & BUSY_BYTE);
    d->total = (uint8_t)((counts >> COUNT_TOTAL_SHIFT) & BUSY_BYTE);
    memcpy(d->buf, &w[BUSY_BUF], sizeof(d->buf));
    for (i = d->total; i < FPU_BUF_WORDS; i++)
        if (d->buf[i] != 0)
            goto reject;
    if (state == STATE_S13) {
        if (w[BUSY_WORD] > FPU_PREDICATE_MASK || d->pc_flag || d->mid_exc || d->mask_known ||
            d->dn_value != 0 || d->mask != 0 || d->cnt != 0 || d->total != 0 || b27 == false)
            goto reject;
        d->predicate = (uint8_t)w[BUSY_WORD];
        step = FPU_STEP_COND;
    } else {
        if (w[BUSY_WORD] > CMD_WORD_MAX || d->tf)
            goto reject;
        if (!mc68k_fpu_decode_command_word(f->config, (uint16_t)w[BUSY_WORD], &d->cmd))
            goto reject;
        d->dk = fpu_dialog_kind(&d->cmd);
        if (d->pc_flag && !((state == STATE_S12 || state == STATE_S18) && dk_has_pc(d->dk)))
            goto reject;
        if (!busy_progress_consistent(f, state, &step))
            goto reject;
        if (!b27 && !(state != STATE_S18 && fpu_command_passes_pending(&d->cmd)))
            goto reject;
    }
    if (!busy_biu_for_state(state, d->dk, &p, &b26) ||
        (w[BUSY_BIU] & BIU_DEFINED_MASK) != (make_biu(false, p, b27, b26) & BIU_DEFINED_MASK))
        goto reject;
    f->arith = b27 ? FPU_ARITH_NONE : FPU_ARITH_PENDING;
    f->context_valid = true;
    fpu_set_step(f, step);
    return true;
reject:
    memset(d, 0, sizeof(*d));
    return false;
}

void fpu_apply_restore_frame(struct mc68k_fpu *f) {
    const uint32_t *w = f->body;
    bool busy = f->restore_busy;
    bool is82 = f->config == MC68K_FPU_68882;
    size_t biu_idx = busy ? BUSY_BIU : (is82 ? IDLE_BIU_INDEX_882 : IDLE_BIU_INDEX_881);
    uint32_t biu = w[biu_idx];
    bool b31 = (biu >> BIU_B31_SHIFT) & 1u;
    unsigned p = (biu >> BIU_P_SHIFT) & BIU_P_MASK;
    bool b27 = (biu >> BIU_B27_SHIFT) & 1u;
    uint32_t image = w[IDLE_IMAGE_INDEX] >> IMAGE_WORD_SHIFT;

    fpu_to_idle(f); /* body は残る（カウンタだけを戻す） */
    if (b31) {
        /* B8: arith を b27 から作り、violation 保留（→S10） */
        f->arith = b27 ? FPU_ARITH_NONE : FPU_ARITH_PENDING;
        f->context_valid = true;
        f->violation_pending = true;
        return;
    }
    if (busy) {
        if (restore_busy_image(f, w, b27))
            return;
    } else if (p == BIU_P_NO_PENDING) {
        /* B2・B3 */
        f->arith = b27 ? FPU_ARITH_NONE : FPU_ARITH_PENDING;
        f->context_valid = true;
        return;
    } else if (p == BIU_P_GENERAL) {
        /* B4・B5: image の command を S2・S3 の command 書込みと同じく開始 */
        f->arith = b27 ? FPU_ARITH_NONE : FPU_ARITH_PENDING;
        if (b27)
            fpu_start_command(f, (uint16_t)image);
        else
            fpu_start_command_pending_rule(f, (uint16_t)image);
        return;
    } else if (p == BIU_P_CONDITIONAL) {
        /* B6・B7 */
        f->arith = b27 ? FPU_ARITH_NONE : FPU_ARITH_PENDING;
        if (b27)
            fpu_start_condition(f, (uint8_t)(image & FPU_PREDICATE_MASK));
        else
            fpu_start_condition_pending_rule(f, (uint8_t)(image & FPU_PREDICATE_MASK));
        return;
    }
    /* B9・B11: 適用しない。begin_restore() が捨てたので arith=NONE、UNSTARTED なしで violation
     * 保留。 context_valid は変えない */
    f->arith = FPU_ARITH_NONE; /* 冗長（残す）: begin_restore() が既に NONE
                                  にしている。ここでも状態を明示する */
    f->violation_pending = true;
}

/* ---- 公開 API ------------------------------------------------------------- */

void mc68k_fpu_begin_save(mc68k_fpu_t *f, uint16_t *format_word, size_t *body_bytes) {
    if (!f || !format_word || !body_bytes)
        return;
    if (f->phase == FPU_PH_SAVE_BODY || f->phase == FPU_PH_RESTORE_BODY) {
        /* 転送中の入れ子の save CIR 読出し: illegal format word
         * で、進行中の転送は変えない（§5.3） */
        *format_word = FORMAT_ILLEGAL;
        *body_bytes = 0;
        return;
    }
    fpu_build_save_frame(f, format_word, body_bytes);
}

void mc68k_fpu_begin_restore(mc68k_fpu_t *f, uint16_t format_word, uint16_t *readback,
                             size_t *body_bytes) {
    bool is_null, is_idle, is_busy;

    if (!f || !readback || !body_bytes)
        return;
    is_null = (format_word >> FORMAT_VERSION_SHIFT) == 0;
    is_idle = format_word == fpu_idle_format_word(f->config);
    is_busy = format_word == fpu_busy_format_word(f->config);
    if (!is_null && !is_idle && !is_busy) {
        /* 不正 format: frame は適用せず phase を idle に戻す。arith・UNSTARTED・violation
         * は変えない */
        *readback = (uint16_t)(FORMAT_ILLEGAL | (format_word & FORMAT_LOW_MASK));
        *body_bytes = 0;
        if (f->phase != FPU_PH_IDLE)
            fpu_to_idle(
                f); /* S8 は UNSTARTED（IA 未充足）を残したまま IDLE に戻り、S6 になる */
        return;
    }
    *readback = format_word;
    /* 有効な format: ダイアログ、arith、UNSTARTED、violation 保留を捨てる（表 B の前置き） */
    fpu_to_idle(f);
    memset(&f->unstarted, 0, sizeof(f->unstarted));
    memset(&f->hold, 0, sizeof(f->hold));
    f->violation_pending = false;
    f->arith = FPU_ARITH_NONE;
    if (is_null) {
        mc68k_fpu_model_reset(f); /* B1 */
        *body_bytes = 0;
        return;
    }
    f->restore_busy = is_busy;
    f->body_words = is_busy ? fpu_busy_body_words(f->config) : fpu_idle_body_words(f->config);
    f->body_pos = 0;
    f->phase = FPU_PH_RESTORE_BODY;
    *body_bytes = f->body_words * sizeof(uint32_t);
}
