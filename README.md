# cmc68000

C99 で実装した Motorola 68000 系 CPU エミュレータの静的ライブラリです。ホストが提供する
bus callback を通じて実行するため、特定のメモリマップ、OS、UI、周辺機器に依存しません。

公開プロダクト名は `cmc68000` です。現行の CMake ターゲット名は `mc68000`、C API 接頭辞は
`mc68k_*` です。

## 導入

```cmake
add_subdirectory(path/to/cmc68000)
target_link_libraries(your_target PRIVATE mc68000)
```

CPU を生成し、bus callback を設定して reset・実行します。最小の動作例は
[Getting Started](docs/integration/getting_started.md) を参照してください。

## 対応モデル

CPU モデルは生成時に選択します。

- MC68000
- MC68010
- MC68020
- MC68EC030 / MC68030
- MC68040
- MC68060（MMU、未実装命令 trap、LPSTOP、PLPA、format `$4` 例外フレーム、命令時間に対応）

MC68LC060 / MC68EC060 は未対応です。モデル別の命令・機能の対応状況は
[`docs/mc68000-internals/support/instruction_support.md`](docs/mc68000-internals/support/instruction_support.md) を参照してください。

## ビルド

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

FPU 共通コア（`src/fpu/`）は Berkeley SoftFloat を `extern/berkeley-softfloat-3` の git submodule
として使う。取得していない場合は `git submodule update --init extern/berkeley-softfloat-3`
を実行する（CMake が未取得を検出してエラーにする）。

## ドキュメント

| 対象 | 文書 |
| --- | --- |
| 組込み | [Getting Started](docs/integration/getting_started.md)、[バス](docs/integration/bus.md)、[実行](docs/integration/execution.md) |
| IRQ と例外 | [割り込みと例外](docs/integration/interrupts_exceptions.md) |
| ホスト側デバッグ | [デバッグ](docs/integration/debugging.md) |
| 開発 | [アーキテクチャ](docs/mc68000-internals/architecture.md)、[ビルド](docs/mc68000-internals/build.md)、[公開 API](docs/mc68000-internals/api.md) |

## 公開 API

ヘッダは `include/mc68000/` 以下にある。

| ヘッダ | 内容 |
|--------|------|
| `cpu.h` | CPU の生成・破棄・reset・実行、IRQ/TRAP、モデル・レジスタ取得、スナップショット、バスアクセス時刻照会 |
| `cpu_types.h` | `mc68k_state_t`（不透明型）、CPU/FPU モデル、レジスタ・MMU レジスタのスナップショット、実行状態、callback 型 |
| `bus_hooks.h` | `mc68k_bus_t`：read/write、DMA 同期、IACK、周辺機器 reset、アクセス時刻、CPU 空間（FC=7）callback |
| `debug.h` | ブレークポイント、watchpoint、実行制御、逆アセンブル、実行履歴・フォールト記録 |

## 上位プロジェクト向け実装ガイド

### TRAP フック（IOCS / DOS コールトレーサー）

`mc68k_set_trap_handler()` で TRAP 命令発生時のコールバックを登録できる。
X68000 の IOCS コール（`TRAP #15`）や DOS コール（`TRAP #2`）のトレーサーは
**上位プロジェクト側**で実装する。

```c
/* 上位プロジェクト（X68000エミュレータ）側での実装例 */

static const char *iocs_name(int func) { /* ... */ }

static int iocs_tracer(mc68k_state_t *cpu, int trap_num, void *ctx)
{
    if (trap_num != 15) return 0;          /* TRAP #15 以外はスルー */

    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);
    int func = (int16_t)(regs.d[0] & 0xFFFF);

    fprintf(stderr, "[IOCS] %s (D0=%04X)\n", iocs_name(func), func);
    return 0; /* 0 = 通常の例外ベクタ処理へ、非0 = ライブラリ側で処理済み */
}

/* 初期化時に登録するだけ */
mc68k_set_trap_handler(cpu, iocs_tracer, NULL);
```

コールバックが非0を返すと例外ベクタには飛ばず次命令から再開する。
IOCS/DOS コール番号テーブルは上位プロジェクトで管理する。

### IRQ

```c
mc68k_set_irq(cpu, 6); /* IRQ レベル 6 をアサート */
mc68k_set_irq(cpu, 0); /* IRQ をネゲート */
```

割り込みは命令境界でのみ受け付ける。`mc68k_execute()` 内で処理される。

### バスエラー（未マップアドレスアクセス）

mc68000 コアはメモリマップを一切持たず、バスコールバック（`read_byte` 等）に
処理を委譲する。そのため「アクセス先にデバイスが無い」判定は上位プロジェクト
側でしか行えない。通常の read/write コールバックは、該当アドレスを検出したら
`false` を返してバスエラーを直接通知する。

```c
/* 上位プロジェクト（X68000エミュレータ）側での実装例 */

bool x68k_read_byte(void *ctx, uint32_t addr, uint8_t *out)
{
    if (!in_any_mapped_region(addr)) {
        return false; /* CPU がベクタ2へ例外ディスパッチする */
    }
    /* ... 通常のマップ済み領域アクセス ... */
    *out = mapped_read_byte(addr);
    return true;
}
```

`read_byte` / `read_word` / `read_long` / `write_byte` / `write_word` / `write_long` は
いずれも `false` でバスエラーを通知する。read 系の値は `out` 引数で返すため、
`false` の場合は設定不要である。アドレスエラー（ベクタ3、奇数アドレスアクセス）とは
別のベクタ（ベクタ2）にディスパッチされる。

`mc68k_signal_bus_error()` は通常のread/writeコールバックには使用しない。IACK
（割り込み確認）コールバック中に、どのデバイスも応答せずスプリアス割り込みとして
扱う必要がある場合だけ呼ぶ。

## 参考資料

### 一次資料

| 資料 | 用途 |
|------|------|
| [M68000 Family Programmer's Reference Manual (Motorola, Rev.1)](https://www.nxp.com/docs/en/reference-manual/M68000PRM.pdf) | 命令エンコーディング・CCR フラグ仕様・例外処理の根拠 |
| [MC68000 User's Manual, 8th Edition](https://www.nxp.com/docs/en/reference-manual/MC68000UM.pdf) | タイミング・バスサイクル仕様 |

### 参照実装・テスト

| 名称 | 用途 |
|------|------|
| [Musashi](https://github.com/kstenerud/musashi) (Karl Stenerud) | 差分テスト (`tests/diff/`) の参照実装。MAME で長年使用されてきた C99 製 M68000 エミュレータ。`tests/diff/musashi/` に git submodule として取り込んでいる |

### サードパーティライセンス

| 名称 | ライセンス | 用途 |
|------|-----------|------|
| [Berkeley SoftFloat 3e](https://github.com/ucb-bar/berkeley-softfloat-3) (John R. Hauser) | BSD-3-Clause（全文は `LICENSES/SoftFloat-BSD-3-Clause.txt`） | FPU 共通コアの浮動小数点演算基盤。`extern/berkeley-softfloat-3` に git submodule として取り込み、`src/fpu/softfloat_target/` に MC68881 向けの差し替え実装を置く |

### 参考にしたエミュレータ実装

| 名称 | 参考箇所 |
|------|---------|
| [MAME 68000 core](https://github.com/mamedev/mame) | 命令サイクル数・フラグ計算の挙動確認 |
| [XEiJ](https://stdkmd.net/xeij/) (Java 製 X68000 エミュレータ) | X68000 固有挙動の確認・将来の互換性検証の基準候補 |

## ライセンス

配布＆改造自由のフリーウェアです。ソースの引用＆組み込みも自由で、連絡は不要です。無保証です。全文は[`LICENSE`](LICENSE)を参照してください。サードパーティのコード・データは、それぞれのライセンスに従います。
