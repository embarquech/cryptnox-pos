/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file tron_tx.cpp
 * @brief Implementation of the Tron transaction hex helpers.
 */

#include "tron_tx.h"

#include <stdio.h>
#include <string.h>

/* Protobuf wire tags used below (field number << 3 | wire type):
 *   TransferContract.owner_address : field 1, bytes  -> 0x0a, len 0x15 (21)
 *   TransferContract.to_address    : field 2, bytes  -> 0x12, len 0x15 (21)
 *   TransferContract.amount        : field 3, varint -> 0x18
 *   Transaction.raw_data           : field 1, bytes  -> 0x0a
 *   Transaction.signature          : field 2, bytes  -> 0x12, len 0x41 (65)
 */

static char nibble_hex(unsigned n)
{
    return (n < 10U) ? static_cast<char>('0' + n)
                     : static_cast<char>('a' + n - 10U);
}

static bool is_hex_digit(char c)
{
    return ((c >= '0') && (c <= '9')) ||
           ((c >= 'a') && (c <= 'f')) ||
           ((c >= 'A') && (c <= 'F'));
}

static char lower_ascii(char c)
{
    return ((c >= 'A') && (c <= 'Z')) ? static_cast<char>(c + ('a' - 'A')) : c;
}

/** @brief Length of @p s if every character is hex, 0 otherwise. */
static size_t hex_strlen(const char *s)
{
    size_t n = 0U;
    for (; s[n] != '\0'; n++) {
        if (!is_hex_digit(s[n])) { return 0U; }
    }
    return n;
}

/** @brief Case-insensitive substring search (no strcasestr in newlib). */
static bool hex_contains(const char *hay, const char *needle)
{
    const size_t nl = strlen(needle);
    const size_t hl = strlen(hay);
    if ((nl == 0U) || (hl < nl)) { return false; }

    for (size_t i = 0U; i + nl <= hl; i++) {
        size_t j = 0U;
        while ((j < nl) && (lower_ascii(hay[i + j]) == lower_ascii(needle[j]))) {
            j++;
        }
        if (j == nl) { return true; }
    }
    return false;
}

/** @brief true if @p addr is a "41"-prefixed 21-byte Tron address in hex. */
static bool addr_hex_ok(const char *addr)
{
    return (addr != NULL) &&
           (hex_strlen(addr) == TRON_ADDR_HEX_LEN) &&
           (addr[0] == '4') && (addr[1] == '1');
}

size_t tron_varint_hex(uint64_t v, char *out, size_t out_size)
{
    uint8_t b[10];            /* 64 bits / 7 bits per byte = 10 bytes max */
    size_t  n = 0U;

    do {
        uint8_t x = static_cast<uint8_t>(v & 0x7FU);
        v >>= 7U;
        if (v != 0U) { x |= 0x80U; }
        b[n] = x;
        n++;
    } while ((v != 0U) && (n < sizeof(b)));

    if ((out == NULL) || (out_size < ((n * 2U) + 1U))) { return 0U; }

    for (size_t i = 0U; i < n; i++) {
        out[i * 2U]      = nibble_hex((b[i] >> 4U) & 0x0FU);
        out[i * 2U + 1U] = nibble_hex(b[i] & 0x0FU);
    }
    out[n * 2U] = '\0';
    return n * 2U;
}

bool tron_tx_contract_ok(const char *raw_data_hex, const char *owner_hex,
                         const char *to_hex, uint64_t amount_sun)
{
    if ((raw_data_hex == NULL) || (hex_strlen(raw_data_hex) == 0U)) {
        return false;
    }
    if (!addr_hex_ok(owner_hex) || !addr_hex_ok(to_hex)) { return false; }
    /* A zero-amount transfer is not a payment; reject rather than sign it. */
    if (amount_sun == 0U) { return false; }

    char amount[24];
    if (tron_varint_hex(amount_sun, amount, sizeof(amount)) == 0U) {
        return false;
    }

    /* "0a15" owner "1215" to "18" amount — the whole contract, contiguous. */
    char want[4U + TRON_ADDR_HEX_LEN + 4U + TRON_ADDR_HEX_LEN + 2U +
              sizeof(amount)];
    int k = snprintf(want, sizeof(want), "0a15%s1215%s18%s",
                     owner_hex, to_hex, amount);
    if ((k <= 0) || (static_cast<size_t>(k) >= sizeof(want))) { return false; }

    return hex_contains(raw_data_hex, want);
}

size_t tron_tx_envelope_hex(const char *raw_data_hex, const char *sig_hex,
                            char *out, size_t out_size)
{
    if ((raw_data_hex == NULL) || (sig_hex == NULL) || (out == NULL)) {
        return 0U;
    }

    const size_t raw_len = hex_strlen(raw_data_hex);
    if ((raw_len == 0U) || ((raw_len % 2U) != 0U)) { return 0U; }
    if (hex_strlen(sig_hex) != TRON_SIG_HEX_LEN) { return 0U; }

    char len_hex[24];
    if (tron_varint_hex(raw_len / 2U, len_hex, sizeof(len_hex)) == 0U) {
        return 0U;
    }

    int k = snprintf(out, out_size, "0a%s%s1241%s",
                     len_hex, raw_data_hex, sig_hex);
    if ((k <= 0) || (static_cast<size_t>(k) >= out_size)) {
        out[0] = '\0';
        return 0U;
    }
    return static_cast<size_t>(k);
}
