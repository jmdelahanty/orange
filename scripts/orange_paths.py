"""The person's Orange data root (~/orange_data of whoever started Orange).

Python twin of src/orange_data_root.h (docs/opt_install_plan_2026-11-14.md,
step c); keep the two in step:

  1. ORANGE_DATA_ROOT, when set: an override for tests.
  2. Under sudo (SUDO_USER set): that user's home from the passwd database.
  3. Running as root without SUDO_USER: refuse (root's home is never a data root).
  4. Otherwise: the real uid's home from the passwd database (not $HOME).
"""

from __future__ import annotations

import os
import pwd
from pathlib import Path


class DataRootError(RuntimeError):
    pass


def orange_data_root() -> Path:
    override = os.environ.get("ORANGE_DATA_ROOT", "")
    if override:
        return Path(override)
    sudo_user = os.environ.get("SUDO_USER", "")
    if sudo_user:
        try:
            return Path(pwd.getpwnam(sudo_user).pw_dir) / "orange_data"
        except KeyError as error:
            raise DataRootError(f"cannot resolve the home of SUDO_USER {sudo_user!r}") from error
    if os.getuid() == 0:
        raise DataRootError("running as root without SUDO_USER: refusing to use root's home "
                            "as the Orange data root")
    try:
        return Path(pwd.getpwuid(os.getuid()).pw_dir) / "orange_data"
    except KeyError as error:
        raise DataRootError(f"cannot resolve the home of uid {os.getuid()}") from error


def orange_wrapper(name: str) -> Path:
    """The sudo wrapper that runs the binaries belonging with these scripts.

    Same rule as scripts/orange_bin_dir.sh (docs/opt_install_plan_2026-11-14.md,
    P3): scripts inside an installed release (<release>/share/orange/scripts,
    the release holding INSTALLED.json and bin/) use /usr/local/bin/orange-<name>,
    which runs installed releases only; a source tree's scripts use
    /usr/local/bin/orange-dev-<name>, which runs the development tree only.
    """
    release = Path(__file__).resolve().parent.parents[2]
    if (release / "INSTALLED.json").is_file() and (release / "bin").is_dir():
        return Path(f"/usr/local/bin/orange-{name}")
    return Path(f"/usr/local/bin/orange-dev-{name}")
