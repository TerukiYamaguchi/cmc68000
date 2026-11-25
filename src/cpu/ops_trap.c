/*
 * ops_trap.c — TRAP #n 命令ハンドラ実装
 */
#include "cpu_internal.h"
#include <stdint.h>

/* ================================================================== */
/* TRAP #n（ライン 4 の一部: 0x4E40-0x4E4F）                          */
/* ================================================================== */

/*
 * TRAP #n: ベクタ番号 (32+n) の例外を発生させる。
 *
 * X68000 での使われ方:
 *   TRAP #15 → IOCS コール（D0 に機能番号）
 *   TRAP #0  → Human68k DOS コール（D0 に機能番号）
 *
 * 例外処理の手順（MC68000 仕様書より）:
 *   1. SR をコピーして保存
 *   2. SR を更新: S=1（スーパーバイザモード）、T=0（トレース解除）
 *   3. PC をスタックに積む（4 バイト、次命令のアドレス）
 *   4. SR コピーをスタックに積む（2 バイト）
 *   5. ベクタテーブル[(32+n)*4] から新しい PC を読む
 *
 * trap_fn が登録されていて非 0 を返した場合はステップ 1-5 を省略し、
 * TRAP 命令の次から実行を再開する（ソフトウェアで IOCS を模倣する場合）。
 */
static int op_trap(mc68k_state_t *cpu, uint16_t opcode) {
    int n = opcode & 0xF; /* TRAP 番号 0-15 */

    /* 登録済みハンドラがあれば先に呼ぶ */
    if (cpu->trap_fn) {
        int handled = cpu->trap_fn(cpu, n, cpu->trap_ctx);
        if (handled)
            return 4; /* ハンドラが処理した: 次命令へ */
    }

    /* 通常の例外処理: TRAP #n はベクタ (32+n)、次命令のアドレス（既に cpu->pc は
     * TRAP の次を指している）をスタックに積む */
    cpu->flow_changed = true;
    /* [DS10 Table 7-22]: 68010=38（CYC_EXC_TRAP） */
    return cpu_dispatch_exception(cpu, cpu->pc, (uint16_t)(32 + n),
                                  cpu_is_68040(cpu) ? cpu_040_tag(cpu, 32)
                                                    : CYC(cpu, CYC_EXC_TRAP));
}

void register_trap_ops(op_handler_t *table) {
    /* TRAP #0-#15: 0x4E40-0x4E4F */
    for (int i = 0x4E40; i <= 0x4E4F; i++) {
        table[i] = op_trap;
    }
}
