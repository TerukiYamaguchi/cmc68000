# 独立公開時のドキュメント移行計画

## 目的

`cmc68000` の開発完了後、CPU エミュレータを独立した GitHub リポジトリとして公開する。
組込み利用者がライブラリを導入できる利用者向け資料と、設計・検証の根拠を追跡できる
開発資料を、いずれも公開する。

新リポジトリは完成時点の現行仕様だけを収録する。旧リポジトリの Issue 番号、PR 番号、
コミット ID、ブランチ名、日次引継ぎ、調査途中の履歴は持ち込まない。

## 名称

公開プロダクト名および将来作成する GitHub リポジトリ名は `cmc68000` とする。
名称は「C 言語で書いた 68000」を表す。

現行ソースツリー名、CMake ターゲット名、公開 C API の `mc68k_*` 接頭辞は、別途の
互換性を含む変更判断があるまで `mc68000` のまま扱う。本計画書および公開文書の整備は、
それらの名称変更を伴わない。

## 公開ドキュメント構成

`x68000/docs/` の構成を基準に、次のツリーを新リポジトリの正本とする。

```text
README.md
docs/
├── develop/
│   ├── architecture.md
│   ├── build.md
│   ├── api.md
│   ├── design/
│   │   ├── _TEMPLATE.md
│   │   ├── cpu_68000.md
│   │   ├── cpu_68010.md
│   │   ├── cpu_68020.md
│   │   ├── cpu_68030.md
│   │   ├── cpu_68040.md
│   │   ├── cpu_68060.md
│   │   ├── fpu.md
│   │   ├── cycle_timing.md
│   │   ├── debugger.md
│   │   └── fault_ring.md
│   ├── support/
│   │   ├── instruction_support.md
│   │   ├── model_support.md
│   │   ├── fpu_support.md
│   │   └── debugger_support.md
│   └── data/
│       └── *.csv
└── integration/
    ├── getting_started.md
    ├── bus.md
    ├── execution.md
    ├── interrupts_exceptions.md
    ├── debugging.md
    └── compatibility.md
examples/
tests/
LICENSES/
```

## 文書の役割

- `README.md` はライブラリの目的、対応CPU、最小の導入経路を示す入口とする。
- `docs/integration/` は組込み利用者向けのAPI契約を扱う。バスcallback、実行、IRQ、
  例外、デバッグ、互換性を、内部実装に依存せず説明する。
- `docs/mc68000-internals/architecture.md` は公開API境界、状態の所有、バスcallback境界、CPU・FPU・
  デバッグの責務、テスト層を俯瞰する。
- `docs/mc68000-internals/design/` は現行設計の正本とする。設計判断、動作仕様、実装箇所、制約、
  受入条件、根拠資料を記す。
- `docs/mc68000-internals/support/` は現行の実装対応状況の正本とする。モデル・命令・FPU・
  デバッグ機能の対応範囲を表で示し、設計の説明を重複させない。
- `docs/mc68000-internals/data/` は設計とテストの根拠となるCSV等を置く。各データは、用途と
  検証方法を設計書から参照可能にする。
- `tests/` は実行可能な仕様とする。設計書は対象テストを参照し、テストは対応する
  設計書をコメントで参照する。

## 設計書の共通構成

`x68000/docs/mc68000-internals/design/_TEMPLATE.md` を基に、各設計書は次を含める。

1. 目的と対象範囲
2. 実装状態
3. 設計の要点
4. 構成とデータモデル
5. インターフェース、レジスタ、入力
6. 動作仕様
7. 実装上の対応
8. 互換性と既知の制約
9. テストと受入条件
10. 根拠資料（一次資料、参照実装、実測・検証データ、対応テスト）

Issue への参照欄と更新履歴は設けない。設計判断は、番号を参照せず、決定・理由・
影響・検証を自己完結して記述する。

## 現行資料からの移行

| 現在の資料 | 移行先 |
| --- | --- |
| `docs/mc68000-internals/reference/cpu_68000.md` から `docs/mc68000-internals/reference/cpu_68060.md` | `docs/mc68000-internals/design/cpu_*.md` |
| `docs/mc68000-internals/reference/fpu.md` | `docs/mc68000-internals/design/fpu.md` |
| `docs/mc68000-internals/reference/cycle_timing.md` | `docs/mc68000-internals/design/cycle_timing.md` |
| `docs/mc68000-internals/reference/debugger.md` | `docs/mc68000-internals/design/debugger.md` |
| `docs/mc68000-internals/reference/fault_ring.md` | `docs/mc68000-internals/design/fault_ring.md` |
| `notes/INSTRUCTION_SUPPORT.md` | `docs/mc68000-internals/support/instruction_support.md` |
| `notes/*.csv` の検証・分類データ | `tests/data/cpu/`・`tests/data/fpu/` |

移行時には、公開ヘッダと実装・テストの一致を確認する。対応状況は公開API、実装、
テストで確認できる現行の状態だけを記載する。

## 持ち込まない資料

- `local/handoff/handoff_*.md`
- Issue、PR、コミット、ブランチを参照する記述
- 調査途中の仮説、旧案、レビュー会話、単発不具合の作業記録
- AIエージェントの作業手順そのもの

これらに含まれる現行仕様に必要な判断は、番号や経緯への依存を除去し、対応する
設計書へ統合する。

## 公開前の完了条件

- 公開ヘッダ、実装、対応状況表、テストの記載が一致している。
- 組込み利用者が `docs/integration/` と `examples/` だけでCPUを初期化・実行できる。
- 各設計書が対象、制約、根拠、対応テストを自己完結して示している。
- 全文書・CSV・コメントから旧リポジトリ固有の番号と識別子を除去している。
- 外部資料、参照実装、サードパーティコードのライセンスと出典を明記している。

## 作成進捗

本節を公開ドキュメント作成の進捗に関する正本とする。公開対象の文書そのものには、
Issue、PR、コミット、作業途中の状態を記載しない。

状態は次の順で更新する。`保留` は外部の実装・検証完了を待つ文書にだけ使い、作業再開時は
`未着手` に戻す。

1. `未着手`: 文書をまだ作成していない。
2. `下書き`: 内容を作成中、または作成後の自己確認が未完了である。
3. `レビュー待ち`: 作成者の自己確認を終え、内容レビューを依頼できる状態である。
4. `完了`: レビューで指摘された修正を反映し、完了条件をすべて満たした。

各文書は `下書き` の後にレビューを行い、その結果を反映してから `完了` とする。各文書を
`完了` とするには、次をすべて満たす必要がある。

- 公開ヘッダ、実装、テストと記載が一致している。
- 旧リポジトリ固有の番号・識別子および作業履歴を含まない。
- 一次資料、ライセンス、対応テストを必要な範囲で参照できる。
- 文書内リンクとコード例を確認している。

| 文書 | 状態 | 68060 依存 | 完了条件 | 根拠・確認対象 |
| --- | --- | --- | --- | --- |
| `README.md` | 下書き | なし | 目的、対応モデル、最小導入、ライセンスへの導線を記載する | 公開ヘッダ、CMake、`LICENSES/` |
| `docs/integration/`<br>`getting_started.md` | レビュー待ち | なし | 初期化から実行までの最小例を示し、例がビルドできる | `cpu.h`、`examples/` |
| `docs/integration/`<br>`bus.md` | 下書き | なし | 全 bus callback の契約と失敗時の扱いを記載する | `bus_hooks.h`、関連テスト |
| `docs/integration/`<br>`execution.md` | 下書き | なし | reset、execute、停止、TRAP の利用側契約を記載する | `cpu.h` |
| `docs/integration/`<br>`interrupts_exceptions.md` | 下書き | なし | IRQ、IACK、例外の利用側責務を記載する | `cpu.h`、`bus_hooks.h` |
| `docs/integration/`<br>`debugging.md` | 下書き | なし | 公開 debug API と利用方法を記載する | `debug.h` |
| `docs/integration/`<br>`compatibility.md` | 未着手 | なし | 対応モデル、既知の制約、互換性検証範囲を記載する | support 文書、統合テスト |
| `docs/mc68000-internals/`<br>`architecture.md` | 下書き | なし | 公開 API 境界と CPU、FPU、bus、debug の責務を記載する | 公開ヘッダ、`src/` |
| `docs/mc68000-internals/`<br>`build.md` | 下書き | なし | ビルド、依存関係、テスト実行を記載する | `CMakeLists.txt`、CTest |
| `docs/mc68000-internals/`<br>`api.md` | 下書き | なし | 公開ヘッダの境界と互換性方針を記載する | `include/mc68000/` |
| `docs/mc68000-internals/design/`<br>`_TEMPLATE.md` | 下書き | なし | 設計書の共通構成と記載規約を固定する | 本計画書「設計書の共通構成」 |
| `docs/mc68000-internals/design/`<br>`cpu_68000.md` | 下書き | なし | 現行設計を自己完結した公開用設計書へ移行する | `docs/mc68000-internals/reference/cpu_68000.md`、実装、テスト |
| `docs/mc68000-internals/design/`<br>`cpu_68010.md` | 下書き | なし | 現行設計を自己完結した公開用設計書へ移行する | `docs/mc68000-internals/reference/cpu_68010.md`、実装、テスト |
| `docs/mc68000-internals/design/`<br>`cpu_68020.md` | 下書き | なし | 現行設計を自己完結した公開用設計書へ移行する | `docs/mc68000-internals/reference/cpu_68020.md`、実装、テスト |
| `docs/mc68000-internals/design/`<br>`cpu_68030.md` | 下書き | なし | 現行設計を自己完結した公開用設計書へ移行する | `docs/mc68000-internals/reference/cpu_68030.md`、実装、テスト |
| `docs/mc68000-internals/design/`<br>`cpu_68040.md` | 下書き | なし | 現行設計を自己完結した公開用設計書へ移行する | `docs/mc68000-internals/reference/cpu_68040.md`、実装、テスト |
| `docs/mc68000-internals/design/`<br>`cpu_68060.md` | 未着手 | なし | 実装済み仕様、制約、根拠、対応テストを記載する | `docs/mc68000-internals/reference/cpu_68060.md`、実装、テスト |
| `docs/mc68000-internals/design/`<br>`fpu.md` | 下書き | なし | FPU の責務、構成、制約、テストを記載する | `docs/mc68000-internals/reference/fpu.md`、実装、テスト |
| `docs/mc68000-internals/design/`<br>`cycle_timing.md` | 未着手 | なし | 全対応モデルの timing 根拠と検証方法を記載する | timing CSV、timing tests |
| `docs/mc68000-internals/design/`<br>`debugger.md` | 下書き | なし | debugger の設計、制約、テストを記載する | `docs/mc68000-internals/reference/debugger.md`、`debug.h`、テスト |
| `docs/mc68000-internals/design/`<br>`fault_ring.md` | 下書き | なし | fault ring のデータ契約、制約、テストを記載する | `docs/mc68000-internals/reference/fault_ring.md`、`debug.h`、テスト |
| `docs/mc68000-internals/support/`<br>`instruction_support.md` | 未着手 | なし | 命令対応表を実装・公開 API・テストと一致させる | `src/cpu/`、tests |
| `docs/mc68000-internals/support/`<br>`model_support.md` | 未着手 | なし | モデル別の対応範囲を実装・テストと一致させる | 公開ヘッダ、tests |
| `docs/mc68000-internals/support/`<br>`fpu_support.md` | 未着手 | なし | FPU 構成別の対応範囲を実装・公開 API・テストと一致させる | `fpu.h`、tests |
| `docs/mc68000-internals/support/`<br>`debugger_support.md` | 未着手 | なし | debug 機能の対応範囲を実装・テストと一致させる | `debug.h`、tests |
| `docs/mc68000-internals/data/`<br>`*.csv` | 保留 | 公開対象の選定・ライセンス確認 | 設計書から用途と検証方法を参照できるデータだけを移行する | 現行 CSV、設計書、tests |
