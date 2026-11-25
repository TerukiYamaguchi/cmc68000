/*
 * debug.h — mc68000 ライブラリのデバッグ支援API
 *
 * エミュレータ開発者がゲスト実行を外側から止めて調べる、実機にはない
 * ホスト側デバッグ支援機能をまとめる。通常運用
 * （IOCS連携・レジスタ表示等）に必要なAPIは cpu.h に残る。
 *
 * 移行方針: 当面 cpu.h の末尾から暫定的に include されており、cpu.h を
 * インクルードするだけで従来通り使える。x68000側で本ヘッダへの明示
 * includeへの切り替えが完了したら、cpu.h 側の暫定includeを外す。
 *
 * スレッド安全性:
 * 本ヘッダのAPIは mc68k_state_t へのアクセスを内部同期しない。同一CPU
 * インスタンスに対する本API、mc68k_execute()、mc68k_reset() 等の呼出しは、
 * ホスト側で直列化すること。
 */
#pragma once
#include "cpu_types.h"
#include <stddef.h>
#include <stdint.h>

/*
 * 非停止エントリプローブ。登録PCで命令フェッチが成功した直後、当該命令を
 * 実行する直前に呼ばれる。通常実行のopcodeテーブルを有効時だけトランポリンへ
 * 差し替えるため、無効時のmc68k_execute()ホットパスには追加コストがない。
 *
 * fnはCPU状態を観測してよいが、mc68k_set_entry_probes()/clear_entry_probes()
 * を再入呼びしてはならない。entry_pcは命令先頭の論理PCである。
 * 同一PCに複数のプローブを登録した場合は、登録順(id昇順)に全て呼ぶ。
 */
#define MC68K_MAX_ENTRY_PROBES 16
typedef void (*mc68k_entry_probe_fn_t)(mc68k_state_t *cpu, unsigned id, uint32_t entry_pc,
                                       void *ctx);

/*
 * エントリプローブを一括設定する。pcsは偶数アドレスをcount個含む配列で、
 * countは1..MC68K_MAX_ENTRY_PROBES、fnは非NULLでなければならない。
 * 成功時0、引数不正・退避テーブル確保失敗時-1。再設定は以前の設定を置換する。
 */
int mc68k_set_entry_probes(mc68k_state_t *cpu, const uint32_t *pcs, size_t count,
                           mc68k_entry_probe_fn_t fn, void *ctx);

/* プローブを無効化し、実行opcodeテーブルを有効化前の状態へ戻す。 */
void mc68k_clear_entry_probes(mc68k_state_t *cpu);

/*
 * デバッグ用履歴リング（pc / reg / call / fault）の共通契約。
 *
 * - 容量は MC68K_*_RING_SIZE 件の固定長。超えたら古いものから上書きする。
 * - 有効件数: 実際に記録した件数（容量で頭打ち）。未記録のスロットは dump / read の
 *   対象にならない。
 * - mc68k_set_X_ring(cpu, 0) は採取を止めるだけで、履歴・有効件数・書込み位置は残す。
 *   再び ON にすると続きから追記する。
 * - mc68k_reset() は ON/OFF 設定・履歴・有効件数・seq を変更しない。
 * - seq は「命令試行の通し番号」。mc68k_execute() が IRQ 受付判定を通過し、opcode fetch に
 *   進む直前に 1 つ進める（最初の試行が 1）。fetch 失敗・例外で中断した試行・RTE 後の
 *   再実行も別の番号になる。IRQ 受付は番号を進めない。リングが OFF でも進み、
 *   mc68k_reset() では 0 に戻らない。同じ試行の pc / reg / call / fault のエントリは
 *   同じ seq を持つ。
 * - 例外: 割込み受付中に起きた group0 fault（fault ring の origin == IRQ_ACCEPT）は試行では
 *   ないため、直前に採番された試行の seq を共有する（最初の試行より前なら 0）。
 *   「同じ seq ならば同じ試行」とは限らないので、突き合わせでは origin も見ること。
 *
 * read 系関数 mc68k_read_X_ring(cpu, out, cap) の境界規則:
 * - cpu == NULL → 0 を返す。
 * - out == NULL → cap を無視し、有効件数を返す。
 * - out != NULL && cap == 0 → 0 を返し、out に触れない。
 * - それ以外 → 最新 min(有効件数, cap) 件を古い順に out[0..] へコピーし、件数を返す。
 *
 * dump 系関数 mc68k_dump_X_ring(cpu, count) の件数規則:
 * - count <= 0 または容量超過なら容量を指定したものとみなし、さらに有効件数で clamp する。
 *   見出しの件数は実際に出力した件数。有効件数 0 なら見出しと終端行だけを出す。
 */
#define MC68K_PC_RING_SIZE    4096
#define MC68K_REG_RING_SIZE   512
#define MC68K_CALL_RING_SIZE  256
#define MC68K_FAULT_RING_SIZE 64

typedef struct {
    uint64_t seq;
    uint32_t pc; /* 命令先頭の論理アドレス */
} mc68k_pc_ring_entry_t;

typedef struct {
    uint64_t seq;
    uint32_t pc;          /* 命令先頭の論理アドレス */
    uint16_t opcode;      /* opcode_valid == 0 のとき値は未定義 */
    uint16_t sr;          /* 実行直前の SR */
    uint32_t d[8];        /* 実行直前の D0-D7 */
    uint32_t a[8];        /* 実行直前の A0-A7（A7 = その時点の SP） */
    uint8_t opcode_valid; /* 1 = opcode fetch 成功。0 = fetch 自体がフォールトした */
} mc68k_reg_ring_entry_t;

typedef struct {
    uint64_t seq;
    uint32_t from_pc;  /* BSR/JSR/RTS 命令自身のアドレス */
    uint32_t to_pc;    /* 分岐先（BSR/JSR）または復帰先（RTS） */
    uint8_t is_return; /* 1 = RTS、0 = BSR/JSR */
} mc68k_call_ring_entry_t;

size_t mc68k_read_pc_ring(const mc68k_state_t *cpu, mc68k_pc_ring_entry_t *out, size_t cap);
size_t mc68k_read_reg_ring(const mc68k_state_t *cpu, mc68k_reg_ring_entry_t *out, size_t cap);
size_t mc68k_read_call_ring(const mc68k_state_t *cpu, mc68k_call_ring_entry_t *out, size_t cap);

/* fault ring のエントリの列挙値。構造体フィールドは uint8_t で持ち、値は以下に固定する。 */
enum {
    MC68K_FAULT_ORIGIN_INSTRUCTION = 0, /* 命令の実行中（opcode fetch を含む） */
    MC68K_FAULT_ORIGIN_IRQ_ACCEPT = 1, /* 割込み受付のフレーム構築中（68020+ の $1 フレーム） */
    MC68K_FAULT_ORIGIN_EXCEPTION_FRAME = 2, /* 通常例外フレーム構築中 */
};
enum {
    MC68K_FAULT_ACCESS_OPCODE = 0,    /* opcode 自体の fetch */
    MC68K_FAULT_ACCESS_EXTENSION = 1, /* 拡張語の fetch */
    MC68K_FAULT_ACCESS_DATA_READ = 2,
    MC68K_FAULT_ACCESS_DATA_WRITE = 3,
};
enum {
    MC68K_FAULT_OUTCOME_NOT_BUILT =
        0, /* フレームを1語も書かずに HALT（68010 再フォルト安全弁） */
    MC68K_FAULT_OUTCOME_FRAME_WRITE_FAULT = 1, /* フレーム書込み中に二重フォルトして HALT */
    MC68K_FAULT_OUTCOME_VECTOR_READ_FAULT =
        2, /* フレームは書けたが、ベクタ読出しで二重フォルトして HALT */
    MC68K_FAULT_OUTCOME_VECTORED =
        3, /* ベクタを読み PC を設定した。ハンドラの fetch 成功は保証しない */
};
/* TT は通常は成功するが、TTR の write-protect による失敗では mmu_valid == 1 の
 * エントリにも現れる。 */
enum {
    MC68K_FAULT_MMU_SOURCE_NONE = 0,
    MC68K_FAULT_MMU_SOURCE_TT = 1,
    MC68K_FAULT_MMU_SOURCE_ATC = 2,
    MC68K_FAULT_MMU_SOURCE_WALK = 3,
};

/*
 * group0 例外（ベクタ2/3）1回分の記録。値はすべて、このフォールトの属性を保護するラッチ
 * （最初にフォールトしたバスサイクル）から取る。フレーム構築中の二重フォルトの属性は入らない。
 */
typedef struct {
    uint64_t seq;        /* フォールトした試行の seq。IRQ_ACCEPT では直前に採番された seq */
    uint64_t crp, srp;   /* 記録時点の値。mc68k_get_mmu_regs() と同じ規則（PMMU なしは 0） */
    uint32_t instr_pc;   /* フレームに積む命令先頭 PC（IRQ_ACCEPT では次に実行する命令の PC） */
    uint32_t fault_addr; /* フォールトした論理アドレス（68060 は frame に積む値。address error
                            は A0=0） */
    uint32_t tc;         /* 記録時点の値。crp/srp と同じ規則 */
    uint32_t descriptor_physical; /* 最後に読み終えた記述子の物理アドレス（下記の有効条件） */
    uint32_t
        table_bus_error_physical; /* テーブルアクセス（記述子の読出し・U/M ビットの書戻し）が
                                   * バスエラーになった物理アドレス（同） */
    uint16_t opcode;              /* opcode_valid == 0 のとき値は未定義 */
    uint16_t ssw;   /* このフレーム用に確定した SSW の生値（モデル別の形式）。書けたかは outcome
                       で判断。MC68060 は FSLW の下位 16 bit（address error は 0）。 */
    uint8_t origin; /* MC68K_FAULT_ORIGIN_* */
    uint8_t opcode_valid; /* 1 = この試行の opcode fetch が成功していた */
    uint8_t vector;       /* 2 = バスエラー、3 = アドレスエラー */
    uint8_t frame_format; /* 0 = 68000 の 14
                             バイトフレーム（フォーマットワードなし）、0x8、0xA、0xB、68040 の
                             0x7（bus error）・0x2（address error）、68060 の 0x4（bus error）・
                             0x2（address error）。予定の形式 */
    uint8_t fc;           /* フォールトしたサイクルの FC */
    uint8_t access_kind;  /* MC68K_FAULT_ACCESS_* */
    uint8_t size;    /* 1 / 2 / 4 バイト（SSW の SIZE の符号化に従う。3 バイトサイクルは 2） */
    uint8_t read;    /* 1 = 読出しサイクル */
    uint8_t rm;      /* 1 = ロックされた RMW サイクル（TAS/CAS/CAS2） */
    uint8_t outcome; /* MC68K_FAULT_OUTCOME_* */
    uint8_t mmu_valid;  /* 1 = PMMU の変換失敗が原因。0 = ホストバス失敗・アドレスエラー・PMMU
                           無効 */
    uint8_t mmu_source; /* MC68K_FAULT_MMU_SOURCE_*。mmu_valid == 0 なら NONE */
    uint8_t
        mmu_fault; /* mc68k_debug_mmu_fault_t の値。mmu_valid == 0 なら MC68K_DEBUG_MMU_NONE */
    uint8_t walk_valid; /* 1 = この変換でテーブルウォークを行った（mmu_source == WALK） */
    uint8_t
        level; /* walk_valid == 1 のときだけ有効。読み終えた記述子の段数。0 =
                記述子を1つも読み終えていない
                （ルートポインタでの失敗、ルートの早期終了ページ、第1段の読出しバスエラーを含む）
              */
} mc68k_fault_ring_entry_t;
/*
 * PMMU フィールドの有効条件:
 * - level: walk_valid == 1 のときだけ値として解釈する。ATC の負エントリで失敗した場合
 *   （mmu_source == ATC）は元のウォークの段数が残っていないので walk_valid = 0。
 * - descriptor_physical: walk_valid == 1 && level >= 1
 * のときだけ有効。失敗した物理アクセス先とは 限らない（「最後に読み終えた記述子」）。
 * - table_bus_error_physical: walk_valid == 1 && mmu_fault == MC68K_DEBUG_MMU_TABLE_BUS_ERROR
 * の ときだけ有効。
 * - MMUSR は記録しない。実行時の変換は MMUSR を合成せず、PTEST の MMUSR（LEVEL 依存）とは
 *   意味が異なるため。
 */

/* fault ring の ON/OFF。既定オフ。契約はリング共通（上記）。cpu == NULL の setter
 * は何もしない。 */
void mc68k_set_fault_ring(mc68k_state_t *cpu, int on);
int mc68k_get_fault_ring(const mc68k_state_t *cpu);
/* fault ring をダンプする。件数規則はリング共通。
 */
void mc68k_dump_fault_ring(const mc68k_state_t *cpu, int count);
size_t mc68k_read_fault_ring(const mc68k_state_t *cpu, mc68k_fault_ring_entry_t *out,
                             size_t cap);

/* デバッグ用: PC トレース・リングバッファを stderr（またはログハンドラ）にダンプ。
 * 件数規則は上の共通契約を参照。 */
void mc68k_dump_pc_ring(const mc68k_state_t *cpu, int count);

/* デバッグ用: PC履歴リング記録の ON/OFF。既定オフ（常時記録は性能低下）。
 * on != 0 で有効、on == 0 で無効にする。cpu == NULL の setter は何もしない。
 * reset後も記録状態と採取済み履歴を維持し、再有効化後は従来の位置から追記する。 */
void mc68k_set_pc_ring(mc68k_state_t *cpu, int on);
int mc68k_get_pc_ring(const mc68k_state_t *cpu);

/* デバッグ用: レジスタ履歴リングをダンプ（暴走直前の全レジスタ遷移）。
 * 各命令の実行直前レジスタ値を古い順に出力。件数規則は上の共通契約を参照。 */
void mc68k_dump_reg_ring(const mc68k_state_t *cpu, int count);

/* デバッグ用: レジスタ履歴記録（REGRING）の ON/OFF。既定オフ（常時記録は性能低下）。
 * デバッグ時のみ ON にして使う。
 * reset後も記録状態と採取済み履歴を維持し、再有効化後は従来の位置から追記する。 */
void mc68k_set_reg_ring(mc68k_state_t *cpu, int on);
int mc68k_get_reg_ring(const mc68k_state_t *cpu);

/* デバッグ用: BADMOVEA診断ログ（ops_move.c、上位8bitが$04-$FEの不正
 * ポインタ値をAnに積んだ際に出力）の ON/OFF。既定オフ（正常起動でも
 * 大量に出るタイトルがあるため常時オンはノイズになる）。 */
void mc68k_set_badmovea_log(mc68k_state_t *cpu, int on);
int mc68k_get_badmovea_log(const mc68k_state_t *cpu);

/* デバッグ用: BADADDR診断ログ（X68000の24-bitアドレス空間を前提とした
 * word/longアクセス診断）の ON/OFF。既定オフ。on != 0 で有効、on == 0 で
 * 無効にする。cpu == NULL の setter は何もしない。reset後も設定を維持する。 */
void mc68k_set_badaddr_log(mc68k_state_t *cpu, int on);
int mc68k_get_badaddr_log(const mc68k_state_t *cpu);

/* デバッグ用: BADJUMP診断ログ（上位8bitが$00/$FF以外へのジャンプ＝暴走兆候を
 * 検知した際に出力し、初回発生時にPCリングがONならPC履歴、レジスタリングが
 * ONならレジスタ履歴をダンプ）の ON/OFF。
 * 既定オフ。初回ダンプフラグは mc68k_reset() でクリアされる。 */
void mc68k_set_badjump_log(mc68k_state_t *cpu, int on);
int mc68k_get_badjump_log(const mc68k_state_t *cpu);

/* デバッグ用: BADORI診断ログ（opcode 0x0000 = ORI.B #0,D0 の実行を検知した際に
 * 出力し、初回発生時にPCリングがONならPC履歴、レジスタリングがONなら
 * レジスタ履歴をダンプ）の ON/OFF。既定オフ。
 * mc68k_set_opcode_illegal(cpu, 0x0000) による例外化とは独立の観測用途
 * （挙動は変えずログのみ出す）。初回ダンプフラグは mc68k_reset() でクリアされる。 */
void mc68k_set_badori_log(mc68k_state_t *cpu, int on);
int mc68k_get_badori_log(const mc68k_state_t *cpu);

/* デバッグ用: PCTRACE1024の ON/OFF。有効時、1024命令ごとにPCを出力する
 * （長時間実行での滞留箇所検出用。REGRINGと違い直近だけでなく全区間を間引いて
 * 流し続ける）。既定オフ。 */
void mc68k_set_pctrace1024(mc68k_state_t *cpu, int on);
int mc68k_get_pctrace1024(const mc68k_state_t *cpu);

/*
 * 指定 opcode を強制的に ILLEGAL 命令扱いにする。
 * 本来有効な命令でも呼び出すと ILLEGAL 命令例外（ベクタ 4）を発生させる。
 *
 * 用途: ORI.B #$00,D0 (opcode 0x0000) を illegal にすると
 *   ゼロ埋めメモリへの CPU 暴走を早期検出できる（XEiJ feature #8.1 相当）。
 */
void mc68k_set_opcode_illegal(mc68k_state_t *cpu, uint16_t opcode);

/*
 * デバッグ出力コールバック。
 *
 * BADMOVEA/BADADDR/BADJUMP/PCTRACE1024/強制ILLEGAL警告等の診断メッセージは、
 * 既定では stderr に直接出力される。ホストがこれをフックしたい場合
 * （ログウィンドウへのリダイレクト、テストでの出力抑制等）は
 * mc68k_set_log_handler() でコールバックを登録する。
 * fn == NULL で登録解除（stderr直書きに戻る、既定動作）。
 */
typedef void (*mc68k_log_fn_t)(void *ctx, const char *msg);
void mc68k_set_log_handler(mc68k_state_t *cpu, mc68k_log_fn_t fn, void *ctx);

/*
 * デバッグ実行の停止理由。
 *
 * mc68k_debug_run() が呼び出しごとに更新する。
 * 通常の mc68k_execute() 呼び出しでは更新されない（mc68k_debug_run経由の
 * デバッグセッションでのみ意味を持つ）。
 */
typedef enum {
    MC68K_STOP_NONE, /* デバッグ上注目すべき理由なし（サイクル予算消費完了、STOP待機を含む） */
    MC68K_STOP_BREAKPOINT, /* アドレス比較型ブレークポイントに到達（判定関数はステップ3で追加済み、
                            * stop_reasonへの反映はmc68k_debug_run()側の責務としてステップ4で配線）
                            */
    MC68K_STOP_WATCHPOINT, /* ウォッチポイントがヒット（ステップ7で配線） */
    MC68K_STOP_TRACE,      /* トレース例外（ベクタ9）。ゲスト自身のシングルステップ要求も含む */
    MC68K_STOP_EXCEPTION,  /* 上記以外の例外（ILLEGAL/特権違反/バスエラー/TRAP等） */
    MC68K_STOP_HALTED,     /* ダブルバスフォルト等でCPU停止（復帰はresetのみ） */
} mc68k_stop_reason_t;

/* デバッグ実行の直近の停止理由を取得する。mc68k_debug_run() 未使用時は常に MC68K_STOP_NONE。 */
mc68k_stop_reason_t mc68k_get_stop_reason(const mc68k_state_t *cpu);

/*
 * 直近の mc68k_debug_run() 呼び出しで実際にディスパッチされた例外のベクタ番号を
 * 取得する。stop_reason が MC68K_STOP_TRACE/MC68K_STOP_EXCEPTION の
 * 場合に意味を持つ。例外が発生していない場合は -1。
 *
 * 既知の制限: accept_irq() 内でのバス/アドレスエラー保留（mc68k_debug_run()の
 * コメント参照、稀なケース）で停止した場合は、その回の呼び出し内ではまだ
 * ディスパッチが完了していないため -1 を返す（ベクタ番号は次回の
 * mc68k_debug_run() 呼び出しで取得できる）。
 */
int mc68k_get_last_exception_vector(const mc68k_state_t *cpu);

/*
 * 非侵襲メモリ読み出しフック（デバッガのメモリダンプ用）。
 *
 * read_byte/read_word/read_long と異なり、メモリマップドI/Oの副作用
 * （read-clearレジスタ等）を発生させてはならない。mc68k_signal_bus_error() も
 * 絶対に呼ばないこと（呼ぶとバスエラーフラグが残留し、mc68k_debug_read_bytes()
 * 呼び出しタイミングでは直後に消費されないため、後続の無関係な命令実行時に
 * 誤ってバスエラー例外が発火する）。アドレスは呼び出し側が24bitマスク済みの
 * 値を渡す。
 *
 * mc68k_bus_t（cpu.hのバスコールバック本体）ではなくここに置く理由:
 * mc68k_create() の calloc により cpu 側フィールドは既定で NULL になるため、
 * 未登録時に「非対応」を保証するのに追加のゼロ初期化規約をホストに強いる
 * 必要がない（mc68k_bus_t は構造体を値でコピーするため、ホスト側が
 * 指示付き初期化子で埋め漏れると新規オプショナルフィールドが不定値になる
 * 危険がある）。
 */
typedef uint8_t (*mc68k_peek_byte_fn_t)(void *ctx, uint32_t addr);
typedef uint16_t (*mc68k_peek_word_fn_t)(void *ctx, uint32_t addr);
typedef uint32_t (*mc68k_peek_long_fn_t)(void *ctx, uint32_t addr);

/* NULL を渡したフックは未登録として扱う（全て NULL ならダンプ機能は非対応）。 */
void mc68k_set_peek_hooks(mc68k_state_t *cpu, mc68k_peek_byte_fn_t peek_byte,
                          mc68k_peek_word_fn_t peek_word, mc68k_peek_long_fn_t peek_long,
                          void *ctx);

/* PMMUを観測する非侵襲変換結果。DEBUG変換はATC/U/M/例外状態を変更しない。 */
typedef enum {
    MC68K_DEBUG_MMU_NONE,
    MC68K_DEBUG_MMU_INVALID,
    MC68K_DEBUG_MMU_WRITE_PROTECT,
    MC68K_DEBUG_MMU_SUPERVISOR,
    MC68K_DEBUG_MMU_LIMIT,
    MC68K_DEBUG_MMU_TABLE_BUS_ERROR,
} mc68k_debug_mmu_fault_t;

typedef struct {
    uint32_t physical;
    uint16_t mmusr;
    uint8_t level; /* 実際にアクセスしたテーブル段数。透過変換・直接マッピングは 0 */
    mc68k_debug_mmu_fault_t fault;
} mc68k_debug_translate_result_t;

/* logical を指定FC/RWで非侵襲に変換する。成功時0、変換不能時-1。 */
int mc68k_debug_translate(mc68k_state_t *cpu, uint32_t logical, unsigned fc, int read,
                          mc68k_debug_translate_result_t *out);

/* 物理addrからlenバイトを非侵襲に読み出す。peek_byte未登録時は-1。 */
int mc68k_debug_read_physical_bytes(mc68k_state_t *cpu, uint32_t addr, uint8_t *buf,
                                    uint32_t len);

/* 論理addrから現行SRに対応するdata FCで読み出す。変換に失敗した場合は
 * 失敗位置以降のbufを変更せず-1を返す。 */
int mc68k_debug_read_bytes(mc68k_state_t *cpu, uint32_t addr, uint8_t *buf, uint32_t len);

/* mc68k_debug_read_bytes()と同じだが、現行SRに依らずFC（0-7）を明示する。ゲストがユーザーモード
 * で停止中でも、スーパーバイザ空間（FC=5/6）のアドレスを読むために使う。fc>7は-1。 */
int mc68k_debug_read_bytes_fc(mc68k_state_t *cpu, uint32_t addr, uint8_t *buf, uint32_t len,
                              unsigned fc);

/*
 * アドレス比較型ブレークポイント。
 *
 * 命令パッチ方式（コアがゲストメモリを書き換える）は層違反のため採用せず、
 * PCとの比較のみで実現する。登録数は MC68K_MAX_BREAKPOINTS 個までの固定長
 * 配列で管理する（pc_ring/reg_ring と同じ流儀。実運用上のBP数は数個〜
 * 十数個程度で、可変長（malloc）にするメリットがない）。
 *
 * 再開セマンティクス（同一PCへの無限再ヒット回避）:
 * 「一時無効化→1命令→再有効化」のような特別な手順は不要。
 * 呼び出し側は必ず「まず mc68k_execute(cpu,1) で1命令実行し、その後で
 * mc68k_breakpoint_hit() を呼んで結果のPCを判定する」の順序を守ること。
 * 停止直後（PCがブレークポイントアドレスに一致した直後）に再度
 * mc68k_breakpoint_hit() を呼ばなければ、無限再ヒットは原理的に起きない
 * （※ BRA * のような自己ループ命令でPCが同じ値に戻る場合は例外で、
 * その場合は毎回ヒット報告されるのが正しい動作）。
 */
#define MC68K_MAX_BREAKPOINTS 16

/*
 * 条件付きブレークポイントの条件コールバック。非0を返すとヒットとみなす。
 * cond は CPU 状態を変更してはならず、ブレークポイントAPI（追加・削除・
 * 有効化）を再入呼びしてもいけない。
 */
typedef int (*mc68k_bp_cond_fn_t)(mc68k_state_t *cpu, void *ctx);

/*
 * ブレークポイントを追加する。cond が非NULLの場合、PCが一致しても
 * cond(cpu, cond_ctx) が非0を返したときのみヒットとみなす（条件付き
 * ブレークポイント）。cond=NULL ならアドレス一致のみでヒット。
 * addr が奇数（命令先頭になり得ない）の場合は登録せず -1 を返す。
 * 戻り値: ブレークポイントID（0以上、配列インデックスそのもの）。
 * MC68K_MAX_BREAKPOINTS 個まで登録済みの場合は -1。
 */
int mc68k_add_breakpoint(mc68k_state_t *cpu, uint32_t addr, mc68k_bp_cond_fn_t cond,
                         void *cond_ctx);

/*
 * id で指定したブレークポイントを削除する（無効なidは無視）。
 * 削除するとスロットは即座に完全クリアされ（addr/cond/cond_ctx含む）、
 * 以後 mc68k_add_breakpoint() が同じ番号のスロットを再利用しうる。
 * 削除済み・未使用のidをホスト側が使い回して enable/remove を呼んでも
 * 無害（無視される）だが、別の新しいブレークポイントに対して誤操作
 * しないよう、host側もidの使い回しに注意すること。
 */
void mc68k_remove_breakpoint(mc68k_state_t *cpu, int id);

/* id で指定したブレークポイントの有効/無効を切り替える（無効なidは無視）。 */
void mc68k_enable_breakpoint(mc68k_state_t *cpu, int id, int enable);

/* 登録済みの全ブレークポイントを一括削除する。 */
void mc68k_clear_breakpoints(mc68k_state_t *cpu);

/*
 * 現在のPCがヒットする、有効なブレークポイントのIDを返す（cond評価込み）。
 * ヒットなしなら -1。同一アドレスに複数登録されている場合は最小のIDのみ
 * 返す。呼び出しの順序規約は本セクション冒頭のコメント参照。
 */
int mc68k_breakpoint_hit(mc68k_state_t *cpu);

/*
 * 例外ブレークマスク。mc68k_debug_run() が特定の
 * 例外ベクタで停止するかどうかをビットごとに制御する。既定は全ビット0
 * （どの例外でも停止しない）。
 *
 * 理由: Line-F（ベクタ11）やTRAP（ベクタ32-47）はX68000のOS/API呼び出し
 * 機構として頻繁に使われる。既定で全例外停止にすると、通常のOS動作中に
 * 毎回 mc68k_debug_run() から抜けてしまい、「ホスト側のループ構造を変更
 * せずそのまま差し替え可能」という要件が満たせなくなる。ホストは興味の
 * あるベクタのビットだけを立てて使う（例: ILLEGAL(4)/特権違反(8)/
 * バスエラー(2)/アドレスエラー(3)など「暴走の兆候」に絞る、あるいは
 * トレース(9)だけ立ててシングルステップ検知に使う等）。
 *
 * ビットnがベクタ番号n（0〜63、有効範囲は0〜47）に対応する。CPU_HALTED
 * （ダブルフォルト）はマスクに関わらず常に停止する（別経路で検出する
 * 終端状態のため）。ブレークポイントと同様、mc68k_reset() では変化しない
 * （デバッグセッション中の設定として永続する）。
 */
void mc68k_set_exception_break_mask(mc68k_state_t *cpu, uint64_t vec_mask);
uint64_t mc68k_get_exception_break_mask(const mc68k_state_t *cpu);

/*
 * デバッグ実行制御。mc68k_execute(cpu, max_cycles)
 * の代替として、デバッグセッション中はこちらに差し替えて呼ぶ想定
 * （ホスト側のループ構造自体は変更しない）。内部で mc68k_execute(cpu,1)
 * を1命令ずつ回し、毎ステップ後にブレークポイント・例外・停止状態を
 * チェックする。
 *
 * 停止条件（優先順位順、上から順にチェックする）:
 *   1. CPU_HALTED（ダブルフォルト）: マスクに関係なく常に停止
 *      （stop_reason = MC68K_STOP_HALTED）
 *   2. CPU_STOPPED（STOP命令待機中でIRQマスクにより前進不可）: stop_reason
 *      は NONE のまま停止（mc68k_execute(cpu,N) を単体で呼んでも同状況
 *      では0を返して何もしないのと同じ挙動なので、既存の呼び出し元との
 *      互換性を壊さない）
 *   3. ブレークポイント／ウォッチポイントのヒット（stop_reason は
 *      MC68K_STOP_BREAKPOINT または MC68K_STOP_WATCHPOINT）。両方の判定
 *      関数は毎ステップ無条件に呼ぶ（ウォッチポイント側の値追跡を
 *      取りこぼさないため）。同一ステップで両方成立する場合はブレーク
 *      ポイントを優先する（ユーザーが明示的に置いた停止点のため）。
 *      ウォッチポイントのヒット詳細（変化前後の値）は
 *      mc68k_get_watchpoint_hit() で別途取得する
 *   4. 保留中のグループ0例外（accept_irq() 内でのバス/アドレスエラーの
 *      稀なケース。次回の mc68k_execute() 呼び出し冒頭で自動解消される
 *      想定だが、ホストに一旦制御を返す。stop_reason = MC68K_STOP_EXCEPTION）
 *   5. 例外ブレークマスクで有効化されたベクタの例外発生（ベクタ9なら
 *      stop_reason = MC68K_STOP_TRACE、それ以外は MC68K_STOP_EXCEPTION）
 *   6. max_cycles到達（stop_reasonはNONEのまま、通常のmc68k_execute()と
 *      同じ「サイクル予算を使い切った」ケース）
 *
 * 停止理由は mc68k_get_stop_reason() で取得する（ホストは呼び出しごとに
 * 確認すること。前回呼び出し時点の値ではなく、直近の mc68k_debug_run()
 * 呼び出しの結果を表す）。戻り値は消費した公称サイクル数
 * （mc68k_execute() と同じ意味）。
 *
 * 既知の制限: accept_irq() 内でのバス/アドレスエラーからの復帰は、次回
 * 呼び出しの冒頭でグループ0例外ディスパッチ＋新規1命令が同一の
 * mc68k_execute(cpu,1) 呼び出し内で連続して起きる（mc68k_execute() の
 * 既存実装の性質。詳細は mc68000/cpu.h の mc68k_execute() コメント
 * 「cycles=1 によるシングルステップのセマンティクス」項目3を参照）。
 * この稀なケースでは、例外ハンドラの先頭に置かれたブレークポイントを
 * 検出できないことがある。
 */
int mc68k_debug_run(mc68k_state_t *cpu, int max_cycles);

/*
 * 逆アセンブラ（独立モジュール src/debug/disasm.c）。
 *
 * addr にある1命令を非侵襲に逆アセンブルし、"MNEMONIC operand1,operand2"
 * 形式の文字列を buf に書き込む（例: "MOVE.L D0,D1"、"ADD.B #$12,(A0)"、
 * "BEQ $001004"）。buf_size に収まらない場合は安全に切り詰められる
 * （必ずNUL終端）。
 *
 * 戻り値: 命令のバイト長（最小2、次の命令addrの計算に使う）。
 * 以下の場合は -1 を返し、buf には触れない:
 *   - cpu が NULL、buf が NULL、buf_size が 0
 *   - addr が奇数（命令は常に偶数アドレスから始まる）
 *   - peek_byte が未登録（mc68k_set_peek_hooks() 未呼び出し。
 *     mc68k_debug_read_bytes() と同じ失敗契約）
 *
 * 68020 以上の coprocessor id=1 の F-line は FPU 命令（FNOP/FBcc/FDBcc/FScc/FTRAPcc/
 * FSAVE/FRESTORE/FMOVE/FMOVEM/算術）として逆アセンブルする。
 * FPU 構成（NONE/68881/68882）では表示を変えない。68000/68010、他の id、符号化として
 * 不正な語は "LINE-F"（長さ2）。
 *
 * 未割り当て（実装されていない）オペコードは "ILLEGAL" として長さ2を返す
 * （decode.c の op_illegal と同じ扱い）。実行用 op_table は参照しない
 * （mc68k_set_opcode_illegal() 等による実行時の書き換えに影響されないよう、
 * オペコードのビットパターンから独立に判定する）。
 */
int mc68k_disassemble(mc68k_state_t *cpu, uint32_t addr, char *buf, size_t buf_size);

/*
 * ウォッチポイント。
 *
 * ソフトウェア方式・ステップ実行ベース: mc68k_watchpoint_check() が
 * 呼ばれるたびに非侵襲読み出し（mc68k_debug_read_bytes()、ステップ2）で
 * 現在値を読み、前回値（登録時または直前のcheck()呼び出し時点の値）と
 * 比較する。したがって以下の制限がある:
 *   - 検出できるのは「値が変化したこと」のみ（書き込みアクセスそのものの
 *     検出ではない）。同じ値を書き戻すアクセスは検出できない
 *   - 1命令の実行中に値が変化してから元に戻った場合は検出できない
 *     （ステップの境界でしか比較しないため）
 *   - mc68k_execute() を直接呼ぶ通常実行では一切機能しない
 *     （mc68k_debug_run() 経由でのみ意味を持つ）
 *   - mc68k_reset() はメモリ内容を変えうるため、登録済みウォッチポイントの
 *     前回値を自動的に再取得する（そうしないとリセット直後の最初の
 *     ステップで偽ヒットになる）
 */
#define MC68K_MAX_WATCHPOINTS 8

/*
 * ウォッチポイントを追加する。size は 1/2/4 のみ有効（それ以外は-1）。
 * size=2/4 で addr が奇数の場合も-1（ブレークポイントの奇数拒否と同じ
 * 理由）。peek_byte が未登録の場合（mc68k_set_peek_hooks() 未呼び出し）
 * も-1（前回値を読めないため）。登録時点の値を非侵襲に読み、前回値として
 * 保持する。
 * 戻り値: ウォッチポイントID（0以上、配列インデックスそのもの）。
 * MC68K_MAX_WATCHPOINTS 個まで登録済みの場合は-1。
 */
int mc68k_add_watchpoint(mc68k_state_t *cpu, uint32_t addr, int size);

/* id で指定したウォッチポイントを削除する（無効なidは無視、スロットは
 * 即座に完全ゼロクリアされ再利用可能になる）。 */
void mc68k_remove_watchpoint(mc68k_state_t *cpu, int id);

/* id で指定したウォッチポイントの有効/無効を切り替える（無効なidは無視）。 */
void mc68k_enable_watchpoint(mc68k_state_t *cpu, int id, int enable);

/* 登録済みの全ウォッチポイントを一括削除する。 */
void mc68k_clear_watchpoints(mc68k_state_t *cpu);

/*
 * 全登録ウォッチポイントの現在値を非侵襲に読み直し、前回値と比較する
 * （エッジトリガ。呼ぶこと自体が前回値を現在値へ更新する副作用を持つ）。
 * mc68k_breakpoint_hit() と異なり全スロットを無条件に走査・更新してから
 * 判定する（先に見つかった1件で打ち切ると、それ以降のスロットの前回値が
 * 古いまま残り、次回呼び出しで検出が1ステップ遅れるため）。
 * 戻り値: 値が変化した最初のウォッチポイントのID。変化なしなら-1。
 * ヒット時の変化前後の値は mc68k_get_watchpoint_hit() で取得する。
 */
int mc68k_watchpoint_check(mc68k_state_t *cpu);

/*
 * 直近の mc68k_watchpoint_check() 呼び出しでヒットしたウォッチポイントの
 * IDと変化前後の値を取得する（mc68k_debug_run() がstop_reasonを
 * MC68K_STOP_WATCHPOINTにした直後に呼ぶ想定）。old_value/new_valueは
 * NULLを渡せば無視できる。ヒットなしなら-1を返す（old_value/new_valueは
 * 未変更）。
 */
int mc68k_get_watchpoint_hit(const mc68k_state_t *cpu, uint32_t *old_value,
                             uint32_t *new_value);

/* 復元後に登録済みウォッチポイントの比較基準値を現在のメモリへ合わせる。 */
void mc68k_rearm_watchpoints(mc68k_state_t *cpu);
/* PC／レジスタ／call／fault の履歴リングを消去する。設定と命令連番は維持する。 */
void mc68k_clear_history_rings(mc68k_state_t *cpu);

/*
 * 簡易バックトレース。
 *
 * BSR/JSR/RTS実行のたびにm68000.cのメインループがopcodeのビットパターン
 * を見て記録する（常時記録（ON/OFF 設定なし）。命令ハンドラ本体は
 * 変更しない）。構造化取得は mc68k_read_call_ring()。
 *
 * 既知の制限: 例外ディスパッチ・RTEはスタックを介した制御移動だが記録
 * 対象外のため、リング上のBSR/JSR/RTSの対応関係が一致しない場合がある
 * （割り込みハンドラ経由のRTS等）。
 */
void mc68k_dump_call_ring(const mc68k_state_t *cpu, int count);

/*
 * n命令後停止カウンタ。PCTRACE1024の上位互換
 * （ログ出力ではなく実際に停止する版）。
 *
 * mc68k_debug_run(cpu, 1) を num_instructions 回まで繰り返す薄い
 * ラッパー。cycles=1 で呼ぶと（どの命令も公称サイクル数が最低4以上のため）
 * 必ず1回だけ mc68k_execute(cpu,1) を回してから予算超過で抜ける、という
 * mc68k_execute(cpu,1) 自身の1ステップ性（mc68000/cpu.h参照）と同じ根拠で
 * 「1回のmc68k_debug_run(cpu,1)呼び出し＝1ステップ」を保証する。
 * BP/WP/例外等の停止条件が先に成立すればその時点で打ち切る
 * （mc68k_get_stop_reason() が MC68K_STOP_NONE 以外になった場合、または
 * exec_state が CPU_NORMAL でなくなった場合）。
 * num_instructions <= 0 の場合は何もせず0を返す。
 * 戻り値: 消費した公称サイクル数の合計（mc68k_execute()と同じ意味）。
 */
int mc68k_debug_run_n(mc68k_state_t *cpu, int num_instructions);
