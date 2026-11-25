/*
 * fpu_model_setup.h — コプロセッサモデルの試験が使う「状態への到達手順」。
 *
 * 設計書 §4.2 の表 A の状態 S1〜S20 へ、公開 API だけで到達する。内部欄を直接書かない。
 * setup 名の意味は scripts/gen_fpu2_take_transition.py の setup 列と同じ。
 */
#pragma once
#include "fpu_model_util.h"

#define IA_VALUE 0x00ABCD00u

static inline uint16_t idle_format_word(mc68k_fpu_config_t c) {
    return c == MC68K_FPU_68882 ? 0x1F38 : 0x1F18;
}

/* idle frame の body（format longword の後ろ）での BIU flags の longword 番号。68881 は
 * +$18、68882 は +$38 （[881UM] Figure 6-4・6-5）。busy frame はモデル固定表現の先頭
 * longword（設計書 §7.1） */
static inline unsigned idle_biu_index(mc68k_fpu_config_t c) {
    return c == MC68K_FPU_68882 ? 13u : 5u;
}

#define BUSY_BIU_INDEX 0u

static inline uint16_t busy_format_word(mc68k_fpu_config_t c) {
    return c == MC68K_FPU_68882 ? 0x1FD4 : 0x1FB4;
}

static inline int is82(mc68k_fpu_config_t c) {
    return c == MC68K_FPU_68882;
}

/* FSAVE の body の byte 数（format longword の後ろ）。idle: 68881 24・68882 56（[881UM]
 * §6.4.2.2、 印字 p.6-32）、busy: 68881 180（§6.4.2.3、p.6-35）・68882 212（同、p.6-36）。Table
 * 5-8（p.5-11）の frame 全体の大きさ 28・60・184・216 から format longword の 4
 * を引いた値と一致する。format word が idle・busy でなければ 0。BIU flags
 * の位置（idle_biu_index）を読む前に、読んだ body がこの長さである ことを確かめるために使う */
static inline size_t save_body_bytes(mc68k_fpu_config_t c, uint16_t fmt) {
    if (fmt == idle_format_word(c))
        return is82(c) ? 56 : 24;
    if (fmt == busy_format_word(c))
        return is82(c) ? 212 : 180;
    return 0;
}

/* 機種を渡せない save() 用。format word は機種ごとに異なる（idle 0x1F18/0x1F38、busy
 * 0x1FB4/0x1FD4）ので、 word から body 長が決まる */
static inline size_t save_body_bytes_any(uint16_t fmt) {
    size_t b = save_body_bytes(MC68K_FPU_68881, fmt);

    return b ? b : save_body_bytes(MC68K_FPU_68882, fmt);
}

static inline void write_ia_ok(mc68k_fpu_t *f) {
    CHECK(mc68k_fpu_write_instruction_address(f, IA_VALUE));
}

/* ---- setup: API だけで状態へ到達する --------------------------------------- */

static inline void do_cmd(mc68k_fpu_t *f, uint16_t w) {
    CHECK(mc68k_fpu_write_command_word(f, w));
}

/* S3: DZ を enable して FDIV.L #0,FP0（FP0=1）。保留算術例外（候補あり）。応答は PC=1 の
 * EA_TRANSFER */
static inline mc68k_fpu_t *to_s3(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    mc68k_fpu_response_t r;

    load_long(f, 0, 1);
    set_fpcr(f, FPCR_ENABLE(ENABLE_DZ));
    do_cmd(f, CMD(2, FMT_L, 0, OPM_FDIV));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.request_pc, 1);
    if (is82(cfg))
        write_ia_ok(f); /* S18 → S15 */
    CHECK(mc68k_fpu_write_operand(f, 0));
    return f;
}

static inline mc68k_fpu_t *to_s6(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);

    drive_regreg(f, CMD(0, 0, 0, OPM_FTST)); /* FP0 は reset 後の NaN。FPCC の NAN が立つ */
    set_fpcr(f, FPCR_ENABLE(ENABLE_BSUN));
    CHECK(mc68k_fpu_write_condition(
        f, 0x12)); /* GT（signaling）: NAN で BSUN、enable 済みで TAKE_PRE */
    return f;
}

static inline mc68k_fpu_t *to_s9(mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f = mk(cfg);
    mc68k_fpu_response_t r;
    uint32_t w;

    set_fpcr(f, FPCR_ENABLE(ENABLE_OPERR));
    do_cmd(f, CMD(3, FMT_L, 0, 0)); /* FMOVE.L FP0,<ea>。FP0 は NaN で OPERR */
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
    CHECK_EQ(r.coprocessor_active, 1);
    CHECK_EQ(r.request_pc, 1);
    if (is82(cfg))
        write_ia_ok(f);
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK(mc68k_fpu_read_operand(f, &w));
    return f;
}

static inline mc68k_fpu_t *to_s18(mc68k_fpu_config_t cfg, char kind) {
    mc68k_fpu_t *f = mk(cfg);
    mc68k_fpu_response_t r;

    set_fpcr(f, FPCR_ENABLE(ENABLE_DZ));
    switch (kind) {
    case 'n':
        do_cmd(f, CMD(0, 1, 0, OPM_FADD));
        break;
    case 'e':
        do_cmd(f, CMD(3, FMT_L, 0, 0));
        break;
    case 'a':
        do_cmd(f, CMD(2, FMT_L, 0, OPM_FMOVE));
        break;
    default:
        do_cmd(f, CMD(3, 7, 0, 1 << 4));
        break; /* 'm': FMOVE.P FP0,<ea>{D1} */
    }
    r = rd(f);
    CHECK_EQ(r.request_pc, 1);
    return f;
}

static inline mc68k_fpu_t *setup(const char *name, mc68k_fpu_config_t cfg) {
    mc68k_fpu_t *f;
    mc68k_fpu_response_t r;
    uint16_t fmt;
    size_t bytes;
    uint32_t w;

    if (!strcmp(name, "s1"))
        return mk(cfg);
    if (!strcmp(name, "s2") || !strcmp(name, "s13") || !strcmp(name, "s13t") ||
        !strcmp(name, "s19") || !strcmp(name, "s20") || !strcmp(name, "s10n")) {
        f = mk(cfg);
        CHECK(mc68k_fpu_write_condition(f, !strcmp(name, "s2") || !strcmp(name, "s13t")
                                               ? 0x0F
                                               : 0x00)); /* s2・s13t は T（TF=1） */
        if (!strcmp(name, "s13") || !strcmp(name, "s13t"))
            return f;
        expect_null(f, 0, 0);
        if (!strcmp(name, "s19")) {
            mc68k_fpu_begin_save(f, &fmt, &bytes);
        } else if (!strcmp(name, "s20")) {
            size_t b;
            uint16_t rb;

            mc68k_fpu_begin_restore(f, idle_format_word(cfg), &rb, &b);
        } else if (!strcmp(name, "s10n")) {
            CHECK(!mc68k_fpu_write_operand(f, 0));
        }
        return f;
    }
    if (!strcmp(name, "s3"))
        return to_s3(cfg);
    if (!strcmp(name, "s19p") || !strcmp(name, "s20p")) {
        uint16_t rb;
        size_t b;

        f = to_s3(cfg);
        if (!strcmp(name, "s19p"))
            mc68k_fpu_begin_save(f, &fmt, &bytes);
        else
            mc68k_fpu_begin_restore(f, idle_format_word(cfg), &rb, &b);
        return f;
    }
    if (!strcmp(name, "s3n") || !strcmp(name, "s4") || !strcmp(name, "s4c") ||
        !strcmp(name, "s9p") || !strcmp(name, "s10p") || !strcmp(name, "s12p") ||
        !strcmp(name, "s14p") || !strcmp(name, "s15p") || !strcmp(name, "s16p") ||
        !strcmp(name, "s17p")) {
        f = to_s3(cfg);
        if (!strcmp(name, "s3n")) {
            set_fpcr(f, 0); /* control move は PENDING を素通し。候補（EXC&ENABLE）が消える */
        } else if (!strcmp(name, "s4") || !strcmp(name, "s9p")) {
            do_cmd(f, CMD(0, 1, 0, OPM_FADD));
            if (!strcmp(name, "s9p"))
                mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK); /* 68882: S4 → S9 */
        } else if (!strcmp(name, "s4c")) {
            CHECK(mc68k_fpu_write_condition(f, 0x01));
        } else if (!strcmp(name, "s10p")) {
            CHECK(!mc68k_fpu_write_operand(f, 0));
        } else if (!strcmp(name, "s12p")) {
            do_cmd(f, 0xF080); /* FMOVEM FP0,<ea>（静的 list） */
        } else if (!strcmp(name, "s14p")) {
            do_cmd(f, 0xF000 | 0x0800 |
                          (3 << 4)); /* FMOVEM 出力、動的 list（postincrement）、D3 */
            r = rd(f);
            CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_MP_REGISTER);
            CHECK(mc68k_fpu_write_operand(f, 0x80));
        } else if (!strcmp(name, "s15p")) {
            uint16_t sel;

            do_cmd(f, 0xD000 | 0x80); /* FMOVEM <ea>,FP0（静的 list） */
            r = rd(f);
            CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_FP_REGISTERS);
            CHECK(mc68k_fpu_read_register_select(f, &sel));
        } else if (!strcmp(name, "s16p")) {
            do_cmd(f, CMD(5, CTL_FPCR, 0, 0));
            r = rd(f);
            CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
        } else if (!strcmp(name, "s17p")) {
            do_cmd(f, 0xF080);
            r = rd(f);
            CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_FP_REGISTERS);
        }
        return f;
    }
    /* protocol 行列（tests/fpu/test_fpu_model_protocol.c）の追加 setup */
    if (!strncmp(name, "s12", 3) && name[3]) {
        f = mk(cfg);
        if (!strcmp(name, "s12pc"))
            set_fpcr(f, FPCR_ENABLE(ENABLE_DZ)); /* 例外 enable で最初の primitive が PC=1 */
        if (name[3] != 'p')
            CHECK(!name[4]); /* 1文字の接尾辞（s12e など）の後ろに文字を許さない */
        switch (name[3]) {
        case 'e':
            do_cmd(f, CMD(2, FMT_L, 0, OPM_FMOVE));
            break; /* EA_TRANSFER（DR=0） */
        case 'm':
            do_cmd(f, 0xF080);
            break; /* FP_REGISTERS（静的 list） */
        case 'c':
            do_cmd(f, CMD(5, CTL_FPCR, 0, 0));
            break; /* EA_TRANSFER（DR=1、control 出力） */
        case 'o':
            do_cmd(f, CMD(3, FMT_L, 0, 0));
            break; /* 外部宛て FMOVE の NULL（CA=1） */
        case 'd':
            do_cmd(f, 0xF000 | 0x0800 | (3 << 4));
            break; /* MP_REGISTER（動的 list） */
        case 'p':
            CHECK(!strcmp(name, "s12pc"));     /* 未知の接尾辞を演算へ流さない */
            do_cmd(f, CMD(0, 1, 0, OPM_FADD)); /* s12pc: 演算（PC=1 の NULL） */
            break;
        default:
            CHECK(0); /* 未知の setup 名 */
        }
        return f;
    }
    if (!strcmp(name, "sq12") || !strcmp(name, "sq18")) {
        f = mk(cfg);
        load_long(f, 0, 1);
        load_long(f, 1, 0);
        set_fpcr(f, FPCR_ENABLE(ENABLE_DZ));
        do_cmd(f, CMD(0, 1, 0,
                      OPM_FDIV)); /* FP0 / FP1 = 1/0: DZ。要求の無い命令の最初の NULL は PC=1 */
        if (!strcmp(name, "sq18"))
            rd_kind(f, MC68K_FPU_RESPONSE_NULL);
        return f;
    }
    if (!strcmp(name, "s2l0")) {
        uint16_t sel;

        f = mk(cfg);
        do_cmd(f, 0xF000); /* FMOVEM 出力、静的 list（postincrement）、list 0 */
        r = rd(f);
        CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_FP_REGISTERS);
        CHECK(mc68k_fpu_read_register_select(f, &sel));
        return f;
    }
    if (!strcmp(name, "s14d") || !strcmp(name, "s15d") || !strcmp(name, "s15r") ||
        !strcmp(name, "s16m")) {
        uint16_t sel;

        f = mk(cfg);
        if (!strcmp(name, "s14d")) {
            do_cmd(f, 0xF000 | 0x0800 | (3 << 4));
            rd_kind(f, MC68K_FPU_RESPONSE_MP_REGISTER); /* 動的 list は CPU レジスタから mask
                                                           を受け取る */
            CHECK(mc68k_fpu_write_operand(f, 0x80));
        } else if (!strcmp(name, "s15d")) {
            do_cmd(f, CMD(3, 7, 0, 1 << 4)); /* FMOVE.P FP0,<ea>{D1} */
            r = rd(f);
            CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_MP_REGISTER);
        } else {
            do_cmd(f, !strcmp(name, "s15r") ? (0xD000 | 0x80) : 0xF080);
            rd_kind(f, MC68K_FPU_RESPONSE_FP_REGISTERS);
            CHECK(mc68k_fpu_read_register_select(f, &sel));
        }
        return f;
    }
    if (!strcmp(name, "s5")) {
        f = mk(cfg);
        do_cmd(f, 0x2000);
        return f;
    }
    if (!strcmp(name, "s6") || !strcmp(name, "s7") || !strcmp(name, "s8")) {
        f = to_s6(cfg);
        if (!strcmp(name, "s7"))
            write_ia_ok(f);
        else if (!strcmp(name, "s8"))
            rd_kind(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION);
        return f;
    }
    if (!strcmp(name, "s9"))
        return to_s9(cfg);
    if (!strcmp(name, "s10m")) {
        f = to_s9(cfg);
        CHECK(!mc68k_fpu_write_operand(f, 0));
        return f;
    }
    if (!strcmp(name, "s11")) {
        f = mk(cfg);
        CHECK(!mc68k_fpu_write_operand(f, 0));
        return f;
    }
    if (!strcmp(name, "s12")) {
        f = mk(cfg);
        do_cmd(f, CMD(0, 1, 0, OPM_FADD));
        return f;
    }
    if (!strcmp(name, "s14") || !strcmp(name, "s16")) {
        f = mk(cfg);
        do_cmd(f, CMD(3, FMT_L, 0, 0));
        expect_null(f, 1, 0);
        if (!strcmp(name, "s16")) {
            r = rd(f);
            CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
        }
        return f;
    }
    if (!strcmp(name, "s15")) {
        f = mk(cfg);
        do_cmd(f, CMD(2, FMT_L, 0, OPM_FMOVE));
        r = rd(f);
        CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
        return f;
    }
    if (!strcmp(name, "s17")) {
        f = mk(cfg);
        do_cmd(f, 0xF080);
        r = rd(f);
        CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_FP_REGISTERS);
        return f;
    }
    if (!strcmp(name, "s18n") || !strcmp(name, "s18e") || !strcmp(name, "s18a") ||
        !strcmp(name, "s18m"))
        return to_s18(cfg, name[3]);
    if (!strcmp(name, "s20f")) {
        size_t b;
        uint16_t rb;

        f = mk(cfg);
        mc68k_fpu_begin_restore(f, idle_format_word(cfg), &rb, &b);
        return f;
    }
    (void)w;
    fprintf(stderr, "unknown setup %s\n", name);
    exit(1);
}
