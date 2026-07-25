/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_addr.cpp
 * @brief Implementation of the hex Ethereum-address parser, with EIP-55
 *        checksum verification (HARDENING.md §7.1).
 */

#include "eth_addr.h"

#include <string.h>
#include <stdbool.h>

#include "keccak256.h"
#include "CW_Utils.h"   /* SDK hardened primitives: secure_wipe */

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

    CW_Utils::secure_wipe(out, 20U);

    /* Single pass: validate every nibble, decode the 20 bytes, lower-case a
     * copy for the checksum hash, and note whether the source is mixed-case. */
    char lower[40];
    bool has_upper = false;
    bool has_lower = false;
    size_t i;
    for (i = 0U; i < 40U; i++) {
        char c = p[i];
        int  v = hex_nibble_val(c);
        if (v < 0) {
            return false;
        }
        if ((c >= 'a') && (c <= 'f')) {
            has_lower = true;
            lower[i]  = c;
        } else if ((c >= 'A') && (c <= 'F')) {
            has_upper = true;
            lower[i]  = static_cast<char>((c - 'A') + 'a');
        } else {
            lower[i] = c;   /* digit — case-neutral */
        }
        if ((i & 1U) == 0U) {
            out[i / 2U] = static_cast<uint8_t>(static_cast<uint8_t>(v) << 4U);
        } else {
            out[i / 2U] |= static_cast<uint8_t>(v);
        }
    }

    /* EIP-55: a mixed-case address carries a checksum, so verify it; an
     * all-lower or all-upper address is treated as un-checksummed and accepted
     * (standard lenient behaviour). A checksummed ADDR_TO thus rejects any
     * single-nibble corruption or typo of the source string. */
    if (has_upper && has_lower) {
        uint8_t h[32];
        keccak256(reinterpret_cast<const uint8_t *>(lower), 40U, h);
        for (i = 0U; i < 40U; i++) {
            char c = p[i];
            bool is_alpha = ((c >= 'a') && (c <= 'f')) ||
                            ((c >= 'A') && (c <= 'F'));
            if (!is_alpha) {
                continue;   /* digits have no case to check */
            }
            uint8_t nib = ((i & 1U) == 0U) ? static_cast<uint8_t>(h[i / 2U] >> 4U)
                                           : static_cast<uint8_t>(h[i / 2U] & 0x0FU);
            bool want_upper = (nib >= 8U);
            bool is_upper   = ((c >= 'A') && (c <= 'F'));
            if (want_upper != is_upper) {
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Host self-test (not built into the firmware). Compile & run with:
 *   g++ -DETH_ADDR_SELFTEST main/eth_addr.cpp main/keccak256.cpp \
 *       <SDK>/CW_Utils.cpp -Imain -I<SDK> -o selftest && ./selftest
 * where <SDK> is the cryptnox-sdk-cpp directory (CW_Utils.h/.cpp).
 * ------------------------------------------------------------------------ */
#ifdef ETH_ADDR_SELFTEST
#include <assert.h>
#include <stdio.h>
#include "hardening.h"

int main(void)
{
    uint8_t a[20];

    /* Canonical EIP-55 vectors (from the spec) — all must verify. */
    assert(eth_addr_parse("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", a));
    assert(eth_addr_parse("0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359", a));
    assert(eth_addr_parse("0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB", a));
    assert(eth_addr_parse("0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb", a));

    /* All-lowercase: un-checksummed, accepted. */
    assert(eth_addr_parse("0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed", a));

    /* Corrupted checksum (one letter's case flipped) — rejected. */
    assert(!eth_addr_parse("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1Beaed", a));

    /* Wrong length / non-hex — rejected. */
    assert(!eth_addr_parse("0x1234", a));
    assert(!eth_addr_parse("0xZZAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", a));

    /* Decision-integrity gate (hardening.h). */
    pos_amount_t amt;
    pos_amount_set(&amt, 1000000ULL);
    pos_addr_t to;
    assert(eth_addr_parse("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", to.addr));
    assert(eth_addr_parse("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", to.addr_echo));
    assert(IS_TRUE32(run_payment_decision(&amt, &to, POS_VERDICT_APPROVED)));
    assert(!IS_TRUE32(run_payment_decision(&amt, &to, POS_VERDICT_DECLINED)));

    amt.amount_minor_inv ^= 1ULL;   /* simulate a bit-flip on the echo */
    assert(!IS_TRUE32(run_payment_decision(&amt, &to, POS_VERDICT_APPROVED)));

    pos_amount_set(&amt, 1000000ULL);
    to.addr_echo[0] ^= 1U;           /* simulate a bit-flip on the address */
    assert(!IS_TRUE32(run_payment_decision(&amt, &to, POS_VERDICT_APPROVED)));

    puts("eth_addr + hardening self-test OK");
    return 0;
}
#endif
