#!/usr/bin/env python3
"""mc68000 の CycloneDX 1.6 SBOM (JSON) を生成する。

使い方: python3 scripts/gen_sbom.py [出力パス]
  既定の出力: build/mc68000.cdx.json

コンポーネントの構成 (git submodule の commit hash は git から取得):
  - extern/berkeley-softfloat-3: FPU 共通コアが使う (配布物に含まれる)
  - tests/diff/musashi: 差分テスト専用 (scope=excluded、配布物に含まれない)
"""

import datetime
import json
import subprocess
import sys
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO_ROOT / "build" / "mc68000.cdx.json"
SPEC_VERSION = "1.6"
SCOPE_REQUIRED = "required"
SCOPE_EXCLUDED = "excluded"

SUBMODULES = [
    {
        "path": "extern/berkeley-softfloat-3",
        "name": "berkeley-softfloat-3",
        "version_label": "3e",
        "url": "https://github.com/ucb-bar/berkeley-softfloat-3",
        "license": "BSD-3-Clause",
        "scope": SCOPE_REQUIRED,
        "description": "FPU common core (softfloat_target/ overrides s_roundPackToExtF80.c)",
    },
    {
        "path": "tests/diff/musashi",
        "name": "musashi",
        "version_label": None,
        "url": "https://github.com/kstenerud/musashi",
        "license": "MIT",
        "scope": SCOPE_EXCLUDED,
        "description": "Reference 68k emulator for differential tests only",
    },
]


def run_git(*args, cwd=REPO_ROOT):
    return subprocess.run(
        ["git", *args], cwd=cwd, check=True, capture_output=True, text=True
    ).stdout.strip()


def submodule_component(entry):
    commit = run_git("rev-parse", "HEAD", cwd=REPO_ROOT / entry["path"])
    component = {
        "type": "library",
        "bom-ref": f"{entry['name']}@{commit}",
        "name": entry["name"],
        "version": entry["version_label"] or commit,
        "scope": entry["scope"],
        "description": entry["description"],
        "licenses": [{"license": {"id": entry["license"]}}],
        "externalReferences": [{"type": "vcs", "url": entry["url"]}],
        "properties": [{"name": "git:commit", "value": commit}],
    }
    return component


def build_sbom():
    root_commit = run_git("rev-parse", "HEAD")
    root = {
        "type": "library",
        "bom-ref": "mc68000",
        "name": "mc68000",
        "version": root_commit,
        "description": "MC68000 family CPU core",
        "externalReferences": [
            {"type": "vcs", "url": "https://github.com/TerukiYamaguchi/cmc68000"}
        ],
    }
    components = [submodule_component(e) for e in SUBMODULES]
    dependencies = [
        {"ref": root["bom-ref"], "dependsOn": [c["bom-ref"] for c in components]}
    ]
    return {
        "bomFormat": "CycloneDX",
        "specVersion": SPEC_VERSION,
        "serialNumber": f"urn:uuid:{uuid.uuid4()}",
        "version": 1,
        "metadata": {
            "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(
                timespec="seconds"
            ),
            "tools": {"components": [{
                "type": "application",
                "name": "gen_sbom.py",
                "description": "scripts/gen_sbom.py",
            }]},
            "component": root,
        },
        "components": components,
        "dependencies": dependencies,
    }


def main():
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUTPUT
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(build_sbom(), indent=2, ensure_ascii=False) + "\n")
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
