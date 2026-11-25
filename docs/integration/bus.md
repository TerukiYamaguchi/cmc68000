# バス

`cmc68000` はメモリマップや周辺機器を所有しません。利用側が `mc68k_bus_t` に
callback を設定し、CPU インスタンス生成時に渡します。CPU は callback テーブルを値コピー
するため、`mc68k_create()` 後に `mc68k_bus_t` 自体を保持する必要はありません。一方、
`ctx` とその指すバス状態は CPU より長く有効でなければなりません。

```c
mc68k_bus_t bus = {
    .read_byte = machine_read_byte,
    .read_word = machine_read_word,
    .read_long = machine_read_long,
    .write_byte = machine_write_byte,
    .write_word = machine_write_word,
    .write_long = machine_write_long,
    .ctx = &machine,
};
mc68k_state_t *cpu = mc68k_create(&bus);
```

`read_byte`、`read_word`、`read_long`、`write_byte`、`write_word`、`write_long` はすべて
必須です。`mc68k_create()` は callback の存在を検査しないため、これらを `NULL` にした
構成では CPU を実行してはいけません。C99 の指示付き初期化子を使うと、将来フィールドが
追加されても意図を保ちやすくなります。

## メモリアクセス callback

CPU が通常の命令・データ・例外フレーム・ベクタをアクセスすると、次の callback を呼びます。
MMU を持つモデルでは、`addr` は変換後の物理アドレスです。MMU を使わないモデルでは、
CPU が発行したアドレスがそのまま渡されます。

| callback | 転送サイズ | 成功時 | 失敗時 |
| --- | --- | --- | --- |
| `read_byte(ctx, addr, out)` | 1 byte | `*out` に値を設定し `true` を返す | `false` を返す |
| `read_word(ctx, addr, out)` | 2 bytes | `*out` に値を設定し `true` を返す | `false` を返す |
| `read_long(ctx, addr, out)` | 4 bytes | `*out` に値を設定し `true` を返す | `false` を返す |
| `write_byte(ctx, addr, value)` | 1 byte | 書込み後に `true` を返す | `false` を返す |
| `write_word(ctx, addr, value)` | 2 bytes | 書込み後に `true` を返す | `false` を返す |
| `write_long(ctx, addr, value)` | 4 bytes | 書込み後に `true` を返す | `false` を返す |

`false` は未マップ領域や応答しないデバイスを表し、CPU は bus error として処理します。
read callback は `false` を返す場合に `*out` を設定する必要はありません。通常の
read/write callback から `mc68k_signal_bus_error()` を呼んではいけません。失敗は必ず
戻り値だけで通知します。

word と long の数値は、CPU のビッグエンディアン順で組み立てます。たとえばメモリ上の
`12 34 56 78` は `read_long()` から `UINT32_C(0x12345678)` として返します。ホストの
エンディアンに依存するポインタキャストは使わないでください。

68000/68010 では奇数アドレスへの word/long アクセスは CPU が address error にします。
68020 以降の非整列アクセスは、モデルに応じて CPU が複数の callback 呼出しへ分解します。
callback はアラインメント例外を独自に生成せず、受け取った各アクセスをそのサイズどおりに
処理してください。

## 物理ページ表による直接アクセス

`mc68k_bus_t.page_map` は、RAM のように副作用がなく、通常の callback と完全に同じ結果を返す
物理ページを CPU が直接読むための任意の表です。`NULL` なら従来どおり全アクセスが callback を
通るため、既存の統合は変更不要です。

表は 4 KiB ページ単位の 2 段配列です。`l1[physical >> 24]` が指す 2 段目の
`(physical >> MC68K_PAGE_SHIFT) & (MC68K_PAGE_L2_COUNT - 1)` 番目に、対象ページの
`mc68k_page_entry_t` を置きます。`host` はページ先頭を指すビッグエンディアンのバイト列です。
`MC68K_PAGE_READ_DIRECT` と `MC68K_PAGE_WRITE_DIRECT` は、それぞれ副作用も bus error もない
読み出し／書き込みだけに指定してください。`MC68K_PAGE_SUPERVISOR_ONLY` を併用すると、直接アクセスは
スーパーバイザ状態だけに制限されます。

`MC68K_PAGE_TIMING` を指定したページでは、`wait_class` が `wait_table` の行を選びます。
`access_timing` callback も設定している場合に限り、その行の read/write（size 1/2）または
read_long/write_long（size 4）が callback と同じ待ちサイクルを表します。`access_timing` が `NULL`
ならページ表の待ちサイクルは使われません。`MC68K_PAGE_CACHEABLE` も callback が返す cacheable 属性と
一致させます。

直接アクセス、タイミング表、read/write の各指定は独立です。当該方向の直接アクセスがページ内で成功した
場合だけ、その `read_*` または `write_*` callback を省略します。`MC68K_PAGE_TIMING` が有効なアクセスは
`access_timing` callback を省略し、無効なら直接アクセスでも callback を呼びます。片方向だけを直接アクセスに
した場合、反対方向の memory callback は従来どおり呼ばれます。直接書込みでは `write_hook` を呼びません。

ページ表と callback の等価性は利用側の責務です。MMIO、読出し副作用、bus error、DMA による内容更新を
即時に観測する必要があるページは直接アクセスに指定しないでください。ページ境界をまたぐアクセス、または
ユーザ状態での `MC68K_PAGE_SUPERVISOR_ONLY` ページも callback 経由になります。`disable_mask` が非 0 の間は
CPU が表を使わないため、利用側は表を更新する前後でこの値を使って安全に無効化できます。CPU は callback
をまたいでエントリや `host` のポインタを保持しませんが、`page_map`、2 段目の配列、各ページの `host` は
CPU のアクセス中に有効でなければなりません。

## 任意 callback

| callback | 用途 | 未設定時 |
| --- | --- | --- |
| `iack` | 割り込み受付時のベクタ応答 | オートベクタを使用する |
| `reset_peripheral` | ゲストの `RESET` 命令による外部周辺機器リセット | 何もしない |
| `access_timing` | バスアクセスごとの待ちサイクルと cacheable 属性 | 待ちサイクル 0、cacheable でないものとして扱う |
| `cpu_space_read` / `cpu_space_write` | CPU 空間（FC=7）へのアクセス | 外付けコプロセッサなしとして扱う |
| `sync` | DMA 同期用に予約されたフック | 現行実装では呼び出されない |
| `write_hook` | 将来の JIT 無効化用に予約されたフック | 現行実装では呼び出されない |

### 割り込み acknowledge

`iack(ctx, level)` は level 1–7 で呼ばれます。戻り値 0 はオートベクタ
（ベクタ `24 + level`）、非 0 はその値をベクタ番号として使うベクタド割り込みです。

IACK サイクルに応答デバイスがない場合だけ、callback 内で
`mc68k_signal_bus_error(cpu)` を呼び、0 を返してください。CPU はこれをスプリアス
割り込みとしてベクタ 24 へ振り分けます。この API は IACK 専用です。

### 周辺機器 reset

`reset_peripheral(ctx)` は、スーパーバイザモードでゲストが `RESET` 命令を実行したときに
呼ばれます。ホストのプロセスや CPU インスタンスをリセットする callback ではありません。
外部デバイスだけをリセットします。

### バスアクセスのタイミング

`access_timing(ctx, access, out)` は、CPU が実際に memory callback を呼んだ直後に呼ばれます。
`access` は物理アドレス、転送サイズ、function code、read/write、命令フェッチかどうか、
read-modify-write 列の一部かどうかを示します。callback は `out->wait_cycles` と
`out->cacheable` を設定します。

`wait_cycles` はそのバスサイクルに加算するサイクル数です。`cacheable` が true で CPU 側の
キャッシュ対象になった read は、キャッシュヒット時の待ち時間を加算しません。初期値は
`wait_cycles = 0`、`cacheable = false` ですが、callback では必要な両フィールドを明示的に
設定してください。

### CPU 空間アクセス

`cpu_space_read` と `cpu_space_write` は、68020/68030 の外付けコプロセッサ接続で使います。
外付けコプロセッサを使う場合は両方を設定します。`fc` は常に `MC68K_FC_CPU_SPACE`（7）で、
値は `size` バイトを下位側に詰めて受け渡します。通常の命令・データアクセスは、この
callback ではなく前節の read/write callback に届きます。

MC68060 では LPSTOP と BKPT の CPU 空間バスサイクルにも、それぞれ
`cpu_space_write` と `cpu_space_read` が使われます。必要な callback だけを設定できます。

## 実装上の注意

- callback 内から同じ CPU の `mc68k_execute()` を再入実行しないでください。
- read callback は成功時に必ず `out` を設定してください。
- バス状態を複数スレッドから変更する場合、同期は利用側の責務です。
- `mc68k_bus_t` の将来のフィールド追加に備え、構造体は `{0}` または指示付き初期化子で
  初期化してください。

次は [実行](execution.md) で実行ループを、[割り込みと例外](interrupts_exceptions.md) で
IRQ と例外の利用側契約を説明します。
