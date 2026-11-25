/*
 * ops_system.c — システム制御・特権命令ハンドラ実装
 *
 * 実装する命令:
 *   - NOP / RTE / RTR       : なにもしない / 例外からリターン / CCR のみ復元してリターン
 *   - TAS                   : テスト＆セット（不可分 RMW）
 *   - RESET / STOP / TRAPV  : 特権命令群
 *   - MOVE An,USP / USP,An  : ユーザ SP の直接操作（スーパーバイザ専用）
 */
#include "cpu_internal.h"
#include <stdint.h>

/* ================================================================== */
/* NOP (0x4E71) / RTE (0x4E73)                                        */
/* ================================================================== */

static int op_nop(mc68k_state_t *cpu, uint16_t opcode) {
    (void)cpu;
    (void)opcode;
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 15); /* 040UM Table 10-5: calculate 8 + execute base 7 */
    return (cpu_is_68020(cpu) || cpu_is_68030_family(cpu)) ? 2 : 4;
}

/* 040UM 表 10-5: 単一 RTE フレームの公称時間。format-$1 経路はスタックを切替え、
 * 選択フレームを再帰処理するため、下記の既存合成計上を維持する。 */
static int rte040_nominal_cycles(int format) {
    switch (format) {
    case 0:
        return 15; /* calculate 2 + execute 13 */
    case 2:
        return 16; /* calculate 2 + execute 14 */
    case 3:
        return 23; /* calculate 3 + execute 20 */
    case 4:
        return 17; /* calculate 2 + execute 15 (EC/LC only) */
    case 7:
        return 27; /* calculate 4 + execute 23 */
    default:
        return 0;
    }
}

/*
 * RTE（Return from Exception）:
 * 例外処理ルーティンの末尾に置く。スタックに積まれた SR と PC を復元する。
 * TRAP → op_trap がスタックに積んだフレームを、ここで解除する。
 * X68000 の割り込みハンドラはすべてこれで終わる。
 */
static int op_rte_body(mc68k_state_t *cpu, uint16_t opcode);

/* 060: RTE の stack 解放（read）は D-cache に allocate しない（[060UM §7.7.1]）。 */
static int op_rte(mc68k_state_t *cpu, uint16_t opcode) {
    const bool saved = cpu->cache_no_allocate;
    cpu->cache_no_allocate = true;
    const int cycles = op_rte_body(cpu, opcode);
    cpu->cache_no_allocate = saved;
    return cycles;
}

static int op_rte_body(mc68k_state_t *cpu, uint16_t opcode) {
    /* RTE が format $7 を in place で書き換える書き込みは、構築中のフレームであって
     * 命令の保留ストア（WB2）ではない。 */
    cpu->wb2_defer_ok = false;
    (void)opcode;
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu); /* 特権違反: スタック未変化 */

    if (!cpu_has_68010_isa(cpu)) {
        /* フレーム: [SP]=SR(2バイト), [SP+2]=PC(4バイト) */
        uint16_t new_sr = cpu_read_word(cpu, cpu->a[7]);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        cpu->a[7] += 2;
        uint32_t new_pc = cpu_read_long(cpu, cpu->a[7]);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        cpu->a[7] += 4;
        /* SR 復元時に S ビットが 1→0（スーパーバイザ→ユーザー）へ変わる場合、
         * 現在の A7（SSP）を退避し USP へ切り替える。これを怠ると RTE 後も
         * A7 が SSP を指したままになり、ユーザーモードのスタックが破壊される。 */
        cpu->pc = new_pc;
        cpu_set_sr(cpu, new_sr);
        /* [UM00 Table 8-14] / [DS10 Table 7-21] "RTE Short": 68000=20、
         * 68010=24（CYC_RTE_SHORT） */
        return CYC(cpu, CYC_RTE_SHORT);
    }

    if (cpu_has_68020_isa(cpu)) {
        uint16_t fmt = cpu_read_word(cpu, cpu->a[7] + 6);
        /* Format はフレーム検証である。ここでのバスフォルト自体は vector 2 として配送し、
         * 下の状態復元アクセスでは代わりに二重フォルトとする。 */
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        int format = fmt >> 12;
        if (cpu_is_68060(cpu)) {
            if (format != 0 && format != 2 && format != 3 && format != 4)
                return cpu_dispatch_exception(cpu, cpu->pc - 2, 14, 50);
            uint16_t new_sr = cpu_read_word(cpu, cpu->a[7]);
            if (cpu->exec_state != CPU_NORMAL)
                return 4;
            uint32_t new_pc = cpu_read_long(cpu, cpu->a[7] + 2);
            if (cpu->exec_state != CPU_NORMAL)
                return 8;
            /* frame 検証の次に PC を検査する。奇数なら RTE 実行前の状態のまま address error。
             */
            if (new_pc & 1) {
                cpu_signal_prefetch_address_error(cpu, new_pc);
                return 8;
            }
            /* $4 は SR/PC だけを復帰する（restart。write-back 状態はない）。 */
            uint32_t bytes = format == 0 ? 8u : format == 4 ? 16u : 12u;
            uint32_t frame_base = cpu->a[7];
            cpu->a[7] += bytes;
            cpu->pc = new_pc;
            if (!cpu_exception_frame_origin_rte_commit(cpu, frame_base, new_sr))
                cpu_set_sr(cpu, new_sr);
            cpu->flow_changed = true;
            return 20;
        }
        if (cpu_is_68040(cpu)) {
            /* [040UM §8.4]: MC68040 が認識するのは $0/$1/$2/$3/$7 だけである。
             * 特に $4 は LC/EC 系用、$A/$B は 68020/68030 用なので、どちらも暗黙に
             * 既存ハンドラを再利用してはならない。 */
            if (format == 1) {
                uint16_t new_sr = cpu_read_word(cpu, cpu->a[7]);
                if (cpu->exec_state != CPU_NORMAL)
                    return 4;
                cpu->a[7] += 8;
                cpu_set_sr(cpu, new_sr);
                cpu->flow_changed = true;
                return 16 + op_rte(cpu, opcode);
            }
            /* [040UM §8.4.5, Appendix B.5.2]: format $4 は MC68EC040/LC040 だけが読む。 */
            if (format == 0 || format == 2 || format == 3 || format == 7 ||
                (format == 4 && cpu->fpu_config == MC68K_FPU_NONE_040)) {
                uint16_t new_sr = cpu_read_word(cpu, cpu->a[7]);
                if (cpu->exec_state != CPU_NORMAL)
                    return 4;
                uint32_t new_pc = cpu_read_long(cpu, cpu->a[7] + 2);
                if (cpu->exec_state != CPU_NORMAL)
                    return 8;
                uint32_t bytes = format == 0 ? 8u : format == 7 ? 60u : format == 4 ? 16u : 12u;
                uint32_t frame_base = cpu->a[7];
                uint32_t effective_address = 0;
                uint16_t ssw = 0;
                /* RTE validates only offsets $0-$D of a format $7 frame
                 * ([040UM §8.4.6.7]); do not read guest-owned write-back or
                 * push-data fields here. */
                if (format == 7) {
                    effective_address = cpu_read_long(cpu, frame_base + 8);
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                    ssw = cpu_read_word(cpu, frame_base + 12);
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                }
                /* [040UM §8.4.6.7]: CT and CU convert the access-error
                 * frame in place to a six-word frame.  Offsets $30-$3B are
                 * exactly where the manual relocates the first six words.
                 * CU is an unimplemented-FP exception (F-line, vector 11);
                 * its saved PC is already the next instruction. */
                if (format == 7 && ((ssw & 0xF000u) == 0x2000u || (ssw & 0xF000u) == 0x4000u)) {
                    uint16_t vector = (ssw & 0x4000u) ? 11u : 9u;
                    uint32_t trace_base = frame_base + 48;
                    cpu_write_word(cpu, trace_base, new_sr);
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                    cpu_write_long(cpu, trace_base + 2, new_pc);
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                    cpu_write_word(cpu, trace_base + 6, (uint16_t)(0x2000u | vector * 4u));
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                    cpu_write_long(cpu, trace_base + 8, effective_address);
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                    cpu->a[7] = trace_base;
                    cpu_set_sr(cpu, (uint16_t)((new_sr | SR_S) & ~(SR_T1 | SR_T0)));
                    uint32_t trace_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));
                    if (cpu->exec_state != CPU_NORMAL)
                        return 12;
                    cpu->pc = trace_pc;
                    cpu->last_exception_vector = vector;
                    cpu->flow_changed = true;
                    return 20;
                }
                /* [040UM §8.4.4、§8.4.6.7]: CP は保留中の FPU 命令後例外を format $3 として
                 * 再生成する。例外ベクタはパイプライン状態であり format-$7
                 * フレームの欄ではないため、 そのフレーム構築時にも保持する。format $3 は 6
                 * 語でフォルト PC を持たない。 */
                if (format == 7 && (ssw & 0xF000u) == 0x8000u && cpu->fpu040_post_pending) {
                    uint32_t post_base = frame_base + 48; /* format $3 は 12 バイト */
                    uint16_t vector = cpu->fpu040_post_vector;
                    cpu_write_word(cpu, post_base, new_sr);
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                    cpu_write_long(cpu, post_base + 2, new_pc);
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                    cpu_write_word(cpu, post_base + 6, (uint16_t)(0x3000u | vector * 4u));
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                    cpu_write_long(cpu, post_base + 8, effective_address);
                    if (cpu->exec_state != CPU_NORMAL)
                        return 8;
                    cpu->a[7] = post_base;
                    cpu_set_sr(cpu, (uint16_t)((new_sr | SR_S) & ~(SR_T1 | SR_T0)));
                    uint32_t post_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));
                    if (cpu->exec_state != CPU_NORMAL)
                        return 12;
                    cpu->pc = post_pc;
                    cpu->last_exception_vector = vector;
                    cpu->fpu040_post_pending = false;
                    cpu->flow_changed = true;
                    return 20;
                }
                /* CM は実効アドレス計算後の整数 MOVEM を再開する。MOVEM ハンドラは
                 * 拡張語を再度消費するが、保存済みアドレスに置き換える。 */
                if (format == 7 && (ssw & 0xF000u) == 0x1000u &&
                    cpu->movem040_continuation_valid) {
                    cpu->a[7] += bytes;
                    cpu->pc = cpu->movem040_instr_pc;
                    if (!cpu_exception_frame_origin_rte_commit(cpu, frame_base, new_sr))
                        cpu_set_sr(cpu, new_sr);
                    cpu->movem040_resume = true;
                    cpu->flow_changed = true;
                    return 20;
                }
                cpu->a[7] += bytes;
                cpu->pc = new_pc;
                if (!cpu_exception_frame_origin_rte_commit(cpu, frame_base, new_sr))
                    cpu_set_sr(cpu, new_sr);
                cpu->flow_changed = true;
                return cpu_040_tag(cpu, rte040_nominal_cycles(format));
            }
            return cpu_dispatch_exception(cpu, cpu->pc - 2, 14, 50);
        }
        if (format == 1) {
            /* Throwaway frame: discard it, restore the master-stack state,
             * then process the frame now selected by that restored SR. */
            uint16_t new_sr = cpu_read_word(cpu, cpu->a[7]);
            if (cpu->exec_state != CPU_NORMAL) {
                cpu->exec_state = CPU_HALTED;
                return 4;
            }
            cpu->a[7] += 8;
            cpu_set_sr(cpu, new_sr);
            cpu->flow_changed = true;
            /* 68020+ は throwaway frame の破棄と、復帰した M-stack 上の
             * 実フレーム処理を一命令で続ける。UM30 §11.6.18 の Throwaway
             * 12 は前半だけなので、内側の RTE の ordinal と合算する。 */
            return (cpu_is_68030_family(cpu) ? 12 : 16) + op_rte(cpu, opcode);
        }
        if (format == 0 || format == 2 || format == 9 || format == 10 || format == 11) {
            uint16_t new_sr = cpu_read_word(cpu, cpu->a[7]);
            if (cpu->exec_state != CPU_NORMAL) {
                cpu->exec_state = CPU_HALTED;
                return 4;
            }
            uint32_t new_pc = cpu_read_long(cpu, cpu->a[7] + 2);
            if (cpu->exec_state != CPU_NORMAL) {
                cpu->exec_state = CPU_HALTED;
                return 8;
            }
            /* format 9（コプロセッサの mid-instruction フレーム）は、中断された対話の再開に
             * 命令先頭・オペレーションワード・実効アドレスを使う。 */
            uint32_t cp_instr_pc = 0, cp_ea = 0;
            uint16_t cp_opword = 0;
            if (format == 9) {
                cp_instr_pc = cpu_read_long(cpu, cpu->a[7] + 8);
                if (cpu->exec_state != CPU_NORMAL) {
                    cpu->exec_state = CPU_HALTED;
                    return 8;
                }
                cp_opword = cpu_read_word(cpu, cpu->a[7] + 14);
                if (cpu->exec_state != CPU_NORMAL) {
                    cpu->exec_state = CPU_HALTED;
                    return 8;
                }
                cp_ea = cpu_read_long(cpu, cpu->a[7] + 16);
                if (cpu->exec_state != CPU_NORMAL) {
                    cpu->exec_state = CPU_HALTED;
                    return 8;
                }
            }
            int bytes = format == 0    ? 8
                        : format == 2  ? 12
                        : format == 9  ? 20
                        : format == 10 ? 32
                                       : 92;
            uint32_t frame_base = cpu->a[7];
            cpu->a[7] += (uint32_t)bytes;
            cpu->pc = new_pc;
            if (!cpu_exception_frame_origin_rte_commit(cpu, frame_base, new_sr))
                cpu_set_sr(cpu, new_sr);
            cpu->flow_changed = true;
            int cycles = 20;
            if (cpu_is_68020(cpu))
                cycles = format == 9 ? 32 : format == 10 ? 43 : format == 11 ? 92 : 21;
            else if (cpu_is_68030_family(cpu))
                cycles = format == 0 || format == 2 ? 18
                         : format == 1              ? 12
                         : format == 9              ? 26
                         : format == 10             ? 36
                                                    : 76;
            if (format == 9)
                cycles += coproc_resume(cpu, cp_opword, cp_instr_pc, cp_ea);
            return cycles;
        }
        return cpu_dispatch_exception(cpu, cpu->pc - 2, 14, 50);
    }

    /* 68010+: フォーマットワード付き可変長フレーム。
     * まず SP+6 のフォーマットワードを読む（この時点では SP を動かさない）。 */
    uint16_t fmt = cpu_read_word(cpu, cpu->a[7] + 6);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    int format = (fmt >> 12) & 0xF;

    if (format == 0) {
        uint16_t new_sr = cpu_read_word(cpu, cpu->a[7]);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t new_pc = cpu_read_long(cpu, cpu->a[7] + 2);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        cpu->a[7] += 8;
        cpu->pc = new_pc;
        cpu_set_sr(cpu, new_sr);
        cpu->flow_changed = true;
        /* [DS10 Table 7-21] "RTE Short"（format $0）: 68010=24（CYC_RTE_SHORT）。
         * このコードパスは model==68010 のときのみ到達する。 */
        return CYC(cpu, CYC_RTE_SHORT);
    }

    if (format == 8) {
        /* グループ0長フレーム（29ワード=58バイト）: 命令全体を命令開始時の
         * レジスタ状態から再実行する近似。
         * 再フォルト安全弁のラッチをここで立てる。 */
        uint16_t new_sr = cpu_read_word(cpu, cpu->a[7]);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t new_pc = cpu_read_long(cpu, cpu->a[7] + 2);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        /* SSW（SP+$08）: RTEのサイクル区分（下記）に使うだけで、フレームの
         * 他フィールド同様、実際の再実行内容には影響しない（A-5、design_mc68010_
         * bus_error_retry.md §4.3）。 */
        uint16_t ssw = cpu_read_word(cpu, cpu->a[7] + 8);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        const uint32_t frame_base = cpu->a[7];
        cpu->a[7] += 58;
        cpu->g0_rte_armed = true;
        cpu->g0_rte_pc = new_pc;
        cpu->pc = new_pc;
        /* 例外処理（frame／vector）の途中のフォルトで積まれたフレームなら、途中まで積んだ
         * 例外フレームごと origin のスタック状態へ戻す。 */
        if (!cpu_exception_frame_origin_rte_commit(cpu, frame_base, new_sr))
            cpu_set_sr(cpu, new_sr);
        cpu->flow_changed = true;
        /* [DS10 Table 7-21] RTE Long の3区分をSSW.RR(bit15)/RW(bit8)から選ぶ
         * 。このコードパスは
         * model==68010 のときのみ到達する（68000は本分岐に到達せず、68020+は
         * format8を別経路のフォーマットエラーとして扱うため、他プロファイル行は
         * CYC_ABSENT）。 */
        bool software_rerun = (ssw & 0x8000u) != 0; /* RR=1: ハンドラが再実行済み */
        if (software_rerun)
            return CYC(cpu, CYC_RTE_LONG_NORETRY);
        bool fault_was_read = (ssw & 0x0100u) != 0; /* RW=1: 読み出しフォルト */
        return CYC(cpu, fault_was_read ? CYC_RTE_LONG_RETRY_READ : CYC_RTE_LONG_RETRY_WRITE);
    }

    /* その他: フォーマットエラー例外（ベクタ14、Group2）。SP は RTE 実行前の
     * 値のまま、積む PC は RTE 命令自身のアドレス（cpu->pc - 2）。 */
    return cpu_dispatch_exception(cpu, cpu->pc - 2, 14, 50);
}

/* ================================================================== */
/* TAS（テスト＆セット、不可分 RMW）                                   */
/* ================================================================== */

/*
 * TAS EA: EA のバイトをテストし（CCR 更新）、bit 7 をセットする。
 * 本来はバスサイクルが不可分だが、X68000 では通常の同期で十分。
 */
static int op_tas(mc68k_state_t *cpu, uint16_t opcode) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t rmw_addr;
    /* TAS は実際にロックされた RMW バスサイクルを持つ唯一の MC68000 基準命令であり、
     * locked=true を渡すのはこれだけである。アクセスディスクリプタだけを cpu->fault_access.rmw
     * の 情報源とする。別途手動で切替える必要はなく（下記 exec_state 検査前に書込みサイクルの
     * フォルトが起こるため、切替えれば誤りになる）。 */
    uint32_t val = ea_rmw_read(cpu, ea_mode, ea_reg, 1, &rmw_addr, true);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!(val & 0xFF))
        cpu->sr |= CCR_Z;
    if (val & 0x80)
        cpu->sr |= CCR_N;

    ea_rmw_write(cpu, ea_mode, ea_reg, 1, rmw_addr, val | 0x80U, true);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    if (cpu_is_68020(cpu))
        return ea_mode == 0 ? 4 : 12 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return ea_mode == 0 ? 4 : 12 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    return (ea_mode == 0) ? 4 : 14 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/* ================================================================== */
/* RTR（CCR のみ復元してリターン）                                      */
/* ================================================================== */

static int op_rtr(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    uint16_t new_ccr = cpu_read_word(cpu, cpu->a[7]);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu->a[7] += 2;
    uint32_t new_pc = cpu_read_long(cpu, cpu->a[7]);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    cpu->a[7] += 4;
    cpu->sr = (cpu->sr & 0xFF00U) | (new_ccr & 0x001FU); /* CCR 5bit のみ更新 */
    cpu->pc = new_pc;
    cpu->flow_changed = true;
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 13); /* 040UM Table 10-5: calculate 7 + execute base 6 */
    return cpu_is_68030_family(cpu) ? 12 : cpu_is_68020(cpu) ? 14 : 20;
}

/* ================================================================== */
/* RESET / STOP / TRAPV / MOVE USP（特権命令群）                       */
/* ================================================================== */

/* RESET (0x4E70): 外部 RESET ピンを 124 クロックアサート。
 * エミュレータでは外部バスへの影響がないため、サイクル消費のみ。 */
static int op_reset_instr(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu); /* 特権違反: 発火させない */
    if (cpu->bus.reset_peripheral)
        cpu->bus.reset_peripheral(cpu->bus.ctx);
    /* [UM00 Table 8-21] / [DS10 Table 7-21]: 68000=132、68010=130
     * （CYC_RESET_INSTR） */
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 1042); /* 040UM Table 10-5: calculate 521 + execute 521 */
    return (cpu_is_68020(cpu) || cpu_is_68030_family(cpu)) ? 518 : CYC(cpu, CYC_RESET_INSTR);
}

/* STOP #imm (0x4E72): 次ワードを SR にロードして CPU を停止。
 * 割り込みマスクを超える IRQ、または RESET で再開する。 */
static int op_stop(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu); /* 特権違反: 即値未消費 */
    uint16_t imm = cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    bool traced = (cpu->sr & (SR_T1 | SR_T0)) != 0;
    cpu_set_sr(cpu, imm);
    cpu->flow_changed = true;
    if (!traced)
        cpu->exec_state = CPU_STOPPED;
    return (cpu_is_68020(cpu) || cpu_is_68030_family(cpu)) ? 8 : 4;
}

/* TRAPV (0x4E76): V フラグが立っていれば TRAPV 例外（ベクタ 7）を発生。
 * V=0 の場合は何もしない（NOP 相当）。 */
static int op_trapv(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    if (!(cpu->sr & CCR_V))
        return cpu_is_68040(cpu) ? cpu_040_tag(cpu, 10) : 4;
    /* ベクタ 7。[DS10 Table 7-22]: 68010=40（CYC_EXC_TRAPV） */
    return cpu_dispatch_exception(cpu, cpu->pc, 7,
                                  cpu_is_68040(cpu)          ? cpu_040_tag(cpu, 38)
                                  : cpu_is_68030_family(cpu) ? 22
                                  : cpu_is_68020(cpu)        ? 25
                                                             : CYC(cpu, CYC_EXC_TRAPV));
}

/* MOVE An, USP (0x4E60+n) / MOVE USP, An (0x4E68+n):
 * スーパーバイザモード専用。ユーザ SP を直接操作する。 */
static int op_move_usp(mc68k_state_t *cpu, uint16_t opcode) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu); /* 特権違反 */
    int an = opcode & 7;
    int dir = (opcode >> 3) & 1; /* 0 = An→USP, 1 = USP→An */
    if (dir == 0)
        cpu->usp = cpu->a[an];
    else
        cpu->a[an] = cpu->usp;
    /* [UM00 Table 8-21] / [DS10 Table 7-21]: MOVE from/to USP は
     * 68000=4、68010=6（両方向とも同値。CYC_MOVE_USP） */
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, dir == 0 ? 13 : 4); /* 040UM Table 10-5: An→USP / USP→An */
    return CYC(cpu, CYC_MOVE_USP);
}

/* ================================================================== */
/* テーブル登録                                                        */
/* ================================================================== */

void register_system_ops(op_handler_t *table) {
    int an;

    /* ---- NOP / RTE ---- */
    table[0x4E71] = op_nop;
    table[0x4E73] = op_rte;

    /* ---- MOVE USP: An→USP (0x4E60-0x4E67) / USP→An (0x4E68-0x4E6F) ---- */
    for (an = 0; an < 8; an++) {
        table[0x4E60 | an] = op_move_usp;
        table[0x4E68 | an] = op_move_usp;
    }

    /* ---- RESET (0x4E70) / STOP (0x4E72) / TRAPV (0x4E76) ---- */
    table[0x4E70] = op_reset_instr;
    table[0x4E72] = op_stop;
    table[0x4E76] = op_trapv;

    /* ---- TAS (0x4AC0-0x4AFF, dst=Data Alterable) ---- */
    for (int ea = 0; ea < 64; ea++) {
        if (ea_is_data_alterable(ea))
            table[0x4AC0 | ea] = op_tas;
    }

    /* ---- RTR (0x4E77) ---- */
    table[0x4E77] = op_rtr;
}
