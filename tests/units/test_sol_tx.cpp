/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/*
 * test_sol_tx.cpp — host unit test for main/sol_tx.cpp: base58 both ways and the
 * native SOL transfer the card signs.
 *
 * The message bytes are what the card signs and nothing re-derives them, so a
 * byte out of place is either a refused transaction or a payment to somebody
 * else. The expected bytes come from solders (Solana's Rust SDK), not from this
 * code: Message.new_with_blockhash([transfer(...)], payer, blockhash).
 *
 *   g++ -std=c++14 -Imain tests/units/test_sol_tx.cpp -o test_sol_tx && ./test_sol_tx
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sol_tx.cpp"

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0U; i < n; i++) { (void)sprintf(&out[i * 2U], "%02x", b[i]); }
}

static void seq(uint8_t *b, size_t n, unsigned first)
{
    for (size_t i = 0U; i < n; i++) { b[i] = static_cast<uint8_t>(first + i); }
}

int main(void)
{
    uint8_t from[32], to[32], bh[32], sig[64], zero[32] = { 0 };
    seq(from, 32U, 1U);
    seq(to, 32U, 100U);
    seq(bh, 32U, 200U);
    seq(sig, 64U, 0U);

    /* base58, against python base58. */
    char s[SOL_SIG_STR_MAX];
    assert(sol_b58_encode(from, 32U, s, sizeof(s)));
    assert(strcmp(s, "4wBqpZM9xaSheZzJSMawUKKwhdpChKbZ5eu5ky4Vigw") == 0);
    assert(sol_b58_encode(zero, 32U, s, sizeof(s)));
    assert(strcmp(s, "11111111111111111111111111111111") == 0);
    assert(sol_b58_encode(sig, 64U, s, sizeof(s)));
    assert(strcmp(s, "1GMkH3brNXiNNs1tiFZHu4yZSRrzJwxi5wB9bHFtMinfCXNnR1adh8Vo8NTheK4evneedH4qmvjeqcBBNAefgS") == 0);
    uint8_t lead[32] = { 0 };
    seq(&lead[3], 29U, 1U);
    assert(sol_b58_encode(lead, 32U, s, sizeof(s)));
    assert(strcmp(s, "1113ev7KArKmSeNWXoiX44de25oM4BcoUZHC9RfTFS") == 0);
    /* Too small an output is refused whole, not truncated. */
    assert(!sol_b58_encode(from, 32U, s, 43U));   /* 43 chars + NUL */
    assert(s[0] == '\0');

    /* Decode: round trip, then everything that must not name an account. */
    uint8_t k[32];
    assert(sol_addr_decode("7kuT1dfMhUysWcLEV1eYk8ir7RTjszHmsUdrrPQNThcv", k));
    assert(memcmp(k, to, 32U) == 0);
    assert(sol_addr_decode("1113ev7KArKmSeNWXoiX44de25oM4BcoUZHC9RfTFS", k));
    assert(memcmp(k, lead, 32U) == 0);
    assert(sol_addr_decode("11111111111111111111111111111111", k));
    assert(memcmp(k, zero, 32U) == 0);
    assert(!sol_addr_decode("", k));
    assert(!sol_addr_decode(NULL, k));
    assert(!sol_addr_decode("7kuT1dfMhUysWcLEV1eYk8ir7RTjszHmsUdrrPQNThc0", k)); /* '0' */
    assert(!sol_addr_decode("7kuT1dfMhUysWcLEV1eYk8ir7RTjszHmsUdrrPQNThcvv", k)); /* > 32 B */
    assert(!sol_addr_decode("7kuT1dfMhUysWcLEV1eYk8ir7RTjszHm", k));             /* < 32 B */
    assert(!sol_addr_decode("17kuT1dfMhUysWcLEV1eYk8ir7RTjszHmsUdrrPQNThc", k)); /* non-canonical */
    assert(!sol_addr_decode("THQGuFzL87ZqhxkgqYEryRAd7gqFqL5rdc", k));           /* a Tron address */

    /* The message, byte for byte. */
    static const char WANT[] =
        "01000103"
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
        "6465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f80818283"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7"
        "01020200010c02000000cb04fb711f010000";
    uint8_t msg[SOL_TRANSFER_MSG_LEN + 8U];
    char    msg_hex[sizeof(msg) * 2U + 1U];
    const size_t n = sol_transfer_message(from, to, bh, 1234567890123ULL,
                                          msg, sizeof(msg));
    assert(n == SOL_TRANSFER_MSG_LEN);
    hex(msg, n, msg_hex);
    assert(strcmp(msg_hex, WANT) == 0);

    /* Refusals: nothing moved, paying yourself, no room. */
    assert(sol_transfer_message(from, to, bh, 0U, msg, sizeof(msg)) == 0U);
    assert(sol_transfer_message(from, from, bh, 1U, msg, sizeof(msg)) == 0U);
    assert(sol_transfer_message(from, to, bh, 1U, msg, SOL_TRANSFER_MSG_LEN - 1U) == 0U);

    /* The wire form: solders' Transaction.populate(message, [sig]). */
    uint8_t tx[SOL_TRANSFER_TX_LEN];
    assert(sol_tx_wire(sig, msg, n, tx, sizeof(tx)) == SOL_TRANSFER_TX_LEN);
    assert(tx[0] == 1U);
    assert(memcmp(&tx[1], sig, 64U) == 0);
    assert(memcmp(&tx[65], msg, n) == 0);
    assert(sol_tx_wire(sig, msg, n, tx, sizeof(tx) - 1U) == 0U);

    printf("test_sol_tx OK\n");
    return 0;
}
