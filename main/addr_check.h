/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file addr_check.h
 * @ingroup device
 * @brief Structural address checks for the config portal.
 *
 * Header-only and free of ESP-IDF dependencies, for the same reason
 * form_parse.h is: this is the code that judges a string a stranger on the
 * network claims is where the money should go, so it is kept where a host-side
 * test can reach it (tests/units/test_addr_check.cpp).
 *
 * The Ethereum side is NOT here — eth_addr_parse() already verifies the EIP-55
 * checksum, which is a real check and not a structural one. Tron gets structural
 * only: the authoritative base58check decode needs a crypto provider, which lives
 * in the main task. See the note on addr_plausible() in provision.cpp.
 */

#ifndef ADDR_CHECK_H
#define ADDR_CHECK_H

#include <stddef.h>
#include <string.h>

/** @brief Length of a base58 Tron address, without the NUL. */
#define ADDR_TRON_LEN  34U

/** @brief true if every character of @p s is in the Bitcoin/Tron base58 set. */
static inline bool addr_is_base58(const char *s)
{
    static const char *const B58 =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if (s == NULL) { return false; }
    /* An empty string has no character outside the alphabet, which would make a
     * plain loop answer "yes". Callers check the length too, but this is the
     * function whose name promises the answer, so it answers correctly. */
    if (*s == '\0') { return false; }
    for (const char *p = s; *p != '\0'; p++) {
        if (strchr(B58, *p) == NULL) { return false; }
    }
    return true;
}

/**
 * @brief Structural check on a Tron address: 34 characters, 'T', base58.
 *
 * Deliberately not the checksum — see the file comment. A value that passes here
 * is still decoded (and can still be refused) on the main task before it is used.
 */
static inline bool addr_tron_plausible(const char *s)
{
    return (s != NULL) && (strlen(s) == ADDR_TRON_LEN) && (s[0] == 'T') &&
           addr_is_base58(s);
}

#endif /* ADDR_CHECK_H */
