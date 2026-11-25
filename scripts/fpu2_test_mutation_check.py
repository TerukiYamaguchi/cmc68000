#!/usr/bin/env python3
"""FPU-2 モデル試験の試験側変異確認（Issue #64）。

tests/fpu/test_fpu_model_*.c に次の変異を機械的に1つずつ当て、その試験が落ちるか（KILLED）、
通るか（SURVIVED）を報告する。production の変異は scripts/fpu2_mutation_check.py が担当する。
  loop: `for (...; i < N; ...)` と `<=` の上限を1つ縮める（ループ・表の縮小）
  dup : 文字列だけの初期化子 `{ "a", "b", ... }` の k 番目（k>=1）を k-1 番目の複製にする（名前リストの重複）
SURVIVED は、等価な変異（縮めても同じ網羅になる、件数 assert の外の補助ループなど）か試験の穴かを
コードを読んで判定する。試験コード自体の頑健さなので、AGENTS.md の定義により多くは非 blocking。

既知の SURVIVED（2026-10-04 時点で 176 件中 6 件。いずれも試験の穴として直さない）:
  exceptions:35・frame:81 の `guard < 64`: 状態が進めば break する安全上限で、63 でも同じ網羅になる。
  protocol:158 `j < i`・166 `j < nrows`・181 `i < nrows`: 一意性・整合の照合ループ。現行 CSV に
    重複や不整合が無いので1つ縮めても結果が変わらない（CSV を毒入れした場合だけ差が出る）。
  restore:84 `i < fr.words`: donor frame の最後の word を読み落とす。donor は s16 だけで、最後の word は
    68881（45 word）・68882（53 word）とも 0（実測）。memset の 0 と同じなので結果が変わらない。

使い方（リポジトリのルートで。build/ は configure 済みであること）:
  python3 scripts/fpu2_test_mutation_check.py --list           変異の一覧（ビルドしない）
  python3 scripts/fpu2_test_mutation_check.py                  全件
  python3 scripts/fpu2_test_mutation_check.py -f dialog 5 9    ファイル名の部分一致と番号で絞る

注意: 実行中は試験ソースを一時的に書き換える。終了時（異常終了を含む）に元へ戻す。他の編集をしない。
"""
import re
import subprocess
import sys
import time
import os
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parent.parent
TEST_GLOB = "tests/fpu/test_fpu_model_*.c"
TEST_TIMEOUT_SEC = 60
FOR_RE = re.compile(r"for\s*\([^;{}]*;\s*(\w+)\s*(<=|<)\s*([^;{}]+?)\s*;")
STRLIST_RE = re.compile(r'\{\s*("[^"\n]*"(?:\s*,\s*"[^"\n]*")+)\s*,?\s*\}')
ELEM_RE = re.compile(r'"[^"\n]*"')


def sh(cmd):
    return subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True)


def mutants(path):
    """(種別, 行, 説明, 変異後の全文) を列挙する。"""
    src = path.read_text(encoding="utf-8")
    out = []
    for m in FOR_RE.finditer(src):
        var, op, bound = m.groups()
        lo, hi = m.span(3)
        if op == "<=":
            new = src[:m.span(2)[0]] + "<" + src[m.span(2)[1]:]
        else:
            new = src[:lo] + f"(({bound}) > 0 ? ({bound}) - 1 : 0)" + src[hi:]
        line = src.count("\n", 0, m.start()) + 1
        out.append(("loop", line, f"{var} {op} {bound}", new))
    for m in STRLIST_RE.finditer(src):
        base = m.start(1)
        elems = [(base + e.start(), base + e.end(), e.group()) for e in ELEM_RE.finditer(m.group(1))]
        line = src.count("\n", 0, m.start()) + 1
        for k in range(1, len(elems)):
            if elems[k][2] == elems[k - 1][2]:
                continue  # 既に同じ（変異にならない）
            s, e, _ = elems[k]
            new = src[:s] + elems[k - 1][2] + src[e:]
            out.append(("dup", line, f"[{k}] {elems[k][2]} -> {elems[k - 1][2]}", new))
    return out


def all_mutants(name_filter):
    res = []
    for p in sorted(ROOT.glob(TEST_GLOB)):
        if name_filter and name_filter not in p.name:
            continue
        for mu in mutants(p):
            res.append((p, *mu))
    return res


def main():
    args = sys.argv[1:]
    name_filter = None
    if "-f" in args:
        i = args.index("-f")
        name_filter = args[i + 1]
        del args[i:i + 2]
    muts = all_mutants(name_filter)
    if "--list" in args:
        for n, (p, kind, line, desc, _) in enumerate(muts):
            print(n, p.name, line, kind, desc)
        print(len(muts), "件")
        return 0
    if not (ROOT / "build").exists():
        print("build/ が無い。cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug を先に実行する")
        return 2
    sel = {int(a) for a in args}
    counts = {"KILLED": 0, "SURVIVED": 0, "BUILDFAIL": 0}
    try:
        for n, (p, kind, line, desc, new) in enumerate(muts):
            if sel and n not in sel:
                continue
            target = p.stem
            orig = p.read_text(encoding="utf-8")
            try:
                p.write_text(new, encoding="utf-8")
                time.sleep(1.1)
                os.utime(p)  # macOS の make は mtime が1秒粒度
                b = sh(f"cmake --build build --target {target} 2>&1 | grep -E ' error|Error '")
                if b.stdout.strip():
                    status = "BUILDFAIL"
                else:
                    t = sh(f"ctest --test-dir build --timeout {TEST_TIMEOUT_SEC} -R '^{target}$' 2>&1")
                    status = "KILLED" if t.returncode != 0 else "SURVIVED"
            finally:
                p.write_text(orig, encoding="utf-8")
                time.sleep(1.1)
                os.utime(p)
            counts[status] += 1
            print(n, status, p.name, line, kind, desc, flush=True)
    finally:
        sh("cmake --build build -j8 >/dev/null 2>&1")
    print("TOTAL", sum(counts.values()), counts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
