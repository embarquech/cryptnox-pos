/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file ota_version.h
 * @ingroup device
 * @brief Dotted version comparison, for deciding whether an update goes
 *        forwards or backwards.
 *
 * Header-only and free of ESP-IDF dependencies on purpose, the same reason
 * form_parse.h is: the answer decides what the panel tells the operator before
 * they replace the firmware on a terminal that signs transactions, and a
 * downgrade that gets announced as an upgrade is the failure that matters. Kept
 * where a host test can reach it (tests/units/test_ota_version.cpp).
 *
 * Deliberately NOT semver. Build metadata, pre-release tags and `git describe`
 * suffixes are ignored rather than ordered — "1.2.3-rc1" compares equal to
 * "1.2.3". Ordering release candidates is not a decision this device needs to
 * make, and the operator confirms every install on the panel regardless.
 */

#ifndef OTA_VERSION_H
#define OTA_VERSION_H

#include <stdbool.h>
#include <stddef.h>

/** @brief Numeric components compared; the rest of the string is ignored. */
#define OTA_VERSION_PARTS  4U

/** @brief Longest version string looked at, matching esp_app_desc_t::version. */
#define OTA_VERSION_MAX    32U

/**
 * @brief Read up to @ref OTA_VERSION_PARTS dotted numbers out of @p s.
 *
 * Skips one optional leading 'v'/'V'. Stops at the first character that is
 * neither a digit nor a dot, so "1.2.3-rc1" and "1.2.3+deadbeef" both yield
 * {1,2,3,0}. Absent components read as 0, which is what makes "1.2" and
 * "1.2.0" compare equal.
 *
 * @param[in]  s   Version string, NUL-terminated.
 * @param[out] out Array of @ref OTA_VERSION_PARTS components.
 */
static inline void ota_version_parse(const char *s,
                                     unsigned long out[OTA_VERSION_PARTS])
{
    for (size_t i = 0U; i < OTA_VERSION_PARTS; i++) { out[i] = 0UL; }
    if (s == NULL) { return; }

    size_t i = 0U;
    if ((s[0] == 'v') || (s[0] == 'V')) { i = 1U; }

    size_t part = 0U;
    bool   any  = false;   /* digits seen in the current component */
    for (; (i < OTA_VERSION_MAX) && (s[i] != '\0'); i++) {
        const char c = s[i];
        if ((c >= '0') && (c <= '9')) {
            /* Clamp rather than wrap: a component this large is junk, and
             * "compares as very large" beats "compares as whatever the
             * overflow happened to leave behind". */
            if (out[part] < 100000000UL) {
                out[part] = (out[part] * 10UL) + (unsigned long)(c - '0');
            }
            any = true;
        } else if (c == '.') {
            /* A dot with no digits before it ends the number entirely — ".."
             * and a leading dot are malformed, not zero-valued components. */
            if (!any) { break; }
            part++;
            any = false;
            if (part >= OTA_VERSION_PARTS) { break; }
        } else {
            break;   /* pre-release or build suffix: ignored */
        }
    }
}

/**
 * @brief Order two version strings.
 *
 * @param[in] a Left version.
 * @param[in] b Right version.
 * @return <0 if @p a is older than @p b, 0 if they name the same release,
 *         >0 if @p a is newer.
 */
static inline int ota_version_cmp(const char *a, const char *b)
{
    unsigned long va[OTA_VERSION_PARTS];
    unsigned long vb[OTA_VERSION_PARTS];
    ota_version_parse(a, va);
    ota_version_parse(b, vb);

    for (size_t i = 0U; i < OTA_VERSION_PARTS; i++) {
        if (va[i] != vb[i]) { return (va[i] < vb[i]) ? -1 : 1; }
    }
    return 0;
}

#endif /* OTA_VERSION_H */
