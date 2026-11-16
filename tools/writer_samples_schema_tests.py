#!/usr/bin/env python3
"""A writer test binary's sample documents pass scripts/validate_output_schemas.py.

Runs <command...> <tmpdir> (the binary writes its samples there), then validates the folder:
schemas and the digest_form_v1 check. Each --expect names a "<schema_id> v<version>" that must
have been validated (a published schema found the documents).

  writer_samples_schema_tests.py --expect "orange.recording.rig_config v3" -- \\
      rig_config_pin_tests tools/fixtures/rig_config --samples
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument("command", nargs="+")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="writer_samples_schema_") as tmp:
        subprocess.run([*args.command, tmp], check=True)
        if not list(Path(tmp).glob("*.json")):
            print("writer_samples_schema_tests: the writer wrote no samples", file=sys.stderr)
            return 1
        result = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "validate_output_schemas.py"), "--strict-nested", tmp],
            capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        missing = [e for e in args.expect if f"  {e}\n" not in result.stdout]
        if result.returncode != 0 or missing:
            print(f"writer_samples_schema_tests: FAIL (not validated: {missing})", file=sys.stderr)
            return 1
    print("writer_samples_schema_tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
