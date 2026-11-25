# 公開 API

## 方針

公開 API は `include/mc68000/` に限定する。`mc68k_state_t` は不透明型であり、利用側は
内部ヘッダや `src/` の構造体に依存してはならない。

| ヘッダ | 内容 |
| --- | --- |
| `cpu.h` | ライフサイクル、実行、IRQ、TRAP hook、状態取得 |
| `cpu_types.h` | モデル、FPU 構成、レジスタ snapshot、実行状態 |
| `bus_hooks.h` | ホスト bus callback |
| `debug.h` | ホスト側デバッグ機能 |
| `fpu.h` | FPU 共通コア |

## ライフサイクル

`mc68k_create()` または `mc68k_create_model()` で CPU を生成し、`mc68k_reset()` 後に
`mc68k_execute()` を呼ぶ。不要になった CPU は `mc68k_destroy()` で破棄する。
モデルと FPU 構成は生成時に固定され、実行中の変更はできない。

## API 互換性

公開ヘッダの関数・型・列挙値・callback 契約は互換性の対象である。公開構造体にフィールドを
追加する場合、利用側が指示付き初期化子またはゼロ初期化を使えるよう、既存フィールドの意味と
順序を維持する。

API の詳細な使い方は [統合ガイド](../integration/getting_started.md) を参照する。内部モジュールの
設計は [アーキテクチャ](architecture.md) を参照する。
