/*
 * cpu.h — mc68000 ライブラリ公開 API
 *
 * 利用側がインクルードする CPU API。src/ 以下の内部ヘッダは公開しない。
 *
 * 使い方:
 *   1. mc68k_bus_t にバスアクセスコールバックを設定する
 *   2. mc68k_create() で CPU インスタンスを生成する
 *   3. mc68k_reset() でリセットシーケンスを実行する
 *      （ベクタテーブルから SSP と PC を読み込む）
 *   4. mc68k_execute() をフレームループから繰り返し呼ぶ
 *   5. 不要になったら mc68k_destroy() で解放する
 */
#pragma once
#include "cpu_types.h"
#include "bus_hooks.h"

typedef struct mc68k_snapshot mc68k_snapshot_t;
typedef struct mc68k_restore_plan mc68k_restore_plan_t;

typedef enum {
    MC68K_SNAPSHOT_OK = 0,
    MC68K_SNAPSHOT_NOMEM,
    MC68K_SNAPSHOT_MISMATCH,
    MC68K_SNAPSHOT_BAD_ARG,
} mc68k_snapshot_status_t;

/* ライフサイクル */
mc68k_state_t *mc68k_create(const mc68k_bus_t *bus);
/*
 * モデルを指定して CPU インスタンスを生成する。mc68k_create() は
 * mc68k_create_model(bus, MC68K_MODEL_68000) に委譲する。
 * モデルは生成時に固定される。
 */
mc68k_state_t *mc68k_create_model(const mc68k_bus_t *bus, mc68k_model_t model);
/*
 * FPU 構成を指定して CPU インスタンスを生成する。
 * mc68k_create_model(bus, model) は mc68k_create_model_with_fpu(bus, model,
 * MC68K_FPU_NONE) を呼ぶ互換ラッパ。受理表: 68000/68010 は NONE のみ、
 * 68020/68EC030/68030 は NONE/68881/68882、68040 は NONE/68040/NONE_040。
 * 未定義の fpu は NULL を返す（model の値は従来どおり検査しない）。
 * mc68k_get_fpu() は CIR 接続の68881/68882だけを返す。68040内蔵FPUは CPU 内部の
 * 専用状態であり、この関数からは取得できない。
 */
mc68k_state_t *mc68k_create_model_with_fpu(const mc68k_bus_t *bus, mc68k_model_t model,
                                           mc68k_fpu_config_t fpu);
/* 生成時に確定したモデルを返す */
mc68k_model_t mc68k_get_model(const mc68k_state_t *cpu);
void mc68k_destroy(mc68k_state_t *cpu);

/* CPU 状態をメモリ内へ保存する。失敗時は NULL を返し、cpu は変更しない。 */
mc68k_snapshot_t *mc68k_snapshot_create(const mc68k_state_t *cpu,
                                         mc68k_snapshot_status_t *status);
/* 復元の構成検査と必要な確保を行う。失敗時は cpu を変更しない。 */
mc68k_restore_plan_t *mc68k_snapshot_prepare(const mc68k_state_t *cpu,
                                              const mc68k_snapshot_t *snap,
                                              mc68k_snapshot_status_t *status);
/* 準備済み plan で復元する。呼出し側は停止中かつ命令境界で呼ぶこと。 */
void mc68k_snapshot_commit(mc68k_state_t *cpu, const mc68k_snapshot_t *snap,
                           mc68k_restore_plan_t *plan);
void mc68k_snapshot_plan_destroy(mc68k_restore_plan_t *plan);
void mc68k_snapshot_destroy(mc68k_snapshot_t *snap);
/*
 * リセットベクタから PC/SSP を初期化する。
 *
 * reset-and-halt（リセットした状態のまま実行を止めておくユースケース）:
 * 専用APIは用意しない。
 * mc68k_reset(cpu) を呼んだ後、ホストが単に mc68k_execute()/
 * mc68k_debug_run() を呼ばなければ、それだけで「リセットして停止」した
 * ことになる（continue/halt APIと同じ協調的フレームループモデルを踏襲、
 * halt専用APIが不要なのと同じ理由）。
 *
 * ブレークポイントは維持されるが、ウォッチポイント（debug.h 参照）が
 * 登録されている場合、リセットでメモリ/レジスタの内容が
 * 変わりうるため前回値を自動的に再取得する（そうしないとリセット直後の
 * 最初のステップで偽ヒットになる）。
 */
void mc68k_reset(mc68k_state_t *cpu);

/*
 * 最大 cycles サイクル実行する。
 * 実際に消費したサイクル数を返す（命令境界で止まるため cycles を超えることがある）。
 *
 * --- cycles=1 によるシングルステップのセマンティクス ---
 *
 * mc68k_execute(cpu, 1) はデバッガのシングルステップ実行プリミティブとして
 * そのまま使える（ステップ実行専用のAPIは新設しない、mc68k_debug_run() も
 * これを内部で使う）。「1 呼び出し = 1 ステップ」が成立する根拠は、内部の
 * 命令実行ループが「サイクル予算を消費する作業を必ず1回だけ行ってから
 * 予算を確認する」構造になっており、かつループ本体のどの経路も必ず正の
 * サイクル数を消費する（NOP ですら 4）ため、cycles=1 を渡すと1回の
 * ループ本体実行後に必ず予算超過で抜けるという性質による。ただし「1 ステップ」
 * が具体的に何を意味するかは状況によって以下のように変わる:
 *
 * 1. 通常時: 1命令をフェッチ・デコード・実行する。PC は次の命令へ進む。
 *
 * 2. 割り込み受付: 呼び出し時点で SR の割り込みマスクを超える IRQ が
 *    保留していた場合、その回の「1ステップ」は保留中の通常命令の実行では
 *    なく、割り込み受付シーケンス（PC/SR をスタックへ積みベクタへジャンプ）
 *    になる。この場合、呼び出し前の PC が指していた命令は実行されない
 *    （次回以降の呼び出しで改めて実行される）。
 *
 * 3. group0例外（バス/アドレスエラー）のディスパッチ: 命令フェッチ中または
 *    命令実行中に発生した場合、同一の mc68k_execute(cpu,1) 呼び出し内で
 *    ディスパッチまで完了し、PC は例外ハンドラの先頭を指す（元の命令は
 *    実行されなかった/未完了のまま中断）。
 *
 *    稀なケースとして、上記2の割り込み受付シーケンス自体がスタックプッシュ
 *    でバス/アドレスエラーを起こした場合、その呼び出しは exec_state ==
 *    CPU_EXCEPTION のまま（未解決の保留状態で）呼び出し元に返る。次回の
 *    mc68k_execute() 呼び出しはまずこの保留分をディスパッチしてから、
 *    続けて新規に1命令実行する（＝この1回の呼び出しだけ「保留の解決＋
 *    新規1命令」の2ステップ相当が起きる）。mc68k_debug_run() はこの
 *    ケースを検出すると MC68K_STOP_EXCEPTION として即座に停止するため
 *    実害はないが、例外ハンドラの先頭に置かれたブレークポイントが
 *    まれに検出できないことがある（詳細は debug.h の mc68k_debug_run()
 *    コメント参照）。
 *
 * 4. トレース例外（ベクタ9）: 呼び出し前に SR の T ビットが立っていた
 *    場合、その回の命令が正常完了した（Group1例外で中断されていない）
 *    ときに限り、命令実行に続けて同一呼び出し内でトレース例外の
 *    ディスパッチまで行われる。つまりこの場合も PC は命令の次ではなく
 *    トレースハンドラの先頭を指す。ゲスト自身が T ビットを立てて自前の
 *    シングルステップ機構（DB.X等のゲスト側デバッガ）を実装している
 *    場合にも同じ経路を通る。ホスト側デバッガのシングルステップ実行
 *    自体は T ビットに一切依存しない（cycles=1 を渡すだけでよい）ため、
 *    この経路はゲストが独自にトレースを使っているときのみ関係する。
 *
 * 5. TRAPフックとの相互作用: TRAP #n 命令は mc68k_set_trap_handler() で
 *    登録されたコールバックが非0を返すと、例外ディスパッチ自体を行わず
 *    次の命令から実行を再開する（通常のTRAP命令実行と同じ1ステップ）。
 *    この場合ベクタ番号(32+n)の例外は発生しないため、
 *    mc68k_set_exception_break_mask() でそのベクタを監視していても
 *    mc68k_debug_run() は反応しない（ディスパッチ自体が起きていないため）。
 *    ホストがTRAPの発生そのものを検知したい場合はtrap_fn側で明示的に
 *    記録する必要がある。
 */
int mc68k_execute(mc68k_state_t *cpu, int cycles);

/*
 * 現在（または直近）の mc68k_execute() 呼出しで、実行中の命令の開始までに
 * 消費したサイクル数を返す。
 *
 * - バスコールバック内から呼べる。命令の途中では実行中の命令の分を含まない
 *   （命令境界の値）。誤差は最大1命令分。#176 のバスウェイトは、前の命令の分として
 *   含まれる。
 * - mc68k_execute() の先頭（早期 return より前）で0に戻る。戻った後に呼ぶと、
 *   その呼出しの戻り値と一致する。
 * - mc68k_debug_run() は内部で mc68k_execute(cpu, 1) を繰り返すため、
 *   各呼出しごとに0から数え直す（debug_run 全体の累計ではない）。
 * - 副作用なし・O(1)。
 * - mc68k_reset() の直後は0。
 */
int mc68k_executed_cycles(const mc68k_state_t *cpu);

/*
 * 現在の mc68k_execute() 呼出し内で、進行中の外部バスアクセス直前までに
 * 経過した CPU サイクル数を返す。
 *
 * 初期実装は MC68060 の bus callback だけを対象とする。68060 以外、
 * callback 外、または out == NULL では false を返し、out は変更しない。
 * 値は命令境界近似であり、直前までに完了した命令と有効な先行 bus wait を
 * 含む。同一命令内の callback は、先行 wait がなければ同じ値を返す。
 * 副作用なし・O(1)。
 */
bool mc68k_bus_access_cycles(const mc68k_state_t *cpu, uint64_t *out);

/*
 * 割り込み要求を設定する（push/latch型）。
 * level 0 = 解除、1–7 = 割り込みレベル（7 が最高優先度・NMI 相当）。
 * MC68000 は SR の割り込みマスク（I2–I0）より高いレベルのみ受け付ける。
 *
 * mc68k_set_irq_level_hook() でpull型フックを登録している間、この関数の
 * 効果は無視される（pending_irqへの代入自体は行われるが判定に使われない）。
 */
void mc68k_set_irq(mc68k_state_t *cpu, int level);

/*
 * IRQレベル問い合わせフックを登録する（pull型割り込み受付）。
 *
 * 実機MC68000のIPL0-2はlevel-sensitiveな入力ピンで、CPUは命令境界ごとに
 * 再サンプリングする。mc68k_set_irq()によるpush/latch型は、ホスト側が
 * IRQ線の更新をポーリング粒度でしか行えない場合、「もう条件が消えたはずの
 * 古いIRQ線」を受理してしまう窓を生む。本フックはその窓を構造的になくす。
 *
 * 呼び出し規約（実装する側が必ず守ること）:
 *   1. fn == NULL で登録解除。既定値はNULL（push/latch型のまま、
 *      mc68k_create()のcallocにより未登録時は必ずNULL）。
 *   2. mc68k_execute()のループ先頭（命令境界、命令フェッチより前）で
 *      毎回1回呼ばれる。加えてCPU_STOPPEDからの再開判定でも1回呼ばれる
 *      （同一命令境界で最大2回呼ばれうる）。
 *   3. 副作用禁止・冪等であること。同一命令境界で複数回呼ばれても
 *      同じ値を返すこと。割り込みのクリアはここで行ってはならない
 *      （それはiackコールバックの責務）。デバイスのread-clearレジスタに
 *      触れてもならない。
 *   4. mc68k_signal_bus_error()を呼んではならない（IACKサイクルではない
 *      ため、スプリアス割り込みの文脈が存在しない。呼ぶとフラグが残留し
 *      後続の無関係なアクセスを誤爆させる）。
 *   5. ここで返したレベルがそのままiack(ctx, level)へ渡される。同一の
 *      デバイス集合から一貫した値を返すこと。
 *   6. 毎命令呼ばれるホットパス。O(1)で返せる実装にすること。
 *   7. 戻り値は0-7のIPL 3bit値。範囲外はコア側がfail-safeで
 *      「要求なし」(0)として扱う（有効な割り込み、特にレベル7=NMIへ
 *      化けることはない）。範囲チェックはコア側の責務であり、
 *      呼び出し側で保証する必要はない。
 *   8. コールバック内からmc68k_set_irq_level_hook()自身を呼ぶ
 *      （実行中の付け替え）ことはサポートしない。
 *
 * X68000 エミュレータ側での使用例:
 *   int x68k_irq_level(void *ctx) {
 *       x68k_bus_t *b = ctx;
 *       if (b->nmi_pending) { b->nmi_pending = 0; return 7; }
 *       if (x68k_bus_ioi_pending_irq(b)) return 1;
 *       if (x68k_bus_dmac2_pending_irq(b) || x68k_bus_dmac3_pending_irq(b)) return 3;
 *       if (x68k_bus_scc_pending_irq(b)) return 5;
 *       if (x68k_bus_mfp_pending_irq(b)) return 6;
 *       return 0;
 *   }
 *   mc68k_set_irq_level_hook(cpu, x68k_irq_level, bus);
 */
void mc68k_set_irq_level_hook(mc68k_state_t *cpu, mc68k_irq_level_fn_t fn, void *ctx);

/* デバッグ用: レジスタスナップショット取得 */
void mc68k_get_regs(const mc68k_state_t *cpu, mc68k_regs_t *out);
void mc68k_get_mmu_regs(const mc68k_state_t *cpu, mc68k_mmu_regs_t *out);
void mc68k_get_mmu040_regs(const mc68k_state_t *cpu, mc68k_mmu040_regs_t *out);
/* MC68060 の MMU register。060 以外では全て 0。 */
void mc68k_get_mmu060_regs(const mc68k_state_t *cpu, mc68k_mmu060_regs_t *out);
uint32_t mc68k_get_pc(const mc68k_state_t *cpu);
cpu_exec_state_t mc68k_get_exec_state(const mc68k_state_t *cpu);

/*
 * 現在のCPUモードを返す（SR bit13 = S）。1 = スーパーバイザ、0 = ユーザ。
 * バスコールバック（read_byte/write_byte 等）から呼ばれるホットパス用の
 * 軽量アクセサ。mc68k_get_regs() は全レジスタのフルコピーで不向きなため、
 * mc68k_get_pc() と同様にこちらを使うこと。
 */
int mc68k_is_supervisor(const mc68k_state_t *cpu);

/* アドレスエラー/バスエラー情報取得（exec_state == CPU_EXCEPTION 時のみ有効） */
uint32_t mc68k_get_addr_err_addr(const mc68k_state_t *cpu);

/*
 * バスエラー通知（iack コールバック専用）。
 *
 * bus_hooks.h の read_byte/read_word/read_long/write_byte/write_word/
 * write_long は戻り値（bool、false = バスエラー）で直接通知する方式に
 * 統一されているため、これらの実装内から呼ぶ必要はない。
 *
 * 本関数は iack コールバックの実装内で、IACKサイクルにどのデバイスも
 * 応答しなかった（= スプリアス割り込み）ことを通知するためだけに使う。
 * iack はベクタ番号（uint8_t、0 = オートベクタ要求）を返す都合上、
 * 「該当デバイスなし」を戻り値だけでは表現できないため。
 *
 * X68000 エミュレータ側での使用例:
 *   uint8_t x68k_iack(void *ctx, int level) {
 *       if (!any_device_responds(level)) {
 *           mc68k_signal_bus_error(((x68k_ctx_t *)ctx)->cpu);
 *           return 0;
 *       }
 *       ...
 *   }
 */
void mc68k_signal_bus_error(mc68k_state_t *cpu);

/* TRAP ハンドラ内でデータレジスタを書き換える（IOCS 戻り値の設定に使う） */
void mc68k_set_dreg(mc68k_state_t *cpu, int n, uint32_t val);

/*
 * TRAP コールバックを登録する。
 *
 * fn == NULL で登録解除。コールバック未登録時は TRAP が来ると
 * 通常の例外処理（SR/PC をスタックに積んでベクタへジャンプ）を行う。
 *
 * X68000 エミュレータ側での IOCS 接続:
 *   mc68k_set_trap_handler(cpu, iocs_trap_handler, x68k_ctx);
 *   ↑ TRAP #15 が来るたびに iocs_trap_handler が呼ばれ、
 *     D0 の値から IOCS 関数を特定してエミュレートする。
 */
void mc68k_set_trap_handler(mc68k_state_t *cpu, mc68k_trap_fn_t fn, void *ctx);
