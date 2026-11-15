#!/usr/bin/env python3
"""The empty-background writers' documents validate against their published schemas (jsonschema).

Runs empty_background_tests with a sample folder (a capture descriptor written by WriteCapture and
start snapshots holding every block shape BindToRecording built) and validates the folder with
scripts/validate_output_schemas.py (the descriptor by schema_id, session.empty_background through
published_schemas.EMBEDDED).

  empty_background_schema_tests.py <empty_background_tests binary>
"""

import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SNAPSHOTS = 5


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="empty_background_schema_") as tmp:
        subprocess.run([sys.argv[1], tmp], check=True, stdout=subprocess.DEVNULL)
        if not (Path(tmp) / "background.json").is_file() or len(list(Path(tmp).glob("recording_snapshot_*.json"))) != SNAPSHOTS:
            print("empty_background_schema_tests: expected background.json and "
                  f"{SNAPSHOTS} recording_snapshot_*.json samples", file=sys.stderr)
            return 1
        result = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "validate_output_schemas.py"), tmp],
            capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        if (result.returncode != 0 or "orange.empty_background_capture v1" not in result.stdout
                or f"validated      {SNAPSHOTS}  orange_recording_empty_background_v1.schema.json" not in result.stdout):
            return 1
    print("empty_background_schema_tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
