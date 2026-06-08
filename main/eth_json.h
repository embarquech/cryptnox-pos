/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_json.h
 * @brief JSON-RPC response parsing helpers — kept in their own unit (cJSON +
 *        CW_Utils only, no ESP-IDF networking) so they can be fuzzed on the
 *        host (see fuzz/fuzz_eth_rpc_json.cpp).
 */

#ifndef ETH_JSON_H
#define ETH_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Verdict for an @c eth_getTransactionReceipt response body. */
typedef enum {
    ETH_JSON_RECEIPT_ERROR = 0, /**< Not valid JSON, or an unexpected shape
                                     (caller should treat as a transient RPC
                                     error and retry).                       */
    ETH_JSON_RECEIPT_PENDING,   /**< @c result is null — not mined yet.       */
    ETH_JSON_RECEIPT_SUCCESS,   /**< @c result.status == "0x1".               */
    ETH_JSON_RECEIPT_REVERTED,  /**< @c result is an object but status != 0x1.*/
} eth_json_receipt_t;

/**
 * @brief Extract the top-level @c "result" string from a JSON-RPC response.
 *
 * Uses a real JSON parser (cJSON) instead of strstr so that JSON-RPC error
 * objects, HTTP error bodies, or look-alike substrings are rejected (F-09).
 *
 * @param[in]  resp     NUL-terminated response body.
 * @param[out] out      NUL-terminated "result" value on success; untouched
 *                      on failure.
 * @param[in]  out_size Capacity of @p out.
 * @return true on success; false if the body is not valid JSON, "result" is
 *         absent or not a string, or the value does not fit in @p out.
 */
bool eth_json_result_string(const char *resp, char *out, size_t out_size);

/**
 * @brief Classify an @c eth_getTransactionReceipt response body.
 *
 * Parses the JSON and inspects the top-level @c result: null means the tx is
 * still pending; an object's @c status field ("0x1" = success, anything else =
 * reverted) gives the final verdict. Any other shape (invalid JSON, result
 * neither null nor object, missing/non-string status) yields
 * @ref ETH_JSON_RECEIPT_ERROR.
 *
 * @param[in] resp NUL-terminated response body.
 * @return One of @ref eth_json_receipt_t.
 */
eth_json_receipt_t eth_json_receipt_status(const char *resp);

#ifdef __cplusplus
}
#endif

#endif /* ETH_JSON_H */
