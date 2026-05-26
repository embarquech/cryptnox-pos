#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EIP-1559 (type 2) transaction parameters. */
typedef struct {
    uint64_t       chain_id;
    uint64_t       nonce;
    uint64_t       max_priority_fee;  /* wei */
    uint64_t       max_fee;           /* wei */
    uint64_t       gas_limit;
    uint8_t        to[20];            /* recipient Ethereum address */
    uint64_t       eth_value;         /* wei (0 for pure ERC-20 transfer) */
    const uint8_t *calldata;
    size_t         calldata_len;
} eth_tx_t;

/*
 * Encode an unsigned EIP-1559 transaction: 0x02 || RLP([chainId, nonce, ...]).
 * Returns total bytes written, or 0 on overflow.
 */
size_t eth_rlp_encode_unsigned(const eth_tx_t *tx, uint8_t *out, size_t out_max);

/*
 * Encode a signed EIP-1559 transaction: 0x02 || RLP([..., v, r, s]).
 * v must be 0 or 1.  r and s are 32-byte big-endian values.
 * Returns total bytes written, or 0 on overflow.
 */
size_t eth_rlp_encode_signed(const eth_tx_t *tx, uint8_t v,
                              const uint8_t r[32], const uint8_t s[32],
                              uint8_t *out, size_t out_max);

#ifdef __cplusplus
}
#endif
