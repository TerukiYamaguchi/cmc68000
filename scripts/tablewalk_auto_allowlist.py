"""fpcp_auto_mutants.py --target tablewalk の許可リスト（生き残りを許す変異。理由つき）。

形式・kind・expect の意味は fpcp_auto_allowlist.py と同じ。対象は Issue #100（68040 の table walk と D-cache）で
変えた行: src/cpu/ops_68040_cache.c、src/cpu/ops_68040_mmu.c。
"""
KINDS = ("equivalent", "observable", "backlog")

ALLOWLIST = [
    {"file": 'src/cpu/ops_68040_cache.c', "func": 'cpu_040_table_read', "line": 'physical & 15u', "kind": "equivalent", "expect": 2, "reason": 'descriptor の物理アドレスは longword 境界（walk の table・index・間接 descriptor のアドレスは 4 の倍数）なので、line 内オフセットのマスクの下位 2 bit（15u → 14u・13u）は結果を変えない'},
    {"file": 'src/cpu/ops_68040_cache.c', "func": 'cpu_040_table_write', "line": 'physical & 15u', "kind": "equivalent", "expect": 2, "reason": '同上（longword 境界のアドレスでは下位 2 bit のマスクが効かない）'},
    {"file": 'src/cpu/ops_68040_mmu.c', "func": 'cpu_mmu040_translate_result', "line": 'bool wp = false, wp_fault = false; uint32_t d = 0, da = 0;', "kind": "equivalent", "expect": 2, "reason": 'd と da の初期値は使われない（level ごとに read の直前に da を、read の成功後に d を代入してから読む）'},
    {"file": 'src/cpu/ops_68040_mmu.c', "func": 'cpu_mmu040_translate_result', "line": 'bool u_pending = false; uint32_t u_addr = 0, u_value = 0;', "kind": "equivalent", "expect": 2, "reason": 'u_addr と u_value の初期値は使われない（u_pending を立てるときに必ず代入し、u_pending のときだけ読む）'},
    {"file": 'src/cpu/ops_68040_mmu.c', "func": 'cpu_mmu040_translate_result', "line": 'uint32_t nd = d | DESC_U | (set_m ? DESC_M : 0);', "kind": "equivalent", "expect": 1, "reason": 'M を立てない側の `0` を `1` にしても、leaf の d の type は 1 か 3（それ以外はここへ来る前に fault か間接の解決）なので bit 0 が既に立っており、nd は同じ'},
    {"file": 'src/cpu/ops_68040_mmu.c', "func": 'cpu_mmu040_translate_result', "line": 'const bool rmw = !(d & DESC_U)', "kind": "equivalent", "expect": 1, "reason": '`!(d & DESC_U)` を真にしても、U=1 でここへ来るのは set_m かつ M=0 のときだけで、そのとき `(!set_m || (d & DESC_M))` が偽なので rmw は同じ'},
]
