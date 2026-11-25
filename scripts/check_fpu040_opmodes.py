#!/usr/bin/env python3
"""tests/data/fpu/mc68040_fpu_opmodes.csv と src/cpu/ops_fpu.c の classify_opmode() を突き合わせる（Issue #87 D-N1）。

  check_fpu040_opmodes.py                  CSV と classify_opmode() の一致、CSV の形状・件数を検査する
  check_fpu040_opmodes.py --refs DIR       さらに DIR の一次資料（pdftotext が必要）と照合する:
                                           M68040UM.PDF の Table 9-10・Table E-1/E-2、MC68881UM.pdf の Table 4-13、
                                           M68000PM.pdf の強制精度命令の opmode（各命令頁）
  check_fpu040_opmodes.py --root DIR       DIR の tests/data/fpu/ と src/ を検査する（変異確認用）

CSV（opmode,name,class,source）は 128 行（opmode $00〜$7F。拡張語 bits 6-0）。class は impl（040 が実行）、
unimpl（format $2 の未実装命令）、illegal（F-line 例外 format $0）。
"""
import argparse
import csv
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OPMODE_COUNT = 128
CLASSES = ("impl", "unimpl", "illegal")
FSINCOS_RANGE = range(0x30, 0x38)
MIN_881_PAIRS = 30  # Table 4-13 の OCR で読めた「$NN 名前」の最小件数（OCR が壊れたときの検出）
MIN_PRM_PAIRS = 24  # 強制精度命令 16 + 基本 8（FMOVE/FSQRT/FABS/FNEG/FDIV/FADD/FMUL/FSUB）


def load_csv(root):
    rows = {}
    errors = []
    with open(root / "tests/data/fpu/mc68040_fpu_opmodes.csv", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            m = re.fullmatch(r"\$([0-9A-F]{2})", r["opmode"])
            if not m:
                errors.append(f"CSV: opmode の書式が不正: {r['opmode']}")
                continue
            op = int(m.group(1), 16)
            if op in rows:
                errors.append(f"CSV: opmode ${op:02X} が重複")
            if r["class"] not in CLASSES:
                errors.append(f"CSV: ${op:02X} の class が不正: {r['class']}")
            if (r["class"] == "illegal") != (r["name"] == ""):
                errors.append(f"CSV: ${op:02X} は illegal のときだけ name が空")
            if not r["source"]:
                errors.append(f"CSV: ${op:02X} の source が空")
            rows[op] = r
    if sorted(rows) != list(range(OPMODE_COUNT)):
        errors.append(f"CSV: opmode が $00〜$7F の 128 行でない（{len(rows)} 行）")
    return rows, errors


def parse_classify(root):
    """classify_opmode() の case 群を読み、opmode → class を返す。"""
    text = (root / "src/cpu/ops_fpu.c").read_text(encoding="utf-8")
    m = re.search(r"static opmode_class_t classify_opmode\(unsigned opmode\) \{(.*?)\n\}\n", text, re.S)
    if not m:
        sys.exit("classify_opmode() が見つからない")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    result = {}
    pending = []
    for tok in re.finditer(r"case (0x[0-9A-Fa-f]+):|return ([^;]*);", body):
        if tok.group(1):
            pending.append(int(tok.group(1), 16))
        else:
            expr = tok.group(2).strip()
            if expr == "OPMODE_IMPL":
                cls = "impl"
            elif expr == "OPMODE_UNIMPL":
                cls = "unimpl"
            else:
                # default: `opmode >= 0x30 && opmode <= 0x37 ? OPMODE_UNIMPL : OPMODE_ILLEGAL`
                if not re.fullmatch(r"opmode >= 0x30 && opmode <= 0x37 \? OPMODE_UNIMPL\s*:\s*OPMODE_ILLEGAL", expr):
                    sys.exit(f"classify_opmode(): 想定外の default: {expr}")
                for op in range(OPMODE_COUNT):
                    result.setdefault(op, "unimpl" if op in FSINCOS_RANGE else "illegal")
                continue
            for op in pending:
                if op in result:
                    sys.exit(f"classify_opmode(): ${op:02X} が複数の case にある")
                result[op] = cls
            pending = []
    return result


def pdftotext(pdf):
    return subprocess.run(["pdftotext", "-layout", str(pdf), "-"], check=True, capture_output=True, text=True).stdout


def check_refs(rows, refs):
    errors = []
    by_name = {r["name"]: op for op, r in rows.items() if r["name"]}

    um40 = pdftotext(refs / "M68040UM.PDF")
    t910 = um40.split("Table 9-10. Unimplemented Instructions", 1)[1].split("When an unimplemented", 1)[0]
    names910 = set(re.findall(r"\bF[A-Z0-9]+\b", t910)) - {"FMOVECR"}  # FMOVECR は opmode でなく source specifier
    unimpl = {r["name"] for r in rows.values() if r["class"] == "unimpl"}
    extra = {"FLOG2"}  # Table E-2: FPSP が実装する（Table 9-10 に無い）
    if names910 != unimpl - extra:
        errors.append(f"Table 9-10 と CSV の unimpl が不一致: 表のみ {sorted(names910 - unimpl)}、CSV のみ {sorted(unimpl - extra - names910)}")
    e2 = um40.split("Table E-2. M68040FPSP Floating-Point Instructions", 1)[1][:6000]
    if not all(re.search(rf"\b{n}\b", e2) for n in extra):
        errors.append("Table E-2 に FLOG2 が見つからない")
    e1 = um40.split("Table E-1. MC68040 Floating-Point Instructions", 1)[1].split("Table E-2", 1)[0]
    # FDSQRT は Table E-1 に無く、Table E-2（FPSP の表。`*` は 040 が実行する印）にだけある。
    names_e1 = set(re.findall(r"\bF[A-Z0-9]+\b", e1)) | set(re.findall(r"\b(F[A-Z0-9]+)\*", e2))
    for op, r in rows.items():
        if r["class"] == "impl" and r["name"] not in names_e1 and r["name"] not in ("FSGLDIV",):
            errors.append(f"${op:02X} {r['name']} が Table E-1/E-2 に無い")

    um881 = pdftotext(refs / "MC68881UM.pdf")
    t413 = um881.split("Table 4-13. Extension Field Encoding for Arithmetic Operations", 1)[1][:4500]
    pairs = re.findall(r"\$([0-9A-F]{2})\s+[!~'.]?(F[A-Z0-9]+)", t413)  # OCR の記号ごみを許す
    if len(pairs) < MIN_881_PAIRS:
        errors.append(f"Table 4-13 の読取りが {len(pairs)} 件しかない（OCR が壊れた？）")
    for hexop, name in pairs:
        op = int(hexop, 16)
        if name == "FSINCOS":
            continue  # $30-$37 の範囲は行頭の表記が別
        if name not in by_name:
            if name != "FAOD":  # OCR: FADD
                errors.append(f"Table 4-13 の {name} が CSV に無い")
        elif by_name[name] != op:
            errors.append(f"Table 4-13: {name} は ${op:02X} だが CSV は ${by_name[name]:02X}")

    prm = pdftotext(refs / "M68000PM.pdf")
    prm_pairs = set(re.findall(r"^\s*([01]{7})\s+(F[A-Z0-9]+)\b", prm, re.M))
    if len(prm_pairs) < MIN_PRM_PAIRS:
        errors.append(f"PRM の opmode 行が {len(prm_pairs)} 件しかない")
    for bits, name in sorted(prm_pairs):
        if by_name.get(name) != int(bits, 2):
            errors.append(f"PRM: {name} は {bits} だが CSV は {by_name.get(name)}")
    forced = {n for n in by_name if re.match(r"F[SD](MOVE|SQRT|ABS|NEG|DIV|ADD|MUL|SUB)$", n)}
    if forced != {n for _, n in prm_pairs if n in forced}:
        errors.append(f"PRM に無い強制精度命令: {sorted(forced - {n for _, n in prm_pairs})}")
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("--refs", type=Path)
    args = ap.parse_args()

    rows, errors = load_csv(args.root)
    if not errors:
        code = parse_classify(args.root)
        if sorted(code) != list(range(OPMODE_COUNT)):
            errors.append("classify_opmode() が全 opmode を分類していない")
        for op in range(OPMODE_COUNT):
            if code.get(op) != rows[op]["class"]:
                errors.append(f"${op:02X} {rows[op]['name']}: CSV は {rows[op]['class']}、classify_opmode() は {code.get(op)}")
        if args.refs:
            errors += check_refs(rows, args.refs)
    for e in errors:
        print("NG:", e)
    if errors:
        return 1
    print(f"OK: {len(rows)} opmodes" + ("（一次資料と照合）" if args.refs else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
