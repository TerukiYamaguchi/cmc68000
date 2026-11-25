# エージェント作業詳細

`AGENTS.md` の必須ルールを補足するオンデマンド文書。作業内容に該当する節だけ読む。

## 開始・引継ぎ・協業

- 開始時は `git fetch origin`、続けて `git log HEAD..origin/main` を確認する。上位 x68000
  リポジトリからも直接 push されるため、古い HEAD のまま commit/push してはならない。
- 最新の `local/handoff/handoff_YYYYMMDD.md` を読む。引継ぎは1日1ファイルとし、同日の追記は同じ
  ファイルの `## 追記（同日Nセッション目）` に置く。日付変更時は過去分を `local/handoff/` へ置く。
- 実装・修正・完了依頼の範囲では、通常の読取り、編集、整形、build/test、commit、Issue 更新・close を
  自律的に行う。進捗照会やレビュー通知は、明示的な中止でない限り作業停止の理由にしない。
- 新機能は GitHub Issue を作成する。x68000 側の対応が要る変更は、完了まで close しない。

## Build と試験

- `build` はリポジトリ直下のローカルな実ディレクトリとし、Git に追加しない。
- 初回の configure、build、全試験は次を使う。

  ```sh
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```

- 表の添字、バッファ、シフトに関わる変更は、別ディレクトリで ASan/UBSan を有効化して対象試験を流す。
  `tests/diff` の Musashi 生成コードは対象外。macOS で変異後の再buildを確認する際は `.o` と実行
  ファイルを消してから取り直す。
- CPU 試験は RAM に生バイナリを置き、`assert` で検証し、末尾の `ILLEGAL (0x4AFC)` で停止させる。

## CPU 実装規約

- 命令ハンドラは `int (*)(mc68k_state_t *, uint16_t)` で消費サイクル数を返す。デコードは 65536
  エントリのフラット関数ポインタ表。ハンドラは `ops_<category>.c`（無印は 68000、世代別は
  `ops_<世代>_<category>.c`）。
- `decode.c` の opcode 登録順序は変更しない。新規命令・EA・例外は近い実装と `cpu_internal.h` を先に確認する。
- MOVE 宛先 EA は mode/reg の配置が source と逆（dst: bits 11-9 = reg、8-6 = mode）。A7 byte の
  pre/post increment は `sp_byte_step()` により2 byte。モード切替は A7 と SSP/USP を自動退避・復帰する。
- word/long の奇数アクセスは address error（vector 3）として `CPU_EXCEPTION` を設定して脱出する。
  未マップの bus error は host 側で `mc68k_signal_bus_error()` を使う。Group 0 は
  `dispatch_group0_exception()`、simple frame は `cpu_dispatch_exception()` を使う。
- 特権命令は副作用より先に `cpu_privilege_violation()` を行う。68000 で MOVE from SR と
  `#imm,CCR` は非特権。IRQ は命令境界だけで受理し、`mc68k_set_irq()` は pending state のみ更新する。

## 文書と参照先

- `docs/mc68000-internals/design/`・`reference/` のコード参照は行番号でなく、関数名と grep で一意な短いアンカーを使う。本文は
  現行設計と根拠、変更経緯・却下案・不具合記録は `## 履歴` に置く。
- 命令分類・モジュール責務は `src/cpu/` と `include/mc68000/`、デバッグ API は
  `include/mc68000/debug.h` と `src/debug/`、RAM/ベクタ設定は対応する `tests/cpu/test_*.c` を読む。
- 例外・trace・IRQ の詳細は対応する `docs/mc68000-internals/design/`・`reference/` と既存試験を読む。
