#!/usr/bin/env python3
"""Validate the UM20 §8.2 to UM30 §11.6 semantic-row timing audit.

The CSV is deliberately a primary-data artifact: a row is one comparison unit,
not one g_cycles cell.  Every populated row must cite both manual locations and
state the R1 prediction (UM20 total minus UM20 r/w accesses).
"""
import csv
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATH = ROOT / "tests" / "data" / "cpu" / "um30_11_6_r1_classification.csv"
TABLES = [f"11.6.{n}" for n in range(1, 19)]
RAW_ORD_COUNT = {1: 41, 2: 75, 3: 38, 4: 74, 5: 35, 6: 42, 7: 22,
                 8: 39, 9: 17, 10: 13, 11: 16, 12: 16, 13: 16,
                 14: 24, 15: 7, 16: 28, 17: 19, 18: 8}
CLASSES = {"inherit", "r1_match", "r1_mismatch", "um20_only", "um30_only"}


def integer(row, key):
    try:
        return int(row[key])
    except ValueError as e:
        raise AssertionError(f"{row['table']}/{row['semantic_id']}: {key} is not an integer") from e


def main():
    partial = "--partial" in sys.argv[1:]
    with PATH.open(newline="") as f:
        rows = list(csv.DictReader(f))
    assert rows, "classification CSV has no semantic rows"
    seen = set()
    tables = {t: 0 for t in TABLES}
    ords = {t: set() for t in TABLES}
    for row in rows:
        key = (row["table"], row["semantic_id"])
        assert row["table"] in tables, f"unknown table: {row['table']}"
        assert key not in seen, f"duplicate semantic row: {key}"
        seen.add(key)
        tables[row["table"]] += 1
        if " ord" in row["um30_ref"]:
            ordinal = int(row["um30_ref"].rsplit(" ord", 1)[1])
            assert ordinal not in ords[row["table"]], f"{row['table']}: duplicate UM30 ordinal {ordinal}"
            ords[row["table"]].add(ordinal)
        assert row["class"] in CLASSES, f"{key}: invalid class"
        if row["class"] in {"um20_only", "um30_only"}:
            assert row["note"], f"{key}: one-sided row needs an availability note"
            continue
        assert row["um20_ref"].startswith("UM20 §8.2.")
        assert row["um30_ref"].startswith("UM30 §11.6.")
        um20 = integer(row, "um20_value")
        um30 = integer(row, "um30_value")
        r1 = integer(row, "r1_predicted")
        delta = integer(row, "delta")
        assert r1 == um20 - integer(row, "um20_r") - integer(row, "um20_w"), f"{key}: R1 formula"
        assert delta == um30 - r1, f"{key}: delta"
        inherited = (um20, integer(row, "um20_r"), integer(row, "um20_p"), integer(row, "um20_w")) == (um30, integer(row, "um30_r"), integer(row, "um30_p"), integer(row, "um30_w"))
        if row["class"] == "inherit":
            assert inherited, f"{key}: inherit mismatch"
        elif row["class"] == "r1_match":
            assert um30 == r1 and delta == 0 and not inherited, f"{key}: R1 match mismatch"
        else:
            assert um30 != r1 and row["note"], f"{key}: mismatch needs rationale"
    missing = [t for t, n in tables.items() if not n]
    if not partial:
        assert not missing, f"tables without semantic rows: {', '.join(missing)}"
    for table in TABLES:
        if tables[table]:
            expected = set(range(1, RAW_ORD_COUNT[int(table.rsplit('.', 1)[1])] + 1))
            assert ords[table] == expected, f"{table}: raw ordinal coverage is incomplete"
    counts = {c: sum(row["class"] == c for row in rows) for c in CLASSES}
    print(f"rows={len(rows)} " + " ".join(f"{c}={counts[c]}" for c in sorted(counts)))
    for table in TABLES:
        values = {c: sum(row["table"] == table and row["class"] == c for row in rows) for c in CLASSES}
        print(table + " " + " ".join(f"{c}={values[c]}" for c in sorted(values)))


if __name__ == "__main__":
    main()
