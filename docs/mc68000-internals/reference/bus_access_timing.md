# CPU バスアクセス時刻公開 API 設計（提案）

## 1. 目的と対象範囲

ホストが CPU の外部バスアクセスと並行して動く DMA を、同じ CPU 時刻軸で同期できるようにする。
具体的には、ホストの `read_*`、`write_*`、CPU 空間、IACK の各 callback が呼ばれる**直前**の
CPU 経過サイクルを取得可能にする。

初期実装の対象 CPU は MC68060 だけである。対象は `mc68k_execute()` 起点の外部バスアクセスである。命令フェッチ、拡張語・オペランド、
例外フレーム、ベクタ、MMU table walk、キャッシュ fill/writeback、外付けコプロセッサ CIR、
IACK を含む。`mc68k_reset()` の raw な初期ベクタ読出しは実行時間を持たないため対象外とする。

本書は設計レビュー前の提案であり、production code はまだ変更しない。

## 2. 背景と問題

既存の `mc68k_executed_cycles()` は、現在または直近の `mc68k_execute()` 呼出しについて、
実行中命令の**開始時点**までの経過サイクルを返す。この契約は命令境界で DMA をまとめて
進める用途には十分だが、同じ命令内に複数の CPU バスアクセスがある場合、それらの間の
順序と DMA の進行を表せない。

また、`mc68k_bus_t::sync` は DMA 同期用の予約 callback だが、現行実装は呼ばない。
`access_timing` は実バス callback の**後**に wait を回収するため、ホストがアクセス前に
DMA を進める時刻源には使えない。

`mc68k_executed_cycles()` の意味を変更すると既存利用者の命令境界契約を壊す。従って、
既存 API は維持し、バスアクセス専用の時刻 API を追加する。

## 3. 公開 API 契約

`include/mc68000/cpu.h` に次を追加する。

```c
/* 現在の mc68k_execute() 呼出し内で、進行中の外部バスアクセス直前までに
 * 経過した CPU サイクル数を返す。外部バス callback の実行中だけ true を返す。
 * false のとき *out は変更しない。副作用なし・O(1)。 */
bool mc68k_bus_access_cycles(const mc68k_state_t *cpu, uint64_t *out);
```

- `true` を返すのは、MC68060 の `mc68k_execute()` が発行した外部バス callback の動的な実行範囲だけである。
  `read_*`、`write_*`、`cpu_space_read`、`cpu_space_write`、`iack` の callback 本体から呼べる。
- `out` は、その callback が表すバスアクセスを開始する**直前**までの、当該
  `mc68k_execute()` 呼出し内の CPU サイクル数である。単位は CPU clock であり、DMAC clock
  などホスト固有の単位には変換しない。
- `out` は `uint64_t` とする。実行 budget と既存 API は `int` のまま維持するが、ホストが
  複数回の実行結果を足し上げる時刻源には符号付き整数の桁あふれを持ち込まない。
- `out` は、先行する命令の基本サイクル、先行するバスアクセスのバス位相、先行する
  `access_timing` の有効な wait を含む。現在のバスアクセス自身の wait は含まない。
- 同一 `mc68k_execute()` 呼出しで後に発行される外部バスアクセスの値は、前の値より小さく
  ならない。同一時刻を許すのは、資料上またはコアの時刻モデル上で同時刻と定義される
  アクセスだけとする。順序を曖昧にしないため、実装は原則として厳密増加を目指す。
- `cpu == NULL`、`out == NULL`、MC68060 以外、または callback 外では `false` を返す。callback 外での
  時刻問い合わせを暗黙に許可しない。
- `mc68k_reset()` の直後、および `mc68k_execute()` の開始時には有効なバス時刻はない。
  `mc68k_debug_run()` は内部の各 `mc68k_execute(cpu, 1)` を独立した epoch とする。
- bus callback から `mc68k_execute()`、`mc68k_reset()`、`mc68k_destroy()` を再入してはならない。
  この API は再入実行を可能にするものではない。

`mc68k_executed_cycles()` は変更しない。これは命令境界の粗い累積値、
`mc68k_bus_access_cycles()` はバス callback 内だけで有効な細かい累積値、と役割を分離する。

## 4. 時刻モデル

内部状態として、少なくとも次の値を分けて保持する。

| 状態 | 意味 |
| --- | --- |
| 命令境界時刻 | 完了済み命令・例外処理のサイクルと回収済み wait の累積。既存 `mc68k_executed_cycles()` の基礎。 |
| バス位相時刻 | 現在の命令または例外処理の中で、完了した内部位相と外部バス位相の累積。 |
| 有効フラグ | 外部 bus callback の実行中だけバス位相時刻を公開する。 |

外部 bus callback の公開を行う共通 helper を設け、次の順序を全経路で守る。

1. アクセス直前までの CPU 位相をバス位相時刻へ反映する。
2. 有効フラグを立てる。
3. ホストの bus callback を呼ぶ。この期間に API は時刻を返す。
4. 有効フラグを下げる。
5. 当該経路が現行どおり `access_timing` を適用する場合だけ、wait と cacheable を得る。
6. 取得した wait が有効なら、次の CPU/DMA 時刻へ進む量としてバス位相時刻へ加える。

失敗する read/write callback も 1 回のバスアクセスであるため、`false` を返す場合も手順 1〜6 を
実行する。これは現行の `cpu_read_*_access()` / `cpu_write_*_access()` が失敗後も
`cpu_account_bus_cycle()` を呼ぶ契約を維持するためである。CPU が bus callback を呼ばずに検出する
address error、cache hit、変換前の失敗は
外部バスアクセスではなく、API を有効にしない。

### 4.1 経路別の公開と wait 課金

時刻の公開と `access_timing` の課金は別の責務である。既存の execute 戻り値を変えないため、
公開 API の追加を理由に、新しい経路へ wait を課金してはならない。

| 経路 | callback 中の時刻公開 | `access_timing` / wait |
| --- | --- | --- |
| 通常の `read_*` / `write_*`、例外フレーム、ベクタ、MMU、cache | する | 現行どおり適用する。成功・bus error の別を問わない。 |
| 外付けコプロセッサの CIR (`port_read()` / `port_write()`) | する | 現行どおり適用する。 |
| 内蔵 FPU の CIR | しない（ホスト callback がない） | 現行どおり適用し、その有効 wait を内部バス位相時刻へ加える。次の外部 callback はその値を含む。 |
| IACK、68060 の LPSTOP broadcast、BKPT acknowledge | する | 現行どおり適用しない。時刻公開だけで execute 戻り値は変えない。 |
| reset の初期ベクタ読出し | しない | 実行 epoch 外なので適用しない。 |

従って、共通 helper は callback の有効範囲だけを一元化し、wait の有無は各既存経路の
課金規則に従って選ぶ。`cpu_account_bus_cycle()` を時刻モデルへ統合し、外部 callback を持たない
内蔵 FPU CIR を含め、既に計上される wait が次の公開時刻へ必ず反映されるようにする。

### 4.2 命令サイクルの割当て

本 API は「ハンドラ終了時に基本サイクルを一括加算する」現行実装のままでは実現できない。
その方式では、同一ハンドラ内の 1 個目と 2 個目のアクセスの間に何 CPU cycle が経過したかを
根拠付きで決められないためである。

初期の 68060 実装は近似規則 `060-IB-1` を使う。callback 時刻は命令境界までに完了した
cycle と、同じ命令内の先行 callback に適用された wait の合計とする。handler の基本 cycle は
命令終了時に従来どおり加算し、同一命令内に基本 cycle を配分しない。したがって先行 wait が
なければ同一命令内の callback は同時刻である。誤差は当該命令の基本 cycle 以下であり、
命令総 cycle と `mc68k_execute()` の戻り値は変えない。

命令総サイクルしか根拠がないモデルについて、恣意的な中間時刻を公開してはならない。その場合は
一次資料、既存の cycle data、または明示的に隔離した近似規則を設計書へ追加し、対象モデルと
誤差範囲を宣言する。68060 以外の CPU model は、同様に根拠付きの位相規則と regression を
追加するまで API を公開しない。

callback ごとの期待時刻は production code から採取しない。実装時に
`tests/data/cpu/bus_access_timing_expectations.csv` を正本として追加し、各行へ CPU model、最小の命令列または
例外シナリオ、callback 順、期待 CPU cycle、根拠種別、根拠参照を記録する。根拠種別は一次資料または
既存 cycle data のいずれかにし、中間位相を資料から直接得られない場合だけ、設計書に記した近似規則の
識別子を記録する。近似規則は入力（命令形式・バスアクセス列）から各位相を決める式と誤差範囲を持ち、
実装の値を写してはならない。CSV と規則は review の対象とする。

## 5. ホスト側の利用契約

ホストは `mc68k_execute()` 呼出しの直前に、その呼出しの基準時刻を保存する。各 CPU bus callback
の先頭で `mc68k_bus_access_cycles()` を呼び、基準時刻に返値を加えた時刻まで DMAC を進めてから、
CPU の read/write を実行する。

DMA が同じメモリ API を使う場合、DMA 自身の read/write 中は CPU 同期を抑止する guard をホストが
持つ。これは CPU core の責務ではない。core は API 呼出しのためにホスト callback を再帰呼出ししない。

`access_timing` は従来どおり、アクセス終了後の wait 計上と cacheable 判定に使う。DMA 同期を
`access_timing` のみに置いてはならない。

## 6. 実装方針

1. `cpu_internal.h` にバス時刻、callback 有効フラグ、内部位相を保持する内部状態を追加する。
2. `read_*`/`write_*`、外付け CPU 空間、IACK、LPSTOP、BKPT の外部 callback を共通 dispatch
   helper に通す。cache fill/writeback、MMU table walk、例外フレーム、CIR が helper を迂回しない
   ことを確認する。内蔵 FPU CIR は callback を通さないが、wait をバス位相時刻へ反映する。
3. 命令・例外の cycle 計上を、バス位相に割り当てられる形へ段階的に移す。総サイクル値と
   既存の cycle regression を変えない。
4. `cpu.h` に API と callback 内利用の契約を追加し、`test_executed_cycles.c` を時刻 API の
   回帰試験へ拡張する。
5. x68000 側で CPU callback 前同期と DMA 再帰抑止を導入する。

公開 API は `cpu.h` のみへ追加する。`mc68k_bus_t` のフィールド追加ではないため、位置指定
初期化子との ABI 互換性問題を増やさない。

## 7. 互換性と制約

- API を呼ばない既存ホストの動作、`mc68k_executed_cycles()` の値、`mc68k_execute()` の戻り値は
  変更しない。
- API は CPU と DMA の順序を与えるが、DMA コントローラの転送速度、バス競合の電気的詳細、
  ホストのクロック変換そのものは定義しない。
- host callback の実時間は CPU cycle として計上しない。
- `access_timing` が NULL の場合でも、バスアクセスの時刻と順序は公開する。追加 wait は 0 とする。
- API 追加後も、bus callback 内で `mc68k_executed_cycles()` を使う既存コードは従来の粗い値を得る。
- `mc68k_bus_t::sync` の呼出し契約はこの変更では追加しない。絶対時刻を返す本 API と、差分を
  受け取る `sync` を同時に導入すると二重計上の責務が曖昧になるためである。必要になれば、
  host 移行後に `sync` の用途を別設計として定義する。

## 8. テストと受入条件

`tests/cpu/test_bus_access_cycles.c` を追加し、公開ヘッダだけを使って次を確認する。

1. 同一 `mc68k_execute()` 中の opcode fetch、拡張語、read、write が callback 内で API を成功させ、
   時刻が単調に進む。
2. 複数回の `mc68k_execute()` と `mc68k_debug_run()` で epoch が混ざらない。
3. `access_timing` の wait は当該アクセスには含まれず、後続アクセスには 1 回だけ含まれる。
   bus error を返したアクセスの wait も、直後の例外フレームの時刻へ 1 回だけ含まれる。
4. バスエラー、address error、例外フレーム、ベクタ読出し、IRQ/IACK について、有効化の有無と
   時刻順序が契約どおりである。IACK の bus error による spurious interrupt も含める。
5. 68020+ の非整列分割アクセス、68030 MMU table walk、68040 cache fill/writeback、CPU 空間
   callback、68060 LPSTOP/BKPT が共通 dispatch を迂回しない。IACK、LPSTOP、BKPT は
   `access_timing` の wait を新規に計上しないことも確認する。
6. API を使わない場合の既存 cycle regression と `mc68k_executed_cycles()` の期待値が変わらない。
7. `cpu == NULL`、`out == NULL`、callback 外では `false` を返し、`out` を変更しない。reset の raw
   読出しでは API が有効にならない。
8. `tests/data/cpu/bus_access_timing_expectations.csv` の callback ごとの期待時刻を照合する。少なくとも、
   複数 bus callback を持つ代表命令、例外フレームとベクタ読出し、68030 MMU table walk、68040 cache
   fill/writeback を対象にする。期待値は production code から作らず、§4.2 の一次資料・既存 cycle data
   または明示近似規則から導く。アクセス数で基本 cycle を等分する変異は、この照合で必ず失敗する。

内部状態を検証する試験は、公開 API の試験と分離して `src` を include する既存形式の内部試験に
置く。こちらで内蔵 FPU CIR の wait と、対象 CPU モデルごとのバス位相合計および
`mc68k_execute()` 戻り値の整合を確認する。公開ヘッダだけを使う試験から内部位相を直接検査しない。

上記に加え、x68000 側では CPU と DMA が同一メモリ位置へアクセスする小さな回帰を作り、
CPU callback 前に DMA が所定時刻まで進むこと、DMA 自身のメモリアクセスが再帰同期しないことを
確認する。

## 9. 却下する案

| 案 | 却下理由 |
| --- | --- |
| `mc68k_executed_cycles()` の意味を命令途中の値へ変更する | 既存の命令境界契約を破る。 |
| `access_timing` 内だけで DMA を同期する | 実アクセス後なので、CPU/DMA のメモリ順序を保証できない。 |
| 命令終了時に `bus.sync` を 1 回だけ呼ぶ | 同一命令内の複数アクセスを順序付けできない。 |
| 命令総サイクルをアクセス数で等分する | CPU モデル・命令・例外の根拠がなく、見かけ上の時刻を公開してしまう。 |

## 10. 根拠と対応箇所

- 公開 API: `include/mc68000/cpu.h` の `mc68k_executed_cycles()`。
- 外部バス callback と `access_timing`: `include/mc68000/bus_hooks.h`。
- 実行ループと命令単位の cycle 計上: `src/cpu/m68000.c` の `execute_body()`。
- 外部メモリアクセスと wait 計上: `src/cpu/cpu_internal.h` の
  `cpu_read_*_access()`、`cpu_write_*_access()`、`cpu_account_bus_cycle()`。
- 現行の命令境界時刻の回帰: `tests/cpu/test_executed_cycles.c`。
- 命令 cycle の現行方針: `docs/mc68000-internals/reference/cycle_timing.md`。
