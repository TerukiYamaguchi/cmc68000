/*
 * debugger.c — デバッグ支援機能の実装
 *
 * PC/レジスタ履歴リングのダンプ、BADMOVEA/BADADDR/BADJUMP/PCTRACE1024トグル、
 * 強制ILLEGAL化、デバッグ出力コールバック、停止理由アクセサ、非侵襲メモリ
 * 読み出し（peek）、アドレス比較型ブレークポイントをここに集約する。
 * これらは実機にはない、エミュレータ開発者向けのホスト側デバッグ支援機能
 * （include/mc68000/debug.h 参照）。
 *
 * 将来実装予定（ステップ5以降）:
 *   - ウォッチポイント
 *   - 逆アセンブラ（独立モジュール、src/debug/disasm.c 想定）
 */
#include "cpu/cpu_internal.h"
#include "mc68000/cpu.h" /* mc68k_execute() のプロトタイプ（cpu/cpu_internal.h は含まない） */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * デバッグ出力の一元窓口。ログハンドラ（mc68k_set_log_handler）が
 * 登録されていればそちらへ、なければ stderr へフォールバックする。
 */
void cpu_log(mc68k_state_t *cpu, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (cpu->log_fn)
        cpu->log_fn(cpu->log_ctx, buf);
    else
        fputs(buf, stderr);
}

void mc68k_set_log_handler(mc68k_state_t *cpu, mc68k_log_fn_t fn, void *ctx) {
    if (!cpu)
        return;
    cpu->log_fn = fn;
    cpu->log_ctx = ctx;
}

/* 書込み位置 pos・有効件数 count・容量 size（2の冪）のリングについて、最新 min(count, cap)
 * 件の先頭インデックスを *start に入れ、件数を返す。 */
static size_t ring_window(int pos, int count, int size, size_t cap, int *start) {
    size_t n = (size_t)count < cap ? (size_t)count : cap;
    *start = (pos - (int)n) & (size - 1);
    return n;
}

/* PC トレース・リングバッファをダンプ（白帯/暴走の実行パス解析用）。
 * count 件分（最大 PC_RING_SIZE、有効件数で clamp）を古い順に出力する。 */
void mc68k_dump_pc_ring(const mc68k_state_t *cpu, int count) {
    if (!cpu)
        return;
    if (count <= 0 || count > PC_RING_SIZE)
        count = PC_RING_SIZE;
    if (count > cpu->pc_ring_count)
        count = cpu->pc_ring_count;
    mc68k_state_t *c = (mc68k_state_t *)cpu; /* cpu_log()はlog_fn/log_ctxを読むのみ */
    int start = (cpu->pc_ring_pos - count) & (PC_RING_SIZE - 1);
    cpu_log(c, "=== PC trace (last %d instrs, oldest first) ===\n", count);
    for (int i = 0; i < count; i++) {
        int idx = (start + i) & (PC_RING_SIZE - 1);
        cpu_log(c, " %06X", cpu->pc_ring[idx].pc);
        if ((i & 0x0F) == 0x0F)
            cpu_log(c, "\n");
    }
    cpu_log(c, "\n=== end PC trace ===\n");
}

size_t mc68k_read_pc_ring(const mc68k_state_t *cpu, mc68k_pc_ring_entry_t *out, size_t cap) {
    if (!cpu)
        return 0;
    if (!out)
        return (size_t)cpu->pc_ring_count;
    int start;
    size_t n = ring_window(cpu->pc_ring_pos, cpu->pc_ring_count, PC_RING_SIZE, cap, &start);
    for (size_t i = 0; i < n; i++)
        out[i] = cpu->pc_ring[(start + (int)i) & (PC_RING_SIZE - 1)];
    return n;
}

/* PC履歴リング記録のON/OFFを切り替える。既定オフ。PCリング本体と書込み位置は
 * 保持するため、OFF後の明示ダンプおよび再ON後の追記ができる。 */
void mc68k_set_pc_ring(mc68k_state_t *cpu, int on) {
    if (!cpu)
        return;
    cpu->pc_ring_on = (on != 0);
}

int mc68k_get_pc_ring(const mc68k_state_t *cpu) {
    return cpu ? (cpu->pc_ring_on ? 1 : 0) : 0;
}

/* レジスタ履歴リングをダンプ（暴走直前の全レジスタ遷移を解析）。
 * 各行: pc opcode | D0..D7 | A0..A7 | SR を実行直前の値で出力する。 */
void mc68k_dump_reg_ring(const mc68k_state_t *cpu, int count) {
    if (!cpu)
        return;
    if (count <= 0 || count > REG_RING_SIZE)
        count = REG_RING_SIZE;
    if (count > cpu->reg_ring_count)
        count = cpu->reg_ring_count;
    mc68k_state_t *c = (mc68k_state_t *)cpu; /* cpu_log()はlog_fn/log_ctxを読むのみ */
    int start = (cpu->reg_ring_pos - count) & (REG_RING_SIZE - 1);
    cpu_log(c, "=== REG trace (last %d instrs, oldest first; regs are PRE-exec) ===\n", count);
    for (int i = 0; i < count; i++) {
        int idx = (start + i) & (REG_RING_SIZE - 1);
        const mc68k_reg_ring_entry_t *s = &cpu->reg_ring[idx];
        cpu_log(c,
                "pc=%06X op=%04X sr=%04X "
                "D=%08X %08X %08X %08X %08X %08X %08X %08X "
                "A=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                s->pc, s->opcode, s->sr, s->d[0], s->d[1], s->d[2], s->d[3], s->d[4], s->d[5],
                s->d[6], s->d[7], s->a[0], s->a[1], s->a[2], s->a[3], s->a[4], s->a[5], s->a[6],
                s->a[7]);
    }
    cpu_log(c, "=== end REG trace ===\n");
}

size_t mc68k_read_reg_ring(const mc68k_state_t *cpu, mc68k_reg_ring_entry_t *out, size_t cap) {
    if (!cpu)
        return 0;
    if (!out)
        return (size_t)cpu->reg_ring_count;
    int start;
    size_t n = ring_window(cpu->reg_ring_pos, cpu->reg_ring_count, REG_RING_SIZE, cap, &start);
    for (size_t i = 0; i < n; i++)
        out[i] = cpu->reg_ring[(start + (int)i) & (REG_RING_SIZE - 1)];
    return n;
}

/* レジスタ履歴記録（REGRING）の ON/OFF を切り替える。
 * 既定オフ（常時記録は性能低下するため）。デバッグ時のみ ON にする。 */
void mc68k_set_reg_ring(mc68k_state_t *cpu, int on) {
    if (!cpu)
        return;
    cpu->reg_ring_on = (on != 0);
}

int mc68k_get_reg_ring(const mc68k_state_t *cpu) {
    return cpu ? (cpu->reg_ring_on ? 1 : 0) : 0;
}

/* PCTRACE1024 の ON/OFF を切り替える。既定オフ。 */
void mc68k_set_pctrace1024(mc68k_state_t *cpu, int on) {
    if (!cpu)
        return;
    cpu->pctrace1024_on = (on != 0);
}

int mc68k_get_pctrace1024(const mc68k_state_t *cpu) {
    return cpu ? (cpu->pctrace1024_on ? 1 : 0) : 0;
}

/* BADMOVEA診断ログ（ops_move.c）の ON/OFF を切り替える。
 * 既定オフ（正常起動でも大量に出るタイトルがあるため常時オンはノイズになる）。 */
void mc68k_set_badmovea_log(mc68k_state_t *cpu, int on) {
    if (!cpu)
        return;
    cpu->badmovea_log_on = (on != 0);
}

int mc68k_get_badmovea_log(const mc68k_state_t *cpu) {
    return cpu ? (cpu->badmovea_log_on ? 1 : 0) : 0;
}

/* BADADDR診断ログ（cpu_internal.h）の ON/OFF を切り替える。既定オフ。 */
void mc68k_set_badaddr_log(mc68k_state_t *cpu, int on) {
    if (!cpu)
        return;
    cpu->badaddr_log_on = (on != 0);
}

int mc68k_get_badaddr_log(const mc68k_state_t *cpu) {
    return cpu ? (cpu->badaddr_log_on ? 1 : 0) : 0;
}

/* BADJUMP診断ログ（m68000.c）の ON/OFF を切り替える。既定オフ。
 * 初回ダンプ済みフラグ（badjump_dumped）は mc68k_reset() でクリアされる。 */
void mc68k_set_badjump_log(mc68k_state_t *cpu, int on) {
    if (!cpu)
        return;
    cpu->badjump_log_on = (on != 0);
}

int mc68k_get_badjump_log(const mc68k_state_t *cpu) {
    return cpu ? (cpu->badjump_log_on ? 1 : 0) : 0;
}

/* BADORI診断ログ（m68000.c）の ON/OFF を切り替える。既定オフ。
 * 初回ダンプ済みフラグ（badori_dumped）は mc68k_reset() でクリアされる。 */
void mc68k_set_badori_log(mc68k_state_t *cpu, int on) {
    if (!cpu)
        return;
    cpu->badori_log_on = (on != 0);
}

int mc68k_get_badori_log(const mc68k_state_t *cpu) {
    return cpu ? (cpu->badori_log_on ? 1 : 0) : 0;
}

mc68k_stop_reason_t mc68k_get_stop_reason(const mc68k_state_t *cpu) {
    return cpu ? cpu->stop_reason : MC68K_STOP_NONE;
}

int mc68k_get_last_exception_vector(const mc68k_state_t *cpu) {
    if (!cpu || cpu->last_exception_vector == 0)
        return -1;
    return cpu->last_exception_vector;
}

/*
 * 強制 ILLEGAL 命令ハンドラ。
 * mc68k_set_opcode_illegal() で差し替えられたエントリが呼ばれると、
 * ILLEGAL 命令例外（ベクタ 4、アドレス 0x000010）をディスパッチする。
 * ゼロ埋めメモリへの CPU 暴走を早期検出するために使う。
 */
static int op_forced_illegal(mc68k_state_t *cpu, uint16_t opcode) {
    uint32_t fault_pc = cpu->pc - 2; /* opcode fetch 後に PC が +2 されているため */
    cpu_log(cpu, "[WARN] Forced-illegal opcode %04X at PC=%06X — likely CPU runaway\n", opcode,
            fault_pc);

    return cpu_dispatch_exception(cpu, fault_pc, 4, 34);
}

/*
 * エントリプローブは opcode fetch 成功後、対応する命令 handler の前に実行する。
 * callback 前に handler を保存する。公開契約はプローブ管理 API の再入を禁止するが、
 * これにより callback 側の観測処理から命令 dispatch も独立する。
 */
static int op_entry_probe_trampoline(mc68k_state_t *cpu, uint16_t opcode) {
    op_handler_t handler = cpu->entry_probe_saved_table[opcode];
    uint32_t entry_pc = cpu->instr_pc;
    for (size_t i = 0; i < cpu->entry_probe_count; ++i) {
        if (cpu->entry_probe_pcs[i] == entry_pc) {
            cpu->entry_probe_fn(cpu, (unsigned)i, entry_pc, cpu->entry_probe_ctx);
        }
    }
    return handler(cpu, opcode);
}

int mc68k_set_entry_probes(mc68k_state_t *cpu, const uint32_t *pcs, size_t count,
                           mc68k_entry_probe_fn_t fn, void *ctx) {
    if (!cpu || !pcs || !fn || count == 0 || count > MC68K_MAX_ENTRY_PROBES)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        if (pcs[i] & 1u)
            return -1;
    }

    if (!cpu->entry_probe_saved_table) {
        cpu->entry_probe_saved_table = malloc(65536 * sizeof(*cpu->entry_probe_saved_table));
        if (!cpu->entry_probe_saved_table)
            return -1;
    }

    /* 再設定は、すでに置換済みの trampoline ではなく現行の実 handler から始める。 */
    if (!cpu->entry_probe_active)
        memcpy(cpu->entry_probe_saved_table, cpu->op_table, 65536 * sizeof(*cpu->op_table));
    memcpy(cpu->entry_probe_pcs, pcs, count * sizeof(*pcs));
    cpu->entry_probe_count = count;
    cpu->entry_probe_fn = fn;
    cpu->entry_probe_ctx = ctx;
    if (!cpu->entry_probe_active) {
        for (size_t i = 0; i < 65536; ++i)
            cpu->op_table[i] = op_entry_probe_trampoline;
        cpu->entry_probe_active = true;
    }
    return 0;
}

void mc68k_clear_entry_probes(mc68k_state_t *cpu) {
    if (!cpu || !cpu->entry_probe_active)
        return;
    memcpy(cpu->op_table, cpu->entry_probe_saved_table, 65536 * sizeof(*cpu->op_table));
    cpu->entry_probe_count = 0;
    cpu->entry_probe_fn = NULL;
    cpu->entry_probe_ctx = NULL;
    cpu->entry_probe_active = false;
}

void mc68k_set_opcode_illegal(mc68k_state_t *cpu, uint16_t opcode) {
    if (!cpu)
        return;
    /* entry probe 有効中の op_table は trampoline だけを持つ。保存表が
     * clear_entry_probes() で復元する正本である。 */
    if (cpu->entry_probe_active)
        cpu->entry_probe_saved_table[opcode] = op_forced_illegal;
    else
        cpu->op_table[opcode] = op_forced_illegal;
}

/* 非侵襲メモリ読み出しフックの登録。全て NULL でダンプ機能を無効化できる。 */
void mc68k_set_peek_hooks(mc68k_state_t *cpu, mc68k_peek_byte_fn_t peek_byte,
                          mc68k_peek_word_fn_t peek_word, mc68k_peek_long_fn_t peek_long,
                          void *ctx) {
    if (!cpu)
        return;
    cpu->peek_byte = peek_byte;
    cpu->peek_word = peek_word;
    cpu->peek_long = peek_long;
    cpu->peek_ctx = ctx;
}

/*
 * addr から len バイトを非侵襲に読み出す。peek_byte 未登録なら -1。
 *
 * peek_byte はバスエラーを通知してはならない規約だが（debug.h参照）、
 * 規約違反のホスト実装から bus_error_pending が残留するのを検出・防止する
 * ため、呼び出し前後で値が変化していないことを assert し、Release ビルド
 * でも変化があれば元の値へ戻して握りつぶす（後続の無関係な命令実行時の
 * 誤ったバスエラー例外発火を防ぐ）。
 */
int mc68k_debug_read_physical_bytes(mc68k_state_t *cpu, uint32_t addr, uint8_t *buf,
                                    uint32_t len) {
    if (!cpu || !cpu->peek_byte)
        return -1;
    bool saved_bus_error_pending = cpu->bus_error_pending;
    uint32_t mask = cpu_has_68020_isa(cpu) ? 0xFFFFFFFFu : 0xFFFFFFu;
    for (uint32_t i = 0; i < len; i++)
        buf[i] = cpu->peek_byte(cpu->peek_ctx, (addr + i) & mask);
    assert(cpu->bus_error_pending == saved_bus_error_pending &&
           "peek_byte must not call mc68k_signal_bus_error()");
    cpu->bus_error_pending = saved_bus_error_pending;
    return 0;
}

int mc68k_debug_translate(mc68k_state_t *cpu, uint32_t logical, unsigned fc, int read,
                          mc68k_debug_translate_result_t *out) {
    if (!cpu || !out || fc > 7)
        return -1;
    const mc68k_access_t access = {.fc = (uint8_t)fc, .read = read != 0, .size = 1};
    mc68k_translate_result_t result =
        cpu_is_68040(cpu)
            ? cpu_mmu040_translate_result(cpu, logical, &access, MC68K_TRANSLATE_DEBUG)
        : cpu_is_68060(cpu)
            ? cpu_mmu060_translate_result(cpu, logical, &access, MC68K_TRANSLATE_DEBUG)
            : cpu_mmu_translate_result(cpu, logical, &access, MC68K_TRANSLATE_DEBUG);
    *out = (mc68k_debug_translate_result_t){
        .physical = result.physical,
        .mmusr = (cpu_is_68040(cpu) || cpu_is_68060(cpu)) ? 0 : result.mmusr,
        .level = result.level,
        .fault = (mc68k_debug_mmu_fault_t)result.fault,
    };
    return result.ok ? 0 : -1;
}

int mc68k_debug_read_bytes(mc68k_state_t *cpu, uint32_t addr, uint8_t *buf, uint32_t len) {
    if (!cpu)
        return -1;
    return mc68k_debug_read_bytes_fc(cpu, addr, buf, len, (cpu->sr & SR_S) ? 5u : 1u);
}

int mc68k_debug_read_bytes_fc(mc68k_state_t *cpu, uint32_t addr, uint8_t *buf, uint32_t len,
                              unsigned fc) {
    if (!cpu || !buf || fc > 7)
        return -1;
    for (uint32_t i = 0; i < len; ++i) {
        mc68k_debug_translate_result_t translated;
        if (mc68k_debug_translate(cpu, addr + i, fc, 1, &translated) != 0)
            return -1;
        /* 68040 copyback: dirty D-cache line は RAM より新しいため cache を優先する。 */
        if (cpu_040_cache_peek(cpu, translated.physical, &buf[i]))
            continue;
        if (mc68k_debug_read_physical_bytes(cpu, translated.physical, &buf[i], 1) != 0)
            return -1;
    }
    return 0;
}

/*
 * ブレークポイントを追加する。空きスロット（in_use==false）を線形走査して
 * 見つけた最初のインデックスをIDとして使う。奇数アドレスは命令先頭になり
 * 得ないため登録を拒否する。
 */
int mc68k_add_breakpoint(mc68k_state_t *cpu, uint32_t addr, mc68k_bp_cond_fn_t cond,
                         void *cond_ctx) {
    if (!cpu || (addr & 1))
        return -1;
    for (int i = 0; i < MC68K_MAX_BREAKPOINTS; i++) {
        if (!cpu->breakpoints[i].in_use) {
            cpu->breakpoints[i] = (mc68k_breakpoint_t){
                .addr = addr,
                .enabled = true,
                .in_use = true,
                .cond = cond,
                .cond_ctx = cond_ctx,
            };
            return i;
        }
    }
    return -1; /* MC68K_MAX_BREAKPOINTS 個すべて使用中 */
}

/* スロットを即座に完全クリアする（addr/cond/cond_ctxを含む）。無効なidは無視。 */
void mc68k_remove_breakpoint(mc68k_state_t *cpu, int id) {
    if (!cpu || id < 0 || id >= MC68K_MAX_BREAKPOINTS)
        return;
    cpu->breakpoints[id] = (mc68k_breakpoint_t){0};
}

void mc68k_enable_breakpoint(mc68k_state_t *cpu, int id, int enable) {
    if (!cpu || id < 0 || id >= MC68K_MAX_BREAKPOINTS || !cpu->breakpoints[id].in_use)
        return;
    cpu->breakpoints[id].enabled = (enable != 0);
}

void mc68k_clear_breakpoints(mc68k_state_t *cpu) {
    if (!cpu)
        return;
    memset(cpu->breakpoints, 0, sizeof(cpu->breakpoints));
}

/*
 * 現在のPC（cpu->pc）がヒットする有効なブレークポイントのIDを返す。
 * 同一アドレスに複数登録されている場合は最小のIDのみ返す（配列を昇順に
 * 走査するため自然に成立する）。呼び出し順序の規約はdebug.h参照。
 */
int mc68k_breakpoint_hit(mc68k_state_t *cpu) {
    if (!cpu)
        return -1;
    for (int i = 0; i < MC68K_MAX_BREAKPOINTS; i++) {
        mc68k_breakpoint_t *bp = &cpu->breakpoints[i];
        if (!bp->in_use || !bp->enabled || bp->addr != cpu->pc)
            continue;
        if (bp->cond && !bp->cond(cpu, bp->cond_ctx))
            continue;
        return i;
    }
    return -1;
}

void mc68k_set_exception_break_mask(mc68k_state_t *cpu, uint64_t vec_mask) {
    if (!cpu)
        return;
    cpu->exc_break_mask = vec_mask;
}

uint64_t mc68k_get_exception_break_mask(const mc68k_state_t *cpu) {
    return cpu ? cpu->exc_break_mask : 0;
}

/*
 * mc68k_execute() を1命令ずつ回し、BP/例外/停止状態をチェックする。
 * 停止条件の優先順位・各条件の意味はdebug.hのコメント参照。
 * mc68k_execute() 自体は無変更（ホットパス保護）。
 */
int mc68k_debug_run(mc68k_state_t *cpu, int max_cycles) {
    if (!cpu)
        return 0;
    cpu->stop_reason = MC68K_STOP_NONE;

    int total = 0;
    while (total < max_cycles) {
        /* last_exception_vector は累積フィールド（次に例外が起きるまで前回値が
         * 残る）。このステップで起きたことだけを判定するため、呼び出し直前に
         * 毎回リセットする（debug_run全体の入口で1回だけではない点に注意）。 */
        cpu->last_exception_vector = 0;
        total += mc68k_execute(cpu, 1);

        if (cpu->exec_state == CPU_HALTED) {
            cpu->stop_reason = MC68K_STOP_HALTED;
            break;
        }
        if (cpu->exec_state == CPU_STOPPED)
            break; /* IRQ待ちで前進不可。stop_reasonはNONEのまま */

        /* BP/WP判定は両方とも無条件に呼ぶ（先に見つかった方で打ち切ると、
         * ウォッチポイント側の前回値更新を取りこぼし次回検出が1ステップ
         * 遅れるため）。停止理由の決定だけ優先順位を付ける。 */
        int bp_id = mc68k_breakpoint_hit(cpu);
        int wp_id = mc68k_watchpoint_check(cpu);
        if (bp_id >= 0) {
            cpu->stop_reason = MC68K_STOP_BREAKPOINT;
            break;
        }
        if (wp_id >= 0) {
            cpu->stop_reason = MC68K_STOP_WATCHPOINT;
            break;
        }

        if (cpu->exec_state == CPU_EXCEPTION) {
            /* accept_irq() 内バス/アドレスエラーで保留のまま返ってきた稀な
             * ケース。次回mc68k_execute()冒頭で自動解消される想定だが、
             * ホストに一旦制御を返す。 */
            cpu->stop_reason = MC68K_STOP_EXCEPTION;
            break;
        }

        uint16_t v = cpu->last_exception_vector;
        if (v != 0 && ((cpu->exc_break_mask >> v) & 1u)) {
            cpu->stop_reason = (v == 9) ? MC68K_STOP_TRACE : MC68K_STOP_EXCEPTION;
            break;
        }
    }
    return total;
}

/* ================================================================== */
/* ウォッチポイント                              */
/* ================================================================== */

/* addr から size(1/2/4)バイトを非侵襲に読み、ビッグエンディアンで
 * uint32_t に組み立てる。読み出し失敗（peek未登録）ならfalse。 */
static bool wp_read_value(mc68k_state_t *cpu, uint32_t addr, int size, uint32_t *out) {
    uint8_t buf[4];
    if (mc68k_debug_read_bytes(cpu, addr, buf, (uint32_t)size) != 0)
        return false;
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v = (v << 8) | buf[i];
    *out = v;
    return true;
}

int mc68k_add_watchpoint(mc68k_state_t *cpu, uint32_t addr, int size) {
    if (!cpu || (size != 1 && size != 2 && size != 4))
        return -1;
    if (size != 1 && (addr & 1))
        return -1; /* ブレークポイントの奇数拒否と同じ理由 */
    if (!cpu->peek_byte)
        return -1; /* peek未登録では前回値を読めない */

    uint32_t initial;
    if (!wp_read_value(cpu, addr, size, &initial))
        return -1;

    for (int i = 0; i < MC68K_MAX_WATCHPOINTS; i++) {
        if (!cpu->watchpoints[i].in_use) {
            cpu->watchpoints[i] = (mc68k_watchpoint_t){
                .addr = addr,
                .size = size,
                .last_value = initial,
                .enabled = true,
                .in_use = true,
            };
            return i;
        }
    }
    return -1; /* MC68K_MAX_WATCHPOINTS 個すべて使用中 */
}

void mc68k_remove_watchpoint(mc68k_state_t *cpu, int id) {
    if (!cpu || id < 0 || id >= MC68K_MAX_WATCHPOINTS)
        return;
    cpu->watchpoints[id] = (mc68k_watchpoint_t){0};
}

void mc68k_enable_watchpoint(mc68k_state_t *cpu, int id, int enable) {
    if (!cpu || id < 0 || id >= MC68K_MAX_WATCHPOINTS || !cpu->watchpoints[id].in_use)
        return;
    cpu->watchpoints[id].enabled = (enable != 0);
}

void mc68k_clear_watchpoints(mc68k_state_t *cpu) {
    if (!cpu)
        return;
    memset(cpu->watchpoints, 0, sizeof(cpu->watchpoints));
}

/*
 * 全登録ウォッチポイントを無条件に走査・更新する（先に見つかった1件で
 * 打ち切らない。理由はdebug.hのコメント参照）。wp_hit_idはこの呼び出し内で
 * ヒットが無ければ-1にリセットする（古いヒット情報が残留しないように）。
 */
int mc68k_watchpoint_check(mc68k_state_t *cpu) {
    if (!cpu)
        return -1;

    cpu->wp_hit_id = -1;
    for (int i = 0; i < MC68K_MAX_WATCHPOINTS; i++) {
        mc68k_watchpoint_t *wp = &cpu->watchpoints[i];
        if (!wp->in_use || !wp->enabled)
            continue;
        uint32_t cur;
        if (!wp_read_value(cpu, wp->addr, wp->size, &cur))
            continue; /* peekが後から無効化された等。この1件はスキップ */
        if (cur != wp->last_value) {
            if (cpu->wp_hit_id < 0) {
                cpu->wp_hit_id = i;
                cpu->wp_hit_old_value = wp->last_value;
                cpu->wp_hit_new_value = cur;
            }
            wp->last_value = cur;
        }
    }
    return cpu->wp_hit_id;
}

int mc68k_get_watchpoint_hit(const mc68k_state_t *cpu, uint32_t *old_value,
                             uint32_t *new_value) {
    if (!cpu || cpu->wp_hit_id < 0)
        return -1;
    if (old_value)
        *old_value = cpu->wp_hit_old_value;
    if (new_value)
        *new_value = cpu->wp_hit_new_value;
    return cpu->wp_hit_id;
}

void cpu_rearm_watchpoints(mc68k_state_t *cpu) {
    if (!cpu || !cpu->peek_byte)
        return;
    for (int i = 0; i < MC68K_MAX_WATCHPOINTS; i++) {
        mc68k_watchpoint_t *wp = &cpu->watchpoints[i];
        if (!wp->in_use)
            continue;
        uint32_t cur;
        if (wp_read_value(cpu, wp->addr, wp->size, &cur))
            wp->last_value = cur;
    }
}

void mc68k_rearm_watchpoints(mc68k_state_t *cpu) {
    cpu_rearm_watchpoints(cpu);
}

void mc68k_clear_history_rings(mc68k_state_t *cpu) {
    if (!cpu)
        return;
    memset(cpu->pc_ring, 0, sizeof(cpu->pc_ring));
    cpu->pc_ring_pos = 0;
    cpu->pc_ring_count = 0;
    memset(cpu->reg_ring, 0, sizeof(cpu->reg_ring));
    cpu->reg_ring_pos = 0;
    cpu->reg_ring_count = 0;
    memset(cpu->call_ring, 0, sizeof(cpu->call_ring));
    cpu->call_ring_pos = 0;
    cpu->call_ring_count = 0;
    memset(cpu->fault_ring, 0, sizeof(cpu->fault_ring));
    cpu->fault_ring_pos = 0;
    cpu->fault_ring_count = 0;
}

/* ================================================================== */
/* 簡易バックトレース                            */
/* ================================================================== */

void mc68k_dump_call_ring(const mc68k_state_t *cpu, int count) {
    if (!cpu)
        return;
    if (count <= 0 || count > CALL_RING_SIZE)
        count = CALL_RING_SIZE;
    if (count > cpu->call_ring_count)
        count = cpu->call_ring_count;
    mc68k_state_t *c = (mc68k_state_t *)cpu; /* cpu_log()はlog_fn/log_ctxを読むのみ */
    int start = (cpu->call_ring_pos - count) & (CALL_RING_SIZE - 1);
    cpu_log(c, "=== call trace (last %d events, oldest first) ===\n", count);
    for (int i = 0; i < count; i++) {
        int idx = (start + i) & (CALL_RING_SIZE - 1);
        const mc68k_call_ring_entry_t *e = &cpu->call_ring[idx];
        cpu_log(c, " %s %06X -> %06X\n", e->is_return ? "RET " : "CALL", e->from_pc, e->to_pc);
    }
    cpu_log(c, "=== end call trace ===\n");
}

size_t mc68k_read_call_ring(const mc68k_state_t *cpu, mc68k_call_ring_entry_t *out,
                            size_t cap) {
    if (!cpu)
        return 0;
    if (!out)
        return (size_t)cpu->call_ring_count;
    int start;
    size_t n =
        ring_window(cpu->call_ring_pos, cpu->call_ring_count, CALL_RING_SIZE, cap, &start);
    for (size_t i = 0; i < n; i++)
        out[i] = cpu->call_ring[(start + (int)i) & (CALL_RING_SIZE - 1)];
    return n;
}

/* ================================================================== */
/* group0 フォールトリング                                 */
/* ================================================================== */

/* fault ring の記録の ON/OFF。既定オフ。履歴と書込み位置は保持する。 */
void mc68k_set_fault_ring(mc68k_state_t *cpu, int on) {
    if (!cpu)
        return;
    cpu->fault_ring_on = (on != 0);
}

int mc68k_get_fault_ring(const mc68k_state_t *cpu) {
    return cpu ? (cpu->fault_ring_on ? 1 : 0) : 0;
}

/* 公開構造体はホストが作ることもあるので、範囲外の値は "?" にする。 */
static const char *fault_kind_name(uint8_t v) {
    static const char *const names[] = {"OPC", "EXT", "DRD", "DWR"};
    return v < sizeof(names) / sizeof(names[0]) ? names[v] : "?";
}

static const char *fault_outcome_name(uint8_t v) {
    static const char *const names[] = {"NOT_BUILT", "FRAME_WRITE", "VECTOR_READ", "VECTORED"};
    return v < sizeof(names) / sizeof(names[0]) ? names[v] : "?";
}

static const char *fault_origin_name(uint8_t v) {
    static const char *const names[] = {"INSN", "IRQ", "EXC_FRAME"};
    return v < sizeof(names) / sizeof(names[0]) ? names[v] : "?";
}

static const char *fault_mmu_source_name(uint8_t v) {
    static const char *const names[] = {"NONE", "TT", "ATC", "WALK"};
    return v < sizeof(names) / sizeof(names[0]) ? names[v] : "?";
}

static const char *fault_mmu_fault_name(uint8_t v) {
    static const char *const names[] = {"NONE", "INVALID", "WP", "SUPER", "LIMIT", "TBERR"};
    return v < sizeof(names) / sizeof(names[0]) ? names[v] : "?";
}

/* fault ring をダンプする。各エントリ1行、mmu_valid のエントリは PMMU の2行目を足す
 * （行フォーマットは docs/mc68000-internals/reference/fault_ring.md）。 */
void mc68k_dump_fault_ring(const mc68k_state_t *cpu, int count) {
    if (!cpu)
        return;
    if (count <= 0 || count > FAULT_RING_SIZE)
        count = FAULT_RING_SIZE;
    if (count > cpu->fault_ring_count)
        count = cpu->fault_ring_count;
    mc68k_state_t *c = (mc68k_state_t *)cpu; /* cpu_log()はlog_fn/log_ctxを読むのみ */
    int start = (cpu->fault_ring_pos - count) & (FAULT_RING_SIZE - 1);
    cpu_log(c, "=== fault trace (last %d faults, oldest first) ===\n", count);
    for (int i = 0; i < count; i++) {
        const mc68k_fault_ring_entry_t *e =
            &cpu->fault_ring[(start + i) & (FAULT_RING_SIZE - 1)];
        char op[8];
        if (e->opcode_valid)
            snprintf(op, sizeof(op), "%04X", e->opcode);
        else
            snprintf(op, sizeof(op), "----");
        cpu_log(c,
                "seq=%llu pc=%08X op=%s vec=%u fmt=%X ssw=%04X addr=%08X fc=%u kind=%s "
                "size=%u %s rm=%u out=%s org=%s\n",
                (unsigned long long)e->seq, e->instr_pc, op, e->vector, e->frame_format, e->ssw,
                e->fault_addr, e->fc, fault_kind_name(e->access_kind), e->size,
                e->read ? "R" : "W", e->rm, fault_outcome_name(e->outcome),
                fault_origin_name(e->origin));
        if (!e->mmu_valid)
            continue;
        char level[8], desc[12], tberr[12];
        if (e->walk_valid)
            snprintf(level, sizeof(level), "%u", e->level);
        else
            snprintf(level, sizeof(level), "-");
        if (e->walk_valid && e->level >= 1)
            snprintf(desc, sizeof(desc), "%08X", e->descriptor_physical);
        else
            snprintf(desc, sizeof(desc), "-");
        if (e->walk_valid && e->mmu_fault == MC68K_DEBUG_MMU_TABLE_BUS_ERROR)
            snprintf(tberr, sizeof(tberr), "%08X", e->table_bus_error_physical);
        else
            snprintf(tberr, sizeof(tberr), "-");
        cpu_log(
            c,
            "  mmu src=%s fault=%s level=%s desc=%s tberr=%s tc=%08X crp=%016llX srp=%016llX\n",
            fault_mmu_source_name(e->mmu_source), fault_mmu_fault_name(e->mmu_fault), level,
            desc, tberr, e->tc, (unsigned long long)e->crp, (unsigned long long)e->srp);
    }
    cpu_log(c, "=== end fault trace ===\n");
}

size_t mc68k_read_fault_ring(const mc68k_state_t *cpu, mc68k_fault_ring_entry_t *out,
                             size_t cap) {
    if (!cpu)
        return 0;
    if (!out)
        return (size_t)cpu->fault_ring_count;
    int start;
    size_t n =
        ring_window(cpu->fault_ring_pos, cpu->fault_ring_count, FAULT_RING_SIZE, cap, &start);
    for (size_t i = 0; i < n; i++)
        out[i] = cpu->fault_ring[(start + (int)i) & (FAULT_RING_SIZE - 1)];
    return n;
}

/* ================================================================== */
/* n命令後停止カウンタ                           */
/* ================================================================== */

int mc68k_debug_run_n(mc68k_state_t *cpu, int num_instructions) {
    if (!cpu || num_instructions <= 0)
        return 0;
    int total = 0;
    for (int i = 0; i < num_instructions; i++) {
        total += mc68k_debug_run(cpu, 1);
        if (cpu->stop_reason != MC68K_STOP_NONE)
            break;
        if (cpu->exec_state != CPU_NORMAL)
            break; /* HALTED/STOPPEDで前進不可 */
    }
    return total;
}
