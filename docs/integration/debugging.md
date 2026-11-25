# デバッグ

`cmc68000` のデバッグ API はホスト側の実行制御と観測を提供します。通常の
`mc68k_execute()` の代わりに `mc68k_debug_run()` を使うと、ブレークポイント、
ウォッチポイント、例外で停止できます。

同一 CPU インスタンスに対するデバッグ API、`mc68k_execute()`、`mc68k_reset()` は
内部で同期されません。複数スレッドから使う場合は、ホスト側で直列化してください。

## デバッグ実行

```c
mc68k_set_exception_break_mask(cpu, UINT64_C(1) << 4); /* illegal instruction */

int used = mc68k_debug_run(cpu, 50000);
switch (mc68k_get_stop_reason(cpu)) {
case MC68K_STOP_BREAKPOINT:
case MC68K_STOP_WATCHPOINT:
case MC68K_STOP_TRACE:
case MC68K_STOP_EXCEPTION:
    inspect_cpu(cpu);
    break;
case MC68K_STOP_HALTED:
    report_halt(cpu);
    break;
case MC68K_STOP_NONE:
    break;
}
```

`mc68k_debug_run(cpu, max_cycles)` は内部で 1 ステップずつ実行し、停止条件を各ステップ後に
確認します。戻り値は `mc68k_execute()` と同じ消費サイクル数です。

| 停止理由 | 意味 |
| --- | --- |
| `MC68K_STOP_NONE` | 予算を使い切った、または `STOP` 待機中 |
| `MC68K_STOP_BREAKPOINT` | ブレークポイントに到達 |
| `MC68K_STOP_WATCHPOINT` | 監視対象の値が変化 |
| `MC68K_STOP_TRACE` | trace 例外（ベクタ 9） |
| `MC68K_STOP_EXCEPTION` | 設定した例外、または保留 group 0 例外 |
| `MC68K_STOP_HALTED` | 二重 bus fault などによる CPU halt |

`mc68k_get_last_exception_vector()` は、`TRACE` または `EXCEPTION` 停止時に実際に配送した
ベクタ番号を返します。例外がなければ `-1` です。例外停止の対象は 0–63 のビットマスクで
設定し、ビット n がベクタ n に対応します。既定ではすべて無効です。

```c
uint64_t break_on_faults = (UINT64_C(1) << 2) |  /* bus error */
                           (UINT64_C(1) << 3) |  /* address error */
                           (UINT64_C(1) << 4) |  /* illegal instruction */
                           (UINT64_C(1) << 8);   /* privilege violation */
mc68k_set_exception_break_mask(cpu, break_on_faults);
```

## ブレークポイントと単一ステップ

アドレス比較型ブレークポイントは、ゲストメモリを変更しません。最大
`MC68K_MAX_BREAKPOINTS` 個を登録できます。

```c
int id = mc68k_add_breakpoint(cpu, 0x00012340, NULL, NULL);
if (id < 0)
    return -1;

/* 条件付きの場合は cond が非0を返したときだけ停止する。 */
mc68k_enable_breakpoint(cpu, id, 1);
```

アドレスは偶数でなければならず、条件 callback は CPU 状態を変更してはいけません。
単一ステップには `mc68k_debug_run(cpu, 1)`、複数命令には
`mc68k_debug_run_n(cpu, count)` を使います。`mc68k_execute(cpu, 1)` でも 1 ステップを
実行できますが、ブレークポイント・ウォッチポイント・例外停止は処理しません。

## 非侵襲メモリ読み出しと逆アセンブル

デバッガのメモリ表示には、通常の bus read callback ではなく peek hook を登録します。
peek hook は memory-mapped I/O の read-clear などの副作用を起こしてはいけません。

```c
mc68k_set_peek_hooks(cpu, machine_peek_byte, machine_peek_word,
                     machine_peek_long, machine);

uint8_t bytes[16];
if (mc68k_debug_read_bytes(cpu, address, bytes, sizeof(bytes)) == 0) {
    char text[80];
    if (mc68k_disassemble(cpu, address, text, sizeof(text)) > 0)
        show_disassembly(address, text);
}
```

`mc68k_debug_read_physical_bytes()` は物理アドレスを、`mc68k_debug_read_bytes()` は
現行 SR に対応する data function code の論理アドレスを読みます。
`mc68k_debug_read_bytes_fc()` では function code を明示できます。

MMU モデルでは `mc68k_debug_translate()` を使うと、ATC、U/M bit、例外状態を変更せずに
論理アドレスを変換できます。変換結果の `fault` で invalid、write-protect、supervisor 違反、
テーブル bus error などを区別できます。

## ウォッチポイント

ウォッチポイントは peek hook による値比較です。最大 `MC68K_MAX_WATCHPOINTS` 個まで、
サイズ 1、2、4 byte の範囲を登録できます。

```c
int id = mc68k_add_watchpoint(cpu, 0x00020000, 4);
if (id < 0)
    return -1;
```

ウォッチポイントは `mc68k_debug_run()` でのみ評価されます。書込みアクセス自体ではなく、
ステップ境界における値の変化を検出します。そのため、同じ値の再書込みや、1 命令内で元の値へ
戻る変更は検出できません。停止後は `mc68k_get_watchpoint_hit()` で ID と変更前後の値を取得します。

## 実行履歴とログ

PC、レジスタ、call、fault のリングを使うと、停止直前の経路を取得できます。

| 種類 | 有効化 | 構造化取得 | 容量 |
| --- | --- | --- | --- |
| PC | `mc68k_set_pc_ring()` | `mc68k_read_pc_ring()` | `MC68K_PC_RING_SIZE` |
| レジスタ | `mc68k_set_reg_ring()` | `mc68k_read_reg_ring()` | `MC68K_REG_RING_SIZE` |
| call | 常時記録 | `mc68k_read_call_ring()` | `MC68K_CALL_RING_SIZE` |
| fault | `mc68k_set_fault_ring()` | `mc68k_read_fault_ring()` | `MC68K_FAULT_RING_SIZE` |

read API は最新の `cap` 件を古い順で返します。`out == NULL` を渡すと、コピーせず有効件数を
取得できます。リングは reset 後も記録内容と有効／無効設定を維持します。

`mc68k_set_log_handler()` で診断ログの出力先を設定できます。未登録時の出力先は `stderr` です。
`mc68k_set_badaddr_log()`、`mc68k_set_badjump_log()`、`mc68k_set_badori_log()` などの
診断フラグは、問題の切り分け時だけ有効にしてください。

詳細なバス障害の原因と fault ring のフィールドは、開発者向けの
`docs/mc68000-internals/design/fault_ring.md` を参照してください。
