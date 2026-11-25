/*
 * m68000.c — MC68000 ライフサイクルと実行ループ
 *
 * 実行方式: 命令カウント精度（instruction-count accuracy）
 *   命令ごとに「公称サイクル数」を加算していく素直な方式。
 *   DMA タイミングが厳密を要求するソフトで問題が出た場合は
 *   bus.sync() をバスアクセス毎に呼ぶ「バスサイクル精度」に移行する。
 *   bus.sync() を最初から API に組み込んでいるのはそのため。
 *
 * 割り込み処理:
 *   MC68000 は命令境界でのみ割り込みを受け付ける（命令途中では割り込まない）。
 *   2つのモデルをサポートする:
 *     push/latch型（既定）: mc68k_set_irq() が pending_irq へ積むだけで、
 *       実際の受付判定はここのループで pending_irq を読んで行う。
 *     pull型（mc68k_set_irq_level_hook()登録時）: ループが命令境界ごとに
 *       ホストへ現在のIRQレベルを問い合わせる（irq_sample_level()）。
 *       実機のIPL0-2はlevel-sensitiveで命令境界ごとに再サンプリングされる
 *       のが正しい挙動であり、push/latch型はホスト側のIRQ線更新が
 *       ポーリング粒度でしか行えない場合の負債（古いIRQ線を受理してしまう
 *       窓が生じる）を負う。pull型はこの窓を構造的になくす。
 */
#include "cpu_internal.h"
#include "mc68000/cpu.h"
#include "fpu/fpu_model.h"
#include <stdlib.h>
#include <string.h>

/* この翻訳単位における reset 以外の CPU バスサイクルは、割込み／例外フレーム、
 * スタック、ベクタのサイクルである。これらは明示的にスーパーバイザデータサイクルとし、
 * reset は意図的に下記の生の物理バス列のままとする。 */
#define cpu_read_byte  cpu_supervisor_read_byte
#define cpu_read_word  cpu_supervisor_read_word
#define cpu_read_long  cpu_supervisor_read_long
#define cpu_write_byte cpu_supervisor_write_byte
#define cpu_write_word cpu_supervisor_write_word
#define cpu_write_long cpu_supervisor_write_long

bool cpu_exception_frame_origin_begin(mc68k_state_t *cpu, uint16_t saved_sr,
                                      uint32_t saved_pc) {
    if (!cpu_has_68010_isa(cpu))
        return true;
    mc68k_exception_frame_origin_t *r = calloc(1, sizeof(*r));
    if (!r) {
        cpu_log(cpu, "EXCEPTION-FRAME-ORIGIN-OOM pc=%08X\n", saved_pc);
        cpu->exec_state = CPU_HALTED;
        return false;
    }
    r->saved_sr = saved_sr;
    r->saved_pc = saved_pc;
    r->a7 = cpu->a[7];
    /* cpu_set_sr() は S/M が切り替わるときにしか影の値を書かないため、現在使っているスタックの
     * 影の値は古いままである。RTE での復元（rte_commit）に使うので、生きている A7 で補う。 */
    r->usp = cpu_sp_slot(cpu, cpu->sr) == &cpu->usp ? cpu->a[7] : cpu->usp;
    r->ssp = cpu_sp_slot(cpu, cpu->sr) == &cpu->ssp ? cpu->a[7] : cpu->ssp;
    r->msp = cpu_sp_slot(cpu, cpu->sr) == &cpu->msp ? cpu->a[7] : cpu->msp;
    r->next = cpu->exception_frame_origins;
    cpu->exception_frame_origins = r;
    return true;
}

void cpu_exception_frame_origin_complete(mc68k_state_t *cpu) {
    if (cpu_is_68060(cpu))
        cpu->irq_defer =
            true; /* 例外処理の完了。handler 先頭 1 命令の完了まで割込みを遅らせる */
    if (!cpu_has_68010_isa(cpu))
        return;
    mc68k_exception_frame_origin_t *r = cpu->exception_frame_origins;
    if (r && !r->bound) {
        cpu->exception_frame_origins = r->next;
        free(r);
    }
}

void cpu_exception_frame_origins_clear(mc68k_state_t *cpu) {
    while (cpu->exception_frame_origins) {
        mc68k_exception_frame_origin_t *r = cpu->exception_frame_origins;
        cpu->exception_frame_origins = r->next;
        free(r);
    }
}

bool cpu_exception_frame_origin_rte_commit(mc68k_state_t *cpu, uint32_t frame_base,
                                           uint16_t new_sr) {
    mc68k_exception_frame_origin_t *r = cpu->exception_frame_origins;
    if (!r || !r->bound || r->group0_base != frame_base)
        return false;
    cpu->exception_frame_origins = r->next;
    cpu->usp = r->usp;
    cpu->ssp = r->ssp;
    cpu->msp = r->msp;
    new_sr = cpu_sr_storable(cpu, new_sr);
    cpu->sr = new_sr;
    if (!(new_sr & SR_S))
        cpu->a[7] = cpu->usp;
    else if (cpu_has_master_stack(cpu) && (new_sr & SR_M))
        cpu->a[7] = cpu->msp;
    else
        cpu->a[7] = cpu->ssp;
    free(r);
    return true;
}

mc68k_state_t *mc68k_create(const mc68k_bus_t *bus) {
    return mc68k_create_model(bus, MC68K_MODEL_68000);
}

/* FPU 構成の受入表。 */
static bool fpu_config_accepted(mc68k_model_t model, mc68k_fpu_config_t fpu) {
    switch (fpu) {
    case MC68K_FPU_NONE:
        return true;
    case MC68K_FPU_68881:
    case MC68K_FPU_68882:
        return model == MC68K_MODEL_68020 || model == MC68K_MODEL_68EC030 ||
               model == MC68K_MODEL_68030;
    case MC68K_FPU_68040:
    case MC68K_FPU_NONE_040:
        return model == MC68K_MODEL_68040;
    case MC68K_FPU_68060:
        return model == MC68K_MODEL_68060;
    case MC68K_FPU_NONE_060: /* LC/EC060 は段階 2。全機種で拒否する */
        return false;
    }
    return false;
}

mc68k_state_t *mc68k_create_model(const mc68k_bus_t *bus, mc68k_model_t model) {
    return mc68k_create_model_with_fpu(bus, model, MC68K_FPU_NONE);
}

mc68k_state_t *mc68k_create_model_with_fpu(const mc68k_bus_t *bus, mc68k_model_t model,
                                           mc68k_fpu_config_t fpu) {
    if (!fpu_config_accepted(model, fpu))
        return NULL;
    mc68k_state_t *cpu = calloc(1, sizeof(*cpu));
    if (!cpu)
        return NULL;

    cpu->fpu_config = fpu;
    cpu->fpu = mc68k_fpu_create(fpu); /* 68881/68882 以外は NULL を返す */
    if ((fpu == MC68K_FPU_68881 || fpu == MC68K_FPU_68882) && !cpu->fpu) {
        free(cpu);
        return NULL;
    }
    if (fpu == MC68K_FPU_68040 || fpu == MC68K_FPU_68060) {
        cpu->fpu040 = calloc(1, sizeof(*cpu->fpu040));
        if (!cpu->fpu040) {
            mc68k_fpu_destroy(cpu->fpu);
            free(cpu);
            return NULL;
        }
        mc68k_fpu040_reset(cpu->fpu040);
    }

    cpu->model = model;
    cpu->bus = *bus; /* コールバックテーブルを値コピー。bus の寿命を気にしなくていい */

    /* デコードテーブルはヒープに確保（関数ポインタ × 65536）。全エントリを ILLEGAL で初期化 */
    cpu->op_table = calloc(65536, sizeof(*cpu->op_table));
    if (!cpu->op_table) {
        free(cpu->fpu040);
        mc68k_fpu_destroy(cpu->fpu);
        free(cpu);
        return NULL;
    }

    build_op_table(cpu->op_table, model);

    /* モデル別サイクルタイミング表を解決する。build_op_table() と
     * 同じ場所（モデル確定直後）から呼ぶのは、cpu->model への代入がここ1箇所
     * しかなく、以降 mc68k_reset() 等でも変わらないため（design_mc680x0_
     * cycle_timing.md §5.4）。 */
    cyc_resolve(cpu);

    /* wp_hit_id は「ヒットなし」を-1で表す（calloc既定の0だと
     * ウォッチポイント0のヒットと区別できないため明示的に初期化する）。 */
    cpu->wp_hit_id = -1;

    return cpu;
}

void mc68k_destroy(mc68k_state_t *cpu) {
    if (!cpu)
        return;
    free(cpu->entry_probe_saved_table);
    cpu_exception_frame_origins_clear(cpu);
    free(cpu->op_table);
    free(cpu->fpu040);
    mc68k_fpu_destroy(cpu->fpu);
    free(cpu);
}

void mc68k_reset(mc68k_state_t *cpu) {
    /*
     * リセットシーケンス（実機の動作）:
     *   1. 強制的にスーパーバイザモードへ移行、割り込みマスクを 7 に設定
     *   2. ベクタテーブルの先頭 8 バイトから SP と PC を読む
     *      0x000000: SSP（スーパーバイザスタックポインタ）
     *      0x000004: PC（プログラムカウンタ）
     *   3. 読み込んだ値でレジスタを初期化
     *
     * D0–D7, A0–A6 の値は不定（実機でもリセット後は保証されない）。
     * ここではデバッグしやすいようにゼロクリアしておく。
     */
    memset(cpu->d, 0, sizeof(cpu->d));
    memset(cpu->a, 0, sizeof(cpu->a));
    cpu->usp = 0;
    cpu->msp = 0;
    cpu->cacr = cpu->caar = 0;
    if (cpu_is_68060(cpu)) {
        /* TC は全ビット 0、TTR は E のみ 0。ATC・URP/SRP は reset で変えない（[060UM
         * §4.1.2]）。 */
        cpu->mmu060_tc = 0;
        memset(cpu->t060_bc_tag, 0, sizeof(cpu->t060_bc_tag));
        cpu->mmu060_itt0 &= ~UINT32_C(0x00008000);
        cpu->mmu060_itt1 &= ~UINT32_C(0x00008000);
        cpu->mmu060_dtt0 &= ~UINT32_C(0x00008000);
        cpu->mmu060_dtt1 &= ~UINT32_C(0x00008000);
        cpu_ifetch_fast_invalidate(cpu); /* TC・ITT を書き換えた */
        cpu->pcr = 0;
        cpu->buscr = 0;
    }
    if (!cpu_is_68060(
            cpu)) /* 060 の I/D cache は reset で tag・状態・data を保持する（[060UM §5.1]） */
        cpu_040_cache_reset(cpu);
    cpu->exec_cycles_at_insn_start = 0; /*  */
    cpu->bus_wait_accum = 0;            /* : 積み残したバスウェイトも他の状態と同様にクリア */
    cpu->bus_access_cycles = 0;
    cpu->bus_access_epoch = false;
    cpu->bus_access_active = false;
    cpu->mmu_tc &= ~UINT32_C(0x80000000);
    cpu->mmu_tt0 &= ~UINT32_C(0x00008000);
    cpu->mmu_tt1 &= ~UINT32_C(0x00008000);
    if (cpu_is_68040(cpu)) {
        cpu->mmu040_tc &= (uint16_t)~UINT16_C(0x8000);
        cpu->mmu040_dtt0 &= ~UINT32_C(0x00008000);
        cpu->mmu040_dtt1 &= ~UINT32_C(0x00008000);
        cpu->mmu040_itt0 &= ~UINT32_C(0x00008000);
        cpu->mmu040_itt1 &= ~UINT32_C(0x00008000);
    }
    cpu->vbr = 0; /* 実機のリセット動作。68000 モデルでは常に 0 のまま */
    cpu->sfc = 0; /* リセット時未定義だが再現性のため 0 クリア（68010+） */
    cpu->dfc = 0;
    cpu->exec_state = CPU_NORMAL;
    cpu->pending_irq = 0;
    cpu->nmi_taken = false;      /* pull型NMIエッジ検出もリセットで解除する */
    cpu->badjump_dumped = false; /* BADJUMPの初回ダンプはリセットごとに再度許可する */
    cpu->badori_dumped = false;  /* BADORIの初回ダンプはリセットごとに再度許可する */
    /* グループ0再フォルト安全弁（68010+）の状態もリセットごとにクリアする */
    cpu->g0_rte_armed = false;
    cpu->g0_rte_pc = 0;
    cpu->g0_last_addr = 0;
    cpu->g0_last_pc = 0;
    cpu->g0_last_vector = 0;
    cpu->g0_refault_count = 0;
    /* 最初のグループ0フォルトの属性保護ラッチも解除する。accept_irq() や
     * cpu_dispatch_exception() のフレーム書込み・ベクタ読出しでフォルトして
     * HALT した場合は、dispatch_group0_exception() を経ないため立ったまま残る。
     * fault ring 用の新設ラッチ2つ（g0_mmu_latched・g0_origin_irq）も同じ理由で
     * 解除する。fault ring の ON/OFF・履歴は変更しない。 */
    cpu->group0_fault_latched = false;
    cpu->fault040.valid = false;
    cpu->wb2.valid = false;
    cpu->wb2_defer_ok = false;
    cpu->g0_wb2_deferred = false;
    cpu->fpu040_post_pending = false;
    cpu->movem040_continuation_valid = false;
    cpu->movem040_resume = false;
    cpu->g0_mmu_latched = false;
    cpu->g0_origin_irq = false;
    cpu->g0_no_rewind = false;
    cpu->irq_defer = false;
    cpu->cache_no_allocate = false;
    cpu->access_tt_hit = false;
    cpu->fault_access.has_origin = false;
    cpu_exception_frame_origins_clear(cpu);
    cpu->cp_restore_readback = 0;
    cpu->cp_save_left = 0;
    /* mc68k_debug_run() の停止理由は「直近の呼び出し結果」を表す一回限りの
     * 情報のため、ゲストリセットのたびにクリアする（last_exception_vectorは
     * mc68k_debug_run() 内部ループでも毎ステップリセットされるが、
     * mc68k_debug_run() を経由しない mc68k_execute() 直接呼び出しが続いた
     * 場合に古い値が残らないよう、ここでも念のためクリアする）。 */
    cpu->stop_reason = MC68K_STOP_NONE;
    cpu->last_exception_vector = 0;
    cpu->wp_hit_id = -1; /* 直近のウォッチポイントヒット情報も同様にクリアする */
    if (cpu->fpu)
        mc68k_fpu_model_reset(cpu->fpu); /* FPn/FPCR/FPSR/FPIAR と protocol state */
    if (cpu->fpu040)
        mc68k_fpu040_reset(cpu->fpu040); /* 内蔵FPUは共通コア状態だけを reset */

    /* スーパーバイザモード + 割り込みマスク 7 */
    cpu->sr = SR_RESET;

    /*
     * リセットベクタの読み出しは bus 直呼び（cpu_read_long ではなく）。
     * リセット時点では CPU はまだ「動いていない」のでアドレスエラー例外は発生しない。
     * ベクタテーブルは常に偶数アドレスなのでアライメント問題もない。
     */
    uint32_t reset_ssp = 0, reset_pc = 0;
    cpu->bus.read_long(cpu->bus.ctx, 0x000000, &reset_ssp);
    cpu->ssp = reset_ssp;
    cpu->a[7] = cpu->ssp; /* A7 = SSP（スーパーバイザモード中は SSP が A7） */
    cpu->bus.read_long(cpu->bus.ctx, 0x000004, &reset_pc);
    cpu->pc = reset_pc;
    cpu->instr_pc = reset_pc;
    cpu->instr_ir = 0;
    cpu_prefetch_reset(cpu);

    /* ウォッチポイントの前回値を再取得する。
     * リセットでメモリ内容が変わりうるため、しないとリセット直後の
     * 最初のステップで偽ヒットになる。peek未登録時は何もしない
     * （debugger.c 実装）。 */
    cpu_rearm_watchpoints(cpu);
}

/* BUSCR（[060UM §7.4]）。例外が起きると L/LE を SL/SLE へ写し、L/LE を 0 にする。 */
#define BUSCR_L   UINT32_C(0x80000000)
#define BUSCR_SL  UINT32_C(0x40000000)
#define BUSCR_LE  UINT32_C(0x20000000)
#define BUSCR_SLE UINT32_C(0x10000000)

static void buscr_save_for_exception(mc68k_state_t *cpu) {
    /* buscr は 060 以外では常に 0 なので、他のモデルでは何も変わらない。 */
    uint32_t b = cpu->buscr;
    cpu->buscr = ((b & BUSCR_L) ? BUSCR_SL : 0u) | ((b & BUSCR_LE) ? BUSCR_SLE : 0u);
}

/*
 * オートベクタ割り込み受付。
 *
 * MC68000 割り込みシーケンス:
 *   1. 現在の SR を保存
 *   2. ユーザモードなら USP を退避してスーパーバイザ SP をロード
 *   3. SR 更新: S=1, T=0, 割り込みマスクを受け付けレベルに設定
 *   4. PC をスタックに積む（4 バイト）
 *   5. 保存 SR をスタックに積む（2 バイト）
 *   6. オートベクタ: ベクタ番号 = 24 + level → アドレス = ベクタ番号 * 4
 *   7. PC = ベクタが示すアドレス
 *
 * 戻り値: 消費サイクル数（正常時 44、途中でバスエラーなら小さい値）
 */
static int accept_irq_body(mc68k_state_t *cpu, int level);
static int exception_vector_read_fault(mc68k_state_t *cpu, uint32_t vector_addr, int cycles,
                                       bool not_instruction);
static bool exception_vector_is_group1(uint16_t vector);
static int accept_irq(mc68k_state_t *cpu, int level) {
    const bool saved = cpu->cache_no_allocate;
    cpu->cache_no_allocate = true;
    const int cycles = accept_irq_body(cpu, level);
    cpu->cache_no_allocate = saved;
    return cycles;
}

static int accept_irq_body(mc68k_state_t *cpu, int level) {
    uint16_t saved_sr = cpu->sr;
    if (!cpu_exception_frame_origin_begin(cpu, saved_sr, cpu->pc))
        return 0;
    buscr_save_for_exception(cpu);
    /* 68010+: フォーマットワード付き8バイトフレーム。ベクタ番号が確定するのは
     * IACK 後（下記）のため、スロットだけ先に確保し書き込みを遅延させる
     * （スロット先行確保+遅延書き込み）。
     * long_frame を const で先に固定するのは、確保と書き込みの条件が
     * 食い違う非対称バグを防ぐため。 */
    const bool long_frame = cpu_has_68010_isa(cpu);
    uint32_t fmt_slot = 0;

    /* ユーザモード → スーパーバイザモードへ切り替え */
    cpu_set_sr(cpu, (uint16_t)(saved_sr | SR_S));

    /* SR: S=1, T1=T0=0, 割り込みマスク = 受け付けレベル */
    cpu->sr = (uint16_t)(cpu->sr & ~(SR_T1 | SR_T0));
    cpu->sr = (uint16_t)((cpu->sr & ~(SR_I2 | SR_I1 | SR_I0)) | (uint16_t)((level & 7) << 8));
    /* 060 は SSP 1 本。割込み受付で M を 0 にするが、stack は切替えない（[060UM §11.1.2]）。 */
    if (cpu_is_68060(cpu))
        cpu->sr = (uint16_t)(cpu->sr & ~SR_M);

    if (long_frame) {
        cpu->a[7] -= 2;
        fmt_slot = cpu->a[7];
    }

    /* スタックに PC → SR の順で積む（TRAP と同じフレーム形式。
     * 68000/68010 とも早期 return の値は現状のまま変えない） */
    cpu->a[7] -= 4;
    cpu_write_long(cpu, cpu->a[7], cpu->pc);
    if (cpu->exec_state != CPU_NORMAL) {
        /* 割込みフレーム構築中のアクセスフォルトは、別のフレームでは報告できない。
         * Group-0 のフレーム／ベクタアクセス失敗と同じ MC68000 の二重フォルト HALT 状態になる。
         */
        if (!cpu_has_68020_isa(cpu))
            cpu->exec_state = CPU_HALTED;
        return 4;
    }
    cpu->a[7] -= 2;
    cpu_write_word(cpu, cpu->a[7], saved_sr);
    if (cpu->exec_state != CPU_NORMAL) {
        if (!cpu_has_68020_isa(cpu))
            cpu->exec_state = CPU_HALTED;
        return 8;
    }

    /* 68020 のマスタースーパーバイザ状態で受理した割込みは、まず実フレームを MSP に残し、
     * 次に ISP 上の使い捨て $1 フレームでハンドラへ入る。$1 の RTE は M
     * を復元して実フレームを公開する。 */
    if (cpu_has_master_stack(cpu) && (saved_sr & SR_M)) {
        cpu_set_sr(cpu, (uint16_t)(cpu->sr & ~SR_M));
        cpu->a[7] -= 8;
        uint32_t throwaway = cpu->a[7];
        cpu_write_word(cpu, throwaway, (uint16_t)(saved_sr | SR_S));
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        cpu_write_long(cpu, throwaway + 2, cpu->pc);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        cpu_write_word(cpu, throwaway + 6, (uint16_t)(0x1000u | (uint16_t)(24 + level) * 4u));
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
    }

    /* ベクタドインタラプト or オートベクタ or スプリアス割り込み。
     * ベクタ「番号」を確定させてから cpu_vector_addr() で VBR 相対アドレスへ
     * 変換する（68000 モデルは vbr==0 のため従来の "vec_num * 4" と同一）。 */
    uint16_t vec_num;
    if (cpu->bus.iack != NULL) {
        uint8_t vec = cpu_bus_iack(cpu, level);
        if (cpu->bus_error_pending) {
            /* IACK サイクル中にバスエラー（どのデバイスも応答しなかった）:
             * スプリアス割り込み（ベクタ24固定、オートベクタ 24+level ではない）。
             * ここで消費しておかないと、直後のベクタフェッチで
             * cpu_read_long() が誤ってグループ0バスエラーを起こす。 */
            cpu->bus_error_pending = false;
            vec_num = 24;
        } else if (vec) {
            vec_num = vec; /* ベクタドインタラプト */
        } else {
            vec_num = (uint16_t)(24 + level); /* オートベクタ（iack が 0 を返した）*/
        }
    } else {
        vec_num = (uint16_t)(24 + level);
    }

    if (cpu_has_master_stack(cpu) && (saved_sr & SR_M)) {
        cpu_write_word(cpu, cpu->a[7] + 6, (uint16_t)(0x1000u | vec_num * 4u));
        if (cpu->exec_state != CPU_NORMAL) {
            return 8;
        }
    }

    if (long_frame) {
        /* 確保済みスロットへフォーマットワードを書く（フォーマット $0）。 */
        cpu_write_word(cpu, fmt_slot, (uint16_t)(vec_num * 4));
        if (cpu->exec_state != CPU_NORMAL) {
            if (!cpu_has_68020_isa(cpu))
                cpu->exec_state = CPU_HALTED;
            return 8; /* SR プッシュ失敗と同じ段階扱い（68010 のみ到達） */
        }
    }

    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vec_num));
    if (cpu->exec_state != CPU_NORMAL)
        return exception_vector_read_fault(cpu, cpu_vector_addr(cpu, vec_num), 12, true);

    /* PC の上位バイトはマスクしない。68000 の内部レジスタは32bit幅で、
     * バス上は24bitしか出ないだけ（実際のマスクは bus フック側の責務）。
     * 実機 ROM はベクタテーブルの未使用上位バイトにベクタ番号を仕込み、
     * 複数ベクタで共有するハンドラが自分の PC 上位バイトを読んで発生源を
     * 判別する技法を使う（X68000 の vector2/vector3 共有バスエラーハンドラ等）。
     * ここでマスクすると、その判別が常に0になり誤動作する。 */
    cpu->pc = new_pc;
    /* handler 先頭の2語は、PC を handler に移した後で取得する（UM §6.3.9.1）。 */
    cpu_prefetch_enter_exception(cpu, cpu_vector_addr(cpu, vec_num), false, true);
    /* インタラプトアクノリッジ: ベクタフェッチ後に IRQ 線をクリア。
     * 実機では CPU が ~IACK サイクルを発行し、デバイスが IRQ 線を下げる。
     * レベルトリガデバイス (OPM 等) は上位レイヤが再アサートする責任を持つ。
     * pull型（irq_level_fn登録時）ではこの代入は判定に使われず意味を持たない
     * （クリアはホストのデバイス側の責務。）。実行時にpullを解除
     * した場合にラッチへ古い値が残らないよう、無条件に行っておく。 */
    cpu->pending_irq = 0;
    if (cpu->irq_level_fn && level == 7)
        cpu->nmi_taken = true; /* pull型のみ: NMI受付エッジを記録し、線が7の
                                * ままハンドラ内で再受付する嵐を防ぐ */
    cpu_exception_frame_origin_complete(cpu);
    /* MC68000 オートベクタ割り込みの公称サイクル数。
     * [DS10 Table 7-22]: 68010=46（CYC_EXC_INTERRUPT） */
    if (cpu_is_68030_family(cpu))
        return (saved_sr & SR_M) ? 33 : 23;
    if (cpu_is_68060(cpu))
        return cyc060_interrupt();
    return CYC(cpu, CYC_EXC_INTERRUPT);
}

/* 68020+ のフレームが長形式（$B）か。 */
static bool group0_long_frame_68020(const mc68k_state_t *cpu) {
    return cpu->opcode_fetch_complete || cpu->fault_access_kind == FAULT_DATA_READ;
}

/* 68020+ の SSW（FC|RC の付加を含む）。 */
static uint16_t group0_ssw_68020(const mc68k_state_t *cpu) {
    uint16_t ssw = (uint16_t)((cpu->fault_access_kind >= FAULT_DATA_READ ? 0x0100u : 0u) |
                              (cpu->fault_access.rmw ? 0x0080u : 0u) |
                              ((cpu->fault_access_kind != FAULT_DATA_WRITE) ? 0x0040u : 0u) |
                              ((uint16_t)(cpu_noted_size_code(cpu) & 3) << 4) |
                              (cpu->fault_access.fc & 7));
    /* FC|RC だけを使い、FB|RB は使わない。NetBSD の busaddrerr2030（buserr_2030.s Lbe4）は、
     * FC/FB が立つときだけ stage-B アドレス（base+0x24、下記 addr_err_addr+2）をフォルト VA
     * として使い、 FC（bit15）が立つときだけそこから 2 を引く。拡張語フォルトは常に
     * long（$B）フレームなので、 FC を立てて -2
     * を発生させ、真のフォルトアドレスへ戻さなければならない。opcode 自身の fetch
     * フォルトは常に short（$A）フレームである。NetBSD は代わりにその PC 欄を読み、FC が立つと
     * 2 を加えるが、 真のフォルトアドレスはすでにその PC（命令自身の開始位置）なので、ここでは
     * FC をクリアしたままにする （Issue #47。以前の $B 用 FB／$A 用 FC
     * の分割はいずれの経路にも合わず、NetBSD が見る VA を両方 +2 ずらした）。 */
    if (cpu->fault_vector == 2 && cpu->fault_access_kind == FAULT_EXTENSION)
        ssw |= 0xA000u;
    return ssw;
}

/* 68010 の SSW（[DS10 Figure 5-10]）。RR(15)は0固定（A-1により実際に命令が再実行される
 * ので「プロセッサ再実行」の宣言は嘘にならない）。HB(10)は0固定の近似（MOVEPフォルト時のみ
 * 実機で立つが本コアはMOVEPのバイト位置を追跡していない）。 */
static uint16_t group0_ssw_68010(const mc68k_state_t *cpu) {
    bool fault_is_read = cpu->addr_err_read;
    return (uint16_t)(((cpu->fault_access_kind == FAULT_OPCODE ||
                        cpu->fault_access_kind == FAULT_EXTENSION)
                           ? 0x2000u
                           : 0u) |
                      ((cpu->fault_access_kind == FAULT_DATA_READ) ? 0x1000u : 0u) |
                      (cpu->fault_access.rmw ? 0x0800u : 0u) |
                      ((cpu_noted_size_code(cpu) == 1) ? 0x0200u : 0u) |
                      (fault_is_read ? 0x0100u : 0u) | (cpu->fault_access.fc & 7u));
}

/* 68000 の SSW。FC（bit 2-0）はフォールトしたアクセスの実際のファンクションコード
 * （cpu->fault_access.fc）を使う。X68000 の IPLROM 例外ハンドラ（IPLROMXV.S の
 * exception_common、fe3cc2 付近）は SSW 低位バイトから FC を読んで "FC=" 表示に
 * 使うため、ここを固定値にしてはならない。 */
static uint16_t group0_ssw_68000(const mc68k_state_t *cpu) {
    return (uint16_t)((cpu->addr_err_read ? 0x0010u : 0x0000u) |
                      (cpu->fault_not_instruction ? 0x0008u : 0x0000u) |
                      (cpu->fault_access.fc & 7u));
}

/* MC68040 User's Manual §8.4.6.2、Figure 8-7。ホストのバス失敗は物理 bus error であるため、
 * ATC と全継続ビットはクリアする。TT=0 は通常転送を表し、fault_fc はすでに 040 の転送修飾子
 * （user/supervisor data=1/5、instruction=2/6）である。MMU 失敗はこのフォールバックに達する前に
 * fault040 を設定する。ここで 68020/030 の SSW 符号化を再利用してはならない。 */
static uint16_t group0_ssw_68040(const mc68k_state_t *cpu) {
    return (uint16_t)((cpu->fault_access.rmw ? 0x0200u : 0u) |
                      (cpu->addr_err_read ? 0x0100u : 0u) |
                      ((uint16_t)(cpu_noted_size_code(cpu) & 3u) << 5) |
                      (cpu->fault_access.alternate ? TT_ALTERNATE : 0u) |
                      (cpu->fault_access.fc & 7u));
}

/* MC68060 Fault Status Long Word（[060UM §8.4.4.3, Figure 8-6]）。BPE（branch cache
 * 未モデル化）、 PBE/SBE（push/store buffer 未モデル化）、SEE（CPU は立てない）は常に 0。 */
enum {
    FSLW_MA = 1u << 27,
    FSLW_LK = 1u << 25,
    FSLW_RW_SHIFT = 23,
    FSLW_SIZE_SHIFT = 21,
    FSLW_TT_SHIFT = 19,
    FSLW_TM_SHIFT = 16,
    FSLW_IO = 1u << 15,
    FSLW_PTA = 1u << 12,
    FSLW_PTB = 1u << 11,
    FSLW_IL = 1u << 10,
    FSLW_PF = 1u << 9,
    FSLW_SP = 1u << 8,
    FSLW_WP = 1u << 7,
    FSLW_TWE = 1u << 6,
    FSLW_RE = 1u << 5,
    FSLW_WE = 1u << 4,
    FSLW_TTR = 1u << 3,
};
enum { FSLW_RW_WRITE = 1, FSLW_RW_READ = 2, FSLW_RW_RMW = 3 };
enum { FSLW_SIZE_BYTE = 0, FSLW_SIZE_WORD = 1, FSLW_SIZE_LONG = 2 };
enum { FSLW_TT_NORMAL = 0, FSLW_TT_ALTERNATE = 2 };
enum { ATC_LEVEL_ROOT = 1, ATC_LEVEL_POINTER = 2, ATC_LEVEL_PAGE = 3 };

static bool group0_060_is_instruction_fetch(const mc68k_state_t *cpu) {
    return cpu->fault_access_kind == FAULT_OPCODE || cpu->fault_access_kind == FAULT_EXTENSION;
}

/* 分割アクセスの 2 回目以降で fault したか。fault address は分割前の最初の論理アドレス。 */
static bool group0_060_is_later_access(const mc68k_state_t *cpu) {
    return cpu->fault_access.has_origin && cpu->fault_access.origin != cpu->addr_err_addr;
}

static uint32_t group0_fault_address_68060(const mc68k_state_t *cpu) {
    if (cpu->fault_vector == 3)
        return cpu->addr_err_addr & ~UINT32_C(1); /* 分岐先。A0 は 0 */
    /* 拡張語の fault は、拡張語でなく命令 opword の論理アドレスを指す（[060UM §8.4.4.2]）。 */
    if (cpu->fault_access_kind == FAULT_EXTENSION)
        return cpu->instr_pc;
    return cpu->fault_access.has_origin ? cpu->fault_access.origin : cpu->addr_err_addr;
}

static uint32_t group0_fslw_68060_mmu(const mc68k_state_t *cpu) {
    const mc68k_translate_result_t *r = &cpu->g0_mmu_result;
    uint32_t f = r->source == MC68K_TRANSLATE_TT ? FSLW_TTR : 0u;
    switch (r->fault) {
    case MMU_FAULT_INVALID:
        if (r->indirect_fault)
            f |= FSLW_IL;
        else if (r->level == ATC_LEVEL_ROOT)
            f |= FSLW_PTA;
        else if (r->level == ATC_LEVEL_POINTER)
            f |= FSLW_PTB;
        else if (r->level == ATC_LEVEL_PAGE)
            f |= FSLW_PF;
        break;
    case MMU_FAULT_SUPERVISOR:
        f |= FSLW_SP;
        break;
    case MMU_FAULT_WRITE_PROTECT:
        f |= FSLW_WP;
        break;
    case MMU_FAULT_TABLE_BUS_ERROR:
        f |= FSLW_TWE | FSLW_LK; /* table search は locked transfer */
        break;
    case MMU_FAULT_NONE:
    case MMU_FAULT_LIMIT:
        break;
    }
    return f;
}

static uint32_t group0_fslw_68060(const mc68k_state_t *cpu) {
    const bool instruction = group0_060_is_instruction_fetch(cpu);
    uint32_t rw = instruction             ? FSLW_RW_READ
                  : cpu->fault_access.rmw ? FSLW_RW_RMW
                  : cpu->addr_err_read    ? FSLW_RW_READ
                                          : FSLW_RW_WRITE;
    /* SIZE は分割前の元のアクセスの大きさ。 */
    const uint8_t size_code = cpu->fault_access.has_origin
                                  ? cpu_fault_size_code(cpu->fault_access.origin_size)
                                  : cpu_noted_size_code(cpu);
    uint32_t size = size_code == 0   ? FSLW_SIZE_LONG
                    : size_code == 1 ? FSLW_SIZE_BYTE
                                     : FSLW_SIZE_WORD;
    uint32_t f =
        (rw << FSLW_RW_SHIFT) | (size << FSLW_SIZE_SHIFT) |
        ((cpu->fault_access.alternate ? FSLW_TT_ALTERNATE : FSLW_TT_NORMAL) << FSLW_TT_SHIFT) |
        ((uint32_t)(cpu->fault_access.fc & 7u) << FSLW_TM_SHIFT);
    if (instruction) {
        f |= FSLW_IO;
        if (cpu->fault_access_kind == FAULT_EXTENSION)
            f |= FSLW_MA;
    } else if (group0_060_is_later_access(cpu)) {
        f |= FSLW_MA;
    }
    if (cpu->fault_access.rmw || (cpu->buscr & BUSCR_L))
        f |= FSLW_LK;
    if (cpu->g0_mmu_latched)
        f |= group0_fslw_68060_mmu(cpu);
    else {
        f |= cpu->addr_err_read ? FSLW_RE : FSLW_WE; /* host bus の TEA */
        if (cpu->access_tt_hit)
            f |= FSLW_TTR; /* TTR が一致したアクセスの fault（[060UM Fig 8-6]） */
    }
    return f;
}

/* モデル別に上の3つを選ぶ。060 の ssw 欄は FSLW の下位 16 bit（address error は 0）。 */
static uint16_t group0_ssw(const mc68k_state_t *cpu) {
    if (cpu_is_68060(cpu))
        return cpu->fault_vector == 2 ? (uint16_t)group0_fslw_68060(cpu) : 0;
    if (cpu_is_68040(cpu))
        return cpu->fault040.valid ? cpu->fault040.ssw : group0_ssw_68040(cpu);
    if (cpu_has_68020_isa(cpu))
        return group0_ssw_68020(cpu);
    if (cpu_has_68010_isa(cpu))
        return group0_ssw_68010(cpu);
    return group0_ssw_68000(cpu);
}

/* 0（68000）/ 0x8（68010）/ 0xA・0xB（68020+）。 */
static uint8_t group0_frame_format(const mc68k_state_t *cpu) {
    if (cpu_is_68060(cpu))
        return cpu->fault_vector == 2 ? 0x4 : 0x2;
    if (cpu_is_68040(cpu))
        return cpu->fault_vector == 2 ? 0x7 : 0x2;
    if (cpu_has_68020_isa(cpu))
        return group0_long_frame_68020(cpu) ? 0xB : 0xA;
    if (cpu_has_68010_isa(cpu))
        return 0x8;
    return 0;
}

/* D0--D7/A0--A7 を命令開始時スナップショットへ巻き戻す。keep_a7 は IRQ 受理時に
 * A7 をそのままにする（mc68000#57）。 */
static void restore_restart_snapshot(mc68k_state_t *cpu, bool keep_a7) {
    for (int i = 0; i < 8; i++) {
        cpu->d[i] = cpu->restart_d[i];
        if (i == 7 && keep_a7)
            continue;
        cpu->a[i] = cpu->restart_a[i];
    }
}

/* SSW の CM（継続）および CP（FPU 命令後）フォルトは、途中の進捗から命令を再開または完了する。
 * よって巻き戻してはならない。 */
static bool group0_keeps_progress_68040(const mc68k_state_t *cpu) {
    return cpu->fault040.valid && (cpu->fault040.ssw & 0x9000u) != 0;
}

/* MC68040 User's Manual §8.4.3、§8.4.6。address error は format $2 を使う。
 * 現在このコアで観測可能な他の Group-0 フォルトは物理 bus error であり、format $7 を使う。
 * 今後の 040 MMU/cache 実装は、この一つのビルダを通じて
 * ATC、write-back、push-data、継続欄を設定する。 */
static int deliver_group0_exception_68040(mc68k_state_t *cpu, uint8_t *stage) {
    uint16_t saved_sr = cpu->sr;
    uint32_t saved_pc = cpu->fault_vector == 2 ? cpu->pc : cpu->instr_pc;
    mc68k_exception_frame_origin_t *origin = cpu->exception_frame_origins;
    bool from_exception_frame = origin && !origin->bound;

    if (cpu->g0_wb2_deferred) {
        /* 保留ストアは命令が完了済みなので、巻き戻さず現在の SR と次の PC を積む。 */
        saved_sr = cpu->sr;
        saved_pc = cpu->pc;
    } else if (from_exception_frame) {
        saved_sr = origin->saved_sr;
        saved_pc = origin->saved_pc;
    } else if (cpu->fault_vector == 2 && !group0_keeps_progress_68040(cpu)) {
        /* データアクセスフォルトは命令を再開する（mc68000#96）。レジスタを巻き戻し、命令開始時の
         * スタック状態を積む。write-back 欄は無効のままなので、マッピングだけを修正して RTE
         * した ハンドラは命令に再度格納させる。単純な格納は代わりに保留 WB2
         * 経路を使う（g0_wb2_deferred）。 */
        bool m_bit_cleared_by_irq_accept =
            cpu->g0_origin_irq && (cpu->restart_sr & SR_M) && !(cpu->sr & SR_M);
        /* フォルト前に S/M を変えた命令（$1 フレームの RTE は MSP
         * へ切替え、実フレームを読む）は、 a[7]
         * を新しいスタック上に残す。先に戻し、cpu_set_sr() が a[7] を再開用でなく
         * 自身のシャドウへ退避するようにする。IRQ 受理は mc68000#57 の処理を維持する。 */
        if (!cpu->g0_origin_irq)
            cpu_set_sr(cpu, cpu->restart_sr);
        restore_restart_snapshot(cpu, m_bit_cleared_by_irq_accept);
        saved_sr = cpu->restart_sr;
        saved_pc = cpu->instr_pc;
    }
    cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));
    cpu->exec_state = CPU_NORMAL;
    *stage = MC68K_FAULT_OUTCOME_FRAME_WRITE_FAULT;

    if (cpu->fault_vector == 3) {
        uint32_t base;
        cpu->a[7] -= 12;
        base = cpu->a[7];
        if (from_exception_frame) {
            origin->group0_base = base;
            origin->bound = true;
        }
        cpu_write_word(cpu, base + 0, saved_sr);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_long(cpu, base + 2, saved_pc);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_word(cpu, base + 6, (uint16_t)(0x2000u | 3u * 4u));
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_long(cpu, base + 8, cpu->addr_err_addr & ~UINT32_C(1));
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
    } else {
        uint32_t base;
        const mc68k_040_access_error_t *fault = &cpu->fault040;
        uint16_t ssw = fault->valid ? fault->ssw : group0_ssw_68040(cpu);
        if (fault->valid && (ssw & 0x8000u)) {
            cpu->fpu040_post_pending = true;
            cpu->fpu040_post_vector = fault->post_vector;
        }
        cpu->a[7] -= 60;
        base = cpu->a[7];
        if (from_exception_frame) {
            origin->group0_base = base;
            origin->bound = true;
        }
        cpu_write_word(cpu, base + 0, saved_sr);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_long(cpu, base + 2, saved_pc);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_word(cpu, base + 6, (uint16_t)(0x7000u | 2u * 4u));
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_long(cpu, base + 8, fault->valid ? fault->effective_address : 0);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_word(cpu, base + 12, ssw);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_byte(cpu, base + 14, 0);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_byte(cpu, base + 15, fault->valid ? fault->wb_status[0] : 0);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_byte(cpu, base + 16, 0);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_byte(cpu, base + 17, fault->valid ? fault->wb_status[1] : 0);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_byte(cpu, base + 18, 0);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_byte(cpu, base + 19, fault->valid ? fault->wb_status[2] : 0);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu_write_long(cpu, base + 20,
                       fault->valid ? fault->fault_address : cpu->addr_err_addr);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t off = 24 + i * 8;
            cpu_write_long(cpu, base + off, fault->valid ? fault->wb_address[i] : 0);
            if (cpu->exec_state != CPU_NORMAL)
                goto halt;
            cpu_write_long(cpu, base + off + 4, fault->valid ? fault->wb_data[i] : 0);
            if (cpu->exec_state != CPU_NORMAL)
                goto halt;
        }
        for (unsigned i = 0; i < 3; ++i) {
            cpu_write_long(cpu, base + 48 + i * 4, fault->valid ? fault->push_data[i] : 0);
            if (cpu->exec_state != CPU_NORMAL)
                goto halt;
        }
    }

    *stage = MC68K_FAULT_OUTCOME_VECTOR_READ_FAULT;
    {
        uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, (uint16_t)cpu->fault_vector));
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu->pc = new_pc;
        cpu->last_exception_vector = (uint16_t)cpu->fault_vector;
        return 50;
    }

halt:
    cpu->exec_state = CPU_HALTED;
    return 4;
}

/* MC68060 User's Manual §8.4.2, §8.4.4。access error は format $4（8 word）、address error は
 * format $2（6 word、address 欄は分岐先で A0=0）。060 は restart モデルなので、access error は
 * 命令開始時のレジスタへ巻き戻して命令の先頭 PC を積む。write-back 状態は持たない。
 * 通常 frame を積む途中の fault は origin（本書 §7）から SR/PC を取る。 */
static int deliver_group0_exception_68060(mc68k_state_t *cpu, uint8_t *stage) {
    uint16_t saved_sr = cpu->sr;
    uint32_t saved_pc = cpu->instr_pc;
    mc68k_exception_frame_origin_t *origin = cpu->exception_frame_origins;
    const bool from_exception_frame = origin && !origin->bound;
    const bool address_error = cpu->fault_vector == 3;
    /* FSLW は BUSCR の L を退避する前、frame を積む前の fault 属性から確定させる。 */
    const uint32_t fslw = address_error ? 0u : group0_fslw_68060(cpu);
    const uint32_t fault_address = group0_fault_address_68060(cpu);

    if (from_exception_frame) {
        saved_sr = origin->saved_sr;
        saved_pc = origin->saved_pc;
    } else if (!cpu->g0_no_rewind) {
        if (!cpu->g0_origin_irq)
            cpu_set_sr(cpu, cpu->restart_sr);
        restore_restart_snapshot(cpu, false);
        saved_sr = cpu->restart_sr;
    }
    buscr_save_for_exception(cpu);
    cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));
    cpu->exec_state = CPU_NORMAL;
    *stage = MC68K_FAULT_OUTCOME_FRAME_WRITE_FAULT;

    const uint32_t frame_bytes = address_error ? 12u : 16u;
    const uint16_t format = address_error ? 0x2000u : 0x4000u;
    cpu->a[7] -= frame_bytes;
    const uint32_t base = cpu->a[7];
    if (from_exception_frame) {
        origin->group0_base = base;
        origin->bound = true;
    }
    cpu_write_word(cpu, base + 0, saved_sr);
    if (cpu->exec_state != CPU_NORMAL)
        goto halt;
    cpu_write_long(cpu, base + 2, saved_pc);
    if (cpu->exec_state != CPU_NORMAL)
        goto halt;
    cpu_write_word(cpu, base + 6, (uint16_t)(format | (uint16_t)cpu->fault_vector * 4u));
    if (cpu->exec_state != CPU_NORMAL)
        goto halt;
    cpu_write_long(cpu, base + 8, fault_address);
    if (cpu->exec_state != CPU_NORMAL)
        goto halt;
    if (!address_error) {
        cpu_write_long(cpu, base + 12, fslw);
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
    }

    *stage = MC68K_FAULT_OUTCOME_VECTOR_READ_FAULT;
    {
        uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, (uint16_t)cpu->fault_vector));
        if (cpu->exec_state != CPU_NORMAL)
            goto halt;
        cpu->pc = new_pc;
        cpu->last_exception_vector = (uint16_t)cpu->fault_vector;
        cpu->irq_defer = true;
        return cyc060_exception((uint16_t)cpu->fault_vector, 0);
    }

halt:
    cpu->exec_state = CPU_HALTED;
    return 4;
}

/*
 * グループ0例外（アドレスエラー = ベクタ3 / バスエラー = ベクタ2）ディスパッチ。
 * どちらも同じ 14 バイトスタックフレームを積む（下記参照）。cpu->fault_vector
 * で発生元を区別する（cpu_internal.h の各バスアクセス関数が設定済み）。
 * X68000 の ROM ハンドラがこのフレーム前提で SR/PC を読み・SP を後始末するため、
 * 14 バイト正確に積む必要がある（6 バイト簡略版では SP が崩壊する）。
 *
 * 呼び出し前: exec_state == CPU_EXCEPTION
 * 呼び出し後: exec_state == CPU_NORMAL (ハンドラへ飛んだ)
 *             exec_state == CPU_HALTED  (ダブルフォルト)
 *
 * 本体は deliver_group0_exception()。group0_fault_latched の解除は
 * dispatch_group0_exception() が全出口で一括して行う。
 */
static int deliver_group0_exception_body(mc68k_state_t *cpu, uint8_t *stage);
static int deliver_group0_exception(mc68k_state_t *cpu, uint8_t *stage) {
    const bool saved = cpu->cache_no_allocate;
    cpu->cache_no_allocate = true;
    const int cycles = deliver_group0_exception_body(cpu, stage);
    cpu->cache_no_allocate = saved;
    return cycles;
}

static int deliver_group0_exception_body(mc68k_state_t *cpu, uint8_t *stage) {
    uint16_t saved_sr = cpu->sr;
    /* MC68000 の Group 0 frame は、命令語をフェッチした後の PC を保存する
     * （PC0+2 から PC0+10 の偶数値）。それ以外のモデルは命令先頭 PC を保存する
     * （cpu->pc はフォルト時点で拡張ワード取得等により進んでいることがある）。 */
    uint32_t saved_pc = (cpu->model == MC68K_MODEL_68000) ? cpu->pc : cpu->instr_pc;

    if (cpu_is_68060(cpu))
        return deliver_group0_exception_68060(cpu, stage);
    if (cpu_is_68040(cpu))
        return deliver_group0_exception_68040(cpu, stage);

    if (cpu_has_68020_isa(cpu)) {
        mc68k_exception_frame_origin_t *origin = cpu->exception_frame_origins;
        bool from_exception_frame = origin && !origin->bound;
        /* エミュレータは単一バスサイクルではなく、命令の再開によってフォルトを復帰する。 */
        /* mc68000#57: a fault while accept_irq() is mid M=1→M=0 transition
         * (the $1 throwaway-frame write on ISP) must
         * not have a[7] rolled back to restart_a[7] before cpu_set_sr() below.
         * cpu_set_sr() saves "the current a[7]" into whichever shadow matches
         * the CURRENT sr (still M=0 here); if we clobber a[7] with the stale
         * pre-IRQ MSP value first, that stale value gets stored into the ISP
         * shadow (cpu->ssp) instead of the true live ISP. Leaving a[7] alone
         * lets cpu_set_sr() capture the real ISP, matching restart_a[]'s
         * per-register rollback semantics for every other register. */
        bool m_bit_cleared_by_irq_accept =
            cpu->g0_origin_irq && (cpu->restart_sr & SR_M) && !(cpu->sr & SR_M);
        if (!from_exception_frame) {
            /* mc68000#101: an instruction that changed S/M before faulting
             * (RTE of a $1 frame) leaves a[7] on the new stack; switch back
             * first, as the 68040 path does. */
            if (!cpu->g0_origin_irq)
                cpu_set_sr(cpu, cpu->restart_sr);
            restore_restart_snapshot(cpu, m_bit_cleared_by_irq_accept);
            saved_sr = cpu->restart_sr;
        } else {
            saved_sr = origin->saved_sr;
            saved_pc = origin->saved_pc;
        }
        /* 再開スナップショットは一つのアーキテクチャ状態である。Group-0 の RTE は D/A と同様に
         * SR も復元しなければならない。積む復帰 SR
         * とハンドラへのスーパーバイザ遷移の両方に使う。 */
        cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));
        cpu->exec_state = CPU_NORMAL;
        bool long_frame = group0_long_frame_68020(cpu);
        uint32_t base;
        uint16_t ssw = group0_ssw_68020(cpu);
        /* Data Output Buffer はフレームへのプッシュを開始する前にここで確定させる。
         * cpu_write_*() は呼び出すたびに cpu->fault_data_out を書き込んだ値で
         * 上書きするため、base+0x10 への Fault Address 書き込み自体が
         * cpu->fault_data_out を汚染し、後続の base+0x18 で参照すると別の値になる
         * （、68010分岐のA-3/落とし穴Bと同じパターン）。 */
        uint32_t dob = cpu->fault_data_out;
        *stage = MC68K_FAULT_OUTCOME_FRAME_WRITE_FAULT;
        if (long_frame) {
            cpu->a[7] -= 92;
            base = cpu->a[7];
            if (from_exception_frame) {
                origin->group0_base = base;
                origin->bound = true;
            }
            cpu_write_word(cpu, base + 0x00, saved_sr);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_long(cpu, base + 0x02, saved_pc);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_word(cpu, base + 0x06, (uint16_t)(0xB000u | cpu->fault_vector * 4));
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_word(cpu, base + 0x0A, ssw);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_long(cpu, base + 0x10, cpu->addr_err_addr);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_long(cpu, base + 0x18, dob);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            /* Stage B にはフォルトした語の次の語が入るため、そのアドレスはフォルトアドレス + 2
             * である。 ゲストハンドラは命令フォルト時に SP+$10 でなくこの欄を読み、SSW.FC
             * が立つと 2 を引く。 */
            cpu_write_long(cpu, base + 0x24, cpu->addr_err_addr + 2);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_long(cpu, base + 0x2C, 0);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_word(cpu, base + 0x36, 0x0001); /* implementation internal version */
        } else {
            cpu->a[7] -= 32;
            base = cpu->a[7];
            if (from_exception_frame) {
                origin->group0_base = base;
                origin->bound = true;
            }
            cpu_write_word(cpu, base + 0x00, saved_sr);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_long(cpu, base + 0x02, saved_pc);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_word(cpu, base + 0x06, (uint16_t)(0xA000u | cpu->fault_vector * 4));
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_word(cpu, base + 0x0A, ssw);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_long(cpu, base + 0x10, cpu->addr_err_addr);
            if (cpu->exec_state != CPU_NORMAL)
                goto group0_halt;
            cpu_write_long(cpu, base + 0x18, dob);
        }
    group0_halt:
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 4;
        }
        *stage = MC68K_FAULT_OUTCOME_VECTOR_READ_FAULT;
        uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, (uint16_t)cpu->fault_vector));
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 12;
        }
        cpu->pc = new_pc;
        cpu->last_exception_vector = (uint16_t)cpu->fault_vector;
        if (cpu_is_68030_family(cpu) && cpu->fault_vector == 2)
            return long_frame ? 62 : 36;
        return 50;
    }

    /* 例外処理（frame／vector）の途中で起きたフォルトは、命令ではなくその例外処理のフォルトである。
     * 命令開始時への巻き戻しはせず、例外が起きた時点の SR／PC を積む（68020 分岐と同じ）。 */
    mc68k_exception_frame_origin_t *origin = cpu->exception_frame_origins;
    const bool from_exception_frame = cpu_has_68010_isa(cpu) && origin && !origin->bound;
    if (from_exception_frame) {
        saved_sr = origin->saved_sr;
        saved_pc = origin->saved_pc;
    } else if (cpu_has_68010_isa(cpu)) {
        /* 命令リスタート機構を68010の長フレームにも適用する
         * 。68020+と同じロールバックを
         * 行う: レジスタだけ巻き戻してSRを巻き戻さない状態を作ってはならない
         * （A7とSビットの不整合がSSP/USPスワップを誤爆させる、68020分岐と同じ理由）。 */
        restore_restart_snapshot(cpu, false);
        saved_sr = cpu->restart_sr;
    }
    cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));
    cpu->exec_state = CPU_NORMAL;

    if (cpu_has_68010_isa(cpu)) {
        /* 再フォルト安全弁: 直前の
         * フォーマット $8 フレーム RTE 復帰直後、ベクタ・アクセスアドレス・
         * 命令先頭 PC がすべて一致するフォルトが再発した場合、ハンドラが
         * 復帰までに何も解消しなかったと判断して CPU_HALTED にする。命令全体を
         * 命令開始時のレジスタ状態から再実行する近似（A-1）のもとでは、同一
         * 命令の再実行は決定的に同一アドレスを叩くため、この判定は成立し続ける。
         * 「1回目はログだけ取り2回目でマップを直す」ハンドラは誤検出されるが、
         * 本コアの対象ワークロード（X68000系）にそのようなハンドラは存在せず、
         * HALT時にはGROUP0-REFAULTログが出るため無言のハングより安全という判断。
         * ハンドラがスタック上の PC を書き換えて命令をスキップする
         * 定石は条件2（復帰先PC==命令先頭PC）が崩れるため誤検出しない。 */
        bool same_fault = cpu->g0_rte_armed && cpu->g0_rte_pc == cpu->instr_pc &&
                          cpu->g0_last_pc == cpu->instr_pc &&
                          cpu->g0_last_addr == cpu->addr_err_addr &&
                          cpu->g0_last_vector == (uint8_t)cpu->fault_vector;
        cpu->g0_rte_armed = false; /* ラッチはワンショット。必ずここで落とす */
        if (same_fault) {
            if (++cpu->g0_refault_count >= MC68K_GROUP0_REFAULT_LIMIT) {
                cpu_log(cpu, "GROUP0-REFAULT pc=%06X addr=%08X vec=%d -> HALTED\n",
                        cpu->instr_pc, cpu->addr_err_addr, cpu->fault_vector);
                cpu->last_exception_vector = (uint16_t)cpu->fault_vector;
                cpu->exec_state = CPU_HALTED;
                return 0; /* バスサイクルを1つも消費していない */
            }
        } else {
            cpu->g0_refault_count = 1;
        }
        cpu->g0_last_pc = cpu->instr_pc;
        cpu->g0_last_addr = cpu->addr_err_addr;
        cpu->g0_last_vector = (uint8_t)cpu->fault_vector;
    }

    if (cpu_has_68010_isa(cpu)) {
        /* フォーマット $8 長フレーム（29ワード=58バイト）。意味を持って
         * 参照されるフィールド（フォーマットワード/SR/PC/SSW/フォールトアドレス/
         * Data Output Buffer/Instruction Input Buffer）は正しく埋め、Data Input
         * Buffer と Internal Information 16ワードは0埋めする（マイクロアーキ
         * テクチャ状態は本コアでモデル化していないため作れない、意図的な近似）。 */
        uint16_t fmt = (uint16_t)(0x8000u | ((uint16_t)cpu->fault_vector * 4u));
        /* SSW（group0_ssw_68010()）と DOB は、cpu_write_*（フレーム自体のプッシュ）が
         * fault_fc / fault_data_out 等を上書きする前に、プッシュ開始前のここで確定させる
         * 必要がある。 */
        bool fault_is_read = cpu->addr_err_read;
        uint16_t ssw = group0_ssw_68010(cpu);
        /* Data Output Buffer: 書き込みフォルトのときだけ積む。読み出しフォルトでは
         * データが存在しないので0のまま。
         *
         * バイト/ワードアクセスはフォルトしたバスサイクルがそのまま1本（下位16bitに
         * 収まる）なので fault_data_out の下位ワードで足りる。ロング整列アクセスは
         * 本コアが cpu_write_long_access() で単一の原子的バス書き込みとして扱うため
         * （実機は16bitバスで上位→下位の2ワードに分けて書くが本コアはモデル化しない
         * 近似）、fault_data_out には32bit値全体が入っている。Fault Address（SP+$0A）は
         * アクセス開始アドレス＝実機の1本目（上位ワード）の位置と一致するので、DOBも
         * 上位ワードを積む必要がある。下位ワードを積むと [DS10 §5.3.9] のDOB=Fault
         * Addressへ書くべき値という規定と矛盾する（mc68000#39 Solレビュー指摘）。 */
        uint16_t dob = fault_is_read ? 0u
                       : (cpu_noted_size_code(cpu) == 0) /* 0=long（cpu_fault_size_code()） */
                           ? (uint16_t)(cpu->fault_data_out >> 16)
                           : (uint16_t)cpu->fault_data_out;

        *stage = MC68K_FAULT_OUTCOME_FRAME_WRITE_FAULT;
        cpu->a[7] -= 58;
        uint32_t base = cpu->a[7];
        if (from_exception_frame) {
            origin->group0_base = base;
            origin->bound = true;
        }

        cpu_write_word(cpu, base + 0x00, saved_sr);
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 4;
        }
        cpu_write_long(cpu, base + 0x02, saved_pc);
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 4;
        }
        cpu_write_word(cpu, base + 0x06, fmt);
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 8;
        }
        cpu_write_word(cpu, base + 0x08, ssw);
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 8;
        }
        cpu_write_long(cpu, base + 0x0A, cpu->addr_err_addr);
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 8;
        }
        /* Data Output Buffer は上で確定した dob（A-3）。Data Input Buffer は
         * 0埋め（意図的な近似、フォルトした読み出しに値は存在しない）。
         * SP+$0E/$12/$16 の3ワードは Motorola 予約で、実機は SP を29ワード
         * 減らしつつ26ワードしか書き込まない（MC68010/MC68012データシート
         * Figure 5-9 の NOTE、Table 7-22 = 126(4/26)）。よってSP確保のみで
         * 書き込まない。 */
        cpu_write_word(cpu, base + 0x10, dob);
        cpu_write_word(cpu, base + 0x14, 0);
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 8;
        }
        cpu_write_word(cpu, base + 0x18, cpu->instr_ir); /* Instruction Input Buffer */
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 8;
        }
        /* Internal Information 16ワードも0埋め。 */
        for (uint32_t off = 0x1A; off <= 0x38; off += 2) {
            cpu_write_word(cpu, base + off, 0);
        }
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 8;
        }

        *stage = MC68K_FAULT_OUTCOME_VECTOR_READ_FAULT;
        uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, (uint16_t)cpu->fault_vector));
        if (cpu->exec_state != CPU_NORMAL) {
            cpu->exec_state = CPU_HALTED;
            return 12;
        }
        cpu->pc = new_pc;
        cpu->last_exception_vector = (uint16_t)cpu->fault_vector;
        return 126; /* 公称値 */
    }

    /* MC68000 グループ0例外（アドレス/バスエラー）の 14 バイトスタックフレーム:
     *   SSP+0  : Special Status Word (R/W, I/N, FC)
     *   SSP+2  : アクセスアドレス (long)
     *   SSP+6  : 命令レジスタ IR (word)
     *   SSP+8  : ステータスレジスタ SR (word)
     *   SSP+10 : プログラムカウンタ PC (long)
     * X68000 の ROM アドレスエラーハンドラは SR を SSP+8、PC を SSP+10 から
     * 読んで表示し、フレーム全体（14 バイト）を前提に SP を後始末する。 */
    uint16_t ssw = group0_ssw_68000(cpu);
    *stage = MC68K_FAULT_OUTCOME_FRAME_WRITE_FAULT;
    cpu->a[7] -= 4;
    cpu_write_long(cpu, cpu->a[7], saved_pc); /* SSP+10: PC */
    if (cpu->exec_state != CPU_NORMAL) {
        cpu->exec_state = CPU_HALTED;
        return 4;
    }
    cpu->a[7] -= 2;
    cpu_write_word(cpu, cpu->a[7], saved_sr); /* SSP+8: SR */
    if (cpu->exec_state != CPU_NORMAL) {
        cpu->exec_state = CPU_HALTED;
        return 8;
    }
    cpu->a[7] -= 2;
    cpu_write_word(cpu, cpu->a[7], cpu->instr_ir); /* SSP+6: IR */
    if (cpu->exec_state != CPU_NORMAL) {
        cpu->exec_state = CPU_HALTED;
        return 8;
    }
    cpu->a[7] -= 4;
    cpu_write_long(cpu, cpu->a[7], cpu->addr_err_addr); /* SSP+2: アクセスアドレス */
    if (cpu->exec_state != CPU_NORMAL) {
        cpu->exec_state = CPU_HALTED;
        return 8;
    }
    cpu->a[7] -= 2;
    cpu_write_word(cpu, cpu->a[7], ssw); /* SSP+0: SSW */
    if (cpu->exec_state != CPU_NORMAL) {
        cpu->exec_state = CPU_HALTED;
        return 8;
    }

    /* fault_vector（2=バスエラー/3=アドレスエラー）に応じたベクタを読む */
    *stage = MC68K_FAULT_OUTCOME_VECTOR_READ_FAULT;
    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, (uint16_t)cpu->fault_vector));
    if (cpu->exec_state != CPU_NORMAL) {
        cpu->exec_state = CPU_HALTED;
        return 12;
    }

    /* PC 上位バイトはマスクしない（accept_irq() のコメント参照）。
     * 実機 X68000 の vector2/vector3 は同じ物理ハンドラを指しつつ上位バイトに
     * ベクタ番号を仕込んでおり、ハンドラ側がそれを読んで「バスエラーが発生
     * しました」/「アドレスエラーが発生しました」を出し分ける。マスクすると
     * 常に「エラー($0000)が発生しました」という汎用メッセージになってしまう。 */
    cpu->pc = new_pc;
    cpu_prefetch_enter_exception(cpu, cpu_vector_addr(cpu, (uint16_t)cpu->fault_vector), true,
                                 true);
    cpu->last_exception_vector =
        (uint16_t)cpu->fault_vector; /* mc68k_debug_run() の停止理由判定用 */
    return 50;
}

/* fault ring 用に、outcome 以外の全フィールドをフレームを書く前の状態から埋める。
 */
static void fault_ring_capture(const mc68k_state_t *cpu, mc68k_fault_ring_entry_t *rec) {
    memset(rec, 0, sizeof(*rec));
    rec->seq = cpu->insn_seq;
    rec->origin = cpu->g0_origin_irq ? MC68K_FAULT_ORIGIN_IRQ_ACCEPT
                  : cpu->exception_frame_origins && !cpu->exception_frame_origins->bound
                      ? MC68K_FAULT_ORIGIN_EXCEPTION_FRAME
                      : MC68K_FAULT_ORIGIN_INSTRUCTION;
    rec->instr_pc = cpu->instr_pc;
    rec->opcode_valid = cpu->opcode_fetch_complete ? 1 : 0;
    rec->opcode = rec->opcode_valid ? cpu->instr_ir : 0;
    rec->vector = (uint8_t)cpu->fault_vector;
    rec->frame_format = group0_frame_format(cpu);
    rec->ssw = group0_ssw(cpu);
    /* 060 は frame に積む値（最初の論理アドレス、address error は A0=0）を記録する。 */
    rec->fault_addr = cpu_is_68060(cpu) ? group0_fault_address_68060(cpu) : cpu->addr_err_addr;
    rec->fc = cpu->fault_access.fc;
    switch (cpu->fault_access_kind) {
    case FAULT_OPCODE:
        rec->access_kind = MC68K_FAULT_ACCESS_OPCODE;
        break;
    case FAULT_EXTENSION:
        rec->access_kind = MC68K_FAULT_ACCESS_EXTENSION;
        break;
    case FAULT_DATA_READ:
        rec->access_kind = MC68K_FAULT_ACCESS_DATA_READ;
        break;
    case FAULT_DATA_WRITE:
        rec->access_kind = MC68K_FAULT_ACCESS_DATA_WRITE;
        break;
    }
    /* cpu_fault_size_code() の逆 */
    rec->size = cpu_noted_size_code(cpu) == 0 ? 4 : cpu_noted_size_code(cpu) == 1 ? 1 : 2;
    rec->read = cpu->addr_err_read ? 1 : 0;
    rec->rm = cpu->fault_access.rmw ? 1 : 0;
    rec->mmu_source = MC68K_FAULT_MMU_SOURCE_NONE;
    rec->mmu_fault = MC68K_DEBUG_MMU_NONE;
    if (cpu->g0_mmu_latched) {
        const mc68k_translate_result_t *r = &cpu->g0_mmu_result;
        rec->mmu_valid = 1;
        switch (r->source) {
        case MC68K_TRANSLATE_IDENTITY:
            rec->mmu_source = MC68K_FAULT_MMU_SOURCE_NONE;
            break;
        case MC68K_TRANSLATE_TT:
            rec->mmu_source = MC68K_FAULT_MMU_SOURCE_TT;
            break;
        case MC68K_TRANSLATE_ATC:
            rec->mmu_source = MC68K_FAULT_MMU_SOURCE_ATC;
            break;
        case MC68K_TRANSLATE_WALK:
            rec->mmu_source = MC68K_FAULT_MMU_SOURCE_WALK;
            break;
        }
        switch (r->fault) {
        case MMU_FAULT_NONE:
            rec->mmu_fault = MC68K_DEBUG_MMU_NONE;
            break;
        case MMU_FAULT_INVALID:
            rec->mmu_fault = MC68K_DEBUG_MMU_INVALID;
            break;
        case MMU_FAULT_WRITE_PROTECT:
            rec->mmu_fault = MC68K_DEBUG_MMU_WRITE_PROTECT;
            break;
        case MMU_FAULT_SUPERVISOR:
            rec->mmu_fault = MC68K_DEBUG_MMU_SUPERVISOR;
            break;
        case MMU_FAULT_LIMIT:
            rec->mmu_fault = MC68K_DEBUG_MMU_LIMIT;
            break;
        case MMU_FAULT_TABLE_BUS_ERROR:
            rec->mmu_fault = MC68K_DEBUG_MMU_TABLE_BUS_ERROR;
            break;
        }
        if (r->source == MC68K_TRANSLATE_WALK) {
            rec->walk_valid = 1;
            rec->level = r->level;
            rec->descriptor_physical = r->descriptor_physical;
            rec->table_bus_error_physical = r->table_bus_error_physical;
        }
    }
    if (model_has_pmmu(cpu->model)) {
        rec->tc = cpu->mmu_tc;
        rec->crp = cpu->mmu_crp;
        rec->srp = cpu->mmu_srp;
    }
    rec->outcome = MC68K_FAULT_OUTCOME_NOT_BUILT; /* 呼び出し側で上書きする */
}

static void fault_ring_push(mc68k_state_t *cpu, const mc68k_fault_ring_entry_t *rec) {
    cpu->fault_ring[cpu->fault_ring_pos] = *rec;
    cpu->fault_ring_pos = (cpu->fault_ring_pos + 1) & (FAULT_RING_SIZE - 1);
    if (cpu->fault_ring_count < FAULT_RING_SIZE)
        cpu->fault_ring_count++;
}

/* グループ0例外ディスパッチの入口。フレームを積めたか（ハンドラへ飛んだ）、
 * 二重フォルト・再フォルト安全弁で HALT したかに関わらず、このフォルトの
 * 属性はもう使わないのでラッチを解除する。解除しないと、次のフォルトで
 * cpu_signal_bus_error()/cpu_signal_address_error()/cpu_note_access() が
 * 属性を更新せず、前のフォルトのアドレス・R/W・FC 等がフレームに積まれる。
 * fault ring の記録と、新設ラッチ2つ（g0_mmu_latched・g0_origin_irq）の解除も
 * ここで行う。 */
static int dispatch_group0_exception(mc68k_state_t *cpu) {
    mc68k_fault_ring_entry_t rec;
    fault_ring_capture(cpu, &rec); /* フレームを書く前に確定させる */
    uint8_t stage = MC68K_FAULT_OUTCOME_NOT_BUILT;
    int cycles = deliver_group0_exception(cpu, &stage);
    rec.outcome = (cpu->exec_state == CPU_NORMAL) ? MC68K_FAULT_OUTCOME_VECTORED : stage;
    if (cpu->fault_ring_on)
        fault_ring_push(cpu, &rec);
    cpu->group0_fault_latched = false;
    cpu->g0_wb2_deferred = false;
    cpu->fault040.valid = false;
    cpu->g0_mmu_latched = false;
    cpu->g0_origin_irq = false;
    cpu->g0_no_rewind = false;
    if (cpu->exec_state == CPU_HALTED)
        cpu_exception_frame_origins_clear(cpu);
    return cycles;
}

/*
 * 例外グループ1判定（ILLEGAL=4/特権違反=8/トレース=9/Line-A=10/Line-F=11）。
 * Group1 例外は「命令が中断された」ことを意味するため、トレース例外を抑止する
 * （XEiJ のコメント「他の例外処理で命令が中断されたときはトレース例外は発生しない」
 * と同じ原則）。Group2（TRAP/TRAPV/CHK/ゼロ除算）はここに含めない＝トレース追随。
 */
static inline bool exc_vector_suppresses_trace(const mc68k_state_t *cpu, uint16_t v) {
    if (cpu_is_68060(cpu)) {
        if (v == 60 || v == 61)
            return true;
        if ((v >= 5 && v <= 7) || v == 14 || (v >= 32 && v <= 55))
            return true;
    }
    return v == 4 || v == 8 || v == 9 || v == 10 || v == 11;
}

/* Group 1/2 例外・割込みのベクタ読出しが失敗した場合。MC68000 は、その時点の PC を
 * ベクタのアドレスとして bus error 例外を取る（UM §6.3.9.1）。それ以外は従来の扱い。 */
/* Group 1（illegal／privilege／trace／Line-A・F）か。割り込みは呼び出し側が Group 1
 * として扱う。 Group 2（TRAP、TRAPV、CHK、ゼロ除算）は命令処理中として扱われる（UM §6.3.9.1）。
 */
static bool exception_vector_is_group1(uint16_t vector) {
    return vector == 4 || vector == 8 || vector == 9 || vector == 10 || vector == 11;
}

static int exception_vector_read_fault(mc68k_state_t *cpu, uint32_t vector_addr, int cycles,
                                       bool not_instruction) {
    if (cpu->model == MC68K_MODEL_68000) {
        cpu->pc = vector_addr;
        cpu->fault_not_instruction = not_instruction;
        return cycles;
    }
    /* 68010 は HALT しない（UM: HALT は bus error・address error・reset
     * の処理中の失敗に限る）。 exec_state は CPU_EXCEPTION
     * のまま戻り、実行ループが同じ呼出しの中で bus error 例外にする。 積む SR／PC は
     * exception_frame_origin から取る。 */
    return cycles;
}

static int normal_exception_frame_fault(mc68k_state_t *cpu, int cycles) {
    /* #67 changes this only for 68020/030 normal frames. */
    if (!cpu_has_68020_isa(cpu))
        cpu->exec_state = CPU_HALTED;
    return cycles;
}

/*
 * シンプルフレーム例外（Group1/Group2 共通）の一括ディスパッチ。
 * PC(4バイト)+SR(2バイト)を積んでベクタから新PCを読む、という
 * TRAP/TRAPV/CHK/ゼロ除算/ILLEGAL/Line-A/Line-F に共通するフレーム構築を
 * ここに集約する（アドレスエラー/バスエラーの14バイトフレームは対象外）。
 */
static int dispatch_exception_body(mc68k_state_t *cpu, uint32_t pushed_pc, uint16_t vector,
                                   int nominal_cycles);
int cpu_dispatch_exception(mc68k_state_t *cpu, uint32_t pushed_pc, uint16_t vector,
                           int nominal_cycles) {
    const bool saved = cpu->cache_no_allocate;
    cpu->cache_no_allocate = true;
    if (cpu_is_68060(cpu)) {
        nominal_cycles = cyc060_exception(vector, cpu->t060_ea_extra);
        cpu->t060_exc = true;
    }
    const int cycles = dispatch_exception_body(cpu, pushed_pc, vector, nominal_cycles);
    cpu->cache_no_allocate = saved;
    return cycles;
}

static int dispatch_exception_body(mc68k_state_t *cpu, uint32_t pushed_pc, uint16_t vector,
                                   int nominal_cycles) {
    uint16_t saved_sr = cpu->sr;

    /* 保留ストア（WB2）を持つ命令の中で別の例外を積むなら、保留を捨てて命令全体を再実行する。
     */
    if (cpu->wb2.valid) {
        cpu_040_wb2_escalate(cpu);
        return 0;
    }

    if (!cpu_exception_frame_origin_begin(cpu, saved_sr, pushed_pc))
        return 0;
    buscr_save_for_exception(cpu);

    cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));

    if (cpu_has_68020_isa(cpu) &&
        (vector == 5 || vector == 6 || vector == 7 || vector == 9 || vector == 56)) {
        cpu->a[7] -= 12;
        uint32_t base = cpu->a[7];
        cpu_write_word(cpu, base, saved_sr);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        cpu_write_long(cpu, base + 2, pushed_pc);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        cpu_write_word(cpu, base + 6, (uint16_t)(0x2000u | vector * 4));
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        cpu_write_long(cpu, base + 8, cpu->instr_pc);
        if (cpu->exec_state != CPU_NORMAL) {
            return 8;
        }
        uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));
        if (cpu->exec_state != CPU_NORMAL) {
            return 12;
        }
        cpu->pc = new_pc;
        cpu->exc_suppress_trace = exc_vector_suppresses_trace(cpu, vector);
        cpu->last_exception_vector = vector;
        cpu_exception_frame_origin_complete(cpu);
        return nominal_cycles;
    }

    if (cpu_has_68010_isa(cpu)) {
        /* フォーマットワード（フォーマット $0 + ベクタオフセット）を他のプッシュより
         * 先に積む。スタックは下方向に伸びるため、これがフレームの最も高いアドレス
         * （SP+6）に来る。 */
        cpu->a[7] -= 2;
        cpu_write_word(cpu, cpu->a[7], (uint16_t)(vector * 4));
        if (cpu->exec_state != CPU_NORMAL) {
            return normal_exception_frame_fault(cpu, 4);
        }
    }

    cpu->a[7] -= 4;
    cpu_write_long(cpu, cpu->a[7], pushed_pc);
    if (cpu->exec_state != CPU_NORMAL) {
        return normal_exception_frame_fault(cpu, 4);
    }
    cpu->a[7] -= 2;
    cpu_write_word(cpu, cpu->a[7], saved_sr);
    if (cpu->exec_state != CPU_NORMAL) {
        return normal_exception_frame_fault(cpu, 8);
    }

    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));
    if (cpu->exec_state != CPU_NORMAL) {
        if (!cpu_has_68020_isa(cpu))
            return exception_vector_read_fault(cpu, cpu_vector_addr(cpu, vector), 12,
                                               exception_vector_is_group1(vector));
        return normal_exception_frame_fault(cpu, 12);
    }

    cpu->pc = new_pc; /* PC上位バイトはマスクしない（accept_irq() のコメント参照）*/
    cpu_prefetch_enter_exception(cpu, cpu_vector_addr(cpu, vector), false,
                                 exception_vector_is_group1(vector));
    cpu->exc_suppress_trace = exc_vector_suppresses_trace(cpu, vector);
    cpu->last_exception_vector = vector; /* mc68k_debug_run() の停止理由判定用 */
    cpu_exception_frame_origin_complete(cpu);
    return nominal_cycles;
}

/* 68040 内蔵 FPU の format $2/$4 フレーム（宣言は cpu_internal.h）。
 * 書込み順・フォルト時の扱いは cpu_dispatch_exception() の 68020 以降 format $2 経路と同じ。 */
static int dispatch_fpu040_exception_body(mc68k_state_t *cpu, uint8_t format, uint16_t vector,
                                          uint32_t pushed_pc, uint32_t ea, uint32_t faulted_pc,
                                          int nominal_cycles);
int cpu_dispatch_fpu040_exception(mc68k_state_t *cpu, uint8_t format, uint16_t vector,
                                  uint32_t pushed_pc, uint32_t ea, uint32_t faulted_pc,
                                  int nominal_cycles) {
    const bool saved = cpu->cache_no_allocate;
    cpu->cache_no_allocate = true;
    if (cpu_is_68060(cpu)) {
        nominal_cycles = cyc060_exception(vector, cpu->t060_ea_extra);
        cpu->t060_exc = true;
    }
    const int cycles = dispatch_fpu040_exception_body(cpu, format, vector, pushed_pc, ea,
                                                      faulted_pc, nominal_cycles);
    cpu->cache_no_allocate = saved;
    return cycles;
}

static int dispatch_fpu040_exception_body(mc68k_state_t *cpu, uint8_t format, uint16_t vector,
                                          uint32_t pushed_pc, uint32_t ea, uint32_t faulted_pc,
                                          int nominal_cycles) {
    uint16_t saved_sr = cpu->sr;
    const uint32_t frame_bytes = format == FPU040_FRAME_UNIMPL_NOFPU ? 16 : 12;

    /* 保留ストア（WB2）を持つ命令の中で別の例外を積むなら、保留を捨てて命令全体を再実行する。
     */
    if (cpu->wb2.valid) {
        cpu_040_wb2_escalate(cpu);
        return 0;
    }

    if (!cpu_exception_frame_origin_begin(cpu, saved_sr, pushed_pc))
        return 0;
    buscr_save_for_exception(cpu);
    cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));

    cpu->a[7] -= frame_bytes;
    uint32_t base = cpu->a[7];
    cpu_write_word(cpu, base, saved_sr);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    cpu_write_long(cpu, base + 2, pushed_pc);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    cpu_write_word(cpu, base + 6, (uint16_t)((uint16_t)format << 12 | vector * 4u));
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    cpu_write_long(cpu, base + 8, ea);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    if (format == FPU040_FRAME_UNIMPL_NOFPU) {
        cpu_write_long(cpu, base + 12, faulted_pc);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
    }
    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));
    if (cpu->exec_state != CPU_NORMAL)
        return 12;
    cpu->pc = new_pc;
    /* format $2/$4 は [040UM §8.3] Table 8-4 の priority level 3、format $3 は実行後の FP
     * 例外で、 同時に保留されたトレースは報告しない。 */
    cpu->exc_suppress_trace = true;
    cpu->last_exception_vector = vector;
    cpu_exception_frame_origin_complete(cpu);
    return nominal_cycles;
}

/* コプロセッサインタフェースの例外フレーム（フレームの形は cpu_internal.h の宣言を参照）。
 * 例外フレームの書込みは cpu_dispatch_exception() と同じくスーパーバイザデータ空間。 */
int cpu_dispatch_coproc_exception(mc68k_state_t *cpu, cp_exc_kind_t kind, uint16_t vector,
                                  uint32_t scan_pc, uint16_t opword, uint32_t ea,
                                  int nominal_cycles) {
    uint16_t saved_sr = cpu->sr;
    const uint32_t frame_bytes = kind == CP_EXC_PRE ? 8 : kind == CP_EXC_POST ? 12 : 20;
    const uint16_t format = kind == CP_EXC_PRE    ? 0x0000u
                            : kind == CP_EXC_POST ? 0x2000u
                                                  : 0x9000u;

    /* 保留ストア（WB2）を持つ命令の中で別の例外を積むなら、保留を捨てて命令全体を再実行する。
     */
    if (cpu->wb2.valid) {
        cpu_040_wb2_escalate(cpu);
        return 0;
    }

    if (!cpu_exception_frame_origin_begin(cpu, saved_sr,
                                          kind == CP_EXC_PRE ? cpu->instr_pc : scan_pc))
        return 0;

    /* 例外処理は S をセットし、T1 と T0 を消す（[UM20 §6.1]）。coproc 例外は 68020 以上だけ */
    cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));
    cpu->a[7] -= frame_bytes;
    uint32_t base = cpu->a[7];
    cpu_write_word(cpu, base, saved_sr);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    cpu_write_long(cpu, base + 2, kind == CP_EXC_PRE ? cpu->instr_pc : scan_pc);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    cpu_write_word(cpu, base + 6, (uint16_t)(format | (vector * 4u)));
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    if (kind != CP_EXC_PRE) {
        cpu_write_long(cpu, base + 8, cpu->instr_pc);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        if (kind == CP_EXC_MID) {
            cpu_write_word(cpu, base + 12, 0); /* 内部レジスタ（実装依存。常に0） */
            if (cpu->exec_state != CPU_NORMAL)
                return 8;
            cpu_write_word(cpu, base + 14, opword);
            if (cpu->exec_state != CPU_NORMAL)
                return 8;
            cpu_write_long(cpu, base + 16, ea);
        }
    }
    if (cpu->exec_state != CPU_NORMAL) {
        return 8;
    }
    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));
    if (cpu->exec_state != CPU_NORMAL) {
        return 12;
    }
    cpu->pc = new_pc;
    /* PRE と MID は命令が完了していない。POST は完了または中断した命令に追随する。 */
    cpu->exc_suppress_trace = kind != CP_EXC_POST;
    cpu->last_exception_vector = vector;
    cpu_exception_frame_origin_complete(cpu);
    return nominal_cycles;
}

/* 特権違反（ベクタ8）: 呼び出し時点で消費済みは opcode 1ワードのみ → 命令先頭 = pc-2。
 * [DS10 Table 7-22]: 68010=38（CYC_EXC_PRIVILEGE） */
int cpu_privilege_violation(mc68k_state_t *cpu) {
    return cpu_dispatch_exception(cpu, cpu->pc - 2, 8, CYC(cpu, CYC_EXC_PRIVILEGE));
}

/*
 * 命令境界での現在のIRQレベルをサンプリングする。
 *
 * irq_level_fn が登録されていればpull型: 毎回ホストへ問い合わせる。
 * 戻り値が0-7の範囲外の場合はfail-safeで0（要求なし）として扱う
 * （& 7 でマスクすると呼び出し側のバグが別の有効な割り込み、特に
 * レベル7=NMIへ化けてしまうため採用しない）。
 *
 * 未登録なら従来通り pending_irq ラッチを読むだけ（push/latch型、無変更）。
 * nmi_taken のクリアはpull型のときだけ行う。latch型は accept_irq() 内の
 * pending_irq=0 によって次のサンプルが自然に0になるため、nmi_taken
 * 自体に一度も触れない（cycles=1実行でも既存の再受付挙動を1ビットも
 * 変えないための分離。詳細はのレビューコメント参照）。
 */
static inline int irq_sample_level(mc68k_state_t *cpu) {
    if (cpu->irq_level_fn) {
        unsigned raw = (unsigned)cpu->irq_level_fn(cpu->irq_level_ctx);
        int level = (raw <= 7u) ? (int)raw : 0; /* 値域外はfail-safeで「要求なし」*/
        if (level < 7)
            cpu->nmi_taken = false;
        return level;
    }
    return cpu->pending_irq;
}

/*
 * サンプルしたレベルを受け付けるべきか判定する。
 *
 * pull型（irq_level_fn登録時）: 通常のマスク比較に加え、レベル7は
 * nmi_takenでエッジ抑止する（線が7に張り付いていてもハンドラ内で
 * 再受付してSSPを食い潰す「割り込み嵐」を防ぐ）。
 *
 * 未登録（latch型）: 現行の2条件式から一切変更しない
 * （level==7 || level>mask）。nmi_takenは参照しない。
 */
static inline bool irq_should_accept(const mc68k_state_t *cpu, int level) {
    if (cpu->irq_level_fn)
        return level > SR_IRQ_MASK(cpu->sr) || (level == 7 && !cpu->nmi_taken);
    return level == 7 || level > SR_IRQ_MASK(cpu->sr);
}

/* bus_wait_accum を取り出して 0 へ戻す。execute_body() が
 * 反復の先頭とループ脱出後で呼ぶことで、ループ内の continue / break /
 * 条件不成立という3種類の脱出経路すべてで過不足なく1回だけ加算される
 * 。
 * ループに入る前の保留グループ0例外のディスパッチ（冒頭の CPU_EXCEPTION 分岐）だけは
 * 別で、CPU_NORMAL に戻ればそのウェイトは最初の反復の先頭で拾われ、
 * 戻らなければ分岐内で拾う。 */
static inline int cpu_take_bus_wait(mc68k_state_t *cpu) {
    int w = cpu->bus_wait_accum;
    cpu->bus_wait_accum = 0;
    return w;
}

/* mc68k_execute() の本体。呼出し前後の exec_cycles_at_insn_start の
 * 初期化・書き戻しは mc68k_execute() が一括で行う。 */
/* MC68000固有の2語プリフェッチを補充する。pcはハンドラが読む命令ストリーム
 * カーソルなので、キューの先読み位置は別フィールドで保持する。 */
enum { PREFETCH_FAULT_NONE = 0, PREFETCH_FAULT_BUS = 1, PREFETCH_FAULT_ADDRESS = 2 };

/* キューが target 語になるまで補充する。 */
static void prefetch_fill_to(mc68k_state_t *cpu, uint8_t target) {
    while (cpu->prefetch_count < target && cpu->exec_state == CPU_NORMAL &&
           cpu->prefetch_fault == PREFETCH_FAULT_NONE) {
        const mc68k_access_t access = cpu_program_access(cpu, 2);
        const uint32_t addr = cpu->prefetch_next_pc;
        if (addr & 1) {
            cpu->prefetch_fault = PREFETCH_FAULT_ADDRESS;
            return;
        }
        uint32_t physical;
        bool cache_inhibit;
        if (!cpu_translate_execute_access(cpu, addr, &access, &physical, &cache_inhibit))
            return;
        uint16_t word = 0;
        const bool ok = cpu_bus_read_word(cpu, physical, &word);
        cpu_account_bus_cycle(cpu, physical, &access, cache_inhibit);
        if (!ok) {
            cpu->prefetch_fault = PREFETCH_FAULT_BUS;
            return;
        }
        cpu->prefetch_words[cpu->prefetch_count++] = word;
        cpu->prefetch_next_pc += 2;
    }
}

static void prefetch_fill(mc68k_state_t *cpu) {
    prefetch_fill_to(cpu, 2);
}

/* cpu_translate_execute_access() の本体（060 以外）。060 は
 * cpu_translate_execute_access_060()。 */
bool cpu_translate_execute_access_slow(mc68k_state_t *cpu, uint32_t logical,
                                       const mc68k_access_t *access, uint32_t *physical,
                                       bool *cache_inhibit) {
    if (cpu_is_68040(cpu) && cpu->wb2.valid && !access->read) {
        cpu_040_wb2_escalate(cpu);
        return false;
    }
    cpu_note_access(cpu, access);
    if (cpu_is_68040(cpu)) {
        mc68k_translate_result_t result =
            cpu_mmu040_translate_result(cpu, logical, access, MC68K_TRANSLATE_EXECUTE);
        if (result.ok) {
            *physical = result.physical;
            *cache_inhibit = result.cache_inhibit;
            cpu->access_cache_mode = result.cache_mode;
            return true;
        }
        if (!cpu->group0_fault_latched) {
            cpu->g0_mmu_latched = true;
            cpu->g0_mmu_result = result;
            cpu_prepare_040_mmu_access_error(cpu, logical, access);
        }
        cpu_signal_bus_error(cpu, logical, access->read);
        return false;
    }
    if (!cpu_is_68030(cpu) ||
        (!(cpu->mmu_tc & UINT32_C(0x80000000)) && !(cpu->mmu_tt0 & UINT32_C(0x00008000)) &&
         !(cpu->mmu_tt1 & UINT32_C(0x00008000)))) {
        *physical = logical;
        *cache_inhibit =
            cpu_is_68ec030(cpu) ? cpu_mmu_ec030_cache_inhibit(cpu, logical, access) : false;
        cpu->access_cache_mode = *cache_inhibit ? 2 : 0;
        return true;
    }
    mc68k_translate_result_t result =
        cpu_mmu_translate_result(cpu, logical, access, MC68K_TRANSLATE_EXECUTE);
    if (result.ok) {
        *physical = result.physical;
        *cache_inhibit = result.cache_inhibit;
        cpu->access_cache_mode = result.cache_inhibit ? 2 : 0;
        return true;
    }
    /* fault ring 用に、最初の group0 fault の変換結果だけを保持する。
     * cpu_signal_bus_error() が group0_fault_latched を立てる前に判定すること。 */
    if (!cpu->group0_fault_latched) {
        cpu->g0_mmu_latched = true;
        cpu->g0_mmu_result = result;
    }
    cpu_signal_bus_error(cpu, logical, access->read);
    return false;
}

/* 使われない先読みを1回だけ発行する。フォルトは無視する（使われない語のため）。 */
void cpu_prefetch_speculative_read(mc68k_state_t *cpu, uint32_t addr) {
    if (cpu->model != MC68K_MODEL_68000 || (addr & 1u))
        return;
    const mc68k_access_t access = cpu_program_access(cpu, 2);
    uint32_t physical;
    bool cache_inhibit;
    if (!cpu_translate_execute_access(cpu, addr, &access, &physical, &cache_inhibit))
        return;
    uint16_t discarded = 0;
    (void)cpu_bus_read_word(cpu, physical, &discarded);
    cpu_account_bus_cycle(cpu, physical, &access, cache_inhibit);
}

/* 先読みで起きたフォルトを bus error／address error 例外として報告する。 */
static void prefetch_raise_fault(mc68k_state_t *cpu) {
    const mc68k_access_t access = cpu_program_access(cpu, 2);
    const uint8_t kind = cpu->prefetch_fault;
    cpu->prefetch_fault = PREFETCH_FAULT_NONE;
    cpu->fault_access_kind = cpu->opcode_fetch_complete ? FAULT_EXTENSION : FAULT_OPCODE;
    cpu_note_access(cpu, &access);
    if (kind == PREFETCH_FAULT_ADDRESS)
        cpu_signal_address_error(cpu, cpu->prefetch_next_pc, true);
    else
        cpu_signal_bus_error(cpu, cpu->prefetch_next_pc, true);
}

/* キューを破棄して pc から先読みし直す位置を設定する。保留中のフォルトも捨てる。 */
static void prefetch_discard(mc68k_state_t *cpu) {
    cpu->prefetch_pc = cpu->pc;
    cpu->prefetch_next_pc = cpu->pc;
    cpu->prefetch_count = 0;
    cpu->prefetch_fault = PREFETCH_FAULT_NONE;
}

void cpu_prefetch_reset(mc68k_state_t *cpu) {
    prefetch_discard(cpu);
    cpu->prefetch_primed = false;
}

/* reset 後の初期2語を取得する。reset 完了後にプログラムを配置できるよう、最初の
 * 実行時に遅延して行う。読出しは通常のバスアクセスとして観測される。 */
void cpu_prefetch_prime(mc68k_state_t *cpu) {
    if (cpu->prefetch_primed)
        return;
    cpu->prefetch_primed = true;
    prefetch_discard(cpu);
    /* 初期取得の失敗は保留し、最初の命令語を使う時点（fetch_word）で通常の例外として報告する。
     * ホストが reset 後に PC を設定し直すことがあるため、reset 中の二重フォルトとは扱わない。
     */
    prefetch_fill(cpu);
}

/* 例外処理の最終段（handler 先頭の2語の取得）。失敗した場合、Group 0 例外中なら二重フォルトで
 * HALT、それ以外は例外ベクタのアドレスを saved PC とする bus error／address error 例外にする
 * （UM §6.3.9.1）。 */
void cpu_prefetch_enter_exception(mc68k_state_t *cpu, uint32_t vector_addr, bool group0,
                                  bool not_instruction) {
    if (cpu->model != MC68K_MODEL_68000)
        return;
    prefetch_discard(cpu);
    prefetch_fill(cpu);
    if (cpu->prefetch_fault == PREFETCH_FAULT_NONE) {
        /* キューは handler 先頭で充填済み。命令完了時に flow_changed でもう一度破棄して
         * 再充填（二重read）しない。 */
        cpu->flow_changed = false;
        return;
    }
    if (group0) {
        prefetch_discard(cpu);
        cpu->exec_state = CPU_HALTED;
        return;
    }
    cpu->pc = vector_addr;
    cpu->fault_not_instruction = not_instruction;
    cpu->opcode_fetch_complete = false;
    prefetch_raise_fault(cpu);
}

/* 分岐・ジャンプ命令は拡張語の消費時に先読みしない。分岐先が決まった後にキューを
 * 再充填するため、逐次側の1語を読むと余計な bus cycle になる（UM Table 8-9, 8-10）。 */
static bool opcode_skips_ext_refill(uint16_t ir) {
    const bool bcc_bra_bsr = (ir & 0xF000u) == 0x6000u;
    const bool dbcc = (ir & 0xF0F8u) == 0x50C8u;
    /* JMP の (d8,An,Xn)／(d8,PC,Xn) は UM Table 8-10 で 14(3/0) と r が1つ多い（JSR は
     * 22(2/2)）。 */
    const bool jmp_indexed =
        (ir & 0xFFC0u) == 0x4EC0u && (((ir >> 3) & 7u) == 6u || (ir & 0x3Fu) == 0x3Bu);
    const bool jmp_jsr = (ir & 0xFF80u) == 0x4E80u && !jmp_indexed;
    const bool stop = ir == 0x4E72u; /* STOP は停止中に先読みしない（UM Table 8-12: 4(0/0)） */
    return bcc_bra_bsr || dbcc || jmp_jsr || stop;
}

uint16_t cpu_prefetch_fetch_word(mc68k_state_t *cpu) {
    cpu_prefetch_prime(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 0;
    if (cpu->prefetch_pc != cpu->pc) {
        prefetch_discard(cpu);
        prefetch_fill(cpu);
    }
    if (cpu->prefetch_count == 0 && cpu->prefetch_fault == PREFETCH_FAULT_NONE &&
        cpu->exec_state == CPU_NORMAL)
        prefetch_fill_to(cpu, 1);
    if (cpu->exec_state != CPU_NORMAL)
        return 0;
    if (cpu->prefetch_count == 0) {
        prefetch_raise_fault(cpu);
        return 0;
    }
    uint16_t word = cpu->prefetch_words[0];
    if (cpu->prefetch_count == 2)
        cpu->prefetch_words[0] = cpu->prefetch_words[1];
    cpu->prefetch_count--;
    cpu->prefetch_pc += 2;
    cpu->pc += 2;
    /* 拡張語を消費した時点では、次に使う1語だけを補充する。最後の補充は命令の
     * データアクセスより後の、命令完了時に行う（M68K83 §7.1.2、仕様 TIM-040）。 */
    if (cpu->opcode_fetch_complete && !opcode_skips_ext_refill(cpu->instr_ir)) {
        prefetch_fill_to(cpu, 1);
        /* 補充の失敗は、命令のデータアクセスより前に、この命令の中で報告する。 */
        if (cpu->prefetch_fault != PREFETCH_FAULT_NONE && cpu->prefetch_count == 0)
            prefetch_raise_fault(cpu);
    }
    return word;
}

void cpu_prefetch_complete_instruction(mc68k_state_t *cpu) {
    if (cpu->model != MC68K_MODEL_68000 || cpu->exec_state != CPU_NORMAL) {
        cpu->prefetch_flush = false;
        return;
    }
    /* 例外・RTE などは flow_changed を立てないため、キュー先頭と PC のずれでも判定する。 */
    bool redirected = false;
    uint32_t resume_pc = 0;
    if (cpu->flow_changed || cpu->prefetch_flush || cpu->prefetch_pc != cpu->pc) {
        resume_pc = cpu->prefetch_pc; /* 破棄前の、命令語の末尾 */
        redirected = true;
        prefetch_discard(cpu);
    }
    cpu->prefetch_flush = false;
    prefetch_fill(cpu);
    if (cpu->prefetch_fault == PREFETCH_FAULT_NONE)
        return;
    /* 命令の最後の補充の失敗は、その命令の中で起きる。分岐後なら saved PC は直前の
     * 命令語の末尾（PC₀+2〜10）になる（UM §6.3.9.1）。 */
    if (redirected)
        cpu->pc = resume_pc;
    cpu->opcode_fetch_complete = false;
    prefetch_raise_fault(cpu);
}

static int execute_body(mc68k_state_t *cpu, int cycles) {
    /*
     * フェッチ → デコード → 実行ループ。
     * consumed が cycles に達するか、例外・停止が発生したら抜ける。
     * MC68000 は命令境界でしか割り込まないため、ループ先頭で割り込みをチェックする。
     */
    if (cpu->exec_state == CPU_HALTED)
        return 0;

    int consumed = 0;

    /* モデルは生成後に変わらない。命令ごとの機種判定は、ループの外で1回だけ行う。 */
    const bool is_68000 = cpu_is_68000(cpu);
    const bool is_68040 = cpu_is_68040(cpu);
    const bool is_68060 = cpu_is_68060(cpu);
    const bool has_restart_isa = cpu_has_68010_isa(cpu);
    const bool has_trace_t0 = cpu_has_t0_trace(cpu);

    /* 前フレームで未処理のグループ0例外（アドレスエラー/バスエラー）をディスパッチして継続 */
    if (cpu->exec_state == CPU_EXCEPTION) {
        /* ea_invalid_pending を立てるのは full_ext_ea() だけで、命令ハンドラの中から
         * しか呼ばれない。ハンドラが戻った直後にループ内（ハンドラ実行後の
         * CPU_EXCEPTION 分岐）で必ず消費されるため、呼出しをまたいで残らず、
         * この分岐には到達しない。到達しないので bus_wait_accum の
         * 吸出しは付けない。 */
        if (cpu->ea_invalid_pending) {
            cpu->ea_invalid_pending = false;
            cpu->exec_state = CPU_NORMAL;
            return cpu_dispatch_exception(cpu, cpu->instr_pc, 4, 34);
        }
        int c = dispatch_group0_exception(cpu);
        if (cpu->exec_state != CPU_NORMAL)
            return c + cpu_take_bus_wait(cpu);
        /* CPU_NORMAL に戻ったディスパッチ分も、この呼出しの消費に含める。 */
        consumed = c;
    }

    /* STOP 待機中: 割り込みマスクを超える IRQ が来たら通常状態に戻って処理 */
    if (cpu->exec_state == CPU_STOPPED) {
        int level = irq_sample_level(cpu);
        if (!irq_should_accept(cpu, level))
            return 0;
        cpu->exec_state = CPU_NORMAL;
    }

    while (consumed < cycles) {
        /* 前回反復までに積み上がったバスウェイトを吸い出す。
         * continue で戻ってきた場合も含め、反復の先頭で必ず1回だけ呼ぶ
         * 。 */
        consumed += cpu_take_bus_wait(cpu);
        /* : バスコールバックから mc68k_executed_cycles() で見える値は
         * 実行中の命令の開始時点まで。必ず上の吸出しの後に代入する。 */
        cpu->exec_cycles_at_insn_start = consumed;

        /* 命令境界（割り込みチェックより前）で instr_pc を更新する。
         * ここでオートベクタ取得自体がバスエラーになった場合も、この時点の
         * cpu->pc（＝次に実行される命令の先頭）が正しくグループ0例外フレームへ
         * スタックされる。 */
        cpu->instr_pc = cpu->pc;
        cpu->opcode_fetch_complete = false;

        /* 命令再実行用のレジスタ退避（68010+）。
         * dispatch_group0_exception() がバスエラー時にここから
         * D0-D7/A0-A7 を巻き戻す。
         * **命令境界で取ること**が必須で、オペコードフェッチ成功後に取ると
         * 「フェッチ自体がバスエラーになった場合」に1つ前の命令の実行開始時点の
         * 値を復元してしまう。直前が RTE（スーパーバイザ→ユーザ復帰）だと
         * 復元される a[7] はカーネルの SSP であり、続く cpu_set_sr() の
         * S=0→1 スワップがユーザ SP をその値で上書きする（NetBSD/x68k の init が
         * デマンドページング初回フォルトで SIGSEGV ループに陥る原因だった）。
         * 68000 モデルではスナップショットを取らない（性能、および X68000
         * IPLROM が依存する14バイトフレーム経路は一切変えない既存方針のため）。 */
        if (has_restart_isa) {
            memcpy(cpu->restart_d, cpu->d, sizeof(cpu->d));
            memcpy(cpu->restart_a, cpu->a, sizeof(cpu->a));
            cpu->restart_sr = cpu->sr;
        }

        /* 割り込みチェック: マスクレベルより高い IRQ、または NMI (level 7) を受け付ける */
        const bool irq_deferred = cpu->irq_defer;
        cpu->irq_defer = false;
        int level = irq_deferred ? 0 : irq_sample_level(cpu);
        if (!irq_deferred && irq_should_accept(cpu, level)) {
            consumed += accept_irq(cpu, level);
            /* 68020+ の $1 フレーム書込み失敗だけが HALT せず group0 を保留して戻る。
             * 次の mc68k_execute() 冒頭で dispatch されるので、由来を残す。 */
            if (cpu->exec_state == CPU_EXCEPTION && !cpu_has_68020_isa(cpu)) {
                /* 68000 は vector 読出し／handler 先頭の取得の失敗を、その場で bus error
                 * 例外にする */
                consumed += dispatch_group0_exception(cpu);
                if (cpu->exec_state != CPU_NORMAL)
                    break;
                continue;
            }
            if (cpu->exec_state == CPU_EXCEPTION)
                cpu->g0_origin_irq = true;
            if (cpu->exec_state != CPU_NORMAL)
                break;
            continue;
        }

        /* 命令試行の通し番号。IRQ 受付は採番しない。
         * リングの ON/OFF に関係なく常に進め、reset でも戻さない。 */
        const uint64_t seq = ++cpu->insn_seq;
        /* 命令フェッチ: PC から 1 ワード読んで PC を 2 進める */
        uint32_t instr_pc = cpu->instr_pc; /* 命令先頭アドレス（不正ジャンプ元の特定用）*/
        /* PC トレース記録 */
        if (cpu->pc_ring_on) {
            cpu->pc_ring[cpu->pc_ring_pos] =
                (mc68k_pc_ring_entry_t){.seq = seq, .pc = instr_pc};
            cpu->pc_ring_pos = (cpu->pc_ring_pos + 1) & (PC_RING_SIZE - 1);
            if (cpu->pc_ring_count < PC_RING_SIZE)
                cpu->pc_ring_count++;
        }
        /* PCTRACE1024: 1024命令ごとにPCを出力（長時間実行での滞留箇所検出用）。 */
        if (cpu->pctrace1024_on) {
            if ((cpu->pctrace1024_count++ & 1023u) == 0) {
                cpu_log(cpu, "[PCTRACE1024] n=%llu pc=%06X\n",
                        (unsigned long long)cpu->pctrace1024_count, instr_pc);
            }
        }

        /* レジスタ履歴記録（実行直前の全レジスタ。opcode は fetch 後に埋める）。
         * reg_ring_on のときだけ記録する（常時オンは性能低下のため既定オフ）。*/
        mc68k_reg_ring_entry_t *snap = NULL;
        if (cpu->reg_ring_on) {
            snap = &cpu->reg_ring[cpu->reg_ring_pos];
            snap->seq = seq;
            snap->pc = instr_pc;
            snap->sr = cpu->sr;
            for (int ri = 0; ri < 8; ri++) {
                snap->d[ri] = cpu->d[ri];
                snap->a[ri] = cpu->a[ri];
            }
            cpu->reg_ring_pos = (cpu->reg_ring_pos + 1) & (REG_RING_SIZE - 1);
            if (cpu->reg_ring_count < REG_RING_SIZE)
                cpu->reg_ring_count++;
        }
        uint16_t opcode = cpu_fetch_word(cpu);
        if (snap) {
            snap->opcode_valid = (cpu->exec_state == CPU_NORMAL);
            /* 無効時は公開契約上未定義。実装は 0 */
            snap->opcode = snap->opcode_valid ? opcode : 0;
        }
        if (cpu->exec_state == CPU_EXCEPTION) {
            if (cpu->ea_invalid_pending) {
                cpu->ea_invalid_pending = false;
                cpu->exec_state = CPU_NORMAL;
                consumed += cpu_dispatch_exception(cpu, cpu->instr_pc, 4, 34);
                if (cpu->exec_state != CPU_NORMAL)
                    break;
                continue;
            }
            consumed += dispatch_group0_exception(cpu);
            if (cpu->exec_state != CPU_NORMAL)
                break;
            continue;
        }
        if (cpu->exec_state != CPU_NORMAL)
            break;
        cpu->instr_ir = opcode; /* フェッチ成功時のみ更新（グループ0例外フレームのIR用） */
        cpu->opcode_fetch_complete = true;
        cpu->fault_not_instruction = false;
        cpu->flow_changed = false;

        /* opcode 0x0000 (ORI.B #0,D0) はゼロ埋め領域への暴走で頻出するパターン。
         * mc68k_set_opcode_illegal(cpu, 0x0000) による例外化とは独立の観測用途
         * （挙動は変えずログのみ出す）。badori_log_on が既定オフなのはBADJUMP等と
         * 同じ理由（ノイズ回避）。 */
        if (cpu->badori_log_on && opcode == 0x0000u) {
            cpu_log(cpu, "BADORI pc=%06X\n", instr_pc);
            /* badori_dumped は mc68k_reset() でクリアされる（インスタンスフィールド）。*/
            if (!cpu->badori_dumped) {
                cpu->badori_dumped = true;
                if (cpu->pc_ring_on)
                    mc68k_dump_pc_ring(cpu, 256);
                if (cpu->reg_ring_on)
                    mc68k_dump_reg_ring(cpu, 96);
            }
        }

        /* トレース例外用: 命令実行前の T ビットをサンプリングする。
         * exc_suppress_trace はハンドラが Group1 例外（ILLEGAL/特権違反/
         * トレース/Line-A/Line-F）を起こしたとき（cpu_dispatch_exception() 参照）に加え、
         * コプロセッサの PRE/MID 例外と cp_restart()（coproc.c）、68040 FPU の例外フレーム
         * （cpu_dispatch_fpu040_exception() 参照。format $3 は POST
         * だが同時のトレースを報告しない）、 68040 の pre-instruction の FP 例外（format
         * $0。fpu040_pre_exception()。trace は再実行で生じる）、 68040 の
         * FTRAPcc（exec_conditional()）でも true になる。 */
        bool tracing = (cpu->sr & SR_T1) != 0;
        bool trace_t0 = has_trace_t0 && (cpu->sr & SR_T0) != 0;
        cpu->exc_suppress_trace = false;

        /* デコード・実行 */
        /* 保留ストア（WB2）を立てるのは 68040 だけ（cpu_translate_write_access()）。他機種では
         * wb2.valid と wb2_defer_ok は常に false なので、書き込みを省いても値は変わらない。 */
        if (is_68040)
            cpu->wb2.valid = false;
        cpu->t060_ea_extra = 0;
        cpu->t060_aux = 0;
        cpu->t060_exc = false;
        /* 保留しない命令: トレース中（CT を積まないので再実行でトレースを保つ）と
         * MOVEM（CM との関係が UM に無く、実機確認までは再実行）。 */
        if (is_68040)
            cpu->wb2_defer_ok = !tracing && !trace_t0 && (opcode & 0xFF80u) != 0x4880u;
        int handler_cycles = cpu->op_table[opcode](cpu, opcode);
        if (is_68040)
            cpu->wb2_defer_ok = false;
        /* 060 の命令時間は handler の値でなく [060UM §10] の表から決める。例外を積んだ命令は
         * cyc060_exception() の値（Table 10-26）が handler の戻り値に入っている。 */
        if (is_68060 && (cpu->exec_state == CPU_NORMAL || cpu->exec_state == CPU_STOPPED) &&
            !cpu->t060_exc)
            handler_cycles = cyc060_instruction(cpu, opcode, instr_pc);
        consumed += handler_cycles;
        /* 命令完了時の先読みが失敗すると PC は saved PC 用に戻るため、分岐先を控える */
        const uint32_t flow_target_pc = cpu->pc;
        /* 68000 以外の cpu_prefetch_complete_instruction() は prefetch_flush を下ろすだけ。 */
        if (is_68000)
            cpu_prefetch_complete_instruction(cpu);
        else
            cpu->prefetch_flush = false;
        /* 保留ストア（WB2）を残した命令は完了済み。命令境界で access error にする。 */
        if (is_68040 && cpu->wb2.valid) {
            if (cpu->exec_state == CPU_NORMAL)
                cpu_040_wb2_begin_delivery(cpu);
            else
                cpu->wb2.valid = false;
        }

        /* 060 の分岐先が奇数なら、命令 prefetch の address
         * error。分岐命令は完了済みなので巻き戻さない。 例外処理で奇数 vector
         * に入った場合（irq_defer）は、次の命令 fetch で検出する。 */
        if (is_68060 && cpu->exec_state == CPU_NORMAL && !cpu->irq_defer && (cpu->pc & 1)) {
            cpu->g0_no_rewind = true;
            cpu_signal_prefetch_address_error(cpu, cpu->pc);
        }

        /* 簡易バックトレース: BSR/JSR/RTSをopcodeの
         * ビットパターンで判定し、コールスタックリングへ記録する。
         * 常時記録（ON/OFF 設定なし）で、命令ハンドラ本体
         * （ops_branch.c）は変更しない。フォルト時（スタックプッシュの
         * バスエラー等）はcpu->pcが分岐先を反映していないため記録しない。 */
        /* BSR は $6xxx、JSR / RTS は $4xxx。それ以外の上位 4 ビットは判定を省く。 */
        const unsigned opcode_group = opcode >> 12;
        if ((opcode_group == 0x6u || opcode_group == 0x4u) && cpu->exec_state == CPU_NORMAL) {
            bool is_bsr = (opcode & 0xFF00) == 0x6100;
            bool is_jsr = (opcode & 0xFFC0) == 0x4E80;
            bool is_rts = (opcode == 0x4E75);
            if (is_bsr || is_jsr || is_rts) {
                cpu->call_ring[cpu->call_ring_pos] = (mc68k_call_ring_entry_t){
                    .seq = seq,
                    .from_pc = instr_pc,
                    .to_pc = cpu->pc,
                    .is_return = is_rts,
                };
                cpu->call_ring_pos = (cpu->call_ring_pos + 1) & (CALL_RING_SIZE - 1);
                if (cpu->call_ring_count < CALL_RING_SIZE)
                    cpu->call_ring_count++;
            }
        }

        /* 制御が上位8bit≠$00/$FF の不正アドレスへ移ったらジャンプ元命令を記録。
         * X68000 は 24bit アドレスなので上位非0/非FF への分岐は暴走兆候。
         * badjump_log_on が既定オフなのはBADMOVEA等と同じ理由（ノイズ回避）。 */
        if (cpu->badjump_log_on && (flow_target_pc >> 24) != 0x00u &&
            (flow_target_pc >> 24) != 0xFFu) {
            cpu_log(cpu, "BADJUMP from=%06X to=%08X opcode=%04X\n", instr_pc, flow_target_pc,
                    opcode);
            /* 初回の不正ジャンプ時点で PC トレースをダンプ（暴走に至る直前のパス）。
             * 白帯(_ABORTRST)時のダンプは白帯描画で埋まるため、発生点で取る。
             * badjump_dumped は mc68k_reset() でクリアされる（インスタンスフィールド）。*/
            if (!cpu->badjump_dumped) {
                cpu->badjump_dumped = true;
                if (cpu->pc_ring_on)
                    mc68k_dump_pc_ring(cpu, 256);
                if (cpu->reg_ring_on)
                    mc68k_dump_reg_ring(cpu, 96);
            }
        }
        if (cpu->exec_state == CPU_EXCEPTION) {
            if (cpu->ea_invalid_pending) {
                cpu->ea_invalid_pending = false;
                cpu->exec_state = CPU_NORMAL;
                consumed += cpu_dispatch_exception(cpu, cpu->instr_pc, 4, 34);
                if (cpu->exec_state != CPU_NORMAL)
                    break;
                continue;
            }
            /* Group-0 のデータフォルトは、ハンドラの固定早期復帰センチネル（通常
             * 4、ただし時間値ではない）で通知する。 UM30 §11.6.18 は vector-2
             * の完全なトランザクションを規定するため、68030 の公称合計を加える前に
             * このセンチネルを捨てる。68060 の合計も表
             * 10-26（cyc060_exception()）であり、同様に含めない。 */
            if ((cpu_is_68030_family(cpu) || cpu_is_68060(cpu)) && cpu->fault_vector == 2)
                consumed -= handler_cycles;
            consumed += dispatch_group0_exception(cpu);
            if (cpu->exec_state != CPU_NORMAL)
                break;
            continue;
        }

        /* 再フォルト安全弁のラッチ解除:
         * $8 フレーム RTE で再開した命令が（再フォルトせずに）完了したら、
         * 以降のフォルトは「連続再発」ではないのでラッチを落とす。
         * ここに到達している＝この命令はグループ0例外を起こしていない
         * （起こしていれば上のCPU_EXCEPTION分岐でcontinueしている）。
         * instr_pc（RTEの復帰先PCで始まった命令かどうか）にキーすることで、
         * RTE自身や割り込みハンドラの命令では解除されず、実際に再開命令が
         * 完了したときだけ解除される。68000モデルではg0_rte_armedが常に
         * falseのため無条件でno-op。 */
        if (cpu->g0_rte_armed && instr_pc == cpu->g0_rte_pc)
            cpu->g0_rte_armed = false;

        /* トレース例外（ベクタ9）: 命令実行前に T=1 だった場合、その命令が
         * 正常完了した（Group1 例外で中断されていない）ときだけ発生する。
         * Group2（TRAP/TRAPV/CHK/ゼロ除算）はここでは抑止されない＝追随する
         * （Motorola資料の例外優先順位、XEiJ実装のコメントと同じ原則）。
         * STOP（CPU_STOPPED）はこの分岐に該当せず自然にスキップされる。 */
        if ((tracing || (trace_t0 && cpu->flow_changed)) && !cpu->exc_suppress_trace &&
            cpu->exec_state == CPU_NORMAL) {
            /* [DS10 Table 7-22]: 68010=38（CYC_EXC_TRACE） */
            consumed += cpu_dispatch_exception(cpu, cpu->pc, 9, CYC(cpu, CYC_EXC_TRACE));
            if (cpu->exec_state == CPU_EXCEPTION && !cpu_has_68020_isa(cpu)) {
                /* trace 例外の vector 読出し／handler 先頭の取得の失敗を、その場で処理する */
                consumed += dispatch_group0_exception(cpu);
            }
            if (cpu->exec_state != CPU_NORMAL)
                break;
            continue;
        }

        if (cpu->exec_state != CPU_NORMAL)
            break;
    }
    /* ループを抜けた直後（最後の反復ぶん）のバスウェイトを吸い出す
     * 。 */
    consumed += cpu_take_bus_wait(cpu);
    return consumed;
}

int mc68k_execute(mc68k_state_t *cpu, int cycles) {
    /* : 早期 return を含む全経路で、先頭は 0、戻った後は戻り値と一致させる。 */
    cpu->exec_cycles_at_insn_start = 0;
    cpu->bus_access_cycles = 0;
    cpu->bus_access_active = false;
    cpu->bus_access_epoch = true;
    int consumed = execute_body(cpu, cycles);
    cpu->bus_access_epoch = false;
    cpu->bus_access_active = false;
    cpu->exec_cycles_at_insn_start = consumed;
    return consumed;
}

int mc68k_executed_cycles(const mc68k_state_t *cpu) {
    return cpu->exec_cycles_at_insn_start;
}

bool mc68k_bus_access_cycles(const mc68k_state_t *cpu, uint64_t *out) {
    if (!cpu || !out || !cpu->bus_access_active)
        return false;
    *out = cpu->bus_access_cycles;
    return true;
}

void mc68k_set_irq(mc68k_state_t *cpu, int level) {
    if (!cpu)
        return;
    cpu->pending_irq = level;
    /*
     * 実際の受け付けは mc68k_execute() 内の命令境界チェックで行う。
     * set_irq() は「IRQ 線を Hi/Lo にする」操作に相当するため、
     * 呼び出し時点では pending_irq を更新するだけでよい。
     *
     * mc68k_set_irq_level_hook() でpull型フックが登録されている間、
     * pending_irq は受け付け判定に一切使われない（irq_sample_level()参照）。
     * つまりこの関数は事実上のno-opになる。移行の途中でホストが両方を
     * 呼んでいても安全に無視されるよう、常に無条件で代入だけは行う
     * （分岐・警告・assertはしない、）。
     */
}

void mc68k_set_irq_level_hook(mc68k_state_t *cpu, mc68k_irq_level_fn_t fn, void *ctx) {
    if (!cpu)
        return;
    cpu->irq_level_fn = fn;
    cpu->irq_level_ctx = ctx;
}

void mc68k_get_regs(const mc68k_state_t *cpu, mc68k_regs_t *out) {
    if (!cpu || !out)
        return;
    for (int i = 0; i < 8; i++) {
        out->d[i] = cpu->d[i];
        out->a[i] = cpu->a[i];
    }
    out->pc = cpu->pc;
    out->sr = cpu->sr;
    out->usp = cpu->usp;
    out->ssp = cpu->ssp;
    out->vbr = cpu->vbr;
    out->sfc = cpu->sfc;
    out->dfc = cpu->dfc;
}

void mc68k_get_mmu_regs(const mc68k_state_t *cpu, mc68k_mmu_regs_t *out) {
    if (!cpu || !out)
        return;
    out->tt0 = cpu->mmu_tt0;
    out->tt1 = cpu->mmu_tt1;
    out->mmusr = cpu->mmu_sr;
    if (model_has_pmmu(cpu->model)) {
        out->tc = cpu->mmu_tc;
        out->crp = cpu->mmu_crp;
        out->srp = cpu->mmu_srp;
    } else {
        out->tc = 0;
        out->crp = 0;
        out->srp = 0;
    }
}

void mc68k_get_mmu060_regs(const mc68k_state_t *cpu, mc68k_mmu060_regs_t *out) {
    if (!out)
        return;
    *out = (mc68k_mmu060_regs_t){0};
    if (!cpu || !cpu_is_68060(cpu))
        return;
    *out = (mc68k_mmu060_regs_t){.urp = cpu->mmu060_urp,
                                 .srp = cpu->mmu060_srp,
                                 .tc = cpu->mmu060_tc,
                                 .dtt0 = cpu->mmu060_dtt0,
                                 .dtt1 = cpu->mmu060_dtt1,
                                 .itt0 = cpu->mmu060_itt0,
                                 .itt1 = cpu->mmu060_itt1};
}

void mc68k_get_mmu040_regs(const mc68k_state_t *cpu, mc68k_mmu040_regs_t *out) {
    if (!out)
        return;
    *out = (mc68k_mmu040_regs_t){0};
    if (!cpu || !cpu_is_68040(cpu))
        return;
    *out = (mc68k_mmu040_regs_t){.urp = cpu->mmu040_urp,
                                 .srp = cpu->mmu040_srp,
                                 .tc = cpu->mmu040_tc,
                                 .dtt0 = cpu->mmu040_dtt0,
                                 .dtt1 = cpu->mmu040_dtt1,
                                 .itt0 = cpu->mmu040_itt0,
                                 .itt1 = cpu->mmu040_itt1,
                                 .mmusr = cpu->mmu040_mmusr};
}

uint32_t mc68k_get_pc(const mc68k_state_t *cpu) {
    return cpu ? cpu->pc : 0;
}

int mc68k_is_supervisor(const mc68k_state_t *cpu) {
    return cpu ? (cpu->sr & SR_S) != 0 : 0;
}

mc68k_fpu_t *mc68k_get_fpu(mc68k_state_t *cpu) {
    return cpu ? cpu->fpu : NULL;
}

const mc68k_fpu_t *mc68k_get_fpu_const(const mc68k_state_t *cpu) {
    return cpu ? cpu->fpu : NULL;
}

mc68k_model_t mc68k_get_model(const mc68k_state_t *cpu) {
    return cpu ? cpu->model : MC68K_MODEL_68000;
}

uint32_t mc68k_get_addr_err_addr(const mc68k_state_t *cpu) {
    return cpu ? cpu->addr_err_addr : 0;
}

void mc68k_signal_bus_error(mc68k_state_t *cpu) {
    /* read/write系はで戻り値方式（false=バスエラー）に移行したため、
     * このフラグ方式は iack コールバック専用（スプリアス割り込み判定、
     * accept_irq()参照）として残る。iack はベクタ番号(uint8_t)を返す都合上、
     * 「該当デバイスなし」を戻り値だけでは表現できないため。 */
    if (!cpu)
        return;
    cpu->bus_error_pending = true;
}

cpu_exec_state_t mc68k_get_exec_state(const mc68k_state_t *cpu) {
    return cpu ? cpu->exec_state : CPU_HALTED;
}

void mc68k_set_trap_handler(mc68k_state_t *cpu, mc68k_trap_fn_t fn, void *ctx) {
    if (!cpu)
        return;
    cpu->trap_fn = fn;
    cpu->trap_ctx = ctx;
}

void mc68k_set_dreg(mc68k_state_t *cpu, int n, uint32_t val) {
    if (!cpu || n < 0 || n > 7)
        return;
    cpu->d[n] = val;
}

/* データ読出しの本体（宣言は cpu_internal.h）。 */
/* D-cache（CACR.DE=1）の読出しヒット。cpu_040_cache_read()
 * が同じアクセスで返す値と同じ値を返す。 ヒットのとき cpu_040_cache_read()
 * はラインの状態を変えず、バス・待ち時間の計上もしないので、
 * ここでも何も変えない。次のどれかなら false を返し、cpu_040_cache_read() に任せる。
 *   - 040/060 以外、命令アクセス、RMW（060 は bypass）、noncachable（cache mode 2 以上。060 は
 *     一致ラインの無効化がある）、DE=0、試験用の無効化
 *   - アクセスがラインまたは 32 ビット語をまたぐ
 *   - 同じセットで、最初に valid で物理ラインが一致するウェイ（cache_line() と同じ選び方）が
 *     無い、または loaded でない（充填は cpu_040_cache_read() が行う） */
static inline bool cpu_dcache_read_hit(const mc68k_state_t *cpu, uint32_t physical,
                                       const mc68k_access_t *access, uint8_t cache_mode,
                                       uint32_t *out) {
    if (access->instruction || access->rmw || cache_mode >= 2 ||
        !(cpu->cacr & MC68K_CACR_040_DE) || cpu->dcache_hit_fast_disable ||
        !cpu_has_onchip_cache(cpu))
        return false;
    const unsigned size = access->size;
    if ((physical & 15u) + size > 16u || (physical & 3u) + size > 4u)
        return false;
    const unsigned sets = cpu_is_68060(cpu) ? MC68K_060_CACHE_SETS : MC68K_040_CACHE_SETS;
    const mc68k_040_cache_line_t *set_lines = cpu->dcache040[(physical >> 4) & (sets - 1u)];
    const uint32_t base = physical & ~UINT32_C(0xf);
    unsigned way = 0;
    while (way < MC68K_040_CACHE_WAYS &&
           !(set_lines[way].valid && set_lines[way].physical_line == base))
        way++;
    if (way == MC68K_040_CACHE_WAYS || !set_lines[way].loaded)
        return false;
    const uint32_t word = set_lines[way].data[(physical & 15u) >> 2];
    const unsigned shift = (4u - size - (physical & 3u)) * 8u;
    *out = (word >> shift) & (size == 4 ? UINT32_MAX : ((UINT32_C(1) << (size * 8u)) - 1u));
    return true;
}

uint8_t cpu_read_byte_access(mc68k_state_t *cpu, uint32_t addr, const mc68k_access_t *access) {
    /* BADADDR ヒューリスティックは除外: Human68k はポインタ上位バイトに
     * フラグを乗せたまま byte 参照する正常コードがあり誤検出が多い。
     * バスエラー検出（未マップアドレス）はサイズ問わず対象にする。 */
    uint8_t v = 0;
    uint32_t physical;
    bool cache_inhibit;
    if (!cpu_translate_execute_access(cpu, addr, access, &physical, &cache_inhibit))
        return 0;
    uint32_t cached;
    if (cpu_dcache_read_hit(cpu, physical, access, cpu->access_cache_mode, &cached))
        return (uint8_t)cached;
    if (cpu_has_onchip_cache(cpu) &&
        cpu_040_cache_read(cpu, addr, physical, access, cpu->access_cache_mode, &cached))
        return (uint8_t)cached;
    bool ok = cpu_bus_read_byte(cpu, physical, &v);
    cpu_account_bus_cycle(cpu, physical, access, cache_inhibit);
    if (!ok)
        cpu_signal_bus_error(cpu, addr, true);
    return v;
}

uint16_t cpu_read_word_access(mc68k_state_t *cpu, uint32_t addr, const mc68k_access_t *access) {
    cpu_check_badaddr(cpu, addr, "rw");
    if (addr & 1) {
        if (!cpu_needs_alignment(cpu)) {
            mc68k_access_t byte_access = *access;
            cpu_access_mark_origin(&byte_access, access, addr);
            byte_access.size = 1;
            uint8_t hi = cpu_read_byte_access(cpu, addr, &byte_access);
            if (cpu->exec_state != CPU_NORMAL)
                return 0;
            uint8_t lo = cpu_read_byte_access(cpu, addr + 1, &byte_access);
            if (cpu->exec_state != CPU_NORMAL)
                return 0;
            return (uint16_t)((hi << 8) | lo);
        }
        /* SSW（68010長フレーム）を正しく埋めるため、フォルト情報をここでも記録する
         * 。 */
        cpu_note_access(cpu, access);
        fprintf(stderr, "ADDR ERR (read_word): addr=%06X  pc=%06X\n", addr, cpu->pc);
        cpu_signal_address_error(cpu, addr, true);
        return 0;
    }
    uint16_t v = 0;
    uint32_t physical;
    bool cache_inhibit;
    if (!cpu_translate_execute_access(cpu, addr, access, &physical, &cache_inhibit))
        return 0;
    uint32_t cached;
    if (cpu_dcache_read_hit(cpu, physical, access, cpu->access_cache_mode, &cached))
        return (uint16_t)cached;
    if (cpu_has_onchip_cache(cpu) &&
        cpu_040_cache_read(cpu, addr, physical, access, cpu->access_cache_mode, &cached))
        return (uint16_t)cached;
    bool ok = cpu_bus_read_word(cpu, physical, &v);
    cpu_account_bus_cycle(cpu, physical, access, cache_inhibit);
    if (!ok)
        cpu_signal_bus_error(cpu, addr, true);
    return v;
}

uint32_t cpu_read_long_access(mc68k_state_t *cpu, uint32_t addr, const mc68k_access_t *access) {
    cpu_check_badaddr(cpu, addr, "rl");
    if ((addr & 3) == 2 && !cpu_needs_alignment(cpu)) {
        mc68k_access_t word_access = *access;
        cpu_access_mark_origin(&word_access, access, addr);
        word_access.size = 2;
        uint16_t hi = cpu_read_word_access(cpu, addr, &word_access);
        if (cpu->exec_state != CPU_NORMAL)
            return 0;
        uint16_t lo = cpu_read_word_access(cpu, addr + 2, &word_access);
        return ((uint32_t)hi << 16) | lo;
    }
    if (addr & 1) {
        if (!cpu_needs_alignment(cpu)) {
            mc68k_access_t byte_access = *access;
            cpu_access_mark_origin(&byte_access, access, addr);
            byte_access.size = 1;
            uint8_t b0 = cpu_read_byte_access(cpu, addr, &byte_access);
            if (cpu->exec_state != CPU_NORMAL)
                return 0;
            mc68k_access_t word_access = *access;
            cpu_access_mark_origin(&word_access, access, addr);
            word_access.size = 2;
            uint16_t mid = cpu_read_word_access(cpu, addr + 1, &word_access);
            if (cpu->exec_state != CPU_NORMAL)
                return 0;
            uint8_t b3 = cpu_read_byte_access(cpu, addr + 3, &byte_access);
            if (cpu->exec_state != CPU_NORMAL)
                return 0;
            return ((uint32_t)b0 << 24) | ((uint32_t)mid << 8) | b3;
        }
        cpu_note_access(cpu, access); /* A-2、落とし穴A（read_word版と同じ理由） */
        fprintf(stderr, "ADDR ERR (read_long): addr=%06X  pc=%06X\n", addr, cpu->pc);
        cpu_signal_address_error(cpu, addr, true);
        return 0;
    }
    uint32_t v = 0;
    uint32_t physical;
    bool cache_inhibit;
    if (!cpu_translate_execute_access(cpu, addr, access, &physical, &cache_inhibit))
        return 0;
    uint32_t cached;
    if (cpu_dcache_read_hit(cpu, physical, access, cpu->access_cache_mode, &cached))
        return cached;
    if (cpu_has_onchip_cache(cpu) &&
        cpu_040_cache_read(cpu, addr, physical, access, cpu->access_cache_mode, &cached))
        return cached;
    bool ok = cpu_bus_read_long(cpu, physical, &v);
    cpu_account_bus_cycle(cpu, physical, access, cache_inhibit);
    if (!ok)
        cpu_signal_bus_error(cpu, addr, true);
    return v;
}

/* 060 の命令フェッチ（CACR.IE=0）の高速経路の後半。cpu_read_word_access() が ATC ヒット後に行う
 * 処理（cpu_translate_execute_access_060() の副作用、バス読出し、待ち時間の計上、バスエラー）と
 * 同じことを、外部呼び出し 1 回で行う。 */
uint16_t cpu_060_ifetch_nocache(mc68k_state_t *cpu, const mc68k_access_t *access,
                                mc68k_atc_entry_t *e, uint32_t physical) {
    cpu_note_access(cpu, access);
    e->history = ++cpu->atc060_clock;
    cpu->access_cache_mode = e->cache_mode;
    cpu->access_tt_hit = false;
    uint16_t v = 0;
    bool ok = cpu_bus_read_word(cpu, physical, &v);
    cpu_account_bus_cycle(cpu, physical, access, e->cache_inhibit);
    if (!ok)
        cpu_signal_bus_error(cpu, cpu->pc, true);
    return v;
}
