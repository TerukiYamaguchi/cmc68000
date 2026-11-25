#!/usr/bin/env python3
"""FPU-3（Issue #68）・FPU-4（Issue #88）の演算子ベース自動変異生成器。

--target fpcp（既定、FPU-3）:
coproc.c 全体、m68000.c の cpu_dispatch_coproc_exception()、ops_system.c の op_rte() の format 9 周辺に、
演算子ごとの変異を機械的に全件生成し、test_fpcp_model・test_fpcp_host のどちらかが落ちるか（KILLED）を確かめる。
生き残り（SURVIVED）は scripts/fpcp_auto_allowlist.py の許可リスト（理由つき）に載っていなければならない。
収束条件は「生き残り ⊆ 許可リスト」（未分類が 0 件、許可リストの expect と実測が一致）。

--target fpu4（FPU-4、68040 内蔵 FPU）:
ops_fpu.c 全体、m68000.c の cpu_dispatch_fpu040_exception()、src/fpu の中間結果 API
（fpu_internal.c の mc68k_fpu_unrounded_*()、s_roundPackToExtF80.c の mantissa_bits_of()・
mc68k_unrounded_round_mantissa()）に変異を当て、test_fpu040_{fline,exec,frame}・test_68040_exception が落ちるかを確かめる。
許可リストは scripts/fpu4_auto_allowlist.py。

--target mc68060（Issue #107〜#111、MC68060-A〜E）:
ops_68060.c・ops_68060_mmu.c 全体と、060 の経路を持つ関数（op_cas、op_muldiv_long、movec_060、op_movec、op_line_f、
dis_lineF_060、ea_rmw_*）、060-C の m68000.c（FSLW・format $4/$2 の builder・BUSCR・割込み遅延など。
MC68060_M68000_FUNCTIONS・MC68060_M68000_LINES）と op_rte の 060 の分岐に変異を当て、
test_68060・test_68060_a・test_68060_mmu・test_68060_mmu_walk・test_68060_c・test_68060_d・test_68060_e・test_fpu_model_config・test_disasm_68060 が落ちるかを確かめる。
共有の関数（op_cas・op_movec・accept_irq など）は 060 の分岐・行だけに絞る（MC68060_SNIPPETS・MC68060_LINE_FILTERS）。
060-D（#110）は ops_68040_cache.c の 060 関数（cache_sets・rmw_bypass・op_cache060・move16_cycles・register_68060_cache_ops）。
060-E（#111）は ops_fpu.c の 060 関数（fpu060_classify・fpu060_trap・exec_save060・exec_restore060・op_fpu060 ほか）と、
m68000.c の fpu_config_accepted・cpu_dispatch_fpu040_exception の buscr_save 行。
許可リストは scripts/mc68060_auto_allowlist.py。

--target tablewalk（Issue #100、68040 の table walk と D-cache）:
ops_68040_cache.c と ops_68040_mmu.c のうち、#100 の実装前の親コミット（TABLEWALK_BASE）から変わった行だけに変異を当て、
test_68040_table_walk_cache が落ちるかを確かめる。許可リストは scripts/tablewalk_auto_allowlist.py。

使い方:
  scripts/fpcp_auto_mutants.py --target fpu4 ...      FPU-4 に切り替える（以下の例はどちらの target でも使える）
  scripts/fpcp_auto_mutants.py --list                 変異を生成して件数と一覧だけ表示する（ビルドしない）
  scripts/fpcp_auto_mutants.py                        全件実行して許可リストと突き合わせる（収束の判定。--jobs 10 で実測約 1 時間 45 分）
                                                      段 1: test_fpcp_model・test_fpcp_host、段 2: 段 1 の生き残りを全ターゲット＋ctest（diff 系を除く）
  scripts/fpcp_auto_mutants.py --jobs 8 --only coproc.c,cir_initial
  scripts/fpcp_auto_mutants.py --results out.json     実行結果を保存する（--from-results で再分類だけ行う）

判定のキャッシュ（~/.cache/mc68000-automut/<target>.json。リポジトリの外）:
  既定で前回の判定を再利用し、当たらない変異だけを走らせる（規則は「判定のキャッシュ」節）。試験だけを足した後の
  再実行は、生き残りだけが再検査の対象になる。ソース側（src・include・extern・tools・CMake・本スクリプト）を変えたら全件が取り直しになる。
  scripts/fpcp_auto_mutants.py --fresh                節目（Issue を閉じる前・フェーズの最後）の収束判定。キャッシュを読まず全件を取り直す
  scripts/fpcp_auto_mutants.py --seed-cache out.json  全件実行（--results out.json）の結果をキャッシュに取り込む。結果 JSON の _meta（ソース・試験の
                                                      ハッシュ、完了の有無）が今の状態と違えば拒否する
  scripts/fpcp_auto_mutants.py --no-cache             キャッシュを読みも書きもしない
  新しい対象（例: 68060）は TARGETS に 1 項目（変異するソース・段 1 の試験・許可リストのモジュール）を足すだけで同じ仕組みを使える。

変異は作業用コピー（既定 /tmp/mc68000-automut/w<N>）で適用する。リポジトリのソースと build は触らない
（同期フォルダ上のソースを書き換えると同期が走るため）。macOS の make はタイムスタンプが1秒粒度なので、
変異のたびに対象のオブジェクトと実行ファイルを消してから再ビルドする。
"""
import argparse
import bisect
import concurrent.futures
import hashlib
import importlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
from dataclasses import dataclass, asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

DEFAULT_WORKDIR = Path("/tmp/mc68000-automut")
# 前回までの判定の保存先（対象ごとに <target>.json）。リポジトリの外に置く
DEFAULT_CACHE_DIR = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "mc68000-automut"
# 判定が依存する入力。SRC_ENV_ITEMS は変えると全変異の判定が無効になる（ライブラリとビルド設定）。
# TESTS_ENV_ITEMS は生き残りの判定だけを無効にする（試験が増えると、生き残りが殺されうる）
SRC_ENV_ITEMS = ("src", "include", "extern", "tools", "CMakeLists.txt", "tests/CMakeLists.txt",
                 "tests/diff/CMakeLists.txt", "scripts/fpcp_auto_mutants.py")  # 最後は実行の仕組み（段 2 の ctest の除外・上限）
TESTS_ENV_ITEMS = ("tests", "tests/data/**/*.csv")
ENV_EXCLUDE_NAMES = {".git", "__pycache__"}
ENV_EXCLUDE_FILES = {".DS_Store"}  # Git 無視の生成物。入力に数えると --seed-cache が拒否されうる
ENV_EXCLUDE_SUFFIXES = (".pyc", ".pyo")
ENV_EXCLUDE_PATHS = {"tests/diff"}  # 段 2 の ctest が除く差分試験（Musashi 生成コード）は入力に数えない
TEST_TIMEOUT_SEC = 180
COPY_ITEMS = ("src", "include", "tests", "extern", "tools", "scripts", "docs", "CMakeLists.txt", ".clang-format")


@dataclass(frozen=True)
class Target:
    source_files: tuple  # 変異で書き換えるファイル（このファイルのオブジェクトだけ消して再ビルドする）
    tests: tuple  # 段 1 で走らせる試験ターゲット
    allowlist: str  # 許可リストのモジュール名（scripts/ 直下）


TARGETS = {
    "fpcp": Target(("src/cpu/coproc.c", "src/cpu/m68000.c", "src/cpu/ops_system.c", "src/cpu/decode.c"),
                   ("test_fpcp_model", "test_fpcp_host"), "fpcp_auto_allowlist"),
    "tablewalk": Target(("src/cpu/ops_68040_cache.c", "src/cpu/ops_68040_mmu.c"),
                        ("test_68040_table_walk_cache",), "tablewalk_auto_allowlist"),
    "fpu4": Target(("src/cpu/ops_fpu.c", "src/cpu/m68000.c", "src/fpu/fpu_internal.c",
                    "src/fpu/softfloat_target/s_roundPackToExtF80.c"),
                   ("test_fpu040_fline", "test_fpu040_exec", "test_fpu040_frame", "test_68040_exception",
                    "test_fpu040_gaps"),
                   "fpu4_auto_allowlist"),
    # MC68060-A/B/C/D（Issue #107〜#110）。060 固有の関数だけに変異を当てる（MOVEC・CAS・MUL/DIV・line-F・逆アセンブラ・EA 補助は 060 の経路を含む関数）
    "mc68060": Target(("src/cpu/ops_68060.c", "src/cpu/ops_68060_mmu.c", "src/cpu/ops_68020_system.c",
                       "src/cpu/ops_68020_arith.c", "src/cpu/ops_system_ext.c", "src/cpu/decode.c",
                       "src/debug/disasm.c", "src/cpu/ea_helpers.c", "src/cpu/m68000.c", "src/cpu/ops_system.c",
                       "src/cpu/ops_68040_cache.c", "src/cpu/ops_fpu.c"),
                      ("test_68060", "test_68060_a", "test_68060_mmu", "test_68060_mmu_walk", "test_68060_c", "test_68060_d",
                       "test_68060_e", "test_fpu_model_config", "test_disasm_68060"),
                      "mc68060_auto_allowlist"),
}
DEFAULT_TARGET = "fpcp"
# tablewalk（Issue #100）は、#100 の実装前の親コミットから今の作業ツリーまでに変わった行だけに変異を当てる
# （既存行は C-N14 の backlog。ファイル全体の変異は別 Issue で扱う）
TABLEWALK_BASE = "6a97ac4^"

STATUS_KILLED = "KILLED"
STATUS_KILLED_STAGE2 = "KILLED-STAGE2"
# 段 2 で流す ctest から外す試験（Musashi 生成コードとの差分試験。FPU-3 の対象外で時間がかかる）と、
# 設計書の追跡性検査（作業コピーに .git が無く、HEAD の src を取り出せない）
STAGE2_CTEST_EXCLUDE = "diff|traceability"
STATUS_SURVIVED = "SURVIVED"
STATUS_BUILD_ERROR = "BUILD-ERROR"


@dataclass(frozen=True)
class Mutant:
    mid: str  # file:line:op:k（line は表示用。許可リストは line を使わない）
    file: str
    func: str
    line: int
    op: str
    start: int
    end: int
    repl: str
    text: str  # 元の行（strip 済み）
    desc: str  # 置換前 → 置換後


# ---------------------------------------------------------------------------------------------
# 判定のキャッシュ（途中の確認を速くする。収束の最終判定は --fresh で全件を取り直す）
# ---------------------------------------------------------------------------------------------
# 変異の判定は、(1) 変異を当てたファイルの全文、(2) ライブラリとビルド設定（src_env）、(3) 試験（tests_env）で
# 決まる。次の規則で前回の判定を使い回し、当たらない変異だけを走らせる。
#   - src_env が違えば、すべて取り直す（実装を変えたら、どの変異の判定も変わりうる）
#   - KILLED / KILLED-STAGE2 / BUILD-ERROR は、src_env が同じなら再利用する。試験は増える一方という前提で、
#     殺した試験の弱体化・削除は検出しない（そのため --fresh を節目に使う）
#   - SURVIVED は、tests_env も同じときだけ再利用する（試験が変われば殺されうる）

CACHE_STATUSES = (STATUS_KILLED, STATUS_KILLED_STAGE2, STATUS_SURVIVED, STATUS_BUILD_ERROR)
KEPT_WHEN_TESTS_CHANGE = (STATUS_KILLED, STATUS_KILLED_STAGE2, STATUS_BUILD_ERROR)


def _hash_tree(patterns):
    """patterns（リポジトリ相対のパス・ディレクトリ・glob）の下の全ファイルの内容から 1 つのハッシュを作る。"""
    h = hashlib.sha256()
    files = []
    for pat in patterns:
        for base in sorted(ROOT.glob(pat)):
            if base.is_dir():
                for dirpath, dirnames, filenames in os.walk(base):
                    dirnames[:] = sorted(
                        d for d in dirnames
                        if d not in ENV_EXCLUDE_NAMES
                        and str((Path(dirpath) / d).relative_to(ROOT)) not in ENV_EXCLUDE_PATHS)
                    files += [Path(dirpath) / f for f in sorted(filenames)
                              if f not in ENV_EXCLUDE_FILES and not f.endswith(ENV_EXCLUDE_SUFFIXES)]
            elif base.is_file():
                files.append(base)
    for f in sorted(set(files)):
        h.update(str(f.relative_to(ROOT)).encode() + b"\0")
        h.update(hashlib.sha256(f.read_bytes()).digest())
    return h.hexdigest()


def compute_envs():
    return {"src_env": _hash_tree(SRC_ENV_ITEMS), "tests_env": _hash_tree(TESTS_ENV_ITEMS)}


def mutant_key(m, originals):
    """変異を当てた後のファイル全文のハッシュ。行番号が動いても、同じ変異は同じ鍵になる。"""
    text = originals[m.file]
    mutated = text[:m.start] + m.repl + text[m.end:]
    return hashlib.sha256((m.file + "\0" + mutated).encode()).hexdigest()[:24]


def load_cache(path):
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


CTEST_TIMEOUT_MARK = "Timeout"  # ctest の "***Timeout"。壁時計で測るので、機械のスリープで偽の検出になりうる
STAGE1_TIMEOUT_MARK = ": timeout"  # 段 1 の subprocess の上限（単調時計。スリープを数えないので、本物の無限ループ）


def is_timeout_kill(status, detail):
    """機械のスリープによる偽の検出（ctest の Timeout）を含みうる kill。キャッシュに残さない。
    段 1 の timeout は本物の無限ループなので対象にしない（キャッシュして、毎回 180 秒を払わない）。"""
    return status in (STATUS_KILLED, STATUS_KILLED_STAGE2) and CTEST_TIMEOUT_MARK in (detail or "")


def cache_lookup(entry, envs, stage1_tests):
    """前回の判定を再利用できるなら (status, detail)、できなければ None。"""
    if not entry or entry.get("status") not in CACHE_STATUSES or is_timeout_kill(entry["status"], entry.get("detail")):
        return None
    if entry.get("src_env") != envs["src_env"] or entry.get("stage1_tests") != list(stage1_tests):
        return None
    if entry["status"] in KEPT_WHEN_TESTS_CHANGE or entry.get("tests_env") == envs["tests_env"]:
        return entry["status"], entry.get("detail", "")
    return None


def save_cache(path, cache, fresh_entries, envs, stage1_tests):
    """今回の判定を足し、src_env が違う古い項目は捨てる（使えないので）。"""
    for key, (status, detail) in fresh_entries.items():
        if is_timeout_kill(status, detail):
            cache.pop(key, None)  # 古い判定も捨て、次回は取り直す
            continue
        cache[key] = {"status": status, "detail": detail, "stage1_tests": list(stage1_tests), **envs}
    cache = {k: v for k, v in cache.items() if v.get("src_env") == envs["src_env"]}
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(cache))
    tmp.replace(path)
    return cache


def write_results(path, results, meta):
    """結果の JSON。_meta に、どの状態で取った結果かを残す（--seed-cache が照合する）。"""
    path.write_text(json.dumps({"_meta": meta, **results} if meta else results, indent=1))


def read_results(path):
    data = json.loads(path.read_text())
    return data, data.pop("_meta", None)


# ---------------------------------------------------------------------------------------------
# 字句の下ごしらえ
# ---------------------------------------------------------------------------------------------

def mask_source(text):
    """コメント・文字列・文字定数を同じ長さの空白に置き換える（オフセットを保つ）。"""
    out = list(text)
    n = len(text)
    i = 0

    def blank(a, b):
        for k in range(a, min(b, n)):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif text[i] in "\"'":
            q = text[i]
            j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            blank(i, j + 1)
            i = j + 1
        else:
            i += 1
    return "".join(out)


FUNC_RE = re.compile(r"(?m)^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;{}]*\)\s*\{")


def function_spans(masked):
    spans = []
    for m in FUNC_RE.finditer(masked):
        end = masked.find("\n}", m.end())
        spans.append((m.start(), len(masked) if end < 0 else end + 2, m.group(1)))
    return spans


def match_paren(masked, open_pos):
    depth = 0
    for k in range(open_pos, len(masked)):
        if masked[k] == "(":
            depth += 1
        elif masked[k] == ")":
            depth -= 1
            if depth == 0:
                return k
    return -1


# ---------------------------------------------------------------------------------------------
# 演算子（each: masked, text -> [(op, start, end, repl)]）
# ---------------------------------------------------------------------------------------------

def op_cir_initial(masked, text):
    """cp_cir_read/write の initial 引数（最後の true/false）を反転する。"""
    out = []
    for m in re.finditer(r"\bcp_cir_(?:read|write)\s*\(", masked):
        close = match_paren(masked, m.end() - 1)
        if close < 0:
            continue
        mm = re.search(r"\b(true|false)\s*$", masked[m.end():close])
        if mm:
            s = m.end() + mm.start(1)
            out.append(("cir_initial", s, m.end() + mm.end(1), "false" if mm.group(1) == "true" else "true"))
    return out


def op_exec_state(masked, text):
    """cp_memory_ok() の除去、exec_state 検査の除去。"""
    out = []
    for m in re.finditer(r"\bcp_memory_ok\s*\(\s*r\s*\)", masked):
        out.append(("memory_ok_remove", m.start(), m.end(), "true"))
    for m in re.finditer(r"(?:\w+->)*exec_state\s*(==|!=)\s*CPU_NORMAL", masked):
        out.append(("exec_state_remove", m.start(), m.end(), "true" if m.group(1) == "==" else "false"))
    return out


def op_compare(masked, text):
    """比較演算子の境界（>= と >、<= と <）と、== と !=。"""
    out = []
    for m in re.finditer(r"<=|>=|==|!=|(?<![<>\-])[<>](?![<>=])", masked):
        tok = m.group(0)
        if tok in ("<", ">") and masked[max(0, m.start() - 1)] == "#":
            continue
        repl = {"<": "<=", ">": ">=", "<=": "<", ">=": ">", "==": "!=", "!=": "=="}[tok]
        out.append(("compare", m.start(), m.end(), repl))
    return out


LITERAL_RE = re.compile(r"(?<![\w.])(0[xX][0-9A-Fa-f]+|\d+)([uUlL]*)(?![\w.])")


def op_constants(masked, text):
    """定数の ±1 と、ビットマスクの 1 ビット反転。"""
    out = []
    for m in LITERAL_RE.finditer(masked):
        lit, suffix = m.group(1), m.group(2)
        hexa = lit[:2] in ("0x", "0X")
        value = int(lit, 16) if hexa else int(lit)
        digits = len(lit) - 2 if hexa else 0
        width = 4 * digits if hexa else max(value.bit_length(), 1)

        def fmt(v):
            return (f"0x{v:0{digits}X}" if hexa else str(v)) + suffix

        for delta in (1, -1):
            if value + delta >= 0 and (value + delta).bit_length() <= max(width, value.bit_length()) + 1:
                out.append(("const_pm1", m.start(), m.end(), fmt(value + delta)))
        line_start = masked.rfind("\n", 0, m.start()) + 1
        before = masked[line_start:m.start()].rstrip()
        after = masked[m.end():m.end() + 8].lstrip()
        define_start = line_start  # 行継続（末尾の \）で続く #define の値も、#define の行として扱う
        while define_start > 0 and masked[define_start - 2:define_start - 1] == "\\":
            define_start = masked.rfind("\n", 0, define_start - 1) + 1
        is_mask = (masked[define_start:].lstrip().startswith("#define") and hexa) or \
            before.endswith(("&", "|", "^", "~")) or after.startswith(("&", "|", "^")) and not after.startswith(("&&", "||"))
        if is_mask and not before.endswith(("&&", "||")):
            for bit in range(width):
                flipped = value ^ (1 << bit)
                out.append(("mask_bitflip", m.start(), m.end(), fmt(flipped)))
    return out


def _term_bounds(masked, pos, direction):
    """pos（&& か || の直前直後）から、同じ括弧の深さで区切り記号までの項の範囲を返す。"""
    n = len(masked)
    depth = 0
    k = pos
    step = 1 if direction > 0 else -1
    stops = ";,?:{}"
    lo = hi = None
    while 0 <= k < n:
        c = masked[k]
        if c in "()":
            opens = (c == "(") == (direction > 0)
            if opens:
                depth += 1
            else:
                if depth == 0:
                    break
                depth -= 1
        elif depth == 0:
            if c in stops:
                break
            two = masked[k:k + 2] if direction > 0 else masked[k - 1:k + 1]
            if two in ("&&", "||"):
                break
            if c == "=" and masked[k - 1:k] not in ("=", "!", "<", ">") and masked[k + 1:k + 2] != "=":
                break
        if not c.isspace():
            lo = k if lo is None else min(lo, k)
            hi = k if hi is None else max(hi, k)
        k += step
    if lo is None:
        return None
    keyword = re.match(r"return\s+", masked[lo:hi + 1])
    if keyword:
        lo += keyword.end()
    return (lo, hi + 1)


def op_logic_terms(masked, text):
    """&& と || の左項・右項を true / false に置き換える（項の除去）。"""
    out = []
    for m in re.finditer(r"&&|\|\|", masked):
        const = "true" if m.group(0) == "&&" else "false"
        right = _term_bounds(masked, m.end(), 1)
        left = _term_bounds(masked, m.start() - 1, -1)
        for side, b in (("left", left), ("right", right)):
            if b:
                a, z = min(b), max(b)
                out.append((f"logic_{side}", a, z, const))
    return out


def op_not_remove(masked, text):
    """否定 `!x`・`!(` の `!` を除去する。"""
    return [("not_remove", m.start(), m.start() + 1, "") for m in re.finditer(r"!(?=[A-Za-z_(])", masked)]


def op_width(masked, text):
    """long→word の幅変更（逆向きも）と、キャスト幅の変更。"""
    out = []
    swaps = {"CIR_SIZE_LONG": "CIR_SIZE_WORD", "CIR_SIZE_WORD": "CIR_SIZE_LONG",
             "cpu_read_long": "cpu_read_word", "cpu_read_word": "cpu_read_long",
             "cpu_write_long": "cpu_write_word", "cpu_write_word": "cpu_write_long",
             "cpu_fetch_word": "cpu_fetch_long"}
    for m in re.finditer(r"\b(" + "|".join(swaps) + r")\b", masked):
        out.append(("width", m.start(), m.end(), swaps[m.group(1)]))
    casts = {"uint8_t": "uint16_t", "uint16_t": "uint32_t", "uint32_t": "uint16_t",
             "int8_t": "int16_t", "int16_t": "int32_t", "int32_t": "int16_t"}
    for m in re.finditer(r"\(\s*(u?int(?:8|16|32)_t)\s*\)", masked):
        out.append(("cast_width", m.start(1), m.end(1), casts[m.group(1)]))
    return out


def op_bool_const(masked, text):
    """return / 代入の true ⇄ false。"""
    out = []
    for m in re.finditer(r"\b(return|=)\s*(true|false)\s*[;,]", masked):
        out.append(("bool_const", m.start(2), m.end(2), "false" if m.group(2) == "true" else "true"))
    return out


def op_operators(masked, text):
    """シフト方向、+ と -、& と | の入替。"""
    out = []
    for m in re.finditer(r"<<|>>", masked):
        out.append(("shift_dir", m.start(), m.end(), ">>" if m.group(0) == "<<" else "<<"))
    for m in re.finditer(r"(?<=\s)([+\-])(?=\s)", masked):
        out.append(("arith_swap", m.start(), m.end(), "-" if m.group(1) == "+" else "+"))
    for m in re.finditer(r"(?<=\s)([&|])(?=\s)", masked):
        out.append(("bitop_swap", m.start(), m.end(), "|" if m.group(1) == "&" else "&"))
    return out


STMT_START_RE = re.compile(r"^\s*(?:r->\w+|cpu->[\w>.\-\[\]]+|\*\w+|cp_\w+\(|mc68k_\w+\(|cpu_\w+\()[^;{}]*;\s*$")


def op_stmt_delete(masked, text):
    """1 行の代入・呼出し文を (void)0; に置き換える。"""
    out = []
    pos = 0
    for line in masked.split("\n"):
        if STMT_START_RE.match(line) and not re.match(r"^\s*(return|if|else|for|while)\b", line) and \
                line.count("(") == line.count(")"):
            s = pos + len(line) - len(line.lstrip())
            e = pos + len(line.rstrip())
            out.append(("stmt_delete", s, e, "(void)0;"))
        pos += len(line) + 1
    return out


# tablewalk（#103 C-N33）: `r.ok = false;`・`line->valid = false;`・`value |= bits;`・`d = nd;` のような
# 値を代入する 1 行の文。STMT_START_RE は `r->` 形式の代入と呼出しだけを拾うので、tablewalk では追加で審査する。
ASSIGN_STMT_RE = re.compile(r"^\s*[A-Za-z_][\w]*(?:(?:\.|->)[A-Za-z_]\w*)*(?:\[[^\]\n]*\])?\s*(?:\|=|&=|\+=|-=|=)[^=][^;{}]*;\s*$")


def op_assign_stmt_delete(masked, text):
    """値を代入する 1 行の文（宣言を除く）を (void)0; に置き換える。"""
    out = []
    pos = 0
    for line in masked.split("\n"):
        if ASSIGN_STMT_RE.match(line) and not re.match(r"^\s*(return|if|else|for|while)\b", line) and \
                line.count("(") == line.count(")"):
            s = pos + len(line) - len(line.lstrip())
            e = pos + len(line.rstrip())
            out.append(("assign_delete", s, e, "(void)0;"))
        pos += len(line) + 1
    return out


INLINE_BLOCK_RE = re.compile(r"(?m)^[^\n]*\)\s*\{([^{}\n]*;)\s*\}[ \t]*$")


def op_inline_block(masked, text):
    """1 行ブロック `if (...) { a; return n; }` の中の文を 1 つずつ (void)0; にする（op_stmt_delete・op_return_delete は
    1 行 1 文だけを対象にするので、1 行ブロックの中の文は拾えない）。"""
    out = []
    for blk in INLINE_BLOCK_RE.finditer(masked):
        body_start = blk.start(1)
        for st in re.finditer(r"[^;]+;", blk.group(1)):
            lead = len(st.group(0)) - len(st.group(0).lstrip())
            s = body_start + st.start() + lead
            e = body_start + st.end()
            kind = "inline_return_delete" if st.group(0).lstrip().startswith("return") else "inline_stmt_delete"
            out.append((kind, s, e, "(void)0;"))
    return out


FORMAT9_SKIP_RE = re.compile(r"^\s*(cpu->a\[7\] \+= \(uint32_t\)bytes;|cpu->pc = new_pc;|cpu_set_sr\(cpu, new_sr\);|"
                             r"cpu->flow_changed = true;)\s*$")


def op_format9_skip(masked, text):
    """op_rte() で format 9 だけ共通の文を飛ばす（共通の文は他の format の試験が守るため、format 9 に限った欠落を見る）。"""
    out = []
    pos = 0
    for line in masked.split("\n"):
        m = FORMAT9_SKIP_RE.match(line)
        if m:
            s = pos + len(line) - len(line.lstrip())
            out.append(("skip_for_format9", s, s, "if (format != 9) "))
        pos += len(line) + 1
    return out


def op_if_cond(masked, text):
    """if の条件全体を true / false / 否定に置き換える（&& || の項単位の logic_* では作れない条件の定数化・反転）。"""
    out = []
    for m in re.finditer(r"\bif\s*\(", masked):
        open_pos = m.end() - 1
        close = match_paren(masked, open_pos)
        if close < 0:
            continue
        s, e = open_pos + 1, close
        out.append(("if_const", s, e, "true"))
        out.append(("if_const", s, e, "false"))
        out.append(("if_negate", s, e, "!(" + text[s:e] + ")"))
    return out


def _ternary_cond_start(masked, q):
    """`?` の位置 q から後ろ向きに、三項演算子の条件の先頭を探す（&& || は条件の一部。代入・return・区切り記号・
    括弧の外側で止まる）。"""
    depth = 0
    lo = None
    k = q - 1
    while k >= 0:
        c = masked[k]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                break
            depth -= 1
        elif depth == 0:
            if c in ";,?:{}":
                break
            if c == "=" and masked[k - 1:k] not in ("=", "!", "<", ">") and masked[k + 1:k + 2] != "=":
                break
        if not c.isspace():
            lo = k
        k -= 1
    if lo is None:
        return None
    returns = list(re.finditer(r"\breturn\s+", masked[lo:q]))  # `if (...) return c ? a : b;` の if 側へ戻らない
    return lo + returns[-1].end() if returns else lo


def op_ternary_cond(masked, text):
    """三項演算子 `c ? a : b` の条件 c を true / false の定数にする（if_const の三項版）。"""
    out = []
    for m in re.finditer(r"\?", masked):
        s = _ternary_cond_start(masked, m.start())
        if s is None:
            continue
        e = m.start()
        while e > s and masked[e - 1].isspace():
            e -= 1
        out.append(("ternary_cond", s, e, "true"))
        out.append(("ternary_cond", s, e, "false"))
    return out


def op_bool_return(masked, text):
    """bool を返す関数の `return <式>;` の式を true / false にする（式が true / false の定数のものは bool_const が扱う）。"""
    out = []
    for fs, fe, name in function_spans(masked):
        head = masked[fs:masked.find("{", fs)]
        if not re.search(r"\bbool\b", head.split(name)[0]):
            continue
        for m in re.finditer(r"\breturn\s+([^;]+);", masked[fs:fe]):
            if m.group(1).strip() in ("true", "false"):
                continue
            s, e = fs + m.start(1), fs + m.end(1)
            out.append(("bool_return", s, e, "true"))
            out.append(("bool_return", s, e, "false"))
    return out


def op_return_delete(masked, text):
    """関数本体の入れ子の中（インデント 8 以上）の `return ...;` を (void)0; にする（早期 return の脱落。続きの処理へ進む）。"""
    return [("return_delete", m.start(1), m.end(1), "(void)0;")
            for m in re.finditer(r"(?m)^[ ]{8,}(return\b[^;{}]*;)[ \t]*$", masked)]


# 関連する識別子の取り違え（引数・条件の変数、EA の分類関数、機種ゲート）。値の取り違えを機械的に作る
IDENT_SWAPS = {
    "code": ("func",), "func": ("code",), "fw": ("rb",), "rb": ("fw",),
    "coprocessor_valid": ("memory_valid",), "memory_valid": ("coprocessor_valid",),
    "ea_is_control": ("ea_is_control_alterable",), "ea_is_control_alterable": ("ea_is_control",),
    "ea_is_data_alterable": ("ea_is_alterable", "ea_is_data"), "ea_is_alterable": ("ea_is_data_alterable",),
    "ea_is_data": ("ea_is_data_alterable",), "ea_is_memory_alterable": ("ea_is_data_alterable",),
    "MC68K_MODEL_68020": ("MC68K_MODEL_68010", "MC68K_MODEL_68030"),
    "MC68K_FC_CPU_SPACE": ("0",),
}


def op_ident_swap(masked, text):
    """IDENT_SWAPS の識別子の取り違えと、アクセス記述子の read の反転・size の固定。"""
    out = []
    for m in re.finditer(r"\b(" + "|".join(IDENT_SWAPS) + r")\b", masked):
        if masked[masked.rfind("\n", 0, m.start()) + 1:m.start()].lstrip().startswith("#define"):
            continue  # マクロの仮引数名は式の取り違えにならない
        for repl in IDENT_SWAPS[m.group(1)]:
            out.append(("ident_swap", m.start(), m.end(), repl))
    for m in re.finditer(r"\.read\s*=\s*(read)\b", masked):
        out.append(("ident_swap", m.start(1), m.end(1), "!read"))
    for m in re.finditer(r"\.size\s*=\s*(size)\b", masked):
        out.append(("ident_swap", m.start(1), m.end(1), "4"))
    for m in re.finditer(r"\br->ea\b(?!\s*=[^=])", masked):
        out.append(("ident_swap", m.start(), m.end(), "0"))
    return out


def op_cycle_adj(masked, text):
    """サイクル数の式: CYC(...) に + 1、`cycles += f(...)` の呼出し結果を 0 にする（戻り値を捨てる）。"""
    out = []
    for m in re.finditer(r"\bCYC\s*\([^()]*\)", masked):
        out.append(("cycle_adj", m.end(), m.end(), " + 1"))
    for m in re.finditer(r"\bcycles\s*\+=\s*(\w+\s*\([^;]*\))\s*;", masked):
        out.append(("cycle_adj", m.start(1), m.end(1), "0"))
    return out


OPERATORS = (op_cir_initial, op_exec_state, op_compare, op_constants, op_logic_terms, op_not_remove,
             op_width, op_bool_const, op_operators, op_stmt_delete, op_if_cond, op_bool_return,
             op_return_delete, op_inline_block, op_ident_swap, op_cycle_adj, op_ternary_cond)

# ---------------------------------------------------------------------------------------------
# 対象範囲
# ---------------------------------------------------------------------------------------------

OPRTE_FORMAT9_LINE_RE = re.compile(r"format == 9|cp_|coproc_resume")


def function_regions(masked, path, names):
    """masked の中の関数 names の本体の範囲（FPU-4 の対象関数）。見つからなければ止める。"""
    spans = {name: (s, e) for s, e, name in function_spans(masked)}
    missing = [name for name in names if name not in spans]
    if missing:
        raise SystemExit(f"{path}: 関数が見つからない: {', '.join(missing)}")
    return [(*spans[name], None, ()) for name in names]


# 060（#107・#108）で ops_68060*.c 以外の対象にする関数
MC68060_FUNCTION_REGIONS = {
    "ops_system_ext.c": ("movec_060",),
    "disasm.c": ("dis_lineF_060",),
    # 060-D（#110）。040 と共有の cpu_040_cache_read/write の 060 分岐（rmw bypass の呼出し）は対象外（手動の変異で確認）
    "ops_68040_cache.c": ("cache_sets", "rmw_bypass", "op_cache060", "move16_cycles", "register_68060_cache_ops"),
    # 060-E（#111）。run_opmode の FINT/FINTRZ は 040 の経路を含むため対象外（手動の変異で確認）
    "ops_fpu.c": ("is_dynamic_list", "is_060_integer_round_opmode", "fpu060_classify", "fpu060_trap",
                        "exec_save060", "exec_restore060", "op_fpu060", "register_68060_fpu_ops"),
}

# 060 以外のモデルと共有する関数は、060 で足した文だけを対象にする（共有部分の既存の検出漏れは 060 の範囲外）。
# (開始の正規表現, 終端の文字列): 開始から終端の次までを 1 つの範囲にする。開始に当たる箇所すべてが対象。
MC68060_SNIPPETS = {
    "ops_68020_arith.c": ((r"if \(cpu_is_68060\(c\) && wide\)", "34);"),),
    "ops_68020_system.c": ((r"if \(cpu_is_68060\(c\)\) \{", "} else {"),),
    "ops_68010_system.c": ((r"if \(cpu_is_68060\(cpu\)\) \{\s+if \(movec_060", "goto illegal;"),
                           (r"cpu_is_68060\(cpu\) \? 34 :", ";")),
    "decode.c": ((r"!cpu_is_68060\(cpu\)", "cpu_is_68060(cpu)"),),
}
MC68060_SNIPPETS["ops_system_ext.c"] = MC68060_SNIPPETS.pop("ops_68010_system.c")
# 共有する関数のうち、060 で引数を足した行だけ（ea_helpers.c の locked）
MC68060_LINE_FILTERS = {"ea_helpers.c": (("ea_rmw_read", "ea_rmw_load"), r"locked")}

# 060-C（#109）で m68000.c の対象にする関数。全体を変異するもの、060 の行だけを変異するもの（pattern に合う行）
MC68060_M68000_FUNCTIONS = (
    "buscr_save_for_exception", "fpu_config_accepted", "group0_060_is_instruction_fetch",
    "group0_060_is_later_access", "group0_fault_address_68060", "group0_fslw_68060_mmu", "group0_fslw_68060",
    "deliver_group0_exception_68060",
)
MC68060_M68000_LINES = {
    "cpu_exception_frame_origin_complete": r"cpu_is_68060|irq_defer",
    "mc68k_reset": r"cpu_is_68060|mmu060|pcr|buscr|irq_defer|g0_no_rewind",
    "accept_irq": r"cpu_is_68060|buscr_save",
    "group0_ssw": r"68060|group0_fslw",
    "group0_frame_format": r"68060|0x4",
    "fault_ring_capture": r"68060|group0_fault_address",
    "cpu_dispatch_exception": r"buscr_save",
    "cpu_dispatch_fpu040_exception": r"buscr_save",
    "mc68k_create_model_with_fpu": r"68060",
    "dispatch_group0_exception": r"g0_no_rewind",
    "execute_body": r"irq_defer|irq_deferred|cpu_is_68060|g0_no_rewind|prefetch_address_error",
}

# FPU-4 で src/fpu 側の対象にする関数（E3 frame 用の中間結果 API。設計書 design_fpu_4.md 推論 21、#82）
FPU4_FUNCTION_REGIONS = {
    "m68000.c": ("cpu_dispatch_fpu040_exception",),
    "fpu_internal.c": ("mc68k_fpu_unrounded_reset", "mc68k_fpu_unrounded_get", "mc68k_fpu_unrounded_round_mantissa"),
    "s_roundPackToExtF80.c": ("mantissa_bits_of", "mc68k_unrounded_round_mantissa"),
}


def changed_lines(path, root=ROOT):
    """TABLEWALK_BASE から作業ツリーまでに追加・変更された行番号（作業ツリー側）の集合。"""
    out = subprocess.run(["git", "diff", "-U0", TABLEWALK_BASE, "--", path], cwd=root, capture_output=True,
                         text=True, check=True).stdout
    lines = set()
    for m in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", out, re.M):
        start, count = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
        lines.update(range(start, start + count))
    return lines


def regions_for(path, masked, target):
    """(start, end, 行フィルタ正規表現 or None, 追加の演算子) のリスト。"""
    n = len(masked)
    if target == "tablewalk":
        return [(0, n, None, OPERATORS + (op_assign_stmt_delete,))]
    if target == "mc68060":
        name = Path(path).name
        if name in ("ops_68060.c", "ops_68060_mmu.c"):
            return [(0, n, None, ())]
        if name == "ops_system.c":  # op_rte の 060 の分岐（040 の分岐の直前まで）
            start = masked.find("if (cpu_is_68060(cpu)) {", masked.find("static int op_rte"))
            end = masked.find("if (cpu_is_68040(cpu)) {", start)
            if min(start, end) < 0:
                raise SystemExit("op_rte() の 060 の分岐が見つからない")
            return [(start, end, None, ())]
        if name == "m68000.c":
            spans = {fn: (s, e) for s, e, fn in function_spans(masked)}
            regions = []
            for fn in MC68060_M68000_FUNCTIONS:
                if fn not in spans:
                    raise SystemExit(f"{path}: 関数が見つからない: {fn}")
                regions.append((*spans[fn], None, ()))
            for fn, pattern in MC68060_M68000_LINES.items():
                if fn not in spans:
                    raise SystemExit(f"{path}: 関数が見つからない: {fn}")
                regions.append((*spans[fn], re.compile(pattern), ()))
            return regions
        regions = []
        if name in MC68060_FUNCTION_REGIONS:
            regions += function_regions(masked, path, MC68060_FUNCTION_REGIONS[name])
        for start_re, end_text in MC68060_SNIPPETS.get(name, ()):
            found = list(re.finditer(start_re, masked))
            if not found:
                raise SystemExit(f"{path}: 060 の文が見つからない: {start_re}")
            for m in found:
                end = masked.find(end_text, m.start())
                if end < 0:
                    raise SystemExit(f"{path}: 終端が見つからない: {end_text}")
                regions.append((m.start(), end + len(end_text), None, ()))
        if name in MC68060_LINE_FILTERS:
            names, pattern = MC68060_LINE_FILTERS[name]
            spans = {fn: (s, e) for s, e, fn in function_spans(masked)}
            for fn in names:
                if fn not in spans:
                    raise SystemExit(f"{path}: 関数が見つからない: {fn}")
                regions.append((*spans[fn], re.compile(pattern), ()))
        return regions
    if target == "fpu4":
        if path.endswith("ops_fpu.c"):
            return [(0, n, None, ())]
        return function_regions(masked, path, FPU4_FUNCTION_REGIONS[Path(path).name])
    if path.endswith("coproc.c"):
        return [(0, n, None, ())]
    if path.endswith("m68000.c"):
        return function_regions(masked, path, ("cpu_dispatch_coproc_exception",))
    if path.endswith("ops_system.c"):
        start = masked.find("if (format == 0 || format == 2 || format == 9")
        end = masked.find("return cycles;", start)
        sub_s = masked.find("if (format == 9) {", start)
        sub_e = masked.find("int bytes", sub_s)
        if min(start, end, sub_s, sub_e) < 0:
            raise SystemExit("op_rte() の format 9 周辺が見つからない")
        return [(sub_s, sub_e, None, ()),
                (start, end, OPRTE_FORMAT9_LINE_RE, ()),
                (start, end, FORMAT9_SKIP_RE, (op_format9_skip,))]
    if path.endswith("decode.c"):  # コプロセッサ命令を登録する機種ゲートだけ
        # ゲートの条件式の書き換えに依らないよう、register_coproc_ops の呼出しから直前の if へ逆引きする
        call = masked.find("register_coproc_ops(")
        start = masked.rfind("if (", 0, call) if call >= 0 else -1
        end = masked.find(";", call) + 1 if call >= 0 else -1
        if start < 0 or end <= start:
            raise SystemExit("decode.c の register_coproc_ops() の機種ゲートが見つからない")
        return [(start, end, None, ())]
    raise SystemExit(f"対象範囲が未定義: {path}")


def generate(path, target, root=ROOT):
    text = (root / path).read_text()
    masked = mask_source(text)
    line_starts = [0] + [i + 1 for i, c in enumerate(text) if c == "\n"]
    spans = function_spans(masked)

    def line_of(off):
        return bisect.bisect_right(line_starts, off)

    def func_of(off):
        for s, e, name in spans:
            if s <= off < e:
                return name
        return "-"

    seen = set()
    mutants = []
    counters = {}
    only_lines = changed_lines(path, root) if target == "tablewalk" else None
    for r_start, r_end, line_re, extra_ops in regions_for(path, masked, target):
        for op in (extra_ops or OPERATORS):
            for name, s, e, repl in op(masked, text):
                if not (r_start <= s <= e <= r_end):
                    continue
                ln = line_of(s)
                if only_lines is not None and ln not in only_lines:
                    continue
                line_text = text[line_starts[ln - 1]:line_starts[ln] - 1 if ln < len(line_starts) else len(text)]
                if line_re is not None and not line_re.search(masked[line_starts[ln - 1]:line_starts[ln] - 1
                                                                      if ln < len(line_starts) else len(masked)]):
                    continue
                if masked[line_starts[ln - 1]:].lstrip().startswith(("#include", "#if", "#endif", "#else")):
                    continue
                key = (s, e, repl)
                if key in seen or text[s:e] == repl:
                    continue
                seen.add(key)
                k = counters.get((ln, name), 0)
                counters[(ln, name)] = k + 1
                mutants.append(Mutant(
                    mid=f"{Path(path).name}:{ln}:{name}:{k}", file=path, func=func_of(s), line=ln, op=name,
                    start=s, end=e, repl=repl, text=line_text.strip(),
                    desc=f"{text[s:e]!r} -> {repl!r}"))
    mutants.sort(key=lambda m: (m.file, m.start, m.op, m.repl))
    return mutants


# ---------------------------------------------------------------------------------------------
# 実行
# ---------------------------------------------------------------------------------------------

def run(cmd, cwd, **kw):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace", **kw)


class Worker:
    def __init__(self, workdir, index, tests, jobs_per_build, source_files):
        self.source_files = source_files
        self.root = workdir / f"w{index}"
        self.build = self.root / "build"
        self.tests = tests
        self.jobs = jobs_per_build

    def setup(self):
        self.root.mkdir(parents=True, exist_ok=True)
        for item in COPY_ITEMS:
            src, dst = ROOT / item, self.root / item
            if src.is_dir():
                run(["rsync", "-a", "--delete", "--exclude=.git", "--exclude=build*", f"{src}/", f"{dst}/"], ROOT)
            elif src.exists():
                shutil.copy2(src, dst)
        r = run(["cmake", "-S", str(self.root), "-B", str(self.build), "-DCMAKE_BUILD_TYPE=Debug"], self.root)
        if r.returncode != 0:
            sys.exit(f"cmake failed: {r.stderr[-400:]}")

    def _clean(self):
        for name in self.tests:
            for p in list(self.build.glob(f"tests/CMakeFiles/{name}.dir/**/*.o")) + [self.build / "tests" / name]:
                p.unlink(missing_ok=True)
        for lib in ("mc68000", "softfloat"):  # s_roundPackToExtF80.c は softfloat ライブラリ側
            for p in self.build.glob(f"CMakeFiles/{lib}.dir/**/*.o"):
                if any(f.endswith("/" + p.name[:-2]) for f in self.source_files):
                    p.unlink()
            (self.build / f"lib{lib}.a").unlink(missing_ok=True)

    def build_and_test(self, full=False):
        """full=False: 段 1（fpcp の 2 試験だけ）。full=True: 段 2（全ターゲットをビルドして ctest を流す）。"""
        self._clean()
        target = [] if full else ["--target", *self.tests]
        r = run(["cmake", "--build", str(self.build), *target, f"-j{self.jobs}"], self.root)
        if r.returncode != 0:
            return STATUS_BUILD_ERROR, (r.stderr or r.stdout).strip().splitlines()[-1:] and \
                (r.stderr or r.stdout).strip().splitlines()[-1][:160]
        if full:
            t = run(["ctest", "--test-dir", str(self.build), "-E", STAGE2_CTEST_EXCLUDE, "-j2", "--timeout", "120",
                     "--output-on-failure"], self.root)
            if t.returncode != 0:
                failed = [l.strip() for l in t.stdout.splitlines() if "***Failed" in l or "Timeout" in l or "***Exception" in l]
                return STATUS_KILLED_STAGE2, (failed[0][:120] if failed else "ctest failed")
            return STATUS_SURVIVED, ""
        for name in self.tests:
            try:
                t = run([str(self.build / "tests" / name)], self.root, timeout=TEST_TIMEOUT_SEC)
            except subprocess.TimeoutExpired:
                return STATUS_KILLED, f"{name}{STAGE1_TIMEOUT_MARK}"
            if t.returncode != 0:
                last = t.stderr.strip().splitlines()
                return STATUS_KILLED, f"{name}: {last[-1][:120] if last else t.returncode}"
        return STATUS_SURVIVED, ""

    def apply(self, m, full=False):
        f = self.root / m.file
        src = f.read_text()
        original = (ROOT / m.file).read_text()
        if src != original:
            f.write_text(original)
            src = original
        f.write_text(src[:m.start] + m.repl + src[m.end:])
        try:
            return self.build_and_test(full)
        finally:
            f.write_text(original)


def run_mutants(mutants, workdir, tests, jobs, source_files, full=False):
    if not mutants:
        return {}
    pool = queue.Queue()
    workers = [Worker(workdir, i, tests, 2, source_files) for i in range(jobs)]
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        list(ex.map(lambda w: w.setup(), workers))
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        baselines = list(ex.map(lambda w: w.build_and_test(full), workers))
    for w, (status, detail) in zip(workers, baselines):
        if status != STATUS_SURVIVED:
            sys.exit(f"baseline failed: {status} {detail}")
        pool.put(w)
    results = {}
    lock = threading.Lock()
    done = [0]

    def task(m):
        w = pool.get()
        try:
            status, detail = w.apply(m, full)
        finally:
            pool.put(w)
        with lock:
            done[0] += 1
            results[m.mid] = {"status": status, "detail": detail or ""}
            if status not in (STATUS_KILLED, STATUS_KILLED_STAGE2):
                print(f"[{done[0]}/{len(mutants)}] {status:11s} {m.mid} {m.desc} :: {m.text[:70]}", flush=True)
            elif done[0] % 50 == 0:
                print(f"[{done[0]}/{len(mutants)}] ...", flush=True)

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        list(ex.map(task, mutants))
    return results


# ---------------------------------------------------------------------------------------------
# 許可リストとの突き合わせ
# ---------------------------------------------------------------------------------------------

def entry_matches(entry, m):
    if entry["file"] != m.file:
        return False
    if entry.get("func") and entry["func"] != m.func:
        return False
    if entry.get("op") and entry["op"] != m.op:
        return False
    if entry.get("line") and entry["line"] not in m.text:
        return False
    if entry.get("desc") and entry["desc"] not in m.desc:
        return False
    return True


def classify(mutants, results, allowlist_module):
    """許可リストは先に書いた項目が優先（最初に当たった項目が生き残りを引き受ける）。"""
    allowlist = importlib.import_module(allowlist_module)
    ALLOWLIST, KINDS = allowlist.ALLOWLIST, allowlist.KINDS
    by_id = {m.mid: m for m in mutants}
    survivors = [by_id[mid] for mid, r in results.items() if r["status"] == STATUS_SURVIVED and mid in by_id]
    errors = []
    counts = [0] * len(ALLOWLIST)
    unclassified = []
    for m in survivors:
        for i, e in enumerate(ALLOWLIST):
            if entry_matches(e, m):
                counts[i] += 1
                break
        else:
            unclassified.append(m)
    for i, e in enumerate(ALLOWLIST):
        if e["kind"] not in KINDS or not e.get("reason"):
            errors.append(f"許可リスト #{i}: kind か reason が不正: {e}")
        if counts[i] != e["expect"]:
            errors.append(f"許可リスト #{i} ({e['file']} {e.get('func')} {e.get('op')} {e.get('line')!r}): "
                          f"生き残り {counts[i]} 件、expect={e['expect']}")
    return survivors, unclassified, [], errors


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", choices=sorted(TARGETS), default=DEFAULT_TARGET,
                    help="変異の対象（fpcp: FPU-3 の 68881/68882 コプロセッサインタフェース、fpu4: 68040 内蔵 FPU、tablewalk: Issue #100 の 68040 table walk、mc68060: MC68060-A/B）")
    ap.add_argument("--list", action="store_true", help="変異を一覧表示して終わる（ビルドしない）")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--workdir", type=Path, default=DEFAULT_WORKDIR)
    ap.add_argument("--tests", default=None, help="走らせる試験ターゲット（カンマ区切り。既定は --target の段 1 の試験）")
    ap.add_argument("--only", default="", help="mid の部分文字列（カンマ区切り、いずれかを含むものだけ）")
    ap.add_argument("--ids-file", type=Path,
                    help="mid を 1 行 1 件で書いたファイル（その変異だけを走らせる。試験を足した後の生き残りの再検査用）")
    ap.add_argument("--results", type=Path, help="結果の JSON を保存する")
    ap.add_argument("--stage2", action="store_true",
                    help="段 1 の生き残りだけを、全ターゲットのビルド＋ctest（diff 系を除く）で再検査する（--from-results と併用）")
    ap.add_argument("--no-stage2", action="store_true", help="段 2 を省く（段 1 の生き残りの再検査をしない。デバッグ用）")
    ap.add_argument("--from-results", type=Path, help="保存済みの結果で分類だけ行う")
    ap.add_argument("--cache-file", type=Path, help=f"判定のキャッシュ（既定 {DEFAULT_CACHE_DIR}/<target>.json）")
    ap.add_argument("--fresh", action="store_true",
                    help="キャッシュを読まず、全件を取り直す（節目の収束判定用。結果はキャッシュに書く）")
    ap.add_argument("--no-cache", action="store_true", help="キャッシュを読みも書きもしない")
    ap.add_argument("--seed-cache", type=Path, metavar="RESULTS",
                    help="直前の全件実行の結果 JSON（--results の出力）をキャッシュに取り込んで終わる。"
                         "結果 JSON の _meta が今のソース・試験・段 1 の試験と違う、または完了した実行でなければ拒否する")
    args = ap.parse_args()

    target = TARGETS[args.target]
    tests = tuple(args.tests.split(",")) if args.tests else target.tests
    mutants = [m for path in target.source_files for m in generate(path, args.target)]
    if args.only:
        keys = args.only.split(",")
        mutants = [m for m in mutants if any(k in m.mid for k in keys)]
    if args.ids_file:
        ids = set(args.ids_file.read_text().split())
        mutants = [m for m in mutants if m.mid in ids]
    if args.list:
        counts = {}
        for m in mutants:
            counts[(m.file, m.op)] = counts.get((m.file, m.op), 0) + 1
            print(f"{m.mid:44s} {m.func:34s} {m.desc:34s} {m.text[:60]}")
        print(f"\n{len(mutants)} mutants")
        for (f, op), n in sorted(counts.items()):
            print(f"  {f:22s} {op:18s} {n}")
        return 0

    cache_path = args.cache_file or DEFAULT_CACHE_DIR / f"{args.target}.json"
    use_cache = not args.no_cache
    envs = compute_envs()
    meta = {**envs, "stage1_tests": list(tests), "stage2": not args.no_stage2, "complete": False}
    originals = {path: (ROOT / path).read_text() for path in target.source_files}
    keys = {m.mid: mutant_key(m, originals) for m in mutants}

    if args.seed_cache:
        seeded, seeded_meta = read_results(args.seed_cache)
        if not seeded_meta or not seeded_meta.get("complete") or any(
                seeded_meta.get(k) != v for k, v in (*envs.items(), ("stage1_tests", list(tests)))):
            sys.exit("seed-cache: 結果 JSON が、完了した実行のものでない、または今のソース・試験・段 1 の試験と違う"
                     "（_meta を照合した）。取り込まない")
        fresh = {keys[mid]: (r["status"], r.get("detail", "")) for mid, r in seeded.items()
                 if mid in keys and r["status"] in CACHE_STATUSES
                 and not (r["status"] == STATUS_SURVIVED and not seeded_meta.get("stage2"))}
        cache = save_cache(cache_path, load_cache(cache_path), fresh, envs, tests)
        print(f"seed-cache: {len(fresh)} 件を取り込んだ（{cache_path}、計 {len(cache)} 件）")
        return 0

    cached_hits = 0
    if args.from_results:
        results, loaded_meta = read_results(args.from_results)
        if args.stage2:
            todo = [m for m in mutants if results.get(m.mid, {}).get("status") == STATUS_SURVIVED]
            print(f"stage2: {len(todo)} survivors")
            pending = {mid for mid, r in results.items() if r["status"] == STATUS_SURVIVED} - {m.mid for m in todo}
            results.update(run_mutants(todo, args.workdir, tests, args.jobs, target.source_files, full=True))
            if args.results:
                # --ids-file / --only で一部だけ段 2 にかけたとき、残りの SURVIVED は段 1 だけの判定。
                # 段 2 を済ませた（stage2 が真だった）結果でなければ、_meta を完了にしない（--seed-cache が取り込まない）
                done = bool(loaded_meta and loaded_meta.get("stage2")) or not pending
                if not done:
                    print(f"注: 段 1 だけの SURVIVED が {len(pending)} 件残るので、_meta は stage2 完了にしない")
                write_results(args.results, results,
                              {**loaded_meta, "stage2": done, "complete": done} if loaded_meta else None)
    else:
        cache = load_cache(cache_path) if (use_cache and not args.fresh) else {}
        results, misses = {}, []
        for m in mutants:
            hit = cache_lookup(cache.get(keys[m.mid]), envs, tests) if cache else None
            if hit:
                results[m.mid] = {"status": hit[0], "detail": hit[1], "cached": True}
            else:
                misses.append(m)
        cached_hits = len(mutants) - len(misses)
        if use_cache:
            print(f"cache: 命中 {cached_hits} 件（再検査しない）、実行 {len(misses)} 件")
        fresh_results = run_mutants(misses, args.workdir, tests, args.jobs, target.source_files)
        results.update(fresh_results)
        if args.results:  # 段 2 の途中で落ちても段 1 を失わない
            write_results(args.results, results, meta)
        todo = [m for m in misses if fresh_results[m.mid]["status"] == STATUS_SURVIVED]
        if todo and not args.no_stage2:
            print(f"stage2: {len(todo)} survivors")
            stage2 = run_mutants(todo, args.workdir, tests, args.jobs, target.source_files, full=True)
            results.update(stage2)
            fresh_results.update(stage2)
        if args.results:
            write_results(args.results, results, {**meta, "complete": True})
        if use_cache:
            # 段 2 を省いた実行の SURVIVED は、まだ最終ではないので書かない
            final = {keys[mid]: (r["status"], r["detail"]) for mid, r in fresh_results.items()
                     if r["status"] in CACHE_STATUSES and not (args.no_stage2 and r["status"] == STATUS_SURVIVED)}
            save_cache(cache_path, load_cache(cache_path), final, envs, tests)

    counts = {}
    for r in results.values():
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    print("\n" + ", ".join(f"{k}: {v}" for k, v in sorted(counts.items())))
    n_loop = sum(1 for r in results.values() if r["status"] == STATUS_KILLED and STAGE1_TIMEOUT_MARK in r.get("detail", ""))
    n_ctest_to = sum(1 for r in results.values() if is_timeout_kill(r["status"], r.get("detail")))
    print(f"timeout の検出: 段 1（無限ループ）{n_loop} 件、段 2（ctest。スリープの偽の可能性あり）{n_ctest_to} 件")
    survivors, unclassified, doubly, errors = classify(mutants, results, target.allowlist)
    print(f"survivors: {len(survivors)} (許可リスト内 {len(survivors) - len(unclassified)}, 未分類 {len(unclassified)})")
    for m in unclassified:
        print(f"UNCLASSIFIED {m.mid} [{m.func}] {m.desc} :: {m.text[:80]}")
    for mid in doubly:
        print(f"DUPLICATE-ENTRY {mid}")
    if args.only or args.no_stage2:
        print("注: --only / --no-stage2 では許可リストの expect が一致しないため、収束の判定にならない")
        errors = []
    elif cached_hits:
        print(f"注: 暫定。{cached_hits} 件はキャッシュの判定（殺した試験の弱体化・削除は検出しない）。"
              "節目の収束判定は --fresh で全件を取り直すこと")
    for e in errors:
        print("ALLOWLIST-ERROR", e)
    return 1 if (unclassified or doubly or errors) else 0


if __name__ == "__main__":
    sys.exit(main())
