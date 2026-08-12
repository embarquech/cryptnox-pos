/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/*
 * test_ota_version.cpp — host unit test for the version comparison that decides
 * whether the panel presents an uploaded firmware image as an update or as a
 * downgrade (ota_version.h).
 *
 * It matters because the wrong answer is silent: an image that goes backwards,
 * announced as "New firmware", is how a terminal gets talked onto a build with
 * a known fault. The signature check does not help — a genuine older release is
 * signed exactly as well as the current one.
 *
 * Header-only unit, so this includes it directly. Build & run from the repo root:
 *
 *   g++ -std=c++14 -Imain tests/units/test_ota_version.cpp -o test_ota_version \
 *       && ./test_ota_version
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ota_version.h"

int main(void)
{
    /* Ordinary ordering, component by component. */
    assert(ota_version_cmp("1.0.0", "1.0.1") < 0);
    assert(ota_version_cmp("1.0.1", "1.0.0") > 0);
    assert(ota_version_cmp("1.2.0", "1.10.0") < 0);   /* not string order */
    assert(ota_version_cmp("2.0.0", "1.99.99") > 0);
    assert(ota_version_cmp("1.0.0", "1.0.0") == 0);

    /* Numeric, not lexicographic — the case that catches a naive strcmp. */
    assert(ota_version_cmp("9.0.0", "10.0.0") < 0);

    /* A leading v is noise: `git describe` emits it, version.txt does not. */
    assert(ota_version_cmp("v1.2.3", "1.2.3") == 0);
    assert(ota_version_cmp("V1.2.4", "v1.2.3") > 0);

    /* Absent components are zero, so these name the same release. */
    assert(ota_version_cmp("1.2", "1.2.0") == 0);
    assert(ota_version_cmp("1", "1.0.0.0") == 0);
    assert(ota_version_cmp("1.2", "1.2.1") < 0);

    /* Suffixes are ignored rather than ordered — documented in the header, and
     * the reason the panel asks a human instead of deciding by itself. */
    assert(ota_version_cmp("1.2.3-rc1", "1.2.3") == 0);
    assert(ota_version_cmp("1.2.3+deadbeef", "1.2.3-dirty") == 0);
    assert(ota_version_cmp("1.2.4-rc1", "1.2.3") > 0);

    /* Fourth component still counts; a fifth is beyond what is compared. */
    assert(ota_version_cmp("1.2.3.4", "1.2.3.5") < 0);
    assert(ota_version_cmp("1.2.3.4.9", "1.2.3.4.1") == 0);

    /* Garbage must not read as newer than a real release — an image whose
     * header could not be parsed has to lose, so the panel warns. */
    assert(ota_version_cmp("", "1.0.0") < 0);
    assert(ota_version_cmp("?", "1.0.0") < 0);
    assert(ota_version_cmp(NULL, "1.0.0") < 0);
    assert(ota_version_cmp("...", "0.0.1") < 0);
    assert(ota_version_cmp(".9", "0.0.0") == 0);   /* leading dot ends the parse */
    assert(ota_version_cmp("", "") == 0);
    assert(ota_version_cmp(NULL, NULL) == 0);

    /* A component long enough to overflow an unsigned long clamps high instead
     * of wrapping to something small. It must not compare as "older". */
    assert(ota_version_cmp("99999999999999999999.0.0", "1.0.0") > 0);

    /* Nothing past the 32nd byte is looked at: esp_app_desc_t::version is a
     * fixed 32-byte field, so a version that fills it carries no terminator to
     * stop at. Pinned from both sides of the boundary — the digit inside it
     * counts, the one immediately outside it does not. */
    const char *at_limit   = "1.2.3.00000000000000000000000009";
    const char *past_limit = "1.2.3.000000000000000000000000009";
    assert(strlen(at_limit)   == OTA_VERSION_MAX);
    assert(strlen(past_limit) == OTA_VERSION_MAX + 1U);
    assert(ota_version_cmp(at_limit,   "1.2.3") > 0);
    assert(ota_version_cmp(past_limit, "1.2.3") == 0);

    printf("test_ota_version: all assertions passed\n");
    return 0;
}
