/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_addr.cpp
 * @brief Implementation of the hex Ethereum-address parser.
 */

#include "eth_addr.h"
#include "keccak256.h"

#include <string.h>

/**
 * @brief Decode a single ASCII hex digit.
 *
 * @param[in] c Character to decode.
 * @return Nibble value 0–15, or -1 if @p c is not in [0-9a-fA-F].
 */
static int hex_nibble_val(char c)
{
    if ((c >= '0') && (c <= '9')) { return c - '0'; }
    if ((c >= 'a') && (c <= 'f')) { return (c - 'a') + 10; }
    if ((c >= 'A') && (c <= 'F')) { return (c - 'A') + 10; }
    return -1;
}

bool eth_addr_parse(const char *hex, uint8_t out[20])
{
    const char *p = hex;
    if ((p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X'))) {
        p += 2;
    }
    if (strlen(p) != 40U) {
        return false;
    }
    (void)memset(out, 0, 20U);
    size_t i;
    for (i = 0U; i < 20U; i++) {
        int hi = hex_nibble_val(p[i * 2U]);
        int lo = hex_nibble_val(p[(i * 2U) + 1U]);
        if ((hi < 0) || (lo < 0)) {
            return false;
        }
        out[i] = static_cast<uint8_t>((static_cast<uint8_t>(hi) << 4U) |
                                       static_cast<uint8_t>(lo));
    }
    return true;
}

static char lower_ascii(char c)
{
    return ((c >= 'A') && (c <= 'Z')) ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool eth_addr_eip55_ok(const char *hex)
{
    if (hex == NULL) { return false; }
    const char *p = hex;
    if ((p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X'))) { p += 2; }
    if (strlen(p) != 40U) { return false; }

    char lower[41];
    for (size_t i = 0U; i < 40U; i++) {
        if (hex_nibble_val(p[i]) < 0) { return false; }
        lower[i] = lower_ascii(p[i]);
    }
    lower[40] = '\0';

    uint8_t h[32];
    keccak256(reinterpret_cast<const uint8_t *>(lower), 40U, h);

    /* Accumulate rather than return early: one decision point, and the loop
     * costs the same whichever character is wrong. */
    uint32_t diff = 0U;
    for (size_t i = 0U; i < 40U; i++) {
        const uint8_t nib = ((i & 1U) != 0U)
                            ? static_cast<uint8_t>(h[i / 2U] & 0x0FU)
                            : static_cast<uint8_t>(h[i / 2U] >> 4U);
        const bool letter = (lower[i] >= 'a') && (lower[i] <= 'f');
        const char want = (letter && (nib >= 8U))
                          ? static_cast<char>(lower[i] - ('a' - 'A'))
                          : lower[i];
        diff |= static_cast<uint32_t>(static_cast<uint8_t>(p[i] ^ want));
    }
    return diff == 0U;
}
