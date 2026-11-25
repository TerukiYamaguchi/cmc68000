#!/usr/bin/env python3
"""FPU-2 モデルの変異確認。

scripts/fpu2_mutations.py の変異を1種ずつソースへ当て、再ビルドして `ctest -R fpu_model` が落ちるか
（KILLED）、通るか（SURVIVED）を報告する。SURVIVED は、等価な変異か試験の穴かをコードを読んで判定する
（AGENTS.md の Review workflow）。

使い方（リポジトリのルートで。build/ は configure 済みであること）:
  python3 scripts/fpu2_mutation_check.py                 全件
  python3 scripts/fpu2_mutation_check.py 3 17 40         表の番号を指定
  python3 scripts/fpu2_mutation_check.py --check-patterns  ビルドせず、変異前の文字列が1回だけ現れるかを検査
  python3 scripts/fpu2_mutation_check.py --list          番号と名前の一覧

注意:
  - 実行中は作業ツリーのソースを一時的に書き換える。終了時（異常終了を含む）に元へ戻し、最後に
    クリーンなビルドをやり直す。実行中は他の編集やコミットをしない。
  - 変異の前後で mtime を進めて再ビルドする。進めないと、同じ秒に書き戻したソースの古い `.o` を使い、
    検出結果が無効になる（A-N135）。
  - 全件で1時間前後かかる。
"""
import os
import subprocess
import sys

sys.dont_write_bytecode = True  # scripts/__pycache__ を作らない
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fpu2_mutations import MUTATIONS  # noqa: E402

TEST_FILTER = "fpu_model"
FAIL_MARKS = ("***Failed", "***Exception", "***Timeout")


def sh(cmd):
    return subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True)


def touch_later(path):
    time.sleep(1.1)  # mtime を確実に進める（同じ秒の .o を再利用させない）
    os.utime(path)


def check_patterns():
    bad = 0
    for i, (name, rel, old, _new) in enumerate(MUTATIONS):
        n = (ROOT / rel).read_text(encoding="utf-8").count(old)
        if n != 1:
            print(f"{i} SKIP {name} count={n}")
            bad += 1
    print(f"{len(MUTATIONS)} 種中、変異前の文字列が1回でない行 {bad} 種")
    return 1 if bad else 0


def run_one(i, name, rel, old, new, counts):
    path = ROOT / rel
    orig = path.read_text(encoding="utf-8")
    n = orig.count(old)
    if n != 1:
        print(i, "SKIP", name, f"count={n}", flush=True)
        counts["SKIP"] += 1
        return
    try:
        path.write_text(orig.replace(old, new), encoding="utf-8")
        touch_later(path)
        b = sh("cmake --build build -j8 2>&1 | grep -E ' error|Error '")
        if b.stdout.strip():
            print(i, "BUILDFAIL", name, flush=True)
            counts["BUILDFAIL"] += 1
            return
        t = sh(f"ctest --test-dir build -j8 -R '{TEST_FILTER}' 2>&1")
        failed = [ln.split()[3] for ln in t.stdout.splitlines()
                  if any(m in ln for m in FAIL_MARKS) and len(ln.split()) > 3]
        if t.returncode != 0 or failed:
            counts["KILLED"] += 1
            print(i, "KILLED", name, "<-", " ".join(failed), flush=True)
        else:
            counts["SURVIVED"] += 1
            print(i, "SURVIVED", name, flush=True)
    finally:
        path.write_text(orig, encoding="utf-8")
        touch_later(path)


def main():
    args = sys.argv[1:]
    if "--list" in args:
        for i, m in enumerate(MUTATIONS):
            print(i, m[0], m[1])
        return 0
    if "--check-patterns" in args:
        return check_patterns()
    if not (ROOT / "build").exists():
        print("build/ が無い。cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug を先に実行する")
        return 2
    sel = {int(a) for a in args}
    counts = {"KILLED": 0, "SURVIVED": 0, "SKIP": 0, "BUILDFAIL": 0}
    try:
        for i, (name, rel, old, new) in enumerate(MUTATIONS):
            if sel and i not in sel:
                continue
            run_one(i, name, rel, old, new, counts)
    finally:
        sh("cmake --build build -j8 >/dev/null 2>&1")
    print("TOTAL", sum(counts.values()), counts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
