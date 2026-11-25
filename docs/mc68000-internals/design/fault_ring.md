# フォルト記録

## 1. 目的と対象範囲

fault ring は group 0 fault を診断用に記録する固定長リングである。通常の実行結果や例外処理を
変更せず、bus error と address error の原因追跡を支援する。

## 2. 実装状態

公開 record と取得 API は `debug.h` を正とする。既定では無効であり、
`mc68k_set_fault_ring()` で有効化する。

## 3. 設計の要点

| 決定 | 理由 |
| --- | --- |
| 固定長循環バッファ | 実行中の割当てを避け、直近の fault を保持する |
| 最初の fault 属性を記録する | frame 構築中の二重 fault と原因を混同しない |
| 構造化 API を正とする | dump 出力形式へ利用側を依存させない |

## 4. 構成とデータモデル

`mc68k_fault_ring_entry_t` は命令開始 PC、fault address、opcode、vector、frame format、
function code、access kind、size、read/write、RMW、MMU 結果、配送結果を保持する。

## 5. インターフェース、レジスタ、入力

`mc68k_set_fault_ring()`、`mc68k_get_fault_ring()`、`mc68k_read_fault_ring()`、
`mc68k_dump_fault_ring()` が公開 API である。容量は `MC68K_FAULT_RING_SIZE` で固定する。

## 6. 動作仕様

group 0 dispatch の直前に record を追加する。容量を超えると古い record から上書きする。無効化は
採取を止めるだけで既存履歴を消さず、reset も履歴・有効件数・設定を変更しない。

## 7. 実装上の対応

`m68000.c` が fault 属性をラッチして record を作成し、`debugger.c` が公開取得・dump API を提供する。

## 8. 互換性と既知の制約

MMU 関連フィールドは対応モデル・fault 種別に応じてだけ有効である。frame 構築中の二重 fault の
属性は record に含めない。

## 9. テストと受入条件

`test_fault_ring` と `test_ring_read` が vector、frame format、access 属性、容量、read 順序、
reset 後の保持を検証する。

## 10. 根拠資料

### 一次資料

- 各 CPU User's Manual の group 0 exception frame。

### 参照実装・検証データ

- `tests/cpu/test_fault_ring.c`、`tests/cpu/test_ring_read.c`。

### ライセンスと出典

- record の値はプロジェクトの公開 debug API 契約に従う。
