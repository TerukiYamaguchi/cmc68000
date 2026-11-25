/*
 * disasm.c — 逆アセンブラ
 *
 * 独立モジュール。他のデバッガ機能（ブレークポイント/debug_run/peek）から
 * 依存されない（不要になればこのファイルを削除するだけで着脱可能）。
 * 逆に、このファイル自身は include/mc68000 配下の公開APIのみに依存し、
 * src/cpu/cpu_internal.h（内部ヘッダ）を一切includeしない。読み出しは
 * mc68k_debug_read_bytes()経由の非侵襲アクセスのみで、
 * ゲスト実行に一切影響しない（PC・レジスタ・exec_stateを変更しない、
 * バスエラーを誘発しない）。
 *
 * 実行用 op_table は使わない（意図的な設計判断）。理由:
 *   1. mc68k_set_opcode_illegal() が正当な命令のmnemonicを実行時に
 *      "ILLEGAL" へ書き換える（debugger.c）。逆アセンブラがこれを参照すると
 *      「本当は何の命令か」という逆アセンブラの目的に反した結果になる
 *   2. Line-A/Line-F も同様に実行時に書き換えられている（m68000.c）
 *   3. mc68k_state_t は不透明型であり、op_table への到達自体が内部ヘッダを
 *      要求してしまう（独立モジュールの原則に反する）
 * 代わりに、opcodeのビットパターンから完全に独立して命令を判定する
 * （decode.c の各 register_*_ops() が構築する最終的なテーブル状態と
 * 同じ結果になるよう、各命令ハンドラのビット抽出ロジックをそのまま
 * 逆アセンブラ用に再実装している）。
 *
 * 表記規約:
 *   - レジスタ: D0-D7 / A0-A7
 *   - ディスプレースメント（d16(An), d8(An,Xn), LINK #disp 等）: 符号付き10進数
 *   - 分岐先・PC相対の解決済みアドレス: $XXXXXX（6桁16進、cpu_internal.hのBADADDR等と
 *     同じくPCは常に妥当な範囲という前提）
 *   - abs.W/abs.L オペランドの実効アドレス: $XXXXXXXX（8桁16進、BADMOVEA/
 *     BADADDRと同じ流儀。ポインタ破損（上位バイトが不正）を隠さず見せるため
 *     マスクしない）
 *   - イミディエイト: #$XX（byte）/ #$XXXX（word）/ #$XXXXXXXX（long）
 */
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/debug.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/* 非侵襲ワード/ロング読み出し（mc68k_debug_read_bytes 経由）           */
/* ================================================================== */

static bool dis_read_word(mc68k_state_t *cpu, uint32_t *cursor, uint16_t *out) {
    uint8_t b[2];
    if (mc68k_debug_read_bytes(cpu, *cursor, b, 2) != 0)
        return false;
    *out = (uint16_t)((b[0] << 8) | b[1]);
    *cursor += 2;
    return true;
}

static bool dis_read_long(mc68k_state_t *cpu, uint32_t *cursor, uint32_t *out) {
    uint16_t hi, lo;
    if (!dis_read_word(cpu, cursor, &hi))
        return false;
    if (!dis_read_word(cpu, cursor, &lo))
        return false;
    *out = ((uint32_t)hi << 16) | lo;
    return true;
}

/* ================================================================== */
/* レジスタ名・サイズサフィックス                                       */
/* ================================================================== */

static const char *const g_dreg[8] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"};
static const char *const g_areg[8] = {"A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"};

/* サイズコード（0=B,1=W,2=L。3は呼び出し側で弾く）→ バイト数 */
static const int g_sz[3] = {1, 2, 4};
static const char *const g_szsuf[3] = {".B", ".W", ".L"};

/* サイズ（バイト数）→ ビットマスク（内部cpu_internal.hのsize_mask()と同じ計算だが
 * 公開ヘッダのみに依存する本ファイル用に独立定義する） */
static uint32_t size_mask_u(int size) {
    return (size == 1) ? 0xFFU : (size == 2) ? 0xFFFFU : 0xFFFFFFFFU;
}

static uint32_t dis_addr(mc68k_state_t *cpu, uint32_t addr) {
    return mc68k_get_model(cpu) >= MC68K_MODEL_68020 ? addr : addr & 0xFFFFFFu;
}

/* ================================================================== */
/* 実効アドレス（EA）テキスト整形                                      */
/* ================================================================== */

/*
 * ea_helpers.c のブリーフ拡張ワード（mode 6 / mode 7 reg 3 共用）と
 * 同じフォーマットを解釈する。base は「間接先レジスタの値」ではなく
 * 表示用の記号（Xn.size(disp)）のみを組み立てる。
 */
static bool format_brief_ext(mc68k_state_t *cpu, uint32_t *cursor, const char *base_sym,
                             char *out, size_t outsz) {
    uint16_t ext;
    if (!dis_read_word(cpu, cursor, &ext))
        return false;
    int da = (ext >> 15) & 1;
    int ridx = (ext >> 12) & 7;
    int wl = (ext >> 11) & 1; /* 0=word, 1=long */
    bool is_68020 = mc68k_get_model(cpu) >= MC68K_MODEL_68020;
    unsigned scale = is_68020 ? 1u << ((ext >> 9) & 3) : 1;
    int8_t d8 = (int8_t)(ext & 0xFF);
    const char *xn = da ? g_areg[ridx] : g_dreg[ridx];
    if (!is_68020 || !(ext & 0x0100)) {
        if (strcmp(base_sym, "PC") == 0) {
            uint32_t base = *cursor - 2;
            uint32_t target = base + (uint32_t)(int32_t)d8;
            snprintf(out, outsz, "$%06X(PC,%s.%s%s)", dis_addr(cpu, target), xn, wl ? "L" : "W",
                     scale == 1   ? ""
                     : scale == 2 ? "*2"
                     : scale == 4 ? "*4"
                                  : "*8");
            return true;
        }
        snprintf(out, outsz, "%d(%s,%s.%s%s)", d8, base_sym, xn, wl ? "L" : "W",
                 scale == 1   ? ""
                 : scale == 2 ? "*2"
                 : scale == 4 ? "*4"
                              : "*8");
        return true;
    }
    bool bs = (ext & 0x80) != 0, is = (ext & 0x40) != 0;
    unsigned bd_size = (ext >> 4) & 3, iis = ext & 7;
    if (bd_size == 0 || iis == 4 || (is && iis >= 5)) {
        snprintf(out, outsz, "<reserved-indexed>");
        return true;
    }
    char bd[16] = "", od[16] = "";
    if (bd_size == 2) {
        uint16_t w;
        if (!dis_read_word(cpu, cursor, &w))
            return false;
        snprintf(bd, sizeof(bd), "%d,", (int)(int16_t)w);
    }
    if (bd_size == 3) {
        uint32_t l;
        if (!dis_read_long(cpu, cursor, &l))
            return false;
        snprintf(bd, sizeof(bd), "$%08X,", l);
    }
    unsigned od_size = iis & 3;
    if (iis && od_size == 2) {
        uint16_t w;
        if (!dis_read_word(cpu, cursor, &w))
            return false;
        snprintf(od, sizeof(od), ",%d", (int)(int16_t)w);
    }
    if (iis && od_size == 3) {
        uint32_t l;
        if (!dis_read_long(cpu, cursor, &l))
            return false;
        snprintf(od, sizeof(od), ",$%08X", l);
    }
    const char *b = bs ? (strcmp(base_sym, "PC") == 0 ? "ZPC" : "0") : base_sym;
    /* ix は先頭のカンマ込み（サプレス時は空）。od も先頭のカンマ込み。 */
    char ix[22] = "";
    if (!is)
        snprintf(ix, sizeof(ix), ",%s.%s%s", xn, wl ? "L" : "W",
                 scale == 1   ? ""
                 : scale == 2 ? "*2"
                 : scale == 4 ? "*4"
                              : "*8");
    if (iis == 0)
        snprintf(out, outsz, "(%s%s%s)", bd, b, ix);
    else if (iis < 5)
        snprintf(out, outsz, "([%s%s%s]%s)", bd, b, ix, od);
    else
        snprintf(out, outsz, "([%s%s]%s%s)", bd, b, ix, od);
    return true;
}

/*
 * EA の用途区分。mode7 の reg サブフィールドがどこまで有効かは、EA が
 * データ読み出し用（ea_read）/データ書き込み用（ea_write）/制御アドレス用
 * （ea_get_addr、LEA/PEA/JMP/JSR/MOVEM）かで異なる。実装（ea_helpers.c）と
 * 一致させないと、無効な組み合わせで余分な拡張ワードを消費したことにして
 * しまい命令長を誤る（EA_WRITE/EA_CONTROLでは即値・書き込み不可なモードは
 * 拡張ワードを一切消費せずフォールトする、というea_write/ea_get_addrの
 * 実際の挙動に合わせてtrueを返さずfalseにする＝呼び出し側がDC.Wへ切り替える）。
 */
typedef enum {
    EA_CTX_READ,    /* ea_read 相当: mode7 reg 0-4 すべて有効 */
    EA_CTX_WRITE,   /* ea_write 相当: mode7 は reg 0-1 のみ有効 */
    EA_CTX_CONTROL, /* ea_get_addr 相当: mode7 は reg 0-3 のみ有効（#imm不可） */
} ea_ctx_t;

/*
 * mode/reg から EA テキストを組み立てる。cursor は「次に読む拡張ワードの
 * アドレス」を指しており、EA が拡張ワードを消費するたびに進める。
 * 戻り値: 成功時 true。ctx で無効とされる mode7 の reg や非侵襲読み出し
 * 失敗時は false（呼び出し側はDC.Wフォールバックへ切り替える）。
 *
 * size は #imm の読み出し幅、および xxx.W(mode7 reg0)の符号拡張・
 * d16(PC)/d8(PC,Xn)の基準アドレス計算にのみ影響する
 * （メモリ間接モードのアドレス自体はsizeに依存しない）。
 */
static bool format_ea(mc68k_state_t *cpu, uint32_t *cursor, int mode, int reg, int size,
                      ea_ctx_t ctx, char *out, size_t outsz) {
    switch (mode) {
    case 0: /* Dn */
        snprintf(out, outsz, "%s", g_dreg[reg]);
        return true;
    case 1: /* An */
        snprintf(out, outsz, "%s", g_areg[reg]);
        return true;
    case 2: /* (An) */
        snprintf(out, outsz, "(%s)", g_areg[reg]);
        return true;
    case 3: /* (An)+ */
        snprintf(out, outsz, "(%s)+", g_areg[reg]);
        return true;
    case 4: /* -(An) */
        snprintf(out, outsz, "-(%s)", g_areg[reg]);
        return true;
    case 5: { /* d16(An) */
        uint16_t d16;
        if (!dis_read_word(cpu, cursor, &d16))
            return false;
        snprintf(out, outsz, "%d(%s)", (int)(int16_t)d16, g_areg[reg]);
        return true;
    }
    case 6: /* d8(An,Xn) */
        return format_brief_ext(cpu, cursor, g_areg[reg], out, outsz);
    case 7:
        switch (reg) {
        case 0: { /* xxx.W: 16bit 符号拡張。ea_helpers.c と同じ計算式 */
            uint16_t w;
            if (!dis_read_word(cpu, cursor, &w))
                return false;
            uint32_t addr = (uint32_t)(int32_t)(int16_t)w;
            snprintf(out, outsz, "$%08X.W", addr);
            return true;
        }
        case 1: { /* xxx.L */
            uint32_t addr;
            if (!dis_read_long(cpu, cursor, &addr))
                return false;
            snprintf(out, outsz, "$%08X.L", addr);
            return true;
        }
        case 2: { /* d16(PC): 基準は拡張ワード自身のアドレス（ea_helpers.c準拠） */
            if (ctx == EA_CTX_WRITE)
                return false; /* ea_write は mode7 reg2/3/4 を扱わない（無消費でフォールト） */
            uint32_t base = *cursor;
            uint16_t d16;
            if (!dis_read_word(cpu, cursor, &d16))
                return false;
            uint32_t target = base + (uint32_t)(int32_t)(int16_t)d16;
            bool is_68020 = mc68k_get_model(cpu) >= MC68K_MODEL_68020;
            snprintf(out, outsz, is_68020 ? "$%08X(PC)" : "$%06X(PC)", dis_addr(cpu, target));
            return true;
        }
        case 3: { /* d8(PC,Xn): 基準は拡張ワード自身のアドレス */
            if (ctx == EA_CTX_WRITE)
                return false;
            return format_brief_ext(cpu, cursor, "PC", out, outsz);
        }
        case 4: /* #imm */
            if (ctx != EA_CTX_READ)
                return false; /* #imm は書き込み先にも制御アドレスにもなれない */
            if (size == 4) {
                uint32_t v;
                if (!dis_read_long(cpu, cursor, &v))
                    return false;
                snprintf(out, outsz, "#$%08X", v);
            } else if (size == 2) {
                uint16_t v;
                if (!dis_read_word(cpu, cursor, &v))
                    return false;
                snprintf(out, outsz, "#$%04X", v);
            } else {
                /* byte immediate も1ワード消費、下位バイトのみ有効
                 * (ea_read mode7/reg4と同じ) */
                uint16_t v;
                if (!dis_read_word(cpu, cursor, &v))
                    return false;
                snprintf(out, outsz, "#$%02X", v & 0xFF);
            }
            return true;
        default: /* reg 5,6,7: 未割り当て */
            return false;
        }
    default:
        return false;
    }
}

/* ================================================================== */
/* 結果バッファ                                                        */
/* ================================================================== */

typedef struct {
    char mnemonic[16];
    char operands[80];
} dis_result_t;

static void set_noop(dis_result_t *r, const char *mnemonic) {
    snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", mnemonic);
    r->operands[0] = '\0';
}

/* ================================================================== */
/* ライン 0: ORI/ANDI/EORI/ADDI/SUBI/CMPI, BTST/BCHG/BCLR/BSET, MOVEP  */
/* ================================================================== */

/*
 * ORI/ANDI/EORI と静的ビット演算・ADDI/SUBI/CMPI はすべて opcode の bit8=0。
 * bit8=1 は動的ビット演算（Dn 経由）または MOVEP。
 * bits11-8（4bitニブル）で ORI(0)/ANDI(2)/SUBI(4)/ADDI(6)/静的ビット演算(8)/
 * EORI(A)/CMPI(C) を判別する（実装のレジスタ範囲設定と一致、他の値は未使用）。
 */
static bool dis_line0(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opcode == 0x0CFC || opcode == 0x0EFC)) {
        uint16_t a, b;
        if (!dis_read_word(cpu, cursor, &a) || !dis_read_word(cpu, cursor, &b))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "CAS2.%c", opcode == 0x0CFC ? 'W' : 'L');
        /* Dc1:Dc2,Du1:Du2,(Rn1):(Rn2) — op_cas2() の dc1/dc2/du1/du2 と同じ抽出順
         * （: 以前は D%dc1:D%du1,D%dc2:D%du2 と誤って組んでいた）。 */
        snprintf(r->operands, sizeof(r->operands), "D%d:D%d,D%d:D%d,(%c%d):(%c%d)", a & 7,
                 b & 7, (a >> 6) & 7, (b >> 6) & 7, (a & 0x8000) ? 'A' : 'D', (a >> 12) & 7,
                 (b & 0x8000) ? 'A' : 'D', (b >> 12) & 7);
        return true;
    }
    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opcode & 0xF9C0) == 0x08C0 &&
        ((opcode >> 9) & 3) != 0) {
        int sc = (opcode >> 9) & 3;
        uint16_t ext;
        char ea[40];
        if (!dis_read_word(cpu, cursor, &ext) ||
            !format_ea(cpu, cursor, ea_mode, ea_reg, g_sz[sc - 1], EA_CTX_WRITE, ea,
                       sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "CAS%s", g_szsuf[sc - 1]);
        snprintf(r->operands, sizeof(r->operands), "D%d,D%d,%s", ext & 7, (ext >> 6) & 7, ea);
        return true;
    }
    /* CMP2/CHK2: bit11=0, bits8-6=011（size は bits10-9）。旧マスク 0xF8C0 は
     * bit8 を判定に含めておらず、bit8=1 の MOVEPを誤って
     * CMP2/CHK2 と判定していた。 */
    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opcode & 0xF800) == 0x0000 &&
        ((opcode >> 6) & 7) == 3) {
        int sc = (opcode >> 9) & 3, sz = sc == 0 ? 1 : sc == 1 ? 2 : 4;
        uint16_t ext;
        char ea[40];
        if (sc == 3 || !dis_read_word(cpu, cursor, &ext) ||
            !format_ea(cpu, cursor, ea_mode, ea_reg, sz, EA_CTX_CONTROL, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s%s", (ext & 0x0800) ? "CHK2" : "CMP2",
                 g_szsuf[sc]);
        snprintf(r->operands, sizeof(r->operands), "%s,%c%d", ea, (ext & 0x8000) ? 'A' : 'D',
                 (ext >> 12) & 7);
        return true;
    }

    if (opcode & 0x0100) {
        /* bit8=1: 動的ビット演算 or MOVEP */
        if (ea_mode == 1) {
            /* MOVEP: opmode(bits8-6, 4-7) → W-read/L-read/W-write/L-write */
            int dn = (opcode >> 9) & 7;
            int an = ea_reg;
            int opmode = (opcode >> 6) & 7;
            uint16_t d16;
            if (!dis_read_word(cpu, cursor, &d16))
                return false;
            static const char *const mnem[4] = {"MOVEP.W", "MOVEP.L", "MOVEP.W", "MOVEP.L"};
            snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", mnem[opmode - 4]);
            if (opmode <= 5)
                snprintf(r->operands, sizeof(r->operands), "%d(%s),%s", (int)(int16_t)d16,
                         g_areg[an], g_dreg[dn]);
            else
                snprintf(r->operands, sizeof(r->operands), "%s,%d(%s)", g_dreg[dn],
                         (int)(int16_t)d16, g_areg[an]);
            return true;
        }
        static const char *const bitop[4] = {"BTST", "BCHG", "BCLR", "BSET"};
        int sel = (opcode >> 6) & 3;
        int dn = (opcode >> 9) & 7;
        char ea[40];
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 1, EA_CTX_READ, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", bitop[sel]);
        snprintf(r->operands, sizeof(r->operands), "%s,%s", g_dreg[dn], ea);
        return true;
    }

    int nibble = (opcode >> 8) & 0xF;

    if (nibble == 0xE) {
        /* MOVES.x EA,Rn / Rn,EA（MC68010+、0x0E00-0x0EFF）:
         * size(bit7-6)=00/01/10、EA(bit5-0)=Memory Alterable。
         * 拡張ワード（レジスタ指定）は EA の拡張ワードより先にフェッチされる
         * （ops_system_ext.c op_moves() の実装順と一致させる）。 */
        int size_code = (opcode >> 6) & 3;
        if (size_code == 3)
            return false;
        /* Memory Alterable: Dn/An直接（mode0/1）は無効
         * （ea_is_memory_alterable()と同じ判定。mode7のreg0/1限定はformat_ea()の
         * EA_CTX_WRITEが処理する）。 */
        if (ea_mode == 0 || ea_mode == 1)
            return false;
        int size = g_sz[size_code];
        uint16_t ext;
        if (!dis_read_word(cpu, cursor, &ext))
            return false;
        int is_an = (ext >> 15) & 1;
        int reg = (ext >> 12) & 7;
        int dr = (ext >> 11) & 1; /* 0: メモリ→レジスタ, 1: レジスタ→メモリ */
        const char *rn = is_an ? g_areg[reg] : g_dreg[reg];
        char ea[40];
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, size, EA_CTX_WRITE, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVES%s", g_szsuf[size_code]);
        if (dr == 0)
            snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, rn);
        else
            snprintf(r->operands, sizeof(r->operands), "%s,%s", rn, ea);
        return true;
    }

    if (nibble == 8) {
        /* 静的ビット演算: 拡張ワード（ビット番号）が EA 拡張より先 */
        static const char *const bitop[4] = {"BTST", "BCHG", "BCLR", "BSET"};
        int sel = (opcode >> 6) & 3;
        uint16_t bitno;
        if (!dis_read_word(cpu, cursor, &bitno))
            return false;
        char ea[40];
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 1, EA_CTX_READ, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", bitop[sel]);
        snprintf(r->operands, sizeof(r->operands), "#$%02X,%s", bitno & 0xFF, ea);
        return true;
    }

    /* ORI/ANDI/EORI/SUBI/ADDI/CMPI 共通: bits7-6 がサイズ（3は未割り当て） */
    int sc = (opcode >> 6) & 3;
    if (sc == 3)
        return false;

    const char *base_name;
    switch (nibble) {
    case 0:
        base_name = "ORI";
        break;
    case 2:
        base_name = "ANDI";
        break;
    case 4:
        base_name = "SUBI";
        break;
    case 6:
        base_name = "ADDI";
        break;
    case 0xA:
        base_name = "EORI";
        break;
    case 0xC:
        base_name = "CMPI";
        break;
    default:
        return false; /* 未割り当てニブル */
    }

    /* ORI/ANDI/EORI の #imm,CCR / #imm,SR 特殊形式（SUBI/ADDI/CMPIには存在しない） */
    if ((nibble == 0 || nibble == 2 || nibble == 0xA) && ea_mode == 7 && ea_reg == 4) {
        uint16_t imm;
        if (!dis_read_word(cpu, cursor, &imm))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", base_name);
        if (sc == 0)
            snprintf(r->operands, sizeof(r->operands), "#$%02X,CCR", imm & 0xFF);
        else
            snprintf(r->operands, sizeof(r->operands), "#$%04X,SR", imm);
        return true;
    }

    /* 即値は EA 拡張ワードより先にフェッチされる（ops_alu.c fetch_immediate順） */
    int size = g_sz[sc];
    char imm_buf[16];
    if (size == 4) {
        uint32_t v;
        if (!dis_read_long(cpu, cursor, &v))
            return false;
        snprintf(imm_buf, sizeof(imm_buf), "#$%08X", v);
    } else {
        uint16_t v;
        if (!dis_read_word(cpu, cursor, &v))
            return false;
        snprintf(imm_buf, sizeof(imm_buf), (size == 1) ? "#$%02X" : "#$%04X",
                 v & size_mask_u(size));
    }
    char ea[40];
    if (!format_ea(cpu, cursor, ea_mode, ea_reg, size, EA_CTX_READ, ea, sizeof(ea)))
        return false;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "%s%s", base_name, g_szsuf[sc]);
    snprintf(r->operands, sizeof(r->operands), "%s,%s", imm_buf, ea);
    return true;
}

/* ================================================================== */
/* ライン 1-3: MOVE.B/MOVE.L/MOVE.W, MOVEA                             */
/* ================================================================== */

static int size_to_sc(int size) {
    return (size == 1) ? 0 : (size == 2) ? 1 : 2;
}

/*
 * MOVE の宛先 EA は opcode 内でフィールド順序が逆（ops_move.c 冒頭コメント
 * 参照）: ソース bits5-3=mode,2-0=reg（自然な順）、宛先 bits11-9=reg,8-6=mode
 * （逆）。ソースEA拡張ワードの後に宛先EA拡張ワードを読む順序（do_move()の
 * ea_read→ea_write順）を維持する。
 */
static bool dis_move(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, int size,
                     dis_result_t *r) {
    int src_mode = (opcode >> 3) & 7;
    int src_reg = opcode & 7;
    int dst_reg = (opcode >> 9) & 7;
    int dst_mode = (opcode >> 6) & 7;

    char src[40];
    if (!format_ea(cpu, cursor, src_mode, src_reg, size, EA_CTX_READ, src, sizeof(src)))
        return false;

    if (dst_mode == 1) {
        snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVEA%s", g_szsuf[size_to_sc(size)]);
        snprintf(r->operands, sizeof(r->operands), "%s,%s", src, g_areg[dst_reg]);
        return true;
    }
    char dst[40];
    if (!format_ea(cpu, cursor, dst_mode, dst_reg, size, EA_CTX_WRITE, dst, sizeof(dst)))
        return false;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVE%s", g_szsuf[size_to_sc(size)]);
    snprintf(r->operands, sizeof(r->operands), "%s,%s", src, dst);
    return true;
}

/* ================================================================== */
/* MOVEM レジスタリスト整形                                            */
/* ================================================================== */

/*
 * MOVEM の16bitマスクを "D0-D7/A0-A3" のように連続範囲へ圧縮して整形する。
 * reversed_bits: -(An) の reg→メモリ方向のみ true
 * （ops_transfer.c 冒頭コメント参照: bit0=A7,...,bit15=D0 の逆順）。
 * どちらの符号化でも、表示は常にD0..D7,A0..A7の昇順（実アセンブラの慣習）。
 */
static void format_reg_list(uint16_t mask, bool reversed_bits, char *out, size_t outsz) {
    bool present[16] = {false};
    for (int bit = 0; bit < 16; bit++) {
        if (mask & (1u << bit)) {
            int reg = reversed_bits ? (15 - bit) : bit;
            present[reg] = true;
        }
    }
    out[0] = '\0';
    size_t pos = 0;
    bool first = true;
    int i = 0;
    while (i < 16) {
        if (!present[i]) {
            i++;
            continue;
        }
        int start = i;
        int end = i;
        while (end + 1 < 16 && present[end + 1])
            end++;
        const char *name_lo = (start < 8) ? g_dreg[start] : g_areg[start - 8];
        const char *name_hi = (end < 8) ? g_dreg[end] : g_areg[end - 8];
        int n;
        if (start == end)
            n = snprintf(out + pos, outsz - pos, "%s%s", first ? "" : "/", name_lo);
        else
            n = snprintf(out + pos, outsz - pos, "%s%s-%s", first ? "" : "/", name_lo, name_hi);
        if (n > 0)
            pos += (size_t)n;
        first = false;
        i = end + 1;
    }
}

/* ================================================================== */
/* ライン 4: 雑多命令群                                                 */
/* ================================================================== */

static bool dis_movem(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int dir = (opcode >> 10) & 1; /* 0=reg→mem, 1=mem→reg */
    int sz = (opcode >> 6) & 1;   /* 0=word, 1=long */
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    if (ea_mode == 0 || ea_mode == 1)
        return false; /* EXTまたは未割り当て（呼び出し側で処理済みのはず） */

    uint16_t mask;
    if (!dis_read_word(cpu, cursor, &mask))
        return false;
    bool reversed = (!dir && ea_mode == 4);
    char reglist[80];
    format_reg_list(mask, reversed, reglist, sizeof(reglist));

    char ea[40];
    if (!format_ea(cpu, cursor, ea_mode, ea_reg, sz ? 4 : 2, EA_CTX_CONTROL, ea, sizeof(ea)))
        return false;

    snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVEM.%s", sz ? "L" : "W");
    if (dir)
        snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, reglist);
    else
        snprintf(r->operands, sizeof(r->operands), "%s,%s", reglist, ea);
    return true;
}

static bool dis_line4(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    char ea[40];

    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opcode & 0xFFF8) == 0x49C0) {
        snprintf(r->mnemonic, sizeof(r->mnemonic), "EXTB.L");
        snprintf(r->operands, sizeof(r->operands), "%s", g_dreg[opcode & 7]);
        return true;
    }
    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opcode & 0xFFF8) == 0x4808) {
        uint32_t disp;
        if (!dis_read_long(cpu, cursor, &disp))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "LINK.L");
        snprintf(r->operands, sizeof(r->operands), "%s,#%d", g_areg[opcode & 7], (int32_t)disp);
        return true;
    }
    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opcode & 0xFFF8) == 0x4848) {
        snprintf(r->mnemonic, sizeof(r->mnemonic), "BKPT");
        snprintf(r->operands, sizeof(r->operands), "#%d", opcode & 7);
        return true;
    }
    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opcode & 0xF1C0) == 0x4100) {
        char ea[40];
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 4, EA_CTX_READ, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "CHK.L");
        snprintf(r->operands, sizeof(r->operands), "%s,D%d", ea, (opcode >> 9) & 7);
        return true;
    }
    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 &&
        ((opcode & 0xFFC0) == 0x4C00 || (opcode & 0xFFC0) == 0x4C40)) {
        uint16_t ext;
        char ea[40];
        bool div = (opcode & 0x40) != 0;
        if (!dis_read_word(cpu, cursor, &ext) ||
            !format_ea(cpu, cursor, ea_mode, ea_reg, 4, EA_CTX_READ, ea, sizeof(ea)))
            return false;
        int q = (ext >> 12) & 7, h = ext & 7;
        bool sign = (ext & 0x0800) != 0, wide = (ext & 0x0400) != 0;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s.L",
                 div ? (sign ? "DIVS" : "DIVU") : (sign ? "MULS" : "MULU"));
        if (wide)
            snprintf(r->operands, sizeof(r->operands), "%s,%s:%s", ea, g_dreg[h], g_dreg[q]);
        else
            snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, g_dreg[q]);
        return true;
    }

    /* ---- 固定オペコード（ea非依存の単発命令）---- */
    switch (opcode) {
    case 0x4E71:
        set_noop(r, "NOP");
        return true;
    case 0x4E70:
        set_noop(r, "RESET");
        return true;
    case 0x4E72: { /* STOP #imm */
        uint16_t imm;
        if (!dis_read_word(cpu, cursor, &imm))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "STOP");
        snprintf(r->operands, sizeof(r->operands), "#$%04X", imm);
        return true;
    }
    case 0x4E73:
        set_noop(r, "RTE");
        return true;
    case 0x4E74: { /* RTD #d（MC68010+） */
        uint16_t d16;
        if (!dis_read_word(cpu, cursor, &d16))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "RTD");
        snprintf(r->operands, sizeof(r->operands), "#%d", (int)(int16_t)d16);
        return true;
    }
    case 0x4E75:
        set_noop(r, "RTS");
        return true;
    case 0x4E76:
        set_noop(r, "TRAPV");
        return true;
    case 0x4E77:
        set_noop(r, "RTR");
        return true;
    case 0x4E7A:   /* MOVEC Rc,Rn（MC68010+） */
    case 0x4E7B: { /* MOVEC Rn,Rc */
        uint16_t ext;
        if (!dis_read_word(cpu, cursor, &ext))
            return false;
        int is_an = (ext >> 15) & 1;
        int reg = (ext >> 12) & 7;
        uint16_t creg = ext & 0x0FFF;
        const char *rn = is_an ? g_areg[reg] : g_dreg[reg];
        char creg_buf[8];
        const char *creg_name;
        switch (creg) {
        case 0x000:
            creg_name = "SFC";
            break;
        case 0x001:
            creg_name = "DFC";
            break;
        case 0x002:
            creg_name = "CACR";
            break;
        case 0x800:
            creg_name = "USP";
            break;
        case 0x801:
            creg_name = "VBR";
            break;
        case 0x802:
            creg_name = "CAAR";
            break;
        case 0x803:
            creg_name = "MSP";
            break;
        case 0x804:
            creg_name = "ISP";
            break;
        default:
            /* 未定義の制御レジスタ番号。実行時はILLEGALだが、逆アセンブラは
             * コード意図を読む道具として生値を表示する（デコードの正当性を
             * 判定する役割ではない）。 */
            snprintf(creg_buf, sizeof(creg_buf), "$%03X", creg);
            creg_name = creg_buf;
            break;
        }
        snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVEC");
        if (opcode == 0x4E7A)
            snprintf(r->operands, sizeof(r->operands), "%s,%s", creg_name, rn);
        else
            snprintf(r->operands, sizeof(r->operands), "%s,%s", rn, creg_name);
        return true;
    }
    default:
        break;
    }

    /* ---- TRAP #n: 0x4E40-0x4E4F ---- */
    if ((opcode & 0xFFF0) == 0x4E40) {
        snprintf(r->mnemonic, sizeof(r->mnemonic), "TRAP");
        snprintf(r->operands, sizeof(r->operands), "#%d", opcode & 0xF);
        return true;
    }

    /* ---- LINK / UNLK: 0x4E50-0x4E5F ---- */
    if ((opcode & 0xFFF8) == 0x4E50) { /* LINK An,#disp */
        int an = opcode & 7;
        uint16_t d16;
        if (!dis_read_word(cpu, cursor, &d16))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "LINK");
        snprintf(r->operands, sizeof(r->operands), "%s,#%d", g_areg[an], (int)(int16_t)d16);
        return true;
    }
    if ((opcode & 0xFFF8) == 0x4E58) { /* UNLK An */
        snprintf(r->mnemonic, sizeof(r->mnemonic), "UNLK");
        snprintf(r->operands, sizeof(r->operands), "%s", g_areg[opcode & 7]);
        return true;
    }

    /* ---- MOVE USP: 0x4E60-0x4E6F ---- */
    if ((opcode & 0xFFF0) == 0x4E60) {
        int an = opcode & 7;
        int dir = (opcode >> 3) & 1; /* 0=An→USP, 1=USP→An */
        snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVE");
        if (dir == 0)
            snprintf(r->operands, sizeof(r->operands), "%s,USP", g_areg[an]);
        else
            snprintf(r->operands, sizeof(r->operands), "USP,%s", g_areg[an]);
        return true;
    }

    /* ---- JSR / JMP: 0x4E80-0x4EFF（制御EA） ---- */
    if (opcode >= 0x4E80 && opcode <= 0x4EFF) {
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 0, EA_CTX_CONTROL, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", (opcode <= 0x4EBF) ? "JSR" : "JMP");
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* ---- NEGX (0x4000-0x40BF) / MOVE SR,EA (0x40C0-0x40FF) ---- */
    if (opcode >= 0x4000 && opcode <= 0x40FF) {
        if (opcode >= 0x40C0) {
            if (!format_ea(cpu, cursor, ea_mode, ea_reg, 2, EA_CTX_WRITE, ea, sizeof(ea)))
                return false;
            snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVE");
            snprintf(r->operands, sizeof(r->operands), "SR,%s", ea);
            return true;
        }
        int sc = (opcode >> 6) & 3;
        if (sc == 3)
            return false;
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, g_sz[sc], EA_CTX_WRITE, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "NEGX%s", g_szsuf[sc]);
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* ---- CHK (bits8-6=110) / LEA (bits8-6=111): dn任意、baseは0x4180+dn*0x200 ---- */
    if ((opcode & 0xF1C0) == 0x4180) { /* CHK.W EA,Dn */
        int dn = (opcode >> 9) & 7;
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 2, EA_CTX_READ, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "CHK");
        snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, g_dreg[dn]);
        return true;
    }
    if ((opcode & 0xF1C0) == 0x41C0) { /* LEA EA,An */
        int an = (opcode >> 9) & 7;
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 0, EA_CTX_CONTROL, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "LEA");
        snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, g_areg[an]);
        return true;
    }

    /* ---- CLR (0x4200-0x42BF) / MOVE CCR,EA (0x42C0-0x42FF) ---- */
    if (opcode >= 0x4200 && opcode <= 0x42FF) {
        if (opcode >= 0x42C0) {
            if (!format_ea(cpu, cursor, ea_mode, ea_reg, 2, EA_CTX_WRITE, ea, sizeof(ea)))
                return false;
            snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVE");
            snprintf(r->operands, sizeof(r->operands), "CCR,%s", ea);
            return true;
        }
        int sc = (opcode >> 6) & 3;
        if (sc == 3)
            return false;
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, g_sz[sc], EA_CTX_WRITE, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "CLR%s", g_szsuf[sc]);
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* ---- NEG (0x4400-0x44BF) / MOVE EA,CCR (0x44C0-0x44FF) ---- */
    if (opcode >= 0x4400 && opcode <= 0x44FF) {
        if (opcode >= 0x44C0) {
            if (!format_ea(cpu, cursor, ea_mode, ea_reg, 2, EA_CTX_READ, ea, sizeof(ea)))
                return false;
            snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVE");
            snprintf(r->operands, sizeof(r->operands), "%s,CCR", ea);
            return true;
        }
        int sc = (opcode >> 6) & 3;
        if (sc == 3)
            return false;
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, g_sz[sc], EA_CTX_WRITE, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "NEG%s", g_szsuf[sc]);
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* ---- NOT (0x4600-0x46BF) / MOVE EA,SR (0x46C0-0x46FF) ---- */
    if (opcode >= 0x4600 && opcode <= 0x46FF) {
        if (opcode >= 0x46C0) {
            if (!format_ea(cpu, cursor, ea_mode, ea_reg, 2, EA_CTX_READ, ea, sizeof(ea)))
                return false;
            snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVE");
            snprintf(r->operands, sizeof(r->operands), "%s,SR", ea);
            return true;
        }
        int sc = (opcode >> 6) & 3;
        if (sc == 3)
            return false;
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, g_sz[sc], EA_CTX_WRITE, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "NOT%s", g_szsuf[sc]);
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* ---- NBCD (0x4800-0x483F) ---- */
    if (opcode >= 0x4800 && opcode <= 0x483F) {
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 1, EA_CTX_READ, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "NBCD");
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* ---- SWAP (mode0) / PEA (mode2,5,6,7): 0x4840-0x487F ---- */
    if (opcode >= 0x4840 && opcode <= 0x487F) {
        if (ea_mode == 0) {
            snprintf(r->mnemonic, sizeof(r->mnemonic), "SWAP");
            snprintf(r->operands, sizeof(r->operands), "%s", g_dreg[ea_reg]);
            return true;
        }
        if (ea_mode == 1 || ea_mode == 3 || ea_mode == 4)
            return false; /* 未割り当て */
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 0, EA_CTX_CONTROL, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "PEA");
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* ---- EXT (mode0) / MOVEM reg→mem (mode2-7): 0x4880-0x48FF ---- */
    if (opcode >= 0x4880 && opcode <= 0x48FF) {
        if (ea_mode == 0) {
            int sz = (opcode >> 6) & 1; /* 0=W(byte→word), 1=L(word→long) */
            snprintf(r->mnemonic, sizeof(r->mnemonic), "EXT.%s", sz ? "L" : "W");
            snprintf(r->operands, sizeof(r->operands), "%s", g_dreg[ea_reg]);
            return true;
        }
        return dis_movem(cpu, opcode, cursor, r);
    }

    /* ---- TST (0x4A00-0x4ABF) / TAS (0x4AC0-0x4AFF, 0x4AFCはILLEGAL) ---- */
    if (opcode >= 0x4A00 && opcode <= 0x4AFF) {
        if (opcode >= 0x4AC0) {
            if (opcode == 0x4AFC)
                return false; /* ILLEGAL命令として予約 */
            if (!format_ea(cpu, cursor, ea_mode, ea_reg, 1, EA_CTX_READ, ea, sizeof(ea)))
                return false;
            snprintf(r->mnemonic, sizeof(r->mnemonic), "TAS");
            snprintf(r->operands, sizeof(r->operands), "%s", ea);
            return true;
        }
        int sc = (opcode >> 6) & 3;
        if (sc == 3)
            return false;
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, g_sz[sc], EA_CTX_READ, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "TST%s", g_szsuf[sc]);
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* ---- MOVEM mem→reg (mode2-7): 0x4C80-0x4CFF ---- */
    if (opcode >= 0x4C80 && opcode <= 0x4CFF)
        return dis_movem(cpu, opcode, cursor, r);

    return false; /* 未割り当て（ILLEGAL） */
}

/* ================================================================== */
/* ライン 5: ADDQ/SUBQ, Scc/DBcc                                       */
/* ================================================================== */

/* cond(0-15) → ニーモニックサフィックス。T/F は Scc(ST/SF)・DBcc(DBT/DBF)
 * 共通だが、Bcc は cond0/1 が BRA/BSR に特殊化されるため別テーブルを使う。 */
static const char *const g_cond_suffix[16] = {"T",  "F",  "HI", "LS", "CC", "CS", "NE", "EQ",
                                              "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE"};

static bool dis_line5(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int sc = (opcode >> 6) & 3;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opcode & 0x00F8) == 0x00F8) {
        int cond = (opcode >> 8) & 0xF;
        int form = opcode & 7;
        if (form == 2) {
            uint16_t ignored;
            if (!dis_read_word(cpu, cursor, &ignored))
                return false;
        } else if (form == 3) {
            uint32_t ignored;
            if (!dis_read_long(cpu, cursor, &ignored))
                return false;
        } else if (form != 4)
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "TRAP%s", g_cond_suffix[cond]);
        r->operands[0] = '\0';
        return true;
    }

    if (sc == 3) {
        /* Scc / DBcc */
        int cond = (opcode >> 8) & 0xF;
        if (ea_mode == 1) {
            /* DBcc Dn,label */
            uint32_t base = *cursor; /* opcode直後（disp2バイト分の基点） */
            uint16_t d16;
            if (!dis_read_word(cpu, cursor, &d16))
                return false;
            uint32_t target = base + (uint32_t)(int32_t)(int16_t)d16;
            if (cond == 1)
                snprintf(r->mnemonic, sizeof(r->mnemonic), "DBRA");
            else
                snprintf(r->mnemonic, sizeof(r->mnemonic), "DB%s", g_cond_suffix[cond]);
            snprintf(r->operands, sizeof(r->operands), "%s,$%06X", g_dreg[ea_reg],
                     dis_addr(cpu, target));
            return true;
        }
        char ea[40];
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 1, EA_CTX_WRITE, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "S%s", g_cond_suffix[cond]);
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* ADDQ / SUBQ */
    int data = (opcode >> 9) & 7;
    if (data == 0)
        data = 8;
    bool is_sub = (opcode >> 8) & 1;
    char ea[40];
    if (!format_ea(cpu, cursor, ea_mode, ea_reg, g_sz[sc], EA_CTX_READ, ea, sizeof(ea)))
        return false;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "%s%s", is_sub ? "SUBQ" : "ADDQ", g_szsuf[sc]);
    snprintf(r->operands, sizeof(r->operands), "#%d,%s", data, ea);
    return true;
}

/* ================================================================== */
/* ライン 6: BRA/BSR/Bcc                                               */
/* ================================================================== */

static const char *const g_bcc_mnemonics[16] = {"BRA", "BSR", "BHI", "BLS", "BCC", "BCS",
                                                "BNE", "BEQ", "BVC", "BVS", "BPL", "BMI",
                                                "BGE", "BLT", "BGT", "BLE"};

static bool dis_line6(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int cond = (opcode >> 8) & 0xF;
    int8_t d8 = (int8_t)(opcode & 0xFF);
    uint32_t base = *cursor; /* opcode直後 = ディスプレースメント加算の基点 */
    uint32_t target;

    if (d8 == 0) {
        uint16_t d16;
        if (!dis_read_word(cpu, cursor, &d16))
            return false;
        target = base + (uint32_t)(int32_t)(int16_t)d16;
    } else if ((uint8_t)d8 == 0xFF && mc68k_get_model(cpu) >= MC68K_MODEL_68020) {
        uint32_t d32;
        if (!dis_read_long(cpu, cursor, &d32))
            return false;
        target = base + d32;
    } else {
        target = base + (uint32_t)(int32_t)d8;
    }
    snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", g_bcc_mnemonics[cond]);
    bool is_68020 = mc68k_get_model(cpu) >= MC68K_MODEL_68020;
    snprintf(r->operands, sizeof(r->operands), is_68020 ? "$%08X" : "$%06X",
             dis_addr(cpu, target));
    return true;
}

/* ================================================================== */
/* ライン 7: MOVEQ                                                     */
/* ================================================================== */

static bool dis_line7(uint16_t opcode, dis_result_t *r) {
    if (opcode & 0x0100)
        return false; /* bit8=1 は未割り当て */
    int dn = (opcode >> 9) & 7;
    int8_t data = (int8_t)(opcode & 0xFF);
    snprintf(r->mnemonic, sizeof(r->mnemonic), "MOVEQ");
    snprintf(r->operands, sizeof(r->operands), "#%d,%s", data, g_dreg[dn]);
    return true;
}

/* ================================================================== */
/* ライン 8/9/B/C/D 共通ヘルパー: EA,Dn / Dn,EA / xxxA / MUL/DIV / Xフォーム */
/* ================================================================== */

/* EA op Dn（opmode0-2）。sc(0-2)がそのままサイズコード。 */
static bool dis_ea_to_dn(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, int sc,
                         const char *base, dis_result_t *r) {
    int dn = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    char ea[40];
    if (!format_ea(cpu, cursor, ea_mode, ea_reg, g_sz[sc], EA_CTX_READ, ea, sizeof(ea)))
        return false;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "%s%s", base, g_szsuf[sc]);
    snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, g_dreg[dn]);
    return true;
}

/* Dn op EA（opmode4-6、RMW）。ea_rmw_readのmode7範囲はea_readと同じ。 */
static bool dis_dn_to_ea(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, int sc,
                         const char *base, dis_result_t *r) {
    int dn = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    char ea[40];
    if (!format_ea(cpu, cursor, ea_mode, ea_reg, g_sz[sc], EA_CTX_READ, ea, sizeof(ea)))
        return false;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "%s%s", base, g_szsuf[sc]);
    snprintf(r->operands, sizeof(r->operands), "%s,%s", g_dreg[dn], ea);
    return true;
}

/* ADDA/SUBA/CMPA（opmode3=word,7=long。EA,An） */
static bool dis_xxxa(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, const char *base,
                     int opmode, dis_result_t *r) {
    int an = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    int size = (opmode == 3) ? 2 : 4;
    char ea[40];
    if (!format_ea(cpu, cursor, ea_mode, ea_reg, size, EA_CTX_READ, ea, sizeof(ea)))
        return false;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "%sA%s", base, (size == 2) ? ".W" : ".L");
    snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, g_areg[an]);
    return true;
}

/* MULU/MULS/DIVU/DIVS（Dn op EA、ワード固定） */
static bool dis_muldiv(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, const char *mnem,
                       dis_result_t *r) {
    int dn = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    char ea[40];
    if (!format_ea(cpu, cursor, ea_mode, ea_reg, 2, EA_CTX_READ, ea, sizeof(ea)))
        return false;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "%s.W", mnem);
    snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, g_dreg[dn]);
    return true;
}

/*
 * ADDX/SUBX/ABCD/SBCD 共通形式（Rx,Ry、bit3=Mでレジスタ/プレデクリメント
 * 形式を切り替え）。has_size=false なら ABCD/SBCD（常にバイト、サフィックス
 * 表示なし）。
 */
static bool dis_x_form(uint16_t opcode, const char *mnem, bool has_size, char szch,
                       dis_result_t *r) {
    int rx = (opcode >> 9) & 7;
    int m = (opcode >> 3) & 1;
    int ry = opcode & 7;
    if (has_size)
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s.%c", mnem, szch);
    else
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", mnem);
    if (!m)
        snprintf(r->operands, sizeof(r->operands), "%s,%s", g_dreg[ry], g_dreg[rx]);
    else
        snprintf(r->operands, sizeof(r->operands), "-(%s),-(%s)", g_areg[ry], g_areg[rx]);
    return true;
}

static bool dis_cmpm(uint16_t opcode, dis_result_t *r) {
    int ax = (opcode >> 9) & 7;
    int sz = (opcode >> 6) & 3;
    int ay = opcode & 7;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "CMPM%s", g_szsuf[sz]);
    snprintf(r->operands, sizeof(r->operands), "(%s)+,(%s)+", g_areg[ay], g_areg[ax]);
    return true;
}

static bool dis_exg(uint16_t opcode, dis_result_t *r) {
    int rx = (opcode >> 9) & 7;
    int opmode5 = (opcode >> 3) & 0x1F;
    int ry = opcode & 7;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "EXG");
    switch (opmode5) {
    case 8:
        snprintf(r->operands, sizeof(r->operands), "%s,%s", g_dreg[rx], g_dreg[ry]);
        return true;
    case 9:
        snprintf(r->operands, sizeof(r->operands), "%s,%s", g_areg[rx], g_areg[ry]);
        return true;
    case 17:
        snprintf(r->operands, sizeof(r->operands), "%s,%s", g_dreg[rx], g_areg[ry]);
        return true;
    default:
        return false;
    }
}

/* ================================================================== */
/* ライン 8: OR / DIVU / DIVS / SBCD                                   */
/* ================================================================== */

static bool dis_line8(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    /* PACK は opmode(bits8-6)==5、UNPK は opmode==6（ops_68020_arith.c の
     * 0x8140/0x8180 系表登録と一致）。旧マスク 0xF1B0 は bit7 を PACK/UNPK
     * 判別ビットとして固定値に含めてしまい、bit7=1 の UNPK が判定から
     * 漏れていた。 */
    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opmode == 5 || opmode == 6) &&
        ea_mode <= 1) {
        uint16_t adjust;
        if (!dis_read_word(cpu, cursor, &adjust))
            return false;
        bool unpk = (opmode == 6);
        bool mem = (opcode & 0x0008) != 0;
        int src = opcode & 7, dst = (opcode >> 9) & 7;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", unpk ? "UNPK" : "PACK");
        if (mem)
            snprintf(r->operands, sizeof(r->operands), "-(A%d),-(A%d),#$%04X", src, dst,
                     adjust);
        else
            snprintf(r->operands, sizeof(r->operands), "D%d,D%d,#$%04X", src, dst, adjust);
        return true;
    }
    if (opmode == 3)
        return dis_muldiv(cpu, opcode, cursor, "DIVU", r);
    if (opmode == 7)
        return dis_muldiv(cpu, opcode, cursor, "DIVS", r);
    if (opmode <= 2)
        return dis_ea_to_dn(cpu, opcode, cursor, opmode, "OR", r);
    if (ea_mode <= 1) {
        if (opmode == 4)
            return dis_x_form(opcode, "SBCD", false, 'B', r);
        return false; /* opmode5,6 かつ ea_mode0/1: 未割り当て */
    }
    return dis_dn_to_ea(cpu, opcode, cursor, opmode & 3, "OR", r);
}

/* ================================================================== */
/* ライン 9: SUB / SUBA / SUBX                                         */
/* ================================================================== */

static bool dis_line9(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    if (opmode == 3 || opmode == 7)
        return dis_xxxa(cpu, opcode, cursor, "SUB", opmode, r);
    if (opmode <= 2)
        return dis_ea_to_dn(cpu, opcode, cursor, opmode, "SUB", r);
    if (ea_mode <= 1)
        return dis_x_form(opcode, "SUBX", true, g_szsuf[opmode & 3][1], r);
    return dis_dn_to_ea(cpu, opcode, cursor, opmode & 3, "SUB", r);
}

/* ================================================================== */
/* ライン B: CMP / CMPA / EOR / CMPM                                   */
/* ================================================================== */

static bool dis_lineB(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    if (opmode == 3 || opmode == 7)
        return dis_xxxa(cpu, opcode, cursor, "CMP", opmode, r);
    if (opmode <= 2)
        return dis_ea_to_dn(cpu, opcode, cursor, opmode, "CMP", r);
    if (ea_mode == 1)
        return dis_cmpm(opcode, r);
    return dis_dn_to_ea(cpu, opcode, cursor, opmode & 3, "EOR", r);
}

/* ================================================================== */
/* ライン C: AND / MULU / MULS / ABCD / EXG                            */
/* ================================================================== */

static bool dis_lineC(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    if (opmode == 3)
        return dis_muldiv(cpu, opcode, cursor, "MULU", r);
    if (opmode == 7)
        return dis_muldiv(cpu, opcode, cursor, "MULS", r);
    if (opmode <= 2)
        return dis_ea_to_dn(cpu, opcode, cursor, opmode, "AND", r);
    int opmode5 = (opcode >> 3) & 0x1F;
    if (opmode5 == 8 || opmode5 == 9 || opmode5 == 17)
        return dis_exg(opcode, r);
    if (ea_mode <= 1) {
        if (opmode == 4)
            return dis_x_form(opcode, "ABCD", false, 'B', r);
        return false;
    }
    return dis_dn_to_ea(cpu, opcode, cursor, opmode & 3, "AND", r);
}

/* ================================================================== */
/* ライン D: ADD / ADDA / ADDX                                         */
/* ================================================================== */

static bool dis_lineD(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    if (opmode == 3 || opmode == 7)
        return dis_xxxa(cpu, opcode, cursor, "ADD", opmode, r);
    if (opmode <= 2)
        return dis_ea_to_dn(cpu, opcode, cursor, opmode, "ADD", r);
    if (ea_mode <= 1)
        return dis_x_form(opcode, "ADDX", true, g_szsuf[opmode & 3][1], r);
    return dis_dn_to_ea(cpu, opcode, cursor, opmode & 3, "ADD", r);
}

/* ================================================================== */
/* ライン E: シフト / ローテート                                        */
/* ================================================================== */

static bool dis_lineE(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    if (mc68k_get_model(cpu) >= MC68K_MODEL_68020 && (opcode & 0xF8C0) >= 0xE8C0 &&
        (opcode & 0xF8C0) <= 0xEFC0) {
        static const char *const bf[] = {"BFTST", "BFEXTU", "BFCHG", "BFEXTS",
                                         "BFCLR", "BFFFO",  "BFSET", "BFINS"};
        int kind = (opcode >> 8) & 7, mode = (opcode >> 3) & 7, reg = opcode & 7;
        uint16_t ext;
        char ea[40], spec[24];
        if (!dis_read_word(cpu, cursor, &ext) ||
            !format_ea(cpu, cursor, mode, reg, 1,
                       kind == 0 || kind == 1 || kind == 3 || kind == 5 ? EA_CTX_CONTROL
                                                                        : EA_CTX_WRITE,
                       ea, sizeof(ea)))
            return false;
        if (ext & 0x0800)
            snprintf(spec, sizeof(spec), "D%d:", (ext >> 6) & 7);
        else
            snprintf(spec, sizeof(spec), "%d:", (ext >> 6) & 31);
        size_t n = strlen(spec);
        if (ext & 0x20)
            snprintf(spec + n, sizeof(spec) - n, "D%d", ext & 7);
        else
            snprintf(spec + n, sizeof(spec) - n, "%d", (ext & 31) ? (ext & 31) : 32);
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", bf[kind]);
        if (kind == 1 || kind == 3 || kind == 5 || kind == 7)
            snprintf(r->operands, sizeof(r->operands), "%s{%s},D%d", ea, spec, (ext >> 12) & 7);
        else
            snprintf(r->operands, sizeof(r->operands), "%s{%s}", ea, spec);
        return true;
    }
    static const char *const shft_names[4][2] = {
        {"ASR", "ASL"}, {"LSR", "LSL"}, {"ROXR", "ROXL"}, {"ROR", "ROL"}};
    int sz76 = (opcode >> 6) & 3;
    int dr = (opcode >> 8) & 1;

    if (sz76 == 3) {
        /* メモリシフト形式: 常にワード、count=1固定、type=bits10-9 */
        int type = (opcode >> 9) & 3;
        int ea_mode = (opcode >> 3) & 7;
        int ea_reg = opcode & 7;
        char ea[40];
        if (!format_ea(cpu, cursor, ea_mode, ea_reg, 2, EA_CTX_READ, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s", shft_names[type][dr]);
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }

    /* レジスタシフト形式 */
    int creg = (opcode >> 9) & 7;
    int ir = (opcode >> 5) & 1; /* 0=即値count, 1=Dnでcount指定 */
    int type = (opcode >> 3) & 3;
    int dn = opcode & 7;
    snprintf(r->mnemonic, sizeof(r->mnemonic), "%s%s", shft_names[type][dr], g_szsuf[sz76]);
    if (ir)
        snprintf(r->operands, sizeof(r->operands), "%s,%s", g_dreg[creg], g_dreg[dn]);
    else
        snprintf(r->operands, sizeof(r->operands), "#%d,%s", creg ? creg : 8, g_dreg[dn]);
    return true;
}

/* ================================================================== */
/* ライン F: コプロセッサ id=1    */
/* ================================================================== */

/*
 * EA の可否は [881UM] Table 4-14・4-15・4-17・4-18 と、FScc・FSAVE・FRESTORE
 * の各命令の頁による。Table 4-19 は FScc の mode 7 reg 0/1
 * （絶対アドレス）を「未定義」と印字するが、FScc の頁は (xxx).W/.L を許可するので後者に従う。
 * 逆アセンブルするのは 68020 以上の coprocessor id=1 の F-line だけで、FPU 構成の有無には
 * 依存しない（実行可否は判定しない）。他の id・68000/68010・符号化として不正な語は
 * dis_lineF() が false を返し、呼び出し側が "LINE-F"（長さ2）にする。
 * opmode の表は fpu_decode.c の valid_opmode()（[881UM] Table 4-13）と同じ集合。
 */
#define FPU_OP_CPID_MASK         0x0E00u /* opcode bit11-9 = coprocessor id */
#define FPU_OP_CPID_FPU          0x0200u /* id=1 */
#define FPU_OP_TYPE_SHIFT        6
#define FPU_OP_TYPE_MASK         7u
#define FPU_TYPE_GENERAL         0u
#define FPU_TYPE_SCC_GROUP       1u
#define FPU_TYPE_FBCC_W          2u
#define FPU_TYPE_FBCC_L          3u
#define FPU_TYPE_FSAVE           4u
#define FPU_TYPE_FRESTORE        5u
#define FPU_COND_MASK            0x1Fu /* predicate は下位5bit相当（Table 4-20 Note 3） */
#define FPU_OPCODE_FNOP          0xF280u
#define FPU_SCC_MODE_DBCC        1
#define FPU_EAMODE_OTHER         7
#define FPU_EAREG_ABS_L          1
#define FPU_EAREG_PC_D8          3
#define FPU_EAREG_IMM            4
#define FPU_EAREG_TRAPCC_W       2
#define FPU_EAREG_TRAPCC_L       3
#define FPU_EAREG_TRAPCC_NONE    4
#define FPU_CMD_CLASS_SHIFT      13
#define FPU_CMD_FIELD3           7u
#define FPU_CMD_SRC_SHIFT        10
#define FPU_CMD_DST_SHIFT        7
#define FPU_CMD_OPMODE_MASK      0x7Fu
#define FPU_CMD_SRC_ROM          7u
#define FPU_CMD_FMT_DYNAMIC_P    7u
#define FPU_CMD_K_REG_SHIFT      4
#define FPU_CMD_K_SIGN           0x40u
#define FPU_CMD_K_RANGE          128
#define FPU_CMD_MOVEM_MODE_SHIFT 11
#define FPU_CMD_MOVEM_MODE_MASK  3u
#define FPU_CMD_MOVEM_DYNAMIC    1u
#define FPU_CMD_MOVEM_POSTINC    2u
#define FPU_CMD_LIST_MASK        0xFFu
#define FPU_OPMODE_FTST          0x3Au
#define FPU_OPMODE_FSINCOS_BASE  0x30u
#define FPU_OPMODE_FSINCOS_MASK  0x78u
#define FPU_CTRL_FPCR            4u
#define FPU_CTRL_FPSR            2u
#define FPU_CTRL_FPIAR           1u
#define FPU_REG_COUNT            8
#define FPU_FMT_B                6u
#define FPU_FMT_W                4u
#define FPU_FMT_S                1u
#define FPU_FMT_L                0u
#define FPU_FMT_NAMES            "LSXPWDB"
#define FPU_COND_COUNT           32
#define FPU_LONG_BYTES           4
#define FPU_OPMODE_COUNT         0x40

static const char *const g_fpu_cond[FPU_COND_COUNT] = {
    "F",   "EQ",  "OGT",  "OGE", "OLT", "OLE", "OGL", "OR",  "UN",  "UEQ", "UGT",
    "UGE", "ULT", "ULE",  "NE",  "T",   "SF",  "SEQ", "GT",  "GE",  "LT",  "LE",
    "GL",  "GLE", "NGLE", "NGL", "NLE", "NLT", "NGE", "NGT", "SNE", "ST",
};

/* FSINCOS（0x30〜0x37）は呼出し側で別処理するため、この表では NULL */
static const char *const g_fpu_opmode[FPU_OPMODE_COUNT] = {
    [0x00] = "FMOVE",   [0x01] = "FINT",    [0x02] = "FSINH",   [0x03] = "FINTRZ",
    [0x04] = "FSQRT",   [0x06] = "FLOGNP1", [0x08] = "FETOXM1", [0x09] = "FTANH",
    [0x0A] = "FATAN",   [0x0C] = "FASIN",   [0x0D] = "FATANH",  [0x0E] = "FSIN",
    [0x0F] = "FTAN",    [0x10] = "FETOX",   [0x11] = "FTWOTOX", [0x12] = "FTENTOX",
    [0x14] = "FLOGN",   [0x15] = "FLOG10",  [0x16] = "FLOG2",   [0x18] = "FABS",
    [0x19] = "FCOSH",   [0x1A] = "FNEG",    [0x1C] = "FACOS",   [0x1D] = "FCOS",
    [0x1E] = "FGETEXP", [0x1F] = "FGETMAN", [0x20] = "FDIV",    [0x21] = "FMOD",
    [0x22] = "FADD",    [0x23] = "FMUL",    [0x24] = "FSGLDIV", [0x25] = "FREM",
    [0x26] = "FSCALE",  [0x27] = "FSGLMUL", [0x28] = "FSUB",    [0x38] = "FCMP",
    [0x3A] = "FTST",
};

typedef enum {
    FPU_EA_SRC,      /* Dn・メモリ・即値（B/W/L/S のソース） */
    FPU_EA_SRC_MEM,  /* メモリ・即値（X/P/D のソース） */
    FPU_EA_DST,      /* Dn・データ交代可能（B/W/L/S の宛先、FScc） */
    FPU_EA_DST_MEM,  /* データ交代可能のメモリ（X/P/D の宛先） */
    FPU_EA_CTRL_IN,  /* FMOVEM 入力・FRESTORE: (An)+ か制御モード */
    FPU_EA_CTRL_OUT, /* FMOVEM 出力・FSAVE: -(An) か制御交代可能 */
} fpu_ea_kind_t;

static bool fpu_ea_valid(fpu_ea_kind_t kind, int mode, int reg) {
    bool is_other = (mode == FPU_EAMODE_OTHER);
    bool src_other_ok = reg <= FPU_EAREG_IMM;
    bool dst_other_ok = reg <= FPU_EAREG_ABS_L;
    bool ctrl_other_ok = reg <= FPU_EAREG_PC_D8;

    switch (kind) {
    case FPU_EA_SRC:
        return mode != 1 && (!is_other || src_other_ok);
    case FPU_EA_SRC_MEM:
        return mode >= 2 && (!is_other || src_other_ok);
    case FPU_EA_DST:
        return mode != 1 && (!is_other || dst_other_ok);
    case FPU_EA_DST_MEM:
        return mode >= 2 && (!is_other || dst_other_ok);
    case FPU_EA_CTRL_IN:
        return mode == 2 || mode == 3 || mode == 5 || mode == 6 || (is_other && ctrl_other_ok);
    case FPU_EA_CTRL_OUT:
        return mode == 2 || mode == 4 || mode == 5 || mode == 6 || (is_other && dst_other_ok);
    }
    return false;
}

/* 形式のバイト数（#imm の読み出し幅）。B は word の下位バイト */
static int fpu_fmt_imm_bytes(unsigned fmt) {
    static const int bytes[] = {4, 4, 12, 12,
                                2, 8, 1}; /* L S X P W D B。B は word の下位バイト */
    return bytes[fmt];
}

/* mode7/reg4 の即値を生のビット列で表す（X/P/D は format_ea が持たない幅） */
static bool fpu_format_imm(mc68k_state_t *cpu, uint32_t *cursor, int bytes, char *out,
                           size_t outsz) {
    uint32_t v;
    uint16_t w;

    if (bytes <= 2) {
        if (!dis_read_word(cpu, cursor, &w))
            return false;
        snprintf(out, outsz, bytes == 1 ? "#$%02X" : "#$%04X",
                 bytes == 1 ? (unsigned)(w & 0xFFu) : (unsigned)w);
        return true;
    }
    size_t len = (size_t)snprintf(out, outsz, "#$");
    for (int i = 0; i < bytes / 4; i++) {
        if (!dis_read_long(cpu, cursor, &v))
            return false;
        len += (size_t)snprintf(out + len, outsz - len, "%08X", v);
    }
    return true;
}

/* operand の EA（#imm の幅 imm_bytes だけ format_ea と異なる。1 は B、2 は W、4 以上は long
 * の並び） */
static bool fpu_format_ea(mc68k_state_t *cpu, uint32_t *cursor, fpu_ea_kind_t kind, int mode,
                          int reg, int imm_bytes, char *out, size_t outsz) {
    if (!fpu_ea_valid(kind, mode, reg))
        return false;
    if (mode == FPU_EAMODE_OTHER && reg == FPU_EAREG_IMM)
        return fpu_format_imm(cpu, cursor, imm_bytes, out, outsz);
    return format_ea(cpu, cursor, mode, reg, 4, EA_CTX_READ, out, outsz);
}

/* FPn の並び（FP0-FP3/FP5）。mask は FP0 を bit0 とする正規化済みの値 */
static void fpu_format_reglist(unsigned mask, char *out, size_t outsz) {
    size_t len = 0;
    int i = 0;

    out[0] = '\0';
    if (mask == 0) {
        snprintf(out, outsz, "#0");
        return;
    }
    while (i < FPU_REG_COUNT) {
        if (!(mask & (1u << i))) {
            i++;
            continue;
        }
        int j = i;
        while (j + 1 < FPU_REG_COUNT && (mask & (1u << (j + 1))))
            j++;
        if (len > 0)
            len += (size_t)snprintf(out + len, outsz - len, "/");
        if (j > i)
            len += (size_t)snprintf(out + len, outsz - len, "FP%d-FP%d", i, j);
        else
            len += (size_t)snprintf(out + len, outsz - len, "FP%d", i);
        i = j + 1;
    }
}

/* control register の選択 → "FPCR/FPSR/FPIAR" の部分集合 */
static void fpu_format_ctrl(unsigned sel, char *out, size_t outsz) {
    size_t len = 0;

    out[0] = '\0';
    if (sel & FPU_CTRL_FPCR)
        len += (size_t)snprintf(out + len, outsz - len, "FPCR");
    if (sel & FPU_CTRL_FPSR)
        len += (size_t)snprintf(out + len, outsz - len, "%sFPSR", len ? "/" : "");
    if (sel & FPU_CTRL_FPIAR)
        len += (size_t)snprintf(out + len, outsz - len, "%sFPIAR", len ? "/" : "");
}

static int fpu_popcount3(unsigned v) {
    return (int)((v & 1u) + ((v >> 1) & 1u) + ((v >> 2) & 1u));
}

/* cpGEN の command word（opcode の EA 欄 mode/reg を ea_mode/ea_reg に受ける） */
static bool dis_fpu_general(mc68k_state_t *cpu, uint32_t *cursor, int ea_mode, int ea_reg,
                            dis_result_t *r) {
    uint16_t cmd;
    char ea[48], aux[48];

    if (!dis_read_word(cpu, cursor, &cmd))
        return false;
    unsigned cls = (unsigned)(cmd >> FPU_CMD_CLASS_SHIFT);
    unsigned field = (cmd >> FPU_CMD_SRC_SHIFT) & FPU_CMD_FIELD3;
    unsigned dst = (cmd >> FPU_CMD_DST_SHIFT) & FPU_CMD_FIELD3;
    unsigned low = cmd & FPU_CMD_OPMODE_MASK;
    bool sincos = (low & FPU_OPMODE_FSINCOS_MASK) == FPU_OPMODE_FSINCOS_BASE;
    const char *name = NULL; /* クラス0/2 の opmode 名。未定義なら NULL */

    if (sincos)
        name = "FSINCOS";
    else if (low < FPU_OPMODE_COUNT)
        name = g_fpu_opmode[low];

    switch (cls) {
    case 0: /* FPm,FPn */
        if (!name)
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s.X", name);
        if (sincos)
            snprintf(r->operands, sizeof(r->operands), "FP%u,FP%u:FP%u", field, low & 7u, dst);
        else if (low == FPU_OPMODE_FTST)
            snprintf(r->operands, sizeof(r->operands), "FP%u", field);
        else
            snprintf(r->operands, sizeof(r->operands), "FP%u,FP%u", field, dst);
        return true;
    case 2: /* <ea>,FPn / FMOVECR */
        if (field == FPU_CMD_SRC_ROM) {
            snprintf(r->mnemonic, sizeof(r->mnemonic), "FMOVECR.X");
            snprintf(r->operands, sizeof(r->operands), "#$%02X,FP%u", low, dst);
            return true;
        }
        if (!name)
            return false;
        if (!fpu_format_ea(cpu, cursor,
                           (field == FPU_FMT_L || field == FPU_FMT_S || field == FPU_FMT_W ||
                            field == FPU_FMT_B)
                               ? FPU_EA_SRC
                               : FPU_EA_SRC_MEM,
                           ea_mode, ea_reg, fpu_fmt_imm_bytes(field), ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s.%c", name, FPU_FMT_NAMES[field]);
        if (low == FPU_OPMODE_FTST)
            snprintf(r->operands, sizeof(r->operands), "%s", ea);
        else if (sincos)
            snprintf(r->operands, sizeof(r->operands), "%s,FP%u:FP%u", ea, low & 7u, dst);
        else
            snprintf(r->operands, sizeof(r->operands), "%s,FP%u", ea, dst);
        return true;
    case 3: { /* FPm,<ea> */
        unsigned fmt = (field == FPU_CMD_FMT_DYNAMIC_P) ? 3u : field;
        bool reg_ok =
            (fmt == FPU_FMT_L || fmt == FPU_FMT_S || fmt == FPU_FMT_W || fmt == FPU_FMT_B);
        if (!fpu_format_ea(cpu, cursor, reg_ok ? FPU_EA_DST : FPU_EA_DST_MEM, ea_mode, ea_reg,
                           fpu_fmt_imm_bytes(fmt), ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "FMOVE.%c", FPU_FMT_NAMES[fmt]);
        if (field == FPU_CMD_FMT_DYNAMIC_P)
            snprintf(r->operands, sizeof(r->operands), "FP%u,%s{D%u}", dst, ea,
                     (low >> FPU_CMD_K_REG_SHIFT) & FPU_CMD_FIELD3);
        else if (fmt == 3u) {
            int k =
                (int)low - ((low & FPU_CMD_K_SIGN) ? FPU_CMD_K_RANGE : 0); /* 7bit 2の補数 */
            snprintf(r->operands, sizeof(r->operands), "FP%u,%s{#%d}", dst, ea, k);
        } else
            snprintf(r->operands, sizeof(r->operands), "FP%u,%s", dst, ea);
        return true;
    }
    case 4:
    case 5: { /* control register 入出力。select 000 は FPIAR（decoder と同じ） */
        bool out = (cls == 5);
        unsigned sel = field ? field : FPU_CTRL_FPIAR;
        bool single = fpu_popcount3(sel) == 1;
        bool an_ok = single && sel == FPU_CTRL_FPIAR;
        if (ea_mode == 1) {
            if (!an_ok)
                return false;
            snprintf(ea, sizeof(ea), "%s", g_areg[ea_reg]);
        } else if (ea_mode == 0) {
            if (!single)
                return false;
            snprintf(ea, sizeof(ea), "%s", g_dreg[ea_reg]);
        } else if (!fpu_format_ea(cpu, cursor, out ? FPU_EA_DST_MEM : FPU_EA_SRC_MEM, ea_mode,
                                  ea_reg, FPU_LONG_BYTES * fpu_popcount3(sel), ea, sizeof(ea)))
            return false;
        fpu_format_ctrl(sel, aux, sizeof(aux));
        snprintf(r->mnemonic, sizeof(r->mnemonic), single ? "FMOVE.L" : "FMOVEM.L");
        if (out)
            snprintf(r->operands, sizeof(r->operands), "%s,%s", aux, ea);
        else
            snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, aux);
        return true;
    }
    case 6:
    case 7: { /* FMOVEM.X */
        bool out = (cls == 7);
        unsigned mode = (cmd >> FPU_CMD_MOVEM_MODE_SHIFT) & FPU_CMD_MOVEM_MODE_MASK;
        bool predec = !(mode & FPU_CMD_MOVEM_POSTINC);
        unsigned list = cmd & FPU_CMD_LIST_MASK;

        if (out && (ea_mode == 4) != predec) /* predecrement mode は -(An) とだけ組む */
            return false;
        if (!fpu_format_ea(cpu, cursor, out ? FPU_EA_CTRL_OUT : FPU_EA_CTRL_IN, ea_mode, ea_reg,
                           FPU_LONG_BYTES, ea, sizeof(ea)))
            return false;
        if (mode & FPU_CMD_MOVEM_DYNAMIC) {
            snprintf(aux, sizeof(aux), "%s",
                     g_dreg[(list >> FPU_CMD_K_REG_SHIFT) & FPU_CMD_FIELD3]);
        } else {
            unsigned norm = 0; /* FP0 = bit0 に正規化する */
            for (int b = 0; b < FPU_REG_COUNT; b++)
                if (list & (1u << b))
                    norm |= 1u << (predec ? b : FPU_REG_COUNT - 1 - b);
            fpu_format_reglist(norm, aux, sizeof(aux));
        }
        snprintf(r->mnemonic, sizeof(r->mnemonic), "FMOVEM.X");
        if (out)
            snprintf(r->operands, sizeof(r->operands), "%s,%s", aux, ea);
        else
            snprintf(r->operands, sizeof(r->operands), "%s,%s", ea, aux);
        return true;
    }
    default: /* クラス001 */
        return false;
    }
}

/* MC68060 の非 FPU F-line（cpID 0/2/3 の LPSTOP・PFLUSH・PLPA・CINV・CPUSH・MOVE16）。
 * PTEST は 060 に無く、ここで拒否されて "LINE-F" になる。 */
enum {
    OP60_LPSTOP = 0xF800,
    OP60_LPSTOP_EXT = 0x01C0,
    OP60_PFLUSH_BASE = 0xF500,
    OP60_PFLUSH_MASK = 0xFFE0,
    OP60_PLPAW_BASE = 0xF588,
    OP60_PLPAR_BASE = 0xF5C8,
    OP60_PLPA_MASK = 0xFFF8,
    OP60_CACHE_BASE = 0xF400,
    OP60_CACHE_MASK = 0xFF00,
    OP60_CACHE_PUSH_BIT = 0x20,
    OP60_MOVE16_BASE = 0xF600,
    OP60_MOVE16_MASK = 0xFFC0,
    OP60_MOVE16_PI_PI = 0xF620,
    OP60_MOVE16_PI_PI_EXT_MASK = 0x8FFF,
    OP60_MOVE16_PI_PI_EXT = 0x8000,
};

static bool dis_lineF_060(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor,
                          dis_result_t *r) {
    int reg = opcode & 7;
    if (opcode == OP60_LPSTOP) {
        uint16_t ext, imm;
        if (!dis_read_word(cpu, cursor, &ext) || ext != OP60_LPSTOP_EXT ||
            !dis_read_word(cpu, cursor, &imm))
            return false;
        set_noop(r, "LPSTOP");
        snprintf(r->operands, sizeof(r->operands), "#$%04X", imm);
        return true;
    }
    if ((opcode & OP60_PFLUSH_MASK) == OP60_PFLUSH_BASE) {
        static const char *const names[4] = {"PFLUSHN", "PFLUSH", "PFLUSHAN", "PFLUSHA"};
        int form = (opcode >> 3) & 3;
        set_noop(r, names[form]);
        if (form < 2)
            snprintf(r->operands, sizeof(r->operands), "(%s)", g_areg[reg]);
        return true;
    }
    if ((opcode & OP60_PLPA_MASK) == OP60_PLPAW_BASE ||
        (opcode & OP60_PLPA_MASK) == OP60_PLPAR_BASE) {
        set_noop(r, (opcode & OP60_PLPA_MASK) == OP60_PLPAW_BASE ? "PLPAW" : "PLPAR");
        snprintf(r->operands, sizeof(r->operands), "(%s)", g_areg[reg]);
        return true;
    }
    if ((opcode & OP60_CACHE_MASK) == OP60_CACHE_BASE) {
        static const char *const caches[4] = {NULL, "DC", "IC", "BC"};
        static const char *const scopes[4] = {NULL, "L", "P", "A"};
        const char *cache = caches[(opcode >> 6) & 3], *scope = scopes[(opcode >> 3) & 3];
        if (!cache || !scope)
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "%s%s",
                 (opcode & OP60_CACHE_PUSH_BIT) ? "CPUSH" : "CINV", scope);
        if (scope[0] == 'A')
            snprintf(r->operands, sizeof(r->operands), "%s", cache);
        else
            snprintf(r->operands, sizeof(r->operands), "%s,(%s)", cache, g_areg[reg]);
        return true;
    }
    if ((opcode & OP60_MOVE16_MASK) == OP60_MOVE16_BASE) {
        uint16_t ext;
        uint32_t abs;
        if ((opcode & 0xFFF8) == OP60_MOVE16_PI_PI) {
            if (!dis_read_word(cpu, cursor, &ext) ||
                (ext & OP60_MOVE16_PI_PI_EXT_MASK) != OP60_MOVE16_PI_PI_EXT)
                return false;
            set_noop(r, "MOVE16");
            snprintf(r->operands, sizeof(r->operands), "(%s)+,(%s)+", g_areg[reg],
                     g_areg[(ext >> 12) & 7]);
            return true;
        }
        if ((opcode & 0xFFE0) != OP60_MOVE16_BASE || !dis_read_long(cpu, cursor, &abs))
            return false;
        static const char *const forms[4] = {"(%s)+,$%08X.L", "$%08X.L,(%s)+", "(%s),$%08X.L",
                                             "$%08X.L,(%s)"};
        int form = (opcode >> 3) & 3;
        set_noop(r, "MOVE16");
        if (form & 1)
            snprintf(r->operands, sizeof(r->operands), forms[form], abs, g_areg[reg]);
        else
            snprintf(r->operands, sizeof(r->operands), forms[form], g_areg[reg], abs);
        return true;
    }
    return false;
}

static bool dis_lineF(mc68k_state_t *cpu, uint16_t opcode, uint32_t *cursor, dis_result_t *r) {
    if (mc68k_get_model(cpu) == MC68K_MODEL_68060 &&
        (opcode & FPU_OP_CPID_MASK) != FPU_OP_CPID_FPU)
        return dis_lineF_060(cpu, opcode, cursor, r);
    unsigned type = (opcode >> FPU_OP_TYPE_SHIFT) & FPU_OP_TYPE_MASK;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t base = *cursor; /* opcode 直後 = FBcc の変位の基点 */
    char ea[48];

    if (mc68k_get_model(cpu) < MC68K_MODEL_68020 ||
        (opcode & FPU_OP_CPID_MASK) != FPU_OP_CPID_FPU)
        return false;

    switch (type) {
    case FPU_TYPE_GENERAL:
        return dis_fpu_general(cpu, cursor, ea_mode, ea_reg, r);
    case FPU_TYPE_SCC_GROUP: {
        uint16_t pred;
        const char *cond;

        if (!dis_read_word(cpu, cursor, &pred))
            return false;
        cond = g_fpu_cond[pred & FPU_COND_MASK];
        if (ea_mode == FPU_SCC_MODE_DBCC) {
            uint16_t d16;
            uint32_t disp_at = *cursor;
            if (!dis_read_word(cpu, cursor, &d16))
                return false;
            snprintf(r->mnemonic, sizeof(r->mnemonic), "FDB%s", cond);
            snprintf(r->operands, sizeof(r->operands), "%s,$%08X", g_dreg[ea_reg],
                     dis_addr(cpu, disp_at + (uint32_t)(int32_t)(int16_t)d16));
            return true;
        }
        if (ea_mode == FPU_EAMODE_OTHER &&
            (ea_reg == FPU_EAREG_TRAPCC_W || ea_reg == FPU_EAREG_TRAPCC_L ||
             ea_reg == FPU_EAREG_TRAPCC_NONE)) {
            snprintf(r->mnemonic, sizeof(r->mnemonic), "FTRAP%s%s", cond,
                     ea_reg == FPU_EAREG_TRAPCC_W   ? ".W"
                     : ea_reg == FPU_EAREG_TRAPCC_L ? ".L"
                                                    : "");
            r->operands[0] = '\0';
            if (ea_reg == FPU_EAREG_TRAPCC_NONE)
                return true;
            if (!format_ea(cpu, cursor, FPU_EAMODE_OTHER, FPU_EAREG_IMM,
                           ea_reg == FPU_EAREG_TRAPCC_W ? 2 : 4, EA_CTX_READ, ea, sizeof(ea)))
                return false;
            snprintf(r->operands, sizeof(r->operands), "%s", ea);
            return true;
        }
        if (!fpu_format_ea(cpu, cursor, FPU_EA_DST, ea_mode, ea_reg, 1, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic), "FS%s", cond);
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    }
    case FPU_TYPE_FBCC_W:
    case FPU_TYPE_FBCC_L: {
        const char *cond = g_fpu_cond[opcode & FPU_COND_MASK];
        uint32_t disp;

        if (type == FPU_TYPE_FBCC_W) {
            uint16_t d16;
            if (!dis_read_word(cpu, cursor, &d16))
                return false;
            if (opcode == FPU_OPCODE_FNOP && d16 == 0) {
                set_noop(r, "FNOP");
                return true;
            }
            disp = (uint32_t)(int32_t)(int16_t)d16;
        } else if (!dis_read_long(cpu, cursor, &disp)) {
            return false;
        }
        snprintf(r->mnemonic, sizeof(r->mnemonic), type == FPU_TYPE_FBCC_W ? "FB%s" : "FB%s.L",
                 cond);
        snprintf(r->operands, sizeof(r->operands), "$%08X", dis_addr(cpu, base + disp));
        return true;
    }
    case FPU_TYPE_FSAVE:
    case FPU_TYPE_FRESTORE:
        if (!fpu_format_ea(cpu, cursor,
                           type == FPU_TYPE_FSAVE ? FPU_EA_CTRL_OUT : FPU_EA_CTRL_IN, ea_mode,
                           ea_reg, FPU_LONG_BYTES, ea, sizeof(ea)))
            return false;
        snprintf(r->mnemonic, sizeof(r->mnemonic),
                 type == FPU_TYPE_FSAVE ? "FSAVE" : "FRESTORE");
        snprintf(r->operands, sizeof(r->operands), "%s", ea);
        return true;
    default:
        return false;
    }
}

/* ================================================================== */
/* トップレベル: mc68k_disassemble()                                   */
/* ================================================================== */

int mc68k_disassemble(mc68k_state_t *cpu, uint32_t addr, char *buf, size_t buf_size) {
    if (!cpu || !buf || buf_size == 0)
        return -1;
    if (mc68k_get_model(cpu) < MC68K_MODEL_68020)
        addr &= 0xFFFFFFu;
    if (addr & 1)
        return -1; /* 命令は常に偶数アドレスから始まる */

    uint32_t cursor = addr;
    uint16_t opcode;
    if (!dis_read_word(cpu, &cursor, &opcode))
        return -1; /* peek_byte 未登録（mc68k_debug_read_bytes()と同じ失敗契約） */

    dis_result_t r;
    bool ok;
    int line = (opcode >> 12) & 0xF;

    switch (line) {
    case 0x0:
        ok = dis_line0(cpu, opcode, &cursor, &r);
        break;
    case 0x1:
        ok = dis_move(cpu, opcode, &cursor, 1, &r);
        break;
    case 0x2:
        ok = dis_move(cpu, opcode, &cursor, 4, &r);
        break;
    case 0x3:
        ok = dis_move(cpu, opcode, &cursor, 2, &r);
        break;
    case 0x4:
        ok = dis_line4(cpu, opcode, &cursor, &r);
        break;
    case 0x5:
        ok = dis_line5(cpu, opcode, &cursor, &r);
        break;
    case 0x6:
        ok = dis_line6(cpu, opcode, &cursor, &r);
        break;
    case 0x7:
        ok = dis_line7(opcode, &r);
        break;
    case 0x8:
        ok = dis_line8(cpu, opcode, &cursor, &r);
        break;
    case 0x9:
        ok = dis_line9(cpu, opcode, &cursor, &r);
        break;
    case 0xA:
        set_noop(&r, "LINE-A"); /* Mac ROMコール用に予約。X68000では未使用 */
        ok = true;
        break;
    case 0xB:
        ok = dis_lineB(cpu, opcode, &cursor, &r);
        break;
    case 0xC:
        ok = dis_lineC(cpu, opcode, &cursor, &r);
        break;
    case 0xD:
        ok = dis_lineD(cpu, opcode, &cursor, &r);
        break;
    case 0xE:
        ok = dis_lineE(cpu, opcode, &cursor, &r);
        break;
    default: /* 0xF: coprocessor id=1（68020以上）だけ FPU 命令。他は LINE-F */
        ok = dis_lineF(cpu, opcode, &cursor, &r);
        if (!ok) {
            cursor = addr + 2;
            set_noop(&r, "LINE-F");
            ok = true;
        }
        break;
    }

    char full[112];
    if (!ok) {
        /* 未割り当てオペコード、またはカバー漏れ時の安全側フォールバック。
         * decode.c の op_illegal と同じくオペコード1ワード分のみを消費した
         * ことにする（各 dis_lineN が false を返す直前に拡張ワードを
         * 一部読んでいる可能性があるため cursor を明示的に巻き戻す）。 */
        cursor = addr + 2;
        snprintf(full, sizeof(full), "ILLEGAL");
    } else if (r.operands[0] == '\0') {
        snprintf(full, sizeof(full), "%s", r.mnemonic);
    } else {
        snprintf(full, sizeof(full), "%s %s", r.mnemonic, r.operands);
    }

    snprintf(buf, buf_size, "%s", full);
    return (int)(cursor - addr);
}
