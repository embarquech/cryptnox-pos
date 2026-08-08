/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/*
 * test_recip_store.cpp — host unit test for the recipient-address hardening
 * primitives: the verdict tokens, the monotonic step counter, the no-early-exit
 * complement comparator, and the EIP-55 checksum that stands between a mistyped
 * character and a payment to a stranger.
 *
 * NVS and the RAM shadow live in recip_store.cpp and need ESP-IDF, so they are
 * not covered here; everything this file tests is the logic they are built on.
 *
 * Single translation unit: includes the production sources directly. Build and
 * run from the repo root:
 *
 *   g++ -std=c++14 -Wall -Imain tests/units/test_recip_store.cpp -o t && ./t
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "recip_store.h"
#include "eth_addr.cpp"
#include "keccak256.cpp"

/* Build the complement of a string, the way provisioning does. */
static size_t complement(const char *s, uint8_t *out)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) { out[i] = (uint8_t)~(uint8_t)s[i]; }
    return n;
}

int main(void)
{
    /* ── verdicts are complements, and only OK is OK ── */
    assert(RECIP_OK == (recip_verdict_t)~RECIP_FAIL);
    assert(RECIP_IS_OK(RECIP_OK));
    assert(!RECIP_IS_OK(RECIP_FAIL));
    /* Every other bit pattern must read as failure, including the tempting
     * ones — a zeroed word and an all-ones word are what corruption produces. */
    assert(!RECIP_IS_OK(0u));
    assert(!RECIP_IS_OK(1u));
    assert(!RECIP_IS_OK(0xFFFFFFFFu));
    printf("verdict tokens ... OK\n");

    /* ── the complement comparator ── */
    const char *addr = "0xcAdDdf4677544D0eB25e4f87Cd978Aa5De23EbC6";
    uint8_t inv[64];
    size_t  n = complement(addr, inv);
    assert(RECIP_IS_OK(recip_bytes_ok((const uint8_t *)addr, n, inv, n, n)));

    /* One flipped bit anywhere must fail — first byte, last byte, middle. */
    const size_t spots[3] = { 0, n / 2, n - 1 };
    for (size_t k = 0; k < 3; k++) {
        inv[spots[k]] ^= 0x01u;
        assert(!RECIP_IS_OK(recip_bytes_ok((const uint8_t *)addr, n, inv, n, n)));
        inv[spots[k]] ^= 0x01u;
    }
    /* A length that disagrees with either copy fails without reading past it. */
    assert(!RECIP_IS_OK(recip_bytes_ok((const uint8_t *)addr, n, inv, n, n - 1)));
    assert(!RECIP_IS_OK(recip_bytes_ok((const uint8_t *)addr, n - 1, inv, n, n)));
    assert(!RECIP_IS_OK(recip_bytes_ok((const uint8_t *)addr, n, inv, n + 1, n)));
    assert(!RECIP_IS_OK(recip_bytes_ok(NULL, n, inv, n, n)));
    assert(!RECIP_IS_OK(recip_bytes_ok((const uint8_t *)addr, 0, inv, 0, 0)));
    /* A copy stored plain instead of complemented must NOT pass. */
    assert(!RECIP_IS_OK(recip_bytes_ok((const uint8_t *)addr, n,
                                       (const uint8_t *)addr, n, n)));
    printf("complement comparator ... OK\n");

    /* ── the step counter is monotonic and self-checking ── */
    recip_steps_t st;
    recip_steps_reset(&st);
    assert(RECIP_IS_OK(recip_steps_at(&st, RECIP_STEP_NONE)));
    assert(RECIP_IS_OK(recip_steps_advance(&st, RECIP_STEP_READ)));
    assert(RECIP_IS_OK(recip_steps_advance(&st, RECIP_STEP_DISPLAY)));
    /* Skipping CONFIRM to go straight to signing is the whole point. */
    assert(!RECIP_IS_OK(recip_steps_advance(&st, RECIP_STEP_SERIALIZE)));
    assert(RECIP_IS_OK(recip_steps_advance(&st, RECIP_STEP_CONFIRM)));
    /* Repeating a step, or going back, is not a legal transition either. */
    assert(!RECIP_IS_OK(recip_steps_advance(&st, RECIP_STEP_CONFIRM)));
    assert(!RECIP_IS_OK(recip_steps_advance(&st, RECIP_STEP_DISPLAY)));
    assert(RECIP_IS_OK(recip_steps_at(&st, RECIP_STEP_CONFIRM)));
    assert(!RECIP_IS_OK(recip_steps_at(&st, RECIP_STEP_SERIALIZE)));
    assert(RECIP_IS_OK(recip_steps_advance(&st, RECIP_STEP_SERIALIZE)));

    /* A flip in either half of the counter must be caught, so a corrupted
     * counter cannot be used to fake having passed a gate. */
    recip_steps_reset(&st);
    (void)recip_steps_advance(&st, RECIP_STEP_READ);
    st.n_inv ^= 0x00000010u;
    assert(!RECIP_IS_OK(recip_steps_at(&st, RECIP_STEP_READ)));
    assert(!RECIP_IS_OK(recip_steps_advance(&st, RECIP_STEP_DISPLAY)));
    printf("step counter ... OK\n");

    /* ── EIP-55: the typo detection the address format gives us ── */
    /* Canonical vectors from the EIP-55 specification. */
    assert(eth_addr_eip55_ok("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"));
    assert(eth_addr_eip55_ok("0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"));
    assert(eth_addr_eip55_ok("0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB"));
    assert(eth_addr_eip55_ok("0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb"));
    /* The project's own configured recipient and contract. */
    assert(eth_addr_eip55_ok("0xcAdDdf4677544D0eB25e4f87Cd978Aa5De23EbC6"));
    assert(eth_addr_eip55_ok("0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238"));
    /* Without the 0x prefix too — the caller may strip it. */
    assert(eth_addr_eip55_ok("5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"));

    /* All-lowercase carries no checksum and must be refused, not waved through:
     * accepting it would silently drop the only typo protection there is. */
    assert(!eth_addr_eip55_ok("0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed"));
    assert(!eth_addr_eip55_ok("0x5AAEB6053F3E94C9B9A09F33669435E7EF1BEAED"));
    /* A single character of wrong case — the realistic transcription slip. */
    assert(!eth_addr_eip55_ok("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1Beaed"));
    /* A single wrong hex digit, checksum intact elsewhere. */
    assert(!eth_addr_eip55_ok("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAee"));
    /* Malformed input must not be read past. */
    assert(!eth_addr_eip55_ok("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAe"));
    assert(!eth_addr_eip55_ok("0x"));
    assert(!eth_addr_eip55_ok(""));
    assert(!eth_addr_eip55_ok(NULL));
    assert(!eth_addr_eip55_ok("0xZZAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"));
    printf("eip-55 checksum ... OK\n");

    printf("test_recip_store: all OK\n");
    return 0;
}
