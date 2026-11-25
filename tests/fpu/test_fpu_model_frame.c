/*
 * test_fpu_model_frame.c — FSAVE/FRESTORE の state frame（ Phase B、設計書
 * §5.3、§7、§11）。
 *
 * frame の大きさ・format word・read-back、null frame の size byte、busy frame の往復（各 phase
 * を FSAVE→FRESTORE して、同じ setup の双子と同じ応答列・同じ FP レジスタで完了すること）、
 * FSAVE と FRESTORE の間に FPCR/FPCC を変えても保存した primitive/TF が返ること、
 * 不整合な busy frame の拒否（B11）、idle frame の30–28、転送中の入れ子の begin_save()
 * を検査する。 期待値は [881UM Table 5-8、Table 6-6] の大きさと format word を手で書いたもの。
 *
 * 落とす変異: 機種の frame size の取り違え、format word の revision/size byte、read-back
 * を常に書込み値に する、busy frame の FRESTORE で pre-instruction
 * 検査や条件評価をやり直す、固定表現の phase を IDLE に する、b27=0 の busy frame を全 phase
 * で受理する、入れ子の begin_save() で転送を壊す。
 */
#include "fpu/fpu_model.h"
#include "fpu_model_setup.h"

#define MAX_BODY 64

typedef struct {
    uint16_t fmt;
    uint32_t body[MAX_BODY];
    unsigned words;
} frame_t;

static frame_t save(mc68k_fpu_t *f) {
    frame_t fr;
    size_t bytes;
    unsigned i;

    memset(&fr, 0, sizeof(fr));
    mc68k_fpu_begin_save(f, &fr.fmt, &bytes);
    CHECK_EQ(bytes,
             save_body_bytes_any(fr.fmt)); /* null・idle・busy 以外の format は body 無し */
    fr.words = (unsigned)(bytes / 4);
    CHECK(fr.words <= MAX_BODY);
    for (i = 0; i < fr.words; i++)
        CHECK(mc68k_fpu_read_operand(f, &fr.body[i]));
    return fr;
}

/* frame を FRESTORE する。有効なら true */
static int restore(mc68k_fpu_t *f, const frame_t *fr) {
    uint16_t rb;
    size_t bytes;
    unsigned i;

    mc68k_fpu_begin_restore(f, fr->fmt, &rb, &bytes);
    if (rb != fr->fmt)
        return 0;
    CHECK_EQ(bytes, (size_t)fr->words * 4);
    for (i = 0; i < fr->words; i++)
        CHECK(mc68k_fpu_write_operand(f, fr->body[i]));
    return 1;
}

static uint64_t mix(uint64_t h, uint64_t v) {
    return (h ^ v) * UINT64_C(1099511628211);
}

static uint64_t mix_resp(uint64_t h, mc68k_fpu_response_t r) {
    h = mix(h, r.kind);
    h = mix(h, r.coprocessor_active);
    h = mix(h, r.request_pc);
    h = mix(h, r.transfer_to_host);
    h = mix(h, r.length);
    h = mix(h, r.valid_ea);
    h = mix(h, r.data_register);
    h = mix(h, r.condition_true);
    return mix(h, r.vector);
}

/* ダイアログを完了まで進め、観測した値のハッシュを返す。入力 word は固定値 */
static uint64_t continue_dialog(mc68k_fpu_t *f) {
    uint64_t h = UINT64_C(1469598103934665603);
    unsigned guard;

    for (guard = 0; guard < 64; guard++) {
        int s = fpu_state_id(f);
        uint32_t w;
        uint16_t sel;

        if (s == 12 || s == 13 || s == 14) {
            h = mix_resp(h, rd(f));
        } else if (s == 15) {
            CHECK(mc68k_fpu_write_operand(f, 0x000000C0u));
        } else if (s == 16) {
            CHECK(mc68k_fpu_read_operand(f, &w));
            h = mix(h, w);
        } else if (s == 17) {
            CHECK(mc68k_fpu_read_register_select(f, &sel));
            h = mix(h, sel);
        } else if (s == 18) {
            h = mix_resp(h, rd(f));
            write_ia_ok(f);
        } else {
            break;
        }
    }
    CHECK(guard < 64);
    h = mix_resp(h, rd(f)); /* 完了後の応答 */
    {
        mc68k_fpu_regs_t g = regs(f);
        unsigned i;

        for (i = 0; i < 8; i++) {
            h = mix(h, g.fp[i].sign_exp);
            h = mix(h, g.fp[i].signif);
        }
        CHECK_EQ(i, 8); /* ループが縮まない */
        h = mix(h, g.fpcr);
        h = mix(h, g.fpsr);
        h = mix(h, g.fpiar);
    }
    return h;
}

/* ---- frame の大きさと format word ----------------------------------------- */

static void test_sizes_and_format_words(void) {
    static const struct {
        mc68k_fpu_config_t cfg;
        const char *idle_setup;
        unsigned idle_words, busy_words;
        uint16_t idle_fmt, busy_fmt;
    } dev[] = {
        {MC68K_FPU_68881, "s2", 6, 45, 0x1F18, 0x1FB4},  /* 28 / 184 byte */
        {MC68K_FPU_68882, "s2", 14, 53, 0x1F38, 0x1FD4}, /* 60 / 216 byte */
    };
    unsigned i;

    for (i = 0; i < 2; i++) {
        mc68k_fpu_t *f = setup("s1", dev[i].cfg);
        frame_t fr = save(f); /* reset 直後: null */

        CHECK_EQ(fr.fmt, 0);
        CHECK_EQ(fr.words, 0);
        mc68k_fpu_destroy(f);
        f = setup(dev[i].idle_setup, dev[i].cfg);
        fr = save(f);
        CHECK_EQ(fr.fmt, dev[i].idle_fmt);
        CHECK_EQ(fr.words, dev[i].idle_words);
        mc68k_fpu_destroy(f);
        f = setup("s16", dev[i].cfg);
        fr = save(f);
        CHECK_EQ(fr.fmt, dev[i].busy_fmt);
        CHECK_EQ(fr.words, dev[i].busy_words);
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

static void test_read_back(void) {
    unsigned i, v;

    for (i = 0; i < 2; i++) {
        mc68k_fpu_t *f = mk(model_cfg(i));
        uint16_t rb;
        size_t bytes;
        uint16_t good_idle = model_cfg(i) == MC68K_FPU_68881 ? 0x1F18 : 0x1F38;
        uint16_t good_busy = model_cfg(i) == MC68K_FPU_68881 ? 0x1FB4 : 0x1FD4;
        uint16_t other_idle = model_cfg(i) == MC68K_FPU_68881 ? 0x1F38 : 0x1F18;

        /* null: $00xx は size byte によらず全て受理（read-back は書込み値と同値、body 0） */
        for (v = 0; v < 256; v++) {
            mc68k_fpu_begin_restore(f, (uint16_t)v, &rb, &bytes);
            CHECK_EQ(rb, v);
            CHECK_EQ(bytes, 0);
        }
        CHECK_EQ(v, 256); /* ループが縮まない */
        /* 有効な idle/busy は書込み値と同値 */
        mc68k_fpu_begin_restore(f, good_idle, &rb, &bytes);
        CHECK_EQ(rb, good_idle);
        mc68k_fpu_begin_restore(f, good_busy, &rb, &bytes);
        CHECK_EQ(rb, good_busy);
        /* 無効（revision 違い、他機種の size、Come Again、size byte 違い）は上位 byte が $02 */
        {
            static const uint16_t bad[] = {0x3F18, 0x3F38, 0x1F00, 0x1F17, 0x1F19,
                                           0x0100, 0x01FF, 0x2000, 0x7F7F, 0xFFFF,
                                           0x1E18, 0x0200, 0x02FF};
            size_t k;

            CHECK_EQ(sizeof(bad) / sizeof(bad[0]),
                     13u); /* bad の要素数。表の行の削除・ループの縮小を見逃さない */
            for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
                mc68k_fpu_begin_restore(f, bad[k], &rb, &bytes);
                CHECK_EQ(rb >> 8, 0x02);
                CHECK_EQ(bytes, 0);
            }
            CHECK_EQ(k, 13u); /* ループが縮まない */
            mc68k_fpu_begin_restore(f, other_idle, &rb, &bytes);
            CHECK_EQ(rb >> 8, 0x02);
            CHECK_EQ(bytes, 0);
        }
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

/* ---- busy frame の往復 ---------------------------------------------------- */

static void roundtrip(mc68k_fpu_config_t cfg, const char *name) {
    mc68k_fpu_t *donor = setup(name, cfg);
    int state = fpu_state_id(donor);
    frame_t fr = save(donor);
    mc68k_fpu_t *twin_a =
        setup(name, cfg); /* FRESTORE 側（開始状態は donor と同じで、dialog を置換する） */
    mc68k_fpu_t *twin_b = setup(name, cfg); /* 比較対象（FSAVE/FRESTORE を経ない） */

    CHECK_EQ(fr.fmt, busy_format_word(cfg));
    CHECK(restore(twin_a, &fr));
    if (fpu_state_id(twin_a) != state) {
        fprintf(stderr, "往復 %s: 状態 S%d → S%d\n", name, state, fpu_state_id(twin_a));
        exit(1);
    }
    if (continue_dialog(twin_a) != continue_dialog(twin_b)) {
        fprintf(stderr, "往復 %s: 再開後の応答列・FP レジスタが双子と違う\n", name);
        exit(1);
    }
    mc68k_fpu_destroy(donor);
    mc68k_fpu_destroy(twin_a);
    mc68k_fpu_destroy(twin_b);
}

static void test_roundtrips(void) {
    static const char *common[] = {"s12", "s12e", "s12m", "s12c", "s12o", "s12d", "s13",
                                   "s14", "s14d", "s15",  "s15d", "s15r", "s16",  "s16m",
                                   "s17", "s12p", "s14p", "s15p", "s16p", "s17p"};
    static const char *only82[] = {"s12pc", "s18n", "s18e", "s18a", "s18m"};
    unsigned i;
    size_t k;
    unsigned count = 0;

    CHECK_DISTINCT_STR(common);
    CHECK_DISTINCT_STR(only82);
    for (i = 0; i < 2; i++) {
        for (k = 0; k < sizeof(common) / sizeof(common[0]); k++) {
            roundtrip(model_cfg(i), common[k]);
            count++;
        }
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
    for (k = 0; k < sizeof(only82) / sizeof(only82[0]); k++) {
        roundtrip(MC68K_FPU_68882, only82[k]);
        count++;
    }
    CHECK_EQ(count, 2u * 20u + 5u);
}

/* FSAVE と FRESTORE の間に FPCR ENABLE を変えても、COMMAND_PENDING の最初の primitive（PC=1）は
 * 保存した値のまま（再評価しない。§7.1） */
static void test_no_reevaluation(void) {
    unsigned i;

    for (i = 0; i < 2; i++) {
        mc68k_fpu_t *f =
            setup("s12pc", model_cfg(i)); /* ENABLE=DZ で演算を書いた: 最初の NULL は PC=1 */
        frame_t fr = save(f);
        mc68k_fpu_response_t r;

        set_fpcr(f, 0); /* FRESTORE の前に ENABLE を0へ */
        CHECK(restore(f, &fr));
        r = rd(f);
        CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
        CHECK_EQ(r.request_pc, 1); /* 再評価すれば PC=0 になる */
        mc68k_fpu_destroy(f);
        /* CONDITION_PENDING: 条件の真偽が反転する FPCC に変えても、保存した TF が返る */
        f = mk(model_cfg(i));
        set_fpsr(f, FPSR_CC_Z);
        CHECK(mc68k_fpu_write_condition(f, 0x01)); /* EQ: 真 */
        fr = save(f);
        set_fpsr(f, 0);
        CHECK(restore(f, &fr));
        r = rd(f);
        CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
        CHECK_EQ(r.condition_true, 1); /* 再評価すれば偽 */
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

/* ---- b27 と busy frame の phase -------------------------------------------- */

static void test_b27_rules(void) {
    unsigned i;
    size_t k;
    /* b27=0（PENDING）を FMOVEM・control move の phase だけが受理する */
    static const char *accept[] = {"s12m", "s12c", "s12d", "s14d", "s15r", "s16m", "s17"};
    static const char *reject[] = {"s12", "s12e", "s12o", "s13", "s14", "s15", "s16"};

    CHECK_EQ(sizeof(accept) / sizeof(accept[0]), 7u);
    CHECK_EQ(sizeof(reject) / sizeof(reject[0]), 7u);
    CHECK_DISTINCT_STR(accept);
    CHECK_DISTINCT_STR(reject);
    for (i = 0; i < 2; i++) {
        unsigned biu0 = 0; /* busy frame の先頭 longword が BIU flags */

        for (k = 0; k < sizeof(accept) / sizeof(accept[0]); k++) {
            mc68k_fpu_t *f = setup(accept[k], model_cfg(i));
            int state = fpu_state_id(f);
            frame_t fr = save(f);

            fr.body[biu0] &= ~(1u << 27);
            CHECK(restore(f, &fr));
            CHECK_EQ(fpu_state_id(f), state);
            mc68k_fpu_destroy(f);
        }
        CHECK_EQ(k, 7u); /* ループが縮まない */
        for (k = 0; k < sizeof(reject) / sizeof(reject[0]); k++) {
            mc68k_fpu_t *f = setup(reject[k], model_cfg(i));
            frame_t fr = save(f);

            fr.body[biu0] &= ~(1u << 27);
            CHECK(restore(f, &fr));
            CHECK_EQ(fpu_state_id(f), 10); /* B11: violation */
            mc68k_fpu_destroy(f);
        }
        CHECK_EQ(k, 7u); /* ループが縮まない */
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
    {
        mc68k_fpu_t *f = setup("s18n", MC68K_FPU_68882);
        frame_t fr = save(f);

        fr.body[0] &= ~(1u << 27);
        CHECK(restore(f, &fr));
        CHECK_EQ(fpu_state_id(f), 10);
        mc68k_fpu_destroy(f);
    }
}

/* ---- 不整合な busy frame の拒否（B11。第23回 A-N117） --------------------------- */

typedef struct {
    const char *setup;
    unsigned index;    /* 壊す longword */
    uint32_t xor_mask; /* 壊し方 */
    const char *why;
} corrupt_t;

static void test_corrupt_busy_frames(void) {
    /* 固定表現: [0]=BIU [1]=magic [2]=state [3]=command word [4]=flags [5]=cnt|total<<8
     * [6][7]=予約 [8..]=buf */
    static const corrupt_t cases[] = {
        {"s16", 1, 1, "magic"},
        {"s16", 2, 0x20, "phase の番号が範囲外"},
        {"s16", 2, 0x1, "phase が command と合わない（S17）"},
        {"s16", 3, 0xFFFF0000u, "command word が16bitを超える"},
        {"s16", 3, 0x2000 ^ 0x6000, "command word が不正（class 001）"},
        {"s16", 5, 0x0100, "total が出力 word 数と合わない"},
        {"s16", 5, 0x0200, "total が出力 word 数より多い"},
        {"s16", 5, 0x0001,
         "cnt を total に揃える（wedged。test_wedged_frames_rejected と同じ種類）"},
        {"s15", 5, 0x0300, "total が入力 word 数より多い（cnt<total のまま）"},
        {"s16", 6, 1, "予約欄"},
        {"s16", 7, 1, "予約欄"},
        {"s16", 0, 1u << 28, "BIU の 30–28 が phase と合わない"},
        {"s16", 0, 1u << 26, "BIU の bit26 が phase と合わない"},
        {"s16", 4, 0x1, "TF が S16 にある"},
        {"s16", 18, 1, "転送 word 列の total 以降が0でない"},
        {"s16", 5, 1u << 16, "counts の未使用 bit"},
        {"s16", 4, 1u << 24, "flags の未使用 bit"},
        {"s12", 4, 1u << 8, "S12 の動的 Dn の値が非0"},
        {"s14d", 4, 0x4, "S14（動的 list）に mid_exc"},
        {"s15d", 4, 1u << 8, "S15（Dn 待ち）の動的 Dn の値が非0"},
        {"s16p", 4, 0x4, "S16（control 出力）に mid_exc"},
        {"s17", 5, 0x0101, "S17 の cnt・total が非0（cnt<=total のまま）"},
        {"s17", 5, 0x0100, "S17 の total が非0"},
        {"s13", 4, 0x2, "S13 に pc_flag"},
        {"s13", 4, 0x4, "S13 に mid_exc"},
        {"s15", 5, 0x0100, "total が入力 word 数と合わない"},
        {"s15r", 4, 0x10000, "mask が static list と合わない"},
        {"s17", 4, 0x10000, "mask が static list と合わない"},
        {"s13", 3, 0x40, "predicate が6bitを超える"},
        {"s12", 4, 0x4, "mid_exc が COMMAND_PENDING にある"},
        {"s12", 4, 0x8, "mask_known が COMMAND_PENDING にある"},
        {"s14", 4, 0x2, "pc_flag は COMMAND_PENDING と PC だけ"},
    };
    size_t k;
    unsigned rejected = 0;

    CHECK_EQ(sizeof(cases) / sizeof(cases[0]), 32u);
    for (k = 0; k < 2 * sizeof(cases) / sizeof(cases[0]); k++) {
        mc68k_fpu_config_t cfg = (k & 1) ? MC68K_FPU_68882 : MC68K_FPU_68881;
        const corrupt_t *c = &cases[k / 2];
        mc68k_fpu_t *f = setup(c->setup, cfg);
        frame_t fr = save(f);

        fr.body[c->index] ^= c->xor_mask;
        CHECK(restore(f, &fr));
        if (fpu_state_id(f) != 10) {
            fprintf(stderr, "壊れた busy frame を受理した: %s（%s）\n", c->setup, c->why);
            exit(1);
        }
        rejected++;
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(k, 64u);        /* ループが縮まない */
    CHECK_EQ(rejected, 64u); /* 32 ケース × 2機種。ループを縮めても通らないようにする */
}

/* BIU の bit25–16 と bit15–0 は restore で無視する（p.6-33）。壊しても受理して再開する */
static void test_busy_biu_undefined_bits_ignored(void) {
    unsigned i;

    for (i = 0; i < 2; i++) {
        mc68k_fpu_t *f = setup("s16", model_cfg(i));
        frame_t fr = save(f);

        fr.body[0] ^= (1u << 5) | (1u << 20);
        CHECK(restore(f, &fr));
        CHECK_EQ(fpu_state_id(f), 16);
        mc68k_fpu_destroy(f);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

/* 68881 の frame に S18（68882 だけの PC phase）を書き込むと B11（68881 は PC に入れない） */
static void test_s18_rejected_on_68881(void) {
    mc68k_fpu_t *f = setup("s12pc", MC68K_FPU_68881);
    frame_t fr = save(f);

    fr.body[2] = 18;
    CHECK(restore(f, &fr));
    CHECK_EQ(fpu_state_id(f), 10);
    mc68k_fpu_destroy(f);
}

/* S15・S16 で残りの転送が0（cnt==total）の frame を B10 で受理すると詰まる（A-N117） */
static void test_wedged_frames_rejected(void) {
    unsigned i;
    static const char *const wedged_names[4] = {"s15r", "s16m", "s15",
                                                "s16"}; /* FMOVEM 入力・出力、EA 入力・出力 */

    CHECK_DISTINCT_STR(wedged_names);
    for (i = 0; i < 2; i++) {
        const char *const *names = wedged_names;
        unsigned k;

        for (k = 0; k < 4; k++) {
            mc68k_fpu_t *f = setup(names[k], model_cfg(i));
            frame_t fr = save(f);
            unsigned total = (fr.body[5] >> 8) & 0xFF;

            fr.body[5] = total | (total << 8); /* cnt==total */
            CHECK(restore(f, &fr));
            CHECK_EQ(fpu_state_id(f), 10);
            mc68k_fpu_destroy(f);
        }
        CHECK_EQ(k, 4u); /* names[] の4個を全て回る */
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

/* ---- idle frame の30–28 ----------------------------------------------------- */

static void test_idle_pending_operation(void) {
    unsigned i, p;

    for (i = 0; i < 2; i++) {
        unsigned biu = i == 0 ? 5 : 13;

        for (p = 0; p < 8; p++) {
            mc68k_fpu_t *f = mk(model_cfg(i));
            frame_t fr;
            int accepted = (p == 7 || p == 3 || p == 1);

            memset(&fr, 0, sizeof(fr));
            fr.fmt = idle_format_word(model_cfg(i));
            fr.words = biu + 1;
            fr.body[0] = (uint32_t)(p == 3 ? CMD(0, 1, 0, OPM_FADD) : (p == 1 ? 0x01 : 0))
                         << 16;
            fr.body[biu] = ((uint32_t)p << 28) | (1u << 27) | (1u << 26) | 0xFFFFu;
            CHECK(restore(f, &fr));
            /* 受理: violation にならない（S10/S11 でない）。拒否（B9）: violation */
            CHECK_EQ(fpu_state_id(f) == 10 || fpu_state_id(f) == 11, !accepted);
            mc68k_fpu_destroy(f);
        }
        CHECK_EQ(p, 8); /* ループが縮まない */
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
}

/* ---- 転送中の入れ子の begin_save() -------------------------------------------- */

static void test_nested_begin_save(void) {
    unsigned i, k;
    unsigned nested_save_body = 0, nested_restore_body = 0;

    for (i = 0; i < 2; i++) {
        mc68k_fpu_t *f = setup("s16", model_cfg(i));
        mc68k_fpu_t *g = setup("s16", model_cfg(i));
        frame_t whole = save(g);
        uint16_t fmt;
        size_t bytes;
        uint32_t w;
        frame_t part;

        /* SAVE_BODY 中の入れ子: $0200、body 0 で、転送は壊れない */
        mc68k_fpu_begin_save(f, &fmt, &bytes);
        memset(&part, 0, sizeof(part));
        part.fmt = fmt;
        part.words = (unsigned)(bytes / 4);
        CHECK_EQ(part.words, whole.words);
        for (k = 0; k < part.words; k++) {
            if (k == 3) {
                uint16_t nfmt;
                size_t nbytes;

                mc68k_fpu_begin_save(f, &nfmt, &nbytes);
                CHECK_EQ(nfmt, 0x0200);
                CHECK_EQ(nbytes, 0);
                nested_save_body++;
            }
            CHECK(mc68k_fpu_read_operand(f, &part.body[k]));
        }
        CHECK_EQ(memcmp(part.body, whole.body, part.words * 4), 0);
        /* body を読み終えた後の operand 読出しは violation */
        CHECK(!mc68k_fpu_read_operand(f, &w));
        mc68k_fpu_destroy(f);
        mc68k_fpu_destroy(g);
        /* RESTORE_BODY 中の入れ子: $0200 で、残りの body を受けて frame が適用される */
        f = setup("s2", model_cfg(i));
        g = setup("s16", model_cfg(i));
        whole = save(g);
        {
            uint16_t rb;

            mc68k_fpu_begin_restore(f, whole.fmt, &rb, &bytes);
            for (k = 0; k < whole.words; k++) {
                if (k == 5) {
                    mc68k_fpu_begin_save(f, &fmt, &bytes);
                    CHECK_EQ(fmt, 0x0200);
                    CHECK_EQ(bytes, 0);
                    CHECK_EQ(fpu_state_id(f), 20);
                    nested_restore_body++;
                }
                CHECK(mc68k_fpu_write_operand(f, whole.body[k]));
            }
        }
        CHECK_EQ(fpu_state_id(f), 16); /* S16 を再開 */
        /* body を受けた後の operand 書込みは violation */
        CHECK(!mc68k_fpu_write_operand(f, 0));
        mc68k_fpu_destroy(f);
        mc68k_fpu_destroy(g);
    }
    CHECK_EQ(i, 2); /* ループが縮まない */
    CHECK_EQ(nested_save_body,
             2u); /* 入れ子の begin_save()
                     を実際に挟んだ回数（2機種）。条件が外れても通らないようにする */
    CHECK_EQ(nested_restore_body, 2u);
}

int main(void) {
    test_sizes_and_format_words();
    test_read_back();
    test_roundtrips();
    test_no_reevaluation();
    test_b27_rules();
    test_corrupt_busy_frames();
    test_wedged_frames_rejected();
    test_busy_biu_undefined_bits_ignored();
    test_s18_rejected_on_68881();
    test_idle_pending_operation();
    test_nested_begin_save();
    printf("test_fpu_model_frame: OK\n");
    return 0;
}
