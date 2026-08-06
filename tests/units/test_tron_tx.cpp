/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/*
 * test_tron_tx.cpp — host unit test for the Tron hex helpers: protobuf varints,
 * the TransferContract integrity check (the thing standing between a hostile
 * full node and a redirected payment) and the signed-Transaction envelope.
 *
 * Single translation unit: it #includes the production source directly, the
 * same pattern as the other tests here. Build & run from the repo root:
 *
 *   g++ -std=c++14 -Imain tests/units/test_tron_tx.cpp -o test_tron_tx && \
 *       ./test_tron_tx
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tron_tx.cpp"

/* A Nile-shaped raw_data for 1.5 TRX (1500000 sun). The contract run inside it
 * is "0a15 <owner> 1215 <to> 18 e0c65b" — everything else (reference block,
 * type_url, expiry, timestamp) is context the check must ignore. */
#define OWNER "41d4a5f19c1b9e0e2a1d5f0b3c4d5e6f708192a3b4"
#define DEST  "41cadddf4677544d0eb25e4f87cd978aa5de23ebc6"
static const char *const RAW_OK =
    "0a02b3a12208d7f0f0b0f0a0c0d040e0c8f6d6e5325ad1010a2d747970652e676f6f676c"
    "65617069732e636f6d2f70726f746f636f6c2e5472616e73666572436f6e747261637412"
    "320a15" OWNER "1215" DEST "18e0c65b70f087f2d6e532";

int main(void)
{
    /* ── varints (protobuf base-128, little-endian groups) ── */
    char v[24];
    assert(tron_varint_hex(0U, v, sizeof(v)) == 2U);
    assert(strcmp(v, "00") == 0);
    assert(tron_varint_hex(1U, v, sizeof(v)) == 2U);
    assert(strcmp(v, "01") == 0);
    assert(tron_varint_hex(127U, v, sizeof(v)) == 2U);
    assert(strcmp(v, "7f") == 0);
    assert(tron_varint_hex(128U, v, sizeof(v)) == 4U);
    assert(strcmp(v, "8001") == 0);
    assert(tron_varint_hex(1500000U, v, sizeof(v)) == 6U);   /* 1.5 TRX */
    assert(strcmp(v, "e0c65b") == 0);
    assert(tron_varint_hex(1000000U, v, sizeof(v)) == 6U);   /* 1.0 TRX */
    assert(strcmp(v, "c0843d") == 0);
    /* Too small an output buffer must fail, not truncate. */
    char tiny[3];
    assert(tron_varint_hex(128U, tiny, sizeof(tiny)) == 0U);
    printf("varint hex ... OK\n");

    /* ── the contract check accepts exactly what was asked for ── */
    assert(tron_tx_contract_ok(RAW_OK, OWNER, DEST, 1500000U));
    /* ...and refuses everything else. Each of these is a payment the terminal
     * must decline rather than sign. */
    assert(!tron_tx_contract_ok(RAW_OK, OWNER, DEST, 1500001U));  /* amount */
    assert(!tron_tx_contract_ok(RAW_OK, DEST, OWNER, 1500000U));  /* swapped */
    assert(!tron_tx_contract_ok(RAW_OK, OWNER,
                                "41cadddf4677544d0eb25e4f87cd978aa5de23ebc7",
                                1500000U));                      /* recipient */
    assert(!tron_tx_contract_ok(RAW_OK, OWNER, DEST, 0U));        /* zero */
    assert(!tron_tx_contract_ok("", OWNER, DEST, 1500000U));
    assert(!tron_tx_contract_ok("not hex!", OWNER, DEST, 1500000U));
    assert(!tron_tx_contract_ok(RAW_OK, "41short", DEST, 1500000U));
    assert(!tron_tx_contract_ok(RAW_OK, OWNER,
                               /* right length, wrong address prefix */
                               "42cadddf4677544d0eb25e4f87cd978aa5de23ebc6",
                               1500000U));
    assert(!tron_tx_contract_ok(NULL, OWNER, DEST, 1500000U));
    /* Hex case must not matter: config addresses carry an EIP-55 checksum. */
    assert(tron_tx_contract_ok(RAW_OK, OWNER,
                               "41CADDDF4677544D0EB25E4F87CD978AA5DE23EBC6",
                               1500000U));

    /* The same check against an actual /wallet/createtransaction answer from a
     * Nile full node (1.5 TRX), so the expected byte run is pinned to what the
     * chain really serialises and not just to our reading of the protobuf. */
    static const char *const NILE_RAW =
        "0a021f5e2208c011d0fa8e1ebf4d408093ad8afd335a67080112630a2d747970652e"
        "676f6f676c65617069732e636f6d2f70726f746f636f6c2e5472616e73666572436f"
        "6e747261637412320a1541d1e7a6bc354106cb410e65ff8b181c600ff142921215"
        "41e552f6487585c2b58bc2c9bb4492bc1f17132cd018e0c65b7091c4a98afd33";
    static const char *const NILE_OWNER =
        "41d1e7a6bc354106cb410e65ff8b181c600ff14292";
    static const char *const NILE_TO =
        "41e552f6487585c2b58bc2c9bb4492bc1f17132cd0";
    assert(tron_tx_contract_ok(NILE_RAW, NILE_OWNER, NILE_TO, 1500000U));
    assert(!tron_tx_contract_ok(NILE_RAW, NILE_OWNER, NILE_TO, 1500000U + 1U));
    printf("contract check ... OK\n");

    /* ── envelope: Transaction { 1: raw_data, 2: signature } ── */
    static const char sig[TRON_SIG_HEX_LEN + 1] =
        "1111111111111111111111111111111111111111111111111111111111111111"
        "2222222222222222222222222222222222222222222222222222222222222222"
        "01";
    char env[1024];
    size_t n = tron_tx_envelope_hex(RAW_OK, sig, env, sizeof(env));
    assert(n == strlen(env));
    /* raw_data is 130 bytes here -> varint 0x82 0x01 */
    assert(strlen(RAW_OK) / 2U == 130U);
    assert(strncmp(env, "0a8201", 6) == 0);
    assert(strncmp(env + 6, RAW_OK, strlen(RAW_OK)) == 0);
    char tail[8 + TRON_SIG_HEX_LEN];
    (void)snprintf(tail, sizeof(tail), "1241%s", sig);
    assert(strcmp(env + 6 + strlen(RAW_OK), tail) == 0);

    /* Bad inputs produce nothing, never a half-built transaction. */
    assert(tron_tx_envelope_hex(RAW_OK, "0011", env, sizeof(env)) == 0U);
    assert(tron_tx_envelope_hex("abc", sig, env, sizeof(env)) == 0U);
    assert(tron_tx_envelope_hex("zz", sig, env, sizeof(env)) == 0U);
    char small[16];
    assert(tron_tx_envelope_hex(RAW_OK, sig, small, sizeof(small)) == 0U);
    assert(small[0] == '\0');
    printf("envelope hex ... OK\n");

    printf("test_tron_tx: all OK\n");
    return 0;
}
