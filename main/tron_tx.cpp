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
 *   TriggerSmartContract.owner_address    : field 1, bytes -> 0x0a, len 0x15
 *   TriggerSmartContract.contract_address : field 2, bytes -> 0x12, len 0x15
 *   TriggerSmartContract.data             : field 4, bytes -> 0x22, len 0x44 (68)
 *   Transaction.raw_data           : field 1, bytes  -> 0x0a
 *   Transaction.raw_data.fee_limit : field 18, varint -> 0x90 0x01
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

/**
 * @brief Case-insensitive substring search, BYTE-ALIGNED (no strcasestr in
 *        newlib).
 *
 * The step is 2, not 1, and that is the security property rather than an
 * optimisation. @p hay is the hex encoding of a byte string, so a real protobuf
 * field can only ever begin at an even character offset. Searching odd offsets
 * too let a hostile node hide the byte run we look for one nibble out of
 * alignment — inside a memo, or any field whose contents it chooses — while the
 * contract that actually executes paid somebody else. Both strings still hash to
 * a consistent txID, so nothing downstream would have caught it.
 *
 * Callers must therefore only ever pass an even-length @p needle (every needle
 * built below is whole bytes) over an even-length @p hay.
 */
static bool hex_contains(const char *hay, const char *needle)
{
    const size_t nl = strlen(needle);
    const size_t hl = strlen(hay);
    if ((nl == 0U) || (hl < nl)) { return false; }
    /* Odd on either side means somebody built a needle that is not whole bytes,
     * or handed us hex that is not a byte string. Neither is matchable. */
    if (((nl % 2U) != 0U) || ((hl % 2U) != 0U)) { return false; }

    for (size_t i = 0U; i + nl <= hl; i += 2U) {
        size_t j = 0U;
        while ((j < nl) && (lower_ascii(hay[i + j]) == lower_ascii(needle[j]))) {
            j++;
        }
        if (j == nl) { return true; }
    }
    return false;
}

/**
 * @brief true if @p raw is usable raw_data hex: all hex, non-empty, whole bytes.
 *
 * The even-length part is not pedantry. Every check below locates whole bytes at
 * byte boundaries (see @ref hex_contains), and an odd-length string has no byte
 * boundaries to speak of — so it is refused here rather than searched.
 */
static bool raw_hex_ok(const char *raw)
{
    if (raw == NULL) { return false; }
    const size_t n = hex_strlen(raw);
    return (n != 0U) && ((n % 2U) == 0U);
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
    if (!raw_hex_ok(raw_data_hex)) { return false; }
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

size_t tron_trc20_param_hex(const char *to_hex, uint64_t amount,
                            char *out, size_t out_size)
{
    if (!addr_hex_ok(to_hex) || (out == NULL) ||
        (out_size < (TRON_TRC20_PARAM_HEX_LEN + 1U))) {
        return 0U;
    }

    size_t p = 0U;
    /* word 1 — address: 12 zero bytes, then the key hash without its "41". */
    for (size_t i = 0U; i < 24U; i++, p++) { out[p] = '0'; }
    for (size_t i = 2U; i < TRON_ADDR_HEX_LEN; i++, p++) {
        out[p] = lower_ascii(to_hex[i]);
    }
    /* word 2 — amount: 24 zero bytes, then the 64-bit value big-endian. */
    for (size_t i = 0U; i < 48U; i++, p++) { out[p] = '0'; }
    for (int sh = 60; sh >= 0; sh -= 4, p++) {
        out[p] = nibble_hex(static_cast<unsigned>((amount >> sh) & 0x0FULL));
    }

    out[p] = '\0';
    return p;
}

bool tron_tx_trc20_ok(const char *raw_data_hex, const char *owner_hex,
                      const char *contract_hex, const char *to_hex,
                      uint64_t amount, uint64_t fee_limit_sun)
{
    if (!raw_hex_ok(raw_data_hex)) { return false; }
    if (!addr_hex_ok(owner_hex) || !addr_hex_ok(contract_hex) ||
        !addr_hex_ok(to_hex)) {
        return false;
    }
    /* A zero-token transfer is not a payment, and an uncapped call is not one
     * the operator agreed to. Neither gets signed. */
    if ((amount == 0U) || (fee_limit_sun == 0U)) { return false; }

    char param[TRON_TRC20_PARAM_HEX_LEN + 1U];
    if (tron_trc20_param_hex(to_hex, amount, param, sizeof(param)) == 0U) {
        return false;
    }

    char want[4U + TRON_ADDR_HEX_LEN + 4U + TRON_ADDR_HEX_LEN + 4U +
              sizeof(TRON_TRC20_SELECTOR) + sizeof(param) + 8U];
    int k = snprintf(want, sizeof(want), "0a15%s1215%s2244" TRON_TRC20_SELECTOR "%s",
                     owner_hex, contract_hex, param);
    if ((k <= 0) || (static_cast<size_t>(k) >= sizeof(want))) { return false; }

    if (!hex_contains(raw_data_hex, want)) {
        /* call_value is proto3-default 0 and therefore normally omitted, but a
         * node that emits it explicitly ("1800") is serialising the same
         * contract — accept that shape rather than decline a valid payment. */
        char want_cv[sizeof(want) + 4U];
        k = snprintf(want_cv, sizeof(want_cv),
                     "0a15%s1215%s18002244" TRON_TRC20_SELECTOR "%s",
                     owner_hex, contract_hex, param);
        if ((k <= 0) || (static_cast<size_t>(k) >= sizeof(want_cv))) {
            return false;
        }
        if (!hex_contains(raw_data_hex, want_cv)) { return false; }
    }

    /* ponytail: byte-aligned substring match, so two things remain possible that
     * a real parser would catch — a hostile fee_limit plus a coincidental
     * "9001<our varint>" on a byte boundary elsewhere in raw_data (~2^-32), and
     * a second `contract` entry alongside ours, since the field repeats. Neither
     * is reachable without a node that is already lying to us AND getting the
     * chain to accept a multi-contract transaction. Parse the protobuf
     * field-by-field if either ever stops being acceptable. */
    char fee_varint[24];
    if (tron_varint_hex(fee_limit_sun, fee_varint, sizeof(fee_varint)) == 0U) {
        return false;
    }
    char fee_want[4U + sizeof(fee_varint)];
    k = snprintf(fee_want, sizeof(fee_want), "9001%s", fee_varint);
    if ((k <= 0) || (static_cast<size_t>(k) >= sizeof(fee_want))) {
        return false;
    }

    return hex_contains(raw_data_hex, fee_want);
}

size_t tron_tx_envelope_hex(const char *raw_data_hex, const char *sig_hex,
                            char *out, size_t out_size)
{
    if ((raw_data_hex == NULL) || (sig_hex == NULL) || (out == NULL) ||
        (out_size == 0U)) {
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
