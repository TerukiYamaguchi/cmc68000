#!/usr/bin/env python3
"""tests/data/fpu/fpu2_table_a.csv・fpu2_table_b.csv（表 A・表 B）と tests/data/fpu/fpu2_phase_biu_assignment.csv を検査する。

  check_fpu2_tables.py                 検査（違反があれば列挙して非0で終了）
  check_fpu2_tables.py --root DIR      DIR/tests/data/fpu/ の CSV を検査する（変異確認用）

正本は CSV である（設計書の表から取り出していた旧方式は、設計書を削除したので廃止した）。
CSV を編集したら引数なしで検査する。

検査内容（違反はすべて列挙する）:
  2. 形状: 表 A は 20 行 x 9 列、表 B は 11 行 x 1 列。行見出しが S1..S20、B1..B11 の通し番号
  4. 遷移先: 「→Sn」「Sn の command/condition 書込みと同じ」が存在する状態・列を指す
  5. 68881 の N/A 行: 行見出しが「68881 は N/A」の行に「81:」を書かず、68881 の分岐（機種指定なし・81:）が N/A 行へ遷移しない。
     「81:」を書くセルは「／82:」を持つ
  6. 不変条件（§4.2）: 行見出しの phase・UNSTARTED・`arith`・`context_valid`・violation が
     不変条件1〜5と矛盾しない。行見出しと不変条件から各行の `arith`・`context_valid` の
     取り得る値を決める
  7. 遷移の整合: 各分岐（「。」と機種で分けた単位）の遷移先について、分岐後の `arith` と
     `context_valid`（§4.2 の記法: `arith`=値、列の規則、「`context_valid` 真」）が
     遷移先の行の取り得る値に含まれる。「Sn の … 書込みと同じ」「開始」「評価」は参照先の
     セルを分岐後の値で辿る。表 B は `begin_restore()` 後（`arith`=NONE）から辿る
  8. command/condition 書込みで（参照を辿って）S4 へ遷移するセルは、セル自身に
     `arith`=PRESENTED_PRE を書く
  9. 応答読出しで別の行へ移るとき、遷移先の応答読出しセルが、入ってきた primitive の種類に
     対応する1文（「。」で分けた括弧の外の1文）を持つ。消費されない種類（NULL・TAKE）なら
     「同じ」、消費される種類なら「消費」。セル全体の部分文字列では判定せず、否定語
     （「返さない」「同じでは」等）を含む文は数えない。種類が命令で決まる最初の primitive
     （S12）からは、遷移先の phase で決める（IDLE は検査しない。要求の無い命令の NULL は読んだ後に idle の NULL へ符号化し直す（§5.1）。UNSTARTED の無い PC は
     「NULL … 同じ」と「EA_TRANSFER/MP_REGISTER … 消費」の両方、RESPONSE_NEXT は「後続
     primitive」、他は「消費」）。S12 のセルは遷移先を機種別に列挙する
 9''. 応答読出しの検査の網羅: 期待する遷移（S12→S2/S14〜S18、S13→S2、S6→S8、S14→S16/S17、§5.5 の
     PC=1 の行→S18）がすべて検査されたこと、S12 の遷移先が行見出しの phase から導いた集合と
     一致すること（81 は S18 を除く）、S18 の IA 書込みが、入った primitive の種類ごとの分岐と遷移先（外部宛て FMOVE→S14、
     `MP_REGISTER`・`EA_TRANSFER`（DR=0）→S15、要求の無い命令→S2/S3）の組と一致すること。
     `EA_TRANSFER`（DR=0）の分岐は marker に含め、EXPECTED_BRANCHES の行数を固定する。
     「`hold` を残」す節の遷移先が `hold` を持つ状態（S2。S3・S18 は持たない）だけであること
     （現行の表には「`hold` を残」す節が無く、この検査は将来の記述への番兵）、「`hold` を空」にする節が
     1つ以上検査されること、S2 の FSAVE は、`hold` の注記と遷移先の組（EXPECTED_HOLD）を両方向で
     期待どおりに照合する。応答値の保持・消費の文は、遷移先が PC（UNSTARTED なし）なら「同じ」
     （NULL・TAKE）と「消費」（EA_TRANSFER・MP_REGISTER・FP_REGISTERS）の両方を、括弧の外の primitive 名で
     判定し、IDLE は要求の無い命令の NULL を読んだ後に idle の NULL へ符号化し直す（§5.1）ので検査しない。
     機種別の文は機種ごとに照合する
 9-3. 遷移先の併記（「→Sa/Sb」）は、分岐後の `arith` で各遷移先へ実際に行けること（和集合だけで
     照合しない）。B10 の「b27=0 で再開できる phase」が、S12〜S18 のうち行見出しの `arith` が
     PENDING を含む行の集合と一致すること（command の種類の制限「FMOVEM・control move」「算術・FMOVE」も
     B10 の文言で固定する）。併記は `context_valid` でも検査する。1文の中に `arith` の異なる分岐を
     「→Sa、→Sb」と並べる書き方は、最後の `arith` だけが使われ偽陽性になるので避ける。`hold` の 81 側の比較件数を assert する
 9'. 書き方の検査: 各セルは機種ごとに、遷移（→Sn）・「不変」・V・`false`・「開始/評価」・
     「Sn の … 書込みと同じ」のどれかを1文に書く。`arith` を否定形（「変えない」等）で書かない。
     不変条件は derive_spec に写してある。UNSTARTED のある行に violation 保留、
     SAVE_BODY/RESTORE_BODY に violation 保留を書くと、不変条件1・5の違反になる
 10. tests/data/fpu/fpu2_phase_biu_assignment.csv の列、phase と condition の組の一意性、device・fsave_frame・
     biu_30_28/biu_26・abort_window_68882 の取り得る値、表 A が使う phase の網羅
"""
import csv
import re
import sys
from pathlib import Path

TABLE_A_ROWS = 20
TABLE_A_COLS = 9
TABLE_B_ROWS = 11
INFERENCE_MARK = "【推】"
NA_MARK = "68881 は N/A"
COLS_A = ["ACK", "ABORT", "FSAVE", "FRESTORE（不正 format）", "command 書込み",
          "condition 書込み", "応答読出し", "IA 書込み", "RESET"]
ARITH_ALL = frozenset({"NONE", "PENDING", "PRESENTED_PRE", "PRESENTED_MID"})
DIALOG_PHASES = {"COMMAND_PENDING", "CONDITION_PENDING", "RESPONSE_NEXT", "OPERAND_IN",
                 "OPERAND_OUT", "REGISTER_SELECT", "PC"}
BODY_PHASES = {"SAVE_BODY", "RESTORE_BODY"}
BIU_COLUMNS = ["phase", "condition", "device", "fsave_frame", "biu_30_28", "biu_26",
               "abort_window_68882", "anchor", "basis"]
BIU_DOMAINS = {
    "device": {"both", "68882"},
    "fsave_frame": {"idle", "busy", "02xx"},
    "biu_30_28": {"", "000", "001", "010", "011", "100", "101", "110", "111"},
    "biu_26": {"", "0", "1"},
    "abort_window_68882": {"outside", "inside", "abort_effective"},
}
KEEP_KINDS = ("NULL", "TAKE")
CONSUMED_KINDS = ("EA_TRANSFER", "MP_REGISTER", "FP_REGISTERS")
NEGATIONS = ("同じでは", "同じでな", "返さない", "返さず", "消費しない", "消費されない", "未消費", "消費前",
             "ではない", "変わる", "ことはない")
# 応答値の同一性・消費の検査が対象にすべき遷移（§5.5・§4.1: 最初の primitive の行き先、TF の NULL、BSUN の再読）
EXPECTED_EDGES = {("S12", d) for d in ("S2", "S14", "S15", "S16", "S17", "S18")} | {
    ("S13", "S2"), ("S6", "S8"), ("S14", "S16"), ("S14", "S17"), ("§5.5", "S18")}
# 保持規則（§5.1）に関わる分岐と遷移先の組。S18 の IA 書込みは、入った primitive の種類ごとに
# 外部宛て FMOVE→S14、`EA_TRANSFER`（DR=0）・`MP_REGISTER`→S15（何も符号化しない）、要求の無い命令→S2/S3（§5.5。S18 は `arith`=NONE だけで、`context_valid` により S2 か S3）
EXPECTED_BRANCHES = (
    ("S18", "IA 書込み", "外部宛て FMOVE", {"S14"}),
    ("S18", "IA 書込み", "`EA_TRANSFER`（DR=0）・`MP_REGISTER` で入った", {"S15"}),
    ("S18", "IA 書込み", "要求の無い命令", {"S2", "S3"}),
)
EXPECTED_BRANCH_COUNT = 3
HOLD_STATES = {"S2"}  # `hold` を持つ状態（§5.1）
RE_HOLD = re.compile(r"`hold` (?:を|は)(残|空)")
# セルの `hold` の注記（残す／空）と遷移先の対応。§5.1 の「残る」「空になる」を表側から両方向で照合する
EXPECTED_HOLD = {
    ("S2", "FSAVE"): {"S2": "空"},
}
RE_CLAUSE_SPLIT = re.compile(r"[。、]")
MAX_REF_DEPTH = 4
RE_TARGET = re.compile(r"→\s*(S\d+(?:/S\d+)*)")
RE_REF = re.compile(r"(S\d+) の (command|condition) 書込みと同じ")
RE_ARITH_SET = re.compile(r"`arith`=(NONE|PENDING|PRESENTED_PRE|PRESENTED_MID)")
RE_V = re.compile(r"(?<![A-Za-z_])V(?![A-Za-z_])")


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as f:
        return [row for row in csv.reader(f)]


def split_top(text, sep):
    """括弧の外にある sep で分ける。"""
    parts, depth, cur = [], 0, []
    for ch in text:
        if ch in "（(":
            depth += 1
        elif ch in "）)":
            depth = max(0, depth - 1)
        if ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return [p for p in parts if p.strip()]


def machine_parts(cell, errors, where):
    """セルを {"81": text, "82": text} に分ける。「81:」より前は両機種に共通。"""
    idx = cell.find("81:")
    if idx < 0:
        if "82:" in cell:
            errors.append(f"{where}: 「82:」だけで「81:」が無い")
        return {"81": cell, "82": cell}
    rest = cell[idx + 3:]
    j = rest.find("／82:")
    if j < 0:
        errors.append(f"{where}: 「81:」に対応する「／82:」が無い")
        return {"81": cell, "82": cell}
    prefix = cell[:idx]
    return {"81": prefix + rest[:j], "82": prefix + rest[j + 4:]}


class Tables:
    def __init__(self, body_a, errors):
        self.errors = errors
        self.rows = {}
        for r in body_a:
            m = re.match(r"(S\d+)\s+(\S+?)(?:、|（|\s|$)", r[0])
            if m:
                self.rows[m.group(1)] = r
        self.spec = {}
        for sid, r in self.rows.items():
            self.spec[sid] = self.derive_spec(sid, r[0])

    def derive_spec(self, sid, head):
        """行見出しと不変条件から、その行の性質と取り得る値を決める。"""
        err = self.errors
        phase = re.match(r"S\d+\s+([A-Z_]+)", head).group(1)
        unst = None
        m = re.search(r"UNSTARTED（([^）]*)）", head)
        if m:
            unst = m.group(1)
        violation = "violation 保留" in head
        na = NA_MARK in head
        if "`context_valid` 偽" in head:
            cv = frozenset({False})
        elif "`context_valid` 真" in head or phase != "RESTORE_BODY":
            cv = frozenset({True})
        else:
            cv = frozenset({True, False})
        stated = None
        m = re.search(r"`arith`=([A-Z_]+(?:/[A-Z_]+)*)", head)
        if m:
            vals = m.group(1).split("/")
            stated = frozenset(v if v in ARITH_ALL else f"PRESENTED_{v}" for v in vals)
        elif "`arith` は任意" in head:
            stated = ARITH_ALL
        derived = None
        if unst is not None:
            # 不変条件1: UNSTARTED は IDLE か PC、violation なし、context_valid 真
            if phase not in ("IDLE", "PC"):
                err.append(f"{sid}: 不変条件1 違反（UNSTARTED の phase が {phase}）")
            if violation:
                err.append(f"{sid}: 不変条件1 違反（UNSTARTED と violation 保留）")
            if False in cv:
                err.append(f"{sid}: 不変条件1 違反（UNSTARTED と `context_valid` 偽）")
            # 不変条件3: PC の UNSTARTED は BSUN だけ
            if phase == "PC" and "BSUN" not in unst:
                err.append(f"{sid}: 不変条件3 違反（PC の UNSTARTED が BSUN でない）")
            # 不変条件2
            if "算術" in unst:
                derived = frozenset({"PRESENTED_PRE"})
            elif "BSUN" in unst or "F-line" in unst:
                derived = frozenset({"NONE"})
            else:
                err.append(f"{sid}: UNSTARTED の出所が読めない: {unst!r}")
        elif phase in DIALOG_PHASES:
            derived = frozenset({"NONE", "PENDING"})  # 不変条件4
        elif phase in BODY_PHASES:
            derived = frozenset({"NONE"})  # 不変条件5
            if violation:
                err.append(f"{sid}: 不変条件5 違反（{phase} と violation 保留）")
        if phase == "PC" and not na:
            err.append(f"{sid}: PC の行に「{NA_MARK}」が無い（68881 は PC を持たない）")
        if derived is not None and stated is not None and not stated <= derived:
            err.append(f"{sid}: 行見出しの `arith` {sorted(stated)} が不変条件の {sorted(derived)} を超える")
        arith = stated if stated is not None else derived
        if arith is None:
            err.append(f"{sid}: 行見出しと不変条件から `arith` を決められない")
            arith = ARITH_ALL
        return {"phase": phase, "unst": unst, "violation": violation, "na": na,
                "cv": cv, "arith": arith}

    def cell(self, sid, col):
        return self.rows[sid][1 + COLS_A.index(col)]

    def branches(self, sid, col, cell, machine, arith_in, cv_in, depth, trail):
        """セルの各分岐を (text, arith_after, cv_after, targets, source) で返す。参照を辿る。"""
        err = self.errors
        where = f"{sid}/{col}"
        if depth > MAX_REF_DEPTH:
            err.append(f"{trail}: 参照が深すぎる")
            return []
        part = machine_parts(cell, err, where)[machine]
        out = []
        for seg in split_top(part, "。"):
            sets = RE_ARITH_SET.findall(seg)
            if sets:
                arith = frozenset({sets[-1]})
            elif "`arith` を b27 から作り" in seg or "`arith` を実行の結果から作り" in seg:
                arith = frozenset({"NONE", "PENDING"})
            elif col in ("FSAVE", "RESET") or (machine == "81" and col in ("ACK", "ABORT")):
                arith = frozenset({"NONE"})
            else:
                arith = arith_in
            if "`context_valid` 真" in seg:
                cv = frozenset({True})
            elif "`context_valid` 偽" in seg or col == "RESET":
                cv = frozenset({False})
            else:
                cv = cv_in
            # 1分岐に複数の遷移先があるとき（「真なら →S10、偽なら →S11」等）は和集合で照合する
            targets = []
            for t in RE_TARGET.findall(seg):
                targets += t.split("/")
            targets = [sorted(set(targets))] if targets else []
            if RE_V.search(seg) and not RE_TARGET.search(seg):
                # V: violation 保留（`context_valid` 真なら S10、偽なら S11）。`arith` は不変
                targets = [sorted({"S10" if c else "S11" for c in cv})]
            out.append((seg, arith, cv, targets, trail))
            refs = [(m.group(1), m.group(2)) for m in RE_REF.finditer(seg)]
            if (not refs and col in ("command 書込み", "condition 書込み")
                    and "→" not in cell and ("開始" in seg or "評価" in seg)):
                # 記法:「開始」「評価」は S2（`arith`=NONE）または S3（PENDING）の同じ列と同じ
                for a in arith:
                    ref = {"NONE": "S2", "PENDING": "S3"}.get(a)
                    if ref is None:
                        err.append(f"{trail}: `arith`={a} で「開始/評価」の参照先が無い")
                        continue
                    refs.append((ref, col.split()[0]))
            for ref_sid, kind in refs:
                ref_col = f"{kind} 書込み"
                if ref_sid not in self.rows:
                    err.append(f"{trail}: 参照先 {ref_sid} が無い")
                    continue
                out += self.branches(ref_sid, ref_col, self.cell(ref_sid, ref_col), machine,
                                     arith, cv, depth + 1, f"{trail} → {ref_sid}/{ref_col}")
        return out

    def check_branch(self, machine, br):
        err = self.errors
        seg, arith, cv, targets, trail = br
        for group in targets:
            for t in group:
                if t not in self.spec:
                    err.append(f"{trail}: 存在しない遷移先 {t}")
            if any(t not in self.spec for t in group):
                continue
            allowed_a = frozenset().union(*(self.spec[t]["arith"] for t in group))
            allowed_c = frozenset().union(*(self.spec[t]["cv"] for t in group))
            label = "/".join(group)
            if len(group) > 1:
                # 「→Sa/Sb」は分岐後の `arith` で実際に行ける遷移先の併記。各遷移先が分岐後の値を取り得ること
                for t in group:
                    if not (arith & self.spec[t]["arith"]):
                        err.append(f"{trail} [{machine}]: →{t} へは `arith` {sorted(arith)} では行けない: {seg[:40]!r}")
                    if not (cv & self.spec[t]["cv"]):
                        err.append(f"{trail} [{machine}]: →{t} へは `context_valid` {sorted(cv)} では行けない: {seg[:40]!r}")
            if not arith <= allowed_a:
                err.append(f"{trail} [{machine}]: →{label} の後の `arith` {sorted(arith)} が"
                           f"遷移先の {sorted(allowed_a)} に含まれない: {seg[:40]!r}")
            if not cv <= allowed_c:
                err.append(f"{trail} [{machine}]: →{label} の後の `context_valid` {sorted(cv)} が"
                           f"遷移先の {sorted(allowed_c)} に含まれない: {seg[:40]!r}")
            if machine == "81":
                for t in group:
                    if self.spec[t]["na"]:
                        err.append(f"{trail} [81]: 68881 の分岐が N/A 行 {t} へ遷移する: {seg[:40]!r}")


def check_semantics(tables, table_b, errors):
    rows = tables.rows
    # 5. N/A 行のセルに「81:」を書かない
    for sid, r in rows.items():
        if tables.spec[sid]["na"]:
            for col, c in zip(COLS_A, r[1:]):
                if "81:" in c:
                    errors.append(f"{sid}/{col}: N/A 行に「81:」がある")
    # 7. 表 A の全分岐
    for sid, r in rows.items():
        sp = tables.spec[sid]
        machines = ["82"] if sp["na"] else ["81", "82"]
        for col, c in zip(COLS_A, r[1:]):
            for m in machines:
                for br in tables.branches(sid, col, c, m, sp["arith"], sp["cv"], 0, f"{sid}/{col}"):
                    tables.check_branch(m, br)
    # 7. 表 B（`begin_restore()` 後: `arith`=NONE、UNSTARTED・violation なし）
    for r in table_b[1:]:
        bid = r[0].split()[0]
        applied = "適用しない" not in r[1]
        cv = frozenset({True}) if applied else frozenset({True, False})
        for m in ["81", "82"]:
            for br in tables.branches(bid, "FRESTORE（有効 format）", r[1], m,
                                      frozenset({"NONE"}), cv, 0, bid):
                tables.check_branch(m, br)
    # 6'. 各セルは機種ごとに、遷移か「不変」か V を少なくとも1文に書く（書き方の揺れで遷移が検査から外れるのを防ぐ）
    for sid, r in rows.items():
        sp = tables.spec[sid]
        for col, c in zip(COLS_A, r[1:]):
            for m in (["82"] if sp["na"] else ["81", "82"]):
                part = machine_parts(c, [], sid)[m]
                if not any(RE_TARGET.search(g) or "不変" in g or RE_V.search(g) or RE_REF.search(g)
                           or "開始" in g or "評価" in g or "`false`" in g
                           for g in split_top(part, "。")):
                    errors.append(f"{sid}/{col} [{m}]: 遷移（→Sn）・「不変」・V のどれも書いていない")
        # `arith` を否定形で書かない（`arith`=値 の形か、書かないかのどちらかにする）
        for col, c in zip(COLS_A, r[1:]):
            if re.search(r"`arith`[^。|]*?(変えない|にしない|でない|しない)", c):
                errors.append(f"{sid}/{col}: `arith` を否定形で書いている（`arith`=値 の形にする）")
    # 8. S4 へ遷移する command/condition 書込みは `arith`=PRESENTED_PRE を自身に書く
    for sid, r in rows.items():
        sp = tables.spec[sid]
        for col in ("command 書込み", "condition 書込み"):
            c = tables.cell(sid, col)
            reach = set()
            for m in (["82"] if sp["na"] else ["81", "82"]):
                for br in tables.branches(sid, col, c, m, sp["arith"], sp["cv"], 0, sid):
                    for g in br[3]:
                        reach.update(g)
            s4 = [s for s, v in tables.spec.items() if v["arith"] == frozenset({"PRESENTED_PRE"})]
            if reach & set(s4) and "`arith`=PRESENTED_PRE" not in c:
                errors.append(f"{sid}/{col}: {sorted(reach & set(s4))} へ遷移するのに"
                              "`arith`=PRESENTED_PRE を書いていない")
    # 9. 応答読出しでの遷移と、遷移先の応答の同一性・消費
    def kind_of(text):
        head = split_top(text, "。")[0] if text else ""
        if "最初の primitive" in head:
            return "mixed"
        if "後続 primitive" in head or any(k in head for k in CONSUMED_KINDS):
            return "consume"
        if any(k in head for k in KEEP_KINDS):
            return "keep"
        return None

    def strip_paren(text):
        """括弧（入れ子を含む）の中を除く。primitive 名の判定を本文だけで行うため。"""
        out, depth = [], 0
        for ch in text:
            if ch in "（(":
                depth += 1
            elif ch in "）)":
                depth = max(0, depth - 1)
            elif depth == 0:
                out.append(ch)
        return "".join(out)

    def seg_has(cell, word, kinds=None, extra=None):
        """セルの「。」で分けた1文（括弧の外）に word があり、kinds のどれか（括弧の外）と
        extra があり、否定語が無いか。"""
        for seg in split_top(cell, "。"):
            if any(n in seg for n in NEGATIONS):
                continue
            # 「消費」は括弧内の「消費済み」で書くので括弧ごと見る。「同じ」と追加語は括弧の外だけを見る
            if word not in (seg if word == "消費" else strip_paren(seg)):
                continue
            if kinds is not None and not any(k in strip_paren(seg) for k in kinds):
                continue
            if extra is not None and extra not in strip_paren(seg):
                continue
            return True
        return False

    fired = set()

    def require(src, dst, need, why, m):
        """遷移先 dst の応答読出しセル（機種 m の分）が、need（(語, 種類, 追加語) の列）を
        否定でない1文の中に持つことを検査する。"""
        fired.add((src, dst))
        c = machine_parts(tables.cell(dst, "応答読出し"), [], dst)[m]
        for word, kinds, extra in need:
            ok = seg_has(c, word, kinds, extra)
            # 「後続 primitive」を返す RESPONSE_NEXT だけは、消費の代わりにそれを書く
            if word == "消費" and not ok and tables.spec[dst]["phase"] == "RESPONSE_NEXT":
                ok = "後続 primitive" in split_top(c, "。")[0]
            if not ok:
                errors.append(f"{dst}/応答読出し [{m}]: {why} から入るのに、{'/'.join(kinds or ())}"
                              f"「{word}」{extra or ''}を含む否定でない1文が無い: {c[:40]!r}")

    keep_need = [("同じ", None, None)]
    consume_need = [("消費", None, None)]

    def mixed_need(dst):
        """種類が命令で決まる最初の primitive から dst へ入るとき、dst が持つべき文。"""
        ph = tables.spec[dst]["phase"]
        if ph == "PC":
            return [("同じ", KEEP_KINDS, None), ("消費", CONSUMED_KINDS, None)]
        if ph == "IDLE":
            return []  # 要求の無い命令の NULL は読んだ後に idle の NULL へ符号化し直す（§5.1）
        return consume_need

    for sid in rows:
        sp = tables.spec[sid]
        c = tables.cell(sid, "応答読出し")
        for m in (["82"] if sp["na"] else ["81", "82"]):
            part = machine_parts(c, [], sid)[m]
            for seg in split_top(part, "。"):
                for t in RE_TARGET.findall(seg):
                    for dst in t.split("/"):
                        if dst == sid or dst not in rows:
                            continue
                        k = kind_of(c)
                        if k == "keep":
                            require(sid, dst, keep_need, f"{sid} の応答読出し（消費されない primitive）", m)
                        elif k == "consume":
                            require(sid, dst, consume_need, f"{sid} の応答読出し（消費される primitive）", m)
                        elif k == "mixed":
                            require(sid, dst, mixed_need(dst), f"{sid} の最初の primitive", m)
    pc_kinds = tables.pc_first_kinds
    for sid, sp in tables.spec.items():
        if sp["phase"] == "PC" and sp["unst"] is None:
            need = []
            if any(k in pc_kinds for k in KEEP_KINDS):
                need.append(("同じ", KEEP_KINDS, None))
            if any(k in pc_kinds for k in CONSUMED_KINDS):
                need.append(("消費", CONSUMED_KINDS, None))
            if not need:
                errors.append("§5.5 の表の PC=1 の行から primitive の種類を読めない")
            require("§5.5", sid, need, f"§5.5 の PC=1 の最初の primitive（{'/'.join(pc_kinds)}）", "82")
    # 期待する遷移（§5.5・§4.1 から導く）が検査されたこと、S12 の遷移先が phase から導いた集合と一致すること
    for edge in EXPECTED_EDGES:
        if edge not in fired:
            errors.append(f"応答値の同一性・消費の検査が {edge[0]}→{edge[1]} を対象にしていない")
    plain = {sid for sid, v in tables.spec.items()
             if v["phase"] == "IDLE" and v["unst"] is None and not v["violation"]
             and v["cv"] == frozenset({True}) and v["arith"] == frozenset({"NONE"})}
    dialog = {sid for sid, v in tables.spec.items()
              if v["phase"] in ("RESPONSE_NEXT", "OPERAND_IN", "OPERAND_OUT", "REGISTER_SELECT")}
    pcs = {sid for sid, v in tables.spec.items() if v["phase"] == "PC" and v["unst"] is None}
    c12 = tables.cell("S12", "応答読出し")
    pending_idle = {sid for sid, v in tables.spec.items()
                    if v["phase"] == "IDLE" and v["unst"] is None and not v["violation"]
                    and v["cv"] == frozenset({True}) and v["arith"] == frozenset({"PENDING"})}
    # 要求の無い命令の完了が保留算術例外を作ると S3 に入る（§5.1 の実装での確定）
    for m, want in (("81", plain | pending_idle | dialog), ("82", plain | pending_idle | dialog | pcs)):
        got = set()
        for seg in split_top(machine_parts(c12, [], "S12")[m], "。"):
            for t in RE_TARGET.findall(seg):
                got.update(t.split("/"))
        if got != want:
            errors.append(f"S12/応答読出し [{m}]: 遷移先 {sorted(got)} が phase から導いた {sorted(want)} と違う")
    # 応答値の保持に関わる分岐と遷移先の組（集合の比較では分岐の入替えが通るため、分岐ごとに照合する）
    for sid, col, marker, want in EXPECTED_BRANCHES:
        segs = [g for g in split_top(machine_parts(tables.cell(sid, col), [], sid)["82"], "。")
                if marker in g]
        got = set()
        for g in segs:
            for t in RE_TARGET.findall(g):
                got.update(t.split("/"))
        if not segs or got != want:
            errors.append(f"{sid}/{col}: 「{marker}」の分岐の遷移先 {sorted(got)} が期待 {sorted(want)} と違う")
    # `hold` の注記（残す／空）と遷移先を節（「。」「、」で分けた単位）ごとに組にする。
    # 「残す」節の遷移先は `hold` を持つ状態だけ（S3 は持たない）。EXPECTED_HOLD のセルは両方向で照合する
    emptied_clauses = 0
    compared_81 = 0
    for sid, r in rows.items():
        for col, c in zip(COLS_A, r[1:]):
            want = EXPECTED_HOLD.get((sid, col))
            for m in (["82"] if tables.spec[sid]["na"] else ["81", "82"]):
                got = {}
                for clause in RE_CLAUSE_SPLIT.split(machine_parts(c, [], sid)[m]):
                    mark = RE_HOLD.search(clause)
                    if not mark:
                        continue
                    targets = {t for tt in RE_TARGET.findall(clause) for t in tt.split("/")}
                    if mark.group(1) == "空":
                        emptied_clauses += 1
                    if mark.group(1) == "残":
                        bad = targets - HOLD_STATES
                        if bad:
                            errors.append(f"{sid}/{col} [{m}]: `hold` を残す節が `hold` を持たない状態 "
                                          f"{sorted(bad)} へ遷移する: {clause[:40]!r}")
                    for t in targets:
                        got[t] = mark.group(1)
                # EXPECTED_HOLD は 68882 の期待値。N/A でない行は 68881 も同じ注記を要求する（N/A 行は比較しない）
                if want is not None and m == "82" and got != want:
                    errors.append(f"{sid}/{col}: `hold` の注記と遷移先の組 {got} が期待 {want} と違う")
                if want is not None and m == "81":
                    compared_81 += 1
                    if got != want:
                        errors.append(f"{sid}/{col} [81]: `hold` の注記と遷移先の組 {got} が 68882 の期待 {want} と違う")
    n81 = sum(1 for (sid, _c) in EXPECTED_HOLD if not tables.spec[sid]["na"])
    if compared_81 != n81:
        errors.append(f"`hold` の 81 側の比較が {compared_81} 件で、期待の {n81} 件と違う")
    # B10 の「b27=0 で再開できる phase」（S12、S14〜S17）が、S12〜S18 のうち行見出しの `arith` が PENDING を含む行と一致する
    b10 = next((r[1] for r in table_b[1:] if r[0].startswith("B10")), "")
    if not re.search(r"b27=0（PENDING）で再開できるのは、PENDING を素通しする FMOVEM・control move の command の phase"
                     r"（S12、S14〜S17）だけで、S13 と S18 と、算術・FMOVE の command の phase は b27=0 だと範囲外として"
                     r" B11 と同じ扱いにする", b10):
        errors.append("B10 の「b27=0 で再開できる phase と範囲外」の文が固定の文言と違う")
    m = re.search(r"（S(\d+)、S(\d+)〜S(\d+)）だけ", b10)
    if not m:
        errors.append("B10 の「b27=0 で再開できる phase」の記述が見つからない")
    else:
        listed = {f"S{m[1]}"} | {f"S{i}" for i in range(int(m[2]), int(m[3]) + 1)}
        pending = {f"S{i}" for i in range(12, 19) if "PENDING" in tables.spec[f"S{i}"]["arith"]}
        if listed != pending:
            errors.append(f"B10 の b27=0 で再開できる phase {sorted(listed)} が、行見出しの `arith` が"
                          f"PENDING を含む行 {sorted(pending)} と違う")
    if emptied_clauses < 1:
        errors.append("`hold` を空にする節が1つも検査されなかった（言い回しが変わった）")
    for key in EXPECTED_HOLD:
        if key[0] not in rows or key[1] not in COLS_A:
            errors.append(f"EXPECTED_HOLD の {key} が表に無い")
    if len(EXPECTED_BRANCHES) != EXPECTED_BRANCH_COUNT:
        errors.append("EXPECTED_BRANCHES の行数が変わった")


def check_biu(biu_rows, biu_header, table_phases, errors):
    """BIU 割当て CSV の形と値の取り得る範囲、表 A が使う phase の網羅を検査する。"""
    if biu_header != BIU_COLUMNS:
        errors.append(f"BIU CSV の列 {biu_header} != {BIU_COLUMNS}")
        return
    seen = set()
    for r in biu_rows:
        if len(r) != len(BIU_COLUMNS):
            errors.append(f"BIU CSV: 列数 {len(r)} != {len(BIU_COLUMNS)}: {r[:2]}")
            continue
        rec = dict(zip(BIU_COLUMNS, r))
        where = f"BIU CSV {rec['phase']}/{rec['condition'][:16]}"
        if (rec["phase"], rec["condition"]) in seen:
            errors.append(f"{where}: phase と condition の組が重複している")
        seen.add((rec["phase"], rec["condition"]))
        for col, allowed in BIU_DOMAINS.items():
            if rec[col] not in allowed:
                errors.append(f"{where}: {col} {rec[col]!r} が取り得る値 {sorted(allowed)} に無い")
        if (rec["biu_30_28"] == "") != (rec["biu_26"] == ""):
            errors.append(f"{where}: biu_30_28 と biu_26 は両方書くか両方空にする")
    missing = sorted(table_phases - {phase for phase, _ in seen})
    if missing:
        errors.append(f"表 A が使う phase が BIU CSV に無い: {missing}")


def main():
    root = Path(__file__).resolve().parent.parent
    if "--root" in sys.argv:
        root = Path(sys.argv[sys.argv.index("--root") + 1]).resolve()
    csv_a_path = root / "tests" / "data" / "fpu" / "fpu2_table_a.csv"
    csv_b_path = root / "tests" / "data" / "fpu" / "fpu2_table_b.csv"
    biu_path = root / "tests" / "data" / "fpu" / "fpu2_phase_biu_assignment.csv"

    table_a = read_csv(csv_a_path)
    table_b = read_csv(csv_b_path)

    errors = []
    for path in (csv_a_path, csv_b_path, biu_path):
        if not path.exists():
            errors.append(f"{path.name} が無い")
    if errors:
        print("\n".join(errors))
        return 1
    body_a = table_a[1:]
    body_b = table_b[1:]
    # 2. 形状と通し番号
    if table_a[0][1:] != COLS_A:
        errors.append(f"表 A の列見出し {table_a[0][1:]} != {COLS_A}")
    if len(body_a) != TABLE_A_ROWS:
        errors.append(f"表 A の行数 {len(body_a)} != {TABLE_A_ROWS}")
    for r in body_a:
        if len(r) != TABLE_A_COLS + 1:
            errors.append(f"表 A: 列数 {len(r)} != {TABLE_A_COLS + 1}: {r[0][:20]!r}")
    if len(body_b) != TABLE_B_ROWS:
        errors.append(f"表 B の行数 {len(body_b)} != {TABLE_B_ROWS}")
    for r in body_b:
        if len(r) != 2:
            errors.append(f"表 B: 列数 {len(r)} != 2: {r[0][:20]!r}")
    for i, r in enumerate(body_a, 1):
        if not re.match(rf"S{i}\b", r[0]):
            errors.append(f"表 A: {i} 行目の見出しが S{i} でない: {r[0][:20]!r}")
    for i, r in enumerate(body_b, 1):
        if not re.match(rf"B{i}\b", r[0]):
            errors.append(f"表 B: {i} 行目の見出しが B{i} でない: {r[0][:20]!r}")
    if errors:
        print("\n".join(errors))
        return 1

    cells_a = [c for r in body_a for c in r[1:]]
    inf_a = sum(INFERENCE_MARK in c for c in cells_a)
    inf_b = sum(INFERENCE_MARK in r[1] for r in body_b)

    # 4〜9. 意味の検査
    tables = Tables(body_a, errors)
    # 最初の primitive が PC=1 の行へ入るときは、NULL・TAKE（同じ）と消費される種類の両方がありうるので、
    # 両方を要求する
    tables.pc_first_kinds = list(KEEP_KINDS + CONSUMED_KINDS)
    check_semantics(tables, table_b, errors)

    # BIU CSV
    biu = read_csv(biu_path)
    check_biu(biu[1:], biu[0], {sp["phase"] for sp in tables.spec.values()}, errors)
    na_text = [s for s, v in tables.spec.items() if v["na"]]

    if errors:
        errors = list(dict.fromkeys(errors))  # 機種ループ等で重なった同じ違反は1件にする
        print("\n".join(errors))
        print(f"NG: {len(errors)} 件")
        return 1
    print(f"OK: 表 A {len(body_a)}x{TABLE_A_COLS}={len(cells_a)} セル（推 {inf_a}）、"
          f"表 B {len(body_b)} 行（推 {inf_b}）、N/A {'・'.join(na_text)}、"
          f"BIU CSV {len(biu) - 1} 行")
    return 0


if __name__ == "__main__":
    sys.exit(main())
