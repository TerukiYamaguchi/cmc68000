# mc68000

Claude Code / Codex 共用の必須ルール。詳細な手順・実装規約は
[`docs/mc68000-internals/agent_workflow.md`](docs/mc68000-internals/agent_workflow.md) を必要時に読む。`CLAUDE.md` は本ファイルを取り込む。

## 必須ルール

- 作業開始時に `git fetch origin`、`git log HEAD..origin/main`、最新の
  `local/handoff/handoff_YYYYMMDD.md` を確認する。同期前に commit/push しない。
- 指示が曖昧、または未承認の設計判断が必要なら、実装前に確認する。通常の実装・修正・完了作業は自律的に進める。
- `git push`、履歴改変、破壊的・復元不能な操作、外部公開、スコープ不明の副作用は明示的な承認なしに行わない。
- 公開 API は `include/mc68000/{cpu.h,cpu_types.h,bus_hooks.h,debug.h}` のみ。上位プロジェクトから `src/` の内部ヘッダを include しない。
- 変更後は警告なしでビルドし、変更に対応するテストを実行する。テストの削除・スキップ・無効化で通過させない。
- ソースコードのコメントは日本語で書く（識別子・コマンド・エラー文言は英語のまま）。既存の英語コメントは、
  触れた箇所だけ日本語に直す。一括変換はしない。
- 命令、EA、例外、CPU 状態、デコード、表添字、バッファ、シフトを変更する前に、詳細規約と既存の近接実装・試験を読む。
- AI レビュー、変異試験、FPU-2〜4、レビュー Issue、oracle コメントを扱う前に、必ず
  [`docs/mc68000-internals/agent_review_workflow.md`](docs/mc68000-internals/agent_review_workflow.md) を読む。

## 詳細文書の入口

- 日常の作業手順、build/test、実装不変条件、設計書・引継ぎの扱い:
  [`docs/mc68000-internals/agent_workflow.md`](docs/mc68000-internals/agent_workflow.md)
- CPU/命令の責務: `src/cpu/`、`include/mc68000/` と近接する実装・試験
- 例外、trace、IRQ: 対応する `docs/mc68000-internals/design/`・`reference/` と既存試験
