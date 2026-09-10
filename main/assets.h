/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file assets.h
 * @ingroup device
 * @brief What each selectable asset IS — one row per @ref pos_chain_t.
 *
 * The nine selections used to be described by four parallel switches in ui.cpp
 * (ticker, network name, address-row caption, icon pair) plus three hand-written
 * picker tables, so adding one asset meant ten coordinated edits and a missed one
 * shipped a row that rendered wrong or named the wrong contract. The facts are
 * data, so they live in a table and the switches became lookups.
 *
 * Deliberately NOT here:
 *
 *   - the family predicates (pos_chain_is_tron and friends). They stay in
 *     settings.h where they are already pinned by tests/units/test_chain.cpp,
 *     and they steer signing paths rather than describing an asset.
 *   - the icons. Those are lv_img_dsc_t, and this header is kept free of LVGL
 *     and of ESP-IDF so a host test can reach it (tests/units/test_assets.cpp).
 *     ui.cpp maps ticker and network to an image; the table says which.
 *   - which storage slot holds a token's contract. That is main.cpp's business
 *     (see active_erc20_token) and the stores are different types.
 *
 * The table is indexed by nothing: it is ordered for the picker — grouped by
 * network, native coin first, exactly as the three tables it replaced were — and
 * looked up by @ref pos_asset_of. test_assets.cpp proves every enumerator appears
 * exactly once, which is the check that makes a table trustworthy in place of ten
 * remembered edit sites.
 */

#ifndef ASSETS_H
#define ASSETS_H

#include <stdbool.h>
#include <stddef.h>

#include "settings.h"   /* pos_chain_t and the family predicates */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A network, as the picker's first step offers it.
 *
 * Not @ref pos_chain_t: several chains share one network (USDC and USDT are both
 * Ethereum), and step 1 of the picker asks for the network alone.
 */
typedef enum {
    POS_NET_ETH = 0,
    POS_NET_POLY,
    POS_NET_TRON,
    POS_NET__COUNT
} pos_net_t;

/** @brief One selectable asset. */
typedef struct {
    pos_chain_t chain;
    const char *ticker;    /**< "USDC" — the selector pill and the amount row. */
    const char *standard;  /**< "ERC-20" — the coin picker's row subtitle.     */
    /**
     * Caption over the contract address, on the confirm screen and the Tx tab.
     * A network's own coin has no contract to show, so its row names the asset
     * instead. Spelled out per asset rather than derived from the ticker: the
     * Tron rows say "Token contract" where the EVM ones name the token, and that
     * is a wording decision, not a rule.
     */
    const char *caption;
    pos_net_t   net;
    bool        native;    /**< the network's own coin, not a token */
} pos_asset_t;

/**
 * @brief Names for one network, in both deployments.
 *
 * The long forms name the deployment, because "Ethereum" and "Ethereum Sepolia"
 * differ by the only thing that decides whether a sale settles in money. The
 * mainnet forms carry no suffix on purpose — a production terminal should not be
 * shouting a word nobody needs, and the testnet ones then stand out.
 */
typedef struct {
    const char *name;       /**< "Ethereum" — the picker's row title.          */
    const char *long_test;  /**< "Ethereum Sepolia" — the selector's subtitle. */
    const char *long_main;  /**< "Ethereum".                                   */
    const char *sub_test;   /**< "Sepolia testnet" — the picker's row subtitle.*/
    const char *sub_main;   /**< "Mainnet".                                    */
} pos_net_info_t;

/* Grouped by network, native coin first within each. The picker walks this in
 * order and filters on `net`, so the order here IS the order on screen. */
static const pos_asset_t POS_ASSETS[] = {
    { POS_CHAIN_ETH_NATIVE,  "ETH",  "Native coin", "Asset",
      POS_NET_ETH,  true  },
    { POS_CHAIN_ETH_SEPOLIA, "USDC", "ERC-20",      "USDC contract",
      POS_NET_ETH,  false },
    { POS_CHAIN_ETH_USDT,    "USDT", "ERC-20",      "USDT contract",
      POS_NET_ETH,  false },

    { POS_CHAIN_POLY_NATIVE, "POL",  "Native coin", "Asset",
      POS_NET_POLY, true  },
    { POS_CHAIN_POLY_USDC,   "USDC", "ERC-20",      "USDC contract",
      POS_NET_POLY, false },
    { POS_CHAIN_POLY_USDT,   "USDT", "ERC-20",      "USDT contract",
      POS_NET_POLY, false },

    { POS_CHAIN_TRON_NILE,   "TRX",  "Native coin", "Asset",
      POS_NET_TRON, true  },
    { POS_CHAIN_TRON_USDT,   "USDT", "TRC-20",      "Token contract",
      POS_NET_TRON, false },
    { POS_CHAIN_TRON_USDC,   "USDC", "TRC-20",      "Token contract",
      POS_NET_TRON, false },
};

#define POS_ASSET_COUNT  (sizeof(POS_ASSETS) / sizeof(POS_ASSETS[0]))

static const pos_net_info_t POS_NETS[POS_NET__COUNT] = {
    { "Ethereum", "Ethereum Sepolia", "Ethereum", "Sepolia testnet", "Mainnet" },
    { "Polygon",  "Polygon Amoy",     "Polygon",  "Amoy testnet",    "Mainnet" },
    { "Tron",     "Tron Nile",        "Tron",     "Nile testnet",    "Mainnet" },
};

/**
 * @brief The row describing @p chain.
 *
 * Never NULL for a value that came out of settings_get_chain(), which already
 * rejects anything past @ref POS_CHAIN__COUNT — but the fallback is the default
 * asset rather than a NULL a caller has to remember to test, for the same reason
 * settings_get_chain() falls back rather than failing: a terminal that renders the
 * wrong ticker is recoverable, one that dereferences NULL on the amount screen is
 * not.
 */
static inline const pos_asset_t *pos_asset_of(pos_chain_t chain)
{
    for (size_t i = 0U; i < POS_ASSET_COUNT; i++) {
        if (POS_ASSETS[i].chain == chain) { return &POS_ASSETS[i]; }
    }
    return &POS_ASSETS[0];
}

/** @brief Which network @p chain is on. */
static inline pos_net_t pos_net_of(pos_chain_t chain)
{
    return pos_asset_of(chain)->net;
}

/** @brief Names for @p net, or Ethereum's for a value out of range. */
static inline const pos_net_info_t *pos_net_info(pos_net_t net)
{
    return ((int)net >= 0 && net < POS_NET__COUNT) ? &POS_NETS[net]
                                                   : &POS_NETS[POS_NET_ETH];
}

#ifdef __cplusplus
}
#endif

#endif /* ASSETS_H */
