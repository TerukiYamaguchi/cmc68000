#!/usr/bin/env python3
"""Reject unreviewed CYC_INHERIT entries in the model timing table."""
import argparse
import subprocess
import sys

# Every inherited raw entry needs a source-specific justification.  Phase 3
# replaces the 68030 entries table-by-table; add a pair here only together
# with its UM30/UM20 classification and oracle comment.
ALLOWED = {
    ('CYC_PROF_68010', 'CYC_DBCC_BRANCH'),
    ('CYC_PROF_68030', 'CYC_DBCC_BRANCH'),
    ('CYC_PROF_68030', 'CYC_DBCC_EXPIRED'),
    ('CYC_PROF_68030', 'CYC_DBCC_CC_TRUE'),
}


def raw_entries(executable):
    result = subprocess.run([executable, '--dump-raw-entries'], check=False,
                            capture_output=True, text=True)
    if result.returncode:
        sys.stderr.write(result.stderr)
        raise RuntimeError('could not dump raw cycle entries')
    entries = []
    for line in result.stdout.splitlines():
        fields = line.split(',')
        if len(fields) != 3:
            raise RuntimeError('invalid raw-entry output: ' + line)
        profile, cls, value = fields
        entries.append((profile, cls, int(value)))
    return entries


def main(executable):
    inherited = {(profile, cls) for profile, cls, value in raw_entries(executable) if value == -1}
    unexpected = inherited - ALLOWED
    missing = ALLOWED - inherited
    if unexpected:
        print('unapproved CYC_INHERIT: ' + ', '.join('/'.join(pair) for pair in sorted(unexpected)))
    if missing:
        print('approved CYC_INHERIT missing: ' + ', '.join('/'.join(pair) for pair in sorted(missing)))
    if unexpected or missing:
        return 1
    print(f'ok: {len(inherited)} reviewed CYC_INHERIT entries')
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('cycle_table_executable')
    args = parser.parse_args()
    try:
        sys.exit(main(args.cycle_table_executable))
    except (OSError, RuntimeError, ValueError) as error:
        print('check_cycle_inherit:', error, file=sys.stderr)
        sys.exit(2)
