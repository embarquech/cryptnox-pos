/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_addr.cpp
 * @brief Implementation of the hex Ethereum-address parser.
 */

#include "eth_addr.h"

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
