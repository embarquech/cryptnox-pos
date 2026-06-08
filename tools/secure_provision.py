#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (c) 2026 Cryptnox SA
"""One-shot factory provisioning of a FRESH board into the full secure state:
Flash Encryption + encrypted NVS + Secure Boot v2.

Per board, in one command:
  1. sanity-check the board is fresh (aborts otherwise),
  2. burn the host flash-encryption key into eFuse BLOCK1,
  3. enable Flash Encryption (FLASH_CRYPT_CONFIG=0xF, FLASH_CRYPT_CNT),
  4. flash the signed + pre-encrypted images (bootloader, table, app) via
     tools/secure_flash.py.
The Secure Boot public-key digest + ABS_DONE_1 are then burned automatically by
the bootloader on the first boot (no risky manual digest burn here).

============================  IRREVERSIBLE  ============================
This burns eFuses. A misconfigured board is bricked. Validate on a
sacrificial unit first. Requires --yes to actually burn anything.
=======================================================================

Prerequisites (generate ONCE per product/batch, keep OFFLINE):
  espsecure.py generate_flash_encryption_key --keylen 256 secure_keys/flash_encryption_key.bin
  espsecure.py generate_signing_key --version 2 secure_keys/secure_boot_signing_key.pem
and a build produced with the secure overlay:
  idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.flash_encryption" build

Usage:
  python tools/secure_provision.py --port COM12 --baud 921600 --yes
"""

import argparse
import glob
import json
import os
import subprocess
import sys

FLASH_CRYPT_CONF = "0xF"          # classic-ESP32 default key-tweak config


def find_key():
    hits = sorted(glob.glob("secure_keys/flash_encryption_key.*"))
    return hits[0] if hits else "secure_keys/flash_encryption_key.bin"


def efuse(port, *args):
    return [sys.executable, "-m", "espefuse", "--port", port, *args]


def fresh_check(port):
    """Abort unless every secure eFuse is still in its virgin state."""
    out = subprocess.check_output(
        efuse(port, "summary", "--format", "json"), text=True)
    fuses = json.loads(out)

    def val(name):
        return fuses.get(name, {}).get("value")

    cnt = val("FLASH_CRYPT_CNT")
    abs1 = val("ABS_DONE_1")
    if cnt not in (0, "0", None) and cnt != 0:
        sys.exit("ABORT: FLASH_CRYPT_CNT=%r — board already has Flash Encryption." % cnt)
    if abs1 not in (False, 0, "0", None):
        sys.exit("ABORT: ABS_DONE_1=%r — board already has Secure Boot." % abs1)
    print("Board is fresh (FLASH_CRYPT_CNT=0, ABS_DONE_1=0).")


def main():
    ap = argparse.ArgumentParser(description="Factory-provision a fresh board (IRREVERSIBLE).")
    ap.add_argument("--port", required=True)
    ap.add_argument("--key", default=find_key(), help="Flash-encryption key file")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--yes", action="store_true",
                    help="Confirm the IRREVERSIBLE eFuse burns (required)")
    args = ap.parse_args()

    if not os.path.isfile(args.key):
        sys.exit("Key not found: %s" % args.key)
    if not os.path.isfile(os.path.join(args.build_dir, "flasher_args.json")):
        sys.exit("No build/flasher_args.json — build with the secure overlay first.")

    print("== 0. Verify the board is fresh ==")
    fresh_check(args.port)

    if not args.yes:
        print("\nDry run (no --yes): would now burn the FE key, enable Flash "
              "Encryption, and flash the signed+encrypted image.\n"
              "Re-run with --yes to perform the IRREVERSIBLE provisioning.")
        return

    print("\n== 1. Burn flash-encryption key (BLOCK1) ==")
    subprocess.check_call(efuse(args.port, "--do-not-confirm",
                                "burn_key", "flash_encryption", args.key))

    print("\n== 2. Enable Flash Encryption (FLASH_CRYPT_CONFIG + FLASH_CRYPT_CNT) ==")
    subprocess.check_call(efuse(args.port, "--do-not-confirm",
                                "burn_efuse", "FLASH_CRYPT_CONFIG", FLASH_CRYPT_CONF))
    subprocess.check_call(efuse(args.port, "--do-not-confirm",
                                "burn_efuse", "FLASH_CRYPT_CNT", "1"))

    print("\n== 3. Flash signed + pre-encrypted images (bootloader, table, app) ==")
    subprocess.check_call([
        sys.executable, "tools/secure_flash.py",
        "--port", args.port, "--baud", str(args.baud),
        "--key", args.key, "--build-dir", args.build_dir,
    ])

    print("\nProvisioning written. RESET the board now: the bootloader finalizes "
          "Secure Boot on first boot (burns the key digest + ABS_DONE_1).\n"
          "Verify after:  espefuse.py --port %s summary | findstr ABS_DONE_1" % args.port)


if __name__ == "__main__":
    main()
