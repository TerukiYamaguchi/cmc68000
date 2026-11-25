#!/usr/bin/env python3
"""Validate the Phase 2B--2E opcode-rule CSV without priority masking."""
import argparse
import csv
import sys

# EA番号 = mode*8 + reg。mode=111/reg=101-111（61-63）はMC68000では予約で、有効EAではない
# (spec §5.1, UM §6.3.6: 合法命令の第1語に一致しない語は illegal)。
RESERVED_EA = {61, 62, 63}
ANY_VALID_EA = set(range(64)) - RESERVED_EA
DATA = ANY_VALID_EA - set(range(8, 16))
DATA_ALT = set(range(8)) | set(range(16, 58))
MEM_ALT = set(range(16, 58))
CONTROL = set(range(16, 24)) | set(range(40, 60))

REQUIRED_2B = {'MOVE_B', 'MOVE_L', 'MOVE_W', 'MOVEA_L', 'MOVEA_W', 'MOVEQ',
               'MOVEM', 'MOVEP', 'LEA', 'PEA', 'LINK', 'UNLK', 'EXG_DD',
               'EXG_AA', 'EXG_DA', 'EXT_W', 'EXT_L', 'SWAP'}
REQUIRED_2C = {'ADD_EA_DN', 'ADD_EA_DN_W', 'ADD_EA_DN_L', 'ADDA_W', 'ADDA_L',
               'ADD_DN_MEM', 'ADD_DN_MEM_W', 'ADD_DN_MEM_L', 'ADDI', 'ADDQ',
               'ADDX', 'SUB_EA_DN_B', 'SUB_EA_DN_W', 'SUB_EA_DN_L', 'SUBA_W',
               'SUBA_L', 'SUB_DN_MEM_B', 'SUB_DN_MEM_W', 'SUB_DN_MEM_L', 'SUBI',
               'SUBQ', 'SUBX', 'CMP_B', 'CMP_W', 'CMP_L', 'CMPA_W', 'CMPA_L',
               'CMPI_B', 'CMPI_W', 'CMPI_L', 'CMPM', 'MULU', 'MULS', 'DIVU', 'DIVS',
               'OR_EA_DN_B', 'OR_EA_DN_W', 'OR_EA_DN_L', 'OR_DN_MEM_B',
               'OR_DN_MEM_W', 'OR_DN_MEM_L', 'AND_EA_DN_B', 'AND_EA_DN_W',
               'AND_EA_DN_L', 'AND_DN_MEM_B', 'AND_DN_MEM_W', 'AND_DN_MEM_L',
               'EOR_B', 'EOR_W', 'EOR_L', 'ORI', 'ANDI', 'EORI', 'BTST_DYNAMIC',
               'BCHG_DYNAMIC', 'BCLR_DYNAMIC', 'BSET_DYNAMIC', 'BTST_IMMEDIATE',
               'BCHG_IMMEDIATE', 'BCLR_IMMEDIATE', 'BSET_IMMEDIATE', 'ABCD', 'SBCD',
               'NBCD', 'CLR', 'NEG', 'NEGX', 'NOT', 'TST'}
REQUIRED_2D = {'BRA', 'BSR', 'BCC', 'DBCC', 'SCC', 'JMP', 'JSR', 'RTS',
               'SHIFT_REG', 'SHIFT_MEM'}
REQUIRED_2E = {'MOVE_FROM_SR', 'MOVE_TO_CCR', 'MOVE_TO_SR', 'MOVE_AN_USP',
               'MOVE_USP_AN', 'ORI_CCR', 'ORI_SR', 'ANDI_CCR', 'ANDI_SR',
               'EORI_CCR', 'EORI_SR', 'CHK', 'ILLEGAL', 'NOP', 'RESET', 'RTE',
               'RTR', 'STOP', 'TAS', 'TRAP', 'TRAPV'}

# PMの形式・サイズ・EA制約から別途算出して固定した各形式のopcode件数。
# CSVの述語変更で合法EA/予約sizeを過不足分類した場合に検出する。
EXPECTED_COUNTS = {
    'MOVE_B':2650, 'MOVE_L':3050, 'MOVE_W':3050, 'MOVEA_L':488, 'MOVEA_W':488,
    'MOVEQ':2048, 'MOVEP':256, 'MOVEM':140, 'LEA':224, 'PEA':28, 'LINK':8, 'UNLK':8,
    'EXG_DD':64, 'EXG_AA':64, 'EXG_DA':64, 'EXT_W':8, 'EXT_L':8, 'SWAP':8,
    'ADD_EA_DN':424, 'ADD_EA_DN_W':488, 'ADD_EA_DN_L':488, 'ADDA_W':488, 'ADDA_L':488,
    'ADD_DN_MEM':336, 'ADD_DN_MEM_W':336, 'ADD_DN_MEM_L':336, 'ADDI':150, 'ADDQ':1328,
    'ADDX':384, 'SUB_EA_DN_B':424, 'SUB_EA_DN_W':488, 'SUB_EA_DN_L':488, 'SUBA_W':488,
    'SUBA_L':488, 'SUB_DN_MEM_B':336, 'SUB_DN_MEM_W':336, 'SUB_DN_MEM_L':336, 'SUBI':150,
    'SUBQ':1328, 'SUBX':384, 'CMP_B':424, 'CMP_W':488, 'CMP_L':488, 'CMPA_W':488,
    'CMPA_L':488, 'CMPI_B':50, 'CMPI_W':50, 'CMPI_L':50, 'CMPM':192, 'MULU':424,
    'MULS':424, 'DIVU':424, 'DIVS':424, 'OR_EA_DN_B':424, 'OR_EA_DN_W':424,
    'OR_EA_DN_L':424, 'OR_DN_MEM_B':336, 'OR_DN_MEM_W':336, 'OR_DN_MEM_L':336,
    'AND_EA_DN_B':424, 'AND_EA_DN_W':424, 'AND_EA_DN_L':424, 'AND_DN_MEM_B':336,
    'AND_DN_MEM_W':336, 'AND_DN_MEM_L':336, 'EOR_B':400, 'EOR_W':400, 'EOR_L':400,
    'ORI':150, 'ANDI':150, 'EORI':150, 'BTST_DYNAMIC':424, 'BCHG_DYNAMIC':400,
    'BCLR_DYNAMIC':400, 'BSET_DYNAMIC':400, 'BTST_IMMEDIATE':53, 'BCHG_IMMEDIATE':50,
    'BCLR_IMMEDIATE':50, 'BSET_IMMEDIATE':50, 'ABCD':128, 'SBCD':128, 'NBCD':50,
    'CLR':150, 'NEG':150, 'NEGX':150, 'NOT':150, 'TST':150,
    'BRA':256, 'BSR':256, 'BCC':3584, 'DBCC':128, 'SCC':800, 'JMP':28,
    'JSR':28, 'RTS':1, 'SHIFT_REG':3072, 'SHIFT_MEM':336,
    'MOVE_FROM_SR':50, 'MOVE_TO_CCR':53, 'MOVE_TO_SR':53, 'MOVE_AN_USP':8,
    'MOVE_USP_AN':8, 'ORI_CCR':1, 'ORI_SR':1, 'ANDI_CCR':1, 'ANDI_SR':1,
    'EORI_CCR':1, 'EORI_SR':1, 'CHK':424, 'ILLEGAL':1, 'NOP':1, 'RESET':1,
    'RTE':1, 'RTR':1, 'STOP':1, 'TAS':50, 'TRAP':16, 'TRAPV':1,
}

def ea_ok(name, op):
    ea = op & 0x3f
    src = ea
    dst = ((op >> 9) & 7) | (((op >> 6) & 7) << 3)
    size = (op >> 6) & 3
    if name == 'data': return ea in DATA
    if name == 'data-an': return ea in DATA or 8 <= ea < 16
    if name == 'data-alterable': return ea in DATA_ALT
    if name == 'data-alterable-size': return ea in DATA_ALT and size != 3
    if name == 'memory-alterable': return ea in MEM_ALT
    if name == 'control': return ea in CONTROL
    if name == 'dn': return ea < 8
    if name == 'move-b': return src in DATA and dst in DATA_ALT
    if name == 'move-w' or name == 'move-l': return src in ANY_VALID_EA and dst in DATA_ALT
    if name == 'movea': return ((op >> 6) & 7) == 1 and src in ANY_VALID_EA
    if name in ('add-ea-dn', 'add-ea-dn-wl'): return ea in DATA or (name != 'add-ea-dn' and 8 <= ea < 16)
    if name == 'addq':
        return size != 3 and (ea in DATA_ALT or (8 <= ea < 16 and size != 0))
    if name == 'addx': return ((op >> 6) & 3) != 3
    if name == 'cmpm': return size != 3
    if name == 'branch-bcc': return ((op >> 8) & 15) >= 2
    if name == 'shift-reg': return size != 3
    if name == 'abcd': return True
    if name == 'movem':
        direction = (op >> 10) & 1
        reg_to_mem = set(range(16, 24)) | set(range(32, 40)) | set(range(40, 56)) | {56, 57}
        mem_to_reg = set(range(16, 32)) | set(range(40, 56)) | {56, 57, 58, 59}
        return ea in (mem_to_reg if direction else reg_to_mem)
    if name in ('move', 'moveq', 'movep', 'link', 'unlk', 'exg', 'none'): return True
    raise ValueError('unknown EA predicate: ' + name)

def generated_rows(rows, hits):
    """Return the one complete MC68000 classification row for every 16-bit opcode."""
    owners = {}
    for row, hit_set in zip(rows, hits):
        for opcode in hit_set:
            owners[opcode] = row
    result = []
    for opcode in range(0x10000):
        if 0xA000 <= opcode <= 0xAFFF:
            result.append({'opcode': f'{opcode:04X}', 'classification': 'Line-A',
                           'form_id': 'LINE_A', 'phase': '2A', 'source': 'PM:Appendix_A',
                           'norm_ids': 'INS-001'})
        elif 0xF000 <= opcode <= 0xFFFF:
            result.append({'opcode': f'{opcode:04X}', 'classification': 'Line-F',
                           'form_id': 'LINE_F', 'phase': '2A', 'source': 'PM:Appendix_A',
                           'norm_ids': 'INS-001'})
        elif opcode in owners:
            row = owners[opcode]
            result.append({'opcode': f'{opcode:04X}', 'classification': row['classification'],
                           'form_id': row['form_id'], 'phase': row['phase'],
                           'source': row['source'], 'norm_ids': row['norm_ids']})
        else:
            result.append({'opcode': f'{opcode:04X}', 'classification': 'illegal encoding',
                           'form_id': 'ILLEGAL_ENCODING', 'phase': '2F',
                           'source': 'PM:Appendix_A', 'norm_ids': 'INS-052'})
    return result

CLASSIFICATION_FIELDS = ('opcode', 'classification', 'form_id', 'phase', 'source', 'norm_ids')

def write_classification(path, classification):
    with open(path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=CLASSIFICATION_FIELDS)
        writer.writeheader()
        writer.writerows(classification)

def check_classification(path, classification):
    with open(path, newline='') as f:
        actual = list(csv.DictReader(f))
    if len(actual) != 0x10000:
        print(f'classification row count: {len(actual)} != 65536')
        return False
    for expected, observed in zip(classification, actual):
        if {field: observed.get(field, '') for field in CLASSIFICATION_FIELDS} != expected:
            print('generated classification mismatch at opcode ' + expected['opcode'])
            return False
    return True

def main(path, write_path=None, check_path=None):
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f))
    required_columns = ('form_id', 'mask', 'value', 'extension', 'ea_predicate',
                        'classification', 'phase', 'source', 'norm_ids')
    if not rows or any(not row.get(column, '').strip() for row in rows for column in required_columns):
        print('invalid rule: required CSV column is empty')
        return 1
    ids = {row['form_id'] for row in rows}
    missing = (REQUIRED_2B | REQUIRED_2C | REQUIRED_2D | REQUIRED_2E) - ids
    if missing:
        print('missing forms: ' + ', '.join(sorted(missing)))
        return 1
    if ids != set(EXPECTED_COUNTS):
        print('expected-count manifest does not match rule IDs')
        return 1
    for row in rows:
        if row['classification'] != 'valid:' + row['form_id']:
            print('invalid classification: ' + row['form_id'])
            return 1
    hits = [set() for _ in rows]
    for i, row in enumerate(rows):
        mask, value = int(row['mask'], 0), int(row['value'], 0)
        hits[i] = {op for op in range(0x10000) if op & mask == value and ea_ok(row['ea_predicate'], op)}
        if not hits[i]:
            print('empty rule: ' + row['form_id'])
            return 1
        if len(hits[i]) != EXPECTED_COUNTS[row['form_id']]:
            print(f"count mismatch: {row['form_id']}: {len(hits[i])} != {EXPECTED_COUNTS[row['form_id']]}")
            return 1
        reserved = hits[i] & (set(range(0xA000, 0xB000)) | set(range(0xF000, 0x10000)))
        if reserved:
            print(f"Line-A/F override attempt: {row['form_id']}: {min(reserved):04X}")
            return 1
    failures = 0
    for i, left in enumerate(rows):
        for j in range(i + 1, len(rows)):
            both = hits[i] & hits[j]
            if both:
                print(f"overlap: {left['form_id']} / {rows[j]['form_id']}: {min(both):04X}")
                failures += 1
    if failures:
        return 1
    classification = generated_rows(rows, hits)
    if len(classification) != 0x10000:
        print('internal error: opcode closure is not 65536')
        return 1
    counts = {}
    for row in classification:
        counts[row['classification']] = counts.get(row['classification'], 0) + 1
    if counts.get('Line-A') != 4096 or counts.get('Line-F') != 4096:
        print('Line-A/F closure count mismatch')
        return 1
    if write_path:
        write_classification(write_path, classification)
    if check_path and not check_classification(check_path, classification):
        return 1
    valid_count = sum(count for name, count in counts.items() if name.startswith('valid:'))
    print(f"ok: {len(rows)} rules, required forms/counts present, no overlaps; "
          f"closure=65536 valid={valid_count} illegal={counts.get('illegal encoding', 0)} "
          f"Line-A={counts['Line-A']} Line-F={counts['Line-F']}")
    return 0

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('rules', nargs='?', default='tests/data/cpu/opcode_rules.csv')
    parser.add_argument('--write-classification')
    parser.add_argument('--check-classification')
    args = parser.parse_args()
    sys.exit(main(args.rules, args.write_classification, args.check_classification))
