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
