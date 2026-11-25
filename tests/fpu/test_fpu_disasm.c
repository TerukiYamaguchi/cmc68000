/*
 * test_fpu_disasm.c — Phase D。
 *
 * (1) mc68k_get_fpu_regs() の FPCR/FPSR/FPIAR/FP を、command（control 出力・FMOVE FPn,<ea>）で
 *     読み戻した値と比較する。FPCR/FPSR のパッキングは §6 の転送と同一でなければならない。
 * (2) F-line（coprocessor id=1）の逆アセンブル。期待値は [881UM] §4.7 の Table 4-11〜4-20 と
 *     各命令の頁のビット配置から手で組み立てた語と文字列で、production
 * の表・関数から取らない。FPU 構成 （NONE/68881/68882）で表示が変わらないこと、68000/68010 と
 * id≠1 は LINE-F（長さ2）になること。
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "fpu_model_util.h"
#include "mc68000/bus_hooks.h"
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/debug.h"

#define RAM_SIZE  0x10000u
#define ADDR      0x1000u
#define MAX_WORDS 8
#define BUF_SIZE  128

static uint8_t g_ram[RAM_SIZE];

static bool rd_b(void *ctx, uint32_t a, uint8_t *o) {
    (void)ctx;
    *o = a < RAM_SIZE ? g_ram[a] : 0xFF;
    return a < RAM_SIZE;
}
static bool rd_w(void *ctx, uint32_t a, uint16_t *o) {
    uint8_t h, l;
    bool ok = rd_b(ctx, a, &h) & rd_b(ctx, a + 1, &l);
    *o = (uint16_t)((h << 8) | l);
    return ok;
}
static bool rd_l(void *ctx, uint32_t a, uint32_t *o) {
    uint16_t h, l;
    bool ok = rd_w(ctx, a, &h) & rd_w(ctx, a + 2, &l);
    *o = ((uint32_t)h << 16) | l;
    return ok;
}
static bool wr_b(void *ctx, uint32_t a, uint8_t v) {
    (void)ctx;
    if (a < RAM_SIZE)
        g_ram[a] = v;
    return a < RAM_SIZE;
}
static bool wr_w(void *ctx, uint32_t a, uint16_t v) {
    return wr_b(ctx, a, (uint8_t)(v >> 8)) & wr_b(ctx, a + 1, (uint8_t)v);
}
static bool wr_l(void *ctx, uint32_t a, uint32_t v) {
    return wr_w(ctx, a, (uint16_t)(v >> 16)) & wr_w(ctx, a + 2, (uint16_t)v);
}
static void sync_cb(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}
static void wh_cb(void *ctx, uint32_t a, uint32_t size) {
    (void)ctx;
    (void)a;
    (void)size;
}
static uint8_t peek_b(void *ctx, uint32_t a) {
    uint8_t v = 0;
    rd_b(ctx, a, &v);
    return v;
}
static uint16_t peek_w(void *ctx, uint32_t a) {
    uint16_t v = 0;
    rd_w(ctx, a, &v);
    return v;
}
static uint32_t peek_l(void *ctx, uint32_t a) {
    uint32_t v = 0;
    rd_l(ctx, a, &v);
    return v;
}

static mc68k_state_t *make_cpu(mc68k_model_t model, mc68k_fpu_config_t fpu) {
    mc68k_bus_t bus = {.read_byte = rd_b,
                       .read_word = rd_w,
                       .read_long = rd_l,
                       .write_byte = wr_b,
                       .write_word = wr_w,
                       .write_long = wr_l,
                       .sync = sync_cb,
                       .write_hook = wh_cb};
    mc68k_state_t *cpu = mc68k_create_model_with_fpu(&bus, model, fpu);

    CHECK(cpu != NULL);
    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, NULL);
    return cpu;
}

typedef struct {
    uint16_t words[MAX_WORDS];
    int count;
    int len;
    const char *text;
} case_t;

#define LINE_F "LINE-F"

/* 68020 以上で FPU 構成によらず同じになる表。語は §4.7 の Table 4-11〜4-20
 * と各命令の頁から組み立てる */
static const case_t g_fpu_cases[] = {
    /* 条件分岐。FNOP は FBcc.W の cond=0・変位0 */
    {{0xF280, 0x0000}, 2, 4, "FNOP"},
    {{0xF280, 0x0004}, 2, 4, "FBF $00001006"},
    {{0xF281, 0x0010}, 2, 4, "FBEQ $00001012"},
    {{0xF29F, 0xFFFE}, 2, 4, "FBST $00001000"},
    {{0xF2CF, 0x0000, 0x0100}, 3, 6, "FBT.L $00001102"},
    {{0xF2A1, 0x0002}, 2, 4, "FBEQ $00001004"}, /* cond $21 は EQ と同じ（Table 4-20 Note 3） */
    {{0xF2A0, 0x0000}, 2, 4, "FBF $00001002"},  /* cond $20 は F。opcode が FNOP ではない */
    /* FScc / FDBcc / FTRAPcc */
    {{0xF240, 0x0001}, 2, 4, "FSEQ D0"},
    {{0xF241, 0x0011}, 2, 4, "FSSEQ D1"}, /* cond $11 は SEQ（signaling） */
    {{0xF250, 0x001F}, 2, 4, "FSST (A0)"},
    {{0xF278, 0x000E, 0x2000}, 3, 6, "FSNE $00002000.W"},
    {{0xF249, 0x000E, 0xFFFC}, 3, 6, "FDBNE D1,$00001000"},
    {{0xF27A, 0x000F, 0x1234}, 3, 6, "FTRAPT.W #$1234"},
    {{0xF27B, 0x0000, 0x0001, 0x0002}, 4, 8, "FTRAPF.L #$00010002"},
    {{0xF27C, 0x0010}, 2, 4, "FTRAPSF"},
    {{0xF27D, 0x0000}, 2, 2, LINE_F}, /* mode 7 reg 5 は FScc でも FTRAPcc でもない */
    /* FSAVE / FRESTORE */
    {{0xF310}, 1, 2, "FSAVE (A0)"},
    {{0xF327}, 1, 2, "FSAVE -(A7)"},
    {{0xF31F}, 1, 2, LINE_F},
    {{0xF33A, 0x0004}, 2, 2, LINE_F},
    {{0xF35F}, 1, 2, "FRESTORE (A7)+"},
    {{0xF367}, 1, 2, LINE_F},
    {{0xF37A, 0x0004}, 2, 4, "FRESTORE $00001006(PC)"},
    /* FPm,FPn */
    {{0xF200, 0x0500}, 2, 4, "FMOVE.X FP1,FP2"},
    {{0xF200, 0x0184}, 2, 4, "FSQRT.X FP0,FP3"},
    {{0xF200, 0x0AB4}, 2, 4, "FSINCOS.X FP2,FP4:FP5"},
    {{0xF200, 0x183A}, 2, 4, "FTST.X FP6"},
    {{0xF200, 0x07B8}, 2, 4, "FCMP.X FP1,FP7"},
    {{0xF200, 0x0022}, 2, 4, "FADD.X FP0,FP0"},
    {{0xF200, 0x0005}, 2, 2, LINE_F}, /* 未定義の opmode */
    {{0xF200, 0x2000}, 2, 2, LINE_F}, /* クラス001 */
    /* <ea>,FPn */
    {{0xF210, 0x4422}, 2, 4, "FADD.S (A0),FP0"},
    {{0xF201, 0x4100}, 2, 4, "FMOVE.L D1,FP2"},
    {{0xF200, 0x4822}, 2, 2, LINE_F}, /* X のソースは Dn 不可 */
    {{0xF209, 0x4100}, 2, 2, LINE_F}, /* An は不可 */
    {{0xF23C, 0x5400, 0x4000, 0x0000, 0x0000, 0x0000}, 6, 12, "FMOVE.D #$4000000000000000,FP0"},
    {{0xF23C, 0x4800, 0x3FFF, 0x0000, 0x8000, 0x0000, 0x0000, 0x0000},
     8,
     16,
     "FMOVE.X #$3FFF00008000000000000000,FP0"},
    {{0xF23C, 0x5800, 0x0012}, 3, 6, "FMOVE.B #$12,FP0"},
    {{0xF200, 0x5D8F}, 2, 4, "FMOVECR.X #$0F,FP3"},
    {{0xF210, 0x4A30}, 2, 4, "FSINCOS.X (A0),FP0:FP4"},
    /* FPm,<ea> */
    {{0xF211, 0x6980}, 2, 4, "FMOVE.X FP3,(A1)"},
    {{0xF210, 0x6C7D}, 2, 4, "FMOVE.P FP0,(A0){#-3}"},
    {{0xF210, 0x6C03}, 2, 4, "FMOVE.P FP0,(A0){#3}"},
    {{0xF210, 0x7CA0}, 2, 4, "FMOVE.P FP1,(A0){D2}"},
    {{0xF203, 0x6000}, 2, 4, "FMOVE.L FP0,D3"},
    {{0xF203, 0x6800}, 2, 2, LINE_F}, /* X の宛先は Dn 不可 */
    {{0xF23C, 0x6000}, 2, 2, LINE_F}, /* 即値の宛先は不可 */
    /* control register */
    {{0xF200, 0x9000}, 2, 4, "FMOVE.L D0,FPCR"},
    {{0xF201, 0xA800}, 2, 4, "FMOVE.L FPSR,D1"},
    {{0xF20A, 0x8400}, 2, 4, "FMOVE.L A2,FPIAR"},
    {{0xF20A, 0x9000}, 2, 2, LINE_F},
    {{0xF20A, 0xA400}, 2, 4, "FMOVE.L FPIAR,A2"}, /* FPIAR の出力は Alterable（Table 4-17） */
    {{0xF20A, 0xA800}, 2, 2, LINE_F},
    {{0xF200, 0x9800}, 2, 2, LINE_F}, /* 複数 register の move は Dn 不可 */
    {{0xF218, 0x9C00}, 2, 4, "FMOVEM.L (A0)+,FPCR/FPSR/FPIAR"},
    {{0xF227, 0xB400}, 2, 4, "FMOVEM.L FPCR/FPIAR,-(A7)"},
    {{0xF23C, 0x9000, 0x0000, 0x1234}, 4, 8, "FMOVE.L #$00001234,FPCR"},
    /* FMOVEM.X。postincrement/control は bit7=FP0、predecrement は bit7=FP7 */
    {{0xF218, 0xD0F0}, 2, 4, "FMOVEM.X (A0)+,FP0-FP3"},
    {{0xF218, 0xD001}, 2, 4, "FMOVEM.X (A0)+,FP7"},
    {{0xF218, 0xD0A5}, 2, 4, "FMOVEM.X (A0)+,FP0/FP2/FP5/FP7"},
    {{0xF227, 0xE00D}, 2, 4, "FMOVEM.X FP0/FP2-FP3,-(A7)"},
    {{0xF227, 0xE850}, 2, 4, "FMOVEM.X D5,-(A7)"},
    {{0xF210, 0xF000}, 2, 4, "FMOVEM.X #0,(A0)"},
    {{0xF210, 0xF8A0}, 2, 4, "FMOVEM.X D2,(A0)"},
    {{0xF200, 0xD0F0}, 2, 2, LINE_F},
    {{0xF220, 0xD0F0}, 2, 2, LINE_F},
    {{0xF227, 0xF00D}, 2, 2, LINE_F}, /* 出力の mode 10 に -(An) は組めない */
    {{0xF210, 0xE00D}, 2, 2, LINE_F}, /* 出力の mode 00 は -(An) だけ */
    {{0xF21F, 0xD0F0}, 2, 4, "FMOVEM.X (A7)+,FP0-FP3"},
    /* 追加（D-B1・B2・B4〜B6）。PC 相対の基点は EA の拡張 word のアドレス（$1004） */
    {{0xF200, 0x0040}, 2, 2, LINE_F}, /* opmode $40〜$7F は未定義（Table 4-13） */
    {{0xF200, 0x007F}, 2, 2, LINE_F},
    {{0xF210, 0x4440}, 2, 2, LINE_F},
    {{0xF23A, 0x4422, 0x0004}, 3, 6, "FADD.S $00001008(PC),FP0"},
    {{0xF209, 0x6000}, 2, 2, LINE_F}, /* 宛先に An は不可 */
    {{0xF201, 0x5800}, 2, 4, "FMOVE.B D1,FP0"},
    {{0xF201, 0x7000}, 2, 4, "FMOVE.W FP0,D1"},
    {{0xF201, 0x6400}, 2, 4, "FMOVE.S FP0,D1"},
    {{0xF210, 0x443A}, 2, 4, "FTST.S (A0)"},
    {{0xF200, 0x8000},
     2,
     4,
     "FMOVE.L D0,FPIAR"}, /* select 000 は FPIAR と同じ（Table 4-17 Note 2） */
    {{0xF200, 0xA000}, 2, 4, "FMOVE.L FPIAR,D0"},
    {{0xF218, 0xC0F0},
     2,
     4,
     "FMOVEM.X (A0)+,FP4-FP7"}, /* 入力の mode 00 は FP7→FP0 の順（Table 4-18 Note 1） */
    /* 即値の幅。B は word の下位バイトだけを表示する */
    {{0xF23C, 0x5800, 0xFF12}, 3, 6, "FMOVE.B #$12,FP0"},
    {{0xF23C, 0x5000, 0x1234}, 3, 6, "FMOVE.W #$1234,FP0"},
    {{0xF23C, 0x4400, 0x3F80, 0x0000}, 4, 8, "FMOVE.S #$3F800000,FP0"},
    {{0xF23C, 0x4000, 0x0000, 0x0007}, 4, 8, "FMOVE.L #$00000007,FP0"},
    {{0xF23C, 0x4C00, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006},
     8,
     16,
     "FMOVE.P #$000100020003000400050006,FP0"},
    /* 複数 control register の即値は register 数×4 バイト（Table 4-17、§4.7.1.5） */
    {{0xF23C, 0x9800, 0x1111, 0x2222, 0x3333, 0x4444},
     6,
     12,
     "FMOVEM.L #$1111222233334444,FPCR/FPSR"},
    {{0xF23C, 0x9C00, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666},
     8,
     16,
     "FMOVEM.L #$111122223333444455556666,FPCR/FPSR/FPIAR"},
    {{0xF23C, 0x8800, 0x1111, 0x2222}, 4, 8, "FMOVE.L #$11112222,FPSR"},
    /* FMOVECR の offset は ROM 判定が先（opmode の表に無い $0B・$39・$3B〜$3F、設計書 §6 の
       $40〜$7F も有効） */
    {{0xF200, 0x5C0B}, 2, 4, "FMOVECR.X #$0B,FP0"},
    {{0xF200, 0x5C3F}, 2, 4, "FMOVECR.X #$3F,FP0"},
    {{0xF200, 0x5C40}, 2, 4, "FMOVECR.X #$40,FP0"},
    {{0xF200, 0x5C7F}, 2, 4, "FMOVECR.X #$7F,FP0"},
    /* 予約ビットが非0でも F-line にしない（[881UM] p.4-126・4-131・4-134、Table 4-16 Note
     * 2・3）。 FMOVEM の bit10-8 を無視するのは設計書 §6 の推論 */
    {{0xF210, 0x0500}, 2, 4, "FMOVE.X FP1,FP2"}, /* FPm,FPn の EA 欄。拡張 word は読まない */
    {{0xF201, 0x5C0B}, 2, 4, "FMOVECR.X #$0B,FP0"},
    {{0xF200, 0x93FF}, 2, 4, "FMOVE.L D0,FPCR"},        /* control の bit9-0 */
    {{0xF203, 0x6005}, 2, 4, "FMOVE.L FP0,D3"},         /* P 以外の extension 欄 */
    {{0xF210, 0x7CAF}, 2, 4, "FMOVE.P FP1,(A0){D2}"},   /* 動的 P の bit3-0 */
    {{0xF218, 0xD7F0}, 2, 4, "FMOVEM.X (A0)+,FP0-FP3"}, /* FMOVEM の bit10-8 */
    {{0xF240, 0xFFC1}, 2, 4, "FSEQ D0"},                /* 条件語の bit15-6 */
    {{0xF27C, 0xFFC0}, 2, 4, "FTRAPF"},
    {{0xF249, 0xFFC0, 0x0000}, 3, 6, "FDBF D1,$00001004"},
    {{0xF23C, 0x5800, 0xFF81}, 3, 6, "FMOVE.B #$81,FP0"}, /* 下位 byte の bit0 も落とさない */
    {{0xF23C, 0x8C00, 0x1111, 0x2222, 0x3333, 0x4444},
     6,
     12,
     "FMOVEM.L #$1111222233334444,FPSR/FPIAR"},
    {{0xF23C, 0x9400, 0x1111, 0x2222, 0x3333, 0x4444},
     6,
     12,
     "FMOVEM.L #$1111222233334444,FPCR/FPIAR"},
    /* F-line だが対象外 */
    {{0xF000}, 1, 2, LINE_F}, /* id=0（MMU） */
    {{0xF400}, 1, 2, LINE_F}, /* id=2 */
    {{0xF380}, 1, 2, LINE_F}, /* type 6 */
    {{0xF3C0}, 1, 2, LINE_F}, /* type 7 */
};

static void put_words(const case_t *c) {
    memset(g_ram, 0, sizeof(g_ram));
    for (int i = 0; i < c->count; i++)
        wr_w(NULL, ADDR + (uint32_t)i * 2, c->words[i]);
}

static int g_fail;

static void run_cases(mc68k_state_t *cpu, const char *tag, const case_t *cases, size_t n) {
    char buf[BUF_SIZE];

    for (size_t i = 0; i < n; i++) {
        put_words(&cases[i]);
        int len = mc68k_disassemble(cpu, ADDR, buf, sizeof(buf));
        if (len != cases[i].len || strcmp(buf, cases[i].text) != 0) {
            fprintf(stderr, "FAIL %s #%zu (%04X): got %d \"%s\", want %d \"%s\"\n", tag, i,
                    cases[i].words[0], len, buf, cases[i].len, cases[i].text);
            g_fail++;
        }
    }
}

static void test_disasm_fpu_configs(void) {
    static const mc68k_fpu_config_t cfgs[] = {MC68K_FPU_NONE, MC68K_FPU_68881, MC68K_FPU_68882};
    static const mc68k_model_t models[] = {MC68K_MODEL_68020, MC68K_MODEL_68EC030,
                                           MC68K_MODEL_68030};
    char tag[32];

    CHECK(sizeof(g_fpu_cases) / sizeof(g_fpu_cases[0]) > 60);
    for (size_t m = 0; m < sizeof(models) / sizeof(models[0]); m++)
        for (size_t c = 0; c < sizeof(cfgs) / sizeof(cfgs[0]); c++) {
            mc68k_state_t *cpu = make_cpu(models[m], cfgs[c]);
            snprintf(tag, sizeof(tag), "model%zu/fpu%zu", m, c);
            run_cases(cpu, tag, g_fpu_cases, sizeof(g_fpu_cases) / sizeof(g_fpu_cases[0]));
            mc68k_destroy(cpu);
        }
}

/* ---- 全 predicate・全 opmode・EA 可否の全数検査（D-B3・D-B4） --------------------------- */

#define COND_COUNT         32
#define OPMODE_RANGE       0x80 /* command word の opmode 欄は 7bit（$40〜$7F は未定義） */
#define OPMODE_VALID_COUNT 45   /* Table 4-13: 名前付き37 + FSINCOS $30〜$37 の8 */
#define EA_MODES           8
#define EA_REGS            8

/* Table 4-20 の predicate 名（$00〜$1F） */
static const char *const k_cond[COND_COUNT] = {
    "F",   "EQ",  "OGT",  "OGE", "OLT", "OLE", "OGL", "OR",  "UN",  "UEQ", "UGT",
    "UGE", "ULT", "ULE",  "NE",  "T",   "SF",  "SEQ", "GT",  "GE",  "LT",  "LE",
    "GL",  "GLE", "NGLE", "NGL", "NLE", "NLT", "NGE", "NGT", "SNE", "ST",
};

/* Table 4-13 の opmode 名。FSINCOS（$30〜$37）は別扱い */
static const struct {
    uint8_t op;
    const char *name;
} k_ops[] = {
    {0x00, "FMOVE"},   {0x01, "FINT"},    {0x02, "FSINH"},   {0x03, "FINTRZ"},
    {0x04, "FSQRT"},   {0x06, "FLOGNP1"}, {0x08, "FETOXM1"}, {0x09, "FTANH"},
    {0x0A, "FATAN"},   {0x0C, "FASIN"},   {0x0D, "FATANH"},  {0x0E, "FSIN"},
    {0x0F, "FTAN"},    {0x10, "FETOX"},   {0x11, "FTWOTOX"}, {0x12, "FTENTOX"},
    {0x14, "FLOGN"},   {0x15, "FLOG10"},  {0x16, "FLOG2"},   {0x18, "FABS"},
    {0x19, "FCOSH"},   {0x1A, "FNEG"},    {0x1C, "FACOS"},   {0x1D, "FCOS"},
    {0x1E, "FGETEXP"}, {0x1F, "FGETMAN"}, {0x20, "FDIV"},    {0x21, "FMOD"},
    {0x22, "FADD"},    {0x23, "FMUL"},    {0x24, "FSGLDIV"}, {0x25, "FREM"},
    {0x26, "FSCALE"},  {0x27, "FSGLMUL"}, {0x28, "FSUB"},    {0x38, "FCMP"},
    {0x3A, "FTST"},
};
#define K_OPS_COUNT (sizeof(k_ops) / sizeof(k_ops[0]))

static int dis_one(mc68k_state_t *cpu, const uint16_t *w, int n, char *buf) {
    memset(g_ram, 0, sizeof(g_ram));
    for (int i = 0; i < n; i++)
        wr_w(NULL, ADDR + (uint32_t)i * 2, w[i]);
    return mc68k_disassemble(cpu, ADDR, buf, BUF_SIZE);
}

static void expect_text(mc68k_state_t *cpu, const uint16_t *w, int n, int len,
                        const char *text) {
    char buf[BUF_SIZE];
    int got = dis_one(cpu, w, n, buf);

    if (got != len || strcmp(buf, text) != 0) {
        fprintf(stderr, "FAIL %04X %04X: got %d \"%s\", want %d \"%s\"\n", w[0],
                n > 1 ? w[1] : 0, got, buf, len, text);
        g_fail++;
    }
}

static void test_all_predicates(mc68k_state_t *cpu) {
    char want[BUF_SIZE];
    unsigned checked = 0;

    for (unsigned p = 0; p < 2 * COND_COUNT; p++) { /* $20〜$3F は下位5bit と同じ（Note 3） */
        const char *name = k_cond[p & 0x1F];
        uint16_t fb[] = {(uint16_t)(0xF280 | p), 0x0004};
        uint16_t fs[] = {0xF240, (uint16_t)p};
        uint16_t fd[] = {0xF249, (uint16_t)p, 0x0000};
        uint16_t ft[] = {0xF27C, (uint16_t)p};

        snprintf(want, sizeof(want), "FB%s $00001006", name);
        expect_text(cpu, fb, 2, 4, want);
        snprintf(want, sizeof(want), "FS%s D0", name);
        expect_text(cpu, fs, 2, 4, want);
        snprintf(want, sizeof(want), "FDB%s D1,$00001004", name);
        expect_text(cpu, fd, 3, 6, want);
        snprintf(want, sizeof(want), "FTRAP%s", name);
        expect_text(cpu, ft, 2, 4, want);
        checked++;
    }
    CHECK_EQ(checked, 64u); /* predicate 6bit の個数 */
}

static void test_all_opmodes(mc68k_state_t *cpu) {
    char want[BUF_SIZE];
    unsigned valid = 0, visited = 0;

    CHECK_EQ(K_OPS_COUNT, 37u); /* Table 4-13 の名前付き opmode（FSINCOS を除く） */
    for (unsigned op = 0; op < OPMODE_RANGE; op++) {
        visited++;
        const char *name = NULL;
        uint16_t reg[] = {0xF200, (uint16_t)((1u << 10) | (2u << 7) | op)};
        uint16_t mem[] = {0xF210, (uint16_t)(0x4000 | (2u << 10) | (2u << 7) | op)};

        for (size_t i = 0; i < K_OPS_COUNT; i++)
            if (k_ops[i].op == op)
                name = k_ops[i].name;
        if (op >= 0x30 && op <= 0x37) {
            snprintf(want, sizeof(want), "FP1,FP%u:FP2", op & 7u);
            char t[BUF_SIZE];
            snprintf(t, sizeof(t), "FSINCOS.X %s", want);
            expect_text(cpu, reg, 2, 4, t);
            snprintf(t, sizeof(t), "FSINCOS.X (A0),FP%u:FP2", op & 7u);
            expect_text(cpu, mem, 2, 4, t);
            valid++;
        } else if (name) {
            if (op == 0x3A) {
                snprintf(want, sizeof(want), "FTST.X FP1");
                expect_text(cpu, reg, 2, 4, want);
                snprintf(want, sizeof(want), "FTST.X (A0)");
            } else {
                snprintf(want, sizeof(want), "%s.X FP1,FP2", name);
                expect_text(cpu, reg, 2, 4, want);
                snprintf(want, sizeof(want), "%s.X (A0),FP2", name);
            }
            expect_text(cpu, mem, 2, 4, want);
            valid++;
        } else {
            expect_text(cpu, reg, 2, 2, LINE_F);
            expect_text(cpu, mem, 2, 2, LINE_F);
        }
    }
    CHECK_EQ(visited, 0x80u); /* 定数 OPMODE_RANGE と別に、command word の opmode 欄 7bit
                                 の個数を直書きする */
    CHECK_EQ(valid, OPMODE_VALID_COUNT);
}

/* [881UM] Table 4-10 の分類。mode 7 は reg 0〜4 だけが EA（5〜7 は無い） */
static bool is_ea(int m, int r) {
    return m < 7 || r <= 4;
}
static bool is_ctl(int m, int r) {
    return m == 2 || m == 5 || m == 6 || (m == 7 && r <= 3);
}
static bool is_alt(int m, int r) {
    return m < 7 || r <= 1;
}

typedef enum {
    K_SRC_DATA,
    K_SRC_MEM,
    K_DST_DATA,
    K_DST_MEM,
    K_CTL_IN,
    K_CTL_OUT,
    K_MOVEM_PRE,
    K_MOVEM_POST,
    K_FPIAR_IN,
    K_FPIAR_OUT,
} ea_kind_t;

static bool ea_expected(ea_kind_t k, int m, int r) {
    switch (k) {
    case K_SRC_DATA:
        return is_ea(m, r) && m != 1;
    case K_SRC_MEM:
        return is_ea(m, r) && m >= 2;
    case K_DST_DATA:
        return is_ea(m, r) && is_alt(m, r) && m != 1;
    case K_DST_MEM:
        return is_ea(m, r) && is_alt(m, r) && m >= 2;
    case K_CTL_IN:
        return m == 3 || is_ctl(m, r); /* (An)+ か制御 */
    case K_CTL_OUT:
        return (is_ctl(m, r) && is_alt(m, r)) || m == 4; /* 制御交代可能か -(An) */
    case K_MOVEM_PRE:
        return m == 4;
    case K_MOVEM_POST:
        return is_ctl(m, r) && is_alt(m, r);
    case K_FPIAR_IN:
        return is_ea(m, r); /* Any */
    case K_FPIAR_OUT:
        return is_ea(m, r) && is_alt(m, r); /* Alterable */
    }
    return false;
}

/* opcode 下位6bit の EA を総当りし、可否が表と一致し、可のとき LINE-F でないことを確認する */
static unsigned ea_matrix(mc68k_state_t *cpu, const char *tag, uint16_t opbase, uint16_t cmd,
                          ea_kind_t kind) {
    unsigned n = 0;
    char buf[BUF_SIZE];

    for (int m = 0; m < EA_MODES; m++)
        for (int r = 0; r < EA_REGS; r++) {
            uint16_t w[] = {(uint16_t)(opbase | (m << 3) | r), cmd};
            int words = (opbase == 0xF300 || opbase == 0xF340)
                            ? 1
                            : 2; /* FSAVE/FRESTORE は command word が無い */
            int len = dis_one(cpu, w, words, buf);
            bool is_line_f = strcmp(buf, LINE_F) == 0;
            bool want = ea_expected(kind, m, r);

            if (want == is_line_f || (is_line_f && len != 2)) {
                fprintf(stderr, "FAIL ea %s mode %d reg %d: \"%s\" len %d, want %s\n", tag, m,
                        r, buf, len, want ? "valid" : LINE_F);
                g_fail++;
            }
            n++;
        }
    return n;
}

static void test_ea_matrices(mc68k_state_t *cpu) {
    unsigned n = 0;
    unsigned fmt;

    /* class 2（<ea>,FPn）と class 3（FPn,<ea>）の形式別: L S W B は Data、X P D は Memory */
    for (fmt = 0; fmt < 7; fmt++) {
        bool data = (fmt == 0 || fmt == 1 || fmt == 4 || fmt == 6);
        n += ea_matrix(cpu, "cls2", 0xF200, (uint16_t)(0x4000 | (fmt << 10) | 0x22),
                       data ? K_SRC_DATA : K_SRC_MEM);
        n += ea_matrix(cpu, "cls3", 0xF200, (uint16_t)(0x6000 | (fmt << 10)),
                       data ? K_DST_DATA : K_DST_MEM);
    }
    n += ea_matrix(cpu, "cls3 P dynamic", 0xF200, 0x7C20, K_DST_MEM);
    /* control register: 単独の FPCR/FPSR は Data（出力は Data Alterable）、FPIAR は
     * Any/Alterable、 複数は Memory（出力は Memory Alterable） */
    n += ea_matrix(cpu, "ctl in FPCR", 0xF200, 0x9000, K_SRC_DATA);
    n += ea_matrix(cpu, "ctl in FPSR", 0xF200, 0x8800, K_SRC_DATA);
    n += ea_matrix(cpu, "ctl in FPIAR", 0xF200, 0x8400, K_FPIAR_IN);
    n += ea_matrix(cpu, "ctl in multi", 0xF200, 0x9800, K_SRC_MEM);
    n += ea_matrix(cpu, "ctl out FPCR", 0xF200, 0xB000, K_DST_DATA);
    n += ea_matrix(cpu, "ctl out FPIAR", 0xF200, 0xA400, K_FPIAR_OUT);
    n += ea_matrix(cpu, "ctl out multi", 0xF200, 0xBC00, K_DST_MEM);
    /* FMOVEM.X: 入力（mode 10）は (An)+ か制御、出力は mode 10 が制御交代可能、mode 00 が -(An)
     * だけ */
    n += ea_matrix(cpu, "movem in", 0xF200, 0xD0F0, K_CTL_IN);
    n += ea_matrix(cpu, "movem out ctl", 0xF200, 0xF0F0, K_MOVEM_POST);
    n += ea_matrix(cpu, "movem out pre", 0xF200, 0xE0F0, K_MOVEM_PRE);
    /* FSAVE（制御交代可能か -(An)）、FRESTORE（制御か (An)+） */
    n += ea_matrix(cpu, "fsave", 0xF300, 0, K_CTL_OUT);
    n += ea_matrix(cpu, "frestore", 0xF340, 0, K_CTL_IN);
    /* 7 形式×2 + 1 + 7 + 3 + 2 の matrix（各 64 組） */
    CHECK_EQ(n, 64u * (7 * 2 + 1 + 7 + 3 + 2));
}

/* FScc: mode 7 reg 2〜4 は FTRAPcc、mode 1 は FDBcc なので、残りが data alterable */
static void test_fscc_ea(mc68k_state_t *cpu) {
    char buf[BUF_SIZE];
    unsigned n = 0;

    for (int m = 0; m < EA_MODES; m++)
        for (int r = 0; r < EA_REGS; r++) {
            uint16_t w[] = {(uint16_t)(0xF240 | (m << 3) | r), 0x0001};
            bool other = (m == 1) || (m == 7 && r >= 2 && r <= 4);
            bool want = is_alt(m, r) && m != 1 && !other;

            if (other)
                continue;
            dis_one(cpu, w, 2, buf);
            if ((strcmp(buf, LINE_F) != 0) != want) {
                fprintf(stderr, "FAIL FScc mode %d reg %d: \"%s\"\n", m, r, buf);
                g_fail++;
            }
            n++;
        }
    CHECK_EQ(n, 64u - 8u - 3u); /* mode 1 の8通りと mode 7 reg 2〜4 の3通りを除く */
}

static void test_exhaustive(void) {
    static const mc68k_fpu_config_t cfgs[] = {MC68K_FPU_NONE, MC68K_FPU_68882};

    for (size_t c = 0; c < sizeof(cfgs) / sizeof(cfgs[0]); c++) {
        mc68k_state_t *cpu = make_cpu(MC68K_MODEL_68030, cfgs[c]);
        test_all_predicates(cpu);
        test_all_opmodes(cpu);
        test_ea_matrices(cpu);
        test_fscc_ea(cpu);
        mc68k_destroy(cpu);
    }
}

/* 68000/68010 は F-line を FPU 命令として扱わない */
static void test_disasm_pre68020(void) {
    static const case_t cases[] = {
        {{0xF280, 0x0000}, 2, 2, LINE_F},
        {{0xF200, 0x0500}, 2, 2, LINE_F},
        {{0xF310}, 1, 2, LINE_F},
    };
    static const mc68k_model_t models[] = {MC68K_MODEL_68000, MC68K_MODEL_68010};

    for (size_t m = 0; m < sizeof(models) / sizeof(models[0]); m++) {
        mc68k_state_t *cpu = make_cpu(models[m], MC68K_FPU_NONE);
        run_cases(cpu, "pre68020", cases, sizeof(cases) / sizeof(cases[0]));
        mc68k_destroy(cpu);
    }
}

/* control register 出力（FMOVE FPcr,<ea>）。応答は NULL を挟まず EA_TRANSFER から始まる */
static uint32_t read_ctrl(mc68k_fpu_t *f, unsigned select) {
    mc68k_fpu_response_t r;
    uint32_t v;

    CHECK(mc68k_fpu_write_command_word(f, CMD(5, select, 0, 0)));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.transfer_to_host, 1);
    CHECK_EQ(r.length, 4);
    CHECK(mc68k_fpu_read_operand(f, &v));
    expect_idle(f);
    return v;
}

/* mc68k_get_fpu_regs() と command による読み戻しの一致 */
static void test_get_fpu_regs_matches_commands(void) {
    static const mc68k_fpu_config_t cfgs[] = {MC68K_FPU_68881, MC68K_FPU_68882};

    for (size_t c = 0; c < sizeof(cfgs) / sizeof(cfgs[0]); c++) {
        mc68k_fpu_t *f = mc68k_fpu_create(cfgs[c]);
        uint32_t out[3], iar = 0x00ABCDEF;
        mc68k_fpu_regs_t r;

        CHECK(f != NULL);
        drive_ctrl_in(f, CTL_FPIAR, &iar, 1);
        load_long(f, 3, 7);
        /* FP の読み戻しは FPCR の ENABLE が0のうちに済ませる（算術・FMOVE は ENABLE があると
         * PC=1） */
        drive_ea_out(f, CMD(3, FMT_X, 3, 0), out, 3, 12);
        set_fpcr(f, 0x0000A590u); /* ENABLE=$A5、PREC=10、RND=01 */
        set_fpsr(f, 0x0A123458u); /* 各 byte が異なる値（test_fpu_model_dialog と同じ） */
        r = regs(f);
        CHECK_EQ(out[0] >> 16, r.fp[3].sign_exp);
        CHECK_EQ(((uint64_t)out[1] << 32) | out[2], r.fp[3].signif);
        CHECK_EQ(r.fp[3].sign_exp, 0x4001u); /* 7 = 1.75 * 2^2 */
        CHECK_EQ(r.fp[3].signif, 0xE000000000000000ull);

        CHECK_EQ(read_ctrl(f, CTL_FPCR), r.fpcr);
        CHECK_EQ(r.fpcr, 0x0000A590u); /* byte 順の取り違えを検出する */
        CHECK_EQ(read_ctrl(f, CTL_FPSR), r.fpsr);
        CHECK_EQ(r.fpsr, 0x0A123458u); /* 絶対値でも照合する（読み戻しとの比較だけでは byte
                                          順の取り違えを見逃す） */
        CHECK_EQ(read_ctrl(f, CTL_FPIAR), r.fpiar);
        CHECK_EQ(r.fpiar, iar);
        mc68k_get_fpu_regs(NULL, &r); /* NULL 引数は何もしない */
        mc68k_get_fpu_regs(f, NULL);
        mc68k_fpu_destroy(f);
    }
}

int main(void) {
    test_get_fpu_regs_matches_commands();
    test_disasm_fpu_configs();
    test_exhaustive();
    test_disasm_pre68020();
    if (g_fail) {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    return 0;
}
