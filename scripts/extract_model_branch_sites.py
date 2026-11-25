#!/usr/bin/env python3
"""MC68060 設計書 §3.0: model 比較・述語の全件抽出。
使い方: scripts/extract_model_branch_sites.py [out.csv] [--rev REV]
--rev を付けると作業ツリーでなく git の REV 時点の src を読む（他セッションの未コミット変更に左右されない）。"""
import csv, glob, re, subprocess, sys
from collections import Counter

PAT = re.compile(
    r'(model\s*(>=|<=|<|>|==|!=)\s*MC68K_MODEL_\w+'
    r'|MC68K_MODEL_\w+\s*(<=|<|>=|>|==|!=)\s*\w*model'
    r'|mc68k_get_model\([^)]*\)\s*(>=|<=|<|>|==|!=)\s*MC68K_MODEL_\w+'
    r'|\bcase\s+MC68K_MODEL_\w+\s*:'
    r'|\bcpu_is_680\w+\(|\bmodel_has_\w+\(|\bcpu_has_\w+\()')
DEF = re.compile(r'^\s*(static\s+)?(inline\s+)?(static\s+inline\s+)?bool\s+(cpu_is_|model_has_|cpu_has_)\w+\(.*\)\s*\{?\s*$')
FUNC = re.compile(r'^[A-Za-z_][\w\s\*]*\b(\w+)\(')

def classify(t):
    if re.search(r'(==|!=)\s*MC68K_MODEL_68040|cpu_is_68040\(', t): return '040分岐'
    if re.search(r'!=\s*MC68K_MODEL_\w+', t): return '否定比較(060は真側)'
    if re.search(r'mc68k_get_model\(|case\s+MC68K_MODEL_', t): return 'get_model/case'
    if re.search(r'(>=|<)\s*MC68K_MODEL_6(8020|8010)', t): return '数値比較'
    if re.search(r'MC68K_MODEL_6(8030|8EC030|8020|8010|8000)|cpu_is_6(8020|8030)\w*\(|cpu_is_68ec030', t): return '他モデル固有(==/述語)'
    return 'その他(述語)'

def sources(rev):
    """(path, lines) を返す。rev=None は作業ツリー。"""
    if rev is None:
        for f in sorted(glob.glob('src/**/*.c', recursive=True) + glob.glob('src/**/*.h', recursive=True)):
            yield f, open(f, encoding='utf-8', errors='replace').read().split('\n')
        return
    names = subprocess.run(['git', 'ls-tree', '-r', '--name-only', rev, 'src'], check=True,
                           capture_output=True, text=True).stdout.split('\n')
    for f in sorted(n for n in names if n.endswith(('.c', '.h'))):
        blob = subprocess.run(['git', 'show', f'{rev}:{f}'], check=True, capture_output=True).stdout
        yield f, blob.decode('utf-8', errors='replace').split('\n')

def main(out, rev):
    rows = []
    for f, lines in sources(rev):
        fn = ''
        for i, l in enumerate(lines, 1):
            m = FUNC.match(l)
            if m and not l.startswith((' ', '\t', '#')) and ';' not in l:
                fn = m.group(1)
            if not PAT.search(l) or DEF.match(l) or re.match(r'\s*(//|/\*)', l) or re.match(r'\s*\* ', l):
                continue
            t = l.strip()
            rows.append((f, i, fn, classify(t), t[:110]))
    with open(out, 'w', newline='') as fp:
        w = csv.writer(fp, lineterminator='\n'); w.writerow(['file', 'line', 'function', 'class', 'code']); w.writerows(rows)
    print(len(rows), dict(Counter(r[3] for r in rows)))

args = [a for a in sys.argv[1:]]
rev = None
if '--rev' in args:
    k = args.index('--rev')
    if k + 1 >= len(args) or args[k + 1].startswith('-'):
        print('usage: extract_model_branch_sites.py [OUT.csv] [--rev REV]', file=sys.stderr)
        sys.exit(2)
    rev = args[k + 1]; del args[k:k + 2]
if len(args) > 1 or any(a.startswith('-') for a in args):
    print('usage: extract_model_branch_sites.py [OUT.csv] [--rev REV]', file=sys.stderr)
    sys.exit(2)
main(args[0] if args else 'tests/data/cpu/mc68060_model_branch_sites.csv', rev)
