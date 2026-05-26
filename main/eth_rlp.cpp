#include "eth_rlp.h"
#include <string.h>

/* Maximum scratch buffer for the list content before prepending the header. */
#define ITEMS_BUF_MAX  320U

/******************************************************************
 * Internal helpers
 ******************************************************************/

/*
 * Write 'value' as a minimal big-endian byte sequence into out[0..].
 * Returns the number of bytes written (1–8).  Value 0 produces one 0x00 byte;
 * the caller is responsible for treating the empty-integer case (value==0
 * → RLP 0x80) before calling this function.
 */
static uint8_t be_minimal(uint64_t value, uint8_t out[8])
{
    uint8_t tmp[8];
    uint8_t first = 0U;
    uint8_t i;

    tmp[0] = (uint8_t)((value >> 56U) & 0xFFU);
    tmp[1] = (uint8_t)((value >> 48U) & 0xFFU);
    tmp[2] = (uint8_t)((value >> 40U) & 0xFFU);
    tmp[3] = (uint8_t)((value >> 32U) & 0xFFU);
    tmp[4] = (uint8_t)((value >> 24U) & 0xFFU);
    tmp[5] = (uint8_t)((value >> 16U) & 0xFFU);
    tmp[6] = (uint8_t)((value >>  8U) & 0xFFU);
    tmp[7] = (uint8_t)(value & 0xFFU);

    while ((first < 7U) && (tmp[first] == 0U)) {
        first++;
    }
    uint8_t count = (uint8_t)(8U - first);
    for (i = 0U; i < count; i++) {
        out[i] = tmp[(size_t)first + i];
    }
    return count;
}

/*
 * RLP-encode a byte string of length data_len at 'data' into out.
 * Returns bytes written.
 */
static size_t rlp_bytes(const uint8_t *data, size_t data_len, uint8_t *out)
{
    size_t written = 0U;

    if (data_len == 0U) {
        out[0] = 0x80U;
        written = 1U;
    } else if ((data_len == 1U) && (data[0] < 0x80U)) {
        out[0] = data[0];
        written = 1U;
    } else if (data_len <= 55U) {
        out[0] = (uint8_t)(0x80U + data_len);
        (void)memcpy(out + 1U, data, data_len);
        written = 1U + data_len;
    } else {
        /* Long string: 0xb7 + len(length) bytes, then length, then data. */
        uint8_t len_bytes[8];
        uint8_t num_lb = be_minimal((uint64_t)data_len, len_bytes);
        out[0] = (uint8_t)(0xB7U + num_lb);
        (void)memcpy(out + 1U, len_bytes, num_lb);
        (void)memcpy(out + 1U + num_lb, data, data_len);
        written = 1U + num_lb + data_len;
    }
    return written;
}

/*
 * RLP-encode a non-negative integer (uint64_t) into out.
 * 0 is encoded as the empty byte string (0x80).
 * Returns bytes written.
 */
static size_t rlp_uint64(uint64_t value, uint8_t *out)
{
    if (value == 0U) {
        out[0] = 0x80U;
        return 1U;
    }
    uint8_t be[8];
    uint8_t be_len = be_minimal(value, be);
    return rlp_bytes(be, be_len, out);
}

/*
 * RLP-encode a 32-byte big-endian integer (e.g. r or s from ECDSA) into out.
 * Leading zero bytes are stripped so the encoding is minimal.
 * Returns bytes written.
 */
static size_t rlp_int256(const uint8_t data[32], uint8_t *out)
{
    size_t first = 0U;
    while ((first < 31U) && (data[first] == 0U)) {
        first++;
    }
    if ((first == 31U) && (data[31] == 0U)) {
        /* Value is zero */
        out[0] = 0x80U;
        return 1U;
    }
    return rlp_bytes(data + first, 32U - first, out);
}

/*
 * Write the RLP list header for a list whose content is content_len bytes.
 * Returns bytes written (does NOT write the content).
 */
static size_t rlp_list_header(size_t content_len, uint8_t *out)
{
    if (content_len <= 55U) {
        out[0] = (uint8_t)(0xC0U + content_len);
        return 1U;
    }
    uint8_t len_bytes[8];
    uint8_t num_lb = be_minimal((uint64_t)content_len, len_bytes);
    out[0] = (uint8_t)(0xF7U + num_lb);
    (void)memcpy(out + 1U, len_bytes, num_lb);
    return 1U + num_lb;
}

/******************************************************************
 * Shared field encoder
 ******************************************************************/

static size_t encode_common_fields(const eth_tx_t *tx, uint8_t *items)
{
    size_t n = 0U;
    n += rlp_uint64(tx->chain_id,          items + n);
    n += rlp_uint64(tx->nonce,             items + n);
    n += rlp_uint64(tx->max_priority_fee,  items + n);
    n += rlp_uint64(tx->max_fee,           items + n);
    n += rlp_uint64(tx->gas_limit,         items + n);
    n += rlp_bytes(tx->to, 20U,            items + n);
    n += rlp_uint64(tx->eth_value,         items + n);
    n += rlp_bytes(tx->calldata, tx->calldata_len, items + n);
    items[n++] = 0xC0U;   /* empty access list */
    return n;
}

/******************************************************************
 * Public API
 ******************************************************************/

size_t eth_rlp_encode_unsigned(const eth_tx_t *tx, uint8_t *out, size_t out_max)
{
    uint8_t items[ITEMS_BUF_MAX];
    size_t  items_len = encode_common_fields(tx, items);

    uint8_t hdr[10];
    size_t  hdr_len = rlp_list_header(items_len, hdr);

    size_t total = 1U + hdr_len + items_len;   /* 0x02 prefix + list */
    if (total > out_max) { return 0U; }

    out[0] = 0x02U;                             /* EIP-1559 type prefix */
    (void)memcpy(out + 1U,          hdr,   hdr_len);
    (void)memcpy(out + 1U + hdr_len, items, items_len);
    return total;
}

size_t eth_rlp_encode_signed(const eth_tx_t *tx, uint8_t v,
                              const uint8_t r[32], const uint8_t s[32],
                              uint8_t *out, size_t out_max)
{
    uint8_t items[ITEMS_BUF_MAX];
    size_t  items_len = encode_common_fields(tx, items);

    items_len += rlp_uint64((uint64_t)v,  items + items_len);
    items_len += rlp_int256(r,             items + items_len);
    items_len += rlp_int256(s,             items + items_len);

    uint8_t hdr[10];
    size_t  hdr_len = rlp_list_header(items_len, hdr);

    size_t total = 1U + hdr_len + items_len;
    if (total > out_max) { return 0U; }

    out[0] = 0x02U;
    (void)memcpy(out + 1U,           hdr,   hdr_len);
    (void)memcpy(out + 1U + hdr_len, items, items_len);
    return total;
}
