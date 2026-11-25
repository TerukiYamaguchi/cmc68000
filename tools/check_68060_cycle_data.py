#!/usr/bin/env python3
"""Validate the /#102 MC68060 timing-data contract (060UM §10 raw / classification / expectations)."""
import csv
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_68040_cycle_data as base  # noqa: E402  (shares read_csv and check_implementation)

RAW_COLUMNS = (
    'table', 'pdf_page', 'printed_page', 'instruction', 'condition', 'operand_form', 'cycles',
    'per_unit', 'reads', 'writes', 'projection', 'extraction', 'note', 'source_text',
)
CLASSIFICATION_COLUMNS = (
    'table', 'instruction', 'condition', 'disposition', 'implementation', 'source', 'notes',
)
EXPECTATION_COLUMNS = (
    'id', 'table', 'instruction', 'condition', 'operand_form', 'case', 'setup', 'code', 'n',
    'expected_cycles', 'adjust', 'adjust_reason',
)
TIMED_PROJECTIONS = {'nominal', 'upper-bound'}
PROJECTIONS = TIMED_PROJECTIONS | {'not-modeled'}
DISPOSITIONS = {'inherit-040', 'override', 'unimplemented-vector-61', 'out-of-scope'}
TIMED_DISPOSITIONS = DISPOSITIONS - {'out-of-scope'}
WILDCARD = '*'
NO_IMPLEMENTATION = 'none'
TABLE_NAME = re.compile(r'^10-\d+$')
CODE_WORD = re.compile(r'^[0-9a-f]{4}$')
SETUP_TOKEN = re.compile(
    r'^(sr=[0-9a-f]+|d0=\d+|ebc|psr=[0-9a-f]+|pd0=\d+|patch=[0-9a-f]+:[0-9a-f]+|fill=[0-9a-f]{2}|'
    r'w16=[0-9a-f]+:[0-9a-f]+|irq=[1-7]|iack=\d+|pc=[0-9a-f]+|dirty=\d+(:\d+)?)$')
RAW_REQUIRED = ('table', 'pdf_page', 'printed_page', 'instruction', 'condition', 'operand_form',
                'projection', 'extraction', 'source_text')
EXPECTATION_REQUIRED = ('id', 'table', 'instruction', 'condition', 'operand_form', 'case', 'code',
                        'n', 'expected_cycles', 'adjust')
MARKERS = ('reason=', 'actual=')


def fail(path, number, message):
    raise ValueError(f'{path}:{number}: {message}')


def check_raw(rows, path):
    index = {}
    for number, row in enumerate(rows, start=2):
        for key in RAW_REQUIRED:
            if not row[key]:
                fail(path, number, f'missing {key}')
        if not TABLE_NAME.match(row['table']):
            fail(path, number, f'bad table {row["table"]!r}')
        if row['projection'] not in PROJECTIONS:
            fail(path, number, f'unknown projection {row["projection"]}')
        if row['projection'] in TIMED_PROJECTIONS:
            try:
                if int(row['cycles']) < 0 or int(row['per_unit']) < 0:
                    raise ValueError
            except ValueError:
                fail(path, number, 'cycles and per_unit must be non-negative integers')
        else:
            for marker in MARKERS:
                if marker not in row['note']:
                    fail(path, number, f'not-modeled row lacks {marker}')
        identity = (row['table'], row['instruction'], row['condition'], row['operand_form'])
        if identity in index:
            fail(path, number, f'duplicate raw identity {identity}')
        index[identity] = (number, row)
    return index


def check_classification(root, rows, path):
    index = {}
    for number, row in enumerate(rows, start=2):
        for key in CLASSIFICATION_COLUMNS:
            if not row[key]:
                fail(path, number, f'missing {key}')
        if row['disposition'] not in DISPOSITIONS:
            fail(path, number, f'unknown disposition {row["disposition"]}')
        key = (row['table'], row['instruction'], row['condition'])
        if key in index:
            fail(path, number, f'duplicate classification key {key}')
        index[key] = (number, row)
        if row['implementation'] == NO_IMPLEMENTATION:
            if row['disposition'] != 'out-of-scope':
                fail(path, number, 'only out-of-scope rows may have implementation none')
        else:
            base.check_implementation(root, row['implementation'], path, number)
        if row['disposition'] == 'out-of-scope':
            for marker in MARKERS:
                if marker not in row['notes']:
                    fail(path, number, f'out-of-scope row lacks {marker}')
    return index


def classify(raw_key, classification):
    table, instruction, condition, _ = raw_key
    for key in ((table, instruction, condition), (table, instruction, WILDCARD)):
        if key in classification:
            return key, classification[key]
    return None, None


def check_coverage(raw, raw_path, classification, classification_path):
    used = set()
    counts = {name: 0 for name in DISPOSITIONS}
    for key, (number, row) in raw.items():
        class_key, entry = classify(key, classification)
        if entry is None:
            fail(raw_path, number, f'raw row {key} reaches no classification row')
        used.add(class_key)
        disposition = entry[1]['disposition']
        counts[disposition] += 1
        modeled = row['projection'] in TIMED_PROJECTIONS
        if modeled and disposition not in TIMED_DISPOSITIONS:
            fail(raw_path, number, f'timed raw row {key} is classified {disposition}')
        if not modeled and disposition != 'out-of-scope':
            fail(raw_path, number, f'not-modeled raw row {key} is classified {disposition}')
    for key, (number, _) in classification.items():
        if key not in used:
            fail(classification_path, number, f'classification row {key} matches no raw row')
    return counts


def check_expectations(rows, path, raw, raw_path, classification):
    covered = set()
    seen = set()
    for number, row in enumerate(rows, start=2):
        for key in EXPECTATION_REQUIRED:
            if row[key] == '':
                fail(path, number, f'missing {key}')
        if row['id'] in seen:
            fail(path, number, f'duplicate id {row["id"]}')
        seen.add(row['id'])
        identity = (row['table'], row['instruction'], row['condition'], row['operand_form'])
        entry = raw.get(identity)
        if entry is None:
            fail(path, number, f'no raw row {identity}')
        raw_row = entry[1]
        if raw_row['projection'] not in TIMED_PROJECTIONS:
            fail(path, number, f'expectation for a not-modeled raw row {identity}')
        _, class_entry = classify(identity, classification)
        if class_entry[1]['disposition'] not in TIMED_DISPOSITIONS:
            fail(path, number, f'expectation for an out-of-scope row {identity}')
        try:
            expected = int(raw_row['cycles']) + int(raw_row['per_unit']) * int(row['n']) + int(row['adjust'])
            if int(row['expected_cycles']) != expected or expected <= 0:
                raise ValueError
        except ValueError:
            fail(path, number, f'expected_cycles {row["expected_cycles"]} is not raw cycles + '
                 f'per_unit * n + adjust')
        if int(row['adjust']) and not row['adjust_reason']:
            fail(path, number, 'nonzero adjust needs adjust_reason')
        instructions = row['code'].split('/')
        for text in instructions:
            words = text.split()
            if not words or not all(CODE_WORD.match(word) for word in words):
                fail(path, number, f'bad code {row["code"]!r}')
        for token in row['setup'].split():
            if not SETUP_TOKEN.match(token):
                fail(path, number, f'unknown setup token {token!r}')
        covered.add(identity)
    for identity, (number, row) in raw.items():
        if row['projection'] in TIMED_PROJECTIONS and identity not in covered:
            fail(raw_path_name(path), number, f'timed raw row {identity} has no expectation')
    return len(rows)


def raw_path_name(expectation_path):
    return expectation_path.with_name('mc68060_cycle_raw.csv')


def main(root):
    raw_path = root / 'tests/data/cpu/mc68060_cycle_raw.csv'
    classification_path = root / 'tests/data/cpu/mc68060_cycle_classification.csv'
    expectation_path = root / 'tests/data/cpu/mc68060_cycle_expectations.csv'
    raw = check_raw(base.read_csv(raw_path, RAW_COLUMNS), raw_path)
    classification = check_classification(
        root, base.read_csv(classification_path, CLASSIFICATION_COLUMNS), classification_path)
    counts = check_coverage(raw, raw_path, classification, classification_path)
    expectations = check_expectations(
        base.read_csv(expectation_path, EXPECTATION_COLUMNS), expectation_path, raw, raw_path,
        classification)
    summary = ', '.join(f'{name}={counts[name]}' for name in sorted(DISPOSITIONS))
    print(f'ok: {len(raw)} raw rows ({summary}), {len(classification)} classification rows, '
          f'{expectations} expectations')


if __name__ == '__main__':
    try:
        main(pathlib.Path(__file__).resolve().parent.parent)
    except (OSError, ValueError, csv.Error) as error:
        print(f'check_68060_cycle_data: {error}', file=sys.stderr)
        sys.exit(1)
