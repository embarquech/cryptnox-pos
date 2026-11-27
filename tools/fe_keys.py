# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (c) 2026 Cryptnox SA
"""Per-board flash-encryption key files.

One key per board, named by its factory MAC:
    secure_keys/flash_encryption_key_<mac>.bin
so a key that leaks decrypts one unit's flash, not every unit ever provisioned.
tools/secure_provision.py creates it; tools/secure_flash.py finds it again by
asking the board for its MAC. The older single shared file
(secure_keys/flash_encryption_key.bin) is still honoured as a fallback for
boards provisioned before this existed.
"""

import glob
import os
import re
import subprocess
import sys

# Keys belong offline, not in the checkout: point CRYPTNOX_KEY_DIR at them.
# ponytail: falls back to the old in-repo folder so existing setups still flash;
# the release build (scripts/idf-release.bat) refuses to run without the variable.
KEY_DIR = os.environ.get("CRYPTNOX_KEY_DIR", "secure_keys")
if "CRYPTNOX_KEY_DIR" not in os.environ:
    print("warning: CRYPTNOX_KEY_DIR not set - using keys in ./secure_keys",
          file=sys.stderr)


def board_mac(port):
    """The board's factory MAC as 12 lower-case hex digits."""
    out = subprocess.check_output(
        [sys.executable, "-m", "esptool", "--port", port, "read_mac"],
        text=True, stderr=subprocess.STDOUT)
    m = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out)
    if not m:
        sys.exit("Could not read the board's MAC on %s:\n%s" % (port, out[-400:]))
    return m.group(1).replace(":", "").lower()


def board_key_path(mac):
    return os.path.join(KEY_DIR, "flash_encryption_key_%s.bin" % mac)


def shared_key_path():
    """The single key boards provisioned before per-board keys share."""
    hits = sorted(glob.glob(os.path.join(KEY_DIR, "flash_encryption_key.*")))
    return hits[0] if hits else os.path.join(KEY_DIR, "flash_encryption_key.bin")
