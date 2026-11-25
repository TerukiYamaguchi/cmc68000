# FPU

## 1. 目的と対象範囲

FPU 共通コア、68881/68882 の外付け接続、68040 内蔵 FPU の状態・演算・例外・frame を定義する。
CPU モデル別の命令可用性は各 CPU 設計書と対応表で扱う。

## 2. 実装状態

`fpu.h` は 80-bit 拡張精度値、変換、丸め、算術、比較、packed decimal、超越関数、FPU model
protocol の公開 API を提供する。CPU 作成時の FPU 構成は固定である。

## 3. 設計の要点

| 決定 | 理由 | 検証 |
| --- | --- | --- |
| 数値演算を CPU 命令実行から分離する | 複数の FPU 接続形態で同じ演算規則を使う | `test_fpu_core`、`test_fpu_arith` |
| 外付け FPU は CPU 空間 CIR を通す | 68020/030 のコプロセッサ境界を再現する | `test_fpcp_host` |
| 内蔵 FPU は CPU 所有状態にする | 外付け CIR を経由しない構成を分離する | `test_fpu040_exec` |

## 4. 構成とデータモデル

`mc68k_fp80_t` は符号・指数と 64-bit 仮数からなる内部拡張精度値である。`mc68k_fpu_state_t` は
FP0–FP7、FPCR、FPSR、FPIAR を保持する。SoftFloat target 層は拡張精度演算を補助する内部依存であり、
利用側は直接使用しない。

## 5. インターフェース、レジスタ、入力

`fpu.h` は整数／S／D／X／packed 形式との変換、四則演算、比較、丸め、例外 flag を公開する。
FPU 構成は `mc68k_create_model_with_fpu()` で選択する。68020/030 は NONE/68881/68882、68040 は
NONE/68040/NONE_040 を受理する。

## 6. 動作仕様

演算は FPCR の丸めモード・丸め精度を参照し、FPSR の例外・条件コードを更新する。各命令の
backend は命令開始時に必要な例外状態を初期化し、共通演算コアの結果を CPU 例外経路へ接続する。

68881/68882 は CPU 空間 callback の CIR protocol を介して command、operand、save/restore を
転送する。68040 の内蔵 FPU は F-line entry から直接実行し、FSAVE/FRESTORE で null、idle、
unimplemented、busy frame を扱う。frame 転送中の bus error は CPU の group 0 例外として扱う。

## 7. 実装上の対応

| モジュール | 責務 |
| --- | --- |
| `fpu_core.c` と演算モジュール | 値変換、丸め、算術、比較、超越関数 |
| `fpu_model.c`、`fpu_proto.c` | 外付け FPU state machine と CIR protocol |
| `fpu_decode.c`、`fpu_exec.c`、`fpu_frame.c` | 命令 decode、実行、FSAVE/FRESTORE |
| `coproc.c` | CPU 空間 callback との接続 |
| `ops_68040_fpu.c` | 68040 内蔵 FPU の CPU 側 entry |

## 8. 互換性と既知の制約

FPU 構成の受理表は CPU モデルごとに異なる。内蔵 FPU と外付け FPU は同じ数値演算コアを使うが、
命令入口、例外 frame、save/restore frame は異なる。SoftFloat のグローバル状態を使うため、
同一プロセスでの FPU 演算の同時実行はサポートしない。

## 9. テストと受入条件

| テスト群 | 検証対象 |
| --- | --- |
| `test_fpu_core`、`test_fpu_arith`、`test_fpu_exceptions` | 演算、丸め、例外 |
| `test_fpu_model_*` | 外付け FPU protocol、frame、decode、状態遷移 |
| `test_fpcp_model`、`test_fpcp_host` | CIR 接続 |
| `test_fpu040_*` | 68040 内蔵 FPU、frame、未実装命令 |

## 10. 根拠資料

### 一次資料

- MC68881/MC68882 User's Manual: データ形式、演算、例外、CIR、frame。
- MC68040 User's Manual: 内蔵 FPU と例外 frame。

### 参照実装・検証データ

- `tests/fpu/` と Berkeley SoftFloat 3e。

### ライセンスと出典

- Berkeley SoftFloat 3e は BSD-3-Clause。ライセンス本文は `LICENSES/` に置く。
