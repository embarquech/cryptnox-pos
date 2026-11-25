/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file sol_tx.cpp
 * @brief Solana base58 and native transfer serialisation.
 */

#include "sol_tx.h"

#include <string.h>

static const char B58[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/* Largest input encoded here is a signature. */
#define B58_IN_MAX  SOL_SIG_LEN

bool sol_b58_encode(const uint8_t *in, size_t n, char *out, size_t out_n)
{
    if ((out == NULL) || (out_n == 0U)) { return false; }
    out[0] = '\0';
    if ((in == NULL) || (n > B58_IN_MAX)) { return false; }

    size_t zeros = 0U;
    while ((zeros < n) && (in[zeros] == 0U)) { zeros++; }

    /* log(256)/log(58) < 1.37: base-58 digits, little-endian. */
    uint8_t digits[(B58_IN_MAX * 137U / 100U) + 1U];
    size_t  len = 0U;
    for (size_t i = zeros; i < n; i++) {
        unsigned carry = in[i];
        for (size_t j = 0U; j < len; j++) {
            carry += static_cast<unsigned>(digits[j]) << 8;
            digits[j] = static_cast<uint8_t>(carry % 58U);
            carry /= 58U;
        }
        while (carry != 0U) {
            digits[len++] = static_cast<uint8_t>(carry % 58U);
            carry /= 58U;
        }
    }

    if ((zeros + len + 1U) > out_n) { return false; }
    size_t k = 0U;
    for (size_t i = 0U; i < zeros; i++) { out[k++] = '1'; }
    for (size_t i = len; i > 0U; i--) { out[k++] = B58[digits[i - 1U]]; }
    out[k] = '\0';
    return true;
}

bool sol_addr_decode(const char *s, uint8_t out[SOL_KEY_LEN])
{
    if ((s == NULL) || (s[0] == '\0') || (strlen(s) >= SOL_ADDR_STR_MAX)) {
        return false;
    }
    uint8_t b[SOL_KEY_LEN] = { 0 };
    for (const char *p = s; *p != '\0'; p++) {
        const char *d = strchr(B58, *p);
        if (d == NULL) { return false; }
        unsigned carry = static_cast<unsigned>(d - B58);
        for (size_t j = SOL_KEY_LEN; j > 0U; j--) {
            carry += 58U * b[j - 1U];
            b[j - 1U] = static_cast<uint8_t>(carry & 0xFFU);
            carry >>= 8;
        }
        if (carry != 0U) { return false; }   /* more than 32 bytes */
    }
    /* Canonical form only: re-encode and compare. Catches a short string that
     * decoded into fewer bytes and a leading '1' that does not stand for a
     * leading zero byte. */
    char back[SOL_ADDR_STR_MAX];
    if (!sol_b58_encode(b, sizeof(b), back, sizeof(back)) ||
        (strcmp(back, s) != 0)) {
        return false;
    }
    (void)memcpy(out, b, SOL_KEY_LEN);
    return true;
}

size_t sol_transfer_message(const uint8_t from[SOL_KEY_LEN],
                            const uint8_t to[SOL_KEY_LEN],
                            const uint8_t blockhash[SOL_KEY_LEN],
                            uint64_t lamports, uint8_t *out, size_t out_n)
{
    if ((out == NULL) || (out_n < SOL_TRANSFER_MSG_LEN) || (lamports == 0U) ||
        (memcmp(from, to, SOL_KEY_LEN) == 0)) {
        return 0U;
    }
    size_t k = 0U;
    /* Header: 1 signature, 0 read-only signers, 1 read-only non-signer. */
    out[k++] = 1U;
    out[k++] = 0U;
    out[k++] = 1U;
    out[k++] = 3U;   /* account keys */
    (void)memcpy(&out[k], from, SOL_KEY_LEN); k += SOL_KEY_LEN;
    (void)memcpy(&out[k], to,   SOL_KEY_LEN); k += SOL_KEY_LEN;
    (void)memset(&out[k], 0,    SOL_KEY_LEN); k += SOL_KEY_LEN;  /* System Program */
    (void)memcpy(&out[k], blockhash, SOL_KEY_LEN); k += SOL_KEY_LEN;
    out[k++] = 1U;   /* instructions */
    out[k++] = 2U;   /* program: the System Program's index */
    out[k++] = 2U;   /* accounts: from, to */
    out[k++] = 0U;
    out[k++] = 1U;
    out[k++] = 12U;  /* data: u32 instruction + u64 lamports, both LE */
    out[k++] = 2U;   /* SystemInstruction::Transfer */
    out[k++] = 0U;
    out[k++] = 0U;
    out[k++] = 0U;
    for (unsigned i = 0U; i < 8U; i++) {
        out[k++] = static_cast<uint8_t>((lamports >> (8U * i)) & 0xFFU);
    }
    return k;
}

size_t sol_tx_wire(const uint8_t sig[SOL_SIG_LEN], const uint8_t *msg,
                   size_t msg_len, uint8_t *out, size_t out_n)
{
    /* A count above 127 would need a second compact-u16 byte; one is all a
     * single-signer transfer has. */
    if ((out == NULL) || (out_n < (1U + SOL_SIG_LEN + msg_len))) { return 0U; }
    out[0] = 1U;
    (void)memcpy(&out[1], sig, SOL_SIG_LEN);
    (void)memcpy(&out[1U + SOL_SIG_LEN], msg, msg_len);
    return 1U + SOL_SIG_LEN + msg_len;
}
