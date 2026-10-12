#!/usr/bin/env python3
"""Real writer output must match the schemas Orange publishes.

Generates a synthetic recording folder with the production writers
(synthetic_recording_bundle) and validates every root document in it with
scripts/validate_output_schemas.py. Nested reference objects are reported, not failed.

  output_schema_validation_tests.py <path to synthetic_recording_bundle>
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
VALIDATOR = REPO / "scripts" / "validate_output_schemas.py"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    try:
        import jsonschema  # noqa: F401
    except ImportError:
        print("output_schema_validation_tests: python3 needs the jsonschema package")
        return 1
    binary = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "bundle"
        generated = subprocess.run(
            [str(binary), "--out", str(out), "--fixture-rig", str(Path(tmp) / "rig"),
             "--recording-id", "bundle", "--camera", "2010093", "--camera", "2010094",
             "--intent", "stimulus_experiment", "--recording-subtype", "omit",
             "--binding-mode", "required", "--request-version", "2", "--manifest"],
            capture_output=True, text=True, check=False)
        if generated.returncode != 0:
            print(generated.stdout[-2000:], generated.stderr[-2000:])
            return 1
        result = subprocess.run([sys.executable, str(VALIDATOR), str(out)],
                                capture_output=True, text=True, check=False)
        print(result.stdout)
        if result.returncode != 0 or " 0 documents validated" in result.stdout:
            print("output_schema_validation_tests: failed")
            return 1
    print("output_schema_validation_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
