#!/usr/bin/env python3
"""The image-levels writers' documents of every status validate against their published schemas.

  image_levels_schema_tests.py <image_levels_tests binary>
"""

import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="image_levels_schema_") as tmp:
        subprocess.run([sys.argv[1], tmp], check=True)
        if len(list(Path(tmp).glob("*.json"))) != 3:
            print("image_levels_schema_tests: expected 3 sample documents", file=sys.stderr)
            return 1
        result = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "validate_output_schemas.py"), tmp],
            capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        if result.returncode != 0 or "orange.recording.image_levels_check v2" not in result.stdout:
            return 1
    print("image_levels_schema_tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
