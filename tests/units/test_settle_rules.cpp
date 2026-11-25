/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 *
 * Host test for main/settle_rules.h — what one poll of an in-flight sale
 * decides. The rule that matters: only a transaction that was never seen, past
 * a deadline the node itself reported, may end as "Nothing charged". Anything
 * else that is not a verdict keeps polling and at worst ends Unconfirmed.
 *
 *   g++ -std=c++14 -Wall -Imain tests/units/test_settle_rules.cpp -o t && ./t
 */

#include <assert.h>
#include <stdio.h>

#include "settle_rules.h"

int main(void)
{
    const sol_status_t X = SOL_STATUS_RPC_ERROR;   /* ignored off Solana */

    /* Verdicts win, past or not, on every chain. */
    for (int past = 0; past < 2; past++) {
        assert(settle_step(ETH_RPC_RECEIPT_SUCCESS,  false, X, past) == SETTLE_PAID);
        assert(settle_step(ETH_RPC_RECEIPT_REVERTED, false, X, past) == SETTLE_REVERTED);
        assert(settle_step(ETH_RPC_RECEIPT_MISMATCH, false, X, past) == SETTLE_MISMATCH);
        assert(settle_step(ETH_RPC_RECEIPT_SUCCESS,  true, SOL_STATUS_SUCCESS, past)
               == SETTLE_PAID);
        assert(settle_step(ETH_RPC_RECEIPT_REVERTED, true, SOL_STATUS_FAILED, past)
               == SETTLE_REVERTED);
    }

    /* Tron/EVM: not found + past = expired; not found before the deadline polls. */
    assert(settle_step(ETH_RPC_RECEIPT_PENDING, false, X, true)  == SETTLE_EXPIRED);
    assert(settle_step(ETH_RPC_RECEIPT_PENDING, false, X, false) == SETTLE_POLL);

    /* An RPC error proves nothing, even past the deadline. */
    assert(settle_step(ETH_RPC_RECEIPT_RPC_ERROR, false, X, true) == SETTLE_POLL);
    assert(settle_step(ETH_RPC_RECEIPT_RPC_ERROR, true, SOL_STATUS_RPC_ERROR, true)
           == SETTLE_POLL);

    /* Solana: never seen + past = expired. */
    assert(settle_step(ETH_RPC_RECEIPT_PENDING, true, SOL_STATUS_UNSEEN, true)
           == SETTLE_EXPIRED);
    assert(settle_step(ETH_RPC_RECEIPT_PENDING, true, SOL_STATUS_UNSEEN, false)
           == SETTLE_POLL);

    /* Solana seen at "processed": may still confirm — never expired, even past
     * the deadline. This is the case that used to read "Nothing charged". */
    assert(settle_step(ETH_RPC_RECEIPT_PENDING, true, SOL_STATUS_PENDING, true)
           == SETTLE_POLL);

    printf("settle_rules OK\n");
    return 0;
}
