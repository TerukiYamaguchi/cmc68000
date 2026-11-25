#!/usr/bin/env python3
"""spec_mc68000.md の規範ID・ケースIDと conformance_map_mc68000.csv の整合を検査する。

検査内容:
  1. spec の本文に現れる全規範ID(REG/BUS/EA/INS/PRV/EXC/IRQ/RST/TIM-nnn)がCSVに1行ある。
  2. CSVの規範IDがspecに存在する(廃止IDは status=retired でのみ許可)。
  3. specの全ケースID(T-<規範ID>-nn)がCSVのcase_idsに現れ、CSVのcase_idsがspecに存在する。
  4. status が covered の行は case_ids・source_ref・machine_tests が非空。
     defined の行は case_ids・source_ref が非空で machine_tests が空。
  5. status が gap の行は gap_note に理由を持つ。
  6. ケースIDは本文の表の行頭(`| T-...`)で定義されたものだけを「定義済み」とする(本文中の言及は不可)。
  7. machine_tests の各要素は tests/CMakeLists.txt に登録された名前、または実在するパスでなければならない。
  8. 本文に現れる未解決ID(TIM-U<n>)は §14.2 の表の行頭に定義されていなければならない。
  (複数の規範IDを1ケースで検証する場合、ケースIDは先頭の規範IDの名前を持ち、
   他の規範IDの行にも同じケースIDを書いてよい。)
"""
import csv
import os
import re
import sys

SPEC = "docs/mc68000-internals/spec_mc68000.md"
MAP = "tests/data/cpu/conformance_map_mc68000.csv"
PREFIX = r"(?:REG|BUS|EA|INS|PRV|EXC|IRQ|RST|TIM)"
NORM_RE = re.compile(rf"\b{PREFIX}-\d{{3}}\b")
CASE_RE = re.compile(rf"\bT-({PREFIX}-\d{{3}})-(\d{{2}})(?:(?:/|\.\.)(\d{{2}}))?")
STATUSES = {"covered", "defined", "gap", "retired"}
COLUMNS = ["norm_id", "axes", "spec_section", "source_ref", "case_ids", "machine_tests", "status", "gap_note"]


def expand_cases(text):
    cases = set()
    for m in CASE_RE.finditer(text):
        norm, lo, hi = m.group(1), int(m.group(2)), m.group(3)
        hi = int(hi) if hi else lo
        for n in range(lo, hi + 1):
            cases.add(f"T-{norm}-{n:02d}")
    return cases


def main():
    spec = open(SPEC, encoding="utf-8").read()
    rows = list(csv.DictReader(open(MAP, encoding="utf-8")))
    errors = []
    if rows and list(rows[0].keys()) != COLUMNS:
        errors.append(f"columns must be {COLUMNS}")
    spec_norms = set(NORM_RE.findall(spec))
    spec_cases = set()
    for line in spec.splitlines():
        if line.startswith("| T-"):
            spec_cases |= expand_cases(line.split("|")[1])
    mentioned = expand_cases(spec)
    for c in sorted(mentioned - spec_cases):
        errors.append(f"case {c} is mentioned but never defined as a table row")
    for u in sorted(set(re.findall(r"\bTIM-U\d+\b", spec))):
        if not re.search(rf"^\| {u} \|", spec, re.M):
            errors.append(f"unresolved id {u} has no table row")
    cmake = open("tests/CMakeLists.txt", encoding="utf-8").read()
    map_norms = {}
    map_cases = set()
    for r in rows:
        nid = r["norm_id"]
        if nid in map_norms:
            errors.append(f"duplicate norm_id {nid}")
        map_norms[nid] = r
        if r["status"] not in STATUSES:
            errors.append(f"{nid}: bad status {r['status']}")
        cases = expand_cases(r["case_ids"].replace(";", " "))
        map_cases |= cases
        has_ref = bool(cases and r["source_ref"].strip())
        if r["status"] == "covered" and not (has_ref and r["machine_tests"].strip()):
            errors.append(f"{nid}: covered requires case_ids, source_ref and machine_tests")
        if r["status"] == "defined" and not (has_ref and not r["machine_tests"].strip()):
            errors.append(f"{nid}: defined requires case_ids and source_ref, and empty machine_tests")
        for t in filter(None, (x.strip() for x in r["machine_tests"].split(";"))):
            if not (os.path.exists(t) or re.search(rf"(?:NAME\s+|add_executable\()\s*{re.escape(t)}\b", cmake)):
                errors.append(f"{nid}: machine test {t} not found")
        if r["status"] == "gap" and not r["gap_note"].strip():
            errors.append(f"{nid}: gap requires gap_note")
    for n in sorted(spec_norms - set(map_norms)):
        errors.append(f"spec norm {n} missing in map")
    for n in sorted(set(map_norms) - spec_norms):
        if map_norms[n]["status"] != "retired":
            errors.append(f"map norm {n} not in spec")
    for c in sorted(spec_cases - map_cases):
        errors.append(f"spec case {c} missing in map")
    for c in sorted(map_cases - spec_cases):
        errors.append(f"map case {c} not defined in spec")
    if errors:
        for e in errors:
            print("error:", e)
        return 1
    counts = {k: sum(1 for r in rows if r["status"] == k) for k in sorted(STATUSES)}
    print(f"ok: {len(rows)} norm ids {counts}, {len(spec_cases)} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
