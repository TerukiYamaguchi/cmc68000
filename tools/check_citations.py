#!/usr/bin/env python3
"""spec_mc68000.md の `[UM|PM ... printed X / PDF p.N]` 形式の引用を、PDF の実ページと照合する。

各引用について、PDF の N ページに印刷ページ番号 X（例: 6-12、B-3）が現れるかを検査する。
PDF は配布対象外(x68000/local/refs/)のため ctest には登録しない。手元で次のように実行する。
  python3 tools/check_citations.py <M68000UM.pdf> <M68000PM.pdf>
pdftotext が必要。
"""
import re
import subprocess
import sys

SPEC = "docs/mc68000-internals/spec_mc68000.md"
LABEL_RE = re.compile(r"\b([0-9]{1,2}|[A-D])-([0-9]{1,3})\b")
CITE_RE = re.compile(r"\[(UM|PM)([^\]]*?)\]")
PRINTED_RE = re.compile(r"printed (?:pp?\.)?\s*([0-9A-D]+-[0-9]+)[^/]*/\s*PDF (?:pp?\.)?\s*(\d+)")


def page_labels(pdf, page, cache):
    key = (pdf, page)
    if key not in cache:
        text = subprocess.run(["pdftotext", "-layout", "-f", str(page), "-l", str(page), pdf, "-"],
                              capture_output=True, text=True, check=True).stdout
        cache[key] = {f"{a}-{b}" for a, b in LABEL_RE.findall(text)}
    return cache[key]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    pdfs = {"UM": sys.argv[1], "PM": sys.argv[2]}
    spec = open(SPEC, encoding="utf-8").read()
    cache, checked, bad = {}, 0, []
    for doc, body in CITE_RE.findall(spec):
        m = PRINTED_RE.search(body)
        if not m:
            continue
        checked += 1
        printed, page = m.group(1), int(m.group(2))
        if printed not in page_labels(pdfs[doc], page, cache):
            bad.append(f"[{doc}{re.sub(chr(10), ' ', body).strip()[:70]}] printed {printed} is not on PDF p.{page}")
    for b in sorted(set(bad)):
        print("error:", b)
    print(f"checked {checked} citations, {len(set(bad))} mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
