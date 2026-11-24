/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file settle_rules.h
 * @brief What one poll of an in-flight sale decides — pure, host-tested
 *        (tests/units/test_settle_rules.cpp). The I/O stays in settle_inflight.
 */

#ifndef SETTLE_RULES_H
#define SETTLE_RULES_H

#include <stdbool.h>

#include "eth_rpc.h"   /* eth_rpc_receipt_result_t */
#include "sol_rpc.h"   /* sol_status_t */

/** @brief The sale after one poll. */
typedef enum {
    SETTLE_POLL,      /**< No verdict: poll again, or end Unconfirmed at the deadline. */
    SETTLE_PAID,      /**< Mined and ours.                                             */
    SETTLE_REVERTED,  /**< Mined, failed: nothing moved.                               */
    SETTLE_MISMATCH,  /**< A receipt that is not our transfer: Unconfirmed, anomaly.  */
    SETTLE_EXPIRED,   /**< Past its deadline and never seen: it can never land.        */
} settle_step_t;

/**
 * @param rc   The poll's verdict, in the EVM terms every chain is mapped onto.
 * @param sol  Whether the sale is Solana; @p ss is then its raw status.
 * @param past Whether the node — read BEFORE the status — says the deadline
 *             has passed (Solana block height, Tron head block time). Always
 *             false on EVM, which has no expiry.
 *
 * Only "never seen" expires. An RPC error says nothing either way, and a Solana
 * transaction seen at "processed" may still confirm — both keep polling, so the
 * worst a bad node can do is end the sale Unconfirmed, never "Nothing charged".
 */
static inline settle_step_t settle_step(eth_rpc_receipt_result_t rc, bool sol,
                                        sol_status_t ss, bool past)
{
    if (rc == ETH_RPC_RECEIPT_SUCCESS)  { return SETTLE_PAID; }
    if (rc == ETH_RPC_RECEIPT_REVERTED) { return SETTLE_REVERTED; }
    if (rc == ETH_RPC_RECEIPT_MISMATCH) { return SETTLE_MISMATCH; }
    const bool unseen = sol ? (ss == SOL_STATUS_UNSEEN)
                            : (rc == ETH_RPC_RECEIPT_PENDING);
    return (past && unseen) ? SETTLE_EXPIRED : SETTLE_POLL;
}

#endif /* SETTLE_RULES_H */
