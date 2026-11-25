#!/usr/bin/env python3
"""Validate the /#91 MC68040 timing-data contract."""
import csv
import pathlib
import re
import sys


RAW_COLUMNS = (
    'table', 'pdf_page', 'printed_page', 'source_line', 'instruction', 'condition',
    'operand_form', 'ea_calculate', 'execute_lead', 'execute_base',
    'ea_fetch_accesses', 'ea_send_accesses', 'projection', 'extraction',
    'manual_correction', 'source_text',
)
CLASSIFICATION_COLUMNS = (
    'instruction', 'condition', 'projection', 'nominal_nonoverlap', 'source',
    'implementation', 'previous_040_path', 'status', 'notes',
)
EXPECTATION_COLUMNS = (
    'instruction', 'condition', 'operand_form', 'expected_cycles', 'source',
    'implementation', 'status',
)
MAP_COLUMNS = (
    'table', 'instruction', 'condition', 'class_instruction', 'class_condition', 'role',
    'class_minus_raw', 'reason',
)
MAP_ROLES = {'representative', 'mapped', 'composed'}
RAW_SUM_COLUMNS = ('ea_calculate', 'execute_base', 'ea_fetch_accesses')
INHERIT_MARKER = 'inherit='
INHERITING_PATH = 'CYC_PROF_68030'
# Adopted rows must not carry costs that  separates from the nominal value.
FORBIDDEN_NOTE_TERMS = re.compile(
    r'cache[- ]miss|copyback|MMU walk|ATC walk|table[- ]search|bus wait|wait state|FPU', re.I)
SRC_DIR = 'src'
IMPLEMENTATION_SEPARATOR = ';'

PROJECTIONS = {'metadata', 'nominal_nonoverlap', 'not-projectable', 'separate-dynamic'}


def read_csv(path, columns):
    with path.open(newline='') as source:
        reader = csv.DictReader(source)
        if tuple(reader.fieldnames or ()) != columns:
            raise ValueError(f'{path}: unexpected columns')
        rows = list(reader)
    if not rows:
        raise ValueError(f'{path}: no data rows')
    return rows


def require(row, key, path, number):
    if not row[key]:
        raise ValueError(f'{path}:{number}: missing {key}')


def raw_sum(row):
    return sum(int(row[key] or 0) for key in RAW_SUM_COLUMNS)


def check_implementation(root, implementation, path, number):
    for target in implementation.split(IMPLEMENTATION_SEPARATOR):
        source_name, separator, functions = target.strip().partition(':')
        if not separator or not functions:
            raise ValueError(f'{path}:{number}: implementation {target!r} is not file:function')
        sources = list((root / SRC_DIR).rglob(source_name))
        if not sources:
            raise ValueError(f'{path}:{number}: {source_name} not found under {SRC_DIR}/')
        text = ''.join(source.read_text() for source in sources)
        for function in functions.split('/'):
            if not re.search(rf'\b{re.escape(function)}\b', text):
                raise ValueError(f'{path}:{number}: {function} not found in {source_name}')


def check_raw_coverage(raw, raw_path, classification_by_key, mapping, map_path):
    """Every raw timing row must reach a classification row, or be explicitly composed."""
    mapped = {}
    for number, row in enumerate(mapping, start=2):
        if row['role'] not in MAP_ROLES:
            raise ValueError(f'{map_path}:{number}: unknown role {row["role"]}')
        try:
            delta = int(row['class_minus_raw'])
        except ValueError:
            raise ValueError(f'{map_path}:{number}: invalid class_minus_raw') from None
        if delta and not row['reason']:
            raise ValueError(f'{map_path}:{number}: nonzero class_minus_raw needs a reason')
        if row['role'] == 'composed':
            if not row['reason']:
                raise ValueError(f'{map_path}:{number}: composed row needs a reason')
        else:
            target = (row['class_instruction'], row['class_condition'])
            if target not in classification_by_key:
                raise ValueError(f'{map_path}:{number}: no classification row {target}')
        identity = (row['table'], row['instruction'], row['condition'])
        if identity in mapped:
            raise ValueError(f'{map_path}:{number}: duplicate map row {identity}')
        mapped[identity] = (number, row, delta)

    used = set()
    for number, row in enumerate(raw, start=2):
        if row['projection'] == 'metadata':
            continue
        identity = (row['table'], row['instruction'], row['condition'])
        exact = (row['instruction'], row['condition'])
        entry = mapped.get(identity)
        if entry is not None:
            used.add(identity)
            _, map_row, delta = entry
            target_key = exact if map_row['role'] == 'composed' else (
                map_row['class_instruction'], map_row['class_condition'])
            role = map_row['role']
        elif exact in classification_by_key:
            target_key, role, delta = exact, 'representative', 0
        else:
            raise ValueError(f'{raw_path}:{number}: raw row {identity} reaches no classification row')
        if role == 'composed':
            continue
        target = classification_by_key[target_key][1]
        if row['projection'] != 'nominal_nonoverlap':
            if target['status'] != 'out-of-scope' and target['projection'] == 'nominal_nonoverlap':
                raise ValueError(f'{raw_path}:{number}: {row["projection"]} raw row adopted as nominal')
            continue
        if role == 'representative' and target['nominal_nonoverlap']:
            expected = raw_sum(row) + delta
            if int(target['nominal_nonoverlap']) != expected:
                raise ValueError(
                    f'{raw_path}:{number}: {identity} raw sum {raw_sum(row)} + {delta} '
                    f'contradicts classification {target["nominal_nonoverlap"]}')
    unused = set(mapped) - used
    if unused:
        raise ValueError(f'{map_path}: map rows match no raw row: {sorted(unused)[:3]}')


def check_adopted_rows(classification, classification_path):
    for number, row in enumerate(classification, start=2):
        if row['status'] != 'implemented':
            continue
        if row['previous_040_path'] == INHERITING_PATH and INHERIT_MARKER not in row['notes']:
            raise ValueError(f'{classification_path}:{number}: {INHERITING_PATH} path lacks {INHERIT_MARKER} rationale')
        match = FORBIDDEN_NOTE_TERMS.search(row['notes'])
        if match:
            raise ValueError(f'{classification_path}:{number}: adopted row mentions {match.group(0)!r}')


def main(root):
    raw_path = root / 'tests/data/cpu/mc68040_cycle_raw.csv'
    classification_path = root / 'tests/data/cpu/mc68040_cycle_classification.csv'
    expectation_path = root / 'tests/data/cpu/mc68040_cycle_expectations.csv'
    raw = read_csv(raw_path, RAW_COLUMNS)
    classification = read_csv(classification_path, CLASSIFICATION_COLUMNS)
    expectations = read_csv(expectation_path, EXPECTATION_COLUMNS)
    map_path = root / 'tests/data/cpu/mc68040_cycle_raw_map.csv'
    mapping = read_csv(map_path, MAP_COLUMNS)

    raw_identities = set()
    for number, row in enumerate(raw, start=2):
        for key in ('table', 'pdf_page', 'printed_page', 'source_line', 'instruction',
                    'condition', 'operand_form', 'projection', 'extraction', 'source_text'):
            require(row, key, raw_path, number)
        if row['projection'] not in PROJECTIONS:
            raise ValueError(f'{raw_path}:{number}: unknown projection {row["projection"]}')
        identity = (row['table'], row['instruction'], row['condition'], row['operand_form'])
        if identity in raw_identities:
            raise ValueError(f'{raw_path}:{number}: duplicate raw identity {identity}')
        raw_identities.add(identity)

    implemented = set()
    classification_by_key = {}
    for number, row in enumerate(classification, start=2):
        for key in ('instruction', 'condition', 'projection', 'source', 'implementation',
                    'previous_040_path', 'status', 'notes'):
            require(row, key, classification_path, number)
        if row['projection'] not in PROJECTIONS:
            raise ValueError(f'{classification_path}:{number}: unknown projection')
        key = (row['instruction'], row['condition'])
        if key in classification_by_key:
            raise ValueError(f'{classification_path}:{number}: duplicate classification key {key}')
        classification_by_key[key] = (number, row)
        check_implementation(root, row['implementation'], classification_path, number)
        if row['status'] not in {'implemented', 'out-of-scope'}:
            raise ValueError(f'{classification_path}:{number}: status must be implemented or out-of-scope')
        if row['status'] == 'implemented' and not row['nominal_nonoverlap']:
            raise ValueError(f'{classification_path}:{number}: implemented row lacks nominal value')
        if row['projection'] == 'separate-dynamic' and row['nominal_nonoverlap']:
            raise ValueError(f'{classification_path}:{number}: dynamic row has a nominal value')
        if row['status'] == 'out-of-scope':
            for marker in ('reason=', 'actual='):
                if marker not in row['notes']:
                    raise ValueError(f'{classification_path}:{number}: out-of-scope row lacks {marker}')
        if row['status'] == 'implemented':
            implemented.add((row['instruction'], row['condition'], row['implementation']))

    for number, row in enumerate(expectations, start=2):
        for key in EXPECTATION_COLUMNS:
            require(row, key, expectation_path, number)
        try:
            if int(row['expected_cycles']) <= 0:
                raise ValueError
        except ValueError:
            raise ValueError(f'{expectation_path}:{number}: invalid expected_cycles') from None
        identity = (row['instruction'], row['condition'], row['implementation'])
        if identity not in implemented:
            raise ValueError(f'{expectation_path}:{number}: no implemented classification {identity}')

    check_raw_coverage(raw, raw_path, classification_by_key, mapping, map_path)
    check_adopted_rows(classification, classification_path)

    print(f'ok: {len(raw)} raw rows, {len(classification)} classification rows, '
          f'{len(expectations)} expectations, {len(mapping)} map rows')


if __name__ == '__main__':
    try:
        main(pathlib.Path(__file__).resolve().parent.parent)
    except (OSError, ValueError, csv.Error) as error:
        print(f'check_68040_cycle_data: {error}', file=sys.stderr)
        sys.exit(1)
