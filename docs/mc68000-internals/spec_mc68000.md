# MC68000 対応 設計書

仕様版: **0.9.14-draft**（Phase 5/6: 対応表は全規範IDが covered。第三者レビュー（2026-11-07）の指摘を反映済みで、再レビュー待ち。TIM-U3・TIM-U4 は一次資料から断定できない項目として §14.2 に残す）

一次資料:

- [MC68000 8-/16-/32-Bit Microprocessors User's Manual, Ninth Edition](https://www.nxp.com/docs/en/reference-manual/MC68000UM.pdf)
  — Motorola, 1993（以下 `[UM §x]` と記す。ローカル保管: `x68000/local/refs/M68000UM.pdf`）
- [M68000 Family Programmer's Reference Manual, Rev. 1](https://www.nxp.com/docs/en/reference-manual/M68000PRM.pdf)
  — Motorola, 1992（以下 `[PM §x]` と記す。ローカル保管: `x68000/local/refs/M68000PM.pdf`）
- [MC68000 16-Bit Microprocessor, April 1983](https://www.bitsavers.org/components/motorola/68000/68000/MC68000_16-Bit_Microprocessor_Apr83.pdf)
  — Motorola, 1983（以下 `[M68K83 §x]` と記す。MC68000固有のプリフェッチ記述を補う資料）

対象: 無印 **MC68000** の命令実行、例外、割り込み、および外部バス観測可能な振る舞い。

本書は特定の既存実装を前提・根拠にしない。MC68000 エミュレータを実装・検証するための
独立した仕様設計書とし、各判断は一次資料で裏付ける。

**記法についての注記**: 実装と対応付ける段階でも、ソース位置は行番号ではなく関数名と
一意な短いアンカーで参照する。

---

## 1. 目的とスコープ

本書は、MC68000 の命令実行・例外・割り込み・外部バス上の観測可能な動作を、一次資料だけから
再現・検証できる粒度で規定する。X68000 を含む上位システム（メモリマップ、デバイス、
ホストUI）の責務は本書の対象外とし、CPU がそれらから受け取るバス応答・割り込み入力・
リセット入力の契約（§4・§9・§10）までを範囲とする。

### 1.1 スコープ内

- 全65,536 opcode の分類と、MC68000 で有効な全命令の意味論（§6）
- CPU 状態、特権・スタック、例外フレーム、割り込み、リセット（§3・§7〜§9）
- バス契約、命令フェッチ、アラインメント、TAS の RMW、公称サイクル（§4・§10）
- 規範IDからケース・機械テストへの対応表（§12）

### 1.2 スコープ外と近似方針

- MC68008／68HC000／68010 以降、未文書opcode、シリコンerrata、ns 単位のピンタイミング（§2.1）
- 公開 API、デバッガ UI、ホスト言語の構成（§11）
- 資料から断定できない項目は §14 に記録し、期待値に使わない。許容近似は §10 の分類に従い、
  実装は近似範囲・観測可能な差・利用上の制約を明記する（§2.2）

### 1.3 受入基準

- 全規範IDが `tests/data/cpu/conformance_map_mc68000.csv` に1行あり、`tools/check_conformance_map.py` が成功する
- 全opcode分類に未分類・重複がない（INS-052/053）
- ケース未導出の規範IDは `gap` として理由つきで残し、§14.3 に集約する
- 各フェーズの成果物は、編集していない第三者のレビューを経て Issue に記録する

---

## 2. 規範の範囲と記法（Phase 0）

この節は、本書を実装仕様および適合性オラクルとして読むときの共通規則である。
後続節が個別の命令・例外・バス動作を規定するときは、本節に反する独自の解釈を置かない。

### 2.1 対象個体と規範範囲

対象は、一次資料に記載された **NMOS MC68000 の文書化済み動作**とする。MC68008、
MC68HC000、MC68010 以降、CPU32、および各社の互換CPUは対象に含めない。特定の
マスク改訂、速度グレード、未文書opcode、シリコンerrata、ns単位の電気的ピンタイミングは
本書の対象外とし、それらを再現する必要が生じたときは別プロファイルと追加資料を設ける。

資料が M68000 ファミリ共通規則を説明する箇所は、MC68000 が明示的に除外されず、
MC68000固有の記述と矛盾しない限りMC68000へ適用する。MC68000固有の記述は、
ファミリ共通の記述より優先する。

本書は次の3種類の規範情報を分けて記述する。

| 種別 | 目的 | 記述する内容 |
|---|---|---|
| **CPU規範仕様** | 実装の正しさを決める | 状態、命令、例外、割り込み、外部バス上の観測可能な動作 |
| **実装ガイド** | 実装の設計を助ける | 機能分割、擬似コード、失敗しやすい順序依存。CPU規範と矛盾してはならない |
| **適合性仕様** | 実装を検証する | 規範IDごとの初期状態、入力、期待最終状態、バストレース、許容差 |

ホスト言語、公開API、デバッガUI、メモリマップ、デバイス実装はCPU規範の対象外である。
ただし、それらがCPUに渡すバス応答・割り込み入力・リセット入力の契約は対象に含める。

### 2.2 適合性軸

適合性は一括の「正しい／誤り」にしない。次の4軸は直線的なレベルではなく独立に報告する。
各適合性ケースは、どの軸を判定するかを明記する。

| 軸 | 必須範囲 | 合否の根拠 |
|---|---|---|
| **F: 機能意味論** | 全opcodeの分類、許可EA、レジスタ・メモリ結果、CCR、PC、機能結果に影響するプリフェッチ | 命令ごとの規範IDと最終CPU状態 |
| **X: 例外・割り込み** | 特権遷移、例外優先順位、保存PC、スタックフレーム、STOP/HALT、割り込みベクタ選択 | 状態遷移、フレームのバイト列、定義済みのバス成功／失敗入力 |
| **B: バス順序** | 命令／オペランド／スタック／ベクタ／IACKの順序、サイズ、read/write、FC、byte lane、フォルト発生点 | 順序付きバストレース |
| **T: 時間精度** | 公称サイクル、wait stateの扱い、バスサイクル境界。ns単位の電気的位相は対象外 | サイクル数と順序付きバストレース |

F と X は機能互換の最低条件とする。Xのうちバス／アドレスエラーを扱うケースは、
少なくとも抽象バス上の成功／失敗の順序を規定する。BまたはTの未達項目がある実装は、
近似する範囲・観測可能な差・利用上の制約を明記しなければならない。

### 2.3 資料の優先順位と引用方法

一次資料間に矛盾、版差、または対象機種の曖昧さがある場合は、次の順に採用する。

1. MC68000固有の記述
2. MC68000を除外しないファミリ共通の記述
3. 同順位では、対象の条件（命令、例外種別、バス状態）をより具体的に示す記述

上記で解決できない事項は推測で確定しない。§14「未解決事項」に、相反する記述、
影響する規範ID、実機観測または追加資料で必要な確認を記録する。

`[UM]` と `[PM]` の間で分野をまたぐ矛盾があるときに、一方を機械的に優先してはならない。
より具体的な記述でも解決できなければ未解決事項に残す。規範的な本文には、少なくとも
`[UM §… printed p.… / PDF p.…]`、`[PM §… printed p.… / PDF p.…]`、または
`[M68K83 §… printed p.…]` を付ける。
表・図を根拠にする場合は節に加えて表／図番号も書く。参照実装、テスト結果、既存の
エミュレータ実装は、根拠ではなく補助的な不整合検出にのみ使う。

### 2.4 表記と共通意味論

- 数値は、特記しない限り10進数、16進数は `$` 接頭辞、2進数は `%` 接頭辞で表す。
  `byte`、`word`、`long` はそれぞれ 8、16、32 bit とする。
- ビット番号は最上位を大きい番号、最下位を bit 0 とする。`x[n:m]` は n から m までを
  含むビット列、`x[n]` は1ビットを表す。
- `sext8(x)`、`sext16(x)` は、それぞれ符号付き8/16 bit値を32 bitへ符号拡張する。
  サイズ付き演算の結果は、そのサイズに切り詰めてから、指定された宛先へ格納する。
- `read8(a)`／`read16(a)`／`write8(a,v)`／`write16(a,v)` は物理バスサイクルを表す。
  `read32(a)`／`write32(a,v)` は論理メモリアクセスであり、物理バスサイクルへの分解と順序は
  §4で定義する。big-endian、byte lane、アラインメント、24-bit外部アドレス、function codeも
  同節で定義する。
- バス応答が失敗したとき、失敗したバスサイクルは未完了とする。それ以前に完了したバス
  サイクルおよび内部副作用は保持し、原子的ロールバックを既定にしない。残る通常命令の
  サイクル、内部状態、例外フレームとベクタ取得は、各規範IDで個別に定義する。
- `zextN(x)` はN bit値を32 bitへゼロ拡張する。サイズ付き演算はN bit剰余演算とし、符号付き／
  符号なし比較・除算・拡張は個別に指定する。逐次代入と同時代入、旧値と新値を区別する必要が
  ある規則では、擬似コード内で明示する。
- `PC₀` は命令語をフェッチする前のアーキテクチャPC、`PCₙ` は命令の全拡張語を正常に消費した
  直後の命令ストリームカーソルを表す。内部プリフェッチ状態とPC相対EAの基準PCは別に定義する。
  例外に保存するPCは命令種別・発生段階ごとに個別に規定し、`PC₀`／`PCₙ` と混同しない。
- 「未変更」は観測可能な値を保持すること、「未定義」はその値を直接の期待値に使わないこと、
  「予約」はMC68000として実行可能な命令を割り当てないことを意味する。未定義値から派生する
  後続の観測結果まで自由であるとは限らないため、規範IDで明示する。予約opcode、`ILLEGAL`、
  Line-A、Line-Fは別の分類として§6で規定する。
- 1つの規則で順序が重要な場合、本文の箇条書きまたは番号付き擬似コード順を規範とする。
  説明図だけから順序を補完してはならない。

### 2.5 規範IDと適合性ケースの約束

規範IDは再利用しない識別子とし、原則として単独で合否判定できる1つの規則に対応させる。
後続節で次の接頭辞を使う。規則を削除してもIDは廃止扱いとし、別の規則へ割り当て直さない。
本書と適合性ケース形式には版番号を付ける。

| 接頭辞 | 対象 |
|---|---|
| `REG-` | レジスタ・SR・アドレス空間 |
| `BUS-` | 通常バス、命令フェッチ、バス信号 |
| `EA-` | 実効アドレス |
| `INS-` | 命令意味論とopcode分類 |
| `PRV-` | 特権とスタック遷移 |
| `EXC-` | 同期例外と例外フレーム |
| `IRQ-` | 割り込み |
| `RST-` | 外部リセット、リセット状態、RESET命令に伴う外部信号 |
| `TIM-` | タイミングとプリフェッチ |

適合性ケースは `T-<規範ID>-<連番>` と命名する。各ケースは、少なくとも規範ID、資料参照、
適合性軸（複数指定可）、初期CPU状態、初期メモリ、入力信号またはバス応答、実行量、期待CPU状態、
期待メモリ、期待例外、期待バストレース、期待サイクル数、そして値を問わない項目を持つ。
終了観測点（命令完了、例外ベクタ取得完了、次命令フェッチ直前など）、初期プリフェッチ状態または
それを再現する起動手順、サイクル数がwait stateを含むか、`N/A`と未指定の区別も必須とする。
規範ID→資料→テストIDと、テストID→規範IDの双方向追跡を維持する。完全なケース表と
機械可読表現は §12 で定義する。

全65,536個の命令語は、`valid`、`illegal encoding`、Line-A、Line-Fなど、相互排他的な
分類のいずれか1つへ割り当てる機械可読表を作る。§6の完了条件は、その表に未分類および
重複分類がないことを機械的に検査できることである。

### 2.6 命令規範テンプレート

§6で各命令を記述する際は、命令群の説明だけで済ませない。各命令（または同一規則を共有する
符号化集合）について、次の欄を埋める。

| 欄 | 必須内容 |
|---|---|
| 規範ID・資料 | `INS-` IDと一次資料の節／表／図／ページ |
| 符号化 | opcodeビット、拡張語、MC68000での可用性 |
| オペランド | サイズ、許可EA、即値の符号・ゼロ拡張 |
| 実行 | 規範的な番号付き擬似コード、read/write順、PC更新 |
| 状態 | レジスタ、メモリ、X/N/Z/V/Cを個別に設定・クリア・未変更・未定義として記す |
| 例外 | 特権・アドレス・バス・同期例外の優先順位、発生段階、保存PC、部分完了状態 |
| バス・時刻 | instruction/data、function code、byte lane、RMW属性、EA別・分岐別のアクセス順とサイクル式 |
| 検証 | 対応する適合性ケースID |

実装上の許容近似はこのテンプレートのCPU規範に混在させず、適合性軸ごとの未達事項として
別に記録する。
---

## 3. MC68000 の基礎モデル

MC68000のアーキテクチャ状態は、8本のデータレジスタ、8本のアドレスレジスタ、PC、
SR、およびユーザ／スーパーバイザのスタックポインタから成る。整数レジスタは32 bit幅だが、
外部バスアドレスは24 bitである。浮動小数点、MMU、キャッシュ、VBR、SFC/DFC、MSP/ISPは
MC68000の状態に含めない。`[PM §1.1, printed pp.1-1–1-3 / PDF pp.11–13]`

### 3.1 レジスタとデータ表現

#### 3.1.1 データレジスタ（REG-001）

`D0`–`D7` は各32 bitの汎用データレジスタである。byte演算は bits 7:0、word演算は
bits 15:0、long演算は bits 31:0 をオペランドとする。byteまたはwordを宛先にする通常の
データレジスタ操作は、指定部分だけを更新し、上位ビットを保持する。命令固有の例外は
§6で明記する。`[PM §1.7.1, printed pp.1-25–1-26 / PDF pp.35–36]`

#### 3.1.2 アドレスレジスタ（REG-002）

`A0`–`A6` と、現在の実行モードで可視な `A7` は各32 bitのアドレスレジスタである。Anは
byteオペランドの直接宛先・直接ソースには使えない。word値をAnへ格納する命令は、wordを
32 bitへ符号拡張してAn全体を更新する。long値をAnへ格納する命令は全32 bitを更新する。
`[PM §1.1.2, printed p.1-2 / PDF p.12]` `[PM §1.7.1, printed p.1-26 / PDF p.36]`

#### 3.1.3 A7、USP、SSP（REG-003）

アーキテクチャ状態として `USP` と `SSP` を別々に保持する。SR.S=0のとき命令中の `A7` は
`USP`、SR.S=1のときは `SSP` を指す。Sビットの変更および例外のスーパーバイザ遷移では、
退避側A7の値を対応するスタックポインタへ保存してから、移行先の値をA7として可視化する。
スタックは低位アドレス方向へ伸長する。MC68000にはMSPおよびISPは存在しない。
`[PM §1.1.2, printed p.1-2 / PDF p.12]` `[PM §1.3.1, printed p.1-10 / PDF p.20]`

#### 3.1.4 PC（REG-004）

PCは32 bitのアーキテクチャレジスタであり、実行中の命令アドレスを保持する。順次実行では
命令語・拡張語の消費に従って進み、分岐、例外、リターン、リセットで新しい値を取る。外部
バスに現れるアドレスは §3.3 の24 bit正規化後の値であり、PC自体を24 bitへ破壊的に
丸めてはならない。`[PM §1.1.3, printed p.1-3 / PDF p.13]`

#### 3.1.5 メモリ内の整数表現（REG-005）

メモリはbyteアドレス可能なbig-endianである。N番地のwordは `mem[N]` を上位byte、
`mem[N+1]` を下位byteとする。N番地のlongは、N番地の上位word、N+2番地の下位wordから成る。
`[PM §1.7.2, printed p.1-27 / PDF p.37]`

### 3.2 ステータスレジスタと実行状態（REG-006–REG-008）

SRは16 bitである。ユーザモードで変更できる部分は下位5 bitのCCRだけである。ただし
MC68000の `MOVE from SR` はユーザモードでも合法であり、SR全体を読める。この命令が
MC68010以降で特権化されることをMC68000へ遡及してはならない。`[UM §2.1.3, printed p.2-3 / PDF p.17]` `[UM §6.3.7 / PDF p.100]`
MC68000で規範化するビットは次のとおりである。`[PM §1.1.4, printed pp.1-3–1-4 / PDF pp.13–14]`

| bit | 名称 | 意味 |
|---|---|---|
| 15 | T1 | trace enable。命令完了後のトレース例外は §8.5 で規定する |
| 13 | S | 0=ユーザ、1=スーパーバイザ。A7のUSP/SSP選択にも使う |
| 10:8 | I2:I0 | 割り込みマスク。受理規則は §9.1 で規定する |
| 4 | X | extend。多倍長演算の入力にもなる |
| 3 | N | 結果の符号 |
| 2 | Z | 結果がゼロ |
| 1 | V | 符号付き算術オーバーフロー |
| 0 | C | 加算のcarryまたは減算のborrow |

bit 14（T0）とbit 12（M）はMC68000では常に0である。一方、bit 11およびbits 7:5は
MC68000固有資料でundefined/reservedであり、読出値や書込み後の値を適合性の期待値にしては
ならない。これらを将来世代の状態として保持することも許さない。`[PM §1.3.2 Figure 1-8,
printed pp.1-10–1-11 / PDF pp.20–21]` `[UM §2.1.3, printed p.2-3 / PDF p.17]`

CCRのX/N/Z/V/Cは命令ごとに個別に規定する。一般則として、Nは演算サイズの最上位bit、
Zは演算サイズへ切り詰めた結果が0か、Vはそのサイズの符号付き表現不能、Cは加算の桁上がり
または減算のborrowを表す。XとCは別の状態であり、算術命令が明示するときだけ同じ値になる。
`[PM §1.1.4, printed pp.1-3–1-4 / PDF pp.13–14]`

CPU実行状態は `running`、`stopped`、`halted` の3つを持つ。`stopped` はSTOP命令後に
受理可能な割り込みまたは外部リセットを待つ状態、`halted` は二重バスフォルト等で進行不能な
状態とする。各状態への遷移は §7–§9 で規定する。

### 3.3 アドレス空間とアラインメント（REG-009–REG-011）

アーキテクチャ上のアドレス計算は32 bit剰余で行う。各外部バスサイクルのアドレスは、その
下位24 bit、すなわち `a mod 2^24` を使用する。したがって、32 bitレジスタの上位8 bitは
レジスタ値として保持されても、外部メモリ空間を選択しない。外部アドレス空間は
`$000000`–`$FFFFFF` の16 MiBである。`[UM §3 Signal Description, address bus]`

byteアクセスは任意のbyteアドレスに可能である。命令語フェッチ、wordアクセス、longアクセス、
スタックのword/longアクセスは偶数アドレスでなければならない。奇数アドレスに対するそれらの
アクセスは、当該データ転送の前にアドレスエラーとなる。longアクセスは偶数アドレスNの
wordアクセス N、N+2 の順に分解する。アラインメント違反時の例外フレームと、例外処理中の
再フォルトは §8 で規定する。`[UM §5 16-Bit Bus Operation]` `[PM Appendix B, Figure B-2,
printed p.B-3 / PDF p.629]`

---

## 4. バス契約とメモリアクセス

本節は、外部デバイスの実装方式ではなく、CPU規範を検証可能にする抽象バスを定義する。
ピン位相・wait state・IACKの詳細なシーケンスは §10 と §9.2で規定する。抽象バスは各物理
サイクルについて、要求と結果を順序付きで記録できなければならない。

### 4.1 ビッグエンディアンとアクセスサイズ（BUS-001–BUS-004）

各抽象バス要求は、少なくとも次のタプルで表す。

`{sequence, address24, direction, size, uds, lds, function_code, kind, response, data}`

| 項目 | 値・意味 |
|---|---|
| `address24` | §3.3で正規化した24 bitアドレス |
| `direction` | `read` または `write` |
| `size` | 1 byteまたは1 word。longは2個のword要求として記録する |
| `uds` / `lds` | byte偶数=`asserted/deasserted`、byte奇数=`deasserted/asserted`、word=`asserted/asserted`。対応data laneは各々D15:8/D7:0 |
| `function_code` | §4.2の3 bit FC |
| `kind` | instruction fetch、operand、stack、vector、IACK、resetのいずれか |
| `response` | `dtack`、`vpa`、`berr`、`berr_halt`、またはwait stateを含む完了情報 |
| `data` | read結果またはwrite値。wordはbig-endianの16 bit値 |

longのメモリ表現は§3.1.5のとおり高位wordが低位アドレスにある。通常の昇順longアクセスだけは
高位word、低位wordの順とする。predecrement、stack、MOVEM、例外フレーム、RMWの物理順は
この一般則を使わず、それぞれの規範IDで定める。途中で失敗しても完了済みサイクルを取り消さない。
`[UM §5.1.1–§5.1.2, printed pp.5-3–5-8 / PDF pp.49–54]`

### 4.2 命令フェッチとプログラムカウンタ（BUS-005–BUS-008）

命令ストリームは偶数アドレスのwordフェッチで構成する。通常の命令語・拡張語フェッチは
現在の実行モードに応じたprogram function codeを使い、成功時にのみ命令ストリームカーソルを
2増やす。フェッチアドレスが奇数ならバス要求を出さずにアドレスエラーとする。

MC68000のfunction codeは次の値を使う。`[UM §3.3 Table 3-3, printed p.3-9 / PDF p.37]`

| FC | 意味 | 使用例 |
|---|---|---|
| `%001` | user data | ユーザモードの通常オペランド |
| `%010` | user program | ユーザモードの命令語・拡張語 |
| `%101` | supervisor data | スーパーバイザの通常オペランド、スタック、ベクタ |
| `%110` | supervisor program | スーパーバイザモードの命令語・拡張語 |
| `%111` | CPU space | 割り込み acknowledge。詳細は §9.2 |

他のFC値を通常の命令／データ要求に使ってはならない。通常の例外フレームpushとvector fetchは
supervisor dataである。ただし外部リセット後の初期SSP/PC取得はsupervisor programである。
`[UM §6.3.1, printed p.6-12 / PDF p.97]` バス上のFCはCPU状態から導出する入力であり、ホストがSRを読み取って
推測する代わりに、各要求へ明示して記録する。

プリフェッチキューの内部構成はこの段階では固定しない。ただし、実装は §10.2 で規定する
観測可能な命令フェッチ、自己書換え、保存PCの結果を満たさなければならない。

### 4.3 アドレスエラーとバスエラー（BUS-009–BUS-011）

通常サイクルの `berr` はバスエラーとして §8.3へ渡す。IACK中の `berr` はbus-error例外ではなく
spurious interruptを選ぶ。`berr_halt` はretry/停止を伴うBERRを区別する。`[UM §6.3.2, printed p.6-12 / PDF p.97]` アドレスエラーは、§3.3のアラインメント規則に
違反したCPU要求から生じ、対応する物理データ転送を発行しない点でバスエラーと異なる。

アドレスエラー／バスエラーの発生時、すでに完了した通常バスサイクルとレジスタ副作用は保持する。
ただし、残る通常命令アクセス、例外フレーム、ベクタ取得、二重フォルトの規則は `EXC-` 規範IDで
個別に決める。ホストが例外フレームを代わりに生成したり、失敗したwriteを復元したりしてはならない。

### 4.4 副作用を持つデバイスアクセス（BUS-012）

CPUは同じアドレスでも要求ごとに独立したバスアクセスを発行する。read-clearレジスタのような
デバイス副作用は、要求の `sequence` と `kind` を含む抽象バスログで観測できるものとする。
デバッグ表示・逆アセンブル・テストの初期化は、CPU実行バスアクセスと区別された非侵襲read契約を
使用しなければならない。この非侵襲readはCPU規範のバス要求を生成せず、適合性B/Tの対象外である。

---

## 5. 実効アドレス計算

MC68000のEAフィールドは mode=bits 5:3、register=bits 2:0である。EAの意味と拡張語の
消費は命令ごとに共通であり、命令が許す分類だけが異なる。MC68020以降のフル拡張語、
スケール、メモリ間接はMC68000に存在しない。`[PM §2.2, printed p.2-4 / PDF p.44]`

### 5.1 モード一覧と分類（EA-001–EA-013）

| EA-ID | mode/reg | 構文 | 実効値またはアドレス | 拡張word |
|---|---|---|---|---|
| EA-001 | 000/r | `Dr` | データレジスタr | 0 |
| EA-002 | 001/r | `Ar` | アドレスレジスタr | 0 |
| EA-003 | 010/r | `(Ar)` | `Ar` | 0 |
| EA-004 | 011/r | `(Ar)+` | 使用後にArを増加 | 0 |
| EA-005 | 100/r | `-(Ar)` | 使用前にArを減少 | 0 |
| EA-006 | 101/r | `(d16,Ar)` | `Ar + sext16(next_word)` | 1 |
| EA-007 | 110/r | `(d8,Ar,Xn)` | §5.3 | 1 |
| EA-008 | 111/000 | `(xxx).W` | `sext16(next_word)` | 1 |
| EA-009 | 111/001 | `(xxx).L` | `next_long` | 2 |
| EA-010 | 111/010 | `(d16,PC)` | `PC_ext + sext16(next_word)` | 1 |
| EA-011 | 111/011 | `(d8,PC,Xn)` | §5.3 | 1 |
| EA-012 | 111/100 | `#<data>` | 命令ストリーム中の即値 | byte/word=1、long=2 |

`PC_ext` は当該EAの最初の拡張wordのアドレスである。`next_word`と`next_long`は、EA解決開始時の
命令ストリームカーソルからbig-endianで順にフェッチした値である。byte即値は1拡張wordの下位byte、
word即値は同word全体、long即値は高位word・低位wordの順の2拡張wordである。mode=111/reg=101–111はMC68000のEAとして
予約であり、命令が独自に定義しない限り有効EAへ解釈してはならない。`[PM §2.2.1–§2.2.18,
printed pp.2-5–2-19 / PDF pp.45–59]`

| 分類 | 含むEA |
|---|---|
| data | EA-001、EA-003–EA-012 |
| data alterable | EA-001、EA-003–EA-009 |
| memory | EA-003–EA-012 |
| memory alterable | EA-003–EA-009 |
| control | EA-003、EA-006–EA-011 |
| alterable | EA-001–EA-009（Anを含む） |

命令仕様は「data」等の分類だけで許可EAを表し、個別命令がさらに狭める場合は §6 の
`INS-` 規範で列挙する。`[PM §2.3 Table 2-4, printed pp.2-19–2-20 / PDF pp.59–60]`

**EA-017**: 上表は Table 2-4 のMC68000の行（bd・メモリ間接はMC68020以降のため除く）と一致させる。以前の版は
memory を EA-003–EA-011 としていたが、Table 2-4 は `#<xxx>`（EA-012）の Memory 欄を X としており、EA-012 を含める。
一方、§6 の規則表（`tests/data/cpu/opcode_rules.csv`）はプレーンな memory 分類を許可EAに使わず、使うのは data／data alterable／
memory alterable／control（および命令固有の述語）だけである。memory alterable は alterable との積であり、EA-012 は
alterable でないため変わらない。したがってこの修正は個別命令の許可EAを変えない。`tools/check_ea_categories.py` が
本表と Table 2-4 の一致を検査する。

### 5.2 ポストインクリメント／プリデクリメントと A7

`(An)+` はオペランドのアドレスとして旧Anを使用し、オペランドサイズNだけAnを増加する。
`-(An)` はオペランドのアドレスとして使用する前に、NだけAnを減少する。Nはbyte=1、word=2、
long=4である。ただし `A7` のbyteアクセスではN=2であり、USP/SSPのword整列を維持する。
`[PM §2.2.4–§2.2.5, printed pp.2-6–2-7 / PDF pp.46–47]`

通常の成功パスでは、postincrementはオペランドアドレス使用後、predecrementは使用前に更新する。
`EA-014`: `-(An)` はNを減算済みでバス要求に入り、失敗後もその値を保持する。`EA-015`: `(An)+` のオペランド
アクセスがバスエラー／アドレスエラーになったとき、Anが旧値か更新後の値かは資料が規定しない。**未定義**とし、
適合性ケースはAnを比較しない（値を問わない項目）。例外自体（vector、14 byte フレーム、handlerへの遷移）は §8 に従い
検証する。UMはバスエラー時の保存情報を「完全な復帰には一般に不十分」とするだけで、レジスタの部分更新を規定しない。
`[UM §6.3.9.1, printed pp.6-16–6-17 / PDF pp.101–102]` いかなるEAも失敗時に一律ロールバックしてはならない。

### 5.3 PC 相対・インデックス・絶対アドレス

brief index extension wordは次のように解釈する。

| bit | 内容 |
|---|---|
| 15 | 0=`Dn`、1=`An` をindexとして選ぶ |
| 14:12 | indexレジスタ番号 |
| 11 | 0=word index、1=long index |
| 10:9 | MC68000ではscaleとして解釈しない。値は無視する |
| 8 | `EA-013`。MC68000は解釈しない。値は無視する |
| 7:0 | 符号付き8 bit displacement |

word indexは `sext16(Xn[15:0])`、long indexはXn全体を用いる。EA-007のアドレスは
`Ar + sext8(d8) + index`、EA-011は `PC_ext + sext8(d8) + index` である。
MC68000はbits 10:9をscaleとして解釈してはならない。
**EA-013**: MC68000のbrief extension wordはbits 10:8を `000` と定める（Figure 2-3 (a)、MC68000/MC68008/MC68010）。
bits 10:9をscaleとして使うのはMC68020以降の形式（同図 (b)）であり、MC68000はscale値を無視して
所望のアドレスにアクセスしない。MC68000と同図(b)以前の世代は、brief extension wordの不正な符号化を例外として
復号しない。したがって、bits 10:8の値は有効アドレスにも例外にも影響しない。bit 8が1の符号化は、
MC68020以降ではfull extension wordの識別に使われるが、MC68000は区別せずbrief形式として扱う。
`[PM §2.4, printed p.2-21 / PDF p.61]` なお、bit 8の無視を明示する文は資料になく、同節の「不正な符号化を
例外として復号しない」とFigure 2-3 (a)の固定値 `0` からの帰結である。UMにはこの符号化図がない。
`[PM §2.2.7, printed p.2-9 / PDF p.49]` `[PM §2.2.12, printed p.2-14 / PDF p.54]`
`[PM §2.4 Figure 2-3, printed p.2-21 / PDF p.61]`

absolute shortは16 bit値を符号拡張してからアドレスに使い、absolute longは高位word、
低位wordの順の2拡張wordを連結した32 bit値を使う。PC相対EAは読み出し専用であり、
alterable分類に含めない。`[PM §2.2.16–§2.2.17, printed p.2-18 / PDF p.58]`

### 5.4 EA の読み書きと read-modify-write（EA-016–EA-018）

EA解決は、(1) 命令が要求するEA分類・サイズを検証し、(2) 必要な拡張wordを順にフェッチし、
(3) レジスタ値またはメモリアドレスを求め、(4) 命令が定める順序で読出し・演算・書込みを
行う、という段階に分ける。分類外のEAは、拡張wordやオペランドを消費する前に命令例外として
扱う。各命令の例外優先順位がこの一般則を上書きする場合は、`INS-` 規範に明記する。

memory EAのread-modify-writeは、単なる `read` と `write` の置換ではない。TASなどの
バスロックを含む命令は、読み書き間のバス属性と割り込み境界を §10.3で規定する。通常の
memory read-modify-writeでも、read成功後のwrite失敗、pre/postincrement、CCR更新、
例外への移行を命令ごとに独立に検証する。

**EA-016**: 複数のEAを持つ命令（`MOVE <ea>,<ea>` 等）とMOVEMについて、フォルト時に先に完了したアクセスの副作用を
保持することは §2.4 のとおりである。一方、フォルトした時点での各レジスタとAnの更新状況（MOVEMで既に読んだレジスタ、
postincrement／predecrementの途中状態）は資料が規定しない。**未定義**とし、適合性ケースはこれらを比較しない。
正常終了時の評価順と逐次更新は §6.2.3（INS-024）の規範が定める。`[UM §6.3.9.1 / PDF p.101]`

---

## 6. 命令デコードと命令セット

命令集合は `[PM Appendix A Table A-3, printed pp.A-12–A-13 / PDF pp.607–608]` を正とする。
本節は2Aでデコード基盤と命令台帳を定義する。個々の意味論は2B–2E、全65,536件の閉包は2Fで完成する。

### 6.1 デコード方針と opcode の可用性

#### 6.1.1 分類語彙（INS-001）

各16 bit opcodeは最終的に、相互排他的な `valid:<form-id>`、`illegal encoding`、`Line-A`、
`Line-F` のいずれかへ分類する。`ILLEGAL`（`$4AFC`）は `valid:ILLEGAL` であり、未割当opcodeの
`illegal encoding` と例外ベクタは同じでも分類は異なる。Line-Aは `$A000–$AFFF`、Line-Fは
`$F000–$FFFF` の全opcodeである。`[PM Appendix A Table A-3, printed pp.A-12–A-13 / PDF pp.607–608]`

#### 6.1.2 機械可読な規則形式（INS-002）

規則表の1行は次の列を持つCSVまたはJSONとする。

`form_id, mask, value, extension, ea_predicate, classification, phase, source, norm_ids`

`mask/value` は16 bit値で `(opcode & mask) == value`、`extension` は追加wordの文法、`ea_predicate` は§5の
EA-ID集合、`classification` は上記語彙、`phase` は意味論の担当フェーズ、`source` は一次資料位置、
`norm_ids` は対応する `INS-` IDである。Line-A/Fは固定生成規則とし、個別のvalid規則行で上書きしない。
規則の優先順位に依存して重複を隠してはならず、`mask/value` と `ea_predicate` を評価して得る16 bit opcode集合の
交差、および同一opcodeの複数分類を機械的に失敗として報告する。extension wordはopcode閉包のキーではなく、
対応するopcode形式の別文法として検査する。

#### 6.1.3 命令台帳（INS-003）

| 担当 | MC68000命令形式 |
|---|---|
| 2B | MOVE, MOVEA, MOVEQ, MOVEM, MOVEP, LEA, PEA, LINK, UNLK, EXG, EXT, SWAP |
| 2C | ADD/ADDA/ADDI/ADDQ/ADDX, SUB/SUBA/SUBI/SUBQ/SUBX, CMP/CMPA/CMPI/CMPM, MULU/MULS, DIVU/DIVS, AND/OR/EOR, ABCD/SBCD/NBCD, CLR/NEG/NEGX/NOT/TST, BTST/BCHG/BCLR/BSET |
| 2D | ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR, BRA/BSR/Bcc, DBcc, Scc, JMP/JSR/RTS |
| 2E | MOVE to/from SR, MOVE to CCR, MOVE USP, ANDI/ORI/EORI to CCR/SR, CHK, ILLEGAL, NOP, RESET, RTE, RTR, STOP, TAS, TRAP, TRAPV |
| 2F | 全規則を統合した65,536 opcodeの閉包検査 |

この台帳にないMC68000命令形式は存在してはならない。後世代専用の形式は、MC68000の規則で
`valid`、`illegal encoding`、Line-A、Line-Fのいずれになるかを個別に再分類する。たとえば高位nibble
A/Fは常にLine-A/Fであり、後世代の符号化がMC68000の既存valid opcodeと重なる場合もある。

#### 6.1.4 2Aの境界（INS-004）

2Aは規則の器、命令形式の担当、Line-A/Fを確定するが、各 `valid` 規則をまだ完全列挙しない。
65,536件の未分類0・重複0、分類件数、生成手順は2Fの受入条件である。2B–2Eは自分の命令形式を
規則表へ追加し、累積規則の交差検査を通す。

### 6.2 データ移動・システム命令

#### 6.2.1 Phase 2B の共通境界（INS-010）

本節は正常バス応答下のF軸だけを規定する。各命令の拡張語、EA解決、PC更新は§5に従う。
フォルト時部分完了は `EA-015`/`EA-016`、詳細バス順・サイクルは後続フェーズへ延期する。

#### 6.2.2 規則表と正常擬似コード

| ID | 16-bit opcode規則 | extension / EA | 正常終了時の規範 |
|---|---|---|---|
| INS-011 MOVE | `00|size|dst-reg|dst-mode|src-mode|src-reg`、size=01/11/10 | source: B=EA-001,003–012; W/L=EA-001–012。dst=EA-001,003–009 | 1. sourceを評価・read。2. destinationを評価。3. write。4. CCR=X不変,N/Z=result,V=C=0 |
| INS-012 MOVEA | MOVEでdst-mode=001、size=11/10 | source=EA-001–012 | `An←sext16(source)` または `An←source`。CCR不変 |
| INS-013 MOVEQ | `$7000 | Dn<<9 | imm8` | なし | `Dn←sext8(imm8)`、MOVEのCCR |
| INS-014 MOVEM | `$4880 | dr<<10 | size<<6 | EA`、mask wordが**EA extensionより先** | reg→mem=EA-003,005–009。mem→reg=EA-003,004,006–011 | 下記MOVEM規範。CCR不変 |
| INS-015 MOVEP | `$0108 | Dn<<9 | opmode<<6 | An`、d16 word | `N=An+sext16(d16)`。opmode=100/101 mem→Dn W/L、110/111 Dn→mem W/L | 下記MOVEP規範。CCR不変 |
| INS-016 LEA | `$41C0 | An<<9 | control-EA` | EA-003,006–011 | `An←EA address`。CCR不変 |
| INS-017 PEA | `$4840 | control-EA` | EA-003,006–011 | 1. EA address算出。2. SP←SP-4。3. write32(SP,EA)。CCR不変 |
| INS-018 LINK | `$4E50|An`、d16 word | なし | 1. oldAn←An。2. SP←SP-4。3. write32(SP,oldAn)。4. An←SP。5. SP←SP+sext16(d16)。CCR不変 |
| INS-019 UNLK | `$4E58|An` | なし | 1. SP←An。2. An←read32(SP)。3. SP←SP+4。CCR不変 |
| INS-020 EXG | `$C140|Rx<<9|Ry` (D/D)、`$C148|Rx<<9|Ry` (A/A)、`$C188|Dx<<9|Ay` (D/A) | なし | 対応するD/D、A/A、D/Aを同時交換。CCR不変 |
| INS-021 EXT | `$4880|Dn`=.W、`$48C0|Dn`=.L | なし | .W: Dn[15:0]←sext8(Dn[7:0])（上位16保持）。.L: Dn←sext16(Dn[15:0])。X不変、N/Zは操作サイズ、V=C=0 |
| INS-022 SWAP | `$4840|Dn` | なし | Dn[31:16]↔Dn[15:0]。X不変、N/Zはlong、V=C=0 |

`EX​TB.L` はMC68020以降だけであり、INS-021のvalid規則に含めない。`[PM EXG, printed p.4-105 / PDF p.208]` `[PM EXT, EXTB, printed p.4-106 / PDF p.209]` `[PM LEA, printed p.4-110 / PDF p.213]` `[PM LINK, printed pp.4-111–4-112 / PDF pp.214–215]` `[PM MOVE, printed pp.4-116–4-118 / PDF pp.219–221] [PM MOVEA, printed pp.4-119–4-120 / PDF pp.222–223]` `[PM MOVEM, printed pp.4-128–4-130 / PDF pp.231–233]` `[PM MOVEP, printed pp.4-131–4-133 / PDF pp.234–236]` `[PM MOVEQ, printed p.4-134 / PDF p.237]` `[PM PEA, printed p.4-159 / PDF p.262]` `[PM SWAP, printed p.4-185 / PDF p.288]` `[PM UNLK, printed p.4-194 / PDF p.297]`

#### 6.2.3 MOVEM と MOVEP の逐次規範（INS-024）

MOVEM control/postincrement符号化のmaskは bit0=D0…bit15=A7、predecrement符号化は bit0=A7…bit15=D0。
mask=0なら転送も基底更新もない。control reg→memはD0…A7の選択順に、各回write後に+sizeする。predecrement
reg→memはA7…D0の選択順に、各回Anを-sizeしてからwriteする。基底AnがmaskにあるMC68000は**初期An値**をwriteする。
postincrement mem→regはD0…A7の選択順にread後+sizeし、基底Anがmaskにあればread値を捨て最終EAを残す。W storeは
register低位16、W loadはsext16。MOVEPはN,N+2[,N+4,N+6]へ高位byteから順に対応し、mem→Dn.WはDn上位16を保持する。

#### 6.2.4 適合性ケース（INS-023）

以下の各ケースはF軸、正常バス応答、`exec=1 instruction`、`end=次命令フェッチ直前`、初期PC=`$1000`、
初期SRは `(SR & $F71F)=$0010`（S=0、X=1、N=Z=V=C=0）を共通入力とし、B/X/Tは `N/A` とする。
各命令word列を`$1000`からbig-endian wordで配置した後、prefetchに旧内容を残さずPC=`$1000`から実行を開始する。
期待例外は全ケースで「なし」である。表中の `SR=$xxxx` は完全なSR値ではなく `(SR & $F71F)=$xxxx` を、
`SR不変`はこの規範bit群の不変を表す。表にないレジスタ・メモリは未変更を期待する。

`tests/cpu/test_phase2b_contract.c` はこの表の全ケース（25件）を、同じ命令語・初期状態・比較maskで実行する機械可読な
回帰オラクルとする。表に期待値のないレジスタとメモリ（低位 `$0000–$1FFF`）は、初期値のままであることも検査する。
初期A7（USP）は `$6000` とし、ケースが A7/USP を指定した場合だけ上書きする。SR 欄が空のケース（T-INS-011-02）はSRを比較しない。

| ケース | 初期CPU・命令word列・メモリ | 期待CPU・メモリ |
|---|---|---|
| T-INS-011-01 | D0=`$11223344`,D1=`$AABBCCDD`; words=`$1200` | D1=`$AABBCC44`,PC=`$1002`,SR=`$0010` |
| T-INS-011-02 | A0=`$200`,A1=`$300`,mem[`$200..201`]=`12 34`; words=`$3318` | A0=`$202`,A1=`$2FE`,mem[`$2FE..2FF`]=`12 34`,PC=`$1002` |
| T-INS-011-03 | D0=`$12345678`; words=`$2200` (MOVE.L D0,D1) | D1=`$12345678`,PC=`$1002`,SR=`$0010` |
| T-INS-012-01 | D0=`$00008000`; words=`$3240` | A1=`$FFFF8000`,PC=`$1002`,SR不変 |
| T-INS-012-02 | D0=`$12345678`; words=`$2240` | A1=`$12345678`,PC=`$1002`,SR不変 |
| T-INS-013-01 | words=`$7480` | D2=`$FFFFFF80`,PC=`$1002`,SR=`$0018` |
| T-INS-014-01 | A0=`$200`,D0=`$12345678`,A7=`$9ABCDEF0`; words=`$4890,$8001` | mem[`$200..203`]=`56 78 DE F0`,A0=`$200`,PC=`$1004`,SR=`$0010` |
| T-INS-014-02 | A0=`$200`,D0=`$11223344`; words=`$48E0,$8080` | A0=`$1F8`,mem[`$1F8..1FF`]=`11 22 33 44 00 00 02 00`,PC=`$1004`,SR=`$0010` |
| T-INS-014-03 | A0=`$200`,mem[`$200..203`]=`80 00 12 34`; words=`$4C98,$0101` | D0=`$FFFF8000`,A0=`$204`,PC=`$1004`,SR=`$0010` |
| T-INS-014-04 | A0=`$200`,mem[`$200..207`]=任意; words=`$48D0,$0000` | A0・mem不変、PC=`$1004`,SR=`$0010` |
| T-INS-014-05 | A0=`$200`,mem[`$200..207`]=`12 34 56 78 9A BC DE F0`; words=`$4CD0,$0201` | D0=`$12345678`,A1=`$9ABCDEF0`,A0=`$200`,PC=`$1004`,SR=`$0010` |
| T-INS-015-01 | A0=`$100`,D0=`$AABBCCDD`; words=`$01C8,$0000` | mem[`$100,$102,$104,$106`]=`AA BB CC DD`,PC=`$1004`,SR=`$0010` |
| T-INS-015-02 | A0=`$100`,D0=`$AABB0000`,mem[`$100,$102`]=`80 01`; words=`$0108,$0000` | D0=`$AABB8001`,PC=`$1004`,SR=`$0010` |
| T-INS-015-03 | A0=`$100`,mem[`$100,$102,$104,$106`]=`80 01 02 03`; words=`$0148,$0000` | D0=`$80010203`,PC=`$1004`,SR=`$0010` |
| T-INS-015-04 | A0=`$100`,D0=`$AABBCCDD`; words=`$0188,$0000` | mem[`$100,$102`]=`CC DD`,PC=`$1004`,SR=`$0010` |
| T-INS-016-01 | words=`$41FA,$0010` | A0=`$1012`,PC=`$1004`,SR=`$0010` |
| T-INS-017-01 | USP=`$200`; words=`$4857` | USP=`$1FC`,mem[`$1FC..1FF`]=`00 00 02 00`,PC=`$1002`,SR=`$0010` |
| T-INS-018-01 | A6=`$300`,USP=`$200`; words=`$4E56,$FFF8` | mem[`$1FC..1FF`]=`00 00 03 00`,A6=`$1FC`,USP=`$1F4`,PC=`$1004`,SR=`$0010` |
| T-INS-019-01 | A6=`$200`,mem[`$200..203`]=`12 34 56 78`; words=`$4E5E` | A6=`$12345678`,USP=`$204`,PC=`$1002`,SR=`$0010` |
| T-INS-020-01 | D0=`$11111111`,A0=`$22222222`; words=`$C188` | D0=`$22222222`,A0=`$11111111`,PC=`$1002`,SR=`$0010` |
| T-INS-020-02 | D0=`$11111111`,D1=`$22222222`; words=`$C141` | D0=`$22222222`,D1=`$11111111`,PC=`$1002`,SR=`$0010` |
| T-INS-020-03 | A0=`$11111111`,A1=`$22222222`; words=`$C149` | A0=`$22222222`,A1=`$11111111`,PC=`$1002`,SR=`$0010` |
| T-INS-021-01 | D0=`$AABBCC80`; words=`$4880` | D0=`$AABBFF80`,PC=`$1002`,SR=`$0018` |
| T-INS-021-02 | D0=`$AABB8000`; words=`$48C0` | D0=`$FFFF8000`,PC=`$1002`,SR=`$0018` |
| T-INS-022-01 | D0=`$80000000`; words=`$4840` | D0=`$00008000`,PC=`$1002`,SR=`$0010` |

### 6.3 算術・BCD・論理・ビット操作

#### 6.3.1 Phase 2C の境界と共通CCR規則（INS-025）

本節は正常バス応答下のF軸を規定する。EA解決、拡張wordの消費、PC更新は§5に従う。
フォルト時の部分完了はEA-015–018、除算ゼロ例外、詳細バス順、RMWの不可分性、サイクルは
Phase 3/§10へ延期する。各表の「不変」は当該CCR bitを保存する意味であり、未定義値は適合性
ケースで比較しない。`addN(a,b,x)=(a+b+x) mod 2^N`、`subN(a,b,x)=(a-b-x) mod 2^N` とする。

通常の加減算結果 `r` は N=signN(r)、Z=(r=0) とする。加算のVは同符号の被加数・加数から
異符号の結果を得たとき、Cはunsigned `a+b+x >= 2^N`。減算のVは被減数と減数の符号が異なり
結果の符号が被減数と異なるとき、Cはunsigned borrow (`a < b+x`) とする。ADD/SUB/NEG系では
X=C、CMP系ではX不変である。論理結果はX不変、N/Z=result、V=C=0である。
`[PM ADD, printed pp.4-4–4-6 / PDF pp.107–109] [PM ADDA, printed pp.4-7–4-8 / PDF pp.110–111] [PM ADDI, printed pp.4-9–4-10 / PDF pp.112–113] [PM ADDQ, printed pp.4-11–4-12 / PDF pp.114–115] [PM ADDX, printed pp.4-13–4-14 / PDF pp.116–117]` `[PM SUB, printed pp.4-174–4-176 / PDF pp.277–279] [PM SUBA, printed pp.4-177–4-178 / PDF pp.280–281] [PM SUBI, printed pp.4-179–4-180 / PDF pp.282–283] [PM SUBQ, printed pp.4-181–4-182 / PDF pp.284–285] [PM SUBX, printed pp.4-183–4-184 / PDF pp.286–287]`

#### 6.3.2 opcode規則と正常意味論（INS-026–INS-034）

機械可読な2B/2C規則は `tests/data/cpu/opcode_rules.csv` を正とし、`python3 tools/check_opcode_rules.py` は
各 `mask/value` とEA述語を16 bit opcode集合へ展開して、規則優先順位による隠蔽なしで交差を失敗にする。
次表はその2C規則の人間可読な要約である。`data-alterable` は
EA-001,003–009、`data` はEA-001,003–012、`memory-alterable` はEA-003–009、`control` は
EA-003,006–011を指す。size=00/01/10はB/W/L、`#0` のquick値は8である。

| ID / form | 16-bit opcode規則 | 許可EA・正常終了時の規範 |
|---|---|---|
| INS-026 ADD, ADDA | `D|Dn|opmode|EA`。opmode 000/001/010=EA→Dn B/W/L、011/111=ADDA.W/.L、100/101/110=Dn→EA B/W/L | ADDは前記加算とCCR更新。ADDA.Wは`An←An+sext16(src)`、.Lは`An←An+src`、CCR不変。EA→DnのW/LだけAn直接を許す。Dn→EAはmemory-alterable。 |
| INS-027 ADDI, ADDQ, ADDX | ADDI=`$0600|size<<6|EA`、imm word(s); ADDQ=`$5000|q<<9|size<<6|EA`; ADDX=`$D100|Dx<<9|size<<6|rm<<3|Dy` | ADDIはdata-alterable。ADDQはBでAn不可、W/LのAnは32-bit加算・CCR不変、他はADD。ADDXはDnまたは`-(Ay),-(Ax)`、Xを加える。ADDXのZは`oldZ AND (r=0)`、他はADD。 |
| INS-028 SUB, SUBA | `9|Dn|opmode|EA`。opmode 000/001/010=EA→Dn、011/111=SUBA.W/.L、100/101/110=Dn→EA | SUBは前記減算とCCR更新。SUBAはADDAと同じ拡張規則で減算しCCR不変。EA制約はINS-026と同じ。 |
| INS-029 SUBI, SUBQ, SUBX | SUBI=`$0400|size<<6|EA`; SUBQ=`$5100|q<<9|size<<6|EA`; SUBX=`$9100|Dx<<9|size<<6|rm<<3|Dy` | SUBI/SUBQ/SUBXは各々ADD側と対称。SUBXはXをborrow入力とし、Zは`oldZ AND (r=0)`。SUBQのAn規則もADDQと同じ。 |
| INS-030 CMP群 | CMP=`B|Dn|opmode|EA`（000/001/010）、CMPA=`B|An|011/111|EA`、CMPI=`$0C00|size<<6|EA`、CMPM=`$B108|Ax<<9|size<<6|Ay` | 値は`dst-src`でCCRを減算規則に更新するがX不変、結果は保存しない。CMPA.Wはsourceを符号拡張し、CMPA.Lはlong。CMP/CMPIのB/W/Lはdata（CMPのW/LはAn可）、CMPIはAn不可。CMPMは`(Ay)+`をsource、`(Ax)+`をdstとして同サイズずつ読出し後に両方を更新する。 |
| INS-031 MUL/DIV | MULU/MULS=`$C0C0/$C1C0|Dn<<9|data`、DIVU/DIVS=`$80C0/$81C0|Dn<<9|data` | MULは16×16→32をDnへ格納、X不変、N/Z=long結果、V=C=0。DIVはDnを32-bit dividend、sourceを16-bit divisorとし、quotientをDn低位word、remainderを高位wordへ格納する。quotientが符号なし/符号付きwordに収まらなければDn不変、V=1、C=0、X不変、N/Z未定義。正常時V=C=0、N/Z=quotient。 |
| INS-032 AND/OR/EOR と即値論理 | AND=`C|Dn|opmode|EA`、OR=`8|Dn|opmode|EA`（各opmodeはADDのdata形式）、EOR=`B100|Dn<<9|size<<6|data-alterable`、ORI/ANDI/EORI=`$0000/$0200/$0A00|size<<6|data-alterable`、imm word(s) | AND/ORはEA→DnまたはDn→EA、EORはDn→EAだけ。通常ORI/ANDI/EORIはimmediateとdestinationを論理演算する。CCR/SR宛ての特殊符号化は2Eで規定する。全形式は論理CCR規則を使う。 |
| INS-033 BCDと単項 | ABCD=`$C100|Dx<<9|rm<<3|Dy`、SBCD=`$8100|Dx<<9|rm<<3|Dy`、NBCD=`$4800|memory-alterable/Dn`; CLR=`$4200|size<<6|data-alterable`; NEG=`$4400|...`; NEGX=`$4000|...`; NOT=`$4600|...`; TST=`$4A00|size<<6|data-alterable` | ABCD/SBCDはpacked BCD byteとXを加減し、X=C=decimal carry/borrow、Z=`oldZ AND (r=0)`、N/V未定義。メモリ形式は双方predecrementしてread/writeする。NBCDは`0-src-X`のBCD版で同じCCR。CLRは0を書込み、X不変,N=V=C=0,Z=1。NEGは`0-src`、NEGXは`0-src-X`（Zは累積）、NOTはbit反転し論理CCR、TSTは書込まず論理CCR。 |
| INS-034 bit操作 | dynamic: BTST=`$0100|Dn<<9|EA`、BCHG/BCLR/BSET=`$0140/$0180/$01C0|Dn<<9|EA`; immediate=`$0800/$0840/$0880/$08C0|EA`、bit-number拡張word | Dn宛先ではbit番号mod 32、memoryではmod 8。対象bitの旧値をZへ（0ならZ=1）設定し、X/N/V/C不変。BTSTはdataをreadのみ、他はDnまたはmemory-alterableをread-modify-writeしてbitを反転/0/1にする。 |

ADD/SUB/AND/ORの`opmode`とEA制約、即値命令、CMPM、BCD、bit manipulationの符号化は
`[PM ADD, printed pp.4-4–4-6 / PDF pp.107–109] [PM ADDA, printed pp.4-7–4-8 / PDF pp.110–111] [PM ADDI, printed pp.4-9–4-10 / PDF pp.112–113] [PM ADDQ, printed pp.4-11–4-12 / PDF pp.114–115] [PM ADDX, printed pp.4-13–4-14 / PDF pp.116–117]` `[PM AND, printed pp.4-15–4-17 / PDF pp.118–120] [PM ANDI, printed pp.4-18–4-20 / PDF pp.121–123]`
`[PM CMP, printed pp.4-75–4-76 / PDF pp.178–179] [PM CMPA, printed pp.4-77–4-78 / PDF pp.180–181] [PM CMPI, printed pp.4-79–4-80 / PDF pp.182–183] [PM CMPM, printed p.4-81 / PDF p.184]` `[PM DIVS, DIVSL, printed pp.4-92–4-95 / PDF pp.195–198] [PM DIVU, DIVUL, printed pp.4-96–4-99 / PDF pp.199–202]`
`[PM MULS, printed pp.4-135–4-137 / PDF pp.238–240] [PM MULU, printed pp.4-138–4-140 / PDF pp.241–243]` `[PM BTST, printed pp.4-61–4-63 / PDF pp.164–166] [PM BCHG, printed pp.4-27–4-29 / PDF pp.130–132] [PM BCLR, printed pp.4-30–4-32 / PDF pp.133–135] [PM BSET, printed pp.4-56–4-58 / PDF pp.159–161]`
`[PM ABCD, printed pp.4-2–4-3 / PDF pp.105–106] [PM SBCD, printed pp.4-170–4-171 / PDF pp.273–274] [PM NBCD, printed pp.4-141–4-142 / PDF pp.244–245]` による。

#### 6.3.3 演算上の境界（INS-035）

DIVUは`q=floor(zext32(Dn)/zext16(src))`、remainderは対応する剰余、DIVSは符号付きの商を0方向へ丸め、
remainderの符号はdividendに従う。除数0ではN/Z/Vは未定義、C=0、X不変であり、destinationの
値は期待値にしない。例外種別、保存PC、フレームはEXC-010で規定する。BCDの各nibbleが非十進値の場合の
結果は資料が保証しないためF軸の期待値にしない。
`[PM DIVS, DIVSL, printed pp.4-92–4-95 / PDF pp.195–198] [PM DIVU, DIVUL, printed pp.4-96–4-99 / PDF pp.199–202]`

CLR/NEG/NEGX/NOTのmemory形式とBCHG/BCLR/BSET memory形式はread成功後にwriteする。これはF軸での
正常結果だけを規定し、write失敗時にCCRをいつ更新するか、前置/後置更新を含む部分完了はEA-016および
EXC-011へ延期する。bit操作のimmediate bit-numberは拡張wordの低位byteを用いる。

#### 6.3.4 適合性ケース（INS-036）

全ケースはF軸、正常バス応答、`exec=1 instruction`、`end=次命令フェッチ直前`、初期PC=`$1000`、
初期SRの比較mask=`$F71F`、表記外のレジスタ・メモリは未変更、期待例外なし、B/X/T=N/Aとする。
`SR=$xxxx` は同mask後の値である。命令word列は`$1000`からbig-endianで配置し、PC=`$1000`から
開始する。ケース開始時の命令キューは、初期PCの2語（`$1000`、`$1002`）を取得済みとし、この初期取得の bus cycle は観測・計測の対象に数えない（§12.6 の「プリフェッチ状態は命令先頭」と同じ）。`SR=undef(NZ)` はN/Zを比較しない。
`tests/cpu/test_phase2c_contract.c` はこの表で追加した全レジスタEAケース（T-INS-026-02、
027-01/03、029-01/02、031-04/05、032-03–06、033-01/04–08）を、同じ命令語・初期状態・
比較maskで実行する機械可読な回帰オラクルとする。同ファイルの `test_spec_634_remaining_cases` は、A1・メモリ・PCを比較する残りの全ケース（T-INS-026-01、027-02、028-01、030-01/02、
031-01/02/03、032-01/02、033-02/03、034-01/02/03）を実行する。初期SRは`$0010`、比較maskは`$F71F`（031-03はN/Zを除く`$F717`）である。


| ケース | 初期CPU・命令word列・メモリ | 期待CPU・メモリ |
|---|---|---|
| T-INS-026-01 | D0=`$7FFFFFFF`,D1=`$1`; words=`$D081` | D0=`$80000000`,PC=`$1002`,SR=`$000A` |
| T-INS-026-02 | A0=`$1000`,D0=`$FFFF8000`; words=`$D0C0` | A0=`$FFFF9000`,PC=`$1002`,SR不変 |
| T-INS-027-01 | D0=`$FFFFFFFF`,D1=`$00000000`,SR=`$0014`; words=`$D101` | D0=`$FFFFFF00`,PC=`$1002`,SR=`$0015` |
| T-INS-027-02 | A0=`$10`; words=`$5048` | A0=`$18`,PC=`$1002`,SR不変 |
| T-INS-028-01 | D0=`$80000000`,D1=`$1`; words=`$9081` | D0=`$7FFFFFFF`,PC=`$1002`,SR=`$0002` |
| T-INS-029-01 | D0=`$0`,D1=`$0`,SR=`$0014`; words=`$9100` | D0=`$000000FF`,PC=`$1002`,SR=`$0019` |
| T-INS-030-01 | D0=`$00010000`,D1=`$00000001`,SR=`$0010`; words=`$B041` | D0不変,PC=`$1002`,SR=`$0019`（CMP.W は下位wordで `0-1` を比較し N=1,C=1。X不変） |
| T-INS-030-02 | A0=`$200`,A1=`$300`,mem[`$200`]=`02`,mem[`$300`]=`03`; words=`$B308` | A0=`$201`,A1=`$301`,PC=`$1002`,SR=`$0010` |
| T-INS-031-01 | D0=`$00000003`,D1=`$00000004`; words=`$C0C1` | D0=`$0000000C`,PC=`$1002`,SR=`$0010` |
| T-INS-031-02 | D0=`$00010000`,D1=`$00000002`; words=`$80C1` | D0=`$00008000`,PC=`$1002`,SR=`$0018`（商の bit15 が1なので N=1） |
| T-INS-031-03 | D0=`$FFFFFFFF`,D1=`$0001`; words=`$80C1` | D0不変,PC=`$1002`,SR=`$0012`,SR=undef(NZ) |
| T-INS-032-01 | D0=`$F0`,D1=`$0F`; words=`$C001` | D0=`$00`,PC=`$1002`,SR=`$0014` |
| T-INS-032-02 | D0=`$AA`,D1=`$FF`; words=`$B101` | D1=`$55`,PC=`$1002`,SR=`$0010` |
| T-INS-033-01 | D0=`$00000099`,D1=`$00000000`,SR=`$0014`; words=`$C300` | D1=`$00000000`,PC=`$1002`,SR=`$0015`,N/V未比較 |
| T-INS-033-02 | D0=`$00000000`,SR=`$0014`; words=`$4000` | D0=`$000000FF`,PC=`$1002`,SR=`$0019` |
| T-INS-033-03 | D0=`$12345678`; words=`$4240` | D0=`$12340000`,PC=`$1002`,SR=`$0014` |
| T-INS-034-01 | D0=`$00000020`; words=`$0100` | D0不変,PC=`$1002`,SR=`$0014` |
| T-INS-034-02 | D0=`$00000020`; words=`$08C0,$0005` | D0=`$00000020`,PC=`$1004`,SR=`$0010` |
| T-INS-034-03 | A0=`$200`,D1=`$00000009`,mem[`$200`]=`00`; words=`$03D0` | mem[`$200`]=`02`,PC=`$1002`,SR=`$0014` |
| T-INS-027-03 | D0=`$1234FFFF`; words=`$0640,$0001` | D0=`$12340000`,PC=`$1004`,SR=`$0015` |
| T-INS-029-02 | D0=`$12340000`; words=`$0440,$0001` | D0=`$1234FFFF`,PC=`$1004`,SR=`$0019` |
| T-INS-031-04 | D0=`$0000FFFE`,D1=`$00000003`; words=`$C1C1` | D0=`$FFFFFFFA`,PC=`$1002`,SR=`$0018` |
| T-INS-031-05 | D0=`$FFFFFFF6`,D1=`$FFFD`; words=`$81C1` | D0=`$FFFF0003`,PC=`$1002`,SR=`$0010` |
| T-INS-032-03 | D0=`$12340000`; words=`$0040,$00F0` | D0=`$123400F0`,PC=`$1004`,SR=`$0010` |
| T-INS-032-04 | D0=`$000000F0`; words=`$0200,$000F` | D0=`$00000000`,PC=`$1004`,SR=`$0014` |
| T-INS-032-05 | D0=`$00000055`; words=`$0A00,$00FF` | D0=`$000000AA`,PC=`$1004`,SR=`$0018` |
| T-INS-032-06 | D0=`$00000080`,D1=`$00000001`; words=`$8001` | D0=`$00000081`,PC=`$1002`,SR=`$0018` |
| T-INS-033-04 | D0=`$00000023`,D1=`$00000009`,SR=`$0014`; words=`$8101` | D0=`$00000013`,PC=`$1002`,SR=`$0000`,N/V未比較 |
| T-INS-033-05 | D0=`$12340007`,SR=`$0000`; words=`$4800` | D0=`$12340093`,PC=`$1002`,SR=`$0011`,N/V未比較 |
| T-INS-033-06 | D0=`$12340001`; words=`$4400` | D0=`$123400FF`,PC=`$1002`,SR=`$0019` |
| T-INS-033-07 | D0=`$123400F0`; words=`$4600` | D0=`$1234000F`,PC=`$1002`,SR=`$0010` |
| T-INS-033-08 | D0=`$12340080`; words=`$4A00` | D0不変,PC=`$1002`,SR=`$0018` |

### 6.4 シフト、回転、条件分岐、制御フロー

#### 6.4.1 Phase 2Dの境界と条件評価（INS-037）

本節は正常なバス応答下のF軸だけを規定する。分岐、制御EA、スタックread/writeの
フォルト時の部分完了・保存PCはEXC-010、実バス順序はBUS-010、サイクルはTIM-010へ
延期する。`PC0` をopcode wordのアドレス、`PC1=PC0+2` とする。Bcc/BRA/BSR/DBccの
displacementは、拡張wordの有無にかかわらず **PC1** からの符号付きbyteオフセットである。

| cc | 意味 | 真となる条件 |
|---:|---|---|
| 0 | T | true |
| 1 | F | false |
| 2 | HI | `!C && !Z` |
| 3 | LS | `C || Z` |
| 4 | CC (HS) | `!C` |
| 5 | CS (LO) | `C` |
| 6 | NE | `!Z` |
| 7 | EQ | `Z` |
| 8 | VC | `!V` |
| 9 | VS | `V` |
| A | PL | `!N` |
| B | MI | `N` |
| C | GE | `!(N xor V)` |
| D | LT | `N xor V` |
| E | GT | `!Z && !(N xor V)` |
| F | LE | `Z || (N xor V)` |

この表はBcc、DBcc、Sccで共用する。各命令はいずれもCCRを変更しない（シフト／回転を除く）。
`$FF` の8-bit displacementはMC68000ではshort `-1` であり、long displacementはMC68020
以降のみの形式なので本Phaseのvalid規則に含めない。`[PM Bcc, printed pp.4-25–4-26 / PDF pp.128–129]`
`[PM BRA, printed p.4-55 / PDF p.158]`
`[PM BSR, printed pp.4-59–4-60 / PDF pp.162–163]`

#### 6.4.2 分岐・条件設定・制御フロー（INS-038–039）

| 規範ID | opcode/EA | 正常終了時の意味論 |
|---|---|---|
| INS-037 BRA, BSR, Bcc | `6|cc|disp8`; `disp8=0`なら次wordを`disp16`とする。BRA=`cc=0`、BSR=`cc=1`、Bcc=`cc=2..15` | BRAは無条件、Bccは条件表が真なら`PC←PC1+sext(disp)`、偽なら拡張wordの直後へ進む。BSRは次命令アドレス（shortはPC1、wordはPC0+4）を`SP←SP-4; write32(SP,return)`で積み、`PC←PC1+sext(disp)`。 |
| INS-038 DBcc, Scc | DBcc=`$50C8|cc<<8|Dn`, disp16; Scc=`$50C0|cc<<8|data-alterable EA` | DBccは条件が真ならDn.WもPCも（拡張word消費を除き）変更しない。偽ならDn.Wだけをdecrementし、結果が`$FFFF`以外なら`PC←PC1+sext(disp16)`、それ以外は次命令へ進む。Sccは条件によりbyte `$FF`/`$00` をEAへwriteする。両形式ともCCR不変。 |
| INS-039 JMP, JSR, RTS | JMP=`$4EC0|control-EA`; JSR=`$4E80|control-EA`; RTS=`$4E75` | JMPはcontrol EAのaddressをPCへ設定する。JSRはEA extensionをすべて消費してからそのPCをreturn addressとしてlong pushし、EA addressをPCへ設定する。RTSは`PC←read32(SP); SP←SP+4`。いずれもCCR不変。 |

control EAはEA-003,006–011、Sccはbyte data-alterable EA（Dnまたはmemory alterable）だけである。
DBccのDn上位16 bitは保持する。`[PM DBcc, printed pp.4-90–4-91 / PDF pp.193–194]`
`[PM JMP, printed p.4-108 / PDF p.211]` `[PM JSR, printed p.4-109 / PDF p.212]`
`[PM RTS, printed p.4-169 / PDF p.272]` `[PM Scc, printed pp.4-172–4-173 / PDF pp.275–276]`

#### 6.4.3 シフト／回転（INS-040）

レジスタ形式は`$E000|count/reg<<9|dr<<8|size<<6|i/r<<5|type<<3|Dn`、sizeはB/W/L、
`type=00/01/10/11`はAS/LS/ROX/RO、`dr=0/1`はR/Lである。immediate countのfield `0` は8、
register countは指定Dnの下位6 bit（modulo 64）である。memory形式は`$E0C0|type<<9|dr<<8|memory-alterable EA`、wordの1 bit操作だけである。

| family | 正常意味論とCCR |
|---|---|
| ASL/ASR | 左はzero-fill、右は符号bit-fill。X/Cは最後に出たbit、count=0ならX不変/C=0。N/Zは結果、ASLのVは途中でsignが変われば1、ASRのV=0。 |
| LSL/LSR | zero-fill。X/Cは最後に出たbit、count=0ならX不変/C=0。N/Zは結果、V=0。 |
| ROXL/ROXR | Xを含む`size+1` bit ringを回す。X/Cは最後に出たbit、count=0ならX不変かつC=X。N/Zは結果、V=0。 |
| ROL/ROR | Xを含めないsize bit ringを回す。X不変、Cは最後に出たbit（count=0なら0）、N/Zは結果、V=0。 |

各register形式は宛先Dnの操作サイズ以外を保持する。memory形式はRMWでwordを書き戻す。
フォルト時のEA更新・read/write順はBUS-010/EXC-010へ延期する。`[PM ASL, ASR, printed pp.4-21–4-24 / PDF pp.124–127]`
`[PM LSL, LSR, printed pp.4-113–4-115 / PDF pp.216–218]`
`[PM ROL, ROR, printed p.4-160 / PDF p.263]`
`[PM ROXL, ROXR, printed pp.4-163–4-165 / PDF pp.266–268]`

#### 6.4.4 累積opcode規則と適合性ケース（INS-041）

`tests/data/cpu/opcode_rules.csv` に2Dの10形式を加える。`tools/check_opcode_rules.py` は2B/2C/2Dの
97形式について必須ID・必須列・classification・非空集合・PMから独立に固定した形式別件数・
規則間重複を検査する（2Fの65,536 opcode閉包は対象外）。

| case | 初期状態・words | 期待状態 |
|---|---|---|
| T-INS-037-01 | Z=0; `$6602` | BNE.S taken: PC=`$1004`, SR不変 |
| T-INS-037-02 | Z=1; `$6600,$0002` | BNE.W not-taken: PC=`$1004`, SR不変 |
| T-INS-037-03 | SP=`$8000`; `$6100,$0004` | BSR.W: SP=`$7FFC`, mem[SP]=`$1004`, PC=`$1006` |
| T-INS-038-01 | D0=`$ABCD0001`; `$51C8,$FFFE` | DBF: D0=`$ABCD0000`, PC=`$1000`, SR不変 |
| T-INS-038-02 | Z=1,D0=`$ABCD0001`; `$57C8,$0000` | DBEQ: D0不変, PC=`$1004`, SR不変 |
| T-INS-038-03 | Z=1; `$57C0` | SEQ D0: D0.B=`$FF`, SR不変 |
| T-INS-039-01 | A0=`$2346`; `$4ED0` | JMP (A0): PC=`$2346`, CCR不変（奇数アドレスへの分岐は、分岐先の先読みで JMP の中で address error になる。T-TIM-040-05） |
| T-INS-039-02 | SP=`$8000`,A0=`$2000`; `$4E90` then `$4E75` | JSR/RTS: return=`$1002`, SP復帰, PC=`$1002`, CCR不変 |
| T-INS-040-01..10 | ASL,ASR,LSL,LSR,ROXL,ROXR,ROL,RORのregister各1件、count=31のASL.L、ASL memory 1件 | 結果、部分Dn保持、N/Z/V/C/Xおよびmemory RMWを直接比較 |

`tests/cpu/test_phase2d_contract.c` が上表の全caseと16条件×16 NZVC組合せを実行する。

### 6.5 システム・特権・例外依存命令

#### 6.5.1 Phase 2Eの境界（INS-042）

本節は、命令wordのデコード、正常バス応答下のレジスタ／メモリ変化、特権違反または
命令例外を発火する**時点とvector番号**を規定する。例外優先順位、保存PC、SR切替、
スタックフレーム、RTEのframe形式、STOP後の割込み／RESET遷移、RESET命令の外部信号、
TASのlockedバス列、フォルト時の部分完了はそれぞれEXC-010、BUS-010、TIM-010へ延期する。

「特権」は命令wordをfetchした直後、EAまたはimmediateを消費する**前**に判定する。
特権違反はvector 8、ILLEGALはvector 4、CHK失敗はvector 6、TRAPVはvector 7、
TRAP #nはvector `32+n` とする。これらの例外では、Phase 3が規定する例外遷移へ移る。
ただしNOP、MOVE from SR（無印MC68000）、MOVE to CCR、CCR宛て即値論理、RTR、TASは
特権でない。MOVE from CCR (`$42C0|EA`) はMC68010以降専用で、MC68000のvalid規則に
含めない。`[PM CHK, printed pp.4-69–4-70 / PDF pp.172–173]`
`[PM ILLEGAL, printed p.4-107 / PDF p.210]`

#### 6.5.2 SR/CCR/USP操作（INS-043–044）

| 規範ID | opcode/EA | 正常意味論 |
|---|---|---|
| INS-042 MOVE SR/CCR | MOVE SR,EA=`$40C0|data-alterable EA`; MOVE EA,CCR=`$44C0|data EA`; MOVE EA,SR=`$46C0|data EA` | MOVE SR,EAは16-bit SRをwriteし、無印MC68000では非特権。MOVE EA,CCRはsource下位5 bitだけでCCRを置換し、SR上位byteを保持する。MOVE EA,SRは特権で、source wordでSR全体を置換する。いずれも通常の演算CCR更新は行わない。 |
| INS-043 MOVE USP | An→USP=`$4E60|An`; USP→An=`$4E68|An` | 特権。指定AnとUSPの一方を他方へ32-bitコピーし、CCR不変。 |
| INS-044 即値CCR/SR | ORI=`$003C/$007C`、ANDI=`$023C/$027C`、EORI=`$0A3C/$0A7C`、各imm word | `...CCR`はimm下位5 bitをCCRへOR/AND/XORし、非特権。`...SR`はSR全体へ同じ演算を行う特権形式。いずれもsizeはCCR=byte、SR=wordで、imm word以外のextensionを持たない。 |

`[PM MOVE to CCR, printed pp.4-123–4-124 / PDF pp.226–227]`
`[PM MOVE from SR, printed p.4-125 / PDF p.228]`
`[PM ANDI to CCR, printed p.4-20 / PDF p.123] [PM ORI to CCR, printed p.4-155 / PDF p.258] [PM EORI to CCR, printed p.4-104 / PDF p.207]`

#### 6.5.3 CHK、基本システム、TAS（INS-045–048）

| 規範ID | opcode/EA | 正常意味論または例外発火点 |
|---|---|---|
| INS-045 CHK.W | `$4180|Dn<<9|data EA` | signed Dn.Wをzeroおよびsigned source.W上限と比較する。`Dn<0 || Dn>source`ならNを負値時1・上限超過時0としてvector 6を発火する。範囲内ではDn/CCR不変。Z/V/CはCHK失敗時未定義なのでF軸比較から除外する。 |
| INS-046 ILLEGAL | `$4AFC` | vector 4を発火する。未割当encodingもvector 4だが、分類は`illegal encoding`であり`valid:ILLEGAL`ではない。 |
| INS-047 NOP/RESET/STOP/RTE/RTR | NOP=`$4E71`; RESET=`$4E70`; STOP=`$4E72,imm16`; RTE=`$4E73`; RTR=`$4E77` | NOPはPC以外不変。RESET/STOP/RTEは特権であり、特権ならそれぞれ外部RESET要求／SR←immとstopped状態／例外frame復帰を開始する。RTRは`CCR←read16(SP)[4:0]; PC←read32(SP+2); SP←SP+6`。RTEのframe解釈、STOP待機、RESET信号幅は後続規範へ延期する。 |
| INS-048 TAS | `$4AC0|data-alterable EA` | old byteからN/Zを設定、V=C=0、X不変とし、destination byteのbit7を1にしてwriteする。memory形式はRMWである。 |

`[PM NOP, printed p.4-147 / PDF p.250]` `[PM RTR, printed p.4-168 / PDF p.271]`
`[PM TAS, printed pp.4-186–4-187 / PDF pp.289–290]`

#### 6.5.4 TRAP形式と後世代境界（INS-049–050）

TRAP=`$4E40|vector4` はvector `32+vector4` を発火し、CCRを変更しない。TRAPV=`$4E76`
はV=1ならvector 7を発火、V=0ならPCを進めるだけでCCR不変である。TRAPのcallback等の
実装上の便宜は規範の代替ではなく、callback未登録時はこのvector契約に従う。

MC68000のvalid集合から除外する代表形式は、MOVE from CCR、CHK.L、BKPT、RTD、RTM、
MOVEC/MOVES、TRAPcc、LINK.L、EXTB.L、long branch、bitfieldである。これらがMC68000の
既存形式と重ならない限り2Fで`illegal encoding`へ分類し、Line-A/F固定範囲はそちらを優先する。
`[PM TRAP, printed p.4-188 / PDF p.291]`
`[PM TRAPV, printed p.4-191 / PDF p.294]`

#### 6.5.5 累積opcode規則と適合性ケース（INS-051）

`tests/data/cpu/opcode_rules.csv` の2Eは21形式であり、2B–2E累積118形式を
`tools/check_opcode_rules.py` が検査する。検査項目は必須ID・必須列・classification・非空集合・
PMから独立に固定した形式別件数・規則間重複である。2Fまで65,536 opcode閉包は主張しない。

| case | 初期状態・words | 期待状態 |
|---|---|---|
| T-INS-042-01 | SR=`$2715`; `$40C0` | D0.W=`$2715`, SR不変（MC68000 user/supervisorを問わず） |
| T-INS-042-02 | SR=`$2700`,D0=`$001F`; `$44C0` | SR=`$271F` |
| T-INS-042-03 | supervisor,D0=`$2001`; `$46C0` | SR=`$2001` |
| T-INS-043-01/02 | supervisor,A0=`$12345678`; `$4E60` / USP=`$87654321`; `$4E68` | An→USP / USP→An、CCR不変 |
| T-INS-044-01..06 | ORI/ANDI/EORIのCCR/SR各形式 | imm word、CCR下位5 bit、SR全体、特権境界を直接比較 |
| T-INS-045-01/02 | CHK.W #10,D0; D0=`10` / D0=`11` | 前者は例外なし、後者はvector 6 |
| T-INS-046-01 | `$4AFC` | vector 4 |
| T-INS-047-01..03 | NOP / RTR stack frame / STOP #`$2700` | PC以外不変 / CCR+PC+SP復元 / stopped状態 |
| T-INS-048-01 | D0.B=`$00`; `$4AC0` | D0.B=`$80`, Z=1,N=V=C=0,X不変 |
| T-INS-049-01..03 | TRAP #3 / TRAPV with V=1 / TRAPV with V=0 | vector 35 / vector 7 / 例外なし |

`tests/cpu/test_phase2e_contract.c` は上表を実行し、例外ケースではvector番号とhandler到達を確認する。

### 6.6 全opcode分類の閉包（Phase 2F）

#### 6.6.1 生成元と分類順序（INS-052）

`tests/data/cpu/opcode_rules.csv` を唯一の生成元とする。`tools/check_opcode_rules.py` は各規則の
`mask/value`とEA述語を16-bit全域へ展開して次の順序で `tests/data/cpu/opcode_classification.csv` を生成する。

1. `$A000–$AFFF` を `Line-A`、`$F000–$FFFF` を `Line-F` とする。個別valid規則がこの範囲を
   上書きしようとした場合は失敗とする。
2. 残るopcodeは、ちょうど1つの `valid:<form-id>` 規則に一致しなければならない。一致規則の
   `form_id`、`phase`、`source`、`norm_ids`をそのまま転記する。
3. valid規則に一致しない残余は `illegal encoding`、`ILLEGAL_ENCODING`、`2F`、
   `PM:Appendix_A`、`INS-052` とする。これは`$4AFC`の`valid:ILLEGAL`とは区別する。

生成物の1行は `opcode,classification,form_id,phase,source,norm_ids` であり、opcodeを`$0000`から
`$FFFF`まで昇順に1行ずつ持つ。再生成は次で行い、差分があれば成果物と規則を同一コミットで更新する。

```sh
python3 tools/check_opcode_rules.py --write-classification tests/data/cpu/opcode_classification.csv
python3 tools/check_opcode_rules.py --check-classification tests/data/cpu/opcode_classification.csv
```

#### 6.6.2 閉包受入結果（INS-053）

固定規則118形式から得る分類件数は次のとおりである。全行に規範IDと一次資料の追跡情報があり、
重複・未分類はいずれも0である。

| classification | 件数 | 追跡元 |
|---|---:|---|
| `valid:<form-id>` | 45,817 | `opcode_rules.csv`の各行 |
| `illegal encoding` | 11,527 | `INS-052`, PM Appendix A |
| `Line-A` | 4,096 | `INS-001`, PM Appendix A |
| `Line-F` | 4,096 | `INS-001`, PM Appendix A |
| **合計** | **65,536** | 全16-bit opcode |

`test_opcode_closure`はCIで規則の妥当性、分類の完全閉包、固定生成物との全65,536行一致を検査する。
この閉包は無印MC68000モデルの命令可用性の規範であり、68010以降のモデル別decode tableの
可用性はそれぞれの設計書で別に規定する。`[PM Appendix A Table A-3, printed pp.A-12–A-13 / PDF pp.607–608]`

### 6.7 命令別の EA 制約と CCR 更新規則

命令別の許可EAとCCR更新規則の正本は §6.2〜§6.5 の規則表である（`tests/data/cpu/opcode_rules.csv` は opcode 集合と分類の正本で、CCR 列は持たない）。
横断表は同じ情報の重複になり、更新時に不一致を生むため作成しない。

---

## 7. 特権状態とスタック

MC68000にはUSPとSSPの二つのスタックポインタがある。SR.S=0ではA7はUSP、SR.S=1では
SSPを指す。Sが変化するときは、旧A7を旧側へ退避して新側からA7をロードする。例外入口は
保存SRを作る**前**にSSPへ切り替え、RTEは復元SRに従って切り替える。
`[PM §1.1.2, printed p.1-2 / PDF p.12]` `[PM RTE, printed p.6-84 / PDF p.537]`

### 7.1 特権命令の一覧

`RESET`、`STOP`、`RTE`、`MOVE An,USP`、`MOVE USP,An`、およびSR全体を書き換える
`MOVE <ea>,SR`／`ANDI`／`ORI`／`EORI` to SRは特権である。S=0で実行した場合、opcodeを
取得した直後、追加の拡張語・EA読出し・外部RESET要求・スタックpopより前にvector 8
（Privilege Violation）へ移る。保存PCは`PC₀`である。CCRだけを操作する命令と`MOVE SR,Dn`
はこの規則の対象外である。`[PM Appendix B Table B-1, printed p.B-2 / PDF p.628]`

**PRV-010**: この判定は命令の副作用より優先し、例外遷移はEXC-010に従う。

### 7.2 SR 書換えとスタックポインタ切替

SRを全体書換えする成功命令、STOP、RTE、例外入口のすべてに同じS遷移規則を用いる。
例外入口では保存SRを変更せず、実行中SRだけを`S=1, T=0`へ更新する。割り込み入口はさらに
I2:I0を受理levelに置換する。従ってユーザ例外の6 byte frameはSSP上にあり、RTEがS=0を
復元すれば、pop後のSSPを保存してUSPをA7へ戻す。

**PRV-020**: USP/SSPの切替はSR.S遷移と不可分のアーキテクチャ状態更新であり、A7の値を
コピーして両方へ残してはならない。

### 7.3 STOP、RESET、RTE、RTR

RTEはSSP先頭から`SR=read16(SP)`、`PC=read32(SP+2)`を復元してSPを6増やす。MC68000には
後世代のformat/offset wordはない。RTRはSR全体やSを変えず、`CCR←read16(SP)[4:0]`、
`PC←read32(SP+2)`、`SP←SP+6`とする。`[PM RTE, printed p.6-84 / PDF p.537]`

STOPは即値をSRへロードし、PCを次命令へ進めてfetch/executeを停止する。STOP開始時にtraceが
有効ならtrace例外へ移り停止状態に入らない。新SRのmaskより高い割り込み、または外部resetは
停止を解除する。RESET命令は外部周辺機器reset要求であり、CPU自身のreset vector再読込みでは
ない。`[PM STOP, printed pp.6-85–6-86 / PDF pp.538–539]`

**PRV-030**: STOP中は受理可能な例外が来るまで命令fetchを行ってはならない。

---

## 8. 例外処理

例外は、(1)旧SRを保存し、(2)SSPへ切替、(3)実行SRを`S=1,T=0`へ更新、(4)種別固有frameを
積み、(5)vector tableの`4×vector`からhandler PCを取得する。MC68000のVBRは常に0である。
frame pushまたはvector fetchが失敗した場合の扱いは例外の種類によって異なる（§8.3、§8.4）。`[PM Appendix B, printed pp.B-1–B-3 /
PDF pp.627–629]`

### 8.1 例外ベクタと発生条件

MC68000で本書が規定するvectorは、bus/access fault=2、address error=3、illegal=4、divide by
zero=5、CHK=6、TRAPV=7、privilege=8、trace=9、Line-A=10、Line-F=11、spurious IRQ=24、
autovector level *n*=24+*n*、TRAP #n=32+n、user-defined=64–255である。予約vectorを
命令独自の例外へ転用してはならない。`[PM Appendix B Table B-1, printed p.B-2 / PDF p.628]`

**EXC-010**: Group 1（illegal/privilege/trace/Line-A/Line-F）は`PC₀`を保存する。Group 2
（TRAP、TRAPV、CHK、除算ゼロ）は正常に消費済みの`PCₙ`を保存する。特権違反はPRV-010の
例外としてGroup 1に属する。

### 8.2 Group 1/2 の 6 バイトフレーム

SPから高位アドレスへの順に`[+0]=saved SR`、`[+2]=saved PC (long)`の6 byteとする。
push時の物理アクセス順・FC・cycleはB/T軸として§10へ延期するが、このframeの内容、長さ、
handler入口時のSPはX軸必須である。`[PM Appendix B Figure B-1, printed p.B-3 / PDF p.629]`

**EXC-011**: handler実行中のSRはS=1かつT=0であり、saved SRは例外前の値そのままとする。

### 8.3 バス／アドレスエラーの 14 バイトフレーム

Group 0 frameは14 byteである。SPから順に`SSW`、fault access address (long)、instruction
register (word)、saved SR (word)、saved PC (long)を置く。SSWはR/W（read=1）とI/N
（instruction=0, not-instruction=1）、function codeを含む。`[PM Appendix B Figure B-2, printed p.B-3 / PDF p.629]`
saved PCは、フォルトした参照を行った命令の最初の語のアドレス（`PC₀`）から2〜10 byte先の値であり、`PC₀` そのものではない。
命令フェッチ中のフォルトでは現在の命令の近傍の値になる。正確な値はTIM-040のプリフェッチ進行に依存し、
命令別の期待値には使わない。`[UM §6.3.9.1, printed p.6-16 / PDF p.101]` Group 1／Group 2 の例外と割り込みの処理の最終段（vectorの読出し、またはhandler先頭の命令フェッチ）で
バスエラーが起きた場合は、その例外vectorのアドレスを saved PC とする bus error 例外を取る（同節）。
Group 0 例外の処理中の同種の失敗は二重フォルトである（§8.4）。

**EXC-020**: vector 2/3と失敗accessの種別を混同せず、14 byte frameを構築する。saved PCの値は TIM-040 の範囲規則で検証する。命令別の
部分完了と通常アクセスの厳密順はEA-015/016および§10の未解決範囲に残す。

### 8.4 二重フォルトと停止状態

次の失敗は二重フォルトであり、CPUは`halted`になる。以後の通常実行と割り込み受理は行わず、ホストによる
外部resetだけが通常状態を再確立する [UM §6.3.9.1, printed p.6-16 / PDF p.101] [UM §5.4.4, printed p.5-28 / PDF p.74]。

- bus error または address error の例外処理中（frameのpush、vectorの読出し、handler先頭の命令フェッチ）の bus error。
- reset 処理中の vector table 読出し（初期SSP、初期PC）での bus error [UM §5.4.4]。

Group 1／Group 2 の例外と割り込みの処理の最終段（vectorの読出し、handler先頭の命令フェッチ）の失敗は
二重フォルトではなく、§8.3 のとおり、vectorのアドレスを saved PC とする bus error 例外になる。

Group 1／Group 2 の例外と割り込みの frame push の失敗は、UM が結果を定めない。続く bus error／address error の
frame push が同じスタック領域への書込みで失敗する場合は、上の規則により halted になる。それ以外の場合の結果は
本書では規定せず、ケースにも使わない。これはX軸の状態遷移規則であり、停止までの電気的バス時刻は規定しない。

**EXC-030**: 二重フォルト（§8.4）はHALTへ移り、HALTに入る前に成功済みのwriteやSP変更を巻き戻さない。Group 1／Group 2 の例外と割り込みのvector読出し、およびhandler先頭の命令フェッチの失敗は、vectorのアドレスを saved PC とする bus error 例外になる（MC68000。68010 は vector 読出しの失敗だけが対象で、saved PC は例外が起きた時点の PC）。

### 8.5 トレース例外

T=1で開始した命令が正常完了した後、vector 9を発生する。Group 1が命令を中断した場合traceは
発生しない。Group 2は完了例外なのでtraceが追随し、Group 2 frameの上にtrace frameを積む。
STOP開始時のtraceは§7.3に従う。`[PM Appendix B Table B-1, printed p.B-2 / PDF p.628]`

**EXC-040**: trace frameのsaved SRには例外前のTを含み、handler中のTは0である。

---

## 9. 割り込みとリセット

IPLは命令境界でsampleする。入力level 1–6は`level > SR.I2:I0`のとき、level 7はmaskを
貫通して受理する。受理時の保存PCは次にfetchする命令の`PC₀`であり、通常の6 byte frameを
使う。handler中のSRはS=1、T=0、mask=受理levelである。

### 9.1 命令境界での割り込み受理

**IRQ-010**: 割り込みは命令の途中では受理しない。STOP中も同じmask判定を適用し、受理時だけ
`stopped → normal → interrupt exception`へ遷移する。未受理の入力はPC、SR、SPを変更しない。

レベル7はNMIとして扱う。level-sensitive入力をhostがpullで供給するときは、入力が一度7未満へ
戻るまで同じassertionを再受理しない。これはhandler中の無限再入を防ぐAPI契約であり、latch型
IRQのassert/deassert管理はhost側の責務とする。

### 9.2 割り込み acknowledge とベクタ選択

IACK応答がvector番号を返せばそのvectorを用いる。応答がautovectorを示せばlevel *n*に対して
`24+n`を用いる。IACK中のbus error（デバイス無応答）は、返却値にかかわらずspurious vector 24を
用いる。各番号の割当はTable B-1に従う。`[PM Appendix B Table B-1, printed p.B-2 / PDF p.628]`

**IRQ-020**: IACKの失敗をGroup 0 bus-error frameへ変換してはならない。IRQ frameは標準6 byteの
ままとし、vector選択だけが24になる。

### 9.3 リセットシーケンス

外部resetはhalted/stoppedを含む実行状態を解除し、SRを`$2700`（S=1、mask=7、trace=0）へ設定して、
address `$000000`から初期SSP、`$000004`から初期PCを読む。reset vector読出しは通常命令の
Group 0例外には変換しない。D0–D7/A0–A6およびUSPはリセット後未定義であり、適合性ケースでは
値を要求しない。Table B-1のvectors 0/1が初期SSP/PCである。`[PM Appendix B Table B-1,
printed p.B-2 / PDF p.628]`

**RST-010**: RESET命令（PRV-010）はこの外部reset state transitionを起動しない。ホストが外部resetを
明示した場合のみ本節のvector再読込みを行う。

---

## 10. タイミング、プリフェッチ、バスシーケンス

この節は適合性軸 B（バス順序）と T（時間精度）の規範範囲を定める（Phase 4）。ns単位の電気的
ピンタイミングは §2.1 により対象外である。各項目を「厳密再現」（B/T軸の必須）と「許容近似」
（未達を明記すれば適合とみなせる差）に分ける。許容近似を採る実装は §2.2 のとおり、近似範囲・
観測可能な差・利用上の制約を明記しなければならない。

| ID | 項目 | 分類 | 根拠 |
|---|---|---|---|
| TIM-010 | 標準バスサイクルは wait なしで4クロック | 厳密再現（T軸） | [UM §5.8 / PDF p.81]、[UM §8 intro / PDF p.117] |
| TIM-011 | 命令サイクル表 `n(r/w)` の n は命令フェッチ・オペランドフェッチ・ストアを含む総クロック数 | 厳密再現（T軸） | [UM §8 intro NOTE / PDF p.117] |
| TIM-012 | wait state は表の値に整数クロック単位で加算する | 厳密再現（T軸） | [UM §8 intro / PDF p.117] |
| TIM-013 | 表の r/w はバスサイクル数であり、順序は §4・§5・§6 のバス規範が定める | 厳密再現（B軸） | [UM §8 intro / PDF p.117] |
| TIM-020 | 命令内のバスアイドル期間（n − 4(r+w)）の配置位置 | 許容近似 | UMはクロック数のみ規定し、アイドルの位置を規定しない |
| TIM-030 | TAS の RMW を不可分バスサイクルとして外部に提示する（AS 保持） | 厳密再現（B軸）。ただし TIM-032 の抽象バス条件付き | [UM §5.1.3 / PDF p.53] |
| TIM-031 | RMW の bus error は retry せず bus error 処理（HALT 不問） | 厳密再現（X/B軸） | [UM §5.4.1 / PDF p.70]、[UM §5.4.2 / PDF p.73] |
| TIM-040 | 命令プリフェッチの語数、補充時点、分岐時の不要fetch | 厳密再現（F/B軸） | [M68K83 §7.1.2, printed p.7-4] |

### 10.1 サイクル会計の単位

**TIM-010** 標準バスサイクルは4クロック周期（8バス状態 S0〜S7）で、wait state を挿入する場合は
整数クロックを追加する（挿入位置は UM の bus cycle 状態図に従い、本書は位置を規範にしない） [UM §5.8 / PDF p.81]。wait なしの read／write の
サイクル数は 4 とする。

**TIM-011** 命令の公称サイクルは `n(r/w)` で表す。n は総クロック数、r は read バスサイクル数、
w は write バスサイクル数である。n は命令フェッチ、全オペランドフェッチ、全ストアを含む
[UM §8 intro NOTE / PDF p.117]。EA計算時間は §8.1 の Table 8-1（PDF p.118）で
規定され、拡張語フェッチ、アドレス計算、メモリオペランドフェッチを含む。

**TIM-012** バス応答側がサイクルを延長すると、延長したクロック数 k を命令の n に加算する
[UM §8 intro / PDF p.117]。k は1バスサイクルごとの整数クロックとする。命令全体の
サイクル数は `n_table + Σk` である。

**TIM-013** バスサイクルのうち read の総数は r、write の総数は w に一致しなければならない。
TAS の表値は `14(2/1)+`（メモリ形式）で、RMW は read 部と write 部から成る
[UM §5.1.3 / PDF p.53]、[UM §8 単一オペランド命令の表 / PDF p.122]。表値 r=2 の内訳
（命令フェッチと RMW read 部の分け方）は UM に明記がなく、本書は規範としない。

**TIM-020（許容近似）** n が 4(r+w) を超える分は、バスがアイドルの内部演算期間である
[UM §8 intro / PDF p.117]。この期間をバスサイクル間のどこに置くかは UM が規定しない。
サイクル精度を命令単位で合わせる実装は、この配置を任意にしてよい。B軸（バス順序）の
判定はアイドル位置に依存してはならない。

**検証への帰結** T軸の合否は、TIM-010〜013 から導く命令別の `n_table + Σk` と、観測された
バスサイクル列で判定する。アイドル配置、E クロック同期（VMA/VPA）、割り込みサンプル点の
クロック位置は本節の対象外とする。

### 10.2 プリフェッチと自己書換えコード

**TIM-040** MC68000 は2語の密結合プリフェッチ機構を使う。命令実行開始時には、命令語は
decoderにあり、次の1語も取得済みである。複数語命令は各追加語を内部で消費するたびに、命令ストリームから1語を補充する。
その命令に属する最後の補充は、命令語を捨てて次命令のdecodeを開始するときに行う
[M68K83 §7.1.2, printed p.7-4]。

追加語の消費で補充するのは次に使う1語だけで、最後の補充は命令完了時に行う。分岐・ジャンプ（Bcc、BRA、BSR、DBcc、JMP、JSR）と STOP は、
追加語の消費時に逐次側の語を読まない（UM Table 8-9, 8-10, 8-12 の read 数に合わせる）。DBcc のカウンタ満了は、
分岐先の1語を使わずに読み、その後に逐次側の2語を補充する（UM Table 8-9 の 14(3/0)）。CCR／SR への即値演算は
キューを破棄して2語を再充填する（UM Table 8-12 の 20(3/0)）。分岐後の次命令語の取得で bus error／address error が起きた場合、saved PC は分岐命令の語の末尾（`PC₀+2`〜`PC₀+10`）にする
[UM §6.3.9.1, printed p.6-16 / PDF p.101]。先読みで起きた bus error／address error は、先読みを行った命令（または例外処理）の中で報告する。
Group 0 は実行中の命令を中断して例外処理に入り [UM §6.2.3, printed p.6-8]、分岐先の2語は分岐命令自身の
read 数 r に数えられるため、そのフォルトは分岐命令の中で起きる。このとき命令は bus error で中断されるので、
trace 例外や保留中の割り込みより先に bus error が処理される [M68K83 §5.2.4, printed p.5-5]。
キューへの先読みが失敗した場合は、その語が使われるかどうかにかかわらず同様である
（先読みは命令自身の bus cycle である、という上の根拠から導いた推論）。拡張語の消費時の補充の失敗も、
命令のデータアクセスより前に報告する（M68K83 §5.2.4: Group 0 は実行中の命令を中断する）。
以上の個別規則は UM の read 数からの**推論**であり、
バス上の読出し順は M68K83 から直接は確定できない。
MOVEM メモリ→レジスタは最後のレジスタの次の語を余分に read し、CLR・Scc・MOVE from SR のメモリ形式は
書込みの前に同じアドレスを一度 read する（UM Table 8-6, 8-10, 8-12 の r。値は捨てる）。
JMP の (d8,An,Xn)／(d8,PC,Xn) だけは、Table 8-10 の 14(3/0) に合わせて拡張語の消費時の補充を行う
（同表の JSR 22(2/2)、LEA 12(2/0)、PEA 20(2/2) の r は2であり、JMP の行と整合しないため §14.4 に記録する）。

1. 単語の分岐命令では、先読み済みの次語を使わないが、そのfetch自体は発生する。
2. interruptまたはtrace例外では、先読み済みの2語を使わない。
3. PCは通常、命令ストリームから最後に取得した語を指す。
4. 実行中の命令が直後の命令語を書き換える場合、既に先読み済みなら書換え前の語を実行する。先読みされていない語への書込みは、補充時に観測される値を使う。

データアクセスのbus error／address errorで積まれる saved PC は、その参照を行った命令の最初の語の
アドレス `PC₀` に対し `PC₀+2 ≤ saved PC ≤ PC₀+10`（偶数）を満たす
[UM §6.3.9.1, printed p.6-16 / PDF p.101]。具体値は命令のプリフェッチ進行に依存するため、期待値に使わない。

資料から確定できる規範は次のとおりである。

- **TIM-041** 命令ストリームの read は、supervisor program（FC=`%110`）または user program
  （FC=`%010`）空間の read バスサイクルとして外部に現れる [UM Table 3-3 / PDF p.37]。
  その総数はサイクル表の r に含まれる。
- **TIM-U4（未規定、§14.2）** 命令内で、拡張語のフェッチが EA 計算に先立つかなど、バス上の順序は M68K83 §7.1.2 も定めない。
  語数と補充時点（TIM-040）および r／w の総数（TIM-013）は規範とし、この順序は期待値に使わない。
- **TIM-U3（観測保留・適合性対象外、§14.2）** 命令内のアイドル期間のピン位相は、命令単位の
  総クロック数と bus cycle数からは決まらない。実機観測が得られるまで許容近似とし、本書の適合性
  判定および Issue #34 の受入れ条件には用いない。

### 10.3 TAS の read-modify-write 原子性

**TIM-030** TAS は MC68000 で唯一、read-modify-write（RMW）バスサイクルを用いる命令で、
オペランドは常にバイトである [UM §5.1.3 / PDF p.53]。RMW の間、AS は assert されたままで、
サイクルは不可分である。

規範は次のとおり。

1. TAS のデータオペランドアクセスは、`read8(a)` の直後に `write8(a, v | $80)` を行う
   1つの不可分なバスアクセスである。
2. read と write の間で AS は assert されたままであり、その間に他のバスサイクルを挟まない
   [UM §5.1.3 / PDF p.53]。
3. CCR と write 値は INS-048（§6.5.3）に従う。write するのは old byte の bit 7 を 1 にした値である
   [UM §2 Table 2-2 TAS / PDF p.28]。
4. オペランドは byte アクセスであり、アラインメントは §4 に従う。

**TIM-031（バスエラー、X/B軸）** UM が規定するのは次の3点である。(a) read 部、通常の write、
RMW の read 部の bus error は S9 で終了する。(b) RMW の write 部は S21 で終了する
[UM §5.4.1 / PDF p.70]。(c) RMW はリトライせず、HALT が assert されていても bus error 処理を
行う [UM §5.4.2 NOTE / PDF p.73]。read 部で失敗したとき write 部が行われないことは
これらからの帰結（推論）であり、例外フレームの内容は §8 に従う。

**TIM-032（抽象バスでの扱い）** 抽象バス契約（§4）が RMW 属性を持たない実装でも、TAS は
上記 read→write の順序を守らなければならない。RMW 属性を持つ実装は、その属性を外部に
通知してよい。通知しない場合の差（マルチプロセッサ環境での排他）を明記する。
マルチバスマスタを持たない X68000 のような構成では、この差は観測されない。

---

## 11. 公開 API とデバッグ観測点

**範囲外（Issue #34 の完了条件に含めない）。** 公開 API・ライフサイクル・デバッガ観測は、
§2.1 により CPU 規範の対象外であり、ホスト言語や実装構成に依存する。本書が規定するのは
バス応答・割り込み入力・リセット入力が CPU に渡す契約（§4・§9・§10）までとする。
API 契約は実装側の設計書（`docs/mc68000-internals/reference/cpu_68000.md` ほか）で扱う。

### 11.1 ライフサイクルとモデル固定

範囲外（上記および §2.1）。

### 11.2 バスおよび例外通知の API 契約

範囲外（上記および §2.1）。

### 11.3 非侵襲観測と実行制御

範囲外（上記および §2.1）。

---

## 12. 検証計画

資料上の規則からテストケースを導出し、命令意味論、例外フレーム、割り込み、バス順序、
タイミングを独立に検証する。ケースは §2.5 の形式に従い、対応表は §12.5 にある。
逆アセンブルは本書の範囲外とする。

### 12.1 命令セット網羅性

ケースID `T-<規範ID>-nn` は規範IDごとに連番とし、全規範IDとケースIDの対応は §12.5 の
`tests/data/cpu/conformance_map_mc68000.csv` で機械検査する。命令セット網羅性は次のケースで判定する。

| ケース | 規範ID | 軸 | 検証内容 | 機械テスト |
|---|---|---|---|---|
| T-INS-041-01 | INS-041 | F | 2B–2D 累積の規則CSVが必須ID・必須列・分類・非空集合・形式別件数・規則間重複の検査を満たす | `tools/check_opcode_rules.py` |
| T-INS-051-01 | INS-051 | F | 2B–2E 累積118形式について同上の検査を満たす | `tools/check_opcode_rules.py` |
| T-INS-052-01 | INS-052 | F | 規則CSVから全65,536 opcodeを再分類した結果が固定生成物 `opcode_classification.csv` と1行ずつ一致する | `test_opcode_closure` |
| T-INS-053-01 | INS-053 | F | 分類件数が valid 45,817 / illegal encoding 11,527 / Line-A 4,096 / Line-F 4,096、重複0・未分類0 | `test_opcode_closure` |

### 12.2 境界値・CCR・EA の組合せ

境界値・CCR・EA の組合せは §6.2.4（T-INS-011〜022）、§6.3.4（T-INS-026〜034）、§6.4.4
（T-INS-037〜039）、§6.5.5（T-INS-042〜049）の各ケース表を正本とする。これらは「初期状態→期待最終
状態」の形式で、サイクル数とバストレースは含まない（軸は F）。B/T 軸のケースは §12.6 に置く。
EA 規範（EA-001〜018）と REG／BUS 規範のケースは §12.8 に置く。EA-015・016 は未定義の値を比較しない形でケースを置く。

### 12.3 例外・割り込み・リセットのフレーム検証

Phase 3のX軸ケースはhandler入口、RTE復帰、またはreset後を観測点とする。B/T軸を`N/A`にした
ケースはframeの内容と状態遷移だけを合否に使う。`tests/cpu/test_phase3_contract.c`は次の中核ケースを
1プロセスで実行し、既存の詳細回帰テストは右欄の範囲を補う。

| ケース | 規範ID | 軸 | 検証内容 | 機械テスト |
|---|---|---|---|---|
| T-PRV-010-01 | PRV-010, PRV-020 | X | user ILLEGALがSSPへ6 byte frameを積みRTEでUSPへ戻る | `test_phase3_contract:test_user_exception_and_rte` |
| T-EXC-010-01/02 | EXC-010/011 | X | Group 1=`PC₀`、Group 2=`PCₙ`、saved SR/handler SR | `test_phase3_contract:test_user_exception_and_rte`, `:test_trap_saves_next_pc` |
| T-EXC-020-01 | EXC-020 | X | address errorのSSW、fault address、IR、SRの14 byte frame（saved PCは T-TIM-040-01） | `test_phase3_contract:test_address_error_frame`; `test_bus_error` |
| T-EXC-030-01/02 | EXC-030 | X | group-0/IRQ frame処理中faultでHALT、外部resetで回復 | `test_debug_run:test_stops_at_halted`; `test_phase3_contract:test_irq_vector_choices` |
| T-EXC-030-03〜06 | EXC-030 | X | bus error 例外のvector読出し／handler先頭の命令フェッチの失敗はHALT（-03、-04）。TRAP のvector読出し／handler先頭の命令フェッチの失敗は、vectorのアドレスを saved PC とする bus error 例外（-05、-06） | `test_conformance_timing:test_group0_vector_read_fault_halts`、`:test_group0_handler_fetch_fault_halts`、`:test_vector_read_fault_takes_bus_error`、`:test_handler_fetch_fault_takes_bus_error` |
| T-EXC-030-07〜18 | EXC-030 | X | 分岐先の先読みの失敗が trace 例外（T=1）より先に処理される（-07）。マスク解除命令（MOVE #,SR）の中の先読み失敗が、保留中の割り込みより先に処理される（-08）。使われない語の先読みの失敗も bus error になる（-09）。命令の途中で割り込みが保留になっても、Group 0 の最終段の失敗は HALT、TRAP の最終段の失敗は bus error 例外になる（-10）。Group 0／1 の例外処理中の失敗は SSW の I/N=1、Group 2（TRAP）は I/N=0（-11、-12）。handler 先頭の取得の失敗と IRQ 受理時の先読み位置（-13）。拡張語の消費時の補充の失敗（-14）。CLR の書込み前 read の失敗とモデル差（-15）。補充の失敗の後にデータ read・An の更新・SR/CCR の更新が出ないこと（-16）。68020 では trace の vector 読出し失敗を同じ実行呼出しの中で即時 dispatch しないこと（-17）。変位語の取得が失敗した BSR.W が push を行わず、BRA.W・Bcc.W も bus error 例外になること（-18） | `test_conformance_timing` の `test_branch_fetch_fault_beats_trace`、`test_prefetch_fault_beats_pending_irq`、`test_unused_prefetch_fault_raises_bus_error`、`test_exception_entry_fault_beats_irq`、`test_exception_entry_fault_ssw_in`、`test_vector_group_ssw_in`、`test_handler_fetch_fault_in_and_irq_order`、`test_extension_refill_fault_is_immediate`、`test_read_before_write_fault_and_model`、`test_refill_fault_has_no_later_side_effects`、`test_trace_entry_fault_model_limited`、`test_branch_word_refill_fault_no_side_effects` |
| T-EXC-040-01/02 | EXC-040 | X | completed instructionでtrace、Group 1ではtrace抑止 | `test_phase3_contract:test_trace_priority`; `test_trace` |
| T-IRQ-010-01 | IRQ-010 | X | mask/level 7、命令境界、STOPからの受理 | `test_irq`, `test_irq_pull` |
| T-IRQ-020-01..03 | IRQ-020 | X | autovector、vectored、IACK bus-error→spurious | `test_phase3_contract:test_irq_vector_choices`; `test_spurious_irq` |
| T-RST-010-01 | RST-010 | X | `$000000/$000004`再読込とSR=`$2700` | `test_phase3_contract:test_stop_wake_and_reset` |

#### 12.3.1 §12.3 のケースの初期条件（§2.5 の必須項目）

共通条件: メモリは RAM 64 KiB（`$0000–$FFFF`）で、先頭に reset vector（SSP=`$8000`、PC=`$1000`）を置き、`mc68k_reset` 後に開始する
（初期 SR=`$2700`、wait state 0、バス応答は常に DTACK、BERR／HALT 入力なし）。命令は `$1000` から配置する。handler は
`$1800` とし、使うvectorへ設定する。実行量は 1 命令（STOP wake は後述）、観測点は handler 入口（RTE のケースは復帰後）である。
表にないレジスタ・メモリは未変更を期待し、SR の bit 11 と bits 7:5 は比較しない。バストレースとサイクル数は検査しない（B／T 軸は `N/A`）。

| ケース | 初期条件（共通条件への追加） | 値を問わない項目 |
|---|---|---|
| T-PRV-010-01 | SR=`$0000`（user）、USP=A7=`$7000`、vector 4=handler、`$1000`=`$4AFC`（ILLEGAL）、handler の命令=RTE（`$4E73`） | なし |
| T-EXC-010-01/02 | 同上（-01）。vector 35=handler、`$1000`=`$4E43`（TRAP #3）（-02） | なし |
| T-EXC-020-01 | vector 3=handler、`$1000`=`$4A50`（TST.W (A0)）、A0=`$1001` | なし（SSW の read bit、fault address、IR、SR を比較。saved PC は T-TIM-040-01） |
| T-EXC-030-01 | SSP=未マップ領域（reset vector で指定）、`$1000`=`$207C`＋未マップ address（MOVEA.L #addr,A0）、続けて `$4A10`（TST.B (A0)）。read が BERR になり、frame の push も失敗する（`test_debug_run` の構成で、RAM 構成が異なる） | 例外 frame の内容（HALT へ遷移することだけ比較） |
| T-EXC-030-02 | A7=SSP=`$8001`（奇数）、SR=`$2000`、IRQ level 3 を入力、IACK はオートベクタ | 例外 frame の内容（HALT へ遷移することだけ比較） |
| T-EXC-030-03 | A0=`$F00000`、`$1000`=`$3210`（MOVE.W (A0),D1）。vector 2 のエントリ（`$8`〜`$B`）の読出しが BERR | HALT へ遷移 |
| T-EXC-030-04 | A0=`$F00000`、`$1000`=`$3210`。vector 2=`$F00000`（BERR 領域）。bus error 例外の後、handler先頭の命令語の取得が失敗する | 1命令目の後は halted でなく、2回目の step で HALT へ遷移 |
| T-EXC-030-05 | `$1000`=`$4E40`（TRAP #0）。vector 32 のエントリ（`$80`〜`$83`）の読出しが BERR。vector 2=handler | vector 2 の14 byte frame（TRAP の6 byte frame の下）、saved PC=`$80`、PC=handler。HALTしない |
| T-EXC-030-06 | `$1000`=`$4E40`。vector 32=`$F00000`（BERR 領域）、vector 2=handler | 2回目の step で vector 2 の14 byte frame、saved PC=`$80`（vector 32 のアドレス）、PC=handler。HALTしない |
| T-EXC-030-07 | SR=`$A000`（T=1,S=1）、`$1000`=`$4EF9 $00F0 $0000`（JMP $F00000）、vector 2=handler、vector 9=handler+`$40` | vector 2 の14 byte frame、PC=handler（trace 例外にならない） |
| T-EXC-030-08 | SR=`$2300`（mask 3）、IRQ level 3 保留、`$F00000`-4=`$46FC $2000`（MOVE #$2000,SR）、直後が BERR 領域 | vector 2 の bus error 例外（IRQ より先）。PC=handler |
| T-EXC-030-09 | `$EFFFFE`=NOP、次の語（`$F00000`）が BERR 領域、PC=`$EFFFFE` | vector 2 の bus error 例外。PC=handler |
| T-EXC-030-10 | 最初の標本化は IRQ なし、以降は level 3。（a）`$1000`=`$3210`、A0=`$F00000`、vector 2=`$F00000`。（b）`$1000`=`$4E40`、vector 32=`$F00000`、vector 2=handler | （a）HALT へ遷移。（b）vector 2 の bus error 例外、saved PC=`$80`（割り込みより先） |
| T-EXC-030-11 | （a）`$1000`=`$4AFC`、vector 4 のエントリが BERR。（b）IRQ level 3、vector 27 のエントリが BERR。（c）`$1000`=`$4E40`、vector 32 のエントリが BERR。vector 2=handler | スタック上の SSW の bit3（I/N）が（a）（b）は1、（c）は0 |
| T-EXC-030-12 | vector 読出しが BERR になる構成で、illegal（4）、privilege（8、user で STOP）、trace（9、T=1）、Line-A（10）、Line-F（11）、CHK（6）、TRAPV（7）、DIVU #0（5）、および IRQ level 1〜7（vector 25〜31）。vector 2=handler | スタック上の SSW の I/N が、vector 4・8・9・10・11 と IRQ は1、5・6・7 は0。saved PC は各 vector のアドレス |
| T-EXC-030-13 | （a）`$1000`=ILLEGAL、vector 4=`$F00000`（BERR）、handler（vector 2）=`$3210`（MOVE.W (A0),D1）、A0=`$F00000`。（b）IRQ level 3、vector 27=`$2000`。（c）IRQ level 3、vector 27=`$F00000` | （a）1命令目は I/N=1、saved PC=`$10`。続く handler の bus error は I/N=0。（b）handler 先頭の2語を `$2000` から読み、旧 PC（`$1000`）を読まない。（c）saved PC=`$6C`、I/N=1 |
| T-EXC-030-14 | `$F00000`-4 に MOVE.W D0,(0,A1)、DIVU #0,D0、JMP (0,A0,D0) のいずれか。拡張語の消費時の補充（`$F00000`）が失敗する | vector 2 の bus error 例外。MOVE のデータ書込みは行われず、ゼロ除算（vector 5）にも分岐にもならない |
| T-EXC-030-15 | （a）CLR.W (A1)、A1=`$300` で `$300` が BERR。（b）68010 で CLR.W (A1) | （a）read で bus error（SSW の R=1）。`$300` への write はない。（b）データ read は出ない（書込み前 read は MC68000 のみ） |
| T-EXC-030-16 | `$F00000`-4 に、ORI／ANDI／SUBI／ADDI／EORI／CMPI の #,(A0)・(A0)+・-(A0) 形式、および ORI／ANDI／EORI #,SR と #,CCR を置く。拡張語の消費時の補充（`$F00000`）が失敗する。SR=`$2715`、A0=`$200` | vector 2 の bus error 例外。`$1F0`〜`$20F` へのデータアクセスはなく、A0=`$200` のまま。スタックに積まれる SR=`$2715`（命令実行前の値） |
| T-EXC-030-17 | 68020。SR=`$A000`（T=1）、`$1000`=NOP、vector 9 のエントリが BERR。vector 2=handler | 1回目の実行呼出しは例外処理が保留（CPU_EXCEPTION）で戻り、PC は handler でない。次の実行呼出しで vector 2 の bus error 例外になり PC=handler（MC68000 は1回目の中で処理する。T-EXC-030-12）。68010 は MC68000 と同様に1回目の中で処理する: PC=handler、D0 は命令実行後の値（巻き戻さない）、frame は format $8（SR=`$A000`、PC=`$1002`、format word=`$8008`、SSW=`$1105`、fault address=VBR+`$24`）で、途中まで積んだ format $0 frame（8 byte）の下に積まれる。vector を直して RTE すると PC=`$1002`、SP は初期値、user モードでは USP／SSP も元に戻り、origin は空になる。VBR≠0、TRAP（vector 32）、ILLEGAL（vector 4）、IRQ（vector 27）も同様 |
| T-EXC-030-18 | `$EFFFFE`=`$6100`／`$6000`／`$6700`（BSR.W／BRA.W／BEQ.W、Z=1）。変位語（`$F00000`）の取得が失敗する。SSP=`$8000` | vector 2 の bus error 例外。SSP=`$8000`-14（BSR.W は戻りアドレスの push なし）、PC=handler。BRA.W／Bcc.W では PC の書換えは観測できない（取得の失敗で PC は進まないため）。実装側の判定は防御的なもので、この試験は BSR.W の push の有無だけを区別する |
| T-EXC-040-01/02 | SR=`$A700`、`$1000`=NOP（`$4E71`）、vector 9=handler（-01）。`$1000`=ILLEGAL、vector 4=handler、vector 9=handler+`$20`（-02） | なし |
| T-IRQ-010-01 | mask 以下の level は受理せず、mask を超える level は命令境界で受理し、STOP 中も受理する。各ケースの SR・level は `test_irq`・`test_irq_pull` 内で設定する（本表は共通条件のみ記述） | なし |
| T-IRQ-020-01..03 | -01: vector 27=handler、IACK は autovector。-02: vector 64=handler+`$20`、IACK が vector 64 を返す。-03: vector 24=handler+`$40`、IACK 中に BERR を入力（IACK が返す値は使われない） | なし |
| T-RST-010-01 | `$1000`=`$4E72,$2000`（STOP #$2000）、vector 28=handler、STOP 後に IRQ level 4 を入力。その後 `$0000`=`$00006FF0`、`$0004`=`$00001234`、SR=0、A7=`$1111` にして reset | STOP 中の追加実行でバス要求が出ないこと以外なし |

### 12.4 参照実装・実機との差分検証

参照実装（XEiJ 等）および実機との比較は、一次資料から導出した期待値の代替にしない。用途は次の2つに
限る。

1. 一次資料から断定できない項目（TIM-U3、TIM-U4）の観測。これは将来の精度向上のための任意の較正であり、
   結果は §14 の該当IDに「観測値・観測条件・観測日」を追記する。確認できるまで適合性ケースの
   期待値、適合性判定、または Issue #34 の受入れ条件には使わない。
2. 本書のケースを通過した実装に対する回帰検出。不一致は、本書の誤り・実装の誤り・参照実装の
   癖のどれかに分類してから扱う。

現行実装と本書の差異は §12.7 の差異表に記録する。

### 12.5 ケース→テスト対応表

規範ID→資料→ケースID→機械テストの対応は `tests/data/cpu/conformance_map_mc68000.csv` を正本とする。
列は `norm_id, axes, spec_section, source_ref, case_ids, machine_tests, status, gap_note` で、
`source_ref` は、規範IDが属する節（または同じ段落）にある一次資料引用を機械的に抽出した値で、
TIM 系を除き規範単位の厳密な対応ではない。規範単位の対応は本文の引用が正本であり、CSV は索引である。
`status` は次の4値とする。

| status | 意味 |
|---|---|
| `covered` | ケースが本書に定義され、機械テストが存在する |
| `defined` | ケースが本書に定義されているが、機械テストが未実装または未確認 |
| `gap` | ケースが未導出。`gap_note` に理由を記す |
| `retired` | 廃止ID（再利用しない） |

`python3 tools/check_conformance_map.py` は、本書の全規範IDとCSVの双方向一致、本書の全ケースIDと
CSVの双方向一致、`covered`／`defined`／`gap` の必須列を検査する。

### 12.6 バス順序・タイミングのケース（Phase 4）

軸は T（時間精度）と B（バス順序）。共通初期条件: PC=`$1000`、SSP=`$8000`、A0=`$200`、A1=`$300`、D0/D1=`$0`、
関連メモリは0初期化（命令語と、各ケースが書く値を除く）、入力信号は常時 DTACK（BERR／HALT／IRQ なし、BERR を使うケースは該当 address だけ BERR）、
観測点は命令完了（バスエラーのケースは例外ベクタ取得完了）。比較しない項目は、SR の bit 11 と bits 7:5、TIM-020 のアイドルクロックの位置、
表にないレジスタとメモリ（未変更を期待する）である。TAS のケース（TIM-030〜032）は `$200`=`$12`（TIM-031 は A0=`$F00100` で BERR）。サイクル数は wait なしの公称値で、UM の `n(r/w)` 表
[UM §8 / PDF p.117–127] から取る。初期は supervisor、プリフェッチ状態は命令先頭（初期PCの2語を取得済みで、その取得は計測に数えない）、wait state は特記しない限り0。

| ケース | 規範ID | 軸 | 命令・条件 | 期待 |
|---|---|---|---|---|
| T-TIM-010-01 | TIM-010 | T | NOP | 4クロック、read 1 |
| T-TIM-011-01 | TIM-011 | T | MOVE.W D0,D1 | 4(1/0) |
| T-TIM-011-02 | TIM-011 | T | MOVE.W (A0),D1 | 8(2/0) |
| T-TIM-011-03 | TIM-011 | T | MOVE.W D1,(A1) | 8(1/1) |
| T-TIM-011-04 | TIM-011 | T | MOVE.W (A0),(A1) | 12(2/1) |
| T-TIM-011-05 | TIM-011 | T | Scc D0（条件真） | 6(1/0) |
| T-TIM-011-06 | TIM-011 | T | Scc D0（条件偽） | 4(1/0) |
| T-TIM-012-01 | TIM-012 | T | MOVE.W (A0),D1、データ read に2クロックの wait | 10クロック、r/w は (2/0) のまま |
| T-TIM-013-01 | TIM-013 | B | MOVE.W D1,(A1) のバストレース | 命令フェッチ read 1、データ write 1（A1 番地、word） |
| T-TIM-013-02 | TIM-010, TIM-013 | T,B | BRA／Bcc（B・W、taken・not taken）、BSR、DBcc（cc true・未満了・満了）、JMP／JSR／LEA／PEA の各EA、MOVE #,SR、ORI／ANDI／EORI #,SR と #,CCR、MOVE D0,CCR／SR、STOP、JMP／JSR／LEA／PEA の (d8,An,Xn)、CLR.W／CLR.L／ST／SF／MOVE SR のメモリ形式、MOVEM メモリ→レジスタ（word・long、(An)・(An)+・(xxx).W）、TRAP・CHK のトラップ時・ILLEGAL、RTE（SSP にフレーム、復帰先が通常アドレスと BERR の場合）。初期状態は §12.6 に従い、DBcc は D0=1（未満了）または 0（満了） | UM Table 8-9, 8-10, 8-12 の クロック数 (r/w)。long の write／read は word 2回として数える。STOP は 4(0/0)。RTE（`.48`）は 20(5/0)（UM Table 8-12）で、復帰先の取得が失敗する場合は bus error 例外になり saved PC は RTE の語の末尾（PC₀+2。TIM-040 の分岐先の取得の失敗の規則） |
| T-TIM-020-01 | TIM-020 | T,B | CLR.L D0 | 総クロック6(1/0)。バス上の read は1回で、アイドル2クロックの位置は判定しない |
| T-TIM-030-01 | TIM-030 | B | TAS (A0)、RMW 属性を通知する実装 | read8(A0) の直後に write8(A0, old\|$80)。AS は保持される |
| T-TIM-031-01 | TIM-031 | X,B | TAS (A0)、read 部で bus error | retry せず bus error 処理（vector 2、§8.3 のフレーム）。HALT 入力の有無で結果が変わらない |
| T-TIM-032-01 | TIM-032 | B | TAS (A0)、RMW 属性を持たない抽象バス | read8(A0) と write8(A0, old\|$80) が連続し、間に他のバスサイクルがない |
| T-TIM-040-01 | TIM-040 | X,B | MOVE.W (A0),D1、A0=`$F00000`（read が BERR）。および MOVE.W D0,(A0)（write が BERR） | vector 2 の 14 byte フレームの saved PC が `$1000+2` 以上 `$1000+10` 以下の偶数 |
| T-TIM-040-02 | TIM-040 | F,B | MOVE.W D0,($1004).W が直後のNOPをILLEGALへ書換える | メモリはILLEGALへ更新されるが、先読み済みのNOPを実行する |
| T-TIM-040-04 | TIM-040 | F,B | MOVE.W D0,($1006).W が、補充される2語目の位置へ ILLEGAL を書込む。直後は NOP, NOP | バス順は `rI@1004`、`wD@1006`、`rI@1006`。2命令目まで NOP を実行し、3命令目で書換え後の ILLEGAL を実行する |
| T-TIM-040-05 | TIM-040, EXC-020 | X,B | JMP (xxx).L の分岐先が BERR 領域（`$F00000`）、および奇数アドレス（`$3001`）。分岐先の先読みが失敗する | JMP の中で vector 2／3 の14 byte フレームを積む。saved PC は `$1000+2` 以上 `$1000+10` 以下の偶数（分岐先ではない） |
| T-TIM-040-03 | TIM-040 | B | BRA.Sが逐次streamの先読み済み語を飛び越える | 逐次streamを破棄し、分岐先から2語をfetchする |
| T-TIM-041-01 | TIM-041 | B | supervisor／user で NOP | 命令フェッチの FC がそれぞれ `%110`／`%010` |

機械テストは `test_conformance_timing`。TAS のメモリ形式は `14(2/1)+EA` にTable 8-1のEA値を
加算する。したがってTAS (An)は `14(2/1)+4(1/0)=18(3/1)`であり、EAのoperand readを別途加算してはならない
[UM Table 8-1, Table 8-6 / PDF p.118, p.122]。

### 12.7 本書と現行実装の差異表

分類は「実装の誤り」「許容近似」「本書の誤り」「一致」「未検証」のいずれか。本表は補助的な不整合
検出であり、合否の根拠ではない（§2.3）。実装を根拠に本書を書き換えてはならない。

| 規範ID | 現行実装の確認箇所 | 分類 | 内容 |
|---|---|---|---|
| TIM-010〜013, TIM-020 | `src/cpu/cycles.c`、`tests/cpu/test_conformance_timing.c` | 一致（r／w、クロック数）。§12.6 の8命令、T-TIM-013-02 の48ケース、T-TIM-014（ADD／ADDA／SUB／SUBA／AND／OR／EOR／ANDI／ORI／EORI／CHK／MULU／MULS の71ケース。ANDI／ORI／EORI は修正不要で一致を確認）が UM と一致する。以前は ADD.L (A0),D1、ADD.W／ADD.L D1,(A1)、ADDA.L (A0),A2、CHK.W (A0),D0 などが EA 時間を加算せず、MULU／MULS が常に最悪値 70 を返していた。SUB／SUBA／AND／OR／EOR も ADD／ADDA と同じ誤りだった（実装の誤り。#122 で修正） | §12.6 のケースを `mc68k_execute(cpu,1)` の戻り値（クロック数）とバスログの read／write 回数で実行し、すべて一致した。wait は `access_timing` の `wait_cycles` で与えた |
| TIM-030, TIM-032 | `include/mc68000/bus_hooks.h` の `mc68k_bus_access_t.rmw`、同テスト | 一致 | TAS (A0) は read8 の直後に write8(old\|$80) を発行し、間に他のバス要求がない。両サイクルに `rmw` が立つ |
| TIM-031 | 同テスト | 一致（一部未検証） | read の BERR で retry せず、vector 2 の 14 byte フレームを積む。HALT 入力の有無による差は、抽象バスに HALT 入力がないため未検証 |
| TIM-041 | 同テスト | 一致（一部未検証） | 命令フェッチの FC が supervisor で `%110`、user で `%010`。命令語以外（拡張語のフェッチ）の FC は本ケースでは未検証 |
| TIM-040, EXC-020 | `src/cpu/m68000.c:cpu_prefetch_fetch_word`、`tests/cpu/test_conformance_timing.c` | 一致 | MC68000モデルは2語キューを使い、追加語消費時と命令完了時に補充する。分岐では逐次streamを破棄して分岐先を2語充填し、自己書換えでは先読み済みの旧語を実行する。saved PC の範囲も T-TIM-040-01 で検証する |
| TIM-040（先読みフォルト） | `src/cpu/m68000.c:prefetch_fill_to`、`prefetch_raise_fault`、`cpu_prefetch_complete_instruction`、`tests/cpu/test_conformance_timing.c` | 一致 | 先読みで起きた bus error／address error は、先読みを行った命令の中で報告する。分岐先のフェッチ失敗で積む saved PC は分岐命令の語の末尾にし、T-TIM-040-05 と T-EXC-030-07〜09 で検証した。以前は保留して使用時に報告しており、trace・割り込みと競合すると bus error が失われた（実装の誤り） |
| EXC-030（例外処理中のフォルト） | `src/cpu/m68000.c:exception_vector_read_fault`、`prefetch_raise_fault`、`tests/cpu/test_conformance_timing.c` | 一致 | 以前は Group 1／Group 2 例外と割り込みの vector 読出し失敗を一律に HALT としており、UM §6.3.9.1 の「vectorのアドレスを saved PC とする bus error 例外」に反していた（実装の誤り）。MC68000 では bus error 例外に修正した。Group 0 例外の vector 読出し、handler先頭の命令フェッチの失敗は HALT。frame push の失敗は、UM が結果を定めない場合を除き従来どおり HALT のまま。68010 は、Group 1／Group 2 例外・trace・割り込みの vector 読出し失敗を HALT にせず、同じ実行呼出しの中で vector 2 の bus error 例外にする（68010 UM では HALT は bus error・address error・reset の例外処理中の失敗に限られる）。積む SR／PC は例外が起きた時点のもの（`exception_frame_origin`）で、命令は巻き戻さず、saved PC は vector のアドレスではない。format $8 の RTE は途中まで積んだ例外フレームごと origin のスタックへ戻る。近似: 68010 の frame push の失敗は HALT のまま（奇数 SSP などでは続く Group 0 frame の push も失敗するので結果は同じ）。再フォルト安全弁は、IRQ と saved PC が命令自身の Group 1 で、handler が何も直さず RTE すると 2 回目に HALT する。trace は毎命令フォルトし続ける。TRAP／CHK は元の例外が失われる。即時配送のため `last_exception_vector` は 2 になる（debugger の trace 停止は効かない。MC68000 と同じ）。push 失敗で HALT した場合、origin は reset まで残る |
| INS-052（予約EA） | `src/cpu/` の ADDA／SUBA／CMPA のデコード | 一致 | EA mode=111/reg=101–111 を source とする ADDA／SUBA／CMPA の144 opcodeは、デコード表への登録を行わず illegal instruction とする。`test_conformance_basic` は6形式×予約EA 3種（`$3D`〜`$3F`）が vector 4 を起こしてAを変更しないこと、およびAn直接（mode=001、PM ADDA／SUBA／CMPAの許可EA）が ILLEGAL にならないことを検証する。分類CSVとデコード表の全opcode突合せは未実施 |
| REG-008 | `src/cpu/cpu_internal.h` の `cpu_set_sr` | 一致 | 以前は `op_move_to_sr` が値を無加工で保存し T0/M が 1 で読めた（実装の誤り）。MC68000 モデルでは `cpu_set_sr` と RTE の復帰で bit 14・12 を 0 にするよう修正し、`test_conformance_basic` が通る |
| REG, BUS, EA（REG-008 以外）、PRV-030 | `tests/cpu/test_conformance_basic.c` | 一致 | §12.8 のケースをすべて実行し一致した。ただし BUS-004 は、コアが long を 1 回の size=4 要求としてフックへ渡す場合も許容している（物理 word 2 回への分解はホスト側の責務として扱う） |

§12.6 のケースは `test_conformance_timing` として機械テスト化済みである。TIM-U3・TIM-U4に関わる項目は、実機観測まで差異表に入れない。

### 12.8 REG・BUS・EA・PRV-030 のケース

共通条件（§2.5 の必須項目）: 軸は F と B。バスは 16 MiB の RAM で、`$F00000–$F0FFFF` の read/write は BERR で
応答する。初期は supervisor（SR=`$2700`）、PC=`$1000`、SSP=`$8000`、A0=`$200`、D1=`$FFFF0000`、
`$200..$207`=`11 11 22 22 33 33 44 44`、`$210`=`55 55`、その他は 0。命令は `$1000` から配置し、旧プリフェッチを
残さず開始する。実行量は 1 命令、観測点は命令完了（例外ケースは handler 入口）、wait state は 0、サイクル数は
検査しない（T 軸は §12.6）。表にない項目は未変更を期待する。比較しない項目は、SR の bit 11 と bits 7:5（未定義）。
バストレースは `{address, size, read/write, FC, instruction}` の順序付き列で観測し、address は下位 24 bit で比較する。
機械テストはすべて `test_conformance_basic`。

| ケース | 規範ID | 命令・条件 | 期待 |
|---|---|---|---|
| T-REG-001-01 | REG-001 | D0=`$11223344`,D1=`$AABBCCDD`; MOVE.W D0,D1 | D1=`$AABB3344`（上位 16 bit 保持） |
| T-REG-002-01 | REG-002 | D0=`$8000`; MOVEA.W D0,A1 | A1=`$FFFF8000` |
| T-REG-003-01 | REG-003 | USP=`$7000`,A7(SSP)=`$6000`; MOVE #$0000,SR | S=0、A7=`$7000`、SSP=`$6000` |
| T-REG-003-02 | REG-003 | ILLEGAL（ベクタ 4 設定） | スタックは低位へ伸長し A7=`$8000-6` |
| T-REG-004-01 | REG-004 | PC=`$AB001000`,`$1000` に NOP | PC=`$AB001002`（上位 8 bit 保持）、命令フェッチの address=`$001000` |
| T-REG-005-01 | REG-005 | D0=`$11223344`,A0=`$200`; MOVE.L D0,(A0) | `$200..$203`=`11 22 33 44` |
| T-REG-006-01 | REG-006 | user、SR=`$0015`、D0=`$FFFF0000`; MOVE SR,D0 | 特権違反を起こさず、D0 下位 16 bit の規範 bit=SR の規範 bit、上位 16 bit 保持、PC=`$1002` |
| T-REG-008-01 | REG-008 | MOVE #$7FFF,SR; MOVE SR,D0 | D0 の bit 14 と bit 12 が 0 |
| T-REG-009-01 | REG-009 | A0=`$AB000200`; MOVE.W (A0),D1 | D1 下位=`$200` の語、A0 不変、データ read は 1 回で address=`$000200` |
| T-REG-011-01 | REG-011 | A0=`$201`; MOVE.W (A0),D1 | ベクタ 3 の handler へ。address=`$201` のデータ転送は発行しない |
| T-EA-001-01 | EA-001 | D0=`$9876`; MOVE.W D0,D1 | D1=`$FFFF9876`、PC=`$1002` |
| T-EA-002-01 | EA-002 | MOVE.W A0,D1 | D1=`$FFFF0200` |
| T-EA-003-01 | EA-003 | MOVE.W (A0),D1 | D1=`$FFFF1111`、A0 不変 |
| T-EA-004-01 | EA-004 | MOVE.W (A0)+,D1 | D1=`$FFFF1111`、A0=`$202` |
| T-EA-004-02 | EA-004 | A7=`$400`,`$400`=`AB 00`; MOVE.B (A7)+,D1 | D1 下位 byte=`$AB`、A7=`$402`（byte でも 2） |
| T-EA-005-01 | EA-005 | A0=`$204`; MOVE.W -(A0),D1 | D1=`$FFFF2222`、A0=`$202` |
| T-EA-006-01 | EA-006 | MOVE.W 4(A0),D1 | D1=`$FFFF3333`、PC=`$1004` |
| T-EA-006-02 | EA-006 | A0=`$204`; MOVE.W -2(A0),D1 | D1=`$FFFF2222`（負の変位） |
| T-EA-007-01 | EA-007 | D2=`$FFFF0002`; MOVE.W 2(A0,D2.W),D1、拡張語の bits 10:9=`11` | D1=`$FFFF3333`（word index は符号拡張した下位 16 bit、bits 10:9 は scale にならない） |
| T-EA-007-02 | EA-007 | A0=`$204`,D2=`$0000FFFC`; MOVE.W 2(A0,D2.W),D1 | D1=`$FFFF2222`（負の word index） |
| T-EA-007-03 | EA-007 | D2=`$10`; MOVE.W 0(A0,D2.L),D1。および index=A2.L,A2=`$10` | どちらも D1=`$FFFF5555` |
| T-EA-013-01 | EA-013 | D2=`$FFFF0002`; MOVE.W 2(A0,D2.W),D1、拡張語の bits 10:8=`111`（`$2702`）。および bit 8 のみ 1（`$2102`） | どちらも D1=`$FFFF3333`、PC=`$1004`、例外なし（bits 10:8 は無視する） |
| T-EA-008-01 | EA-008 | MOVE.W ($0200).W,D1 | D1=`$FFFF1111`、PC=`$1004` |
| T-EA-008-02 | EA-008 | MOVE.W ($8000).W,D1 | データ read の address=`$FF8000`（符号拡張後の下位 24 bit） |
| T-EA-009-01 | EA-009 | MOVE.W ($00000204).L,D1 | D1=`$FFFF3333`、PC=`$1006` |
| T-EA-010-01 | EA-010 | `$1200`=`77 77`; MOVE.W $1FE(PC),D1 | D1=`$FFFF7777`（PC_ext=`$1002`） |
| T-EA-011-01 | EA-011 | `$1200`=`77 77`,D2=`$1FC`; MOVE.W 2(PC,D2.W),D1 | D1=`$FFFF7777` |
| T-EA-012-01 | EA-012 | MOVE.W #$1234,D1 | D1=`$FFFF1234`、PC=`$1004` |
| T-EA-012-02 | EA-012 | MOVE.L #$12345678,D1 | D1=`$12345678`、PC=`$1006` |
| T-EA-012-03 | EA-012 | 拡張語=`$FFAB`; MOVE.B #$AB,D1 | D1=`$FFFF00AB`（下位 byte のみ）、PC=`$1004` |
| T-EA-014-01 | EA-014 | A0=`$201`,D0=`$5A`; MOVE.B D0,-(A0)。および A7=`$400`; MOVE.B D0,-(A7) | A0=`$200`,`$200`=`5A`。A7=`$3FE`,`$3FE`=`5A` |
| T-EA-014-02 | EA-014 | A0=`$F01000`; MOVE.W D0,-(A0)、write が BERR | ベクタ 2 の handler へ。A0=`$F00FFE`（失敗後も減算値を保持） |
| T-EA-015-01 | EA-015 | A0=`$F00000`; MOVE.W (A0)+,D1、read が BERR | ベクタ 2 の handler へ。A7=`$8000-14`。比較しない: A0、D1 |
| T-EA-015-02 | EA-015 | A0=`$F00000`,D0=`$1234`; MOVE.W D0,(A0)+、write が BERR | ベクタ 2 の handler へ。A7=`$8000-14`。比較しない: A0 |
| T-EA-016-01 | EA-016 | A0=`$EFFFFE`; MOVEM.W (A0)+,D0-D1（2 回目の read が `$F00000` で BERR） | ベクタ 2 の handler へ。A7=`$8000-14`。比較しない: D0、D1、A0 |
| T-EA-016-02 | EA-016 | A0=`$200`,A1=`$F00002`; MOVE.W (A0)+,-(A1)、write が BERR | ベクタ 2 の handler へ。A7=`$8000-14`。比較しない: A0、A1、D レジスタ |
| T-EA-017-01 | EA-017 | §5.1 の分類表（data／data alterable／memory／memory alterable／control／alterable）を PM Table 2-4 の MC68000 の行と照合する | 全分類が一致する |
| T-EA-018-01 | EA-018 | `$200`=`00 01`; NEG.W (A0) | `$200`=`FF FF`。address=`$200` の read が 1 回、その後に write が 1 回 |
| T-EA-018-02 | EA-018 | A0=`$F00100`; NEG.W (A0)、read が BERR | ベクタ 2 の handler へ。address=`$F00100` への write は発行しない |
| T-BUS-001-01 | BUS-001 | `$200`=`A1`,`$201`=`B2`; MOVE.B (A0),D1 と MOVE.B 1(A0),D1 | それぞれ size=1 の read が 1 回（`$200`、`$201`）。D1 下位 byte=`$A1`、`$B2` |
| T-BUS-004-01 | BUS-004 | MOVE.L (A0),D1 | D1=`$11112222`。データ要求は size=4 の 1 回、または size=2 の 2 回（`$200`、`$202` の昇順） |
| T-BUS-005-01 | BUS-005 | NOP | PC=`$1002`、最初の要求は命令フェッチの read、size は word 以上 |
| T-BUS-005-02 | BUS-005 | PC=`$1001`（ベクタ 3 設定） | handler へ。address=`$1001` の命令フェッチ要求は発行しない |
| T-BUS-008-01 | BUS-008 | MOVE.W (A0),D1 を supervisor／user（SR=`$0000`,USP=`$7000`）で実行 | 命令フェッチの FC は `%110`／`%010`、データの FC は `%101`／`%001`。他の FC は現れない |
| T-BUS-009-01 | BUS-009 | A0=`$F00000`; MOVE.W (A0),D1 | ベクタ 2 の handler へ。A7=`$8000-14`（14 byte フレーム） |
| T-BUS-010-01 | BUS-010 | IACK 中の BERR | bus error ではなく spurious interrupt を選ぶ。T-IRQ-020-03 と同一ケース |
| T-BUS-011-01 | BUS-011 | A0=`$201`; MOVE.W (A0),D1 | ベクタ 3 の handler へ。A7=`$8000-14`。address=`$201` のデータ要求なし |
| T-BUS-012-01 | BUS-012 | MOVE.W (A0),D1 を 2 回連続 | 各命令でデータ read が 1 回ずつ、合計 2 回（同一 address でも統合しない） |
| T-BUS-012-02 | BUS-012 | デバッガの非侵襲 read（`mc68k_debug_read_bytes`）で `$200` の 2 byte を読む | `11 11` が返り、CPU のバス要求は 0 回 |
| T-PRV-030-01 | PRV-030 | STOP #$2000 を実行後、さらに 1000 サイクル実行 | STOP 完了後のバス要求（命令フェッチを含む）が 0 回 |

### 12.9 命令メタ規範の検証手段

次のメタ規範は、自身の専用ケースを持たず、既存ケースを検証手段とする。対応は
`tests/data/cpu/conformance_map_mc68000.csv` の `case_ids` を正本とする。

| 規範ID | 検証手段 |
|---|---|
| INS-001（分類語彙） | T-INS-053-01（分類件数と重複・未分類 0） |
| INS-002（規則形式） | T-INS-041-01（規則CSVの必須列・集合の検査） |
| INS-003（命令台帳） | T-INS-051-01（118 形式の累積検査） |
| INS-004（2A の境界） | T-INS-052-01（全 65,536 opcode の閉包） |
| INS-010, INS-023, INS-024 | 2B の T-INS-011〜022 の各ケース（`test_phase2b_contract`） |
| INS-025, INS-035, INS-036 | 2C の T-INS-026〜034 の各ケース（`test_phase2c_contract`） |

---

## 13. 将来世代への引継ぎ条件

**範囲外（Issue #34 の完了条件に含めない）。** MC68010 以降は §2.1 により本書の対象外である。
世代差分は `docs/mc68000-internals/reference/cpu_68010.md` 以降の各設計書で扱う。ただし本書の規範ID・ケース形式・
対応表（§12.5）は他世代の仕様書が再利用できるよう、世代非依存の表記で書いている。

### 13.1 MC68010 への差分境界

範囲外（上記および §2.1）。

### 13.2 MC68020 以降への差分境界

範囲外（上記および §2.1）。

---

## 14. 未解決事項

### 14.1 Phase 1 の未解決規範

なし。EA-013 は PM §2.4 から、EA-017 は PM Table 2-4 との照合で確定した。EA-015 と EA-016 は資料が規定しないため、
**未定義**（適合性ケースで比較しない値）として §5.2・§5.4 で規範化した。実機で確定できた場合は規範へ昇格できる。

規範本文と機械可読な規則表の間にあった不一致（EA-RSV）は解消した。§5.1 は mode=111/reg=101–111 を予約とするのに、規則表の述語が
EA 番号 61〜63 を有効EAに含めていたため、これらを source EA とする 1,155 opcode（例: `$D03D`）が `valid` に分類されていた。UM は
「合法な MC68000 命令の第1語の符号化に一致しない語」を illegal instruction とする [UM §6.3.6, printed p.6-14 / PDF p.99] ので、
述語から予約EAを除き、これらを `illegal encoding` へ移した（valid 46,588→45,433、illegal 10,756→11,911）。

### 14.2 Phase 4 の観測保留項目（適合性対象外）

| ID | 事項 | 影響 | 解決条件 |
|---|---|---|---|
| TIM-U4 | 命令内で、拡張語のフェッチと EA 計算・オペランド access のバス上の前後関係 | B軸の命令内順序 | M68K83 は補充の語数と時点だけを定める。追加資料または実機のバストレース観測。TIM-040 の「最後の補充は命令のデータアクセスより後」（M68K83 の規則）は規範であり、T-TIM-040-04 で検証する。それ以外の命令内の順序は期待値に使わず、r／w の総数と補充規則で判定する |
| TIM-U5 | TAS のメモリ形式 `14(2/1)+EA` の r。UM の表を文字どおり加算すると TAS (An) は 18(3/1) だが、EA の operand read と RMW の read が二重に数えられている疑いがある | T軸（r） | 実機のバストレース観測、または追加資料。TAS メモリ形式の r は規範にせず、クロック数（18）だけを規範とする。現行実装は 18(2/1) |
| TIM-U3 | 命令内バスアイドル期間の位置（TIM-020） | T軸のバス位相 | 実機のバストレース観測。将来の精度向上項目であり、許容近似として扱う。Issue #34 の受入れ・適合性判定を妨げない |

### 14.4 UM の表の不整合（記録）

次の項目は UM の表の中で整合しないか、他の記述と矛盾する。期待値には、整合する側または物理的に妥当な側を採り、
採った側を明記する。

| 項目 | UM の記載 | 扱い |
|---|---|---|
| JMP の (d8,An,Xn)／(d8,PC,Xn) | Table 8-10: JMP 14(3/0)。同じ表の JSR 22(2/2)、LEA 12(2/0)、PEA 20(2/2) は r=2 | JMP の行を採り r=3 とする。他は r=2 |
| RTR | Table 8-12: 20(2/0)。RTR は CCR とPC をポップし、分岐の再充填を含むため read は5回になる | r は規範にしない（クロック数 20 のみ） |
| MOVEM (An)+ Long | Table 8-10: (3+n/0) と (3+2n/0) が混在 | 長語は 3+2n とする |
| MOVE.L (d16,PC),(xxx).L | 32(5/2) と記載される行 | 試験に使わない |
| TRAPV（トラップ時） | Table 8-14: 34(5/3)。TRAP は 34(4/3) | 実装は 34(4/3)。r は規範にしない |

DBcc のカウンタ満了で読む分岐先の1語（T-TIM-013-02）は、キューに入らず捨てられる読出しであり、
分岐先が奇数アドレスのときは省き、フォルトも無視する。キューへの先読み（TIM-040）とは別で、UM の read 数に
合わせるための推論である。

### 14.3 Phase 5 時点のケース未導出（残件）

`tests/data/cpu/conformance_map_mc68000.csv` に `status=gap` および `status=defined` の規範IDはない。
