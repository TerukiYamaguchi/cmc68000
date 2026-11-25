/*
 * test_fpu_model_040only.c — 68040 専用の強制精度命令語（[040UM Table E-1]）の可用性表（Issue
 * #83、 現行仕様「68040 専用命令語の可用性」）。
 *
 * 「モデル × 命令語」の各セルを固定する。
 *   68881/68882: opmode $40〜$7F は [881UM Table 4-13] の "Undefined, Reserved"（Note
 * 2）。共有デコーダは 不正 word とし、CIR は vector 11 の take pre-instruction（PC=0）を返す。
 *   68040:       強制精度 16 命令は実装済み。$40〜$7F のその他は不正
 * F-line（fpu040_classify）。 期待値は production の表から取らず、[040UM Table E-1] の命令名と
 * opmode の対応から手で書く。 opmode は FSMOVE=$40 のように、基本命令の opmode に S は +$40、D
 * は +$44 を加えた符号化 （現行仕様 推論 3。FSQRT/FABS/FNEG/FADD/FSUB/FMUL/FDIV
 * の基本 opmode は [881UM Table 4-13]）。
 *
 * 落とす変異: 68881/68882 のデコーダが $40 以降の opmode を受理する、CIR が F-line でなく
 * protocol violation（vector 13）や通常の応答を返す、68040
 * の分類が強制精度命令を落とす／$40〜$7F の他の語を受理する。
 */
#include "fpu_model_util.h"
#include "cpu/cpu_internal.h"

enum {
    OPMODE_FORCED_FIRST = 0x40,
    OPMODE_FORCED_LAST = 0x7F,
    FLINE_VECTOR = 11,
    OPCODE_FPU_ID1 = 0xF200, /* cpID=1、TYPE_GEN、EA=Dn/FPm,FPn */
    EXT_CLASS_REG_REG = 0x0400,
    EXT_CLASS_EA_REG_L = 0x4000, /* <ea>,FPn の .L 形式 */
};

typedef struct {
    const char *name;
    unsigned opmode;
} forced_t;

static const forced_t forced[] = {
    {"FSMOVE", 0x40}, {"FSSQRT", 0x41}, {"FDMOVE", 0x44}, {"FDSQRT", 0x45},
    {"FSABS", 0x58},  {"FSNEG", 0x5A},  {"FDABS", 0x5C},  {"FDNEG", 0x5E},
    {"FSDIV", 0x60},  {"FSADD", 0x62},  {"FSMUL", 0x63},  {"FDDIV", 0x64},
    {"FDADD", 0x66},  {"FDMUL", 0x67},  {"FSSUB", 0x68},  {"FDSUB", 0x6C},
};
#define N_FORCED (sizeof(forced) / sizeof(forced[0]))

static int is_forced(unsigned op) {
    size_t i;

    for (i = 0; i < N_FORCED; i++)
        if (forced[i].opmode == op)
            return 1;
    return 0;
}

/* 68881/68882: $40〜$7F はクラス 000/010 とも不正 word で、CIR は vector 11 の take pre（PC=0）
 */
static void test_881_882_cells(void) {
    unsigned c, op;

    for (c = 0; c < 2; c++) {
        for (op = OPMODE_FORCED_FIRST; op <= OPMODE_FORCED_LAST; op++) {
            const uint16_t words[] = {CMD(0, 1, 0, op), CMD(2, FMT_X, 0, op),
                                      CMD(2, FMT_L, 3, op)};
            size_t i;

            for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
                mc68k_fpu_command_t d;
                mc68k_fpu_t *f;

                CHECK(!mc68k_fpu_decode_command_word(model_cfg(c), words[i], &d));
                CHECK(!d.valid);
                f = mk(model_cfg(c));
                CHECK(mc68k_fpu_write_command_word(f, words[i]));
                expect_take(f, MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION, FLINE_VECTOR, 0);
                mc68k_fpu_destroy(f);
            }
        }
        CHECK_EQ(op, OPMODE_FORCED_LAST + 1); /* ループが縮まない */
    }
    CHECK_EQ(c, 2);
}

/* 強制精度 16 命令が、68881/68882 では不正・68040 では実装済みで、基本命令は逆になること */
static void test_cross_model_agreement(void) {
    size_t i;
    unsigned op, n_forced = 0, n_illegal040 = 0;

    for (i = 0; i < N_FORCED; i++) {
        mc68k_fpu_command_t d;

        CHECK(forced[i].opmode >= OPMODE_FORCED_FIRST &&
              forced[i].opmode <= OPMODE_FORCED_LAST);
        CHECK(!mc68k_fpu_decode_command_word(MC68K_FPU_68881, CMD(0, 1, 0, forced[i].opmode),
                                             &d));
        CHECK(!mc68k_fpu_decode_command_word(MC68K_FPU_68882, CMD(0, 1, 0, forced[i].opmode),
                                             &d));
        CHECK_EQ(
            fpu040_classify(OPCODE_FPU_ID1, (uint16_t)(EXT_CLASS_REG_REG | forced[i].opmode)),
            FPU040_IMPL);
        CHECK_EQ(
            fpu040_classify(OPCODE_FPU_ID1, (uint16_t)(EXT_CLASS_EA_REG_L | forced[i].opmode)),
            FPU040_IMPL);
    }
    for (op = OPMODE_FORCED_FIRST; op <= OPMODE_FORCED_LAST; op++) {
        const fpu040_class_t cls =
            fpu040_classify(OPCODE_FPU_ID1, (uint16_t)(EXT_CLASS_REG_REG | op));

        if (is_forced(op)) {
            CHECK_EQ(cls, FPU040_IMPL);
            n_forced++;
        } else {
            CHECK_EQ(cls, FPU040_ILLEGAL); /* 040 でも $40〜$7F の残りは不正 F-line */
            n_illegal040++;
        }
    }
    CHECK_EQ(n_forced, 16);
    CHECK_EQ(n_illegal040, 48);
    CHECK_EQ(N_FORCED, 16);
}

/* 強制精度命令の基本命令（S/D を除く）は 68881/68882 で有効のまま。既存挙動を変えないことの確認
 */
static void test_base_ops_unchanged(void) {
    static const unsigned base_ops[] = {0x00, 0x04, 0x18, 0x1A, 0x20, 0x22, 0x23, 0x28};
    unsigned c;
    size_t i;

    for (c = 0; c < 2; c++)
        for (i = 0; i < sizeof(base_ops) / sizeof(base_ops[0]); i++) {
            mc68k_fpu_command_t d;

            CHECK(mc68k_fpu_decode_command_word(model_cfg(c), CMD(0, 1, 0, base_ops[i]), &d));
            CHECK_EQ(d.opmode, base_ops[i]);
            CHECK_EQ(
                fpu040_classify(OPCODE_FPU_ID1, (uint16_t)(EXT_CLASS_REG_REG | base_ops[i])),
                FPU040_IMPL);
        }
    CHECK_EQ(c, 2);
}

int main(void) {
    test_881_882_cells();
    test_cross_model_agreement();
    test_base_ops_unchanged();
    printf("test_fpu_model_040only: OK\n");
    return 0;
}
