/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/*
 * test_eth_addr.cpp — host unit test for the hex address parser + EIP-55
 * checksum verification (HARDENING.md §7.1).
 *
 * Single translation unit: it #includes the production sources directly, the
 * same pattern as the fuzz harnesses, so the test can never drift from the
 * firmware. Build & run from the repo root:
 *
 *   g++ -std=c++14 -Imain -Icryptnox-sdk-esp32/cryptnox-sdk-cpp \
 *       tests/units/test_eth_addr.cpp -o test_eth_addr && ./test_eth_addr
 */

#include <assert.h>
#include <stdio.h>

/* CW_Utils::fill_secure_random is ESP32-specific and never reached from
 * safe_memcpy / secure_wipe; stub it for the linker. */
#include "CW_Utils.h"
bool CW_Utils::fill_secure_random(uint8_t *dest, size_t len)
{
    (void)dest;
    (void)len;
    return false;
}

#include "CW_Utils.cpp"
#include "keccak256.cpp"
#include "eth_addr.cpp"

int main(void)
{
    uint8_t a[ETH_ADDR_LEN];

    /* Canonical EIP-55 vectors (from the spec) — all must verify. */
    assert(eth_addr_parse("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", a));
    assert(eth_addr_parse("0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359", a));
    assert(eth_addr_parse("0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB", a));
    assert(eth_addr_parse("0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb", a));

    /* All-lowercase: un-checksummed, accepted. */
    assert(eth_addr_parse("0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed", a));

    /* Bare (no 0x prefix) — accepted. */
    assert(eth_addr_parse("5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", a));

    /* Corrupted checksum (one letter's case flipped) — rejected. */
    assert(!eth_addr_parse("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1Beaed", a));

    /* Wrong length / non-hex — rejected. */
    assert(!eth_addr_parse("0x1234", a));
    assert(!eth_addr_parse("0xZZAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", a));

    /* Decoded bytes match the vector. */
    assert(eth_addr_parse("0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb", a));
    assert((a[0] == 0xD1U) && (a[1] == 0x22U) && (a[19] == 0xDBU));

    puts("eth_addr unit test OK");
    return 0;
}
