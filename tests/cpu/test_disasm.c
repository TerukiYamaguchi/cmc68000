/*
 * test_disasm.c — 逆アセンブラ mc68k_disassemble()の
 * ユニットテスト。代表的な命令×アドレッシングモードの組み合わせを
 * テーブル駆動で検証する（全65536オペコードの網羅ではなく、各整形パスを
 * 少なくとも1回通す）。
 *
 * mc68k_disassemble() は非侵襲（peek_byte経由）なので、CPUを実行せず
 * RAMへバイト列を書いてそのまま呼び出すだけでよい。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "mc68000/debug.h"

#define RAM_SIZE 0x10000u

typedef struct {
    uint8_t ram[RAM_SIZE];
} test_ctx_t;

static bool rd_b(void *ctx, uint32_t addr, uint8_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    *out = (addr < RAM_SIZE) ? t->ram[addr] : 0xFF;
    return addr < RAM_SIZE;
}
static bool rd_w(void *ctx, uint32_t addr, uint16_t *out) {
    uint8_t hi = 0, lo = 0;
    bool ok1 = rd_b(ctx, addr, &hi);
    bool ok2 = rd_b(ctx, addr + 1, &lo);
    *out = (uint16_t)((hi << 8) | lo);
    return ok1 && ok2;
}
static bool rd_l(void *ctx, uint32_t addr, uint32_t *out) {
    uint16_t hi = 0, lo = 0;
    bool ok1 = rd_w(ctx, addr, &hi);
    bool ok2 = rd_w(ctx, addr + 2, &lo);
    *out = ((uint32_t)hi << 16) | lo;
    return ok1 && ok2;
}
static bool wr_b(void *ctx, uint32_t addr, uint8_t v) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr < RAM_SIZE)
        t->ram[addr] = v;
    return addr < RAM_SIZE;
}
static bool wr_w(void *ctx, uint32_t addr, uint16_t v) {
    bool ok1 = wr_b(ctx, addr, (uint8_t)(v >> 8));
    bool ok2 = wr_b(ctx, addr + 1, (uint8_t)v);
    return ok1 && ok2;
}
static bool wr_l(void *ctx, uint32_t addr, uint32_t v) {
    bool ok1 = wr_w(ctx, addr, (uint16_t)(v >> 16));
    bool ok2 = wr_w(ctx, addr + 2, (uint16_t)v);
    return ok1 && ok2;
}
static void sync_cb(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}
static void wh_cb(void *ctx, uint32_t addr, uint32_t size) {
    (void)ctx;
    (void)addr;
    (void)size;
}

/* peek: rd_b等と同じ配列を読むだけ（非侵襲、副作用なし） */
static uint8_t peek_b(void *ctx, uint32_t addr) {
    uint8_t v = 0;
    rd_b(ctx, addr, &v);
    return v;
}
static uint16_t peek_w(void *ctx, uint32_t addr) {
    uint16_t v = 0;
    rd_w(ctx, addr, &v);
    return v;
}
static uint32_t peek_l(void *ctx, uint32_t addr) {
    uint32_t v = 0;
    rd_l(ctx, addr, &v);
    return v;
}

static mc68k_state_t *make_cpu(test_ctx_t *t) {
    mc68k_bus_t bus = {
        .read_byte = rd_b,
        .read_word = rd_w,
        .read_long = rd_l,
        .write_byte = wr_b,
        .write_word = wr_w,
        .write_long = wr_l,
        .sync = sync_cb,
        .write_hook = wh_cb,
        .iack = NULL,
        .reset_peripheral = NULL,
        .ctx = t,
    };
    mc68k_state_t *cpu = mc68k_create(&bus);
    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, t);
    return cpu;
}

static mc68k_state_t *make_cpu_68020(test_ctx_t *t) {
    mc68k_bus_t bus = {.read_byte = rd_b,
                       .read_word = rd_w,
                       .read_long = rd_l,
                       .write_byte = wr_b,
                       .write_word = wr_w,
                       .write_long = wr_l,
                       .sync = sync_cb,
                       .write_hook = wh_cb,
                       .ctx = t};
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, t);
    return cpu;
}

#define ADDR 0x1000u

typedef struct {
    const char *name;   /* テストケース名（失敗時の特定用） */
    uint16_t words[6];  /* オペコード＋拡張ワード列（0で埋め、count分だけ使う） */
    int count;          /* words の有効ワード数 */
    int expect_len;     /* 期待する命令バイト長 */
    const char *expect; /* 期待する逆アセンブル文字列 */
} case_t;

/* 失敗しても即abortせず全件チェックしてから最後にまとめて報告する
 * （デバッグ時に一度に全ての不一致を見るため）。 */
static int g_fail_count = 0;

static void run_case(test_ctx_t *t, mc68k_state_t *cpu, const case_t *c) {
    memset(t->ram, 0, sizeof(t->ram));
    for (int i = 0; i < c->count; i++)
        wr_w(t, ADDR + (uint32_t)i * 2, c->words[i]);

    char buf[128];
    int len = mc68k_disassemble(cpu, ADDR, buf, sizeof(buf));
    if (len != c->expect_len || strcmp(buf, c->expect) != 0) {
        fprintf(stderr, "FAIL [%s]: got len=%d text=\"%s\", want len=%d text=\"%s\"\n", c->name,
                len, buf, c->expect_len, c->expect);
        g_fail_count++;
    }
}

static const case_t g_cases[] = {
    /* --- EAモード網羅（MOVE.L経由） --- */
    {"move_dn_an", {0x2001}, 1, 2, "MOVE.L D1,D0"},
    {"move_an_direct", {0x2009}, 1, 2, "MOVE.L A1,D0"},
    {"movea_an", {0x2249}, 1, 2, "MOVEA.L A1,A1"},
    {"move_ind", {0x2011}, 1, 2, "MOVE.L (A1),D0"},
    {"move_postinc", {0x2019}, 1, 2, "MOVE.L (A1)+,D0"},
    {"move_predec", {0x2021}, 1, 2, "MOVE.L -(A1),D0"},
    {"move_d16an", {0x2029, 0x000C}, 2, 4, "MOVE.L 12(A1),D0"},
    {"move_d8anxn", {0x2031, 0x3004}, 2, 4, "MOVE.L 4(A1,D3.W),D0"},
    {"move_absw", {0x2038, 0x1234}, 2, 4, "MOVE.L $00001234.W,D0"},
    {"move_absl", {0x2039, 0x0012, 0x3456}, 3, 6, "MOVE.L $00123456.L,D0"},
    {"move_d16pc", {0x203A, 0x0004}, 2, 4, "MOVE.L $001006(PC),D0"},
    {"move_d8pcxn", {0x203B, 0x1002}, 2, 4, "MOVE.L $001004(PC,D1.W),D0"},
    {"move_imm", {0x203C, 0x0012, 0x3456}, 3, 6, "MOVE.L #$00123456,D0"},
    {"move_to_ind", {0x2280}, 1, 2, "MOVE.L D0,(A1)"},

    /* --- MOVEA / MOVE.B / MOVE.W サイズ --- */
    {"move_b", {0x1001}, 1, 2, "MOVE.B D1,D0"},
    {"move_w", {0x3001}, 1, 2, "MOVE.W D1,D0"},

    /* --- Bcc/BSR/BRA --- */
    {"bra_s", {0x6004}, 1, 2, "BRA $001006"},
    {"bra_w", {0x6000, 0x0010}, 2, 4, "BRA $001012"},
    {"bsr_s", {0x6104}, 1, 2, "BSR $001006"},
    {"beq_s", {0x6704}, 1, 2, "BEQ $001006"},
    {"bne_s", {0x6604}, 1, 2, "BNE $001006"},

    /* --- DBcc / Scc --- */
    {"dbra", {0x51C8, 0x0004}, 2, 4, "DBRA D0,$001006"},
    {"dbeq", {0x57C8, 0x0004}, 2, 4, "DBEQ D0,$001006"},
    {"seq", {0x57C0}, 1, 2, "SEQ D0"},
    {"sf", {0x51C1}, 1, 2, "SF D1"},

    /* --- MOVEQ --- */
    {"moveq_pos", {0x7005}, 1, 2, "MOVEQ #5,D0"},
    {"moveq_neg", {0x70FF}, 1, 2, "MOVEQ #-1,D0"},

    /* --- ADDQ/SUBQ --- */
    {"addq", {0x5440}, 1, 2, "ADDQ.W #2,D0"},
    {"subq_8", {0x5100}, 1, 2, "SUBQ.B #8,D0"},

    /* --- ADD/SUB/CMP/AND/OR/EOR（EA,Dn と Dn,EA） --- */
    {"add_ea_dn", {0xD001}, 1, 2, "ADD.B D1,D0"},
    {"add_dn_ea", {0xD111}, 1, 2, "ADD.B D0,(A1)"},
    {"adda_w", {0xD0C1}, 1, 2, "ADDA.W D1,A0"},
    {"adda_l", {0xD1C1}, 1, 2, "ADDA.L D1,A0"},
    {"sub_ea_dn", {0x9001}, 1, 2, "SUB.B D1,D0"},
    {"suba_l", {0x91C1}, 1, 2, "SUBA.L D1,A0"},
    {"cmp_ea_dn", {0xB001}, 1, 2, "CMP.B D1,D0"},
    {"cmpa_w", {0xB0C1}, 1, 2, "CMPA.W D1,A0"},
    {"and_ea_dn", {0xC001}, 1, 2, "AND.B D1,D0"},
    {"and_dn_ea", {0xC111}, 1, 2, "AND.B D0,(A1)"},
    {"or_ea_dn", {0x8001}, 1, 2, "OR.B D1,D0"},
    {"eor_dn_ea", {0xB101}, 1, 2, "EOR.B D0,D1"},

    /* --- ADDI/SUBI/CMPI/ANDI/ORI/EORI --- */
    {"addi_b", {0x0600, 0x0005}, 2, 4, "ADDI.B #$05,D0"},
    {"cmpi_l", {0x0C81, 0x0000, 0x0064}, 3, 6, "CMPI.L #$00000064,D1"},
    {"ori_ccr", {0x003C, 0x0011}, 2, 4, "ORI #$11,CCR"},
    {"andi_sr", {0x027C, 0x2700}, 2, 4, "ANDI #$2700,SR"},
    {"eori_w", {0x0A40, 0x00FF}, 2, 4, "EORI.W #$00FF,D0"},

    /* --- MULU/MULS/DIVU/DIVS --- */
    {"mulu", {0xC0C1}, 1, 2, "MULU.W D1,D0"},
    {"muls", {0xC1C1}, 1, 2, "MULS.W D1,D0"},
    {"divu", {0x80C1}, 1, 2, "DIVU.W D1,D0"},
    {"divs", {0x81C1}, 1, 2, "DIVS.W D1,D0"},

    /* --- ADDX/SUBX/ABCD/SBCD（レジスタ形式・プレデクリメント形式） --- */
    {"addx_reg", {0xD100}, 1, 2, "ADDX.B D0,D0"},
    {"addx_mem", {0xD108}, 1, 2, "ADDX.B -(A0),-(A0)"},
    {"subx_reg", {0x9300}, 1, 2, "SUBX.B D0,D1"},
    {"abcd_reg", {0xC100}, 1, 2, "ABCD D0,D0"},
    {"sbcd_mem", {0x8108}, 1, 2, "SBCD -(A0),-(A0)"},
    {"cmpm", {0xB308}, 1, 2, "CMPM.B (A0)+,(A1)+"},

    /* --- CHK/NBCD/TAS/CLR/NEG/NEGX/NOT/TST --- */
    {"chk", {0x4181}, 1, 2, "CHK D1,D0"},
    {"nbcd", {0x4800}, 1, 2, "NBCD D0"},
    {"tas", {0x4AC0}, 1, 2, "TAS D0"},
    {"illegal_sentinel", {0x4AFC}, 1, 2, "ILLEGAL"},
    {"clr_w", {0x4240}, 1, 2, "CLR.W D0"},
    {"neg_l", {0x4480}, 1, 2, "NEG.L D0"},
    {"negx_b", {0x4000}, 1, 2, "NEGX.B D0"},
    {"not_w", {0x4640}, 1, 2, "NOT.W D0"},
    {"tst_b", {0x4A00}, 1, 2, "TST.B D0"},

    /* --- BTST/BCHG/BCLR/BSET（静的・動的） --- */
    {"btst_static", {0x0800, 0x0003}, 2, 4, "BTST #$03,D0"},
    {"bset_dynamic", {0x03C0}, 1, 2, "BSET D1,D0"},
    {"bchg_mem", {0x0850, 0x0002}, 2, 4, "BCHG #$02,(A0)"},

    /* --- MOVE CCR/SR/USP --- */
    {"move_from_ccr", {0x42C0}, 1, 2, "MOVE CCR,D0"},
    {"move_to_ccr", {0x44C0}, 1, 2, "MOVE D0,CCR"},
    {"move_from_sr", {0x40C0}, 1, 2, "MOVE SR,D0"},
    {"move_to_sr", {0x46C0}, 1, 2, "MOVE D0,SR"},
    {"move_an_usp", {0x4E60}, 1, 2, "MOVE A0,USP"},
    {"move_usp_an", {0x4E68}, 1, 2, "MOVE USP,A0"},

    /* --- MC68010: MOVEC/RTD/MOVES（モデル引数なし、opcodeビットのみで判定） --- */
    {"movec_vbr_to_dn", {0x4E7A, 0x0801}, 2, 4, "MOVEC VBR,D0"},
    {"movec_dn_to_vbr", {0x4E7B, 0x1801}, 2, 4, "MOVEC D1,VBR"},
    {"movec_illegal_creg", {0x4E7A, 0x0123}, 2, 4, "MOVEC $123,D0"},
    {"rtd_pos", {0x4E74, 0x0010}, 2, 4, "RTD #16"},
    {"rtd_neg", {0x4E74, 0xFFFC}, 2, 4, "RTD #-4"},
    {"moves_mem_to_dn_l", {0x0E90, 0x0000}, 2, 4, "MOVES.L (A0),D0"},
    {"moves_dn_to_mem_b", {0x0E10, 0x1800}, 2, 4, "MOVES.B D1,(A0)"},
    {"moves_mem_to_an_w", {0x0E50, 0x9000}, 2, 4, "MOVES.W (A0),A1"},
    {"moves_ea_dn_invalid", {0x0E00}, 1, 2, "ILLEGAL"}, /* Dn直接はMemory Alterable外 */

    /* --- LEA/PEA/JMP/JSR/RTS --- */
    {"lea", {0x41E9, 0x0004}, 2, 4, "LEA 4(A1),A0"},
    {"pea", {0x4851}, 1, 2, "PEA (A1)"},
    {"jmp", {0x4ED1}, 1, 2, "JMP (A1)"},
    {"jsr", {0x4E91}, 1, 2, "JSR (A1)"},
    {"rts", {0x4E75}, 1, 2, "RTS"},

    /* --- SWAP/EXT/EXG --- */
    {"swap", {0x4840}, 1, 2, "SWAP D0"},
    {"ext_w", {0x4880}, 1, 2, "EXT.W D0"},
    {"ext_l", {0x48C0}, 1, 2, "EXT.L D0"},
    {"exg_dd", {0xC141}, 1, 2, "EXG D0,D1"},
    {"exg_aa", {0xC149}, 1, 2, "EXG A0,A1"},
    {"exg_da", {0xC189}, 1, 2, "EXG D0,A1"},

    /* --- MOVEM --- */
    {"movem_store_predec", {0x48A1, 0xC000}, 2, 4, "MOVEM.W D0-D1,-(A1)"},
    {"movem_load_postinc", {0x4CD9, 0x0003}, 2, 4, "MOVEM.L (A1)+,D0-D1"},
    {"movem_store_ctrl", {0x4891, 0x0001}, 2, 4, "MOVEM.W D0,(A1)"},

    /* --- MOVEP --- */
    {"movep_w_load", {0x0108, 0x0004}, 2, 4, "MOVEP.W 4(A0),D0"},
    {"movep_l_store", {0x01C8, 0x0008}, 2, 4, "MOVEP.L D0,8(A0)"},

    /* --- LINK/UNLK --- */
    {"link", {0x4E56, 0xFFE0}, 2, 4, "LINK A6,#-32"},
    {"unlk", {0x4E5E}, 1, 2, "UNLK A6"},

    /* --- TRAP --- */
    {"trap15", {0x4E4F}, 1, 2, "TRAP #15"},

    /* --- システム制御 --- */
    {"nop", {0x4E71}, 1, 2, "NOP"},
    {"rte", {0x4E73}, 1, 2, "RTE"},
    {"rtr", {0x4E77}, 1, 2, "RTR"},
    {"reset", {0x4E70}, 1, 2, "RESET"},
    {"stop", {0x4E72, 0x2700}, 2, 4, "STOP #$2700"},
    {"trapv", {0x4E76}, 1, 2, "TRAPV"},

    /* --- シフト/ローテート（レジスタ・メモリ形式） --- */
    {"asl_reg", {0xE300}, 1, 2, "ASL.B #1,D0"},
    {"asr_dn_count", {0xE069}, 1, 2, "LSR.W D0,D1"},
    {"lsl_mem", {0xE3D0}, 1, 2, "LSL (A0)"},
    {"rol_reg", {0xE318}, 1, 2, "ROL.B #1,D0"},
    {"roxr_reg", {0xE010}, 1, 2, "ROXR.B #8,D0"},

    /* --- LINE-A / LINE-F --- */
    {"line_a", {0xA000}, 1, 2, "LINE-A"},
    {"line_f", {0xF000}, 1, 2, "LINE-F"},
};

static void test_hand_written_cases(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu(&t);
    int n = (int)(sizeof(g_cases) / sizeof(g_cases[0]));
    for (int i = 0; i < n; i++)
        run_case(&t, cpu, &g_cases[i]);
    mc68k_destroy(cpu);
    assert(g_fail_count == 0);
}

static void test_68020_cases(void) {
    test_ctx_t t;
    memset(&t, 0, sizeof(t));
    mc68k_state_t *cpu = make_cpu_68020(&t);
    char buf[128];
    const case_t cases[] = {
        {"extb", {0x49C0}, 1, 2, "EXTB.L D0"},
        {"linkl", {0x4808, 0x0000, 0x0004}, 3, 6, "LINK.L A0,#4"},
        {"bral", {0x60FF, 0x0000, 0x0002}, 3, 6, "BRA $00001004"},
        {"pack", {0x8140, 0x0000}, 2, 4, "PACK D0,D0,#$0000"},
        {"bfextu", {0xE9C0, 0x1708}, 2, 4, "BFEXTU D0{28:8},D1"},
        {"cas", {0x0AC0, 0x0000}, 2, 4, "CAS.B D0,D0,D0"},
        /* #49: 0x08C0-0x08C7（sc=00）は CAS ではなく静的 BSET #imm,Dn。
         * CAS 分岐が sc=0 を横取りして ILLEGAL と表示する不具合の回帰。 */
        {"bset_static_not_cas_dn", {0x08C0, 0x000E}, 2, 4, "BSET #$0E,D0"},
        {"bset_static_not_cas_mem", {0x08D0, 0x0003}, 2, 4, "BSET #$03,(A0)"},
        /* #45: UNPK が opcode & 0xF1B0 == 0x8100 の bit7 固定判定で漏れ、
         * ILLEGAL と表示されていた不具合の回帰。 */
        {"unpk_dn", {0x8583, 0x0000}, 2, 4, "UNPK D3,D2,#$0000"},
        {"unpk_predec", {0x858B, 0x0000}, 2, 4, "UNPK -(A3),-(A2),#$0000"},
        /* #45: CMP2/CHK2 判定マスク 0xF8C0 が bit8 を無視し、bit8=1 の
         * MOVEP.L D0,(d16,A0) を CMP2.B と誤判定していた不具合の回帰。 */
        {"movep_l_store_68020", {0x01C8, 0x0008}, 2, 4, "MOVEP.L D0,8(A0)"},
        {"cmp2_b", {0x00D0, 0x0000}, 2, 4, "CMP2.B (A0),D0"},
        /* #45: CAS2 のオペランドが Dc1:Du1,Dc2:Du2 の誤った組で表示され、
         * ext ワードの Dc1/Dc2/Du1/Du2 抽出順（op_cas2()と同じ）と食い違って
         * いた不具合の回帰。 */
        {"cas2_w", {0x0CFC, 0x8040, 0x9083}, 3, 6, "CAS2.W D0:D3,D1:D2,(A0):(A1)"},
        /* #65: full-format の base と index の間のカンマ抜け、post-index で index 抑制時の
         * 二重カンマ、od 無しの末尾カンマの回帰。LEA <ea>,A1 の拡張 word は
         * D4.L*4 = 0x4C00、bit8 = full、bs = 0x80、is = 0x40、bd = bits5-4、iis = bits2-0。 */
        {"full_noind", {0x43F0, 0x4D20, 0x0010}, 3, 6, "LEA (16,A0,D4.L*4),A1"},
        {"full_pre_od", {0x43F0, 0x4D22, 0x0010, 0x0020}, 4, 8, "LEA ([16,A0,D4.L*4],32),A1"},
        {"full_post_od", {0x43F0, 0x4D26, 0x0010, 0x0020}, 4, 8, "LEA ([16,A0],D4.L*4,32),A1"},
        {"full_post_nood", {0x43F0, 0x4D25, 0x0010}, 3, 6, "LEA ([16,A0],D4.L*4),A1"},
        {"full_is_od", {0x43F0, 0x4D62, 0x0010, 0x0020}, 4, 8, "LEA ([16,A0],32),A1"},
        {"full_bs_long",
         {0x43F0, 0x4DB3, 0x0000, 0x1234, 0x0000, 0x5678},
         6,
         12,
         "LEA ([$00001234,0,D4.L*4],$00005678),A1"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(t.ram, 0, sizeof(t.ram));
        for (int w = 0; w < cases[i].count; w++)
            wr_w(&t, ADDR + w * 2, cases[i].words[w]);
        int len = mc68k_disassemble(cpu, ADDR, buf, sizeof(buf));
        if (len != cases[i].expect_len || strcmp(buf, cases[i].expect) != 0)
            fprintf(stderr, "%s: %d %s\n", cases[i].name, len, buf);
        assert(len == cases[i].expect_len);
        assert(strcmp(buf, cases[i].expect) == 0);
    }
    mc68k_destroy(cpu);
}

/* peek未登録・NULL引数・奇数アドレスは-1を返しbufに触れない */
static void test_failure_contract(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    wr_w(&t, ADDR, 0x4E71); /* NOP */

    mc68k_bus_t bus = {
        .read_byte = rd_b,
        .read_word = rd_w,
        .read_long = rd_l,
        .write_byte = wr_b,
        .write_word = wr_w,
        .write_long = wr_l,
        .sync = sync_cb,
        .write_hook = wh_cb,
        .iack = NULL,
        .reset_peripheral = NULL,
        .ctx = &t,
    };
    mc68k_state_t *cpu = mc68k_create(&bus); /* peek未登録 */

    char buf[16] = "UNTOUCHED";
    assert(mc68k_disassemble(cpu, ADDR, buf, sizeof(buf)) == -1);
    assert(strcmp(buf, "UNTOUCHED") == 0); /* bufに触れていない */

    mc68k_set_peek_hooks(cpu, peek_b, peek_w, peek_l, &t);
    assert(mc68k_disassemble(cpu, ADDR + 1, buf, sizeof(buf)) == -1); /* 奇数アドレス */
    assert(mc68k_disassemble(NULL, ADDR, buf, sizeof(buf)) == -1);
    assert(mc68k_disassemble(cpu, ADDR, NULL, sizeof(buf)) == -1);
    assert(mc68k_disassemble(cpu, ADDR, buf, 0) == -1);

    /* 正常系: 登録後は動く */
    assert(mc68k_disassemble(cpu, ADDR, buf, sizeof(buf)) == 2);
    assert(strcmp(buf, "NOP") == 0);

    mc68k_destroy(cpu);
}

/* バッファが小さい場合、安全に切り詰められ必ずNUL終端される */
static void test_buffer_truncation(void) {
    test_ctx_t t;
    mc68k_state_t *cpu = make_cpu(&t);
    memset(t.ram, 0, sizeof(t.ram));
    wr_w(&t, ADDR, 0x2039); /* MOVE.L $00123456.L,D0 */
    wr_l(&t, ADDR + 2, 0x00123456);

    char buf[8];
    int len = mc68k_disassemble(cpu, ADDR, buf, sizeof(buf));
    assert(len == 6); /* 命令長は切り詰めに関わらず正しい */
    assert(strlen(buf) < sizeof(buf));

    mc68k_destroy(cpu);
}

int main(void) {
    test_hand_written_cases();
    test_68020_cases();
    test_failure_contract();
    test_buffer_truncation();
    return 0;
}
