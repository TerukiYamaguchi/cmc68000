/*
 * test_cycle_table.c — cyc_resolve() / g_cycles の解決結果を検証する
 * (, 現行仕様 T1-1〜T1-4)
 *
 * cpu->cyc[] を直接読むので、命令は一切実行しない（bus はゼロ初期化で足りる）。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static mc68k_state_t *make_cpu(mc68k_model_t model) {
    mc68k_bus_t bus;
    memset(&bus, 0, sizeof(bus));
    return mc68k_create_model(&bus, model);
}

/* T1-1: cyc_resolve() 後の全クラス値が設計書の表（§5.4）と一致すること。
 * プロファイル5種 × 全クラスをハードコードした期待値配列と突き合わせる。 */
static void test_t1_1_resolved_values(void) {
    struct {
        mc68k_model_t model;
        int16_t branch, expired, cc_true;
    } cases[] = {
        {MC68K_MODEL_68000, 10, 14, 12},
        {MC68K_MODEL_68010, 10, 16, 10}, /* branch は 68000 から継承 */
        {MC68K_MODEL_68020, 6, 10, 6},
        {MC68K_MODEL_68030, 6, 10, 6},   /* 全クラス 68020 から継承 */
        {MC68K_MODEL_68EC030, 6, 10, 6}, /* CYC_PROF_68030 を共有 */
        {MC68K_MODEL_68040, 6, 8, 8},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mc68k_state_t *cpu = make_cpu(cases[i].model);
        assert(cpu->cyc[CYC_DBCC_BRANCH] == cases[i].branch);
        assert(cpu->cyc[CYC_DBCC_EXPIRED] == cases[i].expired);
        assert(cpu->cyc[CYC_DBCC_CC_TRUE] == cases[i].cc_true);
        mc68k_destroy(cpu);
    }
}

/* T1-2: CYC_PROF_68000 の全値が、本設計着手前のリテラル（旧 op_dbcc() の
 * return 12/10/14）と一致すること（§9.1 の不変保証）。 */
static void test_t1_2_68000_unchanged(void) {
    mc68k_state_t *cpu = make_cpu(MC68K_MODEL_68000);
    assert(cpu->cyc[CYC_DBCC_CC_TRUE] == 12);
    assert(cpu->cyc[CYC_DBCC_BRANCH] == 10);
    assert(cpu->cyc[CYC_DBCC_EXPIRED] == 14);
    mc68k_destroy(cpu);
}

/* T1-3: 継承の解決が正しいこと。CYC_INHERIT の行が親の値になり、
 * オーバーライド行だけ変わる。68010 は BRANCH だけ継承（10=68000と同値）、
 * EXPIRED/CC_TRUE はオーバーライド（68000とは異なる値）。 */
static void test_t1_3_inheritance(void) {
    mc68k_state_t *cpu000 = make_cpu(MC68K_MODEL_68000);
    mc68k_state_t *cpu010 = make_cpu(MC68K_MODEL_68010);

    /* 継承行: 68010 の BRANCH は 68000 と同値のはず */
    assert(cpu010->cyc[CYC_DBCC_BRANCH] == cpu000->cyc[CYC_DBCC_BRANCH]);
    /* オーバーライド行: 68010 の EXPIRED/CC_TRUE は 68000 と異なるはず
     * （継承ではなくこの世代固有の値であることの検査） */
    assert(cpu010->cyc[CYC_DBCC_EXPIRED] != cpu000->cyc[CYC_DBCC_EXPIRED]);
    assert(cpu010->cyc[CYC_DBCC_CC_TRUE] != cpu000->cyc[CYC_DBCC_CC_TRUE]);

    mc68k_destroy(cpu000);
    mc68k_destroy(cpu010);

    /* 68030 は全クラスが 68020 からの継承（値が完全一致） */
    mc68k_state_t *cpu020 = make_cpu(MC68K_MODEL_68020);
    mc68k_state_t *cpu030 = make_cpu(MC68K_MODEL_68030);
    assert(cpu030->cyc[CYC_DBCC_BRANCH] == cpu020->cyc[CYC_DBCC_BRANCH]);
    assert(cpu030->cyc[CYC_DBCC_EXPIRED] == cpu020->cyc[CYC_DBCC_EXPIRED]);
    assert(cpu030->cyc[CYC_DBCC_CC_TRUE] == cpu020->cyc[CYC_DBCC_CC_TRUE]);
    mc68k_destroy(cpu020);
    mc68k_destroy(cpu030);
}

/* T1-4: CYC_ABSENT になるモデル／クラスの組み合わせが期待どおりであること。
 * Phase 0 の時点では DBcc の3クラスは5モデルすべてに存在するので、
 * CYC_ABSENT はどのモデル・どのクラスにも現れない。
 * Phase 1b で68000に存在しない命令のクラス（MOVE_FROM_CCR_REG・
 * MOVEC_SRC_TO_REG・MOVES_L・EXC_MOVEC_ILLEGAL）を新設したため、68000では
 * この4クラスのみCYC_ABSENTになる。CYC_RTE_LONG_NORETRY・CYC_RTE_LONG_RETRY_READ・
 * CYC_RTE_LONG_RETRY_WRITEはformat8分岐が
 * model==68010でしか到達しないため、68000/68020/68030の3モデルすべてで
 * CYC_ABSENT（notes/phase1_68010_diff_survey.md §3.7・現行仕様
 * §10 Phase 1b・現行仕様 A-4）。CYC_EXC_BKPTも
 * 68000に対応する命令が無いため68000のみCYC_ABSENT（、68010以降は
 * BKPTを新規登録したので値を持つ）。それ以外の組み合わせにCYC_ABSENTが無いこと、
 * ABSENT以外の全クラスが正の値（未解決=0残留がない）ことも検査する
 * （§5.4「NDEBUG でも検出できるようにする」）。 */
static void test_t1_4_absent_combinations(void) {
    mc68k_model_t models[] = {
        MC68K_MODEL_68000,   MC68K_MODEL_68010, MC68K_MODEL_68020,
        MC68K_MODEL_68EC030, MC68K_MODEL_68030,
    };
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        mc68k_state_t *cpu = make_cpu(models[i]);
        bool is_68000 = models[i] == MC68K_MODEL_68000;
        bool is_020_or_030 = models[i] == MC68K_MODEL_68020 || models[i] == MC68K_MODEL_68030 ||
                             models[i] == MC68K_MODEL_68EC030;
        bool is_pre020 = !is_020_or_030;
        for (int cls = 0; cls < CYC_CLASS_COUNT; cls++) {
            bool is_rte_long = cls == CYC_RTE_LONG_NORETRY || cls == CYC_RTE_LONG_RETRY_READ ||
                               cls == CYC_RTE_LONG_RETRY_WRITE;
            bool expect_absent =
                (is_68000 && (cls == CYC_MOVE_FROM_CCR_REG || cls == CYC_MOVEC_SRC_TO_REG ||
                              cls == CYC_MOVES_L || cls == CYC_EXC_MOVEC_ILLEGAL ||
                              cls == CYC_EXC_BKPT || is_rte_long)) ||
                (is_020_or_030 && is_rte_long);
            expect_absent = expect_absent || (is_pre020 && (cls == CYC_CHK_EXCEPTION_DN ||
                                                            cls == CYC_CHK_EXCEPTION_EA ||
                                                            cls == CYC_CHK2_EXCEPTION));
            if (expect_absent) {
                assert(cpu->cyc[cls] == CYC_ABSENT);
            } else {
                assert(cpu->cyc[cls] != CYC_ABSENT);
                assert(cpu->cyc[cls] > 0);
            }
        }
        mc68k_destroy(cpu);
    }
}

/* T1-1/T1-2拡張: Phase 1bで新設した45クラス＋で
 * 新設したCYC_EXC_BKPTの解決値が、notes/phase1_68010_diff_survey.md §3・
 * の値と一致すること。cycles.c の g_cycles テーブルと同じ値を
 * ここに独立して書き写すことで、テーブル記入時の転記ミスを検出する
 * （意図的な二重管理）。これらのクラスは68020/68030行もCYC_INHERITを
 * 使わない設計なので、解決値=生エントリであり、この配列がそのまま
 * T1-1の役割を果たす。 */
static void test_t1_1_t1_2_phase1b_classes(void) {
    static const int16_t expected[CYC_PROF_COUNT][CYC_CLASS_COUNT - CYC_ADDA_L_EA_REG] = {
        [CYC_PROF_68000 - CYC_PROF_68000] =
            {
                8,          8,   8,          8,          8,          8,          8,  158,
                140,        70,  70,         16,         16,         16,         16, 14,
                8,          6,   8,          12,         8,          12,         8,  8,
                20,         20,  20,         10,         6,          CYC_ABSENT, 4,  CYC_ABSENT,
                CYC_ABSENT, 132, 20,         CYC_ABSENT, CYC_ABSENT, CYC_ABSENT, 40, 38,
                34,         44,  CYC_ABSENT, 34,         34,         34,         34, CYC_ABSENT,
            },
        [CYC_PROF_68010 - CYC_PROF_68000] =
            {
                6,  6,   6,  6,   6,   6,   6,  122, 108, 42, 40, 14, 14, 14, 14, 12,
                4,  4,   10, 14,  6,   10,  6,  6,   16,  16, 16, 8,  4,  4,  6,  12,
                20, 130, 24, 110, 112, 112, 44, 42,  38,  46, 46, 38, 38, 38, 40, 42,
            },
        [CYC_PROF_68020 - CYC_PROF_68000] =
            {
                8,          8,          8,  8,  8,  8,  8,  158, 140, 70,  70, 16,
                16,         16,         16, 14, 8,  6,  8,  12,  8,   12,  8,  8,
                20,         20,         20, 10, 6,  6,  4,  10,  16,  132, 20, CYC_ABSENT,
                CYC_ABSENT, CYC_ABSENT, 40, 38, 34, 44, 34, 34,  34,  34,  34, 34,
            },
        [CYC_PROF_68030 - CYC_PROF_68000] =
            {
                8,          8,          8,  8,  8,  8,  8,  158, 140, 70,  70, 16,
                16,         16,         16, 14, 8,  6,  8,  12,  8,   12,  8,  8,
                12,         12,         12, 8,  6,  6,  4,  10,  16,  132, 20, CYC_ABSENT,
                CYC_ABSENT, CYC_ABSENT, 40, 38, 18, 23, 34, 18,  22,  18,  22, 9,
            },
        [CYC_PROF_68040 - CYC_PROF_68000] =
            {
                8,          8,          8,  8,  8,  8,  8,  158, 140, 70,   70, 16,
                16,         16,         16, 14, 8,  6,  8,  12,  8,   12,   8,  8,
                20,         20,         20, 10, 6,  6,  13, 21,  16,  1042, 15, CYC_ABSENT,
                CYC_ABSENT, CYC_ABSENT, 40, 38, 32, 44, 34, 18,  22,  32,   38, 9,
            },
    };

    mc68k_model_t models[] = {
        MC68K_MODEL_68000, MC68K_MODEL_68010, MC68K_MODEL_68020,
        MC68K_MODEL_68030, MC68K_MODEL_68040,
    };
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        mc68k_state_t *cpu = make_cpu(models[i]);
        for (int cls = CYC_ADDA_L_EA_REG; cls <= CYC_EXC_BKPT; cls++) {
            int16_t want = expected[i][cls - CYC_ADDA_L_EA_REG];
            assert(cpu->cyc[cls] == want);
        }
        mc68k_destroy(cpu);
    }
}

/* T1-8:
 * Phase 1b が新設した45クラス＋ が新設した CYC_EXC_BKPT
 * （CYC_ADDA_L_EA_REG〜CYC_EXC_BKPT、DBcc3クラスはPhase 0からの既存クラス
 * なので対象外）について、cyc_resolve() で解決した後の値ではなく g_cycles
 * の生エントリ自体が CYC_PROF_68020/68030 行で CYC_INHERIT になっていない
 * ことを検査する。test_t1_1_t1_2_phase1b_classes() は解決後の値しか見ない
 * ため、偶然の値一致で CYC_INHERIT が紛れ込んでも検出できない
 * （notes/phase1_68010_diff_survey.md §7）。CYC_ABSENT は許容する
 * （到達不能と確認済みのクラスがあるため、§10 Phase 1b の完了条件）。 */
static void test_t1_8_phase1b_raw_entries_not_inherit(void) {
    for (int cls = CYC_ADDA_L_EA_REG; cls <= CYC_EXC_BKPT; cls++) {
        int16_t v020 = cyc_raw_entry(CYC_PROF_68020, (cyc_class_t)cls);
        int16_t v030 = cyc_raw_entry(CYC_PROF_68030, (cyc_class_t)cls);
        assert(v020 != CYC_INHERIT);
        assert(v030 != CYC_INHERIT);
    }
}

/* #90: the 040 profile is an independent timing profile.  Resolved values
 * alone would not expose an accidental fall-through to the 030 row, so check
 * every raw class entry rather than a representative subset. */
static void test_68040_raw_entries_not_inherit(void) {
    for (int cls = 0; cls < CYC_CLASS_COUNT; cls++)
        assert(cyc_raw_entry(CYC_PROF_68040, (cyc_class_t)cls) != CYC_INHERIT);
}

/* T1-9（Phase 3f）: 68030 の CYC_INHERIT は、UM30 §11.6.15 と UM20
 * §8.2.15 で値・(r/p/w) がともに一致する DBcc の三形態だけである。
 *
 * Phase 3 の原典530行は EA selector と命令ハンドラのリテラルにも分散して
 * いるため、この raw table はその全行の台帳ではない。しかし raw table に
 * CYC_INHERIT を置ける class はここで全数検査できる。R1 一致だけの値や、
 * 値だけ偶然一致する値を継承へ戻す変更を Debug test で検出する。
 * NDEBUG を含む静的な検査は tools/check_cycle_inherit.py が担当する。 */
static void test_t1_9_68030_inherit_allowlist(void) {
    static const cyc_class_t allowed[] = {
        CYC_DBCC_BRANCH,
        CYC_DBCC_EXPIRED,
        CYC_DBCC_CC_TRUE,
    };
    size_t inherited = 0;
    size_t scanned = 0;
    for (int cls = 0; cls < CYC_CLASS_COUNT; cls++) {
        bool allowed_here = false;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
            allowed_here |= cls == allowed[i];
        int16_t raw = cyc_raw_entry(CYC_PROF_68030, (cyc_class_t)cls);
        assert((raw == CYC_INHERIT) == allowed_here);
        inherited += raw == CYC_INHERIT;
        scanned++;
    }
    assert(scanned == CYC_CLASS_COUNT);
    assert(inherited == sizeof(allowed) / sizeof(allowed[0]));
}

/* #102: CYC_PROF_68060 の行は全 class を明示し、040 以前の行へ暗黙に落とさない。
 * 生エントリが CYC_INHERIT でなく、未記入（designated initializer の既定値 0）でも
 * ないこと、060 モデルの解決値が生エントリそのものであることを検査する。 */
static void test_060_profile_has_no_implicit_fallback(void) {
    mc68k_state_t *cpu = make_cpu(MC68K_MODEL_68060);
    for (int cls = 0; cls < CYC_CLASS_COUNT; cls++) {
        int16_t raw = cyc_raw_entry(CYC_PROF_68060, (cyc_class_t)cls);
        assert(raw != CYC_INHERIT);
        assert(raw != 0);
        assert(cpu->cyc[cls] == raw);
    }
    mc68k_destroy(cpu);
}

static void dump_raw_entries(void) {
    static const char *const profile_names[CYC_PROF_COUNT] = {
        "CYC_PROF_68000", "CYC_PROF_68010", "CYC_PROF_68020",
        "CYC_PROF_68030", "CYC_PROF_68040", "CYC_PROF_68060",
    };
    static const char *const class_names[CYC_CLASS_COUNT] = {
        "CYC_DBCC_BRANCH",
        "CYC_DBCC_EXPIRED",
        "CYC_DBCC_CC_TRUE",
        "CYC_ADDA_L_EA_REG",
        "CYC_SUBA_L_EA_REG",
        "CYC_ADD_L_EA_REG",
        "CYC_AND_L_EA_REG",
        "CYC_OR_L_EA_REG",
        "CYC_SUB_L_EA_REG",
        "CYC_EOR_L_DNDN",
        "CYC_DIVS",
        "CYC_DIVU",
        "CYC_MULS",
        "CYC_MULU",
        "CYC_ADDI_L_DN",
        "CYC_SUBI_L_DN",
        "CYC_EORI_L_DN",
        "CYC_ORI_L_DN",
        "CYC_CMPI_L_DN",
        "CYC_SUBQ_W_AN",
        "CYC_SCC_TRUE",
        "CYC_BCLR_MEM_DYNAMIC",
        "CYC_BCLR_MEM_STATIC",
        "CYC_BCC_BYTE_NOT_TAKEN",
        "CYC_BCC_WORD_NOT_TAKEN",
        "CYC_ADDX_L_REGREG",
        "CYC_SUBX_L_REGREG",
        "CYC_ANDI_CCRSR",
        "CYC_EORI_CCRSR",
        "CYC_ORI_CCRSR",
        "CYC_CHK_NORMAL",
        "CYC_MOVE_FROM_SR_REG",
        "CYC_MOVE_FROM_CCR_REG",
        "CYC_MOVE_USP",
        "CYC_MOVEC_SRC_TO_REG",
        "CYC_MOVES_L",
        "CYC_RESET_INSTR",
        "CYC_RTE_SHORT",
        "CYC_RTE_LONG_NORETRY",
        "CYC_RTE_LONG_RETRY_READ",
        "CYC_RTE_LONG_RETRY_WRITE",
        "CYC_EXC_CHK",
        "CYC_EXC_DIVZERO",
        "CYC_EXC_ILLEGAL",
        "CYC_EXC_INTERRUPT",
        "CYC_EXC_MOVEC_ILLEGAL",
        "CYC_EXC_PRIVILEGE",
        "CYC_EXC_TRACE",
        "CYC_EXC_TRAP",
        "CYC_EXC_TRAPV",
        "CYC_EXC_BKPT",
        "CYC_CHK_EXCEPTION_DN",
        "CYC_CHK_EXCEPTION_EA",
        "CYC_CHK2_EXCEPTION",
    };
    assert(sizeof(profile_names) / sizeof(profile_names[0]) == CYC_PROF_COUNT);
    assert(sizeof(class_names) / sizeof(class_names[0]) == CYC_CLASS_COUNT);

    for (int profile = 0; profile < CYC_PROF_COUNT; profile++) {
        for (int cls = 0; cls < CYC_CLASS_COUNT; cls++) {
            printf("%s,%s,%d\n", profile_names[profile], class_names[cls],
                   cyc_raw_entry((cyc_profile_t)profile, (cyc_class_t)cls));
        }
    }
}

int main(int argc, char **argv) {
    if (argc == 2) {
        if (strcmp(argv[1], "--dump-raw-entries") == 0) {
            dump_raw_entries();
            return 0;
        }
        fprintf(stderr, "usage: %s [--dump-raw-entries]\n", argv[0]);
        return 2;
    }
    assert(argc == 1);
    test_t1_1_resolved_values();
    test_t1_2_68000_unchanged();
    test_t1_3_inheritance();
    test_t1_4_absent_combinations();
    test_t1_1_t1_2_phase1b_classes();
    test_t1_8_phase1b_raw_entries_not_inherit();
    test_060_profile_has_no_implicit_fallback();
    test_68040_raw_entries_not_inherit();
    test_t1_9_68030_inherit_allowlist();
    return 0;
}
