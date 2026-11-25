# アーキテクチャ

`cmc68000` は、ホストが提供するバスと実行ループに接続する C99 の静的ライブラリです。
CPU はメモリマップ、スレッド、UI、周辺機器を所有しません。公開プロダクト名は
`cmc68000` ですが、現行の CMake ターゲット名と C API 接頭辞は `mc68000`／`mc68k_*` です。

## 境界

```text
ホストアプリケーション
  ├─ メモリ、ROM、I/O、DMA、IRQ、フレーム時間を所有
  ├─ mc68k_bus_t callback を実装
  └─ mc68k_execute() / mc68k_debug_run() を駆動
                 │
                 ▼
cmc68000
  ├─ 公開 API: include/mc68000/
  ├─ CPU core: フェッチ、decode、命令実行、例外、IRQ、MMU/cache
  ├─ FPU: 外付け／内蔵 FPU の命令モデルと共通演算コア
  └─ Debug: 実行制御、観測、逆アセンブル、履歴リング
                 │
                 ▼
ホストの bus callback
  └─ read/write、IACK、タイミング、CPU 空間アクセス
```

`mc68k_state_t` は不透明型です。ホストは公開 API だけを通じて CPU 状態を生成、実行、観測し、
`src/` の内部ヘッダや内部構造に依存しません。

## 公開 API

| ヘッダ | 責務 |
| --- | --- |
| `cpu.h` | CPU の生成、reset、実行、IRQ、TRAP hook、状態取得 |
| `cpu_types.h` | CPU モデル、FPU 構成、レジスタ snapshot、実行状態 |
| `bus_hooks.h` | ホストが実装する bus callback とタイミング情報 |
| `debug.h` | ブレークポイント、watchpoint、非侵襲 read、実行履歴、逆アセンブル |
| `fpu.h` | 浮動小数点状態と共通演算コア |

CPU モデルと FPU 構成は生成時に固定されます。実行中に CPU モデルを切り替える API はありません。
`mc68k_create_model_with_fpu()` は、モデルと FPU 構成の受理表に合わない組合せでは `NULL` を返します。

## CPU core

`src/cpu/` は CPU の実行状態と命令セットを所有します。

- `m68000.c` はライフサイクル、実行ループ、IRQ 受付、例外配送、公開 CPU API を実装する。
- `decode.c` はモデルごとの opcode table を構築する。命令モジュールは opcode handler を登録する。
- `ops_*.c` は命令の実行を担当する。世代固有の命令、MMU、cache、FPU の差分は専用モジュールに置く。
- `ea_helpers.c` は実効アドレスの解決と読み書きの共通処理を提供する。
- `cycles.c` は命令と実効アドレスのサイクル値を解決する。
- `coproc.c` は 68020/68030 の外付けコプロセッサ用 CPU 空間インタフェースを扱う。

CPU core はバスを直接実装しません。命令フェッチ、データアクセス、例外フレーム、ベクタ読出しは
`mc68k_bus_t` を通じてホストへ委譲します。MMU を持つモデルで通常の memory callback に渡る
アドレスは、変換後の物理アドレスです。

## FPU

`src/fpu/` は浮動小数点の値表現、演算、例外、命令 decode、命令実行、frame を分離します。

- `fpu_core.c` と演算モジュールは、80-bit 拡張精度値と丸め・例外の共通処理を提供する。
- `fpu_model.c`、`fpu_decode.c`、`fpu_exec.c` は CPU モデルごとの FPU 命令経路を担当する。
- 外付け 68881/68882 は CPU 空間 callback を介して接続できる。
- 68040/68060 の内蔵 FPU は CPU が所有する専用状態として実行する。

FPU 共通コアは Berkeley SoftFloat を内部依存として使用します。利用側は SoftFloat の API へ
直接依存しません。

## Debug

`src/debug/` は、CPU の通常実行経路を置き換えずに観測と停止制御を追加します。

- `debugger.c` は `mc68k_debug_run()`、ブレークポイント、watchpoint、非侵襲 read、
  fault/PC/register/call ring を実装する。
- `disasm.c` は peek hook を使って副作用なしに命令を逆アセンブルする。

デバッグ API は CPU 実行と内部同期しません。同じ CPU インスタンスへの実行、reset、状態変更、
デバッグ API 呼出しは、ホストが直列化します。

## テスト

`tests/` は実行可能な仕様です。

| 層 | 内容 |
| --- | --- |
| `tests/memory/` | メモリ・bus の基本契約 |
| `tests/cpu/` | 命令、例外、IRQ、モデル差分、MMU、cache、cycle の回帰 |
| `tests/fpu/` | FPU 演算、例外、frame、モデル別の実行 |
| `tests/diff/` | 参照コアとの差分検証 |

CMake プロジェクトの最上位としてビルドした場合だけ、テストを有効化します。別プロジェクトから
`add_subdirectory()` で組み込む場合は、ライブラリ本体だけをビルドします。

## 設計資料との関係

モデル固有の動作、例外フレーム、MMU、cache、FPU の詳細は `docs/mc68000-internals/design/` に記載します。
命令やモデルの対応範囲は `docs/mc68000-internals/support/` に集約し、この文書には重複させません。
