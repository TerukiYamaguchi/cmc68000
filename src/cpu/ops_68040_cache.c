/* MC68040 のキャッシュ保守と MOVE16（MC68040UM §4、§7.4.4）。
 * MC68060 はライン／ウェイの記憶域を共有するが、方針は異なる。128 セット（8KB）、
 * CACR.DPI と TC.P のページサイズを用いる CINV/CPUSH、ロック RMW での非割当てである（MC68060UM
 * §5）。 */
#include "cpu_internal.h"
#include <string.h>

enum {
    CACHE_D = 1,
    CACHE_I = 2,
    CACHE_BOTH = 3,
    SCOPE_LINE = 1,
    SCOPE_PAGE = 2,
    SCOPE_ALL = 3
};
#define CYCLES_060_CACHE_OP 4 /* 仮値。060 の命令時間は 060-G で CSV 化する */
#define TC_060_P            UINT32_C(0x00004000)
#define PAGE_MASK_4K        UINT32_C(0xfff)
#define PAGE_MASK_8K        UINT32_C(0x1fff)

static bool push_line(mc68k_state_t *cpu, mc68k_040_cache_line_t *line);
static bool push_line_mode(mc68k_state_t *cpu, mc68k_040_cache_line_t *line, bool keep_dirty);

static unsigned cache_sets(const mc68k_state_t *cpu) {
    return cpu_is_68060(cpu) ? MC68K_060_CACHE_SETS : MC68K_040_CACHE_SETS;
}

void cpu_040_cache_reset(mc68k_state_t *cpu) {
    memset(cpu->icache040, 0, sizeof(cpu->icache040));
    memset(cpu->dcache040, 0, sizeof(cpu->dcache040));
    memset(cpu->icache040_next, 0, sizeof(cpu->icache040_next));
    memset(cpu->dcache040_next, 0, sizeof(cpu->dcache040_next));
    cpu->icache040_last = 0;
}

static mc68k_040_cache_line_t *cache_line(mc68k_state_t *cpu, bool instruction,
                                          uint32_t physical, bool allocate) {
    unsigned set = (physical >> 4) & (cache_sets(cpu) - 1u);
    mc68k_040_cache_line_t(*cache)[MC68K_040_CACHE_WAYS] =
        instruction ? cpu->icache040 : cpu->dcache040;
    uint8_t *next = instruction ? cpu->icache040_next : cpu->dcache040_next;
    uint32_t base = physical & ~UINT32_C(0xf);
    for (unsigned way = 0; way < MC68K_040_CACHE_WAYS; way++)
        if (cache[set][way].valid && cache[set][way].physical_line == base)
            return &cache[set][way];
    if (!allocate)
        return NULL;
    mc68k_040_cache_line_t *line = &cache[set][next[set]++ & 3u];
    if (line->valid && line->dirty && !push_line(cpu, line))
        return NULL;
    *line = (mc68k_040_cache_line_t){.physical_line = base, .valid = true};
    return line;
}

/* アクセスはライン内の 1 個の 32 ビットワードに収まらなければならない。
 * 68040 のアクセス関数は、非整列 word/long アクセスをキャッシュ到達前に byte/word
 * 片へ分割する。 */
static uint32_t line_get(const mc68k_040_cache_line_t *line, unsigned offset, unsigned size) {
    assert((offset & 3u) + size <= 4u);
    uint32_t word = line->data[offset >> 2];
    unsigned shift = (4u - size - (offset & 3u)) * 8u;
    return (word >> shift) & (size == 4 ? UINT32_MAX : ((UINT32_C(1) << (size * 8u)) - 1u));
}

static bool load_line(mc68k_state_t *cpu, mc68k_040_cache_line_t *line, uint32_t logical,
                      const mc68k_access_t *access) {
    if (line->loaded)
        return true;
    for (unsigned i = 0; i < 4; i++) {
        bool ok = cpu_bus_read_long(cpu, line->physical_line + i * 4u, &line->data[i]);
        mc68k_access_t fill = *access;
        fill.size = 4;
        cpu_account_bus_cycle(cpu, line->physical_line + i * 4u, &fill, false);
        if (!ok) {
            line->valid = false;
            cpu_signal_bus_error(cpu, logical, true);
            return false;
        }
    }
    line->loaded = true;
    return true;
}

/* 060 の TAS/CAS（ロック RMW）とキャッシュ不可アクセスは D-cache に割り当てず外部へ出る。
 * ヒットしたラインは dirty なら書き戻して無効化する（[060UM §5.1]）。書き戻し失敗時は
 * bus error を保持して true を返し、呼出し側は外部アクセスをしない。 */
static bool rmw_bypass(mc68k_state_t *cpu, uint32_t physical, uint32_t *out) {
    mc68k_040_cache_line_t *line = cache_line(cpu, false, physical, false);
    if (!line)
        return false;
    if (line->dirty && !push_line(cpu, line)) {
        *out = 0;
        return true;
    }
    line->valid = false;
    return false;
}

bool cpu_040_cache_read(mc68k_state_t *cpu, uint32_t logical, uint32_t physical,
                        const mc68k_access_t *access, uint8_t cache_mode, uint32_t *out) {
    /* 060: noncachable アクセスに一致した有効な line は、dirty なら push して無効化、clean
     * なら無効化する （[060UM §5.1 p.5-4]）。bus へは cache の外で出る。 */
    if (cpu_is_68060(cpu) && cache_mode >= 2 && !access->instruction &&
        (cpu->cacr & MC68K_CACR_040_DE))
        return rmw_bypass(cpu, physical, out);
    if (cache_mode >= 2 ||
        !(cpu->cacr & (access->instruction ? MC68K_CACR_040_IE : MC68K_CACR_040_DE)) ||
        (physical & 15u) + access->size > 16)
        return false;
    if (cpu_is_68060(cpu) && access->rmw)
        return rmw_bypass(cpu, physical, out);
    if (cpu_is_68060(cpu) && cpu->cache_no_allocate && !access->instruction) {
        /* 例外 vector の fetch・RTE の stack 解放は read miss で allocate しない（[060UM
         * §7.7.1]） */
        const mc68k_040_cache_line_t *hit = cache_line(cpu, false, physical, false);
        if (!hit || !hit->loaded)
            return false;
        *out = line_get(hit, physical & 15u, access->size);
        return true;
    }
    mc68k_040_cache_line_t *line = cache_line(cpu, access->instruction, physical, true);
    if (!line)
        return false;
    if (!load_line(cpu, line, logical, access))
        return true;
    if (access->instruction)
        cpu->icache040_last = (uint16_t)(line - &cpu->icache040[0][0]);
    *out = line_get(line, physical & 15u, access->size);
    return true;
}

bool cpu_040_cache_write(mc68k_state_t *cpu, uint32_t logical, uint32_t physical,
                         uint32_t value, const mc68k_access_t *access, uint8_t cache_mode) {
    if (cpu_is_68060(cpu) && cache_mode >= 2 &&
        (cpu->cacr & MC68K_CACR_040_DE)) { /* write は data のみ */
        uint32_t unused;
        return rmw_bypass(
            cpu, physical,
            &unused); /* push が失敗したら bus error を保持して外部 write をしない */
    }
    if (access->instruction || cache_mode >= 2 || !(cpu->cacr & MC68K_CACR_040_DE) ||
        (physical & 15u) + access->size > 16)
        return false;
    if (cpu_is_68060(cpu) && access->rmw) {
        uint32_t unused;
        return rmw_bypass(cpu, physical, &unused);
    }
    /* 例外の stacking は write miss で allocate しない（[060UM §7.7.3]） */
    const bool allocate = cache_mode == 1 && !(cpu_is_68060(cpu) && cpu->cache_no_allocate);
    mc68k_040_cache_line_t *line = cache_line(cpu, false, physical, allocate);
    if (!line)
        return false;
    if (cache_mode == 1 && !load_line(cpu, line, logical, access))
        return true;
    assert((physical & 3u) + access->size <= 4u); /* see line_get() */
    unsigned shift = (4u - access->size - (physical & 3u)) * 8u;
    uint32_t mask =
        access->size == 4 ? UINT32_MAX : ((UINT32_C(1) << (access->size * 8u)) - 1u) << shift;
    line->data[(physical & 15u) >> 2] =
        (line->data[(physical & 15u) >> 2] & ~mask) | (value << shift);
    if (cache_mode == 1) {
        line->dirty |= (uint8_t)(1u << ((physical & 15u) >> 2));
        return true;
    }
    return false;
}

bool cpu_040_cache_peek(mc68k_state_t *cpu, uint32_t physical, uint8_t *out) {
    if (!cpu_has_onchip_cache(cpu))
        return false;
    const mc68k_040_cache_line_t *line = cache_line(cpu, false, physical, false);
    if (!line || !line->loaded)
        return false;
    *out = (uint8_t)line_get(line, physical & 15u, 1);
    return true;
}

static bool selected(uint32_t line, unsigned scope, uint32_t address, uint32_t page_mask) {
    if (scope == SCOPE_ALL)
        return true;
    if (scope == SCOPE_PAGE)
        return (line & ~page_mask) == (address & ~page_mask);
    return line == (address & ~UINT32_C(0xf));
}

/* 例外を発生させず dirty ロングワードを書き戻す。失敗時は未書込みロングワードの dirty ビットを
 * 保持し、*failed には失敗したロングワードのアドレスを入れる。 */
static bool push_line_quiet(mc68k_state_t *cpu, mc68k_040_cache_line_t *line, uint32_t *failed,
                            bool keep_dirty) {
    for (unsigned i = 0; i < 4; i++) {
        if (!(line->dirty & (1u << i)))
            continue;
        bool ok = cpu_bus_write_long(cpu, line->physical_line + i * 4u, line->data[i]);
        const mc68k_access_t write = cpu_supervisor_data_access(false, 4, false);
        cpu_account_bus_cycle(cpu, line->physical_line + i * 4u, &write, false);
        if (!ok) {
            *failed = line->physical_line + i * 4u;
            return false;
        }
        if (!keep_dirty)
            line->dirty &= (uint8_t)~(1u << i);
    }
    return true;
}

static bool push_line(mc68k_state_t *cpu, mc68k_040_cache_line_t *line) {
    return push_line_mode(cpu, line, false);
}

/* keep_dirty は CPUSH の DPI=1 用: 書き戻しても dirty を残す（[060UM Table 5-3 CD6]）。 */
static bool push_line_mode(mc68k_state_t *cpu, mc68k_040_cache_line_t *line, bool keep_dirty) {
    uint32_t failed;
    if (push_line_quiet(cpu, line, &failed, keep_dirty))
        return true;
    cpu->fault040 = (mc68k_040_access_error_t){
        .valid = true, .fault_address = line->physical_line, .ssw = 0};
    cpu_signal_bus_error(cpu, failed, false);
    return false;
}

/* テーブル検索アクセス（MC68040UM §3.2.5、§4.3.3、design_issue100）。ラインを決して
 * 割り当てず例外も発生させない。bus error は失敗アドレスとともに呼出し側へ報告する。 */
static mc68k_040_cache_line_t *table_line(mc68k_state_t *cpu, uint32_t physical) {
    /* 060 の table search は data cache を完全に無視する（[060UM §5.4.3 p.5-9、§11.1.2.2.2]）。
     */
    if (cpu_is_68060(cpu) || !(cpu->cacr & MC68K_CACR_040_DE))
        return NULL;
    return cache_line(cpu, false, physical, false);
}

bool cpu_040_table_read(mc68k_state_t *cpu, uint32_t physical, uint32_t *out) {
    const mc68k_040_cache_line_t *line = table_line(cpu, physical);
    if (line && line->loaded) {
        *out = line_get(line, physical & 15u, 4);
        return true;
    }
    const mc68k_access_t read = cpu_supervisor_data_access(true, 4, false);
    bool ok = cpu_bus_read_long(cpu, physical, out);
    cpu_account_bus_cycle(cpu, physical, &read, true);
    return ok;
}

bool cpu_040_table_write(mc68k_state_t *cpu, uint32_t physical, uint32_t value) {
    mc68k_040_cache_line_t *line = table_line(cpu, physical);
    const mc68k_access_t write = cpu_supervisor_data_access(false, 4, false);
    bool ok = cpu_bus_write_long(cpu, physical, value);
    cpu_account_bus_cycle(cpu, physical, &write, true);
    /* ラインはバスに追従する。書込み失敗時は元のままとし、clean ラインがバスに存在しない値を
     * 保持していると主張しないようにする。 */
    if (ok && line && line->loaded)
        line->data[(physical & 15u) >> 2] = value; /* the dirty mask is kept */
    return ok;
}

bool cpu_040_table_set_bits(mc68k_state_t *cpu, uint32_t physical, uint32_t bits, uint32_t *out,
                            uint32_t *failed) {
    mc68k_040_cache_line_t *line = table_line(cpu, physical);
    if (line) {
        if (!push_line_quiet(cpu, line, failed, false))
            return false;
        line->valid = false;
    }
    const mc68k_access_t read = cpu_supervisor_data_access(true, 4, true);
    const mc68k_access_t write = cpu_supervisor_data_access(false, 4, true);
    uint32_t value;
    bool ok = cpu_bus_read_long(cpu, physical, &value);
    cpu_account_bus_cycle(cpu, physical, &read, true);
    if (ok) {
        value |= bits;
        ok = cpu_bus_write_long(cpu, physical, value);
        cpu_account_bus_cycle(cpu, physical, &write, true);
    }
    if (!ok) {
        *failed = physical;
        return false;
    }
    *out = value;
    return true;
}

static int op_cache(mc68k_state_t *cpu, uint16_t opcode) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    unsigned caches = (opcode >> 6) & 3u, scope = (opcode >> 3) & 3u;
    bool push = (opcode & 0x20u) != 0;
    if (scope == 0)
        return cpu_dispatch_exception(cpu, cpu->pc - 2, 4, 34);
    /* [PM §6 CINV/CPUSH] CACHE=00 selects neither cache, so it is a NOP. */
    if (caches == 0)
        return cpu_040_tag(cpu, push ? 6 : 9);
    uint32_t address = cpu->a[opcode & 7];
    for (unsigned type = 0; type < 2; type++) {
        if (!(caches & (type ? CACHE_D : CACHE_I)))
            continue;
        mc68k_040_cache_line_t(*cache)[MC68K_040_CACHE_WAYS] =
            type ? cpu->dcache040 : cpu->icache040;
        for (unsigned set = 0; set < cache_sets(cpu); set++)
            for (unsigned way = 0; way < MC68K_040_CACHE_WAYS; way++) {
                mc68k_040_cache_line_t *line = &cache[set][way];
                if (!line->valid ||
                    !selected(line->physical_line, scope, address, PAGE_MASK_4K))
                    continue;
                if (push && type && !push_line(cpu, line))
                    return cpu_040_tag(cpu, scope == SCOPE_LINE ? 6 : 267);
                line->valid = false;
            }
    }
    /* MC68040UM 表 10-3/10-4。バス wait は push_line() で加算する。 */
    if (!push)
        return cpu_040_tag(cpu, scope == SCOPE_PAGE ? 266 : 9);
    /* 表 10-4: CPUSHP と CPUSHA は最良時 267 クロックの行を共有する。 */
    return cpu_040_tag(cpu, scope == SCOPE_LINE ? 6 : 267);
}

/* 060 の CINV/CPUSH（[060UM App.D D-1、D-11、D-12、§5.2、§5.3]）。An は物理アドレスで変換
 * しない。scope=00 は illegal。CPUSH の D-cache は DPI=0 で push+無効化、DPI=1 で push のみ
 * （dirty は残す。clean line の DPI=1 は一次資料が食い違うため valid のまま、設計書 §11）。
 * I-cache 指定は DPI に依らず無効化。branch cache は分岐予測を持たないので扱わない。 */
static int op_cache060(mc68k_state_t *cpu, uint16_t opcode) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    unsigned caches = (opcode >> 6) & 3u, scope = (opcode >> 3) & 3u;
    bool push = (opcode & 0x20u) != 0;
    if (scope == 0)
        return cpu_dispatch_exception(cpu, cpu->pc - 2, 4, 34);
    if (caches == 0)
        return CYCLES_060_CACHE_OP;
    uint32_t address = cpu->a[opcode & 7];
    uint32_t page_mask = (cpu->mmu060_tc & TC_060_P) ? PAGE_MASK_8K : PAGE_MASK_4K;
    bool keep_dirty = push && (cpu->cacr & MC68K_CACR_060_DPI);
    /* 命令時間用に、実際に書き戻した dirty line 数を t060_aux へ積む（timing_060.c の
     * cyc060_cache_op）。 */
    uint32_t pushed_lines = 0;
    for (unsigned type = 0; type < 2; type++) {
        if (!(caches & (type ? CACHE_D : CACHE_I)))
            continue;
        mc68k_040_cache_line_t(*cache)[MC68K_040_CACHE_WAYS] =
            type ? cpu->dcache040 : cpu->icache040;
        for (unsigned set = 0; set < cache_sets(cpu); set++)
            for (unsigned way = 0; way < MC68K_040_CACHE_WAYS; way++) {
                mc68k_040_cache_line_t *line = &cache[set][way];
                if (!line->valid || !selected(line->physical_line, scope, address, page_mask))
                    continue;
                if (push && type) {
                    const bool was_dirty = line->dirty != 0;
                    if (!push_line_mode(cpu, line, keep_dirty)) {
                        cpu->t060_aux = pushed_lines;
                        return CYCLES_060_CACHE_OP;
                    }
                    if (was_dirty)
                        pushed_lines++;
                    if (keep_dirty)
                        continue;
                }
                line->valid = false;
            }
    }
    cpu->t060_aux = pushed_lines;
    return CYCLES_060_CACHE_OP;
}

bool cpu_040_move16(mc68k_state_t *cpu, uint32_t source, uint32_t destination,
                    bool source_postincrement, bool destination_postincrement) {
    uint32_t values[4];
    source &= ~UINT32_C(0xf);
    destination &= ~UINT32_C(0xf);
    const mc68k_access_t read = cpu_data_access(cpu, true, 4, false);
    const mc68k_access_t write = cpu_data_access(cpu, false, 4, false);
    /* 060: CACR.EDC=0 の data cache は無効で、MOVE16 も line に触れない（[060UM §5.2]） */
    const bool dcache = !cpu_is_68060(cpu) || (cpu->cacr & MC68K_CACR_040_DE);
    for (unsigned i = 0; i < 4; i++) {
        uint32_t physical;
        bool cache_inhibit;
        if (!cpu_translate_execute_access(cpu, source + i * 4u, &read, &physical,
                                          &cache_inhibit))
            return false;
        mc68k_040_cache_line_t *line = dcache ? cache_line(cpu, false, physical, false) : NULL;
        if (line && line->loaded)
            values[i] = line_get(line, physical & 15u, 4);
        else {
            bool ok = cpu_bus_read_long(cpu, physical, &values[i]);
            cpu_account_bus_cycle(cpu, physical, &read, cache_inhibit);
            if (!ok) {
                cpu_signal_bus_error(cpu, source + i * 4u, true);
                return false;
            }
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        uint32_t physical;
        bool cache_inhibit;
        if (!cpu_translate_execute_access(cpu, destination + i * 4u, &write, &physical,
                                          &cache_inhibit))
            return false;
        mc68k_040_cache_line_t *line = dcache ? cache_line(cpu, false, physical, false) : NULL;
        if (line) {
            if (line->dirty && !push_line(cpu, line))
                return false;
            line->valid = false;
        }
        bool ok = cpu_bus_write_long(cpu, physical, values[i]);
        cpu_account_bus_cycle(cpu, physical, &write, cache_inhibit);
        if (!ok) {
            cpu_signal_bus_error(cpu, destination + i * 4u, false);
            return false;
        }
    }
    (void)source_postincrement;
    (void)destination_postincrement;
    return true;
}

static int move16_cycles(mc68k_state_t *cpu, int cycles040) {
    return cpu_is_68060(cpu) ? CYCLES_060_CACHE_OP : cpu_040_tag(cpu, cycles040);
}

static int op_move16(mc68k_state_t *cpu, uint16_t opcode) {
    unsigned src = opcode & 7u;
    uint16_t ext;
    if ((opcode & 0xfff8u) == 0xf620u) {
        ext = cpu_fetch_word(cpu);
        if (cpu->exec_state != CPU_NORMAL)
            return 6;
        if (!(ext & 0x8000u))
            return cpu_dispatch_exception(cpu, cpu->pc - 4, 4, 34);
        unsigned dst = (ext >> 12) & 7u;
        uint32_t dst_addr = cpu->a[dst];
        if (!cpu_040_move16(cpu, cpu->a[src], dst_addr, true, true))
            return 6;
        cpu->a[src] += 16;
        cpu->a[dst] = dst_addr + 16;
        return move16_cycles(cpu, 13); /* 040UM Table 10-5: calculate 6 + execute base 7 */
    }
    uint32_t absolute = (uint32_t)cpu_fetch_word(cpu) << 16;
    if (cpu->exec_state != CPU_NORMAL)
        return 6;
    absolute |= cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 6;
    bool memory_source = (opcode & 8u) != 0, post = (opcode & 0x10u) == 0;
    if (!cpu_040_move16(cpu, memory_source ? absolute : cpu->a[src],
                        memory_source ? cpu->a[src] : absolute, post && !memory_source,
                        post && memory_source))
        return 6;
    if (post)
        cpu->a[src] += 16;
    if (memory_source)
        return move16_cycles(cpu, post ? 13 : 11); /* xxx.L,(An)+ / xxx.L,(An) */
    return move16_cycles(cpu, 11);                 /* (An)+,xxx.L / (An),xxx.L */
}

void register_68040_cache_ops(op_handler_t *table) {
    for (unsigned cache = 0; cache < 4; cache++)
        for (unsigned scope = 0; scope < 4; scope++)
            for (unsigned reg = 0; reg < 8; reg++) {
                table[0xf400u | (cache << 6) | (scope << 3) | reg] = op_cache;
                table[0xf420u | (cache << 6) | (scope << 3) | reg] = op_cache;
            }
    for (unsigned reg = 0; reg < 8; reg++)
        table[0xf620u | reg] = op_move16;
    for (unsigned reg = 0; reg < 8; reg++)
        for (unsigned form = 0; form < 4; form++)
            table[0xf600u | (form << 3) | reg] = op_move16;
}

void register_68060_cache_ops(op_handler_t *table) {
    for (unsigned cache = 0; cache < 4; cache++)
        for (unsigned scope = 0; scope < 4; scope++)
            for (unsigned reg = 0; reg < 8; reg++) {
                table[0xf400u | (cache << 6) | (scope << 3) | reg] = op_cache060;
                table[0xf420u | (cache << 6) | (scope << 3) | reg] = op_cache060;
            }
    for (unsigned reg = 0; reg < 8; reg++)
        table[0xf620u | reg] = op_move16;
    for (unsigned reg = 0; reg < 8; reg++)
        for (unsigned form = 0; form < 4; form++)
            table[0xf600u | (form << 3) | reg] = op_move16;
}
