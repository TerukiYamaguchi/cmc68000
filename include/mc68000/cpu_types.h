/*
 * cpu_types.h — CPU 公開型定義
 *
 * MC68000 は 8 本のデータレジスタ（D0–D7）と
 * 8 本のアドレスレジスタ（A0–A6 + A7/SP）を持つ。
 * A7 はスタックポインタを兼ねるが、スーパーバイザモードと
 * ユーザモードで物理的に別のレジスタ（SSP / USP）が使われる。
 * モード切り替え時に CPU が自動で退避・復帰する。
 *
 * SR（ステータスレジスタ）の上位バイトはスーパーバイザバイト
 *   bit 15: T（トレースモード）
 *   bit 13: S（スーパーバイザ/ユーザ）
 *   bit 10–8: I2–I0（割り込みマスクレベル）
 * 下位バイトが CCR（コンディションコードレジスタ）
 *   bit 4: X（エクステンド）
 *   bit 3: N（ネガティブ）
 *   bit 2: Z（ゼロ）
 *   bit 1: V（オーバーフロー）
 *   bit 0: C（キャリー）
 *
 * mc68k_state_t は不透明型（opaque）にする。
 * x68000 側が内部構造に依存するとヘッダ変更の影響が広がるため。
 */
#pragma once
#include <stdint.h>

/* デバッグ用レジスタスナップショット。mc68k_get_regs() で取得する */
typedef struct {
    uint32_t d[8]; /* D0–D7: データレジスタ */
    uint32_t a[8]; /* A0–A6: アドレスレジスタ、A7: 現在のスタックポインタ */
    uint32_t pc;
    uint16_t sr;  /* ステータスレジスタ（CCR を含む） */
    uint32_t usp; /* ユーザスタックポインタ（ユーザモード退避値） */
    uint32_t ssp; /* スーパーバイザスタックポインタ */
    uint32_t vbr; /* ベクタベースレジスタ（68010+。68000 モデルでは常に 0） */
    uint8_t sfc;  /* ソースファンクションコード（68010+、下位3bitのみ有効） */
    uint8_t dfc;  /* デスティネーションファンクションコード（68010+、下位3bitのみ有効） */
} mc68k_regs_t;

/* CPU 実行状態 */
typedef enum {
    CPU_NORMAL,    /* 通常実行中 */
    CPU_EXCEPTION, /* 例外処理中 */
    CPU_STOPPED,   /* STOP 命令による待機（割り込みで再開） */
    CPU_HALTED     /* ダブルバスフォルトによる停止（電源再投入のみ復帰可） */
} cpu_exec_state_t;

/*
 * CPU モデル（生成時固定）。命令セットの可用性（op_table のモデル別構築）と
 * 例外フレーム形状（68010+ はフォーマットワード付き）を切り替える。
 * 実行中の切り替えは不可（mc68k_set_model() は存在しない。実機の
 * 「CPU差し替えは電源断を伴う」という制約に合わせる。
 */
typedef enum {
    MC68K_MODEL_68000 = 0, /* 既定。mc68k_create() はこれを選ぶ */
    MC68K_MODEL_68010,
    MC68K_MODEL_68020,
    MC68K_MODEL_68EC030,
    MC68K_MODEL_68030,
    MC68K_MODEL_68040,
    MC68K_MODEL_68060,
} mc68k_model_t;

/*
 * FPU 構成。生成時固定。
 * mc68k_create_model_with_fpu() の受理表: 68000/68010 は NONE のみ、
 * 68020/68EC030/68030 は NONE/68881/68882、68040 は NONE/68040/NONE_040、
 * 68060 は NONE/68060。NONE_040 は FPU を持たない MC68EC040/MC68LC040 を表す。
 * NONE_060（MC68LC060/EC060）は予約で、全機種で生成失敗（NULL）。未定義値も生成失敗。
 */
typedef enum {
    MC68K_FPU_NONE = 0,
    MC68K_FPU_68881,
    MC68K_FPU_68882,
    MC68K_FPU_68040,
    MC68K_FPU_NONE_040,
    MC68K_FPU_68060,
    MC68K_FPU_NONE_060,
} mc68k_fpu_config_t;

/* PMMU / access-control register。MC68EC030 では tt0/tt1/mmusr は
 * それぞれ AC0/AC1/ACUSR を表し、tc/crp/srp は常に zero を返す。 */
typedef struct {
    uint32_t tc;
    uint64_t crp;
    uint64_t srp;
    uint32_t tt0;
    uint32_t tt1;
    uint16_t mmusr;
} mc68k_mmu_regs_t;

/* MC68040 MMU register。68030 PMMU snapshot とは register 幅と意味が異なるため
 * 意図的に分離する。 */
typedef struct {
    uint32_t urp, srp;
    uint16_t tc;
    uint32_t dtt0, dtt1, itt0, itt1;
    uint32_t mmusr;
} mc68k_mmu040_regs_t;

/* MC68060 MMU register（TCR は 32bit）。040 の snapshot とは意図的に分離する。MMUSR は無い。 */
typedef struct {
    uint32_t urp, srp, tc;
    uint32_t dtt0, dtt1, itt0, itt1;
} mc68k_mmu060_regs_t;

/* 前方宣言のみ。完全定義は src/cpu/cpu_internal.h（内部ヘッダ）にある */
typedef struct mc68k_state mc68k_state_t;

/*
 * TRAP 命令コールバック型
 *
 * X68000 の IOCS（I/O Control System）は TRAP #15 で呼ばれる。
 * このコールバックを登録することで、IOCS 呼び出しをソフトウェアで
 * エミュレートしたり、ログを取ったりできる。
 *
 * 戻り値:
 *   0 = 通常の例外処理を続ける（SR/PC をスタックに積み例外ベクタへジャンプ）
 *   非 0 = ハンドラが処理した（TRAP の次の命令から実行を再開する）
 */
typedef int (*mc68k_trap_fn_t)(mc68k_state_t *cpu, int trap_num, void *ctx);

/*
 * IRQレベル問い合わせコールバック型（pull型割り込み受付）
 *
 * 戻り値 = 現在 IPL0-2 に出ている割り込みレベル（0 = 要求なし、1-7）。
 * 範囲外の値（8以上、または符号拡張で範囲外になる値）はコア側が
 * fail-safeで「要求なし」(0)として扱う。有効な割り込み（特にレベル7=NMI）へ
 * 化けることはない。
 *
 * 詳細な呼び出し規約は mc68k_set_irq_level_hook() のコメントを参照。
 */
typedef int (*mc68k_irq_level_fn_t)(void *ctx);
