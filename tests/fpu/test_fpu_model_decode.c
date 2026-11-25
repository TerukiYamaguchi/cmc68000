/*
 * test_fpu_model_decode.c — 共有デコーダ mc68k_fpu_decode_command_word()（、設計書
 * §5、§6、§11）。
 *
 * 全 65536 語を両機種で、手書きの規則（クラス、opmode の集合）と照合し、descriptor の欄を
 * 手で求めた表で検査する。予約ビットは0でなくても F-line にならず、descriptor は変わらない
 * （[881UM] Table 4-16 Note 2・3、p.4-130〜133）。k-factor は7bit の2の補数で符号拡張する。
 * opmode の集合は [881UM] の命令一覧（FMOVE〜FTST）から手で書いた。
 *
 * 落とす変異: descriptor の欄の取り違え・同一視（FP0 source と EA source、静的と動的の
 * k-factor）、 予約ビットを不正にする、k-factor の符号拡張の欠落、movecr_offset の幅、FMOVEM の
 * mode の読み誤り。
 */
#include "fpu_model_util.h"

static int opmode_valid(unsigned op) {
    static const unsigned valid[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x06, 0x08, 0x09, 0x0A, 0x0C, 0x0D, 0x0E,
        0x0F, 0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x18, 0x19, 0x1A, 0x1C, 0x1D,
        0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x30,
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x3A,
    };
    size_t i;

    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        if (valid[i] == op)
            return 1;
    return 0;
}

/* 手書きの有効判定 */
static int expected_valid(unsigned w) {
    unsigned cls = w >> 13;
    unsigned field = (w >> 10) & 7;

    switch (cls) {
    case 0:
        return opmode_valid(w & 0x7F);
    case 2:
        return field == 7 ? 1 : opmode_valid(w & 0x7F);
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
        return 1;
    default:
        return 0; /* 001、および bit15-13=1xx のうち 100〜111 以外は無い */
    }
}

static void test_exhaustive(void) {
    unsigned c, w, valid_count = 0;

    for (c = 0; c < 2; c++) {
        valid_count = 0;
        for (w = 0; w < 65536; w++) {
            mc68k_fpu_command_t d;
            int ok = mc68k_fpu_decode_command_word(model_cfg(c), (uint16_t)w, &d);

            CHECK_EQ(ok, expected_valid(w));
            CHECK_EQ(d.valid, ok);
            CHECK_EQ(d.command_word, w);
            valid_count += (unsigned)ok;
            if (!ok) {
                mc68k_fpu_command_t z;

                memset(&z, 0, sizeof(z));
                z.command_word = (uint16_t)w;
                CHECK_EQ(memcmp(&d, &z, sizeof(z)), 0); /* 不正 word の他の欄は0 */
            }
        }
        CHECK_EQ(w, 65536); /* ループが縮まない */
        /* 手で導出した有効語数: class 000 は 8×8×45（opmode 45種）、class 010 は 7形式×8×45 と
         * FMOVECR の 8×128、 class 011〜111 は各 8192 語 */
        CHECK_EQ(valid_count, 8u * 8u * 45u + 7u * 8u * 45u + 8u * 128u + 5u * 8192u);
    }
    CHECK_EQ(c, 2); /* ループが縮まない */
    {
        mc68k_fpu_command_t d;

        CHECK(!mc68k_fpu_decode_command_word(MC68K_FPU_NONE, 0x0022,
                                             &d)); /* 構成なしは常に不正 */
        CHECK(!d.valid);
        CHECK(!mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0x0022, NULL));
    }
}

typedef struct {
    uint16_t word;
    mc68k_fpu_command_class_t cls;
    mc68k_fpu_source_kind_t src;
    unsigned opmode, dst, src_fp, fmt, ext;
    int to_ext;
} case_t;

static void test_fields(void) {
    static const case_t cases[] = {
        /* FADD FP1,FP0（class 000）と FADD.X <ea>,FP0（class 010）は source_kind で区別される
         */
        {0x0422, MC68K_FPU_COMMAND_ARITHMETIC, MC68K_FPU_SOURCE_FP_REGISTER, 0x22, 0, 1,
         MC68K_FPU_FORMAT_X, 0, 0},
        {0x4822, MC68K_FPU_COMMAND_ARITHMETIC, MC68K_FPU_SOURCE_EXTERNAL, 0x22, 0, 0,
         MC68K_FPU_FORMAT_X, 0, 0},
        /* FMOVE FP0,FP1 は FP0 を source とし、FP0 source と EA source が潰れない */
        {0x0080, MC68K_FPU_COMMAND_MOVE, MC68K_FPU_SOURCE_FP_REGISTER, 0x00, 1, 0,
         MC68K_FPU_FORMAT_X, 0, 0},
        {0x4080, MC68K_FPU_COMMAND_MOVE, MC68K_FPU_SOURCE_EXTERNAL, 0x00, 1, 0,
         MC68K_FPU_FORMAT_L, 0, 0},
        /* 形式コード: L,S,X,P,W,D,B = 0..6 */
        {0x4400, MC68K_FPU_COMMAND_MOVE, MC68K_FPU_SOURCE_EXTERNAL, 0x00, 0, 0,
         MC68K_FPU_FORMAT_S, 0, 0},
        {0x4C00, MC68K_FPU_COMMAND_MOVE, MC68K_FPU_SOURCE_EXTERNAL, 0x00, 0, 0,
         MC68K_FPU_FORMAT_P, 0, 0},
        {0x5000, MC68K_FPU_COMMAND_MOVE, MC68K_FPU_SOURCE_EXTERNAL, 0x00, 0, 0,
         MC68K_FPU_FORMAT_W, 0, 0},
        {0x5400, MC68K_FPU_COMMAND_MOVE, MC68K_FPU_SOURCE_EXTERNAL, 0x00, 0, 0,
         MC68K_FPU_FORMAT_D, 0, 0},
        {0x5800, MC68K_FPU_COMMAND_MOVE, MC68K_FPU_SOURCE_EXTERNAL, 0x00, 0, 0,
         MC68K_FPU_FORMAT_B, 0, 0},
        /* FMOVE FP2,<ea>（出力）。source は FPU 内部の FP2 */
        {0x6500, MC68K_FPU_COMMAND_MOVE, MC68K_FPU_SOURCE_FP_REGISTER, 0x00, 0, 2,
         MC68K_FPU_FORMAT_S, 0, 1},
        /* FMOVECR #$0B,FP3: IMMEDIATE */
        {0x5D8B, MC68K_FPU_COMMAND_MOVE, MC68K_FPU_SOURCE_IMMEDIATE, 0x00, 3, 0,
         MC68K_FPU_FORMAT_X, 0, 0},
        /* control: FPCR,FPSR,FPIAR = 4,2,1 */
        {0x9000, MC68K_FPU_COMMAND_MOVE_CONTROL, MC68K_FPU_SOURCE_EXTERNAL, 0, 0, 0,
         MC68K_FPU_FORMAT_L, 4, 0},
        {0xB400, MC68K_FPU_COMMAND_MOVE_CONTROL, MC68K_FPU_SOURCE_FP_REGISTER, 0, 0, 0,
         MC68K_FPU_FORMAT_L, 5, 1},
        {0xBC00, MC68K_FPU_COMMAND_MOVE_CONTROL, MC68K_FPU_SOURCE_FP_REGISTER, 0, 0, 0,
         MC68K_FPU_FORMAT_L, 7, 1},
        {0x8400, MC68K_FPU_COMMAND_MOVE_CONTROL, MC68K_FPU_SOURCE_EXTERNAL, 0, 0, 0,
         MC68K_FPU_FORMAT_L, 1, 0},
        /* select 000 は FPIAR だけと同じ（Table 4-17 Note 2） */
        {0x8000, MC68K_FPU_COMMAND_MOVE_CONTROL, MC68K_FPU_SOURCE_EXTERNAL, 0, 0, 0,
         MC68K_FPU_FORMAT_L, 1, 0},
        /* FMOVEM */
        {0xD080, MC68K_FPU_COMMAND_MOVEM, MC68K_FPU_SOURCE_EXTERNAL, 0, 0, 0,
         MC68K_FPU_FORMAT_X, 0, 0},
        {0xF080, MC68K_FPU_COMMAND_MOVEM, MC68K_FPU_SOURCE_FP_REGISTER, 0, 0, 0,
         MC68K_FPU_FORMAT_X, 0, 1},
    };
    size_t i;

    CHECK_EQ(sizeof(cases) / sizeof(cases[0]),
             18u); /* cases の要素数。表の行の削除・ループの縮小を見逃さない */
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mc68k_fpu_command_t d;
        const case_t *c = &cases[i];

        CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68882, c->word, &d));
        CHECK_EQ(d.command_class, c->cls);
        CHECK_EQ(d.source_kind, c->src);
        CHECK_EQ(d.opmode, c->opmode);
        CHECK_EQ(d.dst_fp, c->dst);
        CHECK_EQ(d.src_fp, c->src_fp);
        CHECK_EQ(d.format, c->fmt);
        CHECK_EQ(d.transfer_to_external, c->to_ext);
        CHECK_EQ(d.control_mask, c->ext);
    }
    CHECK_EQ(i, 18u); /* ループが縮まない */
}

static void test_special_fields(void) {
    mc68k_fpu_command_t d;

    /* FMOVECR の offset は7bit（$00〜$7F）。$40 以上も有効 */
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0x5C7F, &d));
    CHECK_EQ(d.movecr_offset, 0x7F);
    /* FSINCOS: opmode $30+FPc の下位3bit が secondary_dst_fp */
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0x0537, &d)); /* FP1 → FP2:FP7 */
    CHECK_EQ(d.dst_fp, 2);
    CHECK_EQ(d.secondary_dst_fp, 7);
    CHECK_EQ(d.src_fp, 1);
    /* class 010 の FSINCOS も secondary_dst_fp を持つ: FSINCOS.X <ea>,FP2:FP7 */
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0x4937, &d));
    CHECK_EQ(d.source_kind, MC68K_FPU_SOURCE_EXTERNAL);
    CHECK_EQ(d.dst_fp, 2);
    CHECK_EQ(d.secondary_dst_fp, 7);
    CHECK_EQ(d.opmode, 0x37);
    /* FMOVE.P の静的 k-factor は7bit の2の補数: +17、-1（$7F）、-64（$40）、+63（$3F） */
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0x6C11, &d));
    CHECK_EQ(d.packed_k_factor, 17);
    CHECK(!d.dynamic_packed_k_factor);
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0x6C7F, &d));
    CHECK_EQ(d.packed_k_factor, -1);
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0x6C40, &d));
    CHECK_EQ(d.packed_k_factor, -64);
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0x6C3F, &d));
    CHECK_EQ(d.packed_k_factor, 63);
    /* 動的 k-factor（形式コード7）: Dn は bit6-4。下位4bit は無視 */
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0x7C50, &d)); /* FP0、D5 */
    CHECK(d.dynamic_packed_k_factor);
    CHECK_EQ(d.packed_k_factor_data_register, 5);
    CHECK_EQ(d.format, MC68K_FPU_FORMAT_P);
    CHECK_EQ(d.packed_k_factor, 0);
    /* FMOVEM: mode bit12-11（00=predecrement 静的、01=predecrement
     * 動的、10/11=postincrement/control） */
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0xE003, &d));
    CHECK(d.predecrement);
    CHECK(!d.dynamic_register_mask);
    CHECK_EQ(d.register_mask, 3);
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0xE830, &d)); /* mode 01: 動的、D3 */
    CHECK(d.predecrement);
    CHECK(d.dynamic_register_mask);
    CHECK_EQ(d.register_mask_data_register, 3);
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0xF8C0,
                                        &d)); /* mode 11: 動的、D4 は bit6-4=100 */
    CHECK(!d.predecrement);
    CHECK(d.dynamic_register_mask);
    CHECK_EQ(d.register_mask_data_register, 4);
    /* FMOVEM 入力の predecrement（mode 00/01）は F-line ではない（Table 4-18 Note 1） */
    CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68881, 0xC001, &d));
    CHECK(d.predecrement);
    CHECK_EQ(d.transfer_to_external, 0);
}

/* 予約ビットは0でなくても F-line にならず、descriptor（command_word を除く）は変わらない */
static void test_reserved_bits_ignored(void) {
    static const struct {
        uint16_t word;
        uint16_t reserved; /* 無視されるビット */
    } cases[] = {
        {0x9000, 0x03FF}, {0xB400, 0x03FF}, /* control: bit9-0 */
        {0xD080, 0x0700}, {0xF080, 0x0700}, /* FMOVEM 静的: bit10-8 */
        {0xF830, 0x078F}, {0xD830, 0x078F}, /* FMOVEM 動的: bit10-8、bit7、bit3-0 */
        {0x7C50, 0x000F},                   /* 動的 k-factor: bit3-0 */
        {0x6500, 0x007F},                   /* 出力 S の k-factor 欄（P 以外では無視） */
    };
    size_t i;
    uint16_t m;
    unsigned checked = 0;

    CHECK_EQ(sizeof(cases) / sizeof(cases[0]),
             8u); /* cases の要素数。表の行の削除・ループの縮小を見逃さない */
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mc68k_fpu_command_t base, d;

        CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68882, cases[i].word, &base));
        for (m = cases[i].reserved;; m = (uint16_t)((m - 1) & cases[i].reserved)) {
            CHECK(mc68k_fpu_decode_command_word(MC68K_FPU_68882, (uint16_t)(cases[i].word | m),
                                                &d));
            d.command_word = base.command_word;
            CHECK_EQ(memcmp(&d, &base, sizeof(d)), 0);
            checked++;
            if (m == 0)
                break;
        }
    }
    CHECK_EQ(i, 8u); /* ループが縮まない */
    /* 予約ビットの部分集合の数: 2^10 ×2（control）＋ 2^3 ×2（静的 FMOVEM）＋ 2^8 ×2（動的
     * FMOVEM）＋ 2^4（動的 k-factor）＋ 2^7（出力 S）。m の列挙が縮んでも通らないようにする */
    CHECK_EQ(checked, 2048u + 16u + 512u + 16u + 128u);
}

int main(void) {
    test_exhaustive();
    test_fields();
    test_special_fields();
    test_reserved_bits_ignored();
    printf("test_fpu_model_decode: OK\n");
    return 0;
}
