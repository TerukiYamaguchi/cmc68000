/*
 * cpu_internal.h — CPU 内部ヘッダ（外部 include 禁止）
 *
 * 命令デコードは 65536 エントリのフラット関数ポインタテーブルで行う。
 * MC68000 の opcode は 16bit なので、テーブルの添字が opcode そのものになる。
 * 実行時は op_table[opcode](cpu, opcode) の一発参照で済む。
 *
 * テーブルサイズは関数ポインタ 8 バイト × 65536 = 512 KiB（64bit環境）。
 * mc68k_state_t 内に直接埋め込むと構造体が巨大になるため
 * mc68k_create() でヒープ確保し、ポインタだけを持つ。
 *
 * 逆アセンブラは opcode を独立にデコードするため、このテーブルには
 * 実行に必要なハンドラだけを保持する。
 */
#pragma once
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "mc68000/debug.h"
#include "mc68000/fpu.h"
#include "fpu/fpu040.h"
#include "cycles.h"
#include "ea_helpers.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

/* ホットなアクセス経路で、コンパイラの判断に任せると展開されない小さな関数を必ず展開する、
 * あるいは遅い経路を 1 つの外部関数にまとめるための指定。 */
#if defined(_MSC_VER)
#define MC68K_ALWAYS_INLINE static __forceinline
#define MC68K_NOINLINE      __declspec(noinline)
#else
#define MC68K_ALWAYS_INLINE static inline __attribute__((always_inline))
#define MC68K_NOINLINE      __attribute__((noinline))
#endif

/* ------------------------------------------------------------------ */
/* SR / CCR ビット定数                                                  */
/* ------------------------------------------------------------------ */

/*
 * ステータスレジスタ（SR）の上位バイト（スーパーバイザバイト）
 *   bit 15: T  トレースモード（1 = 毎命令後にトレース例外）
 *   bit 13: S  スーパーバイザモード（1 = スーパーバイザ、0 = ユーザ）
 *   bit 10–8: I2–I0  割り込みマスクレベル（0–7）
 */
#define SR_T1 ((uint16_t)(1 << 15))
#define SR_T  SR_T1 /* 68000/68010 compatibility name */
#define SR_T0 ((uint16_t)(1 << 14))
#define SR_S  ((uint16_t)(1 << 13))
#define SR_M  ((uint16_t)(1 << 12))
#define SR_I2 ((uint16_t)(1 << 10))
#define SR_I1 ((uint16_t)(1 << 9))
#define SR_I0 ((uint16_t)(1 << 8))

/*
 * コンディションコードレジスタ（CCR）— SR 下位バイト
 *   X: エクステンドビット（多倍長演算のキャリー保持）
 *   N: ネガティブ（結果の MSB）
 *   Z: ゼロ（結果が 0）
 *   V: オーバーフロー（符号付き演算の桁あふれ）
 *   C: キャリー（符号なし演算の桁あふれ）
 */
#define CCR_X ((uint16_t)(1 << 4))
#define CCR_N ((uint16_t)(1 << 3))
#define CCR_Z ((uint16_t)(1 << 2))
#define CCR_V ((uint16_t)(1 << 1))
#define CCR_C ((uint16_t)(1 << 0))

/* リセット時の初期 SR: スーパーバイザモード + 割り込みマスク 7 */
#define SR_RESET ((uint16_t)(SR_S | SR_I2 | SR_I1 | SR_I0))

/* SR から割り込みマスクレベル（0–7）を取り出す */
#define SR_IRQ_MASK(sr) (((sr) >> 8) & 7)

/*
 * MC68030 / MC68EC030 CACR。
 * 値は ops_system_ext.c の MOVEC 受理マスクと一致する
 * （030系 0x3313、020以下 0x03）。CEI(bit3)/CED(bit11) は書込み時のみ意味を
 * 持つ一過性ビットなのでマスクに含めない。
 */
#define MC68K_CACR_EI UINT32_C(0x00000001) /* Enable Instruction cache */
#define MC68K_CACR_FI UINT32_C(0x00000002) /* Freeze Instruction cache */
#define MC68K_CACR_CI UINT32_C(0x00000010) /* Clear Instruction cache */
#define MC68K_CACR_ED UINT32_C(0x00000100) /* Enable Data cache */
#define MC68K_CACR_FD UINT32_C(0x00000200) /* Freeze Data cache */
#define MC68K_CACR_CD UINT32_C(0x00001000) /* Clear Data cache */
#define MC68K_CACR_WA UINT32_C(0x00002000) /* Write Allocate */
#define MC68K_CACR_030_MASK                                                          \
    (MC68K_CACR_EI | MC68K_CACR_FI | MC68K_CACR_CI | MC68K_CACR_ED | MC68K_CACR_FD | \
     MC68K_CACR_CD | MC68K_CACR_WA) /* = 0x3313 */
#define MC68K_CACR_020_MASK UINT32_C(0x00000003)

/* ------------------------------------------------------------------ */
/* 命令ハンドラとデコードテーブル                                        */
/* ------------------------------------------------------------------ */

/* 命令ハンドラ。消費サイクル数を返す */
typedef int (*op_handler_t)(mc68k_state_t *cpu, uint16_t opcode);

/* 通常例外フレームは書込み中にフォルトしうる。続く Group-0 フレームは例外前の
 * スタックバンクを保存し、その RTE が中断時のアーキテクチャ状態へ戻れるようにする。
 * 部分フレームへ戻してはならない。例外ネストに人工的な上限を設けないため、レコードはヒープで連結する。
 */
typedef struct mc68k_exception_frame_origin {
    struct mc68k_exception_frame_origin *next;
    uint16_t saved_sr;
    uint32_t saved_pc;
    uint32_t a7, usp, ssp, msp;
    uint32_t group0_base;
    bool bound;
} mc68k_exception_frame_origin_t;

/* ------------------------------------------------------------------ */
/* CPU 内部状態の完全定義                                               */
/* ------------------------------------------------------------------ */

#define PC_RING_SIZE MC68K_PC_RING_SIZE /* PC トレース・リングバッファ長（2 の冪） */

#define REG_RING_SIZE MC68K_REG_RING_SIZE /* レジスタ履歴リングバッファ長（2 の冪） */

/* ブレークポイント1スロット。
 * in_use = スロット使用中フラグ（削除後の再利用管理）。 */
typedef struct {
    uint32_t addr;
    bool enabled;
    bool in_use;
    mc68k_bp_cond_fn_t cond;
    void *cond_ctx;
} mc68k_breakpoint_t;

/* ウォッチポイント1スロット。
 * last_value は mc68k_watchpoint_check() が現在値との比較・更新に使う
 * （エッジトリガ方式。呼ぶこと自体が状態を更新する副作用を持つ）。 */
typedef struct {
    uint32_t addr;
    int size; /* 1, 2, 4 */
    uint32_t last_value;
    bool enabled;
    bool in_use;
} mc68k_watchpoint_t;

/* コールスタックリングバッファ長 */
#define CALL_RING_SIZE  MC68K_CALL_RING_SIZE
#define FAULT_RING_SIZE MC68K_FAULT_RING_SIZE

/*
 * グループ0フォルトの連続再発をハルトと判定する回数。
 * 1 回目 = 最初のフォルト、2 回目 = フォーマット $8 の RTE で復帰した直後に
 * ベクタ・アクセスアドレス・命令先頭 PC がすべて一致するフォルトが再発した回。
 * 本コアはフォルトしたバスサイクルの再発行を実装しないため、
 * この 2 回目の時点で「CPU の状態が前進していない」ことが論理的に確定する。
 * 3 回目以降を待っても新しい情報は得られないので 2 が必要十分。
 * ＝ 経験的に調整したしきい値ではない。
 */
#define MC68K_GROUP0_REFAULT_LIMIT 2

#define MC68K_ATC_ENTRIES   22
#define MC68K_060_ATC_HINTS 16 /* ATC ヒント表の要素数（2 のべき乗） */
#define MC68K_060_ATC_ENTRIES \
    64 /* 060 の命令 ATC・データ ATC は各 64 entry（[060UM §4.1.2 FOTC=0]） */
#define MC68K_040_CACHE_SETS           64
/* 分岐キャッシュは [060UM §5.11]。timing_060.c は direct-mapped で模す */
#define MC68K_060_BRANCH_CACHE_ENTRIES 256u
#define MC68K_060_CACHE_SETS           128 /* 8KB = 128 set x 4 way x 16 byte（[060UM §5.1]） */
#define MC68K_CACHE_SETS_MAX           MC68K_060_CACHE_SETS
#define MC68K_040_CACHE_WAYS           4
/* CACR (MC68040UM Figure 4-4). */
#define MC68K_CACR_040_DE              UINT32_C(0x80000000)
#define MC68K_CACR_040_IE              UINT32_C(0x00008000)
#define MC68K_CACR_060_DPI             UINT32_C(0x10000000) /* CPUSH: push のみで無効化しない */
typedef struct {
    uint32_t physical_line;
    uint32_t data[4];
    uint8_t dirty;
    bool valid, loaded;
} mc68k_040_cache_line_t;
typedef struct {
    uint32_t logical_page, physical_page;
    uint8_t fc, page_shift, cache_mode;
    bool valid, bus_error, cache_inhibit, write_protect, modified, global;
    uint8_t fault_reason; /* mmu_fault_t。負ATCエントリだけで使用 */
    uint64_t history;     /* LRU の時刻（060 は 32bit だと周回する） */
} mc68k_atc_entry_t;
/* 68060 の命令フェッチ用高速経路の状態。直近に ATC で解決した命令側 entry の slot 添字と、
 * 変換時の TC・ITT0/1 を覚える。結果（物理アドレス・cache mode）はコピーせず、毎回 entry
 * から読む。 S とページの一致は毎フェッチ確認する。TC・ITT0/1 と実 entry の再検証は、
 * 検証時の世代 gen が cpu->ifast_gen と違うときだけ行う。TC・ITT・ATC の entry を変える操作は
 * cpu_ifetch_fast_invalidate() で世代を進める。 */
typedef struct {
    uint64_t gen;
    uint32_t logical_page;
    uint32_t tc, itt0, itt1;
    uint8_t slot, fc_s, page_shift;
    bool valid;
} mc68k_atc060_ifast_t;

/*
 * PMMU 変換は CPU 例外配送と意図的に分離して表現する。PTEST とデバッガ読出しは
 * 実行中のバスサイクルと同じ walk を必要とするが、CPU
 * 状態を変えずに失敗を調べなければならない。
 */
typedef enum {
    MMU_FAULT_NONE,
    MMU_FAULT_INVALID,
    MMU_FAULT_WRITE_PROTECT,
    MMU_FAULT_SUPERVISOR,
    MMU_FAULT_LIMIT,
    MMU_FAULT_TABLE_BUS_ERROR,
} mmu_fault_t;

typedef enum {
    MC68K_TRANSLATE_IDENTITY,
    MC68K_TRANSLATE_TT,
    MC68K_TRANSLATE_ATC,
    MC68K_TRANSLATE_WALK,
} mc68k_translate_source_t;

/*
 * PTEST はここに含めない。検索規則が通常の変換の派生ではないためである
 * （LEVEL 0 は ATC だけ、LEVEL 1--7 はテーブルだけを検索する）。専用の入口 cpu_mmu_ptest()
 * を持つ。
 */
typedef enum {
    MC68K_TRANSLATE_EXECUTE,
    MC68K_TRANSLATE_PLOAD,
    MC68K_TRANSLATE_DEBUG,
} mc68k_translate_mode_t;

typedef struct {
    uint8_t fc;
    bool read;
    bool rmw;
    uint8_t size;
    bool instruction;
    bool split;     /* 奇数アドレス等で分割された下位アクセス。68040 の WB2 保留の対象外 */
    bool alternate; /* MOVES の SFC/DFC によるアクセス。68040 の WB status の TT は 10。060 は
                       SFC/DFC=0/3/4/7 だけ (変換せず noncachable)。1/2/5/6 は通常の data
                       アクセスとして扱う（TT=00） */
    /* 分割アクセスの元の先頭アドレス。060 の FSLW.MA（2 回目以降のアクセスの fault）と、
     * 分割しても fault address は最初の論理アドレスになる規則に使う。 */
    bool has_origin;
    uint32_t origin;
    uint8_t origin_size;
} mc68k_access_t;

typedef struct {
    bool ok;
    mc68k_translate_source_t source;
    uint32_t physical;
    bool cache_inhibit;
    uint8_t cache_mode; /* 0=write-through, 1=copyback, 2/3=cache inhibited */
    /* write_protect is the logical OR of WP over every descriptor traversed
     * and is independent of the access direction; fault carries WRITE_PROTECT
     * only when the access was actually denied.  modified is the M bit of the
     * final page descriptor as it was read. */
    bool write_protect, modified;
    mmu_fault_t fault;
    bool indirect_fault; /* 060: 2 つ目の indirect descriptor に当たった（FSLW.IL） */
    uint16_t mmusr;
    uint8_t level; /* テーブル段数（＝MMUSR.N）。透過変換・直接マッピングは 0 */
    uint32_t descriptor_physical;
    uint32_t table_bus_error_physical;
} mc68k_translate_result_t;

/* MC68040 format $7 の入力。MMU、キャッシュ、整数パイプラインのコードが Group-0
 * access error を起こす前に設定する。例外ビルダはこれを直列化するだけで、030 SSW から
 * 040 のフィールドを導出しない。 */
enum {
    WB_STATUS_VALID = 0x80u,
    /* SSW / WBnS の TT = 10: 代替ファンクションコード（MOVES） */
    TT_ALTERNATE = 0x10u,
};

typedef struct {
    bool valid;
    uint32_t effective_address;
    uint16_t ssw;
    uint8_t wb_status[3]; /* [0]=WB3S, [1]=WB2S, [2]=WB1S */
    uint32_t fault_address;
    uint32_t wb_address[3];
    uint32_t wb_data[3];   /* [2] is WB1D or PD0 */
    uint32_t push_data[3]; /* PD1..PD3 */
    uint8_t post_vector;   /* CP: pending FPU post-instruction vector */
} mc68k_040_access_error_t;

/* MC68040 の書き込み ATC フォルトの保留ストア（WB2、040UM §8.4.6）。実機は書き込みを
 * 保留ストアとして持つので命令は完了する。valid の間は命令が最後まで実行され、命令境界で
 * フレームを積む。メモリへは書かない。 */
typedef struct {
    bool valid;
    uint32_t logical;
    uint32_t data;
    mc68k_access_t access;
    mc68k_translate_result_t mmu;
} mc68k_040_pending_write_t;

/* FSAVE の body の最大長（long）。68882 の busy frame が 53 */
#define CP_SAVE_BODY_MAX_WORDS 64u

struct mc68k_state {
    mc68k_model_t model; /* 生成時固定（mc68k_create_model()）。命令可用性は op_table 側で
                          * 解決済みなので、ここを見るのは例外フレーム形状の判定のみ */
    mc68k_fpu_config_t fpu_config; /* 生成時固定。68040 の F-line 分類にも使う。 */
    mc68k_fpu_t *fpu;       /* CIR 接続の68881/68882だけを所有。その他の構成では NULL。 */
    mc68k_fpu040_t *fpu040; /* MC68K_FPU_68040 の内蔵FPU状態。CIRモデルとは混在させない。 */
    ea_desc_t last_ea;      /* 直近にEAヘルパーが解決した形式（Phase 2a timing layer） */
    uint32_t vbr;           /* ベクタベースレジスタ（68010+）。68000 モデルでは常に 0 */
    uint8_t sfc;            /* ソースファンクションコード（3bit 有効、68010+） */
    uint8_t dfc;            /* デスティネーションファンクションコード（3bit 有効、68010+） */
    uint32_t d[8];          /* データレジスタ D0–D7 */
    uint32_t a[8];          /* アドレスレジスタ A0–A7（A7 = 現在の SP） */
    uint32_t pc;
    uint16_t sr;  /* ステータスレジスタ */
    uint32_t usp; /* ユーザ SP（スーパーバイザモード時に退避） */
    uint32_t ssp; /* スーパーバイザ SP（ユーザモード時に退避） */
    uint32_t msp; /* 68020+ master supervisor SP */
    uint32_t cacr, caar;
    uint8_t access_cache_mode;
    bool access_tt_hit; /* 060: 直近の変換が TTR に一致した（host bus error の FSLW.TTR） */
    mc68k_040_cache_line_t icache040[MC68K_CACHE_SETS_MAX][MC68K_040_CACHE_WAYS];
    mc68k_040_cache_line_t dcache040[MC68K_CACHE_SETS_MAX][MC68K_040_CACHE_WAYS];
    uint8_t icache040_next[MC68K_CACHE_SETS_MAX], dcache040_next[MC68K_CACHE_SETS_MAX];
    /* 命令側で直近にヒットまたは充填したライン（icache040 の平坦な添字）。
     * cpu_060_ifetch_fast() が使う。使うたびに valid・loaded・physical_line を検証するので、
     * 古くても結果は変わらない。 */
    uint16_t icache040_last;
    /* 現在の命令／例外処理で積み上げたバスウェイト。
     * mc68k_execute() が反復ごとに cpu_take_bus_wait() で吸い出して 0 へ戻す。 */
    int bus_wait_accum;
    /* mc68k_executed_cycles() の値。mc68k_execute() が管理する。 */
    int exec_cycles_at_insn_start;
    /* 68060 の外部バス callback に公開する命令境界近似時刻。 */
    uint64_t bus_access_cycles;
    bool bus_access_epoch;
    bool bus_access_active;
    /* モデル別サイクルタイミング解決済み表。
     * mc68k_create_model() 内、build_op_table() と同じ場所で cyc_resolve()
     * が一度だけ構築する。CYC_ABSENT はこのモデルに存在しない命令のクラス。
     * それ以外は必ず正の値（cyc_get() の assert(> 0) が未解決=0残留を検出する）。 */
    int16_t cyc[CYC_CLASS_COUNT];
    /* 68040 の timing が 040UM の表値から決まった回数。
     * cpu_040_tag() だけが加算する。試験が命令実行の前後で比較し、表値を
     * 経由せず他機種の値へ落ちた（fallback）実行を検出する。 */
    uint32_t cyc040_tagged;
    /* 060 の命令時間（timing_060.c）。mc68k_execute() が命令ごとに初期化する。
     * t060_ea_extra: full format EA の追加計算時間。t060_aux: 命令固有の補助値（MOVEM の
     * mask、MOVEC の制御 register、bit field の 5 byte 跨ぎ、Bcc の後方分岐、FPU の拡張語）。
     * t060_exc: 命令中に 060 の例外時間で例外を積んだ。 */
    int t060_ea_extra;
    uint32_t t060_aux;
    bool t060_exc;
    /* 060 の branch cache の簡略モデル（timing_060.c）。tag は pc|1、0 は未登録。 */
    uint32_t t060_bc_tag[MC68K_060_BRANCH_CACHE_ENTRIES];
    uint8_t t060_bc_user[MC68K_060_BRANCH_CACHE_ENTRIES];
    uint32_t mmu_tt0, mmu_tt1; /* EC030 AC0/AC1; 68030 TT0/TT1 */
    uint16_t mmu_sr;           /* EC030 ACUSR; 68030 MMUSR */
    uint32_t mmu_tc;
    uint64_t mmu_crp, mmu_srp;
    mc68k_atc_entry_t atc[MC68K_ATC_ENTRIES];
    uint32_t atc_clock;
    uint32_t mmu040_urp, mmu040_srp;
    uint16_t mmu040_tc;
    uint32_t mmu040_dtt0, mmu040_dtt1, mmu040_itt0, mmu040_itt1, mmu040_mmusr;
    uint32_t pcr, buscr; /* MC68060 control registers; separate from 040 MMU state. */
    mc68k_atc_entry_t atc040_i[MC68K_ATC_ENTRIES];
    mc68k_atc_entry_t atc040_d[MC68K_ATC_ENTRIES];
    uint32_t atc040_clock;
    /* MC68060 の MMU。040 の状態とは共有しない（設計書 §5）。ATC の tag は FC2 だけ。 */
    uint32_t mmu060_tc, mmu060_urp, mmu060_srp;
    uint32_t mmu060_itt0, mmu060_itt1, mmu060_dtt0, mmu060_dtt1;
    mc68k_atc_entry_t atc060_i[MC68K_060_ATC_ENTRIES];
    mc68k_atc_entry_t atc060_d[MC68K_060_ATC_ENTRIES];
    uint64_t atc060_clock;
    /* [0]=data,[1]=instruction。logical page 下位ビットで引く「直近ヒット slot」のヒント表。
     * 使用前に必ず実 entry（valid・fc・page_shift・logical_page）と照合するので、PFLUSH・
     * slot 上書き・snapshot 復元で古くなっても無効化は不要。 */
    uint8_t atc060_hint[2][MC68K_060_ATC_HINTS];
    mc68k_atc060_ifast_t atc060_ifast;
    /* 命令フェッチ高速経路の世代。単調に増やし、snapshot
     * 復元でも戻さない（snapshot_excludes）。 */
    uint64_t ifast_gen;
    bool atc060_ifast_disable;    /* 試験用: true なら命令フェッチの高速経路を使わない */
    bool ifetch_fast_disable;     /* 試験用: true なら命令側ライン高速経路を使わない */
    bool atc060_dfast_disable;    /* 試験用: true ならデータアクセスの高速経路を使わない */
    bool dcache_hit_fast_disable; /* 試験用: true なら D-cache の読出しヒットの inline
                                     経路を使わない */
    cpu_exec_state_t exec_state;
    int pending_irq;                   /* 保留中の割り込みレベル（0 = なし）。
                                        * irq_level_fn 未登録時（push/latch型）のみ判定に使われる
                                        * 。 */
    mc68k_irq_level_fn_t irq_level_fn; /* pull型IRQレベル問い合わせフック（NULL = 未登録、
                                        * push/latch型のまま。） */
    void *irq_level_ctx;               /* irq_level_fn へ渡す不透明コンテキスト */
    bool nmi_taken;                    /* レベル7(NMI)のエッジ検出。pull型でのみ使用（latch型は
                                        * pending_irq=0によるクリアで代替するため不要）。線が7未満に
                                        * 下がると irq_sample_level() がクリアし、再度7を受け付け
                                        * 可能にする */
    mc68k_bus_t bus;                   /* バスコールバック（値コピー） */
    op_handler_t *op_table;            /* 65536 エントリのデコードテーブル */

    /* 非停止エントリプローブ。有効時だけop_tableの全エントリを
     * 共通トランポリンへ置き換え、元のハンドラ表をentry_probe_saved_tableに
     * 保持する。無効時はop_tableを直接呼ぶ従来のホットパスのままである。 */
    op_handler_t *entry_probe_saved_table;
    uint32_t entry_probe_pcs[MC68K_MAX_ENTRY_PROBES];
    size_t entry_probe_count;
    mc68k_entry_probe_fn_t entry_probe_fn;
    void *entry_probe_ctx;
    bool entry_probe_active;

    /* グループ0例外（アドレスエラー/バスエラー）情報（例外処理ハンドラが参照する） */
    uint32_t addr_err_addr; /* エラーが発生したアドレス */
    bool addr_err_read;     /* true = 読み出し中、false = 書き込み中 */
    int fault_vector;       /* 2 = バスエラー、3 = アドレスエラー */
    enum { FAULT_OPCODE, FAULT_EXTENSION, FAULT_DATA_READ, FAULT_DATA_WRITE } fault_access_kind;
    /* 直近に記録したバスサイクルの属性（cpu_note_access が構造体 1 個のコピーで記録する）。
     * fc・rmw・alternate は MOVES の SFC/DFC アクセスを含む。has_origin/origin/origin_size は
     * 分割アクセスの一部だったときの元アクセスの先頭アドレス（060 の FSLW 用）。サイズコードは
     * cpu_noted_size_code() で導出する。 */
    mc68k_access_t fault_access;
    /* 060: 例外処理を始めた直後の 1 命令境界では割込みを受け付けない（[060UM §11.1.2]）。 */
    bool irq_defer;
    /* 060: 例外処理（vector fetch・stacking）と RTE の stack 解放の間は D-cache に allocate
     * しない （[060UM §7.7.1、§7.7.3]）。 */
    bool cache_no_allocate;
    bool group0_fault_latched;
    mc68k_040_access_error_t fault040;
    mc68k_040_pending_write_t wb2;
    bool wb2_defer_ok; /* 命令ハンドラ実行中だけ true（例外フレームの書き込みは保留しない） */
    bool g0_wb2_deferred; /* 配送中のフォルトが保留ストア由来（巻き戻さず次の PC を積む） */
    bool fpu040_post_pending;
    uint8_t fpu040_post_vector;
    /* Format-$7 CM continuation for indexed/PC-relative integer MOVEM. */
    bool movem040_continuation_valid;
    bool movem040_resume;
    uint32_t movem040_instr_pc;
    uint32_t movem040_effective_address;
    uint32_t fault_data_out;
    bool opcode_fetch_complete;
    /* 命令が PC を変えた（分岐の taken など）。T0 trace と 060 の分岐時間が使う */
    bool flow_changed;
    uint32_t restart_d[8], restart_a[8];
    uint16_t restart_sr;
    /* EA の予約済みフル拡張ワード。fault_vector (group 0 専用) と混ぜない。 */
    bool ea_invalid_pending;

    /* 命令境界のPC（グループ0例外フレームにスタックするPC、）。
     * mc68k_execute() のループ先頭（割り込みチェックより前）で cpu->pc から
     * 更新する。フォルト時点の cpu->pc は拡張ワード取得等で命令先頭より
     * 進んでいることがあるため、dispatch_group0_exception() はこちらを使う
     * （XEiJ 実装との比較で確認済み: バスエラー/アドレスエラーとも命令先頭を
     * スタックするのが正しい）。 */
    uint32_t instr_pc;

    /* MC68000 の2語命令プリフェッチ。pcは既存ハンドラ用の命令ストリームカーソル、
     * prefetch_pc はキュー先頭の物理的な命令語アドレスを保持する。 */
    uint16_t prefetch_words[2];
    uint32_t prefetch_pc;
    uint32_t prefetch_next_pc;
    uint8_t prefetch_count;
    bool prefetch_primed;
    /* 先読みで起きたフォルトは保留し、その語が実際に使われた時に報告する。
     * 0=なし、1=バスエラー、2=アドレスエラー。保留中のアドレスは prefetch_next_pc。 */
    uint8_t prefetch_fault;
    /* CCR 即値命令など、分岐でなくてもキューを破棄して再充填する命令が立てる（UM Table 8-12）。
     */
    bool prefetch_flush;
    /* Group 0／Group 1 例外の処理中に起きた bus error／address error の SSW で I/N=1
     * （命令処理中でない）にする。Group 2 例外と通常の命令では 0（UM §6.3.9.1）。 */
    bool fault_not_instruction;
    /* CLR・Scc・MOVE from SR が立てる。最初のメモリ書込みの前に同じアドレスを read する。 */
    bool ea_read_before_write;

    /* 命令境界のIR（グループ0例外フレームのIRフィールド、SSP+6）。
     * mc68k_execute() でフェッチ成功時のみ更新する（フェッチ自体がフォルトした
     * 場合は実機同様に前命令のIRを残す）。X68000 ROM ハンドラ（IPLROMXV
     * $FF05E4）はこのフィールドと一致するワードをスタック済みPCから最大5ワード
     * 遡って探索し、命令先頭を復元する仕組みのため、0固定だと誤補正が起きる。 */
    uint16_t instr_ir;

    /* バスエラー通知（mc68k_signal_bus_error() がセットする）。
     * read/write系はで戻り値方式に移行したため、現在は
     * iack コールバック専用（スプリアス割り込み判定、accept_irq()参照）。 */
    bool bus_error_pending;

    /* TRAP ハンドラ（NULL なら通常の例外ベクタ処理） */
    mc68k_trap_fn_t trap_fn;
    void *trap_ctx;

    /* PC トレース・リングバッファ（白帯/暴走検出時に実行パスをダンプ）。
     * pc_ring_on が真のときだけ記録する（常時記録は性能低下するため既定オフ）。 */
    mc68k_pc_ring_entry_t pc_ring[PC_RING_SIZE];
    int pc_ring_pos;
    int pc_ring_count; /* 有効件数（PC_RING_SIZE で頭打ち） */
    int pc_ring_on;

    /* 直近に採番した命令試行の seq。IRQ 受付では進めず、
     * リング OFF 中も進め、reset でも戻さない（debug.h のリング共通契約）。 */
    uint64_t insn_seq;

    /* レジスタ履歴リングバッファ（暴走直前の全レジスタ遷移を解析）。
     * reg_ring_on が真のときだけ記録する（常時記録は性能低下するため既定オフ）。 */
    mc68k_reg_ring_entry_t reg_ring[REG_RING_SIZE];
    int reg_ring_pos;
    int reg_ring_count; /* 有効件数（REG_RING_SIZE で頭打ち） */
    int reg_ring_on;

    /* BADMOVEA診断ログ（ops_move.c）の記録可否。既定オフ。 */
    int badmovea_log_on;

    /* X68000固有の24-bit BADADDR診断ログ。既定オフ。 */
    int badaddr_log_on;

    /* PCTRACE1024: 1024命令ごとにPCをstderrへ出力（長時間実行での滞留箇所検出用）。
     * reg_ring等のリングバッファと異なり全区間を間引いて流し続ける。既定オフ。 */
    int pctrace1024_on;
    uint64_t pctrace1024_count;

    /* トレース例外抑止フラグ。cpu_dispatch_exception() が Group1 例外
     * （ILLEGAL/特権違反/トレース/Line-A/Line-F）で true をセットする。
     * mc68k_execute() が1命令ごとに false へリセットしてから命令を実行し、
     * 命令実行後のトレース判定で参照する（生存期間は1命令の後処理のみ）。 */
    bool exc_suppress_trace;

    /* BADJUMP診断ログ（m68000.c）の記録可否。既定オフ。
     * badjump_dumped は初回ダンプ済みフラグで、mc68k_reset() でクリアされる。 */
    int badjump_log_on;
    bool badjump_dumped;

    /* BADORI診断ログ（m68000.c）の記録可否。既定オフ。opcode 0x0000
     * (ORI.B #0,D0) はゼロ埋め領域への暴走で頻出するパターンで、
     * mc68k_set_opcode_illegal(cpu, 0x0000) による例外化とは独立に
     * 「実行結果は変えずログだけ出す」観測用途として提供する。
     * badori_dumped は初回ダンプ済みフラグで、mc68k_reset() でクリアされる。 */
    int badori_log_on;
    bool badori_dumped;

    /* デバッグ出力コールバック（mc68k_set_log_handler()）。
     * NULL ならstderr直書き（既定動作）。BADMOVEA/BADADDR/BADJUMP/PCTRACE1024/
     * 強制ILLEGAL警告等の診断メッセージはすべて cpu_log() 経由で出力する。 */
    mc68k_log_fn_t log_fn;
    void *log_ctx;

    /* 直近に cpu_dispatch_exception()/dispatch_group0_exception() が
     * ディスパッチした例外のベクタ番号（mc68k_debug_run() の停止理由判定用、
     *  ステップ4）。0 = 未発生。 */
    uint16_t last_exception_vector;

    /* デバッグ実行の停止理由。 */
    mc68k_stop_reason_t stop_reason;

    /* 非侵襲メモリ読み出しフック。
     * デバッガのメモリダンプ用。NULL = 未登録（mc68k_create()のcallocで既定NULL）。 */
    mc68k_peek_byte_fn_t peek_byte;
    mc68k_peek_word_fn_t peek_word;
    mc68k_peek_long_fn_t peek_long;
    void *peek_ctx;

    /* アドレス比較型ブレークポイント。in_use=false の
     * スロットが空き。pc_ring/reg_ring と同じ流儀の固定長配列（malloc不要）。 */
    mc68k_breakpoint_t breakpoints[MC68K_MAX_BREAKPOINTS];

    /* mc68k_debug_run() の例外ブレークマスク。
     * ビットnがベクタ番号n。既定0（calloc）＝どの例外でも停止しない。 */
    uint64_t exc_break_mask;

    /* ウォッチポイント。in_use=false のスロットが空き。 */
    mc68k_watchpoint_t watchpoints[MC68K_MAX_WATCHPOINTS];
    /* 直近の mc68k_watchpoint_check() ヒット情報（エッジトリガの副作用を
     * ホストが後から読めるようにするためのスナップショット）。
     * wp_hit_id = -1 は「直近の呼び出しでヒットなし」。 */
    int wp_hit_id;
    uint32_t wp_hit_old_value;
    uint32_t wp_hit_new_value;

    /* コールスタックリング。常時記録（トグルなし。
     * BSR/JSR/RTSのみでpc_ring本体より低頻度なため性能への影響は無視できる）。 */
    mc68k_call_ring_entry_t call_ring[CALL_RING_SIZE];
    int call_ring_pos;
    int call_ring_count; /* 有効件数（CALL_RING_SIZE で頭打ち） */

    /* グループ0フォルト再発検出（68010+ のフォーマット $8 近似 RTE の安全弁）。
     * 68000 モデルでは使用しない。 */
    bool g0_rte_armed;      /* 直前に $8 フレームの RTE を実行した */
    uint32_t g0_rte_pc;     /* その RTE がスタックから取り出した復帰先 PC */
    uint32_t g0_last_addr;  /* 直近グループ0フォルトのアクセスアドレス */
    uint32_t g0_last_pc;    /* 直近グループ0フォルトの命令先頭 PC */
    uint8_t g0_last_vector; /* 直近グループ0フォルトのベクタ（2=バスエラー/3=アドレスエラー） */
    uint8_t g0_refault_count;

    /* fault ring。dispatch_group0_exception() が1件ずつ記録する。既定オフ。 */
    mc68k_fault_ring_entry_t fault_ring[FAULT_RING_SIZE];
    int fault_ring_pos;
    int fault_ring_count;
    int fault_ring_on;

    /* 最初の group0 fault が PMMU の変換失敗だったときの変換結果。
     * group0_fault_latched と同じ「最初の fault だけ」の規則で保持し、後続のフレーム書込み・
     * ベクタ読出しでの変換失敗では置き換えない。解除は dispatch_group0_exception() の全出口と
     * mc68k_reset()。 */
    bool g0_mmu_latched;
    mc68k_translate_result_t g0_mmu_result;

    /* 保留中の group0 fault が accept_irq() のフレーム構築中に起きた。
     * 解除は g0_mmu_latched と同じ。 */
    bool g0_origin_irq;

    /* 060: 配送中の address error は、分岐命令が完了した後に分岐先で検出したもの。
     * 命令開始時へ巻き戻さず、現在の SR・レジスタを積む。 */
    bool g0_no_rewind;

    /* Pending/bound origins for a Group-0 fault while constructing a normal
     * exception or interrupt frame (mc68000#67). */
    mc68k_exception_frame_origin_t *exception_frame_origins;

    /* 内蔵 FPU モデルを担当する CIR ポートが、リストア CIR の書込みで得た読み返し値を
     * 保持する。リストア CIR の読出しはこれを返す。 */
    uint16_t cp_restore_readback;
    /* FSAVE の body。モデルは body をメモリ順（+4 から昇順）で返すが、実機の 68881/68882
     * はオペランド CIR へ その逆順で出し、MPU が最初に読んだ long を最高アドレスへ書く（[881UM
     * §6.4.2]、[UM20 Figure 7-14]）。 ポートはセーブ CIR の読出しで body
     * を全部取り出し、オペランド CIR の読出しで逆順に返す。 */
    uint32_t cp_save_body[CP_SAVE_BODY_MAX_WORDS];
    unsigned cp_save_left;
};

/* cyc_get()/CYC() は cycles.h ではなくここに置く（、mc68k_state の
 * 完全定義に依存するため）。assert を唯一の安全機構にはしない
 * （NDEBUG補完は cyc_resolve() 側の無条件チェックと T1-4 が担う。§5.4）。 */
static inline int cyc_get(const mc68k_state_t *cpu, cyc_class_t cls) {
    assert(cls < CYC_CLASS_COUNT);
    assert(cpu->cyc[cls] != CYC_ABSENT); /* op_table が登録しない命令のはず */
    assert(cpu->cyc[cls] > 0);           /* 0 は cyc_resolve() 未実行（calloc 直後）を示す。
                                          * DBcc の3クラスに解決値0が正当になることはないので
                                          * この検査は誤検出しない。未解決のまま参照すると
                                          * mc68k_execute() の while (consumed < cycles) が
                                          * 0クロック消費で無限ループするため、ここで早期に落とす。 */
    /* 68040 の cyc[] は全クラスが明示値（CYC_INHERIT なし）なので、参照は表値の使用にあたる。
     * state は呼出し側が所有する非 const オブジェクトなので const を外して数える。 */
    if (cpu->model == MC68K_MODEL_68040)
        ((mc68k_state_t *)cpu)->cyc040_tagged++;
    return (int)cpu->cyc[cls];
}
#define CYC(cpu, cls) cyc_get((cpu), (cls))

static inline bool model_has_030_access_control(mc68k_model_t model) {
    return model == MC68K_MODEL_68EC030 || model == MC68K_MODEL_68030;
}

static inline bool model_has_68020_isa(mc68k_model_t model) {
    return model == MC68K_MODEL_68020 || model_has_030_access_control(model) ||
           model == MC68K_MODEL_68040 || model == MC68K_MODEL_68060;
}

/* 世代順ではなく、各モデルが持つ命令・アドレシング基盤を明示する。
 * model_has_68020_isa() は共通デコーダを登録するための判定であり、060 が
 * 68020 の全命令をネイティブ実行できることを表さない。 */
static inline bool model_has_68010_isa(mc68k_model_t model) {
    return model == MC68K_MODEL_68010 || model == MC68K_MODEL_68020 ||
           model == MC68K_MODEL_68EC030 || model == MC68K_MODEL_68030 ||
           model == MC68K_MODEL_68040 || model == MC68K_MODEL_68060;
}

static inline bool cpu_has_68010_isa(const mc68k_state_t *cpu) {
    return model_has_68010_isa(cpu->model);
}

static inline bool cpu_has_68020_isa(const mc68k_state_t *cpu) {
    return model_has_68020_isa(cpu->model);
}

/* ADD/ADDA/SUB/SUBA/AND/OR/EOR の基本クロック数（EA 時間を除く）。[UM00 Table 8-4] */
enum {
    ADD_EA_DN_BASE_CYCLES = 4,     /* ADD.B/W <ea>,Dn */
    ADD_L_EA_BASE_CYCLES = 6,      /* ADD.L <ea>,Dn / ADDA.L <ea>,An（直接・即値以外） */
    ADDA_W_BASE_CYCLES = 8,        /* ADDA.W <ea>,An */
    ADD_DN_MEM_BASE_CYCLES = 8,    /* ADD.B/W Dn,<M> */
    ADD_DN_MEM_L_BASE_CYCLES = 12, /* ADD.L Dn,<M> */
};

/* EA が Dn/An 直接または即値か。Long の基本時間が増える条件（UM Table 8-4 の **）。 */
static inline bool ea_is_direct_or_imm(int ea_mode, int ea_reg) {
    return ea_mode <= 1 || (ea_mode == 7 && ea_reg == 4);
}

static inline bool cpu_is_68000(const mc68k_state_t *cpu) {
    return cpu->model == MC68K_MODEL_68000;
}

static inline bool cpu_is_68010(const mc68k_state_t *cpu) {
    return cpu->model == MC68K_MODEL_68010;
}

static inline bool cpu_is_68020(const mc68k_state_t *cpu) {
    return cpu->model == MC68K_MODEL_68020;
}

static inline bool cpu_is_68ec030(const mc68k_state_t *cpu) {
    return cpu->model == MC68K_MODEL_68EC030;
}

static inline bool cpu_is_68030(const mc68k_state_t *cpu) {
    return cpu->model == MC68K_MODEL_68030;
}

/* 68030 and 68EC030 share the integer timing profile. */
static inline bool cpu_is_68030_family(const mc68k_state_t *cpu) {
    return cpu->model == MC68K_MODEL_68EC030 || cpu->model == MC68K_MODEL_68030;
}

static inline bool cpu_is_68040(const mc68k_state_t *cpu) {
    return cpu->model == MC68K_MODEL_68040;
}

static inline bool cpu_is_68060(const mc68k_state_t *cpu) {
    return cpu->model == MC68K_MODEL_68060;
}

/* SR.M と MSP（マスタスタック）を持つ機種。060 は M ビットを持たない。 */
static inline bool cpu_has_master_stack(const mc68k_state_t *cpu) {
    return !cpu_is_68060(cpu) && cpu_has_68020_isa(cpu);
}

/* SR.T0（change-of-flow trace）を持つ機種。060 は T0 の trace を行わない。 */
static inline bool cpu_has_t0_trace(const mc68k_state_t *cpu) {
    return !cpu_is_68060(cpu) && cpu_has_68020_isa(cpu);
}

/* CPU 空間のコプロセッサ命令（line-F の cpid 0）を、ユーザモードで特権違反にする機種。
 * 060 は line-F を FPU の F-line として扱う。 */
static inline bool cpu_has_cpid0_privilege_check(const mc68k_state_t *cpu) {
    return !cpu_is_68060(cpu) && cpu_has_68020_isa(cpu);
}

/* 060 は 64bit MUL/DIV・MOVEP・CHK2/CMP2・CAS2・非整列 CAS を vector 61 に渡す（設計書 §4）。
 */
static inline bool cpu_has_68060_unimpl_int(const mc68k_state_t *cpu) {
    return cpu_is_68060(cpu);
}

/* timing_060.c: 060 の命令時間（[060UM §10]）と例外処理時間（Table 10-26）。 */
int cyc060_instruction(mc68k_state_t *cpu, uint16_t opcode, uint32_t instr_pc);
int cyc060_exception(uint16_t vector, int ea_extra);
int cyc060_interrupt(void);

/* 68040 の timing を 040UM の表値から返す出口を示す。値はそのまま返す。 */
static inline int cpu_040_tag(mc68k_state_t *cpu, int cycles) {
    cpu->cyc040_tagged++;
    return cycles;
}

/* ops_arith_ext.c: CHK <ea>,Dn の範囲内（040UM Table 10-6）。表に無い形は -1。 */
int chk40_in_bounds_cycles(const ea_desc_t *ea);

static inline bool cpu_has_68040_cache(const mc68k_state_t *cpu) {
    return cpu_is_68040(cpu);
}

/* 040/060 の I/D cache 経路（060 は 128 set。方針は 060 の述語で分ける）。 */
static inline bool cpu_has_onchip_cache(const mc68k_state_t *cpu) {
    return cpu_is_68040(cpu) || cpu_is_68060(cpu);
}

/* 060 の内蔵 FPU 構成（LC/EC060 と汎用 CPU 構成の NONE は偽。設計書 §8.2）。 */
static inline bool cpu_has_060_fpu(const mc68k_state_t *cpu) {
    return cpu_is_68060(cpu) && cpu->fpu_config == MC68K_FPU_68060;
}

static inline bool cpu_has_integrated_fpu(const mc68k_state_t *cpu) {
    return cpu_is_68040(cpu) && cpu->fpu_config == MC68K_FPU_68040;
}

/* デバッグ出力の一元窓口。ログハンドラ未登録時はstderrへフォールバック
 * （debugger.c 実装）。BADMOVEA/BADJUMP/PCTRACE1024/強制ILLEGAL警告から使う。 */
void cpu_log(mc68k_state_t *cpu, const char *fmt, ...);

/* ウォッチポイントの前回値を全件再取得する（debugger.c 実装、
 *  ステップ7）。mc68k_reset() から呼ばれる。peek未登録時は
 * 何もしない。 */
void cpu_rearm_watchpoints(mc68k_state_t *cpu);
bool cpu_snapshot_ranges_valid(void);
void cpu_snapshot_test_fail_alloc_after(int count);

/* decode.c が実装する。mc68k_create_model() で一度だけ呼ばれる */
void build_op_table(op_handler_t *table, mc68k_model_t model);

/* Bcc/Scc/DBcc で共用する条件コード評価（ops_branch.c 実装） */
int eval_cond(uint16_t sr, int cond);

/* AND/OR/EOR/NOT/TST/MOVEQ/SWAP/EXT で共用する CCR 更新（ops_logic.c 実装） */
void ccr_logic(mc68k_state_t *cpu, uint32_t result, int size);

/*
 * シンプルフレーム例外（Group1: ILLEGAL/特権違反/トレース/Line-A/Line-F、
 * Group2: TRAP/TRAPV/CHK/ゼロ除算）を一括ディスパッチする（m68000.c 実装）。
 *   pushed_pc     : スタックに積む PC（呼び出し側が決める。命令先頭=pc-2 か 次命令=pc）
 *   vector        : 例外ベクタ番号
 *   nominal_cycles: 正常時に返す公称サイクル数
 * 戻り値: 消費サイクル。フレーム構築中に二重フォルトが起きた場合は
 *         exec_state=CPU_HALTED をセットし部分サイクルを返す。
 * グループ0（アドレスエラー/バスエラー）例外はフレーム形状が異なるため対象外。 */
int cpu_dispatch_exception(mc68k_state_t *cpu, uint32_t pushed_pc, uint16_t vector,
                           int nominal_cycles);

/* コプロセッサインタフェースの例外フレーム。
 *   PRE : format 0（4 word）。PC = 命令先頭。
 *   POST: format 2（6 word）。PC = scan_pc、4番目の long = 命令先頭。
 *   MID : format 9（10 word）。PC =
 * scan_pc。命令先頭・オペレーションワード・実効アドレスを持ち、 RTE
 * がこれらから対話を再開する（coproc_resume()）。 戻り値・二重フォルトの扱いは
 * cpu_dispatch_exception() と同じ。 */
typedef enum { CP_EXC_PRE, CP_EXC_POST, CP_EXC_MID } cp_exc_kind_t;
int cpu_dispatch_coproc_exception(mc68k_state_t *cpu, cp_exc_kind_t kind, uint16_t vector,
                                  uint32_t scan_pc, uint16_t opword, uint32_t ea,
                                  int nominal_cycles);

/* 68040 内蔵 FPU の例外フレーム。
 *   UNIMPL     : format $2（6 word）。PC = 次命令、+8 = EA。
 *   POST       : format $3（6 word）。PC = 次命令、+8 = EA（post-instruction 例外）。
 *   UNIMPL_NOFPU: format $4（8 word）。+8 = EA、+12 = 命令先頭（MC68EC040/LC040 専用）。
 * 戻り値・二重フォルトの扱いは cpu_dispatch_exception() と同じ。 */
enum { FPU040_FRAME_UNIMPL = 2, FPU040_FRAME_POST = 3, FPU040_FRAME_UNIMPL_NOFPU = 4 };
int cpu_dispatch_fpu040_exception(mc68k_state_t *cpu, uint8_t format, uint16_t vector,
                                  uint32_t pushed_pc, uint32_t ea, uint32_t faulted_pc,
                                  int nominal_cycles);

/* ops_fpu.c: 68040 の cpID 1 F-line 命令。
 * fpu040_classify(): 命令語（と汎用/条件命令の拡張語）から、不正 F-line / 040 未実装 /
 * 040 実装済みを分類する。拡張語を持たない型では ext を無視する。 */
typedef enum { FPU040_ILLEGAL, FPU040_UNIMPL, FPU040_IMPL } fpu040_class_t;
fpu040_class_t fpu040_classify(uint16_t opcode, uint16_t ext);
void register_68040_fpu_ops(op_handler_t *table);
/* 68060 の cpID 1 F-line（ops_fpu.c。共通コアと 040 の実行部を共有する）。 */
void register_68060_fpu_ops(op_handler_t *table);

bool cpu_exception_frame_origin_begin(mc68k_state_t *cpu, uint16_t saved_sr, uint32_t saved_pc);
void cpu_exception_frame_origin_complete(mc68k_state_t *cpu);
void cpu_exception_frame_origins_clear(mc68k_state_t *cpu);
bool cpu_exception_frame_origin_rte_commit(mc68k_state_t *cpu, uint32_t frame_base,
                                           uint16_t new_sr);

/* coproc.c: 68020/68030 のコプロセッサ命令（F-line、cpID 1..7）。 */
void register_coproc_ops(op_handler_t *table);
/* RTE が format 9 のフレームを取り出した後、中断された対話を応答 CIR の読出しから続ける。
 * 呼び出し時点で SR・PC（= scanPC）は復元済み。 */
int coproc_resume(mc68k_state_t *cpu, uint16_t opword, uint32_t instr_pc, uint32_t ea);

/* 特権違反（ベクタ8）ディスパッチ。呼び出し時点で消費済みは opcode 1ワードのみ
 * のため、命令先頭 = pc-2 を積む（m68000.c 実装）。 */
int cpu_privilege_violation(mc68k_state_t *cpu);

/* MC68000 の命令プリフェッチ。68010以降では通常の逐次fetchを使う。 */
void cpu_prefetch_reset(mc68k_state_t *cpu);
void cpu_prefetch_prime(mc68k_state_t *cpu);
void cpu_prefetch_enter_exception(mc68k_state_t *cpu, uint32_t vector_addr, bool group0,
                                  bool not_instruction);
void cpu_prefetch_speculative_read(mc68k_state_t *cpu, uint32_t addr);
void cpu_prefetch_complete_instruction(mc68k_state_t *cpu);
uint16_t cpu_prefetch_fetch_word(mc68k_state_t *cpu);

/* ------------------------------------------------------------------ */
/* EA ヘルパー（ea_helpers.c 実装）                                    */
/* ------------------------------------------------------------------ */

/*
 * オペランドサイズのビットマスクと符号ビット。
 * 複数の翻訳単位が参照するため cpu_internal.h に static inline として定義する。
 */
static inline uint32_t size_mask(int size) {
    return (size == 1) ? 0xFFU : (size == 2) ? 0xFFFFU : 0xFFFFFFFFU;
}

static inline uint32_t sign_bit(int size) {
    return (size == 1) ? 0x80U : (size == 2) ? 0x8000U : 0x80000000U;
}

/* SR を new_sr に更新し、S ビット遷移に応じて A7↔SSP/USP を自動スワップする。
 * MOVE EA,SR / ANDI,EORI #imm,SR / RTE / STOP はすべてこれ経由で SR を書く。 */
static inline uint32_t *cpu_sp_slot(mc68k_state_t *cpu, uint16_t sr) {
    if (!(sr & SR_S))
        return &cpu->usp;
    if (cpu_has_master_stack(cpu) && (sr & SR_M))
        return &cpu->msp;
    return &cpu->ssp;
}
/* MC68000 の SR に T0(bit14)・M(bit12) は存在せず、常に 0 で読める（仕様 REG-008）。 */
static inline uint16_t cpu_sr_storable(const mc68k_state_t *cpu, uint16_t sr) {
    if (cpu->model == MC68K_MODEL_68000)
        return (uint16_t)(sr & ~(SR_T0 | SR_M));
    return sr;
}
static inline void cpu_set_sr(mc68k_state_t *cpu, uint16_t new_sr) {
    new_sr = cpu_sr_storable(cpu, new_sr);
    uint32_t *from = cpu_sp_slot(cpu, cpu->sr), *to = cpu_sp_slot(cpu, new_sr);
    cpu->sr = new_sr;
    if (from != to) {
        *from = cpu->a[7];
        cpu->a[7] = *to;
    }
}

/*
 * 例外ベクタのアドレスを計算する（VBR からの相対オフセット）。
 * 68000 モデルは vbr==0 に固定される（mc68k_reset() でクリア）ため、
 * 従来の "vector * 4" と完全に同一の結果になる。
 * リセットベクタ（$000000/$000004）は VBR の影響を受けないため対象外
 * （mc68k_reset() は直接 0/4 番地を読む）。
 */
static inline uint32_t cpu_vector_addr(const mc68k_state_t *cpu, uint16_t vector) {
    return cpu->vbr + (uint32_t)vector * 4;
}

/* EA 読み書き（ea_helpers.c 実装） */
uint32_t ea_read(mc68k_state_t *cpu, int mode, int reg, int size);
void ea_write(mc68k_state_t *cpu, int mode, int reg, int size, uint32_t val);
/* 明示的なバスサイクル文脈版。MOVES は通常の EA ヘルパーに現在の SR から FC を推測させず、
 * ここで SFC/DFC を使う。 */
uint32_t ea_read_access(mc68k_state_t *cpu, int mode, int reg, int size,
                        const mc68k_access_t *access);
void ea_write_access(mc68k_state_t *cpu, int mode, int reg, int size, uint32_t val,
                     const mc68k_access_t *access);
/* 制御 EA のアドレス計算（JMP/JSR/LEA/PEA で使用） */
uint32_t ea_get_addr(mc68k_state_t *cpu, int mode, int reg);
/* RMW（Read-Modify-Write）EA: アドレスを一度だけ計算してread、writeで再利用。
 * locked は実機で不可分バスサイクルとなる TAS/CAS 専用（SSW.RM / PMMU RWM
 * 判定に影響する、）。ADD/SUB/AND/OR/EOR <ea>、NOT/NEG/NEGX、
 * BCHG/BCLR/BSET、メモリシフト、NBCD 等の通常 RMW 命令は独立した read サイクル
 * ＋write サイクルなので false を渡す。 */
/* RMW の EA 計算だけを行い、メモリには触れない（mode 2〜7、#imm 除く）。
 * (An)+/-(An) の更新と拡張ワード fetch は行う。失敗時は exec_state != CPU_NORMAL。
 * operand access より前にアドレス検査が要る命令（060 の非整列 CAS）用。 */
uint32_t ea_rmw_addr(mc68k_state_t *cpu, int mode, int reg, int size);
/* ea_rmw_addr() で得たアドレスの read 部。ea_rmw_write と対で使う。 */
uint32_t ea_rmw_load(mc68k_state_t *cpu, int size, uint32_t addr, bool locked);
uint32_t ea_rmw_read(mc68k_state_t *cpu, int mode, int reg, int size, uint32_t *addr_out,
                     bool locked);
void ea_rmw_write(mc68k_state_t *cpu, int mode, int reg, int size, uint32_t addr, uint32_t val,
                  bool locked);

/*
 * EA カテゴリ判定（ea = mode<<3|reg、0-63）。: 各 register_*_ops() の
 * テーブル登録ループで無効な EA 組み合わせを ILLEGAL のまま残すために使う
 * （ea_helpers.c 実装）。
 */
int ea_is_data_alterable(int ea);
int ea_is_memory_alterable(int ea);
int ea_is_alterable(int ea);
int ea_is_control(int ea);
int ea_is_control_alterable(int ea);
int ea_is_data(int ea);
int ea_is_data_or_an(int ea);

/*
 * 各命令グループの登録関数。
 * 命名規則: ops_<category>.c（無印=MC68000ベースライン）。
 * 将来 68020/030/040/060 拡張時は ops_<世代>_<category>.c で追加する想定
 * （例: ops_68020_bitfield.c）。
 */
void register_move_ops(op_handler_t *table);   /* ops_move.c: MOVE/MOVEA */
void register_alu_ops(op_handler_t *table);    /* ops_alu.c: ADD/SUB/CMP 系 */
void register_branch_ops(op_handler_t *table); /* ops_branch.c: Bcc/BRA/BSR/JMP/JSR/RTS */
void register_trap_ops(op_handler_t *table);   /* ops_trap.c: TRAP #n */
void register_logic_ops(
    op_handler_t *table); /* ops_logic.c: AND/OR/EOR/CLR/NEG/NOT/TST/ビット操作/MOVE CCR */
void register_transfer_ops(
    op_handler_t
        *table); /* ops_transfer.c: MOVEQ/MOVE SR/LEA/PEA/SWAP/EXT/MOVEM/LINK/UNLK/EXG/MOVEP */
void register_arith_ext_ops(
    op_handler_t
        *table); /* ops_arith_ext.c: MULU/MULS/DIVU/DIVS/ADDX/SUBX/CMPM/CHK/ABCD/SBCD/NBCD */
void register_shift_ops(op_handler_t *table); /* ops_shift.c: シフト/ローテート/Scc/DBcc */
void register_system_ops(
    op_handler_t *table); /* ops_system.c: NOP/RTE/RTR/RESET/STOP/TRAPV/MOVE USP/TAS */
void register_68010_ops(op_handler_t *table); /* ops_system_ext.c: MOVE from CCR/MOVE from
                                                 SR特権化（68010+のみ） */
void register_68020_system_ops(op_handler_t *table);
void register_68020_arith_ops(op_handler_t *table);
void register_68020_bitfield_ops(op_handler_t *table);
void register_68030_mmu_ops(op_handler_t *table);
void register_68040_mmu_ops(op_handler_t *table);
void register_68040_cache_ops(op_handler_t *table);
void register_68060_cache_ops(op_handler_t *table);
void register_68060_ops(op_handler_t *table);
void register_68060_mmu_ops(op_handler_t *table);
void cpu_040_cache_reset(mc68k_state_t *cpu);
bool cpu_040_cache_read(mc68k_state_t *cpu, uint32_t logical, uint32_t physical,
                        const mc68k_access_t *access, uint8_t cache_mode, uint32_t *out);
bool cpu_040_cache_write(mc68k_state_t *cpu, uint32_t logical, uint32_t physical,
                         uint32_t value, const mc68k_access_t *access, uint8_t cache_mode);
/* デバッガ読出し用の副作用なし D-cache byte 検索（割当て、充填、バスアクセスなし）。 */
bool cpu_040_cache_peek(mc68k_state_t *cpu, uint32_t physical, uint8_t *out);
/* テーブル検索アクセス（design_issue100）: 68040 は D-cache ヒットを使い、ミスでは決して
 * 割り当てず例外も起こさない。set_bits は最初にラインを書き戻して無効化するロック
 * read-modify-write。 68060 は D-cache を完全に無視する。*failed
 * には失敗したバスアクセスのアドレスを入れる。 */
bool cpu_040_table_read(mc68k_state_t *cpu, uint32_t physical, uint32_t *out);
bool cpu_040_table_write(mc68k_state_t *cpu, uint32_t physical, uint32_t value);
bool cpu_040_table_set_bits(mc68k_state_t *cpu, uint32_t physical, uint32_t bits, uint32_t *out,
                            uint32_t *failed);
bool cpu_040_move16(mc68k_state_t *cpu, uint32_t source, uint32_t destination,
                    bool source_postincrement, bool destination_postincrement);
/* 純粋な PMMU 検索。CPU 例外は決して配送しない。 */
mc68k_translate_result_t cpu_mmu_translate_result(mc68k_state_t *cpu, uint32_t logical,
                                                  const mc68k_access_t *access,
                                                  mc68k_translate_mode_t mode);
/* PTEST の探索。LEVEL=0 は TT と ATC だけを、LEVEL=1-7 はテーブルだけを検索する。
 * どちらも U/M・ATC のエントリ・置換履歴を変更しない。戻り値は MMUSR、
 * last_descriptor には最後に読んだ記述子の物理アドレス（PTEST の A/An）を返す。 */
uint16_t cpu_mmu_ptest(mc68k_state_t *cpu, uint32_t logical, const mc68k_access_t *access,
                       unsigned level, uint32_t *last_descriptor);
/* EC030 の AC0/AC1 による cache-inhibit(CI, bit10) 判定。
 * cpu_mmu_translate_result() は MC68030 専用で EC030 では常に恒等変換を
 * 返すため、EC030 の CI はこの専用関数で取得する（ops_68030_mmu.c 実装）。 */
bool cpu_mmu_ec030_cache_inhibit(mc68k_state_t *cpu, uint32_t logical,
                                 const mc68k_access_t *access);
mc68k_translate_result_t cpu_mmu040_translate_result(mc68k_state_t *cpu, uint32_t logical,
                                                     const mc68k_access_t *access,
                                                     mc68k_translate_mode_t mode);

/* 060 専用の翻訳器（ops_68060_mmu.c）。040/030 の翻訳器には落とさない。 */
mc68k_translate_result_t cpu_mmu060_translate_result(mc68k_state_t *cpu, uint32_t logical,
                                                     const mc68k_access_t *access,
                                                     mc68k_translate_mode_t mode);
/* TC=0 かつ TTR 全無効なら、翻訳は恒等で既定属性も既定値（WT
 * cacheable、保護なし）になる。ただし alternate な空間の MOVES（SFC/DFC=0・3・4・7）は
 * noncachable（呼び出し側で access->alternate を見る）。 */
void cpu_mmu060_ifast_prime(mc68k_state_t *cpu, uint32_t logical, const mc68k_access_t *a);

/* 命令フェッチ高速経路の前提（TC・ITT0/1・命令側 ATC の entry）を変える操作の後に呼ぶ。次の
 * フェッチで、TC・ITT と entry を再検証させる。 */
static inline void cpu_ifetch_fast_invalidate(mc68k_state_t *cpu) {
    cpu->ifast_gen++;
}

/* 命令フェッチの ATC 変換を省く高速経路。前回と同じ S・ページ・TC・ITT0/1 で、記憶した slot の
 * entry が今も有効なら、cpu_mmu060_translate_result() の ATC ヒットと同じ結果を返す。
 * TTR の範囲は 16MB 単位でページより粗いので、同じページ・同じ S なら TTR 判定は変わらない。 */
static inline bool cpu_mmu060_ifast_try(mc68k_state_t *cpu, uint32_t logical,
                                        const mc68k_access_t *a, uint32_t *physical,
                                        bool *cache_inhibit) {
    const mc68k_atc060_ifast_t *f = &cpu->atc060_ifast;
    if (!f->valid || ((logical ^ f->logical_page) >> f->page_shift) != 0 ||
        (uint8_t)(a->fc & 4u) != f->fc_s || cpu->mmu060_tc != f->tc ||
        cpu->mmu060_itt0 != f->itt0 || cpu->mmu060_itt1 != f->itt1)
        return false;
    mc68k_atc_entry_t *e = &cpu->atc060_i[f->slot];
    if (!e->valid || (e->fc & 4u) != f->fc_s || e->page_shift != f->page_shift ||
        e->logical_page != f->logical_page)
        return false;
    e->history = ++cpu->atc060_clock; /* LRU の挙動を通常経路と同一に保つ */
    *physical = e->physical_page | (logical & ((UINT32_C(1) << f->page_shift) - 1u));
    *cache_inhibit = e->cache_inhibit;
    cpu->access_cache_mode = e->cache_mode;
    cpu->access_tt_hit = false;
    return true;
}

static inline bool cpu_mmu060_is_identity(const mc68k_state_t *cpu) {
    return cpu->mmu060_tc == 0 &&
           !((cpu->mmu060_itt0 | cpu->mmu060_itt1 | cpu->mmu060_dtt0 | cpu->mmu060_dtt1) &
             UINT32_C(0x00008000));
}

static inline bool model_has_pmmu(mc68k_model_t model) {
    return model == MC68K_MODEL_68030;
}
static inline bool model_has_030_cache(mc68k_model_t model) {
    return model_has_030_access_control(model);
}

/* MOVE SR,EA ハンドラ本体（ops_transfer.c 実装）。68000 では非特権のまま登録され、
 * 68010 では ops_system_ext.c の特権チェック付きラッパが上書き登録する。 */
int op_move_from_sr(mc68k_state_t *cpu, uint16_t opcode);
int op_tst(mc68k_state_t *cpu, uint16_t opcode);
int op_cmpi(mc68k_state_t *cpu, uint16_t opcode);

/* ------------------------------------------------------------------ */
/* CPU 経由のバスアクセスヘルパー（アライメントチェック付き）            */
/* ------------------------------------------------------------------ */

/*
 * MC68000 はアドレスバスを駆動する前に CPU 自身がアライメントを確認し、
 * 奇数アドレスへの word/long アクセスで Address Error 例外を発生させる。
 * これらの inline 関数がその役割を担う。
 * バイトアクセスはアライメント不問（どのアドレスでも OK）。
 */

/*
 * X68000 は 24bit アドレスバス。アドレスの上位 8bit が $00（RAM/IO）でも
 * $FF（ROM/IO ミラー・-1）でもない（=$01〜$FE）アクセスは、上位 8bit が
 * 無視される結果ゴミアドレスを叩く暴走の兆候。これはMC68000一般の仕様ではなく
 * X68000固有のヒューリスティックであり、将来CPUモデル/ホスト別の診断方針を
 * 導入する際はこの関数内の判定を置換・拡張する。$FFFFFFFF 等は上位 $FF なので除外。
 */
static inline void cpu_check_badaddr(mc68k_state_t *cpu, uint32_t addr, const char *kind) {
    if (!cpu->badaddr_log_on)
        return;
    uint32_t hi = addr >> 24;
    if (hi != 0x00u && hi != 0xFFu)
        cpu_log(cpu, "BADADDR(%s) addr=%08X pc=%06X\n", kind, addr, cpu->pc);
}

/*
 * バスコールバックが false（該当デバイスなし）を返したら
 * バスエラー例外（ベクタ2）としてディスパッチする。
 */
static inline void cpu_signal_bus_error(mc68k_state_t *cpu, uint32_t addr, bool is_read) {
    if (!cpu->group0_fault_latched) {
        cpu->fault_vector = 2;
        cpu->addr_err_addr = addr;
        cpu->addr_err_read = is_read;
        cpu->group0_fault_latched = true;
    }
    cpu->exec_state = CPU_EXCEPTION;
}

static inline bool cpu_needs_alignment(const mc68k_state_t *cpu) {
    return !cpu_has_68020_isa(cpu);
}

static inline void cpu_signal_address_error(mc68k_state_t *cpu, uint32_t addr, bool is_read) {
    if (!cpu->group0_fault_latched) {
        cpu->fault_vector = 3;
        cpu->addr_err_addr = addr;
        cpu->addr_err_read = is_read;
        cpu->group0_fault_latched = true;
    }
    cpu->exec_state = CPU_EXCEPTION;
}

/* CPU 起点の全バスサイクルはこれらの文脈のいずれかを通る。FC を fault_access.fc
 * から導出してはならない。
 * 同フィールドは直前のフォルトを表し、例外フレーム構築のためだけに保持される。 */
static inline mc68k_access_t cpu_data_access(const mc68k_state_t *cpu, bool read, uint8_t size,
                                             bool rmw) {
    return (mc68k_access_t){
        .fc = (cpu->sr & SR_S) ? 5 : 1, .read = read, .rmw = rmw, .size = size};
}

static inline mc68k_access_t cpu_program_access(const mc68k_state_t *cpu, uint8_t size) {
    return (mc68k_access_t){
        .fc = (cpu->sr & SR_S) ? 6 : 2, .read = true, .size = size, .instruction = true};
}

static inline mc68k_access_t cpu_supervisor_data_access(bool read, uint8_t size, bool rmw) {
    return (mc68k_access_t){.fc = 5, .read = read, .rmw = rmw, .size = size};
}

static inline uint8_t cpu_fault_size_code(uint8_t size) {
    return size == 4 ? 0 : size == 1 ? 1 : 2;
}

/* 分割した下位アクセスに、元のアクセスの先頭アドレスと大きさを持たせる。分割は 1 段だけ
 * （奇数 long の中の word は偶数アドレス、2 mod 4 の long の word は整列済み）。 */
static inline void cpu_access_mark_origin(mc68k_access_t *sub, const mc68k_access_t *parent,
                                          uint32_t addr) {
    sub->has_origin = true;
    sub->origin = addr;
    sub->origin_size = parent->size;
}

/* group-0 フォルトがラッチされた後（cpu_signal_bus_error/address_error）でも、複数サイクル命令
 * （MOVEM/MOVEP、bitfield・CAS2 ループ、上記の奇数アドレス byte 分割）は、呼出し側が
 * exec_state != CPU_NORMAL
 * を検出する前に、同じ反復で後続バスサイクルを発行しうる。このガードがないと cpu_note_access()
 * は後続サイクルの属性で fault_access/fault_access_kind を
 * 上書きし続け、後で構築するフレームが誤ったサイクルを表す（例:
 * 同じ反復で同じ不正アドレスへの書込みが
 * 読出しフォルトの後に走ると、読出しフォルトを誤って書込みにする）。 */
MC68K_ALWAYS_INLINE void cpu_note_access(mc68k_state_t *cpu, const mc68k_access_t *access) {
    if (cpu->group0_fault_latched)
        return;
    cpu->fault_access = *access;
    if (!access->instruction)
        cpu->fault_access_kind = access->read ? FAULT_DATA_READ : FAULT_DATA_WRITE;
}

/* 直近に記録したバスサイクルの大きさを、フレームのサイズコードで返す。 */
static inline uint8_t cpu_noted_size_code(const mc68k_state_t *cpu) {
    return cpu_fault_size_code(cpu->fault_access.size);
}

/* 060: 命令 prefetch の奇数アドレス（分岐先、RTS/RTR/RTD/RTE の PC）。fetch
 * 時と同じ属性で記録する。 */
static inline void cpu_signal_prefetch_address_error(mc68k_state_t *cpu, uint32_t target) {
    const mc68k_access_t access = cpu_program_access(cpu, 2);
    if (!cpu->group0_fault_latched)
        cpu->fault_access_kind = FAULT_OPCODE;
    cpu_note_access(cpu, &access);
    cpu_signal_address_error(cpu, target, true);
}

/* MC68040UM §8.4.6.2: アドレス変換失敗は ATC フォルトである。キャッシュ push を除く
 * 全アクセスでフォルトアドレスは論理アドレスであり、この MMU 経路はキャッシュ push
 * を生成しない。 g0_mmu_result は失敗が ATC ヒットかテーブル walk かを保持するが、format $7
 * にその区別の欄はない。 */
static inline void cpu_prepare_040_mmu_access_error(mc68k_state_t *cpu, uint32_t logical,
                                                    const mc68k_access_t *access) {
    cpu->fault040 = (mc68k_040_access_error_t){
        .valid = true,
        .ssw =
            (uint16_t)(0x0400u | (access->rmw ? 0x0200u : 0u) | (access->read ? 0x0100u : 0u) |
                       ((uint16_t)cpu_fault_size_code(access->size) << 5) |
                       (access->alternate ? TT_ALTERNATE : 0u) | (access->fc & 7u)),
        .fault_address = logical,
    };
}

/* 保留ストア（WB2）を持つ命令が 2 回目の書き込みなどで保留を維持できなくなったとき、
 * 保留を捨てて最初のフォルトを通常の再実行モデルの bus error にする。 */
static inline void cpu_040_wb2_escalate(mc68k_state_t *cpu) {
    mc68k_040_pending_write_t pending = cpu->wb2;
    cpu->wb2.valid = false;
    cpu_note_access(cpu, &pending.access);
    if (!cpu->group0_fault_latched) {
        cpu->g0_mmu_latched = true;
        cpu->g0_mmu_result = pending.mmu;
        cpu_prepare_040_mmu_access_error(cpu, pending.logical, &pending.access);
    }
    cpu_signal_bus_error(cpu, pending.logical, false);
}

/* 命令境界で保留ストアをフレームの WB2 欄に載せて bus error を起こす。 */
static inline void cpu_040_wb2_begin_delivery(mc68k_state_t *cpu) {
    mc68k_040_pending_write_t pending = cpu->wb2;
    cpu->wb2.valid = false;
    cpu_note_access(cpu, &pending.access);
    cpu->g0_mmu_latched = true;
    cpu->g0_mmu_result = pending.mmu;
    cpu_prepare_040_mmu_access_error(cpu, pending.logical, &pending.access);
    cpu->fault040.wb_status[1] =
        (uint8_t)(WB_STATUS_VALID | ((uint8_t)cpu_fault_size_code(pending.access.size) << 5) |
                  (pending.access.alternate ? TT_ALTERNATE : 0u) | (pending.access.fc & 7u));
    cpu->fault040.wb_address[1] = pending.logical;
    cpu->fault040.wb_data[1] = pending.data;
    cpu_signal_bus_error(cpu, pending.logical, false);
    cpu->g0_wb2_deferred = true;
}

/* このバスサイクルだけを変換する。特に、すでに保留中の CPU 例外でホスト callback を抑止しない。
 * group-0 のフレーム／ベクタサイクルはバスへ到達し続け、そこで二重フォルトとして診断できる。
 *
 * cache_inhibit は *physical
 * と独立して、MMU／アクセス制御検索がこのアクセスのキャッシュを拒否するかを 報告する。MC68030
 * は cpu_mmu_translate_result()（TT bit10／PTE bit10）から取得する。MC68EC030 は オンチップ
 * PMMU を持たないため、同一性変換経路で cpu_mmu_ec030_cache_inhibit()（AC0/AC1 bit10）から
 * 取得する。MC68040/MC68060 は専用変換器から取得し、060 は同一性変換時の代替空間 MOVES でも
 * true になり得る。 他の全モデルは常に false を報告する。 */
bool cpu_translate_execute_access_slow(mc68k_state_t *cpu, uint32_t logical,
                                       const mc68k_access_t *access, uint32_t *physical,
                                       bool *cache_inhibit);

bool cpu_translate_execute_access_060(mc68k_state_t *cpu, uint32_t logical,
                                      const mc68k_access_t *access, uint32_t *physical,
                                      bool *cache_inhibit);

/* 060 のデータアクセス用 ATC ヒット高速経路。cpu_mmu060_translate_result() の ATC ヒットと
 * 同じ結果を、構造体を介さずに返す。次の場合は false を返し、通常経路（WP フォルト・M 更新の
 * walk・ATC ミスを含む）に任せる。
 *   - 命令側、alternate（MOVES の SFC/DFC=0・3・4・7）、TC.E=0、DTT0/1 のどちらかが有効
 *   - hint 表の slot が今のアクセスと一致しない（valid・S・page_shift・logical_page
 * を毎回検証）
 *   - write（TAS/CAS の read 部を含む）で、entry が W=1 または M=0
 * hint 表は通常経路が全探索したときだけ更新するので、ここでは更新しない（通常経路と同じ挙動）。
 * 結果は毎回 entry から読むため、PFLUSH・M 更新の slot 上書きの後に古い値を使うことはない。 */
#define CPU_060_TC_E     UINT32_C(0x00008000)
#define CPU_060_TC_P     UINT32_C(0x00004000)
#define CPU_060_TTR_E    UINT32_C(0x00008000)
#define CPU_060_FC_SUPER 4u
#define CPU_060_PS_4K    12u
#define CPU_060_PS_8K    13u
static inline bool cpu_mmu060_dfast_try(mc68k_state_t *cpu, uint32_t logical,
                                        const mc68k_access_t *a, uint32_t *physical,
                                        bool *cache_inhibit) {
    const uint32_t tc = cpu->mmu060_tc;
    if (a->instruction || a->alternate || cpu->atc060_dfast_disable || !(tc & CPU_060_TC_E) ||
        ((cpu->mmu060_dtt0 | cpu->mmu060_dtt1) & CPU_060_TTR_E))
        return false;
    const unsigned ps = (tc & CPU_060_TC_P) ? CPU_060_PS_8K : CPU_060_PS_4K;
    const uint32_t mask = (UINT32_C(1) << ps) - 1u;
    const unsigned slot = cpu->atc060_hint[0][(logical >> ps) & (MC68K_060_ATC_HINTS - 1u)];
    if (slot >= MC68K_060_ATC_ENTRIES)
        return false;
    mc68k_atc_entry_t *e = &cpu->atc060_d[slot];
    if (!e->valid || (e->fc & CPU_060_FC_SUPER) != (a->fc & CPU_060_FC_SUPER) ||
        e->page_shift != ps || e->logical_page != (logical & ~mask))
        return false;
    if ((!a->read || a->rmw) && (e->write_protect || !e->modified))
        return false;
    cpu_note_access(cpu, a);
    e->history = ++cpu->atc060_clock; /* LRU の挙動を通常経路と同一に保つ */
    *physical = e->physical_page | (logical & mask);
    *cache_inhibit = e->cache_inhibit;
    cpu->access_cache_mode = e->cache_mode;
    cpu->access_tt_hit = false;
    return true;
}

/* 本体は外部関数。060 は cpu_translate_execute_access_060()（ops_68060_mmu.c）、それ以外は
 * cpu_translate_execute_access_slow()（m68000.c）。全モデルの本体を inline にすると大きすぎて
 * どの呼び出し元でもインライン化されないため、ここでは分岐だけを inline にする。 */
static inline bool cpu_translate_execute_access(mc68k_state_t *cpu, uint32_t logical,
                                                const mc68k_access_t *access,
                                                uint32_t *physical, bool *cache_inhibit) {
    if (cpu_is_68060(cpu)) {
        if (cpu_mmu060_dfast_try(cpu, logical, access, physical, cache_inhibit))
            return true;
        return cpu_translate_execute_access_060(cpu, logical, access, physical, cache_inhibit);
    }
    return cpu_translate_execute_access_slow(cpu, logical, access, physical, cache_inhibit);
}

/* 書き込み用の変換。68040 の ATC フォルトは、命令ハンドラ実行中の単純な書き込み（RMW・分割・
 * 例外フレームを除く）に限り保留ストア（WB2）にして命令を完了させる。戻り値 false かつ
 * exec_state == CPU_NORMAL は「保留した」を表し、呼び出し側は書き込まずに戻る。 */
static inline bool cpu_translate_write_access(mc68k_state_t *cpu, uint32_t logical,
                                              uint32_t val, const mc68k_access_t *access,
                                              uint32_t *physical, bool *cache_inhibit) {
    if (!cpu_is_68040(cpu) || !cpu->wb2_defer_ok || cpu->wb2.valid ||
        (cpu->exception_frame_origins && !cpu->exception_frame_origins->bound) ||
        cpu->group0_fault_latched || cpu->exec_state != CPU_NORMAL || access->rmw ||
        access->split || access->instruction)
        return cpu_translate_execute_access(cpu, logical, access, physical, cache_inhibit);
    cpu_note_access(cpu, access);
    mc68k_translate_result_t result =
        cpu_mmu040_translate_result(cpu, logical, access, MC68K_TRANSLATE_EXECUTE);
    if (result.ok) {
        *physical = result.physical;
        *cache_inhibit = result.cache_inhibit;
        cpu->access_cache_mode = result.cache_mode;
        return true;
    }
    cpu->wb2 = (mc68k_040_pending_write_t){
        .valid = true, .logical = logical, .data = val, .access = *access, .mmu = result};
    return false;
}

/* CACR EI/ED による抑止対象かどうか。ホストが cacheable と
 * 答え、MMU が CI を立てておらず、対応する CACR ビットが立っているアクセス
 * だけがウェイトを免除される。D write は write-through として常に外部
 * ウェイトを残す。この判定を1関数に閉じておくと、Phase 2（ライン状態を
 * 持つキャッシュ）へ移行するときここだけを差し替えればよい
 * 。 */
static inline bool cpu_cache_read_eligible(const mc68k_state_t *cpu,
                                           const mc68k_access_t *access) {
    if (!model_has_030_cache(cpu->model))
        return false;
    if (access->instruction)
        return (cpu->cacr & MC68K_CACR_EI) != 0;
    if (!access->read)
        return false; /* D write は write-through */
    /* RMW の read 側は ED の対象、write 側は上の行で外部アクセスになる。 */
    return (cpu->cacr & MC68K_CACR_ED) != 0;
}

/* バスサイクル1回のコストをホストへ問い合わせ、命令単位アキュムレータへ
 * 積む。呼び出しは「実際に cpu->bus.* を呼んだ直後」だけに
 * 置くこと。ただしコプロセッサの CIR は、内蔵 FPU
 * モデルへの アクセスも、ホストの CIR と同じ fc=7
 * のバスサイクルとして数える。 */
/* ページ表（mc68k_page_map_t）の引き。無効化中・未登録・ページをまたぐアクセスは NULL。
 * CPU 空間（FC=7）と size 1/2/4 以外（CIR の 3 バイト）は表を使わない。 */
static inline const mc68k_page_entry_t *cpu_page_entry(const mc68k_state_t *cpu,
                                                       uint32_t physical, unsigned size) {
    const mc68k_page_map_t *map = cpu->bus.page_map;
    if (!map || map->disable_mask != 0u)
        return NULL;
    if (size != 1u && size != 2u && size != 4u)
        return NULL;
    if ((physical & MC68K_PAGE_MASK) + size > MC68K_PAGE_SIZE)
        return NULL;
    const mc68k_page_entry_t *l2 = map->l1[physical >> MC68K_PAGE_L1_SHIFT];
    if (!l2)
        return NULL;
    return &l2[(physical >> MC68K_PAGE_SHIFT) & (MC68K_PAGE_L2_COUNT - 1u)];
}

/* 表から access_timing 相当の結果を求める。引けなければ false。 */
static inline bool cpu_page_timing(const mc68k_state_t *cpu, uint32_t physical,
                                   const mc68k_access_t *access, mc68k_bus_timing_t *t) {
    if (access->fc == MC68K_FC_CPU_SPACE)
        return false;
    const mc68k_page_entry_t *e = cpu_page_entry(cpu, physical, access->size);
    if (!e || !(e->flags & MC68K_PAGE_TIMING))
        return false;
    const mc68k_page_wait_t *w = &cpu->bus.page_map->wait_table[e->wait_class];
    if (access->size == 4u)
        t->wait_cycles = access->read ? w->read_long : w->write_long;
    else
        t->wait_cycles = access->read ? w->read : w->write;
    t->cacheable = (e->flags & MC68K_PAGE_CACHEABLE) != 0u;
    return true;
}

static inline void cpu_account_bus_cycle(mc68k_state_t *cpu, uint32_t physical,
                                         const mc68k_access_t *access, bool cache_inhibit) {
    if (!cpu->bus.access_timing)
        return;
    mc68k_bus_timing_t t = {0, false};
    if (!cpu_page_timing(cpu, physical, access, &t)) {
        const mc68k_bus_access_t a = {.physical = physical,
                                      .size = access->size,
                                      .fc = access->fc,
                                      .read = access->read,
                                      .instruction = access->instruction,
                                      .rmw = access->rmw};
        cpu->bus.access_timing(cpu->bus.ctx, &a, &t);
    }
    if (t.wait_cycles == 0)
        return;
    if (t.cacheable && !cache_inhibit && cpu_cache_read_eligible(cpu, access))
        return;
    cpu->bus_wait_accum += (int)t.wait_cycles;
}

/* 表から直接読めるエントリ。スーパーバイザ専用ページは SR の S ビットで判定する
 * （FC ではない。ホストの保護判定が SR を見るため）。 */
static inline const uint8_t *cpu_page_direct_read(const mc68k_state_t *cpu, uint32_t physical,
                                                  unsigned size) {
    const mc68k_page_entry_t *e = cpu_page_entry(cpu, physical, size);
    if (!e || !(e->flags & MC68K_PAGE_READ_DIRECT))
        return NULL;
    if ((e->flags & MC68K_PAGE_SUPERVISOR_ONLY) && !(cpu->sr & SR_S))
        return NULL;
    return e->host + (physical & MC68K_PAGE_MASK);
}

/* 表へ直接書けるエントリ。判定は cpu_page_direct_read と同じ（WRITE_DIRECT、S ビット）。 */
static inline uint8_t *cpu_page_direct_write(const mc68k_state_t *cpu, uint32_t physical,
                                             unsigned size) {
    const mc68k_page_entry_t *e = cpu_page_entry(cpu, physical, size);
    if (!e || !(e->flags & MC68K_PAGE_WRITE_DIRECT))
        return NULL;
    if ((e->flags & MC68K_PAGE_SUPERVISOR_ONLY) && !(cpu->sr & SR_S))
        return NULL;
    return (uint8_t *)(uintptr_t)e->host + (physical & MC68K_PAGE_MASK);
}

static inline void cpu_bus_access_begin(mc68k_state_t *cpu) {
    if (!cpu->bus_access_epoch || !cpu_is_68060(cpu))
        return;
    cpu->bus_access_cycles =
        (uint64_t)cpu->exec_cycles_at_insn_start + (uint64_t)cpu->bus_wait_accum;
    cpu->bus_access_active = true;
}

static inline void cpu_bus_access_end(mc68k_state_t *cpu) {
    cpu->bus_access_active = false;
}

static inline bool cpu_bus_read_byte(mc68k_state_t *cpu, uint32_t addr, uint8_t *out) {
    const uint8_t *direct = cpu_page_direct_read(cpu, addr, 1u);
    if (direct) {
        *out = direct[0];
        return true;
    }
    cpu_bus_access_begin(cpu);
    bool ok = cpu->bus.read_byte(cpu->bus.ctx, addr, out);
    cpu_bus_access_end(cpu);
    return ok;
}

static inline bool cpu_bus_read_word(mc68k_state_t *cpu, uint32_t addr, uint16_t *out) {
    const uint8_t *direct = cpu_page_direct_read(cpu, addr, 2u);
    if (direct) {
        *out = (uint16_t)(((uint16_t)direct[0] << 8) | direct[1]);
        return true;
    }
    cpu_bus_access_begin(cpu);
    bool ok = cpu->bus.read_word(cpu->bus.ctx, addr, out);
    cpu_bus_access_end(cpu);
    return ok;
}

static inline bool cpu_bus_read_long(mc68k_state_t *cpu, uint32_t addr, uint32_t *out) {
    const uint8_t *direct = cpu_page_direct_read(cpu, addr, 4u);
    if (direct) {
        *out = ((uint32_t)direct[0] << 24) | ((uint32_t)direct[1] << 16) |
               ((uint32_t)direct[2] << 8) | (uint32_t)direct[3];
        return true;
    }
    cpu_bus_access_begin(cpu);
    bool ok = cpu->bus.read_long(cpu->bus.ctx, addr, out);
    cpu_bus_access_end(cpu);
    return ok;
}

static inline bool cpu_bus_write_byte(mc68k_state_t *cpu, uint32_t addr, uint8_t value) {
    uint8_t *direct = cpu_page_direct_write(cpu, addr, 1u);
    if (direct) {
        direct[0] = value;
        return true;
    }
    cpu_bus_access_begin(cpu);
    bool ok = cpu->bus.write_byte(cpu->bus.ctx, addr, value);
    cpu_bus_access_end(cpu);
    return ok;
}

static inline bool cpu_bus_write_word(mc68k_state_t *cpu, uint32_t addr, uint16_t value) {
    uint8_t *direct = cpu_page_direct_write(cpu, addr, 2u);
    if (direct) {
        direct[0] = (uint8_t)(value >> 8);
        direct[1] = (uint8_t)value;
        return true;
    }
    cpu_bus_access_begin(cpu);
    bool ok = cpu->bus.write_word(cpu->bus.ctx, addr, value);
    cpu_bus_access_end(cpu);
    return ok;
}

static inline bool cpu_bus_write_long(mc68k_state_t *cpu, uint32_t addr, uint32_t value) {
    uint8_t *direct = cpu_page_direct_write(cpu, addr, 4u);
    if (direct) {
        direct[0] = (uint8_t)(value >> 24);
        direct[1] = (uint8_t)(value >> 16);
        direct[2] = (uint8_t)(value >> 8);
        direct[3] = (uint8_t)value;
        return true;
    }
    cpu_bus_access_begin(cpu);
    bool ok = cpu->bus.write_long(cpu->bus.ctx, addr, value);
    cpu_bus_access_end(cpu);
    return ok;
}

static inline bool cpu_bus_cpu_space_read(mc68k_state_t *cpu, uint8_t fc, uint32_t addr,
                                          uint8_t size, uint32_t *out) {
    cpu_bus_access_begin(cpu);
    bool ok = cpu->bus.cpu_space_read(cpu->bus.ctx, fc, addr, size, out);
    cpu_bus_access_end(cpu);
    return ok;
}

static inline bool cpu_bus_cpu_space_write(mc68k_state_t *cpu, uint8_t fc, uint32_t addr,
                                           uint8_t size, uint32_t value) {
    cpu_bus_access_begin(cpu);
    bool ok = cpu->bus.cpu_space_write(cpu->bus.ctx, fc, addr, size, value);
    cpu_bus_access_end(cpu);
    return ok;
}

static inline uint8_t cpu_bus_iack(mc68k_state_t *cpu, int level) {
    cpu_bus_access_begin(cpu);
    uint8_t vector = cpu->bus.iack(cpu->bus.ctx, level);
    cpu_bus_access_end(cpu);
    return vector;
}

/* データ読出しの本体。展開すると各 .o に重複して残り、命令キャッシュを圧迫するため、外部関数 1
 * つに まとめる（m68000.c）。 */
uint8_t cpu_read_byte_access(mc68k_state_t *cpu, uint32_t addr, const mc68k_access_t *access);
uint16_t cpu_read_word_access(mc68k_state_t *cpu, uint32_t addr, const mc68k_access_t *access);
uint32_t cpu_read_long_access(mc68k_state_t *cpu, uint32_t addr, const mc68k_access_t *access);

static inline uint8_t cpu_read_byte(mc68k_state_t *cpu, uint32_t addr) {
    const mc68k_access_t access = cpu_data_access(cpu, true, 1, false);
    return cpu_read_byte_access(cpu, addr, &access);
}

static inline uint16_t cpu_read_word(mc68k_state_t *cpu, uint32_t addr) {
    const mc68k_access_t access = cpu_data_access(cpu, true, 2, false);
    return cpu_read_word_access(cpu, addr, &access);
}

static inline uint32_t cpu_read_long(mc68k_state_t *cpu, uint32_t addr) {
    const mc68k_access_t access = cpu_data_access(cpu, true, 4, false);
    return cpu_read_long_access(cpu, addr, &access);
}

static inline void cpu_write_byte_access(mc68k_state_t *cpu, uint32_t addr, uint8_t val,
                                         const mc68k_access_t *access) {
    /* BADADDR ヒューリスティックは除外（cpu_read_byte と同じ理由）。 */
    cpu->fault_data_out = val;
    uint32_t physical;
    bool cache_inhibit;
    if (!cpu_translate_write_access(cpu, addr, val, access, &physical, &cache_inhibit))
        return;
    if (cpu_has_onchip_cache(cpu) &&
        cpu_040_cache_write(cpu, addr, physical, val, access, cpu->access_cache_mode))
        return;
    bool ok = cpu_bus_write_byte(cpu, physical, val);
    cpu_account_bus_cycle(cpu, physical, access, cache_inhibit);
    if (!ok)
        cpu_signal_bus_error(cpu, addr, false);
}

static inline void cpu_write_byte(mc68k_state_t *cpu, uint32_t addr, uint8_t val) {
    const mc68k_access_t access = cpu_data_access(cpu, false, 1, false);
    cpu->fault_data_out = val;
    cpu_write_byte_access(cpu, addr, val, &access);
}

static inline void cpu_write_word_access(mc68k_state_t *cpu, uint32_t addr, uint16_t val,
                                         const mc68k_access_t *access) {
    cpu->fault_data_out = val;
    cpu_check_badaddr(cpu, addr, "ww");
    if (addr & 1) {
        if (!cpu_needs_alignment(cpu)) {
            mc68k_access_t byte_access = *access;
            cpu_access_mark_origin(&byte_access, access, addr);
            byte_access.size = 1;
            byte_access.split = true;
            cpu_write_byte_access(cpu, addr, (uint8_t)(val >> 8), &byte_access);
            if (cpu->exec_state == CPU_NORMAL)
                cpu_write_byte_access(cpu, addr + 1, (uint8_t)val, &byte_access);
            return;
        }
        fprintf(stderr,
                "ADDR ERR (write_word): addr=%06X val=%04X  pc=%06X\n"
                "  D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X\n"
                "  A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X\n",
                addr, val, cpu->pc, cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3], cpu->d[4],
                cpu->d[5], cpu->d[6], cpu->d[7], cpu->a[0], cpu->a[1], cpu->a[2], cpu->a[3],
                cpu->a[4], cpu->a[5], cpu->a[6], cpu->a[7]);
        cpu_note_access(cpu, access); /* A-2、落とし穴A（read_word版と同じ理由） */
        cpu_signal_address_error(cpu, addr, false);
        return;
    }
    uint32_t physical;
    bool cache_inhibit;
    if (!cpu_translate_write_access(cpu, addr, val, access, &physical, &cache_inhibit))
        return;
    if (cpu_has_onchip_cache(cpu) &&
        cpu_040_cache_write(cpu, addr, physical, val, access, cpu->access_cache_mode))
        return;
    bool ok = cpu_bus_write_word(cpu, physical, val);
    cpu_account_bus_cycle(cpu, physical, access, cache_inhibit);
    if (!ok)
        cpu_signal_bus_error(cpu, addr, false);
}

static inline void cpu_write_word(mc68k_state_t *cpu, uint32_t addr, uint16_t val) {
    const mc68k_access_t access = cpu_data_access(cpu, false, 2, false);
    cpu->fault_data_out = val;
    cpu_write_word_access(cpu, addr, val, &access);
}

static inline void cpu_write_long_access(mc68k_state_t *cpu, uint32_t addr, uint32_t val,
                                         const mc68k_access_t *access) {
    cpu->fault_data_out = val;
    cpu_check_badaddr(cpu, addr, "wl");
    if ((addr & 3) == 2 && !cpu_needs_alignment(cpu)) {
        mc68k_access_t word_access = *access;
        cpu_access_mark_origin(&word_access, access, addr);
        word_access.size = 2;
        word_access.split = true;
        cpu_write_word_access(cpu, addr, (uint16_t)(val >> 16), &word_access);
        if (cpu->exec_state == CPU_NORMAL)
            cpu_write_word_access(cpu, addr + 2, (uint16_t)val, &word_access);
        return;
    }
    if (addr & 1) {
        if (!cpu_needs_alignment(cpu)) {
            mc68k_access_t byte_access = *access;
            cpu_access_mark_origin(&byte_access, access, addr);
            byte_access.size = 1;
            byte_access.split = true;
            cpu_write_byte_access(cpu, addr, (uint8_t)(val >> 24), &byte_access);
            if (cpu->exec_state != CPU_NORMAL)
                return;
            mc68k_access_t word_access = *access;
            cpu_access_mark_origin(&word_access, access, addr);
            word_access.size = 2;
            word_access.split = true;
            cpu_write_word_access(cpu, addr + 1, (uint16_t)(val >> 8), &word_access);
            if (cpu->exec_state != CPU_NORMAL)
                return;
            cpu_write_byte_access(cpu, addr + 3, (uint8_t)val, &byte_access);
            return;
        }
        fprintf(stderr, "ADDR ERR (write_long): addr=%06X val=%08X  pc=%06X\n", addr, val,
                cpu->pc);
        cpu_note_access(cpu, access); /* A-2、落とし穴A（read_word版と同じ理由） */
        cpu_signal_address_error(cpu, addr, false);
        return;
    }
    uint32_t physical;
    bool cache_inhibit;
    if (!cpu_translate_write_access(cpu, addr, val, access, &physical, &cache_inhibit))
        return;
    if (cpu_has_onchip_cache(cpu) &&
        cpu_040_cache_write(cpu, addr, physical, val, access, cpu->access_cache_mode))
        return;
    bool ok = cpu_bus_write_long(cpu, physical, val);
    cpu_account_bus_cycle(cpu, physical, access, cache_inhibit);
    if (!ok)
        cpu_signal_bus_error(cpu, addr, false);
}

static inline void cpu_write_long(mc68k_state_t *cpu, uint32_t addr, uint32_t val) {
    const mc68k_access_t access = cpu_data_access(cpu, false, 4, false);
    cpu->fault_data_out = val;
    cpu_write_long_access(cpu, addr, val, &access);
}

/* 例外フレーム、RTE/RTR スタック、例外ベクタは、進入原因の SR／フォルト状態にかかわらず
 * スーパーバイザデータ空間（FC=5）を使う。 */
static inline uint8_t cpu_supervisor_read_byte(mc68k_state_t *cpu, uint32_t addr) {
    const mc68k_access_t access = cpu_supervisor_data_access(true, 1, false);
    return cpu_read_byte_access(cpu, addr, &access);
}
static inline uint16_t cpu_supervisor_read_word(mc68k_state_t *cpu, uint32_t addr) {
    const mc68k_access_t access = cpu_supervisor_data_access(true, 2, false);
    return cpu_read_word_access(cpu, addr, &access);
}
static inline uint32_t cpu_supervisor_read_long(mc68k_state_t *cpu, uint32_t addr) {
    const mc68k_access_t access = cpu_supervisor_data_access(true, 4, false);
    return cpu_read_long_access(cpu, addr, &access);
}
static inline void cpu_supervisor_write_byte(mc68k_state_t *cpu, uint32_t addr, uint8_t val) {
    const mc68k_access_t access = cpu_supervisor_data_access(false, 1, false);
    cpu->fault_data_out = val;
    cpu_write_byte_access(cpu, addr, val, &access);
}
static inline void cpu_supervisor_write_word(mc68k_state_t *cpu, uint32_t addr, uint16_t val) {
    const mc68k_access_t access = cpu_supervisor_data_access(false, 2, false);
    cpu->fault_data_out = val;
    cpu_write_word_access(cpu, addr, val, &access);
}
static inline void cpu_supervisor_write_long(mc68k_state_t *cpu, uint32_t addr, uint32_t val) {
    const mc68k_access_t access = cpu_supervisor_data_access(false, 4, false);
    cpu->fault_data_out = val;
    cpu_write_long_access(cpu, addr, val, &access);
}

/*
 * PC から 1 ワードをフェッチして PC を 2 進める。
 * アドレスエラーが発生した場合は PC を進めない（エラーアドレスを保持するため）。
 *
 * fault_access_kind の代入は cpu_note_access() と同じ group0_fault_latched
 * ガードの対象にする。既にデータフォールトがラッチされた後の反復でこの関数が
 * 呼ばれても（現状そういうハンドラは存在しないが）、EXTENSION/OPCODE に
 * 化けさせないため（、mc68000リポジトリ）。
 */
/* 060 の命令フェッチ高速経路。通常経路（cpu_read_word_access から変換、cpu_040_cache_read へ
 * 続く流れ）と同じ条件・同じ副作用で、直前の命令側ライン、または同じセットのラインに当たる語
 * （CACR.IE=1）、またはバスから直接読んだ語（CACR.IE=0）を返す。次のどれかに当たれば false
 * を返し、 通常経路に任せる。
 *   - 060 以外、ifast 無効、試験用の無効化、BADADDR ログ有効、group-0 保留中
 *   - ifast 条件（S・ページ・TC・ITT0/1・ATC entry）が成り立たない
 *   - IE=1 でキャッシュ不可の entry、同じセットに valid・loaded・物理ライン一致のラインが無い
 *     （CINV/CPUSH・置換を含む）
 * 副作用は通常経路と同じ（cpu_note_access、ATC の history、access_cache_mode、
 * access_tt_hit、IE=0 ではバス読出しと待ち時間の計上、バスエラー）。 */
uint16_t cpu_060_ifetch_nocache(mc68k_state_t *cpu, const mc68k_access_t *access,
                                mc68k_atc_entry_t *e, uint32_t physical);

static inline bool cpu_060_ifetch_fast(mc68k_state_t *cpu, const mc68k_access_t *access,
                                       uint16_t *out) {
    if (!cpu_is_68060(cpu) || cpu->atc060_ifast_disable || cpu->ifetch_fast_disable ||
        cpu->badaddr_log_on || cpu->group0_fault_latched)
        return false;
    const bool icache = (cpu->cacr & MC68K_CACR_040_IE) != 0;
    const uint32_t pc = cpu->pc;
    mc68k_atc060_ifast_t *f = &cpu->atc060_ifast;
    /* S とページの一致は毎回確認する。 */
    if (!f->valid || ((pc ^ f->logical_page) >> f->page_shift) != 0 ||
        (uint8_t)(access->fc & 4u) != f->fc_s)
        return false;
    mc68k_atc_entry_t *e = &cpu->atc060_i[f->slot];
    if (f->gen != cpu->ifast_gen) {
        /* 無効化イベントがあった。TC・ITT・entry を再検証し、通れば世代を更新する。 */
        if (cpu->mmu060_tc != f->tc || cpu->mmu060_itt0 != f->itt0 ||
            cpu->mmu060_itt1 != f->itt1 || !e->valid || (e->fc & 4u) != f->fc_s ||
            e->page_shift != f->page_shift || e->logical_page != f->logical_page)
            return false;
        f->gen = cpu->ifast_gen;
    }
    if (icache && e->cache_mode >= 2)
        return false;
    const uint32_t physical = e->physical_page | (pc & ((UINT32_C(1) << f->page_shift) - 1u));
    if (!icache) {
        *out = cpu_060_ifetch_nocache(cpu, access, e, physical);
        return true;
    }
    const uint32_t base = physical & ~UINT32_C(0xf);
    const mc68k_040_cache_line_t *line = &cpu->icache040[0][0] + cpu->icache040_last;
    if (!line->valid || !line->loaded || line->physical_line != base) {
        /* 直前のラインに無いとき、同じセットの 4 ウェイを探す（cpu_040_cache_read の
         * cache_line() と同じく、最初に valid
         * で物理ラインが一致するウェイを選ぶ）。見つかっても loaded でなければ
         * 通常経路が充填するので任せる。ヒットのとき通常経路が変えるのは icache040_last だけ。
         */
        const mc68k_040_cache_line_t *set_lines =
            cpu->icache040[(physical >> 4) & (MC68K_060_CACHE_SETS - 1u)];
        unsigned way = 0;
        while (way < MC68K_040_CACHE_WAYS &&
               !(set_lines[way].valid && set_lines[way].physical_line == base))
            way++;
        if (way == MC68K_040_CACHE_WAYS || !set_lines[way].loaded)
            return false;
        line = &set_lines[way];
        cpu->icache040_last = (uint16_t)(line - &cpu->icache040[0][0]);
    }
    cpu_note_access(cpu, access);
    e->history = ++cpu->atc060_clock;
    cpu->access_cache_mode = e->cache_mode;
    cpu->access_tt_hit = false;
    *out = (uint16_t)(line->data[(physical & 15u) >> 2] >> ((2u - (physical & 3u)) * 8u));
    return true;
}

static inline uint16_t cpu_fetch_word(mc68k_state_t *cpu) {
    if (cpu->model == MC68K_MODEL_68000)
        return cpu_prefetch_fetch_word(cpu);
    /* フォールト等で exec_state が NORMAL でないときは取得せず 0 を返す。
     * MC68000 経路の cpu_prefetch_fetch_word と同じ契約で、BSR.L/Bcc.L や
     * long 即値の上位語が失敗した後に下位語を取りに行かないようにする。 */
    if (cpu->exec_state != CPU_NORMAL)
        return 0;
    if (!cpu->group0_fault_latched)
        cpu->fault_access_kind = cpu->opcode_fetch_complete ? FAULT_EXTENSION : FAULT_OPCODE;
    const mc68k_access_t access = cpu_program_access(cpu, 2);
    if (cpu->pc & 1) {
        /* A-2（fault_access_kindは上で設定済み、instruction=trueなので上書きされない） */
        cpu_note_access(cpu, &access);
        cpu_signal_address_error(cpu, cpu->pc, true);
        return 0;
    }
    uint16_t w;
    if (!cpu_060_ifetch_fast(cpu, &access, &w))
        w = cpu_read_word_access(cpu, cpu->pc, &access);
    if (cpu->exec_state == CPU_NORMAL)
        cpu->pc += 2;
    return w;
}
