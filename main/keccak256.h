#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ethereum uses the original Keccak-256 (0x01 padding), NOT SHA3-256 (0x06). */
void keccak256(const uint8_t *input, size_t length, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif
