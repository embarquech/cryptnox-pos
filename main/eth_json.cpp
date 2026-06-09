/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_json.cpp
 * @brief Implementation of the JSON-RPC response parsing helpers.
 */

#include "eth_json.h"

#include <string.h>

#include "CW_Utils.h"   /* hardened memory primitives (CODING_RULES §1.4) */
#include "cJSON.h"

bool eth_json_result_string(const char *resp, char *out, size_t out_size)
{
    bool ok = false;
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        return false;
    }
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsString(result) && (result->valuestring != NULL)) {
        size_t len = strlen(result->valuestring);
        if ((len + 1U) <= out_size) {
            ok = CW_Utils::safe_memcpy(
                reinterpret_cast<uint8_t *>(out), out_size,
                reinterpret_cast<const uint8_t *>(result->valuestring),
                len + 1U);
        }
    }
    cJSON_Delete(root);
    return ok;
}

eth_json_receipt_t eth_json_receipt_status(const char *resp)
{
    eth_json_receipt_t verdict = ETH_JSON_RECEIPT_ERROR;
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        return ETH_JSON_RECEIPT_ERROR;
    }
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsNull(result)) {
        verdict = ETH_JSON_RECEIPT_PENDING;   /* not mined yet */
    } else if (cJSON_IsObject(result)) {
        const cJSON *status =
            cJSON_GetObjectItemCaseSensitive(result, "status");
        if (cJSON_IsString(status) && (status->valuestring != NULL)) {
            verdict = (strcmp(status->valuestring, "0x1") == 0)
                          ? ETH_JSON_RECEIPT_SUCCESS
                          : ETH_JSON_RECEIPT_REVERTED;
        }
    }
    cJSON_Delete(root);
    return verdict;
}
