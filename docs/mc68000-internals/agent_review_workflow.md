# AI レビュー・変異試験手順

`AGENTS.md` から参照する詳細手順。AI レビュー、変異試験、FPU-2/FPU-3/FPU-4、oracle コメント、
レビュー結果の Issue 記録を行う前に読む。通常の実装・小規模修正では読まない。

## Review workflow

- AI レビュー回数に上限は設けない。blocking が 0 件になるまでレビューと修正を繰り返す。
- blocking は (1) production の契約違反、(2) 期待値と一次資料の不一致、(3) production の変異が
  生き残る検出漏れだけ。試験コード自体の頑健さ、出典の頁・言い回し、コメントの不正確さは
  non-blocking として Issue にまとめる。ただしコメントの事実誤りは同じ commit で直す。
- blocking 0、期待値不一致 0、production の生き残りが理由付き equivalent だけになったらフェーズを閉じる。
  試験内部の網羅性は別 Issue の backlog にしてよい。
- 指摘は種類として修正する。同じ形を grep で全件洗い出す。出典の頁・節・数値を書く箇所は
  `pdftotext -layout` の該当行と並べて確認する。出典は1か所にまとめ、他は参照する。
- 初回レビューでは、件数 assert の無いループ、死んだ oracle 欄、runner の早期 return、production 値の写しを
  全件調査し、oracle 毒入れとループ縮小の変異確認を含める。試験頑健性の指摘は non-blocking としてまとめる。
- 値・ビット・マスクは両端だけでなく各ビットを試し、可変ビットの正規形も受理側で試す。検出は、応答反復上限等の
  偽失敗でないことを試験関数単独でも確認する。
- 設計書の一般則は production が及ぶ範囲まで照合する。再レビューは前回指摘の解消、前回差分、差分の参照先との整合を対象にする。
- コメントのみの変更は AI レビューに出さない。コメント除去・空白正規化後の token diff 一致、build、ctest で確認する。
- 提出前に oracle 毒入れ、ループ縮小変異、コメント中の数値・主張の一次資料照合を済ませる。機械チェックで済む
  grep、diff、build、ctest はレビュー依頼前に行う。レビュー結果を Issue に記録する際は実施エージェント名を明記する。
- 指摘番号は `<Phase>-B<n>` / `<Phase>-N<n>` とし、フェーズ内で回をまたいで通し番号にする。

## FPCP automated mutations

- 収束は固定した変異集合で判定する。対象ファイルの条件式・定数・オフセット・CIR 呼出し箇所を機械的に全件列挙し、
  生き残りは理由付き allowlist だけにする。新しい欠陥種は演算子追加として別に審査する。
- FPU-3 は `scripts/fpcp_auto_mutants.py` で `coproc.c`、`cpu_dispatch_coproc_exception()`、
  `op_rte()` format 9 を対象にする。allowlist は `scripts/fpcp_auto_allowlist.py`（kind:
  `equivalent` / `observable` / `backlog`、理由・件数 `expect` 付き）。全件は `--jobs 10` で実行し、
  未分類 0 と `expect` 不一致 0 が終了コード 0 の条件。
- FPU-4 は `--target fpu4`（`ops_fpu.c`、`m68000.c` の
  `cpu_dispatch_fpu040_exception()`、中間結果 API）を対象にし、allowlist は
  `scripts/fpu4_auto_allowlist.py`。段2の `***Timeout 900 sec` 前後はスリープによる偽検出の可能性があるため、
  `--from-results <JSON> --stage2 --ids-file <mid>` で再実行してから `expect` を数える。
- 日常確認は変更した関数・行だけを `--only` で実行する。キャッシュ
  `~/.cache/mc68000-automut/<target>.json` は暫定確認であり、収束根拠にはしない。Issue close 前・
  フェーズ終了時だけ `--fresh` で全件を取り直す。`--results <JSON>` 後は、同一 `_meta` の場合のみ
  `--seed-cache <JSON>` で取り込める。
- FPU-2 は `scripts/fpu2_mutation_check.py`（表は `scripts/fpu2_mutations.py`）を使い、
  `--check-patterns` で現行コードに当たらない行も検出する。新対象は
  `scripts/fpcp_auto_mutants.py` の `TARGETS` と専用 allowlist に追加する。

## Oracle comments

- 期待値の根拠は印字値と出典（UM20 §8.2.x の印字頁、UM30 §11.6.x の ord、CSV 行）だけを書く。
  「全行で成立」は検証スクリプトで確かめた場合だけ書く。
- 一次資料にない値は「推論」と明記し、根拠は表の構造だけにする。推論は1か所にまとめ、他は参照する。
- 表を写す前に `pdftotext -layout` 等で機械抽出して行を照合し、抽出結果は `tests/data/` の CSV に置く。
