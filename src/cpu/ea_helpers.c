/*
 * ea_helpers.c — 実効アドレス（EA）アクセス共有ヘルパー
 *
 * 実効アドレス（EA）モード一覧:
 *   mode 0: Dn          データレジスタ直接
 *   mode 1: An          アドレスレジスタ直接（常に 32bit）
 *   mode 2: (An)        アドレスレジスタ間接
 *   mode 3: (An)+       ポストインクリメント間接
 *   mode 4: -(An)       プレデクリメント間接
 *   mode 5: d16(An)     16bit ディスプレースメント付き間接
 *   mode 6: d8(An,Xn)   インデックス付き間接
 *   mode 7, reg 0: xxx.W   絶対短アドレス（16bit 符号拡張）
 *   mode 7, reg 1: xxx.L   絶対長アドレス（32bit）
 *   mode 7, reg 2: d16(PC) PC 相対（ソースのみ）
 *   mode 7, reg 3: d8(PC,Xn) PC 相対インデックス
 *   mode 7, reg 4: #imm   イミディエイト（ソースのみ）
 *
 * ここで定義する ea_read/ea_write/ea_rmw_read/ea_rmw_write/ea_get_addr は
 * ops_move.c 以下すべての命令ハンドラファイルから共有される。
 */
#include "cpu_internal.h"
#include <stdint.h>

/*
 * A7（スタックポインタ）へのバイトアクセス時のステップ数。
 * MC68000 はスタックをワード境界に揃えるため、A7 のバイトアクセスでも
 * インクリメント/デクリメントは 2 バイト単位で行う。
 */
static inline int sp_byte_step(int reg, int size) {
    return (size == 1 && reg == 7) ? 2 : size;
}

/*
 * ブリーフ拡張ワードによるアドレス計算（mode 6 / mode 7 reg 3 共用）。
 *
 * ブリーフ拡張ワードのフォーマット（16bit）:
 *   bit 15   : 0=Dn, 1=An（インデックスレジスタ種別）
 *   bits 14-12: インデックスレジスタ番号
 *   bit 11   : 0=word 符号拡張, 1=long（インデックスサイズ）
 *   bits 10-8: スケール（MC68000 では 0 のみ有効、実質的に 1 倍固定）
 *   bits 7-0 : 8bit 符号付きディスプレースメント
 *
 * X68000 のコンパイラが頻繁に生成するモードなので
 * 正確に実装しておく必要がある。
 */
static void ea_desc_init(mc68k_state_t *cpu, int mode, int reg, int size) {
    cpu->last_ea = (ea_desc_t){
        .mode = (uint8_t)mode,
        .reg = (uint8_t)reg,
        .size = (uint8_t)size,
        .ext_form = EA_EXT_NONE,
    };
}

static uint32_t brief_ea_calc(mc68k_state_t *cpu, uint32_t base, uint16_t ext,
                              ea_desc_t *desc) {
    desc->ext_form = EA_EXT_BRIEF;
    int da = (ext >> 15) & 1;
    int ridx = (ext >> 12) & 7;
    int wl = (ext >> 11) & 1; /* 0=word sign-extend, 1=long */
    int8_t d8 = (int8_t)(ext & 0xFF);
    uint32_t xn = da ? cpu->a[ridx] : cpu->d[ridx];
    if (!wl)
        xn = (uint32_t)(int32_t)(int16_t)(xn & 0xFFFF);
    uint32_t scale = cpu_has_68020_isa(cpu) ? 1u << ((ext >> 9) & 3) : 1;
    return base + xn * scale + (uint32_t)(int32_t)d8;
}

static uint32_t full_ext_ea(mc68k_state_t *cpu, uint32_t base, uint16_t ext,
                            const mc68k_access_t *pointer_access, ea_desc_t *desc) {
    bool bs = (ext & 0x0080) != 0, is = (ext & 0x0040) != 0;
    unsigned bd_size = (ext >> 4) & 3, iis = ext & 7;
    desc->ext_form = EA_EXT_FULL;
    /* 拡張語は 01=null、10=word、11=long を符号化する。タイミング表が直接使えるよう、
     * ディスクリプタでは意図的に密な 0/1/2 表現を用いる。 */
    desc->bd_size = (uint8_t)(bd_size - 1);
    desc->iis = (uint8_t)iis;
    desc->od_size = iis == 0 ? 0 : (uint8_t)(iis & 3);
    desc->base_suppressed = (uint8_t)bs;
    desc->index_suppressed = (uint8_t)is;
    if (bd_size == 0 || (iis == 4) || (is && iis >= 5)) {
        cpu->ea_invalid_pending = true;
        cpu->exec_state = CPU_EXCEPTION;
        return 0;
    }
    /* 060 の EA 計算時間（[060UM Table 10-5]）: full format は 1、memory indirect は 3。 */
    cpu->t060_ea_extra += iis == 0 ? 1 : 3;
    uint32_t bd = 0, od = 0, index = 0;
    if (bd_size == 2) {
        bd = (uint32_t)(int32_t)(int16_t)cpu_fetch_word(cpu);
    } else if (bd_size == 3) {
        uint32_t hi = cpu_fetch_word(cpu);
        if (cpu->exec_state == CPU_NORMAL)
            bd = (hi << 16) | cpu_fetch_word(cpu);
    }
    if (cpu->exec_state != CPU_NORMAL)
        return 0;
    if (!is) {
        index = (ext & 0x8000) ? cpu->a[(ext >> 12) & 7] : cpu->d[(ext >> 12) & 7];
        if (!(ext & 0x0800))
            index = (uint32_t)(int32_t)(int16_t)index;
        index *= 1u << ((ext >> 9) & 3);
    }
    uint32_t base_bd = (bs ? 0 : base) + bd;
    if (iis == 0)
        return base_bd + index;
    bool post = iis >= 5;
    unsigned od_size = iis & 3;
    if (od_size == 2) {
        od = (uint32_t)(int32_t)(int16_t)cpu_fetch_word(cpu);
    } else if (od_size == 3) {
        uint32_t hi = cpu_fetch_word(cpu);
        if (cpu->exec_state == CPU_NORMAL)
            od = (hi << 16) | cpu_fetch_word(cpu);
    }
    if (cpu->exec_state != CPU_NORMAL)
        return 0;
    uint32_t ptr;
    if (pointer_access) {
        /* メモリ間接ポインタは最終 MOVES オペランドが書込みでも読出しサイクルである。
         * ただしそのオペランドの DFC/SFC ファンクションコードを使う。 */
        mc68k_access_t access = *pointer_access;
        access.read = true;
        access.rmw = false;
        access.size = 4;
        ptr = cpu_read_long_access(cpu, post ? base_bd : base_bd + index, &access);
    } else {
        ptr = cpu_read_long(cpu, post ? base_bd : base_bd + index);
    }
    if (cpu->exec_state != CPU_NORMAL)
        return 0;
    return ptr + od + (post ? index : 0);
}

static uint32_t indexed_ea_access(mc68k_state_t *cpu, uint32_t base,
                                  const mc68k_access_t *pointer_access, ea_desc_t *desc) {
    uint16_t ext = cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 0;
    if (!cpu_has_68020_isa(cpu) || !(ext & 0x0100))
        return brief_ea_calc(cpu, base, ext, desc);
    return full_ext_ea(cpu, base, ext, pointer_access, desc);
}

static uint32_t indexed_ea(mc68k_state_t *cpu, uint32_t base, ea_desc_t *desc) {
    return indexed_ea_access(cpu, base, NULL, desc);
}

/* ================================================================== */
/* メモリアクセスヘルパー                                              */
/* ================================================================== */

static uint32_t mem_read_sized_access(mc68k_state_t *cpu, uint32_t addr, int size,
                                      const mc68k_access_t *template_access) {
    mc68k_access_t access =
        template_access ? *template_access : cpu_data_access(cpu, true, (uint8_t)size, false);
    access.read = true;
    access.rmw = false;
    access.size = (uint8_t)size;
    switch (size) {
    case 1:
        return cpu_read_byte_access(cpu, addr, &access);
    case 2:
        return cpu_read_word_access(cpu, addr, &access);
    case 4:
        return cpu_read_long_access(cpu, addr, &access);
    default:
        return 0;
    }
}

static void mem_write_sized_access(mc68k_state_t *cpu, uint32_t addr, int size, uint32_t val,
                                   const mc68k_access_t *template_access) {
    mc68k_access_t access =
        template_access ? *template_access : cpu_data_access(cpu, false, (uint8_t)size, false);
    access.read = false;
    access.rmw = false;
    access.size = (uint8_t)size;
    switch (size) {
    case 1:
        cpu_write_byte_access(cpu, addr, (uint8_t)val, &access);
        break;
    case 2:
        cpu_write_word_access(cpu, addr, (uint16_t)val, &access);
        break;
    case 4:
        cpu_write_long_access(cpu, addr, val, &access);
        break;
    }
}

/* locked marks a genuinely indivisible RMW bus cycle (TAS/CAS/CAS2 only).
 * Ordinary read-modify-write instructions (ADD/SUB/AND/OR/EOR <ea>, NOT/NEG/
 * NEGX, BCHG/BCLR/BSET, memory shifts, NBCD, ...) merely reuse one EA
 * calculation across an independent read cycle and write cycle, so they pass
 * locked=false (: setting it unconditionally made SSW.RM look like a
 * locked cycle for every RMW instruction, which NetBSD's WRFAULT_2030 then
 * misread as a write fault on what was really a read). */
static uint32_t mem_rmw_read_sized(mc68k_state_t *cpu, uint32_t addr, int size, bool locked) {
    const mc68k_access_t access = cpu_data_access(cpu, true, (uint8_t)size, locked);
    switch (size) {
    case 1:
        return cpu_read_byte_access(cpu, addr, &access);
    case 2:
        return cpu_read_word_access(cpu, addr, &access);
    case 4:
        return cpu_read_long_access(cpu, addr, &access);
    default:
        return 0;
    }
}

static void mem_rmw_write_sized(mc68k_state_t *cpu, uint32_t addr, int size, uint32_t val,
                                bool locked) {
    const mc68k_access_t access = cpu_data_access(cpu, false, (uint8_t)size, locked);
    switch (size) {
    case 1:
        cpu_write_byte_access(cpu, addr, (uint8_t)val, &access);
        break;
    case 2:
        cpu_write_word_access(cpu, addr, (uint16_t)val, &access);
        break;
    case 4:
        cpu_write_long_access(cpu, addr, val, &access);
        break;
    }
}

/* ================================================================== */
/* 実効アドレス（EA）読み取り                                          */
/* ================================================================== */

uint32_t ea_read_access(mc68k_state_t *cpu, int mode, int reg, int size,
                        const mc68k_access_t *access) {
    uint32_t addr;
    ea_desc_init(cpu, mode, reg, size);
    switch (mode) {
    case 0: /* Dn */
        return cpu->d[reg] & size_mask(size);
    case 1: /* An: サイズに関わらず 32bit 全体 */
        return cpu->a[reg];
    case 2: /* (An) */
        return mem_read_sized_access(cpu, cpu->a[reg], size, access);
    case 3: /* (An)+: 先に読んでから An を進める */
        addr = cpu->a[reg];
        cpu->a[reg] += sp_byte_step(reg, size);
        return mem_read_sized_access(cpu, addr, size, access);
    case 4: /* -(An): 先に An を戻してから読む */
        cpu->a[reg] -= sp_byte_step(reg, size);
        return mem_read_sized_access(cpu, cpu->a[reg], size, access);
    case 5: { /* d16(An) */
        int16_t disp = (int16_t)cpu_fetch_word(cpu);
        if (cpu->exec_state != CPU_NORMAL)
            return 0;
        return mem_read_sized_access(cpu, cpu->a[reg] + (uint32_t)(int32_t)disp, size, access);
    }
    case 6: /* d8(An,Xn): ブリーフ拡張ワード */
        addr = indexed_ea_access(cpu, cpu->a[reg], access, &cpu->last_ea);
        return cpu->exec_state == CPU_NORMAL ? mem_read_sized_access(cpu, addr, size, access)
                                             : 0;
    case 7:
        switch (reg) {
        case 0: { /* xxx.W: 16bit 符号拡張 */
            addr = (uint32_t)(int32_t)(int16_t)cpu_fetch_word(cpu);
            return cpu->exec_state == CPU_NORMAL
                       ? mem_read_sized_access(cpu, addr, size, access)
                       : 0;
        }
        case 1: { /* xxx.L: 32bit */
            addr = (uint32_t)cpu_fetch_word(cpu) << 16;
            if (cpu->exec_state == CPU_NORMAL)
                addr |= (uint32_t)cpu_fetch_word(cpu);
            return cpu->exec_state == CPU_NORMAL
                       ? mem_read_sized_access(cpu, addr, size, access)
                       : 0;
        }
        case 2: { /* d16(PC): 基準 PC は拡張ワードのアドレス */
            uint32_t base = cpu->pc;
            int16_t disp = (int16_t)cpu_fetch_word(cpu);
            return cpu->exec_state == CPU_NORMAL
                       ? mem_read_sized_access(cpu, base + (uint32_t)(int32_t)disp, size,
                                               access)
                       : 0;
        }
        case 3: { /* d8(PC,Xn): PC 相対インデックス */
            uint32_t base = cpu->pc;
            addr = indexed_ea_access(cpu, base, access, &cpu->last_ea);
            return cpu->exec_state == CPU_NORMAL
                       ? mem_read_sized_access(cpu, addr, size, access)
                       : 0;
        }
        case 4: { /* #imm */
            if (size == 4) {
                uint32_t hi = cpu_fetch_word(cpu);
                if (cpu->exec_state != CPU_NORMAL)
                    return 0;
                uint32_t lo = cpu_fetch_word(cpu);
                return (hi << 16) | lo;
            }
            return cpu_fetch_word(cpu) & size_mask(size);
        }
        default:
            cpu->exec_state = CPU_EXCEPTION;
            return 0;
        }
    default:
        cpu->exec_state = CPU_EXCEPTION;
        return 0;
    }
}

uint32_t ea_read(mc68k_state_t *cpu, int mode, int reg, int size) {
    return ea_read_access(cpu, mode, reg, size, NULL);
}

/* ================================================================== */
/* 実効アドレス（EA）書き込み                                          */
/* ================================================================== */

/* MC68000 の CLR・Scc・MOVE from SR は、書込みの前に同じアドレスを一度 read する
 * （UM Table 8-6, 8-12 の表値 8(1/1)+ に read が含まれる）。cpu->ea_read_before_write が
 * 立っている間だけ、最初のメモリ書込みの直前に読出しを発行して値を捨てる。 */
static void mem_write_sized_access_rbw(mc68k_state_t *cpu, uint32_t addr, int size, uint32_t val,
                                       const mc68k_access_t *access) {
    if (cpu->ea_read_before_write && cpu->model == MC68K_MODEL_68000) {
        cpu->ea_read_before_write = false;
        (void)mem_read_sized_access(cpu, addr, size, access);
        if (cpu->exec_state != CPU_NORMAL)
            return;
    }
    mem_write_sized_access(cpu, addr, size, val, access);
}

void ea_write_access(mc68k_state_t *cpu, int mode, int reg, int size, uint32_t val,
                     const mc68k_access_t *access) {
    uint32_t addr;
    ea_desc_init(cpu, mode, reg, size);
    switch (mode) {
    case 0: { /* Dn: サイズ以下のビットのみ更新、上位は保持 */
        uint32_t mask = size_mask(size);
        cpu->d[reg] = (cpu->d[reg] & ~mask) | (val & mask);
        break;
    }
    case 1: /* An: MOVEA 専用。CCR は呼び出し元で制御 */
        cpu->a[reg] = val;
        break;
    case 2:
        mem_write_sized_access_rbw(cpu, cpu->a[reg], size, val, access);
        break;
    case 3: /* (An)+: 先に書いてから進める */
        mem_write_sized_access_rbw(cpu, cpu->a[reg], size, val, access);
        cpu->a[reg] += sp_byte_step(reg, size);
        break;
    case 4: /* -(An): 先に戻してから書く */
        cpu->a[reg] -= sp_byte_step(reg, size);
        mem_write_sized_access_rbw(cpu, cpu->a[reg], size, val, access);
        break;
    case 5: {
        int16_t disp = (int16_t)cpu_fetch_word(cpu);
        if (cpu->exec_state == CPU_NORMAL)
            mem_write_sized_access_rbw(cpu, cpu->a[reg] + (uint32_t)(int32_t)disp, size, val,
                                   access);
        break;
    }
    case 6: /* d8(An,Xn) */
        addr = indexed_ea_access(cpu, cpu->a[reg], access, &cpu->last_ea);
        if (cpu->exec_state == CPU_NORMAL)
            mem_write_sized_access_rbw(cpu, addr, size, val, access);
        break;
    case 7:
        switch (reg) {
        case 0: {
            addr = (uint32_t)(int32_t)(int16_t)cpu_fetch_word(cpu);
            if (cpu->exec_state == CPU_NORMAL)
                mem_write_sized_access_rbw(cpu, addr, size, val, access);
            break;
        }
        case 1: {
            addr = (uint32_t)cpu_fetch_word(cpu) << 16;
            if (cpu->exec_state == CPU_NORMAL)
                addr |= (uint32_t)cpu_fetch_word(cpu);
            if (cpu->exec_state == CPU_NORMAL)
                mem_write_sized_access_rbw(cpu, addr, size, val, access);
            break;
        }
        default:
            cpu->exec_state = CPU_EXCEPTION;
            break;
        }
        break;
    default:
        cpu->exec_state = CPU_EXCEPTION;
        break;
    }
}

void ea_write(mc68k_state_t *cpu, int mode, int reg, int size, uint32_t val) {
    ea_write_access(cpu, mode, reg, size, val, NULL);
}

/* ================================================================== */
/* RMW（Read-Modify-Write）EA ヘルパー                                */
/* ================================================================== */

/*
 * RMW 命令（ADDQ/NEG/AND Dn,EA 等）で EA を一度だけフェッチするためのヘルパー。
 * ea_read と ea_write を独立に呼ぶと、拡張ワード（mode 5/6/7）を
 * それぞれが PC から読んでしまい、2 回目は次命令のバイトを誤読する。
 * ea_rmw_read でアドレスを *addr_out に保存し、ea_rmw_write で再利用する。
 * mode 0（Dn）/ mode 1（An）は UINT32_MAX を返す（レジスタ直接）。
 */
uint32_t ea_rmw_addr(mc68k_state_t *cpu, int mode, int reg, int size) {
    uint32_t addr;
    ea_desc_init(cpu, mode, reg, size);
    switch (mode) {
    case 2:
        return cpu->a[reg];
    case 3:
        addr = cpu->a[reg];
        cpu->a[reg] += (uint32_t)sp_byte_step(reg, size);
        return addr;
    case 4:
        cpu->a[reg] -= (uint32_t)sp_byte_step(reg, size);
        return cpu->a[reg];
    case 5: {
        int16_t d = (int16_t)cpu_fetch_word(cpu);
        if (cpu->exec_state != CPU_NORMAL)
            return 0;
        return cpu->a[reg] + (uint32_t)(int32_t)d;
    }
    case 6:
        addr = indexed_ea(cpu, cpu->a[reg], &cpu->last_ea);
        return cpu->exec_state == CPU_NORMAL ? addr : 0;
    default: /* mode 7 (#imm を除く) */
        addr = ea_get_addr(cpu, mode, reg);
        /* ea_get_addr() は制御 EA ヘルパーであり、既定では word サイズを記録する。
         * この RMW 呼出し側は命令の実際のオペランドサイズを持ち、EA
         * タイミング表はそのサイズを使う。 */
        cpu->last_ea.size = (uint8_t)size;
        return cpu->exec_state == CPU_NORMAL ? addr : 0;
    }
}

uint32_t ea_rmw_read(mc68k_state_t *cpu, int mode, int reg, int size, uint32_t *addr_out,
                     bool locked) {
    if (mode == 0 || mode == 1) {
        ea_desc_init(cpu, mode, reg, size);
        *addr_out = UINT32_MAX;
        return mode == 0 ? cpu->d[reg] & size_mask(size) : cpu->a[reg];
    }
    if (mode == 7 && reg == 4) {
        /* #imm: ea_get_addr()は制御EA（JMP/JSR/LEA/PEA/MOVEM）専用で
         * 即値を正しく非サポートとするが、BTST等のデータ読み取りでは
         * 即値ソースは正当（ea_read()と同じ扱い）。書き戻し不要な点も
         * mode 0/1 と同じなので addr_out は UINT32_MAX にしておく。 */
        ea_desc_init(cpu, mode, reg, size);
        *addr_out = UINT32_MAX;
        if (size == 4) {
            uint32_t hi = cpu_fetch_word(cpu);
            if (cpu->exec_state != CPU_NORMAL)
                return 0;
            uint32_t lo = cpu_fetch_word(cpu);
            return (hi << 16) | lo;
        }
        return cpu_fetch_word(cpu) & size_mask(size);
    }
    *addr_out = ea_rmw_addr(cpu, mode, reg, size);
    if (cpu->exec_state != CPU_NORMAL)
        return 0;
    return mem_rmw_read_sized(cpu, *addr_out, size, locked);
}

uint32_t ea_rmw_load(mc68k_state_t *cpu, int size, uint32_t addr, bool locked) {
    return mem_rmw_read_sized(cpu, addr, size, locked);
}

void ea_rmw_write(mc68k_state_t *cpu, int mode, int reg, int size, uint32_t addr, uint32_t val,
                  bool locked) {
    if (mode == 0) {
        uint32_t mask = size_mask(size);
        cpu->d[reg] = (cpu->d[reg] & ~mask) | (val & mask);
    } else if (mode == 1) {
        cpu->a[reg] = val;
    } else {
        mem_rmw_write_sized(cpu, addr, size, val, locked);
    }
}

/* ================================================================== */
/* EA カテゴリ判定（: 無効な宛先/ソースEAをデコード時に弾く）  */
/* ================================================================== */

/*
 * Motorola分類（Data/Memory/Control × Alterable）に基づくEAカテゴリ判定。
 * 引数 ea は mode<<3|reg（0-63）。各 register_*_ops() のテーブル登録ループから
 * 呼び、無効な組み合わせは登録をスキップして ILLEGAL のまま残すために使う。
 *
 * mode 7 の reg 5-7 は未割り当て（実機は例外なく無効）のため、
 * 以下すべての関数で自動的に false になる。
 */

/* Data Alterable: Dn、(An)、(An)+、-(An)、d16(An)、d8(An,Xn)、abs.W、abs.L。 */
int ea_is_data_alterable(int ea) {
    int mode = (ea >> 3) & 7;
    if (mode == 1)
        return 0;
    if (mode == 7)
        return (ea & 7) <= 1;
    return 1;
}

/* Memory Alterable: Data Alterable から Dn も除外 */
int ea_is_memory_alterable(int ea) {
    int mode = (ea >> 3) & 7;
    if (mode == 0 || mode == 1)
        return 0;
    if (mode == 7)
        return (ea & 7) <= 1;
    return 1;
}

/* Alterable: Dn、An を含む書込み可能な全モード（PC 相対／immediate のみ除外）。 */
int ea_is_alterable(int ea) {
    int mode = (ea >> 3) & 7;
    if (mode == 7)
        return (ea & 7) <= 1;
    return 1;
}

/* Control: (An)、d16(An)、d8(An,Xn)、abs.W、abs.L、d16(PC)、d8(PC,Xn)。 */
int ea_is_control(int ea) {
    int mode = (ea >> 3) & 7;
    if (mode == 2 || mode == 5 || mode == 6)
        return 1;
    if (mode == 7)
        return (ea & 7) <= 3;
    return 0;
}

/* Control Alterable: Control から PC相対を除外（MOVEM reg→mem 用） */
int ea_is_control_alterable(int ea) {
    int mode = (ea >> 3) & 7;
    if (mode == 2 || mode == 5 || mode == 6)
        return 1;
    if (mode == 7)
        return (ea & 7) <= 1;
    return 0;
}

/* Data: An除く読み取り全般（immediate含む） */
int ea_is_data(int ea) {
    int mode = (ea >> 3) & 7;
    if (mode == 1)
        return 0;
    if (mode == 7)
        return (ea & 7) <= 4;
    return 1;
}

/* Data + An直接: ADD/SUB/CMP の EA→Dn word/long 方向専用。
 * ea_is_data から An 直接（mode 1）除外を外したもの。byte サイズのみ
 * An 不可というサイズ依存の特例があるため ea_is_data とは別関数にする。 */
int ea_is_data_or_an(int ea) {
    int mode = (ea >> 3) & 7;
    if (mode == 7)
        return (ea & 7) <= 4;
    return 1;
}

/* ================================================================== */
/* 制御 EA のアドレス計算（JMP/JSR/LEA/PEA/MOVEM で使用）              */
/* ================================================================== */

/*
 * 制御 EA の実効アドレス計算（メモリを読まず、アドレスだけを返す）。
 * データ EA と異なり、Dn/An 直接モードは無効。
 */
uint32_t ea_get_addr(mc68k_state_t *cpu, int mode, int reg) {
    ea_desc_init(cpu, mode, reg, 2);
    switch (mode) {
    case 2: /* (An) */
        return cpu->a[reg];
    case 5: { /* d16(An) */
        int16_t d = (int16_t)cpu_fetch_word(cpu);
        if (cpu->exec_state != CPU_NORMAL)
            return 0;
        return cpu->a[reg] + (int32_t)d;
    }
    case 6: /* d8(An,Xn) */
        return indexed_ea(cpu, cpu->a[reg], &cpu->last_ea);
    case 7:
        switch (reg) {
        case 0: /* xxx.W: 16bit 符号拡張 */
            return (uint32_t)(int32_t)(int16_t)cpu_fetch_word(cpu);
        case 1: { /* xxx.L: 32bit */
            uint32_t h = cpu_fetch_word(cpu);
            if (cpu->exec_state != CPU_NORMAL)
                return 0;
            uint32_t l = cpu_fetch_word(cpu);
            return (h << 16) | l;
        }
        case 2: { /* d16(PC) */
            uint32_t b = cpu->pc;
            int16_t d = (int16_t)cpu_fetch_word(cpu);
            if (cpu->exec_state != CPU_NORMAL)
                return 0;
            return b + (int32_t)d;
        }
        case 3: /* d8(PC,Xn) */
            return indexed_ea(cpu, cpu->pc, &cpu->last_ea);
        default:
            cpu->exec_state = CPU_EXCEPTION;
            return 0;
        }
    default:
        cpu->exec_state = CPU_EXCEPTION;
        return 0;
    }
}
