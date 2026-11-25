#!/usr/bin/env python3
"""spec_mc68000.md §5.1 のEA分類表を PM Table 2-4 (MC68000 の行) と突き合わせる。

期待値は PM Table 2-4 [PM §2.3, printed pp.2-19-2-20 / PDF pp.59-60] から転記した独立データで、
実装やopcode規則CSVには依存しない。MC68020以降の行(bd/メモリ間接)は含めない。
実行: python3 tools/check_ea_categories.py (リポジトリルートで)
"""
import re
import sys

SPEC = "docs/mc68000-internals/spec_mc68000.md"

# EA-ID -> (data, memory, control, alterable)。X=True, —=False。
TABLE_2_4 = {
    "EA-001": (True, False, False, True),   # Dn
    "EA-002": (False, False, False, True),  # An
    "EA-003": (True, True, True, True),     # (An)
    "EA-004": (True, True, False, True),    # (An)+
    "EA-005": (True, True, False, True),    # -(An)
    "EA-006": (True, True, True, True),     # (d16,An)
    "EA-007": (True, True, True, True),     # (d8,An,Xn)
    "EA-008": (True, True, True, True),     # (xxx).W
    "EA-009": (True, True, True, True),     # (xxx).L
    "EA-010": (True, True, True, False),    # (d16,PC)
    "EA-011": (True, True, True, False),    # (d8,PC,Xn)
    "EA-012": (True, True, False, False),   # #<xxx>
}
CATEGORIES = ["data", "memory", "control", "alterable"]
# 組合せ分類は定義どおり(§2.3): alterable memory = alterable かつ memory、data alterable = data かつ alterable
COMBINED = {
    "data alterable": lambda t: t[0] and t[3],
    "memory alterable": lambda t: t[1] and t[3],
}


def expand(text):
    """'EA-001、EA-003–EA-009（Anを含む）' を EA-ID の集合へ展開する。"""
    ids = set()
    text = re.sub(r"（[^）]*）", "", text)
    for part in re.split(r"[、,]", text):
        part = part.strip()
        m = re.fullmatch(r"EA-(\d{3})(?:[–-]EA-(\d{3}))?", part)
        if not m:
            raise ValueError(f"cannot parse EA list element: {part!r}")
        lo = int(m.group(1))
        hi = int(m.group(2) or m.group(1))
        ids |= {f"EA-{n:03d}" for n in range(lo, hi + 1)}
    return ids


def main():
    spec = open(SPEC, encoding="utf-8").read()
    errors = []
    spec_sets = {}
    for m in re.finditer(r"^\| (data alterable|memory alterable|data|memory|control|alterable) \| (EA-[^|]+?) \|$", spec, re.M):
        spec_sets[m.group(1)] = expand(m.group(2))
    expected = {}
    for i, name in enumerate(CATEGORIES):
        expected[name] = {ea for ea, t in TABLE_2_4.items() if t[i]}
    for name, fn in COMBINED.items():
        expected[name] = {ea for ea, t in TABLE_2_4.items() if fn(t)}
    for name in sorted(expected):
        if name not in spec_sets:
            errors.append(f"spec §5.1 has no row for category '{name}'")
        elif spec_sets[name] != expected[name]:
            errors.append(f"category '{name}': spec={sorted(spec_sets[name])} Table 2-4={sorted(expected[name])}")
    if errors:
        for e in errors:
            print("error:", e)
        return 1
    print(f"ok: {len(expected)} EA categories match PM Table 2-4")
    return 0


if __name__ == "__main__":
    sys.exit(main())
