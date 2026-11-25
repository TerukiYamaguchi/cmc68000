/*
 * bus_hooks.h — バスアクセスコールバック定義
 *
 * MC68000はアドレスバス23bit・データバス16bitの外付けバスを持つ。
 * このライブラリはバスの物理実装を持たず、すべてコールバック経由で
 * 外部（x68000側）に委譲する。これにより mc68000 ライブラリ自体は
 * SDL2にもプラットフォームにも一切依存しない純粋な C99 コードになる。
 *
 * read_byte/read_word/read_long/write_byte/write_word/write_long は
 * 戻り値でバスエラーを直接通知する（true = 成功、false = 該当デバイスなし）。
 * read系は読み出した値を out 引数経由で返す（戻り値はエラー通知専用）。
 * 旧フラグ方式（mc68k_signal_bus_error()/bus_error_pending）は
 * 「呼んだら直後に必ずチェックする」という呼び出し側の規律に依存しており、
 * チェック漏れでフラグが残留し後続の無関係なアクセスを誤爆させるバグを
 * 生んでいたため廃止した。mc68k_signal_bus_error() は
 * iack コールバック専用（スプリアス割り込み判定）として残る。
 *
 * sync() は DMA 同期用。CPU が一定サイクルを消費するたびに呼ばれ、
 * x68000側が HD68450 DMA コントローラのタイミングを合わせるのに使う。
 * 初期実装では空実装で構わない。
 *
 * write_hook() は将来の JIT 移行に備えた自己書き換え検出フック。
 * インタプリタ段階では NULL でよいが、フィールドは最初から用意しておく。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* タイミングフックが受け取るバスサイクル1回分の記述。
 * 「バスサイクル」= コアが read_byte/read_word/read_long/write_byte/
 * write_word/write_long のいずれかを1回呼ぶこと。68020+ の非整列アクセスは
 * コア自身が複数のバスサイクルへ分解するため、1つの論理アクセスがここへ
 * 複数回渡ることがある。 */
typedef struct {
    uint32_t physical; /* CPU が発行した物理アドレス（MMU 変換後） */
    uint8_t size; /* このバスサイクルで転送するバイト数（1 / 2 / 4。CPU 空間のオペランド CIR
                     だけ 3 もある） */
    uint8_t fc;   /* ファンクションコード 0-7 */
    bool read;
    bool instruction; /* プログラム空間の読み出し（命令フェッチ・拡張ワード） */
    bool rmw;         /* TAS/CAS のリードモディファイライト列の一部 */
} mc68k_bus_access_t;

typedef struct {
    uint16_t wait_cycles; /* このバスサイクル1回に上乗せする追加サイクル数 */
    bool cacheable;       /* ホストから見てキャッシュ可能な領域か */
} mc68k_bus_timing_t;

/*
 * 物理ページ→配列ポインタ表（ホストが所有し、コアは読むだけ）。
 * 表にヒットしたアクセスは、read_* と access_timing のコールバックを呼ばずに
 * コアが配列から直接読み、ウェイトも表から求める。表が NULL、無効化中、またはヒット
 * しないアクセスは従来どおりコールバックを通る。
 *
 * 表の内容はホストのコールバックと完全に等価でなければならない（同じ値・同じウェイト・
 * バスエラーにならない範囲のみ）。コアはコールバックをまたいでエントリのポインタを
 * 保持しない。
 */
#define MC68K_PAGE_SHIFT        12u
#define MC68K_PAGE_SIZE         (1u << MC68K_PAGE_SHIFT)
#define MC68K_PAGE_MASK         (MC68K_PAGE_SIZE - 1u)
#define MC68K_PAGE_L1_SHIFT     24u
#define MC68K_PAGE_L1_COUNT     256u
#define MC68K_PAGE_L2_COUNT     (1u << (MC68K_PAGE_L1_SHIFT - MC68K_PAGE_SHIFT))
#define MC68K_PAGE_WAIT_CLASSES 16u

/* mc68k_page_entry_t.flags */
#define MC68K_PAGE_TIMING \
    0x01u /* wait_class と MC68K_PAGE_CACHEABLE が access_timing と一致する */
#define MC68K_PAGE_CACHEABLE   0x02u /* access_timing の cacheable */
#define MC68K_PAGE_READ_DIRECT 0x04u /* host から直接読める（バスエラー・副作用なし） */
#define MC68K_PAGE_SUPERVISOR_ONLY \
    0x08u /* READ_DIRECT / WRITE_DIRECT はスーパーバイザ（SR の S ビット）のときだけ */
#define MC68K_PAGE_WRITE_DIRECT \
    0x10u /* host へ直接書ける（バスエラー・副作用なし）。write_hook は呼ばれない */

typedef struct {
    /* ページ先頭の配列ポインタ（ビッグエンディアンのバイト列）。READ_DIRECT または
     * WRITE_DIRECT のとき有効。WRITE_DIRECT のページは書き込み可能な配列でなければならない
     * （コアが const を外して書く）。 */
    const uint8_t *host;
    uint8_t wait_class; /* mc68k_page_map_t.wait_table[] への添字 */
    uint8_t flags;
} mc68k_page_entry_t;

typedef struct {
    uint16_t read, write;           /* size 1/2 */
    uint16_t read_long, write_long; /* size 4 */
} mc68k_page_wait_t;

typedef struct {
    uint32_t disable_mask; /* 0 のときだけ表を使う。ビットの意味はホストが決める */
    mc68k_page_wait_t wait_table[MC68K_PAGE_WAIT_CLASSES];
    /* physical >> 24 で 2 段目（MC68K_PAGE_L2_COUNT 個）を選ぶ。NULL ならコールバック */
    const mc68k_page_entry_t *l1[MC68K_PAGE_L1_COUNT];
} mc68k_page_map_t;

typedef struct {
    bool (*read_byte)(void *ctx, uint32_t addr, uint8_t *out);
    bool (*read_word)(void *ctx, uint32_t addr, uint16_t *out);
    bool (*read_long)(void *ctx, uint32_t addr, uint32_t *out);
    bool (*write_byte)(void *ctx, uint32_t addr, uint8_t val);
    bool (*write_word)(void *ctx, uint32_t addr, uint16_t val);
    bool (*write_long)(void *ctx, uint32_t addr, uint32_t val);
    void (*sync)(void *ctx, int cycles);                         /* DMA タイミング同期 */
    void (*write_hook)(void *ctx, uint32_t addr, uint32_t size); /* JIT 無効化予約 */
    /*
     * IACK コールバック: CPU が割り込みを受け付けるときに呼ばれる。
     * level = 割り込みレベル (1–7)。
     * 戻り値 0 = オートベクタ（ベクタ 24+level を使用）
     * 戻り値 非0 = ベクタドインタラプト（その番号 × 4 のアドレスへジャンプ）
     * NULL の場合は常にオートベクタ扱い。
     */
    uint8_t (*iack)(void *ctx, int level);
    /*
     * RESET 命令コールバック: CPU が RESET 命令 (0x4E70) を実行したとき呼ばれる。
     * 外部ペリフェラルをリセット状態に戻す処理をここに書く。
     * NULL の場合は何もしない。
     */
    void (*reset_peripheral)(void *ctx);
    void *ctx; /* コールバックに渡す不透明コンテキスト */
    /*
     * バスサイクル1回のコストをホストへ問い合わせる（MC68030内蔵
     * キャッシュ性能近似）。NULL なら wait 0 / cacheable false 相当で、既存の
     * 利用者は挙動が変わらない。呼び出しは成功したアクセスだけでなく、バス
     * エラーになったアクセス・PMMU のテーブルウォーク・例外フレーム書込み・
     * ベクタ読出しでも行われる。
     *
     * !!! 新しいフィールドは必ずこの位置（ctx の後ろ）に追加すること。
     * tests/ 配下に mc68k_bus_t の位置指定初期化子が14箇所あり、ctx までの
     * 11要素の並びに依存している。
     */
    void (*access_timing)(void *ctx, const mc68k_bus_access_t *access, mc68k_bus_timing_t *out);
    /*
     * CPU 空間（FC=7）アクセス。68020/68030 の
     * コプロセッサインタフェースが、外付けコプロセッサの CIR（コプロセッサインタフェース
     * レジスタ）を読み書きするときだけ呼ばれる。通常のデータ・命令アクセスは従来の
     * read_byte 等のまま（FC を渡さない）。
     *
     * addr = 0x00020000 | (cpID << 13) | CIR オフセット（[UM20 §7.1.4]）。size は 1〜4。16bit
     * の CIR
     * （応答・コントロール・セーブ・リストア・オペレーションワード・コマンド・コンディション・
     * レジスタセレクト）は 2、命令アドレス・オペランドアドレス CIR は 4。オペランド CIR
     * は転送する バイト数（通常 4。長さが 4 の倍数でない operand の端数は 1・2・3。[UM20 §7.3.8
     * Figure 7-21、§7.4.7、§7.4.9]）。 値は数値で、size バイトを下位詰めにする（read は *out
     * の下位 size バイトに返す。write は 下位 size バイトが有効）。条件 CIR には条件命令の word
     * 全体を渡す（上位ビットは予約。 [UM20 §7.3.7] のとおり実際の条件は bit5-0）。戻り値 false
     * = 該当デバイスなし（バスエラー）。 どちらかが NULL
     * なら外付けコプロセッサは無いものとして扱い、F-line 命令は従来どおり F-line 例外（ベクタ
     * 11）になる。ただし 68020/68030 の cpSAVE/cpRESTORE をユーザモードで実行すると、
     * 実機と同じく CIR に触れる前に特権違反（ベクタ 8）になる（コプロセッサの有無を知る前。
     * [UM20 §7.5.2.3]）。これだけは従来の挙動（F-line）から変わる。
     * EA の形式が不正な cpSAVE/cpRESTORE は、ユーザモードでも特権違反より先に F-line になる。
     * access_timing へ渡す size は、オペランド CIR の 3 バイト転送で 3
     * になる（mc68k_bus_access_t）。内蔵 FPU（mc68k_create_model_with_fpu()）が cpID=1
     * を担当するときは、このフックは呼ばれない。
     * MC68060 は、LPSTOP の write（addr 0、size 2、下位 16 bit = 即値 SR）と BKPT の
     * acknowledge read（addr 0、size 1）にも使う。hook が NULL ならバスサイクルを出さず、LPSTOP
     * は停止、 BKPT は illegal instruction（vector
     * 4）になる。false（TEA）でも結果は変わらない。
     *
     * !!! 新しいフィールドは必ずこの位置（access_timing の後ろ）に追加すること。
     */
    bool (*cpu_space_read)(void *ctx, uint8_t fc, uint32_t addr, uint8_t size, uint32_t *out);
    bool (*cpu_space_write)(void *ctx, uint8_t fc, uint32_t addr, uint8_t size, uint32_t val);
    /*
     * 物理ページ→配列ポインタ表。NULL なら全アクセスがコールバック経由（従来どおり）。
     *
     * !!! 新しいフィールドは必ずこの位置（cpu_space_write の後ろ）に追加すること。
     */
    const mc68k_page_map_t *page_map;
} mc68k_bus_t;

/* CPU 空間アクセスの FC（[UM20 §7.1.4]）。 */
#define MC68K_FC_CPU_SPACE 7u
