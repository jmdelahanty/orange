"""Expected Orange rig-ledger results, computed by pancakebatter's rig_events.py (the owner's logic).

usage: python3 generate_expected.py <pancakebatter checkout>
Writes ledgers/<name>.expected.json for every ledgers/<name>.events.yml: active_conditions (rig_events
.active_conditions, no arena filter) and attestations (attested events of rig_events._timeline, with the
fields Orange seals). Re-run when rig_events.py changes and commit the outputs with the pancakebatter commit.
"""
import json
import subprocess
import sys
from pathlib import Path

repo = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(repo))
import yaml  # noqa: E402
import rig_events  # noqa: E402

commit = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
here = Path(__file__).resolve().parent / "ledgers"
for path in sorted(here.glob("*.events.yml")):
    events = (yaml.safe_load(path.read_text()) or {}).get("events") or []
    out = {
        "generated_by": f"pancakebatter rig_events.py @ {commit}",
        "active_conditions": rig_events.active_conditions(events),
        "attestations": [{k: ev.get(k) for k in ("event_id", "fields", "rig_geometry_revision",
                                                  "orientation_sha256", "scene_id")}
                         for ev in rig_events._timeline(events) if ev.get("kind") == "attested"],
    }
    target = path.with_name(path.name.replace(".events.yml", ".expected.json"))
    target.write_text(json.dumps(out, indent=2, sort_keys=True, default=str) + "\n")
    print(target.name, len(out["active_conditions"]), "conditions", len(out["attestations"]), "attestations")
