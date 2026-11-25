/* MC68060 の MMU（設計書 docs/mc68000-internals/reference/cpu_68060.md §5）。
 * 040 の ATC・翻訳器とは共有しない。差分は次のとおり。
 *   - ATC の tag は FC2（user/supervisor）だけで、命令側とデータ側を別に持つ。
 *   - 失敗した table search は ATC に entry を作らない。
 *   - W=1 の entry への書込みは、M 更新の walk より先に fault とし entry を変えない。
 *   - TC.E=0 でも TC の既定属性（DCO/DWO/DCI）が適用される。
 *   - MMUSR・PTEST は無い。PLPA が戻り値でなく access error で失敗を報告する。 */
#include "cpu_internal.h"

#define TC_E               UINT32_C(0x00008000)
#define TC_P               UINT32_C(0x00004000)
#define TC_DCO_SHIFT       8
#define TC_DWO             UINT32_C(0x00000020)
#define TC_DCI_SHIFT       3
#define CACHE_MODE_MASK    3u
#define CACHE_MODE_INHIBIT 2u /* 10/11 = cache-inhibited */

#define TTR_E          UINT32_C(0x00008000)
#define TTR_W          UINT32_C(0x00000004)
#define TTR_BASE_SHIFT 24
#define TTR_MASK_SHIFT 16
#define TTR_FIELD_MASK 0xffu
#define TTR_S_SHIFT    13
#define TTR_CM_SHIFT   5
#define TTR_S_ANY      2u

#define FC_SUPERVISOR_BIT 4u

#define DESC_TYPE_MASK        3u
#define DESC_RESIDENT_PAGE    1u
#define DESC_INDIRECT         2u
#define DESC_RESIDENT_PAGE_B  3u
#define DESC_W                UINT32_C(0x04)
#define DESC_U                UINT32_C(0x08)
#define DESC_M                UINT32_C(0x10)
#define DESC_S                UINT32_C(0x80)
#define DESC_G                UINT32_C(0x400)
#define DESC_CM_SHIFT         5
#define ROOT_TABLE_ALIGN_MASK UINT32_C(0x1ff)
#define ROOT_LEVELS           3u

static bool ttr_match(uint32_t ttr, uint32_t logical, const mc68k_access_t *a) {
    if (!(ttr & TTR_E))
        return false;
    uint32_t ignore = (ttr >> TTR_MASK_SHIFT) & TTR_FIELD_MASK;
    if (((logical >> TTR_BASE_SHIFT) & ~ignore) !=
        (((ttr >> TTR_BASE_SHIFT) & TTR_FIELD_MASK) & ~ignore))
        return false;
    unsigned s = (ttr >> TTR_S_SHIFT) & 3u;
    return s >= TTR_S_ANY || s == ((a->fc >> 2) & 1u);
}

static unsigned page_shift(const mc68k_state_t *cpu) {
    return (cpu->mmu060_tc & TC_P) ? 13 : 12;
}

static mc68k_atc_entry_t *atc_bank(mc68k_state_t *cpu, bool instruction) {
    return instruction ? cpu->atc060_i : cpu->atc060_d;
}

static bool atc_entry_matches(const mc68k_atc_entry_t *e, const mc68k_access_t *a, unsigned ps,
                              uint32_t page) {
    return e->valid && (e->fc & FC_SUPERVISOR_BIT) == (a->fc & FC_SUPERVISOR_BIT) &&
           e->page_shift == ps && e->logical_page == page;
}

static mc68k_atc_entry_t *atc_lookup(mc68k_state_t *cpu, uint32_t logical,
                                     const mc68k_access_t *a, unsigned ps) {
    uint32_t page = logical & ~((UINT32_C(1) << ps) - 1u);
    mc68k_atc_entry_t *atc = atc_bank(cpu, a->instruction);
    uint8_t *last =
        &cpu->atc060_hint[a->instruction ? 1 : 0][(logical >> ps) & (MC68K_060_ATC_HINTS - 1u)];
    /* 同じ logical page 下位ビットで引いた直近ヒット slot を先に検証する。ヒントは常に実 entry
     * と照合するので、PFLUSH・slot 上書き・snapshot 復元で古くなっても無効化は不要。 */
    if (*last < MC68K_060_ATC_ENTRIES && atc_entry_matches(&atc[*last], a, ps, page))
        return &atc[*last];
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++)
        if (atc_entry_matches(&atc[i], a, ps, page)) {
            *last = (uint8_t)i;
            return &atc[i];
        }
    return NULL;
}

/* 命令フェッチ高速経路の状態を、今 ATC に載っている entry で更新する。 */
void cpu_mmu060_ifast_prime(mc68k_state_t *cpu, uint32_t logical, const mc68k_access_t *a) {
    const unsigned ps = page_shift(cpu);
    mc68k_atc_entry_t *e = atc_lookup(cpu, logical, a, ps);
    if (!e)
        return;
    cpu->atc060_ifast = (mc68k_atc060_ifast_t){
        .gen = cpu->ifast_gen,
        .logical_page = e->logical_page,
        .tc = cpu->mmu060_tc,
        .itt0 = cpu->mmu060_itt0,
        .itt1 = cpu->mmu060_itt1,
        .slot = (uint8_t)(e - cpu->atc060_i),
        .fc_s = (uint8_t)(e->fc & FC_SUPERVISOR_BIT),
        .page_shift = (uint8_t)ps,
        .valid = true,
    };
}

static mc68k_atc_entry_t *atc_alloc(mc68k_state_t *cpu, const mc68k_access_t *a) {
    mc68k_atc_entry_t *atc = atc_bank(cpu, a->instruction), *best = &atc[0];
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++) {
        if (!atc[i].valid)
            return &atc[i];
        if (atc[i].history < best->history)
            best = &atc[i];
    }
    return best;
}

static bool table_read(mc68k_state_t *cpu, uint32_t physical, uint32_t *out,
                       mc68k_translate_mode_t mode) {
    if (mode == MC68K_TRANSLATE_DEBUG)
        return cpu_bus_read_long(cpu, physical, out);
    return cpu_040_table_read(cpu, physical, out);
}

static mc68k_translate_result_t default_attributes(const mc68k_state_t *cpu,
                                                   const mc68k_access_t *a, bool wr) {
    mc68k_translate_result_t r = {.ok = true, .source = MC68K_TRANSLATE_IDENTITY};
    unsigned shift = a->instruction ? TC_DCI_SHIFT : TC_DCO_SHIFT;
    r.cache_mode = (uint8_t)((cpu->mmu060_tc >> shift) & CACHE_MODE_MASK);
    r.cache_inhibit = r.cache_mode >= CACHE_MODE_INHIBIT;
    if (!a->instruction) {
        r.write_protect = (cpu->mmu060_tc & TC_DWO) != 0;
        if (wr && r.write_protect) {
            r.ok = false;
            r.fault = MMU_FAULT_WRITE_PROTECT;
        }
    }
    return r;
}

mc68k_translate_result_t cpu_mmu060_translate_result(mc68k_state_t *cpu, uint32_t logical,
                                                     const mc68k_access_t *a,
                                                     mc68k_translate_mode_t mode) {
    mc68k_translate_result_t r = {
        .ok = true, .source = MC68K_TRANSLATE_IDENTITY, .physical = logical};
    /* TAS/CAS の read 部は U/M 更新・W bit・TTR write protect では write として扱う。 */
    const bool wr = !a->read || a->rmw;
    const bool debug = mode == MC68K_TRANSLATE_DEBUG;
    if (a->alternate) { /* MOVES の SFC/DFC=0・3・4・7 は変換せず物理アドレスとして使う（[060UM
                           §4.2.5]） */
        r.cache_mode = CACHE_MODE_INHIBIT;
        r.cache_inhibit = true;
        return r;
    }
    uint32_t t0 = a->instruction ? cpu->mmu060_itt0 : cpu->mmu060_dtt0;
    uint32_t t1 = a->instruction ? cpu->mmu060_itt1 : cpu->mmu060_dtt1;
    uint32_t ttr = ttr_match(t0, logical, a) ? t0 : ttr_match(t1, logical, a) ? t1 : 0;
    if (ttr) {
        r.source = MC68K_TRANSLATE_TT;
        r.cache_mode = (uint8_t)((ttr >> TTR_CM_SHIFT) & CACHE_MODE_MASK);
        r.cache_inhibit = r.cache_mode >= CACHE_MODE_INHIBIT;
        r.write_protect = (ttr & TTR_W) != 0;
        if (wr && r.write_protect) {
            r.ok = false;
            r.fault = MMU_FAULT_WRITE_PROTECT;
        }
        return r;
    }
    if (!(cpu->mmu060_tc & TC_E)) {
        mc68k_translate_result_t d = default_attributes(cpu, a, wr);
        d.physical = logical;
        return d;
    }

    const unsigned ps = page_shift(cpu);
    const uint32_t mask = (UINT32_C(1) << ps) - 1u;
    mc68k_atc_entry_t *hit = atc_lookup(cpu, logical, a, ps);
    if (hit) {
        r.source = MC68K_TRANSLATE_ATC;
        r.physical = hit->physical_page | (logical & mask);
        r.cache_mode = hit->cache_mode;
        r.cache_inhibit = hit->cache_inhibit;
        r.write_protect = hit->write_protect;
        r.modified = hit->modified;
        if (wr && hit->write_protect) {
            /* M 更新の walk より先に fault にする。entry は変えない。 */
            r.ok = false;
            r.fault = MMU_FAULT_WRITE_PROTECT;
            return r;
        }
        if (!wr || hit->modified || debug) {
            if (!debug)
                hit->history = ++cpu->atc060_clock;
            return r;
        }
        /* W=0・M=0 の entry への書込み: descriptor の M を立てるため walk し、同じ slot
         * を上書きする。 */
    }

    r = (mc68k_translate_result_t){
        .ok = true, .source = MC68K_TRANSLATE_WALK, .physical = logical};
    uint32_t table = ((a->fc & FC_SUPERVISOR_BIT) ? cpu->mmu060_srp : cpu->mmu060_urp) &
                     ~ROOT_TABLE_ALIGN_MASK;
    unsigned indices[ROOT_LEVELS] = {logical >> 25, (logical >> 18) & 0x7f,
                                     ps == 12 ? (logical >> 12) & 0x3f
                                              : (logical >> 13) & 0x1f};
    bool wp = false;
    uint32_t d = 0, da = 0;
    /* root/pointer descriptor の U write は次 level の read 後に行う。 */
    bool u_pending = false;
    uint32_t u_addr = 0, u_value = 0;
    for (unsigned level = 0; level < ROOT_LEVELS; level++) {
        da = table + indices[level] * 4u;
        if (!table_read(cpu, da, &d, mode)) {
            if (u_pending)
                cpu_040_table_write(cpu, u_addr, u_value);
            r.ok = false;
            r.fault = MMU_FAULT_TABLE_BUS_ERROR;
            r.table_bus_error_physical = da;
            return r;
        }
        if (u_pending) {
            u_pending = false;
            if (!cpu_040_table_write(cpu, u_addr, u_value)) {
                r.ok = false;
                r.fault = MMU_FAULT_TABLE_BUS_ERROR;
                r.table_bus_error_physical = u_addr;
                return r;
            }
        }
        r.level = level + 1;
        r.descriptor_physical = da;
        if (level < ROOT_LEVELS - 1) {
            if ((d & DESC_TYPE_MASK) < 2) {
                r.ok = false;
                r.fault = MMU_FAULT_INVALID;
                return r;
            }
            wp |= (d & DESC_W) != 0;
            if (wr && wp) { /* write で W を見つけたら table search を直ちに終える。この
                               descriptor の U は更新しない */
                r.write_protect = true;
                r.ok = false;
                r.fault = MMU_FAULT_WRITE_PROTECT;
                return r;
            }
            if (!(d & DESC_U) && !debug) {
                u_pending = true;
                u_addr = da;
                u_value = d | DESC_U;
            }
            table = d & ~(level == 0 ? ROOT_TABLE_ALIGN_MASK
                                     : (ps == 12 ? UINT32_C(0xff) : UINT32_C(0x7f)));
            continue;
        }
        if ((d & DESC_TYPE_MASK) == DESC_INDIRECT) {
            da = d & ~UINT32_C(3);
            if (!table_read(cpu, da, &d, mode)) {
                r.ok = false;
                r.fault = MMU_FAULT_TABLE_BUS_ERROR;
                r.table_bus_error_physical = da;
                return r;
            }
            r.descriptor_physical = da;
            if ((d & DESC_TYPE_MASK) == DESC_INDIRECT)
                r.indirect_fault = true; /* 2 つ目の indirect は FSLW.IL */
        }
        if ((d & DESC_TYPE_MASK) != DESC_RESIDENT_PAGE &&
            (d & DESC_TYPE_MASK) != DESC_RESIDENT_PAGE_B) {
            r.ok = false;
            r.fault = MMU_FAULT_INVALID;
            return r;
        }
        wp |= (d & DESC_W) != 0;
        if ((d & DESC_S) && !(a->fc & FC_SUPERVISOR_BIT)) {
            r.ok = false;
            r.fault = MMU_FAULT_SUPERVISOR;
            return r;
        }
        /* write で accumulated WP が立っていたら、history bit（U・M）を更新せず直ちに fault
         * にする （entry も作らない、[060UM §4.2.6.3 p.4-22、Table 4-1]。Fig 4-9 の流れ図は U
         * の更新を描くが、 本文と Table 4-1 に従う。設計書 §11）。 */
        if (wr && wp) {
            r.write_protect = true;
            r.modified = (d & DESC_M) != 0;
            r.ok = false;
            r.fault = MMU_FAULT_WRITE_PROTECT;
            return r;
        }
        const bool set_m = wr;
        if (!debug && (!(d & DESC_U) || (set_m && !(d & DESC_M)))) {
            uint32_t nd = d | DESC_U | (set_m ? DESC_M : 0);
            const bool rmw = !(d & DESC_U) && (!set_m || (d & DESC_M));
            uint32_t failed = da;
            if (!(rmw ? cpu_040_table_set_bits(cpu, da, DESC_U, &nd, &failed)
                      : cpu_040_table_write(cpu, da, nd))) {
                r.ok = false;
                r.fault = MMU_FAULT_TABLE_BUS_ERROR;
                r.table_bus_error_physical = failed;
                return r;
            }
            d = nd;
        }
        r.physical = (d & ~mask) | (logical & mask);
        r.cache_mode = (uint8_t)((d >> DESC_CM_SHIFT) & CACHE_MODE_MASK);
        r.cache_inhibit = r.cache_mode >= CACHE_MODE_INHIBIT;
        r.write_protect = wp;
        r.modified = (d & DESC_M) != 0;
    }
    if (!debug) {
        mc68k_atc_entry_t *e = hit ? hit : atc_alloc(cpu, a);
        *e = (mc68k_atc_entry_t){
            .logical_page = logical & ~mask,
            .physical_page = r.physical & ~mask,
            .fc = (uint8_t)(a->fc & FC_SUPERVISOR_BIT),
            .page_shift = (uint8_t)ps,
            .valid = true,
            .cache_inhibit = r.cache_inhibit,
            .cache_mode = r.cache_mode,
            .write_protect = r.write_protect,
            .modified = r.modified,
            .global = (d & DESC_G) != 0,
            .history = ++cpu->atc060_clock,
        };
        /* entry の上書き。命令フェッチ高速経路が覚えた slot も変わりうる。 */
        cpu_ifetch_fast_invalidate(cpu);
    }
    return r;
}

/* PFLUSH (An)/PFLUSHN (An): DFC の FC2 の空間にある、An のページの entry
 * だけを命令側・データ側の 両方から無効にする。PFLUSHA/PFLUSHAN: 空間に依らず全 entry（N は
 * non-global のみ）。 */
static int op_060_pflush(mc68k_state_t *cpu, uint16_t op) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    const bool all = (op & 0x10u) != 0;
    const bool nonglobal = (op & 0x08u) == 0;
    const unsigned ps = page_shift(cpu);
    const uint32_t page = cpu->a[op & 7] & ~((UINT32_C(1) << ps) - 1u);
    const unsigned space = cpu->dfc & FC_SUPERVISOR_BIT;
    for (unsigned bank = 0; bank < 2; bank++) {
        mc68k_atc_entry_t *atc = bank ? cpu->atc060_d : cpu->atc060_i;
        for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; i++) {
            mc68k_atc_entry_t *e = &atc[i];
            if (!e->valid || (nonglobal && e->global))
                continue;
            if (all || ((e->fc & FC_SUPERVISOR_BIT) == space && e->logical_page == page))
                e->valid = false;
        }
    }
    cpu_ifetch_fast_invalidate(cpu);
    return 4; /* 命令時間は 060-G */
}

/* PLPAR/PLPAW: DFC の空間で An を変換し、ATC/table search に成功したときだけ An
 * を物理アドレスにする。 TTR hit・TC.E=0 では An を変えない。失敗は戻り値でなく access
 * error（[060UM §4.7.3]）。 */
static int op_060_plpa(mc68k_state_t *cpu, uint16_t op) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    const bool read = (op & 0x40u) != 0;
    const mc68k_access_t access = {
        .fc = cpu->dfc, .read = read, .size = 1, .instruction = false};
    uint32_t *an = &cpu->a[op & 7];
    mc68k_translate_result_t r =
        cpu_mmu060_translate_result(cpu, *an, &access, MC68K_TRANSLATE_PLOAD);
    if (!r.ok) {
        /* 失敗は PLPA の access（DFC の空間・read/write）の access error。原因は FSLW に載せる
         * （[060UM §4.7.3、§11.1.2.2.4]）。 */
        cpu_note_access(cpu, &access);
        if (!cpu->group0_fault_latched) {
            cpu->g0_mmu_latched = true;
            cpu->g0_mmu_result = r;
        }
        cpu_signal_bus_error(cpu, *an, read);
        return 4;
    }
    if (r.source == MC68K_TRANSLATE_ATC || r.source == MC68K_TRANSLATE_WALK)
        *an = r.physical;
    return 4; /* 命令時間は 060-G */
}

/* cpu_translate_execute_access() の 060 版。命令フェッチ高速経路、同一性変換、通常の変換の順。
 */
bool cpu_translate_execute_access_060(mc68k_state_t *cpu, uint32_t logical,
                                      const mc68k_access_t *access, uint32_t *physical,
                                      bool *cache_inhibit) {
    cpu_note_access(cpu, access);
    const bool ifast = access->instruction && access->read && !access->rmw &&
                       !access->alternate && !cpu->atc060_ifast_disable;
    if (ifast && cpu_mmu060_ifast_try(cpu, logical, access, physical, cache_inhibit))
        return true;
    if (cpu_mmu060_is_identity(cpu)) {
        *physical = logical;
        /* alternate な空間の MOVES（SFC/DFC=0・3・4・7）は変換がなくても noncachable */
        *cache_inhibit = access->alternate;
        cpu->access_cache_mode = access->alternate ? 2 : 0;
        cpu->access_tt_hit = false;
        return true;
    }
    mc68k_translate_result_t result =
        cpu_mmu060_translate_result(cpu, logical, access, MC68K_TRANSLATE_EXECUTE);
    cpu->access_tt_hit = result.source == MC68K_TRANSLATE_TT;
    if (result.ok) {
        if (ifast &&
            (result.source == MC68K_TRANSLATE_ATC || result.source == MC68K_TRANSLATE_WALK))
            cpu_mmu060_ifast_prime(cpu, logical, access);
        *physical = result.physical;
        *cache_inhibit = result.cache_inhibit;
        cpu->access_cache_mode = result.cache_mode;
        return true;
    }
    if (!cpu->group0_fault_latched) {
        cpu->g0_mmu_latched = true;
        cpu->g0_mmu_result = result;
    }
    cpu_signal_bus_error(cpu, logical, access->read);
    return false;
}

void register_68060_mmu_ops(op_handler_t *table) {
    for (unsigned an = 0; an < 8; ++an) {
        table[0xf500u | an] = op_060_pflush; /* PFLUSHN (An) */
        table[0xf508u | an] = op_060_pflush; /* PFLUSH (An) */
        table[0xf510u | an] = op_060_pflush; /* PFLUSHAN */
        table[0xf518u | an] = op_060_pflush; /* PFLUSHA */
        table[0xf588u | an] = op_060_plpa;   /* PLPAW (An) */
        table[0xf5c8u | an] = op_060_plpa;   /* PLPAR (An) */
    }
}
