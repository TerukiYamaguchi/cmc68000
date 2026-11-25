#!/usr/bin/env python3
"""tests/data/fpu/fpu2_take_transition.csv（設計書 §4.2 の表 A の全セル×機種の期待値）を生成する。

  gen_fpu2_take_transition.py            CSV を（再）生成する
  gen_fpu2_take_transition.py --check    CSV が最新で、表 A と矛盾しないことを検査する

CSV は tests/fpu/test_fpu_model_transition.c の期待値で、production のコードや定数から
取らない。期待値は表 A（tests/data/fpu/fpu2_table_a.csv）の各セルの文言を読んで、このスクリプトの
`rows` に手で写したもので、次を機械的に確かめる。
  * 表 A の 20 状態 × 9 事象 × 2 機種の全セル（N/A 以外）に1行以上ある
  * N/A（68881 の S7・S8・S18）の行が無い
  * 【推】の列（inference）は表 A のセルの【推】の有無から取る（手で書かない）
分岐のあるセル（command 書込みの算術/FMOVEM/不正 word、setup の arith 違い）は分岐ごとに行を持つ。

列:
  device     68881 / 68882
  state      表 A の状態番号（開始状態）
  setup      試験が到達に使う手順の名前（C 側の setup_*）。同じ状態でも arith などが違う手順を分ける
  event      ack / abort / fsave / frestore_bad / command / condition / response / ia / reset
  variant    command の分岐（arith / movem / invalid）。他は -
  ret        API の戻り値（T / F）。void の事象は -
  next       事象の後の状態番号（fsave は body を全部読んだ後、frestore_bad は直後）
  probe      事象ごとの追加検査（key=value を ; で区切る）。無ければ -
  inference  表 A のセルが【推】を含めば 1、含まなければ 0
"""
import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE_A = ROOT / "tests" / "data" / "fpu" / "fpu2_table_a.csv"
OUT = ROOT / "tests" / "data" / "fpu" / "fpu2_take_transition.csv"
OUT_B = ROOT / "tests" / "data" / "fpu" / "fpu2_restore_transition.csv"
OUT_M = ROOT / "tests" / "data" / "fpu" / "fpu2_protocol_matrix.csv"
TABLE_B = ROOT / "tests" / "data" / "fpu" / "fpu2_table_b.csv"

EVENTS = ["ack", "abort", "fsave", "frestore_bad", "command", "condition", "response", "ia", "reset"]
DEVICES = ["68881", "68882"]
NA_81 = {7, 8, 18}  # 68881 は N/A の状態（表 A）
COLUMNS = ["device", "state", "setup", "event", "variant", "ret", "next", "probe", "inference"]

rows = []


def add(dev, state, setup, event, variant, ret, nxt, probe="-"):
    assert dev in DEVICES and event in EVENTS
    rows.append([dev, state, setup, event, variant, ret, nxt, probe, None])


def both(state, setup, event, variant, ret, nxt, probe="-"):
    for dev in DEVICES:
        if dev == "68881" and state in NA_81:
            continue
        add(dev, state, setup, event, variant, ret, nxt, probe)


def d81(state, setup, event, variant, ret, nxt, probe="-"):
    add("68881", state, setup, event, variant, ret, nxt, probe)


def d82(state, setup, event, variant, ret, nxt, probe="-"):
    add("68882", state, setup, event, variant, ret, nxt, probe)


def command_rows(state, setup, arith, movem, invalid, ret="T", dev=None):
    """arith / movem / invalid の3分岐（dev 指定なら片方の機種だけ）"""
    fn = both if dev is None else (d81 if dev == "68881" else d82)
    fn(state, setup, "command", "arith", ret, arith)
    fn(state, setup, "command", "movem", ret, movem)
    fn(state, setup, "command", "invalid", ret, invalid)


def fsave(fmt, p="-", b31=0, b27=1, b26=1, img=None):
    parts = [f"fmt={fmt}"]
    if fmt in ("idle", "busy"):
        parts += [f"b31={b31}", f"p={p}", f"b27={b27}", f"b26={b26}"]
    if img is not None:
        parts.append(f"img={img:04X}")
    return ";".join(parts)


def resp_null(ca=0, pc=0, tf=None, kind="phase"):
    """NULL primitive の期待値（[881UM Table 7-3]）。kind: idle（IA=0、PF=1）、cond（IA=0、PF=0、TF）、
    phase（命令の途中/終了段階。IA=1、PF=0）。proc は !PF"""
    if kind == "idle":
        ia, proc = 0, 0
    elif kind == "cond":
        ia, proc = 0, 1
    else:
        ia, proc = 1, 1
    s = f"NULL;ca={ca};pc={pc};ia={ia};proc={proc}"
    # TF は条件命令の結果の NULL（cond）だけが持つ。それ以外の NULL は TF=0 を検査する
    return s + f";tf={0 if tf is None else tf}"


def null_idle():
    return resp_null(0, 0, None, "idle")


def null_cond(tf):
    return resp_null(0, 0, tf, "cond")


def resp_take(kind, vec, pc):
    return f"{kind};vec={vec};pc={pc}"


# 戻り値・FPIAR の更新・read-back は runner が全行で直接検査するので probe には書かない
BAD_FMT = "-"
IA_OK = "-"


def pj(*parts):
    """probe の欄を ; で連結する（"-" は除く）"""
    parts = [p for p in parts if p != "-"]
    return ";".join(parts) if parts else "-"


# ---- S1 ----
both(1, "s1", "ack", "-", "-", 1)
both(1, "s1", "abort", "-", "-", 1)
both(1, "s1", "fsave", "-", "-", 1, fsave("null"))
both(1, "s1", "frestore_bad", "-", "-", 1, BAD_FMT)
both(1, "s1", "command", "arith", "T", 12)
both(1, "s1", "command", "movem", "T", 12)
both(1, "s1", "command", "invalid", "T", 5)
both(1, "s1", "condition", "-", "T", 13)
both(1, "s1", "response", "-", "-", 1, null_idle())
d81(1, "s1", "ia", "-", "T", 1, IA_OK)
d82(1, "s1", "ia", "-", "F", 11)
both(1, "s1", "reset", "-", "-", 1)

# ---- S2（setup は T 条件の TF=1 の NULL を hold に持つ。事象の後の応答で保持／空を見る） ----
HOLD_T = "then=" + null_cond(1).replace(";", ":")
HOLD_EMPTY = "then=" + null_idle().replace(";", ":")
both(2, "s2", "ack", "-", "-", 2, HOLD_T)
both(2, "s2", "abort", "-", "-", 2, HOLD_T)
both(2, "s2", "fsave", "-", "-", 2, pj(fsave("idle", "111", 0, 1, 1, 0), HOLD_EMPTY))
both(2, "s2", "frestore_bad", "-", "-", 2, pj(BAD_FMT, HOLD_T))
command_rows(2, "s2", 12, 12, 5)
both(2, "s2", "condition", "-", "T", 13)
both(2, "s2", "response", "-", "-", 2, null_cond(1))
d81(2, "s2", "ia", "-", "T", 2, pj(IA_OK, HOLD_T))
d82(2, "s2", "ia", "-", "F", 10)
both(2, "s2", "reset", "-", "-", 1)

# ---- S3（候補あり: s3、候補なし: s3n） ----
for dev_fn, nxt in ((d81, 2), (d82, 3)):
    for setup in ("s3", "s3n"):
        dev_fn(3, setup, "ack", "-", "-", nxt)
        dev_fn(3, setup, "abort", "-", "-", nxt)
both(3, "s3", "fsave", "-", "-", 2, fsave("idle", "111", 0, 0, 1, 0))
both(3, "s3", "frestore_bad", "-", "-", 3, BAD_FMT)
command_rows(3, "s3", 4, 12, 4)
command_rows(3, "s3n", 12, 12, 5)
both(3, "s3", "condition", "-", "T", 4)
both(3, "s3n", "condition", "-", "T", 13)
both(3, "s3", "response", "-", "-", 3, null_idle())
d81(3, "s3", "ia", "-", "T", 3, IA_OK)
d82(3, "s3", "ia", "-", "F", 10)
both(3, "s3", "reset", "-", "-", 1)

# ---- S4（command の UNSTARTED: s4、condition の UNSTARTED: s4c）。vector は DZ=50 ----
d81(4, "s4", "ack", "-", "-", 2)
d82(4, "s4", "ack", "-", "-", 9)
d81(4, "s4c", "ack", "-", "-", 2)
d82(4, "s4c", "ack", "-", "-", 9)
d81(4, "s4", "abort", "-", "-", 2)
d82(4, "s4", "abort", "-", "-", 9)
d81(4, "s4c", "abort", "-", "-", 2)
d82(4, "s4c", "abort", "-", "-", 4)
both(4, "s4", "fsave", "-", "-", 2, fsave("idle", "011", 0, 0, 1, 0x0422))
both(4, "s4c", "fsave", "-", "-", 2, fsave("idle", "001", 0, 0, 1, 0x0001))
both(4, "s4", "frestore_bad", "-", "-", 4, BAD_FMT)
TAKE50 = "then=TAKE_PRE:vec=50:pc=0"
both(4, "s4", "command", "arith", "T", 4, TAKE50)
d81(4, "s4", "command", "movem", "T", 12)
d82(4, "s4", "command", "movem", "T", 4, TAKE50)
both(4, "s4", "command", "invalid", "T", 4, TAKE50)
both(4, "s4", "condition", "-", "T", 4, TAKE50)
both(4, "s4", "response", "-", "-", 4, resp_take("TAKE_PRE", 50, 0))
d81(4, "s4", "ia", "-", "T", 4, IA_OK)
d82(4, "s4", "ia", "-", "F", 10)
both(4, "s4", "reset", "-", "-", 1)

# ---- S5 ----
both(5, "s5", "ack", "-", "-", 2)
both(5, "s5", "abort", "-", "-", 2)
both(5, "s5", "fsave", "-", "-", 2, fsave("idle", "011", 0, 1, 1, 0x2000))
both(5, "s5", "frestore_bad", "-", "-", 5, BAD_FMT)
command_rows(5, "s5", 12, 12, 5)
both(5, "s5", "condition", "-", "T", 13)
both(5, "s5", "response", "-", "-", 5, resp_take("TAKE_PRE", 11, 0))
d81(5, "s5", "ia", "-", "T", 5, IA_OK)
d82(5, "s5", "ia", "-", "F", 10)
both(5, "s5", "reset", "-", "-", 1)

# ---- S6 ----
both(6, "s6", "ack", "-", "-", 2)
d81(6, "s6", "abort", "-", "-", 2)
d82(6, "s6", "abort", "-", "-", 6)
both(6, "s6", "fsave", "-", "-", 2, fsave("idle", "001", 0, 1, 1, 0x0012))
both(6, "s6", "frestore_bad", "-", "-", 6, BAD_FMT)
command_rows(6, "s6", 12, 12, 5)
both(6, "s6", "condition", "-", "T", 13)
d81(6, "s6", "response", "-", "-", 6, resp_take("TAKE_PRE", 48, 1))
d82(6, "s6", "response", "-", "-", 8, resp_take("TAKE_PRE", 48, 1))
d81(6, "s6", "ia", "-", "T", 6, IA_OK)
d82(6, "s6", "ia", "-", "T", 7, IA_OK)
both(6, "s6", "reset", "-", "-", 1)

# ---- S7（68882 のみ） ----
d82(7, "s7", "ack", "-", "-", 2)
d82(7, "s7", "abort", "-", "-", 7)
d82(7, "s7", "fsave", "-", "-", 2, fsave("idle", "001", 0, 1, 1, 0x0012))
d82(7, "s7", "frestore_bad", "-", "-", 7, BAD_FMT)
command_rows(7, "s7", 12, 12, 5, dev="68882")
d82(7, "s7", "condition", "-", "T", 13)
d82(7, "s7", "response", "-", "-", 7, resp_take("TAKE_PRE", 48, 1))
d82(7, "s7", "ia", "-", "F", 10)
d82(7, "s7", "reset", "-", "-", 1)

# ---- S8（68882 のみ） ----
d82(8, "s8", "ack", "-", "-", 2)
d82(8, "s8", "abort", "-", "-", 8)
d82(8, "s8", "fsave", "-", "-", 2, fsave("idle", "001", 0, 1, 1, 0x0012))
d82(8, "s8", "frestore_bad", "-", "-", 6, BAD_FMT)
command_rows(8, "s8", 10, 10, 10, ret="F", dev="68882")
d82(8, "s8", "condition", "-", "F", 10)
d82(8, "s8", "response", "-", "-", 8, resp_take("TAKE_PRE", 48, 1))
d82(8, "s8", "ia", "-", "T", 7, IA_OK)
d82(8, "s8", "reset", "-", "-", 1)

# ---- S9（s9: 転送後 TAKE_MID の ACK 前（両機種）、s9p: 68882 の ACK 後の PRE） ----
d81(9, "s9", "ack", "-", "-", 2)
d82(9, "s9", "ack", "-", "-", 9)
d81(9, "s9", "abort", "-", "-", 2)
d82(9, "s9", "abort", "-", "-", 9)
both(9, "s9", "fsave", "-", "-", 2, fsave("idle", "111", 0, 0, 1, 0))
both(9, "s9", "frestore_bad", "-", "-", 9, BAD_FMT)
# 転送後の TAKE_MID（PRESENTED_MID）から替えると、読出しは TAKE_PRE（vector は live の OPERR=52）
TAKE52 = "then=TAKE_PRE:vec=52:pc=0"
both(9, "s9", "command", "arith", "T", 4, TAKE52)
d81(9, "s9", "command", "movem", "T", 12)
d82(9, "s9", "command", "movem", "T", 4, TAKE52)
both(9, "s9", "command", "invalid", "T", 4, TAKE52)
both(9, "s9", "condition", "-", "T", 4, TAKE52)
both(9, "s9", "response", "-", "-", 9, resp_take("TAKE_MID", 52, 0))
d82(9, "s9p", "response", "-", "-", 9, resp_take("TAKE_PRE", 50, 0))
d81(9, "s9", "ia", "-", "T", 9, IA_OK)
d82(9, "s9", "ia", "-", "F", 10)
both(9, "s9", "reset", "-", "-", 1)

# ---- S10（s10n: arith=NONE、s10p: PENDING、s10m: PRESENTED_MID） ----
for setup, b27, abort82 in (("s10n", 1, 2), ("s10p", 0, 3), ("s10m", 0, 9)):
    both(10, setup, "ack", "-", "-", 2)
    d81(10, setup, "abort", "-", "-", 2)
    d82(10, setup, "abort", "-", "-", abort82)
    both(10, setup, "fsave", "-", "-", 2, fsave("idle", "111", 1, b27, 1, 0))
    both(10, setup, "frestore_bad", "-", "-", 10, BAD_FMT)
    command_rows(10, setup, 10, 10, 10, ret="F")
    both(10, setup, "condition", "-", "F", 10)
    both(10, setup, "response", "-", "-", 10, resp_take("TAKE_MID", 13, 0))
    d81(10, setup, "ia", "-", "T", 10, IA_OK)
    d82(10, setup, "ia", "-", "F", 10)
    both(10, setup, "reset", "-", "-", 1)

# ---- S11 ----
both(11, "s11", "ack", "-", "-", 1)
both(11, "s11", "abort", "-", "-", 1)
both(11, "s11", "fsave", "-", "-", 1, fsave("null"))
both(11, "s11", "frestore_bad", "-", "-", 11, BAD_FMT)
command_rows(11, "s11", 11, 11, 11, ret="F")
both(11, "s11", "condition", "-", "F", 11)
both(11, "s11", "response", "-", "-", 11, resp_take("TAKE_MID", 13, 0))
d81(11, "s11", "ia", "-", "T", 11, IA_OK)
d82(11, "s11", "ia", "-", "F", 11)
both(11, "s11", "reset", "-", "-", 1)

# ---- S12（s12: register-to-register、s12p: PENDING の FMOVEM 静的出力） ----
for setup, b27, abort82, first in (
        ("s12", 1, 2, (resp_null(0, 0), 2)),  # 要求の無い命令の NULL（途中/終了段階）
        ("s12p", 0, 3, ("FP_REGISTERS;ca=1;dr=1;len=12;pc=0", 17))):
    d81(12, setup, "ack", "-", "-", 2)
    d82(12, setup, "ack", "-", "-", 12)
    d81(12, setup, "abort", "-", "-", 2)
    d82(12, setup, "abort", "-", "-", abort82)
    both(12, setup, "fsave", "-", "-", 2, fsave("busy", "011", 0, b27, 1))
    both(12, setup, "frestore_bad", "-", "-", abort82, BAD_FMT)
    command_rows(12, setup, 10, 10, 10, ret="F")
    both(12, setup, "condition", "-", "F", 10)
    both(12, setup, "response", "-", "-", first[1], first[0])
    both(12, setup, "reset", "-", "-", 1)
d81(12, "s12", "ia", "-", "T", 12, IA_OK)
d82(12, "s12", "ia", "-", "T", 12, IA_OK)      # PC=0 の NULL で後続の転送が無いもの（§5.5）
d81(12, "s12p", "ia", "-", "T", 12, IA_OK)
d82(12, "s12p", "ia", "-", "F", 10)            # FP_REGISTERS は register select 読出し待ち（規則2）

# 要求の無い命令の完了が保留算術例外を作ると S3 に入る（表 A の S12・S18 の →S2/S3）。
# sq12: DZ を enable して FDIV（FP0/FP1=1/0）を書いた S12（PC=1 の NULL。IA を先書きして S18 を経由しない）。
# sq18: 同じ命令の PC=1 の NULL を読んだ S18（68882）。完了後の応答は idle の NULL
d81(12, "sq12", "response", "-", "-", 3, resp_null(0, 1))  # 読出しで完了して S3
d82(12, "sq12", "response", "-", "-", 18, resp_null(0, 1))  # PC=1 の NULL を読むと S18
d82(18, "sq18", "ia", "-", "T", 3, "then=" + null_idle().replace(";", ":"))
both(12, "sq12", "ia", "-", "T", 12, IA_OK)

# ---- S13 ----
d81(13, "s13", "ack", "-", "-", 2)
d82(13, "s13", "ack", "-", "-", 13)
d81(13, "s13", "abort", "-", "-", 2)
d82(13, "s13", "abort", "-", "-", 13)
both(13, "s13", "fsave", "-", "-", 2, fsave("busy", "001", 0, 1, 1))
both(13, "s13", "frestore_bad", "-", "-", 2, BAD_FMT)
command_rows(13, "s13", 10, 10, 10, ret="F")
both(13, "s13", "condition", "-", "F", 10)
both(13, "s13", "response", "-", "-", 2, null_cond(0))
# 条件が真（TF=1）の S13。ACK・ABORT の後も同じ TF の NULL が読めること（TF を消す変異の検出）
d82(13, "s13t", "ack", "-", "-", 13, "then=" + null_cond(1).replace(";", ":"))
d82(13, "s13t", "abort", "-", "-", 13, "then=" + null_cond(1).replace(";", ":"))
both(13, "s13t", "response", "-", "-", 2, null_cond(1))
both(13, "s13", "ia", "-", "T", 13, IA_OK)
both(13, "s13", "reset", "-", "-", 1)

# ---- S14 / S15 / S16 / S17（各 setup と、PENDING の FMOVEM・control move 版 p） ----
dialog_states = [
    # state, setup, setup_p, busy p, b26, response
    (14, ("s14", "110", 0, 1, ("EA_TRANSFER;ca=1;dr=1;len=4;pc=0", 16)),
          ("s14p", "011", 1, 0, ("FP_REGISTERS;ca=1;dr=1;len=12;pc=0", 17))),
    (15, ("s15", "100", 1, 1, (resp_null(1, 0), 15)),
          ("s15p", "100", 1, 0, (resp_null(1, 0), 15))),
    (16, ("s16", "110", 0, 1, (resp_null(1, 0), 16)),
          ("s16p", "110", 0, 0, (resp_null(1, 0), 16))),
    (17, ("s17", "011", 1, 1, (resp_null(1, 0), 17)),
          ("s17p", "011", 1, 0, (resp_null(1, 0), 17))),
]
for state, normal, pending in dialog_states:
    for setup, p, b26, b27, resp in (normal, pending):
        abort82 = 2 if b27 else 3
        d81(state, setup, "ack", "-", "-", 2)
        d82(state, setup, "ack", "-", "-", state)
        d81(state, setup, "abort", "-", "-", 2)
        d82(state, setup, "abort", "-", "-", abort82)
        both(state, setup, "fsave", "-", "-", 2, fsave("busy", p, 0, b27, b26))
        both(state, setup, "frestore_bad", "-", "-", abort82, BAD_FMT)
        command_rows(state, setup, 10, 10, 10, ret="F")
        both(state, setup, "condition", "-", "F", 10)
        both(state, setup, "response", "-", "-", resp[1], resp[0])
        both(state, setup, "reset", "-", "-", 1)
    # IA 書込み: S14・S16・S17 は 68881 だけ合法、S15 は両機種で合法
    for setup in (normal[0], pending[0]):
        if state == 15:
            both(state, setup, "ia", "-", "T", state, IA_OK)
        else:
            d81(state, setup, "ia", "-", "T", state, IA_OK)
            d82(state, setup, "ia", "-", "F", 10)

# ---- S18（68882 のみ。入った primitive の種類: s18n/s18e/s18a/s18m） ----
for setup, resp, ia_next in (
        ("s18n", resp_null(0, 1), 2),     # register-to-register の PC=1 の NULL
        ("s18e", resp_null(1, 1), 14),    # 外部宛て FMOVE の PC=1 の NULL（CA=1）
        ("s18a", resp_null(1, 0), 15),    # PC=1 の EA_TRANSFER（DR=0）を読んだ後（消費済み）
        ("s18m", resp_null(1, 0), 15)):   # PC=1 の MP_REGISTER を読んだ後（消費済み）
    d82(18, setup, "ack", "-", "-", 18)
    d82(18, setup, "abort", "-", "-", 2)
    d82(18, setup, "fsave", "-", "-", 2, fsave("busy", "011", 0, 1, 1))
    d82(18, setup, "frestore_bad", "-", "-", 2, BAD_FMT)
    command_rows(18, setup, 10, 10, 10, ret="F", dev="68882")
    d82(18, setup, "condition", "-", "F", 10)
    d82(18, setup, "response", "-", "-", 18, resp)
    d82(18, setup, "ia", "-", "T", ia_next, IA_OK)
    d82(18, setup, "reset", "-", "-", 1)

# ---- S19（s19: S2 から、s19p: 保留算術例外のある S3 から。FSAVE が arith を捨てるので S2 へ戻る） ----
for setup in ("s19", "s19p"):
    d81(19, setup, "ack", "-", "-", 2)
    d82(19, setup, "ack", "-", "-", 19)
    both(19, setup, "abort", "-", "-", 2)
    both(19, setup, "fsave", "-", "-", 19, "fmt=illegal")
    both(19, setup, "frestore_bad", "-", "-", 2, BAD_FMT)
    command_rows(19, setup, 10, 10, 10, ret="F")
    both(19, setup, "condition", "-", "F", 10)
    both(19, setup, "response", "-", "-", 19, null_idle())
    d81(19, setup, "ia", "-", "T", 19, IA_OK)
    d82(19, setup, "ia", "-", "F", 10)
    both(19, setup, "reset", "-", "-", 1)

# ---- S20（s20: S2 から、s20f: S1 から入った（context_valid が違う）、s20p: S3 から入った（arith は捨てる）） ----
for setup, back, viol in (("s20", 2, 10), ("s20f", 1, 11), ("s20p", 2, 10)):
    d81(20, setup, "ack", "-", "-", back)
    d82(20, setup, "ack", "-", "-", 20)
    both(20, setup, "abort", "-", "-", back)
    both(20, setup, "fsave", "-", "-", 20, "fmt=illegal")
    both(20, setup, "frestore_bad", "-", "-", back, BAD_FMT)
    command_rows(20, setup, viol, viol, viol, ret="F")
    both(20, setup, "condition", "-", "F", viol)
    both(20, setup, "response", "-", "-", 20, null_idle())
    both(20, setup, "ia", "-", "T", 20, IA_OK)
    both(20, setup, "reset", "-", "-", 1)


# ============================================================================
# 表 B（有効な format word の FRESTORE）: tests/data/fpu/fpu2_restore_transition.csv
# 列: device,state,setup,frame,variant,next,probe,inference
#   frame    B1..B11
#   variant  frame の分岐（B4・B5: arith/movem/invalid の image、B8: idle/busy、B9: 30-28 の値）。無ければ -
#   next     FRESTORE の body を全部書いた後の状態番号
# 開始状態は表 A の20状態。各状態へ到達する手順（setup）は表 A の CSV と同じ。
# ============================================================================
START_SETUPS = [
    (1, "s1"), (2, "s2"), (3, "s3"), (3, "s3n"), (4, "s4"), (5, "s5"), (6, "s6"), (7, "s7"),
    (8, "s8"), (9, "s9"), (10, "s10n"), (11, "s11"), (12, "s12"), (13, "s13"), (14, "s14"),
    (15, "s15"), (16, "s16"), (17, "s17"), (18, "s18n"), (19, "s19"), (20, "s20"), (20, "s20f"),
]
# 開始時の context_valid（偽: S1、S11、S1 から入った S20）
CTX_FALSE = {"s1", "s11", "s20f"}
# 開始状態の live FPSR/FPCR に §8.1 の候補があるか → 候補の vector（無ければ None）
LIVE_VECTOR = {"s3": 50, "s4": 50, "s6": 48, "s7": 48, "s8": 48, "s9": 52}
NA_B_STATES = {7, 8, 18}
rows_b = []


def addb(dev, state, setup, frame, variant, nxt, probe="-"):
    rows_b.append([dev, state, setup, frame, variant, nxt, probe, None])


def gen_table_b():
    for dev in DEVICES:
        for state, setup in START_SETUPS:
            if dev == "68881" and state in NA_B_STATES:
                continue
            ctx = setup not in CTX_FALSE
            vec = LIVE_VECTOR.get(setup)
            idle_hold = "then=" + null_idle().replace(";", ":")
            addb(dev, state, setup, "B1", "-", 1, idle_hold + ";regs=reset;ctx=0")
            addb(dev, state, setup, "B2", "-", 2, idle_hold + ";fsave_b27=1")
            addb(dev, state, setup, "B3", "-", 3, idle_hold + ";fsave_b27=0")
            addb(dev, state, setup, "B4", "arith", 12)
            addb(dev, state, setup, "B4", "movem", 12)
            addb(dev, state, setup, "B4", "invalid", 5)
            take = f"then=TAKE_PRE:vec={vec}:pc=0" if vec else "-"
            addb(dev, state, setup, "B5", "arith", 4 if vec else 12, take)
            addb(dev, state, setup, "B5", "movem", 12)
            addb(dev, state, setup, "B5", "invalid", 4 if vec else 5, take)
            addb(dev, state, setup, "B6", "-", 13)
            addb(dev, state, setup, "B7", "-", 4 if vec else 13, take)
            viol = "then=TAKE_MID:vec=13:pc=0;fsave_b27=1"
            # B9・B11 は frame を適用せず context_valid を変えない。偽なら FSAVE は null frame で b27 が無い
            viol_nc = viol if ctx else "then=TAKE_MID:vec=13:pc=0"
            addb(dev, state, setup, "B8", "idle", 10, viol)
            addb(dev, state, setup, "B8", "busy", 10, viol)
            addb(dev, state, setup, "B9", "p010", 10 if ctx else 11, viol_nc)
            addb(dev, state, setup, "B10", "-", 16, "then=" + resp_null(1, 0).replace(";", ":") + ";fsave_b27=1")
            addb(dev, state, setup, "B11", "-", 10 if ctx else 11, viol_nc)


def finish_b():
    gen_table_b()
    with open(TABLE_A.parent / "fpu2_table_b.csv", newline="", encoding="utf-8") as fh:
        tb = list(csv.reader(fh))
    assert len(tb) == 12 and tb[0][0] == "frame"
    inf = {}
    for i, row in enumerate(tb[1:], start=1):
        assert row[0].startswith(f"B{i} "), row[0]
        inf[f"B{i}"] = 1 if "【推】" in row[1] else 0
    for r in rows_b:
        r[1] = int(r[1])
        r[5] = int(r[5])
        r[7] = inf[r[3]]
    frames = {r[3] for r in rows_b}
    assert frames == {f"B{i}" for i in range(1, 12)}, frames
    keys = {(r[0], r[1], r[3]) for r in rows_b}
    for dev in DEVICES:
        for s in range(1, 21):
            if dev == "68881" and s in NA_B_STATES:
                continue
            for b in range(1, 12):
                assert (dev, s, f"B{b}") in keys, (dev, s, b)
    assert len(keys) == 440 - 33, len(keys)
    return rows_b


def render_b(rs):
    out = ["device,state,setup,frame,variant,next,probe,inference"]
    for r in rs:
        out.append(",".join(str(x) for x in r))
    return "\n".join(out) + "\n"


# ============================================================================
# protocol violation 行列: tests/data/fpu/fpu2_protocol_matrix.csv（設計書 §11 の protocol violation）
# CIR アクセス7種 × §4.1 の phase 10種。列: device,phase,access,setups,expect,basis
#   setups  到達手順（| で区切った全てで同じ期待値になること）
#   expect  legal / violation / na（68881 に無い PC phase）
# 根拠: [881UM] p.6-20〜21（規則は「何を待っているか」で書かれる）と p.7-5〜7（§7.2.6〜7.2.9）。
# phase の当てはめと規則間の食い違いはより厳しい側（violation）を採る推論（設計書 §11）。
# §5.5 の p.6-21 NOTE の対象セル（68882 の外部宛て FMOVE/FMOVEM の DR=1 発行前の operand 読出し）は
# 期待値を violation とし、実機は無視と watchdog の bus error（簡略化。設計書 §5.5）。
# ============================================================================
ACCESSES = ["command", "condition", "operand_write", "operand_read", "register_select_read",
            "register_select_write", "ia_write"]
PHASES = ["IDLE", "COMMAND_PENDING", "CONDITION_PENDING", "RESPONSE_NEXT", "OPERAND_IN",
          "OPERAND_OUT", "REGISTER_SELECT", "PC", "SAVE_BODY", "RESTORE_BODY"]
rows_m = []


def addm(dev, phase, access, setups, expect, basis):
    rows_m.append([dev, phase, access, setups, expect, basis])


# phase ごとに合法なアクセス（IA 以外）
LEGAL = {
    "IDLE": {"command", "condition"},
    "COMMAND_PENDING": set(),
    "CONDITION_PENDING": set(),
    "RESPONSE_NEXT": set(),
    "OPERAND_IN": {"operand_write"},
    "OPERAND_OUT": {"operand_read"},
    "REGISTER_SELECT": {"register_select_read"},
    "PC": set(),
    "SAVE_BODY": {"operand_read"},
    "RESTORE_BODY": {"operand_write"},
}
PHASE_SETUPS = {
    # IDLE は保持 TAKE なし（s2）、保持中の算術 TAKE（s9）、list 0 の FMOVEM の register select 読出し後（s2l0）
    "IDLE": "s2|s9|s2l0",
    "COMMAND_PENDING": "s12",
    "CONDITION_PENDING": "s13",
    "RESPONSE_NEXT": "s14|s14d",
    "OPERAND_IN": "s15|s15d|s15r",
    "OPERAND_OUT": "s16|s16m",
    "REGISTER_SELECT": "s17",
    "PC": "s18n|s18e|s18a|s18m|s8",
    "SAVE_BODY": "s19",
    "RESTORE_BODY": "s20",
}
# 68882 の IA 書込み（IDLE の s6 の例外は表 A の S6 で別に検査済み）
IA_82 = {
    "IDLE": "violation", "CONDITION_PENDING": "legal", "RESPONSE_NEXT": "violation",
    "OPERAND_IN": "legal", "OPERAND_OUT": "violation", "REGISTER_SELECT": "violation",
    "PC": "legal", "SAVE_BODY": "violation", "RESTORE_BODY": "legal",
}
# COMMAND_PENDING × IA 書込みの5行（§5.5 の表。最初の primitive 別）
CMDPEND_IA_82 = [
    ("s12pc", "legal"),   # PC=1 のもの
    ("s12e", "legal"),    # PC=0 の EA_TRANSFER（DR=0）
    ("s12m", "violation"),  # FP_REGISTERS
    ("s12o", "violation"),  # 外部宛て FMOVE の PC=0 の NULL（CA=1）
    ("s12", "legal"),     # PC=0 の NULL で後続の転送が無いもの
]

# 根拠欄に記す2種の注記（設計書 §4.1、§5.5、§11）。CSV はコンマ区切りなので根拠欄にコンマを含めない
CMDPEND_DEVIATION = ("既知の逸脱候補: 要求の無い命令の最初の NULL を読む前の command 書込みは"
                     " p.7-22・p.6-38・p.7-6 の反対の読みでは合法（設計書 §4.1）")
NOTE_DIFF = ("p.6-21 NOTE の対象（%s）。実機は無視と watchdog の bus error で"
             "モデルは vector 13 に簡略化（設計書 §5.5）")
# NOTE の対象セルの operand 読出しの setup（§5.5 の3つ）: 外部宛て FMOVE の NULL と動的 list の MP_REGISTER の
# COMMAND_PENDING、動的 k-factor の MP register 待ち、PC=1 の MP_REGISTER／外部宛て FMOVE の PC=1 の NULL の後の PC
NOTE_SETUPS = {"COMMAND_PENDING": ["s12o", "s12d"], "OPERAND_IN": ["s15d"], "PC": ["s18e", "s18m"]}


def gen_matrix():
    for dev in DEVICES:
        for phase in PHASES:
            for access in ACCESSES:
                if dev == "68881" and phase == "PC":
                    addm(dev, phase, access, PHASE_SETUPS[phase], "na", "68881 は PC 要求を無視できるので PC phase が無い")
                    continue
                if access == "ia_write":
                    if dev == "68881":
                        addm(dev, phase, access, PHASE_SETUPS[phase], "legal", "68881 は IA 書込みが phase に関係なく常に合法（p.6-22、p.7-7〜8）")
                    elif phase == "COMMAND_PENDING":
                        for st, ex in CMDPEND_IA_82:
                            addm(dev, phase, access, st, ex, "§5.5 の表の最初の primitive 別（推論）")
                    else:
                        addm(dev, phase, access, PHASE_SETUPS[phase], IA_82[phase], "p.6-21 の 68882 の規則を phase に当てはめた（推論）")
                    continue
                if access == "register_select_write":
                    addm(dev, phase, access, PHASE_SETUPS[phase], "violation", "register select CIR 書込みは全 phase で violation（p.6-21）")
                    continue
                ex = "legal" if access in LEGAL[phase] else "violation"
                setups = PHASE_SETUPS[phase]
                basis = "p.6-20〜21 と §7.2.6〜7.2.9（厳しい側の当てはめは推論）"
                if phase == "COMMAND_PENDING" and access == "command":
                    basis += "。" + CMDPEND_DEVIATION
                if access == "operand_read" and phase in NOTE_SETUPS:
                    note = NOTE_SETUPS[phase]
                    if phase == "COMMAND_PENDING":
                        setups += "|" + "|".join(note)  # s12 だけでなく NOTE の対象の最初の primitive も通す
                    if dev == "68882":
                        basis += "。" + NOTE_DIFF % "・".join(note)
                addm(dev, phase, access, setups, ex, basis)


def render_m():
    out = ["device,phase,access,setups,expect,basis"]
    for r in rows_m:
        assert "," not in "".join(r[:5]) and "," not in r[5]
        out.append(",".join(r))
    return "\n".join(out) + "\n"


def load_inference():
    """表 A の【推】の有無: (状態番号, 事象) → 0/1"""
    with open(TABLE_A, newline="", encoding="utf-8") as fh:
        table = list(csv.reader(fh))
    head = table[0]
    col = {"ack": 1, "abort": 2, "fsave": 3, "frestore_bad": 4, "command": 5,
           "condition": 6, "response": 7, "ia": 8, "reset": 9}
    assert head[0] == "状態" and len(table) == 21
    inf = {}
    for i, row in enumerate(table[1:], start=1):
        assert row[0].startswith(f"S{i} "), row[0]
        for ev, c in col.items():
            inf[(i, ev)] = 1 if "【推】" in row[c] else 0
    return inf


def finish():
    inf = load_inference()
    for r in rows:
        r[1] = int(r[1])
        r[6] = int(r[6])
        r[8] = inf[(r[1], r[3])]
    seen = {}
    for r in rows:
        key = (r[0], r[1], r[3])
        seen[key] = seen.get(key, 0) + 1
        assert not (r[0] == "68881" and r[1] in NA_81), r
    missing = []
    for dev in DEVICES:
        for s in range(1, 21):
            if dev == "68881" and s in NA_81:
                continue
            for ev in EVENTS:
                if (dev, s, ev) not in seen:
                    missing.append((dev, s, ev))
    assert not missing, missing
    cells = len(DEVICES) * 20 * len(EVENTS) - len(NA_81) * len(EVENTS)
    assert len(seen) == cells == 333, (len(seen), cells)
    return rows


def render(rs):
    out = [",".join(COLUMNS)]
    for r in rs:
        out.append(",".join(str(x) for x in r))
    return "\n".join(out) + "\n"


def main():
    rs = finish()
    text = render(rs)
    rb = finish_b()
    text_b = render_b(rb)
    gen_matrix()
    n82 = sum(1 for r in rows_m if r[0] == "68882")
    n81 = sum(1 for r in rows_m if r[0] == "68881")
    assert (n81, n82) == (70, 74), (n81, n82)
    text_m = render_m()
    if "--check" in sys.argv:
        bad = 0
        for path, body in ((OUT, text), (OUT_B, text_b), (OUT_M, text_m)):
            if not path.exists() or path.read_text(encoding="utf-8") != body:
                print(f"{path.name} が最新でない。gen_fpu2_take_transition.py で再生成すること")
                bad = 1
        if not bad:
            print(f"OK: 表 A {len(rs)} 行（333 セル）、表 B {len(rb)} 行（407 セル）、violation 行列 {n81}+{n82} 行")
        return bad
    OUT.write_text(text, encoding="utf-8")
    OUT_B.write_text(text_b, encoding="utf-8")
    OUT_M.write_text(text_m, encoding="utf-8")
    print(f"{OUT}: {len(rs)} 行（333 セル）、{OUT_B}: {len(rb)} 行（407 セル）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
