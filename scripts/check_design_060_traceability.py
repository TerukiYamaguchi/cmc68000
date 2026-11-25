#!/usr/bin/env python3
"""MC68060 設計書の追跡性検査（§3.0 の方針表 ↔ CSV ↔ §10.1 試験一覧 ↔ §10 の完了条件）。
検査: (0) CSV 再生成との一致（作業ツリーと HEAD の差は警告）、(1) 関数の網羅、(1b) 列数、(1c) 変異表、(1d) §7.1/§8.1/§8.2/§2/§3.1 の列数・行数・試験（§8.2 は全セルが ○/×、§7.1 は §8.2.5 の 18 項目の全件）、(2)〜(4) ID の定義と Issue 範囲、(2c) §10.1 の ID の重複と形式。
限界: ID が正しい行に割り当てられているか（行と試験の内容の対応）は検査しない。ID の欠落・未定義・Issue 範囲外だけを検出する。"""
import csv, re, sys

DOC = 'docs/mc68000-internals/reference/cpu_68060.md'
CSV = 'tests/data/cpu/mc68060_model_branch_sites.csv'
ID = re.compile(r'T-[A-G]\d+')
errors = []

doc = open(DOC, encoding='utf-8').read()

def need(text, key):
    if key not in text:
        print(f'ERROR: 見出し {key!r} が見つからない'); sys.exit(2)
    return text.index(key)

s30 = doc[need(doc, '### 3.0 '):need(doc, '### 3.1 ')]
s10 = doc[need(doc, '## 10. '):need(doc, '## 11. ')]
s101 = s10[need(s10, '### 10.1'):]
s10_main = s10[:s10.index('### 10.1')]

# (0) CSV が現行の src と一致する（抽出を再実行して差分を取る）
import subprocess, tempfile, os
with tempfile.TemporaryDirectory() as td:
    out = os.path.join(td, 'x.csv')
    subprocess.run([sys.executable, 'scripts/extract_model_branch_sites.py', out, '--rev', 'HEAD'], check=True, stdout=subprocess.DEVNULL)
    key = lambda path: sorted((r['file'], r['function'], r['class'], r['code']) for r in csv.DictReader(open(path)))
    # 行番号は他セッションの変更で動くため比較しない（内容の増減だけを検出する）
    # 作業ツリー（他セッションの未コミット変更を含む）との差は、HEAD に載ったとき検査が落ちる予告として警告する
    wt = os.path.join(td, 'wt.csv')
    subprocess.run([sys.executable, 'scripts/extract_model_branch_sites.py', wt], check=True, stdout=subprocess.DEVNULL)
    if key(wt) != key(out):
        added = set(key(wt)) - set(key(out))
        print(f'WARNING: 作業ツリーの src は HEAD と分岐箇所が {len(added)} 行違う（コミットされると CSV 再生成と §3.0 の行追加が要る）')
        for r in sorted(added)[:10]:
            print('  +', r[0], r[1], r[2])
    if key(out) != key(CSV):
        errors.append('CSV が現行の src と一致しない（scripts/extract_model_branch_sites.py を再実行して更新し、増えた行を §3.0 で扱う）')

# (1) CSV の「他モデル固有」以外の全関数が §3.0 の表の「箇所」列（第 2 列）にある
rows30 = [l.split('|') for l in s30.split('\n')
          if l.startswith('|') and not l.startswith('|---') and not l.startswith('| 区分')]
places = '\n'.join(c[2] for c in rows30)
funcs = {r['function'] for r in csv.DictReader(open(CSV)) if not r['class'].startswith('他モデル固有')}
for f in sorted(funcs):
    if not re.search(r'(?<![A-Za-z0-9_])' + re.escape(f) + r'(?![A-Za-z0-9_])', places):
        errors.append(f'§3.0 の箇所列に関数 {f} がない')

# (1b) §3.0 の表の各行は 5 列（セルに | を含めない）
for c in rows30:
    if len(c) != 7:
        errors.append(f'§3.0 の行の列数が {len(c) - 2}（5 でない）: ' + c[1].strip())

# (1c) §10.1 の変異表: 区分の集合が §3.0 と一致し、ID が §3.0 の同じ行の試験列に含まれる
if '代表的な変異' not in s101:
    print('ERROR: §10.1 に「代表的な変異」の表がない'); sys.exit(2)
mut = s101[s101.index('代表的な変異'):]
mrows = [l.split('|') for l in mut.split('\n')
         if l.startswith('|') and not l.startswith('|---') and not l.startswith('| 本書 §3.0')]
from collections import Counter
for name, tbl in (('§3.0', rows30), ('変異表', mrows)):
    for k, n in Counter(c[1].strip() for c in tbl).items():
        if n > 1:
            errors.append(f'{name} で区分が重複: {k}')
sec = {c[1].strip(): set(ID.findall(c[-2])) for c in rows30}
mk = {c[1].strip(): set(ID.findall(c[-2])) for c in mrows}
for k in sorted(set(sec) ^ set(mk)):
    errors.append(f'変異表と §3.0 の区分が一致しない: {k}')
for k in sorted(set(sec) & set(mk)):
    if mk[k] != sec[k]:
        errors.append(f'変異表の ID が §3.0 の試験列と一致しない: {k} {sorted(mk[k])} vs {sorted(sec[k])}')

# (1d) §8.1 / §2 / §3.1 の各行に試験 ID がある（表を参照する試験が全欄を固定する対応）
def section(start, end):
    if start not in doc or end not in doc:
        print(f'ERROR: 節 {start!r} または {end!r} が見つからない'); sys.exit(2)
    i = doc.index(start)
    return doc[i:doc.index(end, i)]

def table_rows(sec, header_first, ncols):
    out = []
    for l in sec.split('\n'):
        if not l.startswith('|') or l.startswith('|---'):
            continue
        c = l.split('|')
        if c[1].strip() == header_first:
            if len(c) != ncols + 2:
                errors.append(f'見出し行の列数が {len(c) - 2}（{ncols} でない）: {header_first}')
            continue
        out.append(c)
    return out

for name, start, end, head, ncols, expect in (
        ('§7.1', '### 7.1 ', '## 8. ', '命令', 3, 14),
        ('§8.1', '### 8.1 ', '### 8.2 ', '例外', 9, 4),
        ('§8.2', '### 8.2 ', '## 9. ', 'CPU model', 9, 7),
        ('§2', '## 2. ', '## 3. ', '分類', 5, 25),
        ('§3.1', '### 3.1 ', '## 4. ', 'Cr', 5, 9)):
    rows = table_rows(section(start, end), head, ncols)
    if len(rows) != expect:
        errors.append(f'{name} の行数が {len(rows)}（{expect} でない）')
    for c in rows:
        if len(c) != ncols + 2:
            errors.append(f'{name} の行の列数が {len(c) - 2}（{ncols} でない）: ' + c[1].strip())
        elif not ID.search(c[-2]):
            errors.append(f'{name} の行に試験 ID なし: ' + c[1].strip())

# §7.1 は §8.2.5 の特権命令 18 項目の全件を、表の行（第 1 列）で含む（節全体の部分文字列では 'STOP' が 'LPSTOP' に当たる）
rows71 = [c[1].strip() for c in table_rows(section('### 7.1 ', '## 8. '), '命令', 3)]
def cell_names(cell):
    return {x.strip() for x in re.split(r'/|（[^）]*）', cell) if x.strip()}
have = set()
for cell in rows71:
    have |= cell_names(cell)
for name in ('ANDI to SR', 'ORI to SR', 'EORI to SR', 'MOVE from SR', 'MOVE to SR', 'MOVE USP', 'MOVEC', 'MOVES',
             'RESET', 'RTE', 'STOP', 'LPSTOP', 'PFLUSH', 'PLPAR', 'PLPAW', 'CINV', 'CPUSH', 'FSAVE', 'FRESTORE'):
    if name not in have:
        errors.append(f'§7.1 の行見出しに特権命令 {name} がない')
# §8.2 の行見出しは 7 機種で重複なし。全セルが ○ か × で、NONE 列は全機種 ○（汎用 CPU）、NONE_060 列は段階 1 で全機種 ×
rows82 = table_rows(section('### 8.2 ', '## 9. '), 'CPU model', 9)
names82 = [c[1].strip() for c in rows82]
if sorted(names82) != sorted(['68000', '68010', '68020', '68EC030', '68030', '68040', '68060']):
    errors.append('§8.2 の行見出しが 7 機種の集合でない: ' + ','.join(names82))
for c in rows82:
    cells = [x.strip() for x in c[2:9]]
    if any(x not in ('○', '×') for x in cells):
        errors.append('§8.2 のセルが ○/× でない: ' + c[1].strip())
    elif cells[0] != '○' or cells[6] != '×':
        errors.append('§8.2 の NONE 列は ○、NONE_060 列は ×（段階 1）: ' + c[1].strip())

# (2) §3.0 の全行に試験 ID
for c in rows30:
    if not ID.search(c[-2]):
        errors.append('試験 ID なし: ' + c[1].strip())

# (2b) §10.1 の試験表の各行は 3 列（ID・内容・Issue）。セルに | を含めない
for l in s101.split('\n'):
    if re.match(r'\|\s*T-[A-G]\d+\s*\|', l) and len(l.split('|')) != 5:
        errors.append('§10.1 の試験行の列数が 3 でない: ' + l[:40])

# (2c) §10.1 の試験行: ID の形式（T-[A-G]数字）と重複
seen = Counter()
for l in s101.split('\n'):
    m = re.match(r'\|\s*(T-\S+)\s*\|', l)
    if m:
        if not re.fullmatch(r'T-[A-G]\d+', m.group(1)):
            errors.append('§10.1 の ID の形式が不正: ' + m.group(1))
        seen[m.group(1)] += 1
for k, n in seen.items():
    if n > 1:
        errors.append(f'§10.1 で ID が重複: {k}')

# (3) 参照された全試験 ID が §10.1 にある
defined = {}
for line in s101.split('\n'):
    m = re.match(r'\|\s*(T-[A-G]\d+)\s*\|.*\|\s*(060-[A-G])\s*\|\s*$', line)
    if m:
        defined[m.group(1)] = m.group(2)
for t in sorted(set(ID.findall(doc))):
    if t not in defined:
        errors.append(f'{t} が §10.1 にない')

# (4) §10.1 の全 ID が所属 Issue の完了条件セルの範囲にある
cells = {}
for line in s10_main.split('\n'):
    m = re.match(r'\|\s*(060-[A-G])\s*\|.*\|\s*(.*?)\s*\|\s*$', line)
    if m:
        cells[m.group(1)] = m.group(2)
def expand(cell):
    out = set()
    for a, b in re.findall(r'(T-[A-G]\d+)〜(T-[A-G]\d+)', cell):
        k = a[2]; out |= {f'T-{k}{n}' for n in range(int(a[3:]), int(b[3:]) + 1)}
    out |= set(ID.findall(cell))
    return out
for t, iss in sorted(defined.items()):
    if t not in expand(cells.get(iss, '')):
        errors.append(f'{t} が {iss} の完了条件にない')
for iss, cell in cells.items():
    for t in expand(cell):
        if defined.get(t) != iss:
            errors.append(f'{iss} の完了条件の {t} は §10.1 で別 Issue/未定義')

print('functions checked:', len(funcs), 'tests defined:', len(defined))
print('\n'.join(errors) if errors else 'OK')
sys.exit(1 if errors else 0)
