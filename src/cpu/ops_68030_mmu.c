/* MC68EC030 のアクセス制御 PMOVE/PTEST 命令と MC68030 PMMU。
 * EC030 では klass-1/klass-2 群（PFLUSHA/PFLUSH/PLOAD/PMOVE TC,CRP,SRP）を
 * F-line 例外にせず無害な NOP として受理する。X68030 IPLROM の CPU 世代検査は
 * PMMU の有無を検出するため意図的にこれらを実行し、オンチップ PMMU を持たない実機 EC030 と
 * 同様に正常完了することを期待する。XEiJ の MC68EC030.irpPgen() も同じ NOP 処理を行う。 */
#include "cpu_internal.h"

/* 命令別・walk段数別の精密 timing はモデル化しない。 */
#define PMMU_NOMINAL_CYCLES 12

/* MMUSR bit assignments ([UM30 §9.7.4]).  Bits 12/8/7/5-3 are 68851-only and
 * always read as zero on the 68030. */
#define MMUSR_B 0x8000u /* bus error during the table search */
#define MMUSR_L 0x4000u /* limit violation */
#define MMUSR_S 0x2000u /* supervisor-only violation */
#define MMUSR_W 0x0800u /* a write-protected descriptor was traversed */
#define MMUSR_I 0x0400u /* invalid */
#define MMUSR_M 0x0200u /* modified */
#define MMUSR_T 0x0040u /* transparent (TT0/TT1) hit */
#define MMUSR_N 0x0007u /* number of table levels accessed */

/* atc_walk() searches every configured level unless PTEST caps the depth. */
#define MMU_WALK_ALL_LEVELS (~0u)

static int line_f(mc68k_state_t *cpu) {
    return cpu_dispatch_exception(cpu, cpu->instr_pc, 11, CYC(cpu, CYC_EXC_ILLEGAL));
}

static int mmu_config_error(mc68k_state_t *cpu) {
    return cpu_dispatch_exception(cpu, cpu->pc, 56, 34);
}

/* ページサイズとテーブル索引の整合性検査は E が立つときだけ適用する（[UM30 §9.7.5.3]）。
 * TC = 0 は OS が変換を無効にする方法である。PMMU は例外を起こした値を保持する。
 * これは意図的に PMOVE 後のトラップとする。 */
static bool tc_valid(uint32_t tc) {
    unsigned ps = (tc >> 20) & 15u, is = (tc >> 16) & 15u;
    if (!(tc & UINT32_C(0x80000000)))
        return true;
    if (ps < 8)
        return false;
    unsigned ti[4] = {(tc >> 12) & 15u, (tc >> 8) & 15u, (tc >> 4) & 15u, tc & 15u};
    if (ti[0] == 0 || (ti[1] == 0 && ti[0] < 2))
        return false;
    unsigned sum = is + ps;
    for (unsigned i = 0; i < 4 && ti[i]; ++i)
        sum += ti[i];
    return sum == 32;
}

static int function_code(mc68k_state_t *cpu, unsigned field, unsigned *out) {
    if ((field & 0x18u) == 0x10u) {
        *out = field & 7u;
        return 1;
    }
    if ((field & 0x18u) == 0x08u) {
        *out = cpu->d[field & 7u] & 7u;
        return 1;
    }
    if (field == 0) {
        *out = cpu->sfc;
        return 1;
    }
    if (field == 1) {
        *out = cpu->dfc;
        return 1;
    }
    return 0;
}

static bool ac_matches(uint32_t ac, uint32_t addr, const mc68k_access_t *access) {
    unsigned lab = ac >> 24, lam = (ac >> 16) & 0xffu;
    unsigned fcbase = (ac >> 4) & 7u, fcmask = ac & 7u;
    if (!(ac & UINT32_C(0x00008000)))
        return false;
    if (((addr >> 24) & ~lam) != (lab & ~lam))
        return false;
    if ((access->fc & ~fcmask) != (fcbase & ~fcmask))
        return false;
    /* RWM=0 makes an RMW sequence ineligible for transparent translation;
     * RWM=1 ignores the read/write selector altogether. */
    if (access->rmw && !(ac & UINT32_C(0x00000100)))
        return false;
    return (ac & UINT32_C(0x00000100)) || access->read == ((ac & UINT32_C(0x00000200)) != 0);
}

/* PMMU テーブルウォークは常に外部アクセスとして課金する。
 * データキャッシュで抑止しないよう cache_inhibit=true を渡す。 */
static bool phys_read_long(mc68k_state_t *cpu, uint32_t addr, uint32_t *out) {
    const mc68k_access_t access = cpu_supervisor_data_access(true, 4, false);
    bool ok = cpu_bus_read_long(cpu, addr, out);
    cpu_account_bus_cycle(cpu, addr, &access, true);
    return ok;
}
static bool phys_write_long(mc68k_state_t *cpu, uint32_t addr, uint32_t v) {
    const mc68k_access_t access = cpu_supervisor_data_access(false, 4, false);
    bool ok = cpu_bus_write_long(cpu, addr, v);
    cpu_account_bus_cycle(cpu, addr, &access, true);
    return ok;
}

/* EC030 の AC0/AC1 は 68030 の TT0/TT1 と同じビット配置を持ち、
 * cpu->mmu_tt0 / mmu_tt1 に格納されている（PMOVE の EC030 分岐、preg==1/3
 * 参照）。ac_matches() が E(bit15)・LAB/LAM・FC・R/W・RWM をすべて見るので、
 * ここは CI(bit10) を取り出すだけでよい。cpu_mmu_translate_result() は
 * MC68030 専用の早期リターンを持つため、その関数へ手を入れずこの専用関数を
 * 別に用意する。 */
bool cpu_mmu_ec030_cache_inhibit(mc68k_state_t *cpu, uint32_t logical,
                                 const mc68k_access_t *access) {
    if (!cpu_is_68ec030(cpu))
        return false;
    uint32_t ac = ac_matches(cpu->mmu_tt0, logical, access)   ? cpu->mmu_tt0
                  : ac_matches(cpu->mmu_tt1, logical, access) ? cpu->mmu_tt1
                                                              : 0;
    return ac != 0 && (ac & UINT32_C(0x00000400)) != 0;
}

static void atc_flush(mc68k_state_t *cpu) {
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        cpu->atc[i].valid = false;
}

static void atc_flush_match(mc68k_state_t *cpu, unsigned fc, unsigned mask, bool by_addr,
                            uint32_t logical) {
    unsigned ps = (cpu->mmu_tc >> 20) & 15u;
    uint32_t page_mask = (UINT32_C(1) << ps) - 1u;
    uint32_t logical_page = logical & ~page_mask;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++) {
        mc68k_atc_entry_t *e = &cpu->atc[i];
        if (!e->valid || ((e->fc ^ fc) & mask) != 0)
            continue;
        if (by_addr && e->logical_page != logical_page)
            continue;
        e->valid = false;
    }
}

/* ATC のキーは valid + FC + 登録時の TC.PS + 論理ページ。page_shift を含めるのは
 * TC.PS 変更直後に古いページサイズのエントリが誤ヒットしないようにするため。 */
static mc68k_atc_entry_t *atc_lookup(mc68k_state_t *cpu, uint32_t logical, unsigned fc) {
    unsigned ps = (cpu->mmu_tc >> 20) & 15u;
    uint32_t page = logical & ~((UINT32_C(1) << ps) - 1u);
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++) {
        mc68k_atc_entry_t *e = &cpu->atc[i];
        if (e->valid && e->fc == fc && e->page_shift == ps && e->logical_page == page)
            return e;
    }
    return NULL;
}

static mc68k_atc_entry_t *atc_alloc(mc68k_state_t *cpu) {
    mc68k_atc_entry_t *best = &cpu->atc[0];
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++) {
        if (!cpu->atc[i].valid)
            return &cpu->atc[i];
        if (cpu->atc[i].history < best->history)
            best = &cpu->atc[i];
    }
    return best;
}

/* 物理ディスクリプタアクセスは意図的に cpu_{read,write}_long を通さない。
 * テーブルディスクリプタを変換すると PMMU へ再帰するためである。 */
static bool walk_read_long(mc68k_state_t *cpu, uint32_t addr, mc68k_translate_result_t *result,
                           uint32_t *out) {
    if (phys_read_long(cpu, addr, out))
        return true;
    result->fault = MMU_FAULT_TABLE_BUS_ERROR;
    result->table_bus_error_physical = addr;
    return false;
}

static bool walk_write_long(mc68k_state_t *cpu, uint32_t addr, mc68k_translate_result_t *result,
                            uint32_t value) {
    if (phys_write_long(cpu, addr, value))
        return true;
    result->fault = MMU_FAULT_TABLE_BUS_ERROR;
    result->table_bus_error_physical = addr;
    return false;
}

/* 長形式記述子の LIMIT は「次段のインデックス欄」に対して適用する。L/U=1 なら
 * 下限（index >= LIMIT が合法）、0 なら上限（index <= LIMIT が合法）。早期終了
 * ページ記述子でも、検索が続いていれば使ったはずのインデックスで判定する
 * （`[UM30 §9.5.1.1]`）。 */
static bool limit_violation(uint32_t desc, uint32_t logical, unsigned consumed,
                            unsigned next_bits) {
    if (!next_bits)
        return false;
    unsigned index = (logical >> (32 - consumed - next_bits)) & ((1u << next_bits) - 1u);
    unsigned limit = (desc >> 16) & 0x7fffu;
    return (desc & UINT32_C(0x80000000)) ? index < limit : index > limit;
}

/* 長形式記述子の S は「FC2 が立っていないアクセス」を拒否する。判定は実行中の
 * SR ではなく変換対象の FC で行う。 */
static bool supervisor_violation(uint32_t desc, const mc68k_access_t *access) {
    return (desc & UINT32_C(0x100)) != 0 && !(access->fc & 4u);
}

/* max_levels は PTEST の LEVEL による探索打ち切り。上限に達しただけの停止は
 * フォルトではなく、result->level（＝MMUSR.N）に到達段数を残して true を返す。
 * その場合 result->physical は未確定なので MMUSR の組み立てにだけ使う。
 *
 * 戻り値 false は「検索そのものが失敗した」（invalid / limit / supervisor /
 * table bus error）ことを表し、負 ATC エントリの対象になる。write protect だけは
 * 検索が成立したうえでの保護違反なので true を返し、呼び出し側が ok を落とす。 */
static bool atc_walk(mc68k_state_t *cpu, uint32_t logical, const mc68k_access_t *access,
                     bool update_um, unsigned max_levels, mc68k_translate_result_t *result) {
    uint32_t tc = cpu->mmu_tc;
    unsigned ps = (tc >> 20) & 15u, shift = (tc >> 16) & 15u;
    unsigned widths[4] = {(tc >> 12) & 15u, (tc >> 8) & 15u, (tc >> 4) & 15u, tc & 15u};
    bool fcl = (tc & UINT32_C(0x01000000)) != 0;
    /* SRE selects SRP from the access being translated (FC2), never from
     * the processor's current supervisor bit.  PLOAD/PTEST provide FC
     * explicitly, so using SR here is observably wrong. */
    uint64_t rp =
        ((tc & UINT32_C(0x02000000)) && (access->fc & 4u)) ? cpu->mmu_srp : cpu->mmu_crp;
    uint32_t desc = (uint32_t)(rp >> 32), table = (uint32_t)rp;
    unsigned consumed = shift;
    uint32_t attr = 0;
    bool inherited_wp = false;
    /* ルートポインタ自身が早期終了ページディスクリプタであり得る。アドレス欄は下位 long、
     * 属性と DT は上位 long にある。使用済みにするメモリディスクリプタはない。 */
    if ((desc & 3u) == 1) {
        if (!fcl && limit_violation(desc, logical, shift, widths[0])) {
            result->fault = MMU_FAULT_LIMIT;
            return false;
        }
        if (supervisor_violation(desc, access)) {
            result->fault = MMU_FAULT_SUPERVISOR;
            return false;
        }
        uint32_t offset_mask = shift ? (UINT32_MAX >> shift) : UINT32_MAX;
        result->physical =
            (table & ~(((UINT32_C(1) << ps) - 1u) & offset_mask)) | (logical & offset_mask);
        result->cache_inhibit = (desc & UINT32_C(0x40)) != 0;
        result->write_protect = (desc & 4u) != 0;
        result->modified = (desc & 0x10u) != 0;
        result->level = 0;
        if (!access->read && result->write_protect)
            result->fault = MMU_FAULT_WRITE_PROTECT;
        return true;
    }
    for (unsigned level = 0; level < 4 && widths[level] && level < max_levels; level++) {
        unsigned bits = widths[level];
        unsigned index = (logical >> (32 - consumed - bits)) & ((1u << bits) - 1u);
        /* FCL substitutes FC for the first lookup index.  It also makes the
         * root LIMIT field inapplicable; later table levels are unchanged. */
        bool fcl_root = level == 0 && fcl;
        if (fcl_root)
            index = access->fc;
        bool long_form = (desc & 3u) == 3;
        /* ここで保護検査に達するのはルートポインタだけである。以後の各ディスクリプタ読出しは、
         * 読んだ直後に検査する。 */
        if (long_form) {
            if (!fcl_root && limit_violation(desc, logical, consumed, bits)) {
                result->fault = MMU_FAULT_LIMIT;
                return false;
            }
            if (supervisor_violation(desc, access)) {
                result->fault = MMU_FAULT_SUPERVISOR;
                return false;
            }
        } else if ((desc & 3u) != 2) {
            result->fault = MMU_FAULT_INVALID;
            return false;
        }
        inherited_wp |= (desc & 4u) != 0;
        uint32_t entry_addr = (table & ~UINT32_C(0xf)) + index * (long_form ? 8u : 4u);
        uint32_t hi = 0, lo = 0;
        if (long_form) {
            if (!walk_read_long(cpu, entry_addr, result, &hi) ||
                !walk_read_long(cpu, entry_addr + 4, result, &lo))
                return false;
            desc = hi;
            table = lo;
        } else {
            if (!walk_read_long(cpu, entry_addr, result, &lo))
                return false;
            desc = table = lo;
        }
        /* この段の記述子を読んだ時点で「アクセスした段数」が確定する。A/An は
         * 最後に読んだ記述子の物理アドレスなので、失敗経路でも残す。 */
        result->level = (uint8_t)(level + 1);
        result->descriptor_physical = entry_addr;
        /* 保護は U を立てる前に判定する。スーパーバイザ違反を検出した記述子には
         * U も M も書かない。 */
        if (long_form && supervisor_violation(desc, access)) {
            result->fault = MMU_FAULT_SUPERVISOR;
            return false;
        }
        /* U is set on every descriptor traversed, not only on the final
         * page descriptor.  Descriptor attributes live in the first long
         * in both short and long forms. */
        if (update_um && !(desc & 8u)) {
            if (!walk_write_long(cpu, entry_addr, result, desc | 8u))
                return false;
            desc |= 8u;
        }
        attr = desc;
        consumed += bits;
        if ((attr & 3u) == 1) {
            /* long 形式ページディスクリプタに LIMIT 欄はない。持つのはルートポインタと
             * long 形式テーブルディスクリプタだけである（§7.8）。 */
            /* 早期終了時、検索で消費しなかった全論理ビットは、単に PS でなく
             * ディスクリプタオフセットの一部である。
             * それらの未使用インデックスビットは記述子のページアドレスへ
             * 「連結」される（[UM30 §9.5.1.2]）。実機は連結を OR で行うため、
             * 記述子側の該当ビットが 0 でない（＝ブロック境界に揃っていない）
             * テーブルでも記述子のアドレスがそのまま残る。ページアドレス欄を
             * ブロック幅で切り捨ててはならない: IPLROM30 の MMU 自己診断
             * （$FF0D92-）は 2MB ブロックに 0x00F00000 という非整列アドレスを
             * 置き、論理 $200000 が物理 $F00000（CGROM）を指すことを確かめる。 */
            uint32_t offset_mask = consumed ? (UINT32_MAX >> consumed) : UINT32_MAX;
            uint32_t base = (table != attr) ? table : attr;
            result->physical =
                (base & ~(((UINT32_C(1) << ps) - 1u) & offset_mask)) | (logical & offset_mask);
            result->cache_inhibit = (attr & UINT32_C(0x40)) != 0;
            result->write_protect = inherited_wp || (attr & 4u) != 0;
            result->modified = (attr & 0x10u) != 0;
            if (!access->read && result->write_protect) {
                result->fault = MMU_FAULT_WRITE_PROTECT;
                return true;
            }
            if (update_um && !access->read && !result->modified) {
                if (!walk_write_long(cpu, entry_addr, result, attr | 0x10u))
                    return false;
                result->modified = true;
            }
            return true;
        }
        if ((attr & 3u) != 2 && (attr & 3u) != 3) {
            result->fault = MMU_FAULT_INVALID;
            return false;
        }
        if (level + 1 == 4 || widths[level + 1] == 0) {
            /* 最終段では、テーブル形状のディスクリプタは間接ページディスクリプタである。
             * ディスクリプタ自身の形式で 1 回たどる。間接ディスクリプタは最終段を共有するため
             * N は進まず、報告するディスクリプタアドレスだけが変わる。 */
            bool indirect_long = (attr & 3u) == 3;
            uint32_t ia = (indirect_long ? table : attr) & ~UINT32_C(0xf);
            uint32_t ihi = 0, ilo = 0;
            if (indirect_long) {
                if (!walk_read_long(cpu, ia, result, &ihi) ||
                    !walk_read_long(cpu, ia + 4, result, &ilo))
                    return false;
                desc = ihi;
                table = ilo;
            } else {
                if (!walk_read_long(cpu, ia, result, &ilo))
                    return false;
                desc = table = ilo;
            }
            result->descriptor_physical = ia;
            if ((desc & 3u) != 1) {
                result->fault = MMU_FAULT_INVALID;
                return false;
            }
            if (indirect_long && supervisor_violation(desc, access)) {
                result->fault = MMU_FAULT_SUPERVISOR;
                return false;
            }
            if (update_um && !(desc & 8u)) {
                if (!walk_write_long(cpu, ia, result, desc | 8u))
                    return false;
                desc |= 8u;
            }
            uint32_t page_mask = (UINT32_C(1) << ps) - 1u;
            uint32_t base = table != desc ? table : desc;
            result->physical = (base & ~page_mask) | (logical & page_mask);
            result->cache_inhibit = (desc & UINT32_C(0x40)) != 0;
            result->write_protect = inherited_wp || (desc & 4u) != 0;
            result->modified = (desc & 0x10u) != 0;
            if (!access->read && result->write_protect) {
                result->fault = MMU_FAULT_WRITE_PROTECT;
                return true;
            }
            if (update_um && !access->read && !result->modified) {
                if (!walk_write_long(cpu, ia, result, desc | 0x10u))
                    return false;
                result->modified = true;
            }
            return true;
        }
    }
    /* Reaching PTEST's requested LEVEL is not a fault: the search simply
     * stopped before a page descriptor was found. */
    if (result->level >= max_levels)
        return true;
    result->fault = MMU_FAULT_INVALID;
    return false;
}

/*
 * PTEST reports the state of a lookup instead of performing one: it alters
 * neither the used/modified bits of the tables nor the ATC ([PM §6 PTEST]).
 * LEVEL 0 searches the transparent registers and the ATC only — a miss is
 * reported as invalid rather than resolved by a table search — while
 * LEVEL 1-7 search the tables only and stop after LEVEL descriptors.
 * このため各 MMUSR ビットの意味は 2 形式で異なる
 * ([UM30 表 9-3]).
 */
uint16_t cpu_mmu_ptest(mc68k_state_t *cpu, uint32_t logical, const mc68k_access_t *access,
                       unsigned level, uint32_t *last_descriptor) {
    *last_descriptor = 0;
    if (level == 0) {
        /* 透過ヒットでは他の全 MMUSR ビットが未定義となるため、単独で報告する。 */
        if (ac_matches(cpu->mmu_tt0, logical, access) ||
            ac_matches(cpu->mmu_tt1, logical, access))
            return MMUSR_T;
        const mc68k_atc_entry_t *e = atc_lookup(cpu, logical, access->fc);
        if (!e)
            return MMUSR_I;
        uint16_t mmusr = 0;
        /* 負エントリは失敗理由を保持し、ATC
         * のみの検索がテーブル再読出しなしで応答できるようにする。 */
        if (e->bus_error)
            mmusr |= MMUSR_B | MMUSR_I;
        if (e->write_protect)
            mmusr |= MMUSR_W;
        if (e->modified)
            mmusr |= MMUSR_M;
        return mmusr;
    }
    mc68k_translate_result_t result = {
        .ok = true,
        .source = MC68K_TRANSLATE_WALK,
        .physical = logical,
        .fault = MMU_FAULT_NONE,
    };
    (void)atc_walk(cpu, logical, access, false, level, &result);
    *last_descriptor = result.descriptor_physical;
    uint16_t mmusr = (uint16_t)(result.level & MMUSR_N);
    /* W and M describe a successful search too; they are not an encoding of
     * the failure reason. */
    if (result.write_protect)
        mmusr |= MMUSR_W;
    if (result.modified)
        mmusr |= MMUSR_M;
    switch (result.fault) {
    case MMU_FAULT_TABLE_BUS_ERROR:
        mmusr |= MMUSR_B | MMUSR_I;
        break;
    case MMU_FAULT_LIMIT:
        mmusr |= MMUSR_L | MMUSR_I;
        break;
    case MMU_FAULT_INVALID:
        mmusr |= MMUSR_I;
        break;
    case MMU_FAULT_SUPERVISOR:
        mmusr |= MMUSR_S;
        break;
    default:
        break;
    }
    return mmusr;
}

/* TT 変換は意図的に TC.E から独立させる。下記の一時的な互換ラッパと異なり、この関数は
 * exec_state や fault 欄を決して設定しない。失敗検索を CPU 例外にするかは呼出し側が決める。 */
mc68k_translate_result_t cpu_mmu_translate_result(mc68k_state_t *cpu, uint32_t logical,
                                                  const mc68k_access_t *access,
                                                  mc68k_translate_mode_t mode) {
    mc68k_translate_result_t result = {
        .ok = true,
        .source = MC68K_TRANSLATE_IDENTITY,
        .physical = logical,
        .fault = MMU_FAULT_NONE,
    };
    if (!cpu_is_68030(cpu))
        return result;
    uint32_t tt = ac_matches(cpu->mmu_tt0, logical, access)   ? cpu->mmu_tt0
                  : ac_matches(cpu->mmu_tt1, logical, access) ? cpu->mmu_tt1
                                                              : 0;
    if (tt) {
        result.source = MC68K_TRANSLATE_TT;
        result.cache_inhibit = (tt & UINT32_C(0x00000400)) != 0;
        result.mmusr = MMUSR_T;
        return result;
    }
    bool table_enabled = (cpu->mmu_tc & UINT32_C(0x80000000)) != 0;
    if (!table_enabled && mode != MC68K_TRANSLATE_PLOAD)
        return result;
    unsigned ps = (cpu->mmu_tc >> 20) & 15u;
    uint32_t mask = (UINT32_C(1) << ps) - 1u, page = logical & ~mask;
    mc68k_atc_entry_t *hit = atc_lookup(cpu, logical, access->fc);
    if (hit) {
        result.source = MC68K_TRANSLATE_ATC;
        if (mode != MC68K_TRANSLATE_DEBUG)
            hit->history = ++cpu->atc_clock;
        /* 読出しで埋めた M=0 エントリは最初の書込みを満たせない。walk は先に最終
         * ディスクリプタの M ビットを更新しなければならない。 */
        if (!access->read && !hit->modified) {
            hit->valid = false;
            hit = NULL;
        }
    }
    if (hit) {
        if (hit->bus_error) {
            result.ok = false;
            result.fault = (mmu_fault_t)hit->fault_reason;
            return result;
        }
        result.write_protect = hit->write_protect;
        result.modified = hit->modified;
        if (!access->read && hit->write_protect) {
            result.ok = false;
            result.fault = MMU_FAULT_WRITE_PROTECT;
            return result;
        }
        result.physical = hit->physical_page | (logical & mask);
        result.cache_inhibit = hit->cache_inhibit;
        return result;
    }
    result.source = MC68K_TRANSLATE_WALK;
    if (!atc_walk(cpu, logical, access, mode != MC68K_TRANSLATE_DEBUG, MMU_WALK_ALL_LEVELS,
                  &result)) {
        result.ok = false;
        mc68k_atc_entry_t *e = atc_alloc(cpu);
        if (mode != MC68K_TRANSLATE_DEBUG)
            *e = (mc68k_atc_entry_t){.logical_page = page,
                                     .fc = access->fc,
                                     .page_shift = ps,
                                     .valid = true,
                                     .bus_error = true,
                                     .fault_reason = (uint8_t)result.fault,
                                     .history = ++cpu->atc_clock};
        return result;
    }
    if (mode != MC68K_TRANSLATE_DEBUG) {
        mc68k_atc_entry_t *e = atc_alloc(cpu);
        *e = (mc68k_atc_entry_t){.logical_page = page,
                                 .physical_page = result.physical - (logical & mask),
                                 .fc = access->fc,
                                 .page_shift = ps,
                                 .valid = true,
                                 .cache_inhibit = result.cache_inhibit,
                                 .write_protect = result.fault == MMU_FAULT_WRITE_PROTECT,
                                 .modified = !access->read,
                                 .history = ++cpu->atc_clock};
    }
    if (result.fault == MMU_FAULT_WRITE_PROTECT)
        result.ok = false;
    return result;
}

static int op_68030_mmu(mc68k_state_t *cpu, uint16_t opcode) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);

    uint16_t ext = cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    if (!model_has_030_access_control(cpu->model))
        return line_f(cpu);

    unsigned klass = ext >> 13, preg = (ext >> 10) & 7u;
    unsigned ea = opcode & 0x3fu;
    /* PMOVE/PTEST are privileged data-space cycles, not instruction fetches. */
    const mc68k_access_t pmove_read = cpu_supervisor_data_access(true, 4, false);
    const mc68k_access_t pmove_write = cpu_supervisor_data_access(false, 4, false);
    if (cpu_is_68030(cpu) && klass == 0 && (ext & 0x00ffu) == 0 && (preg == 2 || preg == 3)) {
        if (!ea_is_control_alterable((int)ea))
            return line_f(cpu);
        uint32_t addr = ea_get_addr(cpu, ea >> 3, ea & 7);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t *tt = preg == 2 ? &cpu->mmu_tt0 : &cpu->mmu_tt1;
        if (ext & 0x0200u)
            cpu_write_long_access(cpu, addr, *tt, &pmove_write);
        else {
            *tt = cpu_read_long_access(cpu, addr, &pmove_read);
            if (!(ext & 0x0100u))
                atc_flush(cpu);
        }
        return cpu->exec_state == CPU_NORMAL ? PMMU_NOMINAL_CYCLES : 4;
    }
    if (cpu_is_68ec030(cpu) && klass == 0 && (ext & 0x01ffu) == 0 && (preg == 1 || preg == 3)) {
        if (!ea_is_control_alterable((int)ea))
            return line_f(cpu);
        uint32_t addr = ea_get_addr(cpu, ea >> 3, ea & 7);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t *ac = preg == 1 ? &cpu->mmu_tt0 : &cpu->mmu_tt1;
        if (ext & 0x0200u)
            cpu_write_long_access(cpu, addr, *ac, &pmove_write);
        else
            *ac = cpu_read_long_access(cpu, addr, &pmove_read);
        return cpu->exec_state == CPU_NORMAL ? PMMU_NOMINAL_CYCLES : 4;
    }
    if (klass == 2 && cpu_is_68030(cpu) && (ext & 0x00ffu) == 0 &&
        (preg == 0 || preg == 2 || preg == 3)) {
        if (!ea_is_control_alterable((int)ea))
            return line_f(cpu);
        uint32_t addr = ea_get_addr(cpu, ea >> 3, ea & 7);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        bool from = (ext & 0x0200u) != 0;
        if (preg == 0) {
            if (from)
                cpu_write_long_access(cpu, addr, cpu->mmu_tc, &pmove_write);
            else {
                cpu->mmu_tc = cpu_read_long_access(cpu, addr, &pmove_read);
                if (!(ext & 0x0100u))
                    atc_flush(cpu);
                if (cpu->exec_state == CPU_NORMAL && !tc_valid(cpu->mmu_tc)) {
                    cpu->mmu_tc &= ~UINT32_C(0x80000000);
                    return mmu_config_error(cpu);
                }
            }
        } else {
            /* CRP/SRP is a quad transfer: the EA is evaluated once and the two
             * longs move in order.  memory->register commits only once both
             * reads have succeeded; register->memory cannot undo the first
             * write, so a fault on the second leaves it in place. */
            uint64_t *rp = preg == 2 ? &cpu->mmu_srp : &cpu->mmu_crp;
            if (from) {
                cpu_write_long_access(cpu, addr, (uint32_t)(*rp >> 32), &pmove_write);
                if (cpu->exec_state == CPU_NORMAL)
                    cpu_write_long_access(cpu, addr + 4, (uint32_t)*rp, &pmove_write);
            } else {
                uint32_t hi = cpu_read_long_access(cpu, addr, &pmove_read);
                if (cpu->exec_state != CPU_NORMAL)
                    return 4;
                uint32_t lo = cpu_read_long_access(cpu, addr + 4, &pmove_read);
                if (cpu->exec_state != CPU_NORMAL)
                    return 4;
                *rp = ((uint64_t)hi << 32) | lo;
                if (!(ext & 0x0100u))
                    atc_flush(cpu);
                /* DT が無効なルートポインタは、レジスタ更新後に発生する設定エラーである。 */
                if (!(hi & 3u))
                    return mmu_config_error(cpu);
            }
        }
        return cpu->exec_state == CPU_NORMAL ? PMMU_NOMINAL_CYCLES : 4;
    }
    if (klass == 3 && preg == 0 && (ext & 0x01ffu) == 0) {
        if (!ea_is_control_alterable((int)ea))
            return line_f(cpu);
        uint32_t addr = ea_get_addr(cpu, ea >> 3, ea & 7);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        if (ext & 0x0200u) {
            const mc68k_access_t word_write = cpu_supervisor_data_access(false, 2, false);
            cpu_write_word_access(cpu, addr, cpu->mmu_sr, &word_write);
        } else {
            const mc68k_access_t word_read = cpu_supervisor_data_access(true, 2, false);
            cpu->mmu_sr = cpu_read_word_access(cpu, addr, &word_read);
        }
        return cpu->exec_state == CPU_NORMAL ? PMMU_NOMINAL_CYCLES : 4;
    }
    if (cpu_is_68030(cpu) && klass == 1) {
        unsigned mode = preg;
        /* PLOAD{R,W}; its EA supplies the logical address. */
        if (mode == 0) {
            if ((ext & 0x1e0u) != 0 || !function_code(cpu, ext & 0x1fu, &preg) ||
                !ea_is_control_alterable((int)ea))
                return line_f(cpu);
            uint32_t logical = ea_get_addr(cpu, ea >> 3, ea & 7);
            if (cpu->exec_state == CPU_NORMAL) {
                /* PLOAD replaces the matching ATC entry with a fresh lookup. */
                atc_flush_match(cpu, preg, 7, true, logical);
                const mc68k_access_t access = {
                    .fc = (uint8_t)preg,
                    .read = (ext & 0x0200u) != 0,
                };
                (void)cpu_mmu_translate_result(cpu, logical, &access, MC68K_TRANSLATE_PLOAD);
            }
            return PMMU_NOMINAL_CYCLES;
        }
        /* PFLUSHA has no EA (opcode F000 is consequently valid). */
        if (mode == 1 && (ext & 0x1ffu) == 0) {
            atc_flush(cpu);
            return PMMU_NOMINAL_CYCLES;
        }
        /* PFLUSH FC,MASK[,EA]. */
        if ((mode == 4 || mode == 6) && !(ext & 0x300u) &&
            function_code(cpu, ext & 0x1fu, &preg)) {
            uint32_t logical = 0;
            if (mode == 6) {
                if (!ea_is_control_alterable((int)ea))
                    return line_f(cpu);
                logical = ea_get_addr(cpu, ea >> 3, ea & 7);
                if (cpu->exec_state != CPU_NORMAL)
                    return 4;
            }
            atc_flush_match(cpu, preg, (ext >> 5) & 7u, mode == 6, logical);
            return PMMU_NOMINAL_CYCLES;
        }
        return line_f(cpu);
    }
    /* PTEST.  Both models decode the same extension word; only the reported
     * state differs, so they share one handler. */
    if (klass == 4) {
        bool ec = cpu_is_68ec030(cpu);
        unsigned level = preg, fc;
        bool read = (ext & 0x0200u) != 0, address_return = (ext & 0x0100u) != 0;
        unsigned areg = (ext >> 5) & 7u;
        /* EC030 には LEVEL も A/An もない。存在するのは AC 検索形式だけである。 */
        if (!function_code(cpu, ext & 0x1fu, &fc) || (ec && level != 0) ||
            (level == 0 && address_return) || (!address_return && areg != 0) ||
            !ea_is_control_alterable((int)ea))
            return line_f(cpu);
        uint32_t addr = ea_get_addr(cpu, ea >> 3, ea & 7);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        const mc68k_access_t access = {.fc = (uint8_t)fc, .read = read};
        if (ec) {
            /* ACUSR reports the transparent hit alone; every other bit keeps
             * whatever the program left there. */
            if (ac_matches(cpu->mmu_tt0, addr, &access) ||
                ac_matches(cpu->mmu_tt1, addr, &access))
                cpu->mmu_sr |= MMUSR_T;
            else
                cpu->mmu_sr &= (uint16_t)~MMUSR_T;
            return PMMU_NOMINAL_CYCLES;
        }
        uint32_t last_descriptor = 0;
        cpu->mmu_sr = cpu_mmu_ptest(cpu, addr, &access, level, &last_descriptor);
        /* A/An は LEVEL != 0 でだけ符号化でき、その形式は TT を参照しない。 */
        if (address_return)
            cpu->a[areg] = last_descriptor;
        return PMMU_NOMINAL_CYCLES;
    }

    /* klass 1 (PFLUSH/PFLUSHA/PLOAD) と klass 2 (PMOVE TC/CRP/SRP) は
     * 実PMMUを持たないEC030では意味を持たないため、拡張ワードだけ読み捨てて
     * 何もしないNOPとして受理する。実効アドレスが拡張ワードを
     * 要するモード（d16(An)/d8(An,Xn)/abs.W/abs.L/d16(PC)/d8(PC,Xn)）の場合
     * のみそれらを読み捨てる。Dn/An直接等ea_get_addr非対応モードは何も
     * 消費せずそのまま抜ける（ea_get_addr呼び出しはCPU_EXCEPTIONを誤って
     * セットしてしまうため不可）。それ以外の未定義の組み合わせ
     * （例: klass0でpreg∉{1,3}）は引き続きF-line。 */
    if (cpu_is_68ec030(cpu) && (klass == 1 || klass == 2)) {
        unsigned mode = ea >> 3, reg = ea & 7u;
        if (mode == 5 || mode == 6 || (mode == 7 && reg <= 3)) {
            (void)ea_get_addr(cpu, (int)mode, (int)reg);
            if (cpu->exec_state != CPU_NORMAL)
                return 4;
        }
        return PMMU_NOMINAL_CYCLES;
    }
    return line_f(cpu);
}

void register_68030_mmu_ops(op_handler_t *table) {
    for (int opcode = 0xf000; opcode <= 0xf03f; opcode++)
        table[opcode] = op_68030_mmu;
}
