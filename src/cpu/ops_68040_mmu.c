/* MC68040 MMU: 040 は固有のレジスタ、ディスクリプタ、オペコード空間を持つ。
 * M68040UM §3.1--§3.7。MC68030 PMMU 実装とは共有してはならない。 */
#include "cpu_internal.h"

#define TC_E  UINT16_C(0x8000)
#define TC_P  UINT16_C(0x4000)
#define TTR_E UINT32_C(0x00008000)
#define TTR_W UINT32_C(0x00000004)

#define MMUSR_B  UINT32_C(0x00000800)
#define MMUSR_G  UINT32_C(0x00000400)
#define MMUSR_U1 UINT32_C(0x00000200)
#define MMUSR_U0 UINT32_C(0x00000100)
#define MMUSR_S  UINT32_C(0x00000080)
#define MMUSR_CM UINT32_C(0x00000060)
#define MMUSR_M  UINT32_C(0x00000010)
#define MMUSR_W  UINT32_C(0x00000004)
#define MMUSR_T  UINT32_C(0x00000002)
#define MMUSR_R  UINT32_C(0x00000001)

#define DESC_U UINT32_C(0x08)
#define DESC_M UINT32_C(0x10)

/* テーブル検索の読出しは D-cache ヒットを使うがラインは割り当てない。
 * デバッグ用 walk はサイクルを計上せず、D-cache と整合するディスクリプタを読む。 */
static bool phys_read_long_040_mode(mc68k_state_t *cpu, uint32_t addr, uint32_t *out,
                                    mc68k_translate_mode_t mode) {
    if (mode != MC68K_TRANSLATE_DEBUG)
        return cpu_040_table_read(cpu, addr, out);
    if (!cpu_bus_read_long(cpu, addr, out))
        return false;
    for (unsigned i = 0; i < 4; i++) {
        uint8_t b;
        if (cpu_040_cache_peek(cpu, addr + i, &b)) {
            unsigned shift = (3u - i) * 8u;
            *out = (*out & ~(UINT32_C(0xff) << shift)) | ((uint32_t)b << shift);
        }
    }
    return true;
}
static bool ttr_match(uint32_t ttr, uint32_t logical, const mc68k_access_t *a) {
    if (!(ttr & TTR_E))
        return false;
    if (((logical >> 24) & ~((ttr >> 16) & 0xffu)) != ((ttr >> 24) & ~((ttr >> 16) & 0xffu)))
        return false;
    unsigned s = (ttr >> 13) & 3u;
    return s >= 2 || s == ((a->fc >> 2) & 1u);
}
static mc68k_atc_entry_t *atc040_lookup(mc68k_state_t *cpu, uint32_t logical,
                                        const mc68k_access_t *a) {
    unsigned ps = (cpu->mmu040_tc & TC_P) ? 13 : 12;
    uint32_t page = logical & ~((UINT32_C(1) << ps) - 1u);
    mc68k_atc_entry_t *atc = a->instruction ? cpu->atc040_i : cpu->atc040_d;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        if (atc[i].valid && atc[i].fc == a->fc && atc[i].page_shift == ps &&
            atc[i].logical_page == page)
            return &atc[i];
    return NULL;
}
static mc68k_atc_entry_t *atc040_alloc(mc68k_state_t *cpu, const mc68k_access_t *a) {
    mc68k_atc_entry_t *atc = a->instruction ? cpu->atc040_i : cpu->atc040_d, *best = &atc[0];
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++) {
        if (!atc[i].valid)
            return &atc[i];
        if (atc[i].history < best->history)
            best = &atc[i];
    }
    return best;
}
static void atc040_flush(mc68k_state_t *cpu, bool nonglobal, bool by_addr, uint32_t logical) {
    unsigned ps = (cpu->mmu040_tc & TC_P) ? 13 : 12;
    uint32_t page = logical & ~((UINT32_C(1) << ps) - 1u);
    for (unsigned bank = 0; bank < 2; bank++)
        for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++) {
            mc68k_atc_entry_t *e = bank ? &cpu->atc040_d[i] : &cpu->atc040_i[i];
            if (e->valid && (!nonglobal || !e->global) && (!by_addr || e->logical_page == page))
                e->valid = false;
        }
}

static void atc040_flush_access(mc68k_state_t *cpu, const mc68k_access_t *a, uint32_t logical) {
    unsigned ps = (cpu->mmu040_tc & TC_P) ? 13 : 12;
    uint32_t page = logical & ~((UINT32_C(1) << ps) - 1u);
    mc68k_atc_entry_t *atc = a->instruction ? cpu->atc040_i : cpu->atc040_d;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        if (atc[i].valid && atc[i].logical_page == page && atc[i].fc == a->fc)
            atc[i].valid = false;
}

mc68k_translate_result_t cpu_mmu040_translate_result(mc68k_state_t *cpu, uint32_t logical,
                                                     const mc68k_access_t *a,
                                                     mc68k_translate_mode_t mode) {
    mc68k_translate_result_t r = {
        .ok = true, .source = MC68K_TRANSLATE_IDENTITY, .physical = logical};
    /* 040UM §3.1.2/§3.3: 読出し・変更・書込みアクセス（TAS/CAS/CAS2）の読出し部は、
     * U/M 更新、W ビット、TTR 書込み保護判定では書込みとして扱う。 */
    const bool wr = !a->read || a->rmw;
    uint32_t t0 = a->instruction ? cpu->mmu040_itt0 : cpu->mmu040_dtt0;
    uint32_t t1 = a->instruction ? cpu->mmu040_itt1 : cpu->mmu040_dtt1;
    uint32_t ttr = ttr_match(t0, logical, a) ? t0 : ttr_match(t1, logical, a) ? t1 : 0;
    if (ttr) {
        r.source = MC68K_TRANSLATE_TT;
        r.cache_inhibit = ((ttr >> 5) & 3u) >= 2;
        r.cache_mode = (uint8_t)((ttr >> 5) & 3u);
        r.write_protect = (ttr & TTR_W) != 0;
        r.mmusr = MMUSR_T | MMUSR_R;
        if (wr && r.write_protect) {
            r.ok = false;
            r.fault = MMU_FAULT_WRITE_PROTECT;
        }
        return r;
    }
    if (!(cpu->mmu040_tc & TC_E))
        return r;
    mc68k_atc_entry_t *hit = atc040_lookup(cpu, logical, a);
    unsigned ps = (cpu->mmu040_tc & TC_P) ? 13 : 12;
    uint32_t mask = (UINT32_C(1) << ps) - 1u;
    if (hit) {
        /* write は leaf M bit の設定を要する。先行する read が埋めた ATC entry では、
         * descriptor を再訪せずに設定できない。R-clear entry（walk 失敗）は table search
         * を行わず retry を中止する（040UM §3.3）。 */
        if (wr && !hit->modified && !hit->bus_error && mode != MC68K_TRANSLATE_DEBUG)
            hit->valid = false;
        else {
            r.source = MC68K_TRANSLATE_ATC;
            r.physical = hit->physical_page | (logical & mask);
            r.cache_inhibit = hit->cache_inhibit;
            r.cache_mode = hit->cache_mode;
            r.write_protect = hit->write_protect;
            r.modified = hit->modified;
            if (hit->bus_error) {
                r.ok = false;
                r.fault = (mmu_fault_t)hit->fault_reason;
                return r;
            }
            if (mode != MC68K_TRANSLATE_DEBUG)
                hit->history = ++cpu->atc040_clock;
            if (wr && hit->write_protect) {
                r.ok = false;
                r.fault = MMU_FAULT_WRITE_PROTECT;
            }
            return r;
        }
    }
    r.source = MC68K_TRANSLATE_WALK;
    uint32_t table = ((a->fc & 4u) ? cpu->mmu040_srp : cpu->mmu040_urp) & ~UINT32_C(0x1ff);
    unsigned indices[3] = {logical >> 25, (logical >> 18) & 0x7f,
                           ps == 12 ? (logical >> 12) & 0x3f : (logical >> 13) & 0x1f};
    bool wp = false, wp_fault = false;
    uint32_t d = 0, da = 0;
    /* ルート／ポインタディスクリプタの U 書込みは次レベルの読出し後に発行するため
     * （Fig 3-10）、ここで保留する。 */
    bool u_pending = false;
    uint32_t u_addr = 0, u_value = 0;
    for (unsigned level = 0; level < 3; level++) {
        da = table + indices[level] * 4u;
        if (!phys_read_long_040_mode(cpu, da, &d, mode)) {
            /* Fig 3-10 はバス転送を判定する前に予定済み U 書込みを実行するため、
             * 読出し失敗時も発行する。報告するのは失敗した読出しである。 */
            if (u_pending)
                cpu_040_table_write(cpu, u_addr, u_value);
            r.ok = false;
            r.fault = MMU_FAULT_TABLE_BUS_ERROR;
            r.mmusr = MMUSR_B;
            r.table_bus_error_physical = da;
            break; /* Fig 3-10 の OTHERWISE: 下で R クリアの ATC エントリを作成する */
        }
        if (u_pending) {
            u_pending = false;
            if (!cpu_040_table_write(cpu, u_addr, u_value)) {
                r.ok = false;
                r.fault = MMU_FAULT_TABLE_BUS_ERROR;
                r.mmusr = MMUSR_B;
                r.table_bus_error_physical = u_addr;
                break; /* Fig 3-10 の OTHERWISE: 下で R クリアの ATC エントリを作成する */
            }
        }
        r.level = level + 1;
        r.descriptor_physical = da;
        if (level < 2) {
            if ((d & 3u) < 2) {
                r.ok = false;
                r.fault = MMU_FAULT_INVALID;
                break;
            }
            wp |= (d & 4u) != 0;
            if (!(d & DESC_U) && mode != MC68K_TRANSLATE_DEBUG) {
                u_pending = true;
                u_addr = da;
                u_value = d | DESC_U;
            }
            table = d & ~(level == 0 ? UINT32_C(0x1ff)
                                     : (ps == 12 ? UINT32_C(0xff) : UINT32_C(0x7f)));
            continue;
        }
        if ((d & 3u) == 2) {
            da = d & ~UINT32_C(3);
            if (!phys_read_long_040_mode(cpu, da, &d, mode)) {
                r.ok = false;
                r.fault = MMU_FAULT_TABLE_BUS_ERROR;
                r.mmusr = MMUSR_B;
                r.table_bus_error_physical = da;
                break; /* Fig 3-10 の OTHERWISE: 下で R クリアの ATC エントリを作成する */
            }
            r.descriptor_physical = da;
        }
        if ((d & 3u) != 1 && (d & 3u) != 3) {
            r.ok = false;
            r.fault = MMU_FAULT_INVALID;
            break;
        }
        wp |= (d & 4u) != 0;
        if ((d & 0x80u) && !(a->fc & 4u)) {
            r.ok = false;
            r.fault = MMU_FAULT_SUPERVISOR;
            break;
        }
        /* 表 3-1: 書込み保護ページへの書込みも U は更新する（M は更新しない）。
         * その後 fault にする。 */
        wp_fault = wr && wp;
        const bool set_m = wr && !wp_fault;
        if (!(d & DESC_U) || (set_m && !(d & DESC_M))) {
            uint32_t nd = d | DESC_U | (set_m ? DESC_M : 0);
            if (mode != MC68K_TRANSLATE_DEBUG) {
                /* M を維持する U=0 の場合、または M がすでに立つ場合だけロック RMW が必要。 */
                const bool rmw = !(d & DESC_U) && (!set_m || (d & DESC_M));
                uint32_t failed = da;
                if (!(rmw ? cpu_040_table_set_bits(cpu, da, DESC_U, &nd, &failed)
                          : cpu_040_table_write(cpu, da, nd))) {
                    r.ok = false;
                    r.fault = MMU_FAULT_TABLE_BUS_ERROR;
                    r.mmusr = MMUSR_B;
                    r.table_bus_error_physical = failed;
                    break; /* Fig 3-10 の OTHERWISE: 下で R クリアの ATC エントリを作成する */
                }
            }
            d = nd;
        }
        r.physical = (d & ~mask) | (logical & mask);
        r.cache_mode = (uint8_t)((d >> 5) & 3u);
        r.cache_inhibit = r.cache_mode >= 2;
        r.write_protect = wp;
        r.modified = (d & DESC_M) != 0;
        r.mmusr = (r.physical & ~mask) | MMUSR_R |
                  (d & (MMUSR_G | MMUSR_U1 | MMUSR_U0 | MMUSR_S | MMUSR_CM | MMUSR_M)) |
                  (wp ? MMUSR_W : 0);
        if (wp_fault) {
            r.ok = false;
            r.fault = MMU_FAULT_WRITE_PROTECT;
        }
    }
    if (r.fault == MMU_FAULT_TABLE_BUS_ERROR)
        r.mmusr = MMUSR_B;
    if (mode != MC68K_TRANSLATE_DEBUG) {
        mc68k_atc_entry_t *e = atc040_alloc(cpu, a);
        *e = (mc68k_atc_entry_t){
            .logical_page = logical & ~mask,
            .physical_page = r.physical & ~mask,
            .fc = a->fc,
            .page_shift = ps,
            .valid = true,
            .bus_error =
                !r.ok &&
                r.fault != MMU_FAULT_WRITE_PROTECT, /* WP フォルトでは有効なエントリを残す */
            .cache_inhibit = r.cache_inhibit,
            .cache_mode = r.cache_mode,
            .write_protect = r.write_protect,
            .modified = r.modified,
            .global = (r.mmusr & MMUSR_G) != 0,
            .fault_reason = (uint8_t)r.fault,
            .history = ++cpu->atc040_clock,
        };
    }
    return r;
}

static int op_040_pflush(mc68k_state_t *cpu, uint16_t op) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    bool all = (op & 0x10u) != 0;
    bool nonglobal = (op & 0x08u) == 0;
    atc040_flush(cpu, nonglobal, !all, all ? 0 : cpu->a[op & 7]);
    /* 040UM 表 10-5: PFLUSH/PFLUSHA/PFLUSHN は計算 11 + 実行基準値 10、
     * PFLUSHAN は計算 27 + 実行基準値 26。これらの典型的な基準値には動的な
     * テーブル検索／バス処理を含めない。 */
    return cpu_040_tag(cpu, all && nonglobal ? 53 : 21);
}

static int op_040_ptest(mc68k_state_t *cpu, uint16_t op) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    const mc68k_access_t access = {
        .fc = cpu->dfc,
        .read = (op & 0x20u) != 0,
        .size = 1,
        .instruction = (cpu->dfc & 3u) == 2u,
    };
    uint32_t logical = cpu->a[op & 7];
    atc040_flush_access(cpu, &access, logical);
    mc68k_translate_result_t r =
        cpu_mmu040_translate_result(cpu, logical, &access, MC68K_TRANSLATE_PLOAD);
    cpu->mmu040_mmusr = r.mmusr;
    return 25;
}

void register_68040_mmu_ops(op_handler_t *table) {
    for (unsigned reg = 0; reg < 8; reg++) {
        table[0xf500u | reg] = op_040_pflush; /* PFLUSHN (An) */
        table[0xf508u | reg] = op_040_pflush; /* PFLUSH (An) */
        table[0xf548u | reg] = op_040_ptest;  /* PTESTW (An) */
        table[0xf568u | reg] = op_040_ptest;  /* PTESTR (An) */
    }
    for (unsigned reserved = 0; reserved < 8; reserved++) {
        table[0xf510u | reserved] = op_040_pflush; /* PFLUSHAN */
        table[0xf518u | reserved] = op_040_pflush; /* PFLUSHA */
    }
}
