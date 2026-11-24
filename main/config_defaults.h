/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file config_defaults.h
 * @brief config.h, plus a default for every key added after the first config.h
 *        files shipped — so an older config.h still builds.
 */

#ifndef CONFIG_DEFAULTS_H
#define CONFIG_DEFAULTS_H

#include "config.h"

/* Tron TRC-20 support post-dates the first config.h files in the field. An
 * empty contract fails its base58 decode at boot, which disables that asset
 * rather than breaking the build of a config that predates it. */
#ifndef TRON_ADDR_USDT
#define TRON_ADDR_USDT  ""
#endif
/* Same story for every asset added since: an empty contract fails its parse at
 * boot and only that one asset is refused, so a config.h written before it
 * existed still builds and still charges in whatever it was set up for. */
#ifndef TRON_ADDR_USDC
#define TRON_ADDR_USDC  ""
#endif
#ifndef ADDR_USDT
#define ADDR_USDT  ""
#endif
#ifndef POLY_ADDR_USDC
#define POLY_ADDR_USDC  ""
#endif
#ifndef POLY_ADDR_USDT
#define POLY_ADDR_USDT  ""
#endif
/* Polygon needs an endpoint and a chain id of its own. A config.h that predates
 * Polygon support falls back to the Ethereum ones, which is harmless: its assets
 * have no contract either, so they are refused before anything is signed. */
#ifndef POLY_RPC_URL
#define POLY_RPC_URL  RPC_URL
#endif
#ifndef CHAIN_ID_AMOY
#define CHAIN_ID_AMOY  CHAIN_ID_SEPOLIA
#endif
#ifndef POLY_MIN_PRIORITY_FEE_GWEI
#define POLY_MIN_PRIORITY_FEE_GWEI  30U
#endif
/* A plain value transfer to an account costs exactly 21000 gas — no contract
 * runs — so GAS_LIMIT_ERC20's allowance for a token's storage writes is not
 * merely generous here, it is the wrong number. */
#ifndef GAS_LIMIT_NATIVE
#define GAS_LIMIT_NATIVE  21000ULL
#endif
#ifndef TRON_TRC20_FEE_LIMIT_SUN
#define TRON_TRC20_FEE_LIMIT_SUN  100000000ULL
#endif
/* The production networks. No fallback: the terminal boots into mainnet by
 * default (and after every update), so a config.h that names no mainnet endpoint
 * used to ship one silently — testnet URLs on the "Production" switch, or the
 * public Solana pool. The build stops instead; copy the keys from
 * config.template.h. The empty contracts below are the usual "this asset was
 * never set up": refused at boot, with the rest still working. */
#if !defined(RPC_URL_MAIN) || !defined(CHAIN_ID_MAINNET)
#error "config.h: define RPC_URL_MAIN and CHAIN_ID_MAINNET (see config.template.h)"
#endif
#if !defined(POLY_RPC_URL_MAIN) || !defined(CHAIN_ID_POLYGON)
#error "config.h: define POLY_RPC_URL_MAIN and CHAIN_ID_POLYGON (see config.template.h)"
#endif
#ifndef TRON_URL_MAIN
#error "config.h: define TRON_URL_MAIN (see config.template.h)"
#endif
#ifndef SOL_URL_MAIN
#error "config.h: define SOL_URL_MAIN - your own provider, not the public pool (see config.template.h)"
#endif
#ifndef ADDR_USDC_MAIN
#define ADDR_USDC_MAIN  ""
#endif
#ifndef ADDR_USDT_MAIN
#define ADDR_USDT_MAIN  ""
#endif
#ifndef POLY_ADDR_USDC_MAIN
#define POLY_ADDR_USDC_MAIN  ""
#endif
#ifndef POLY_ADDR_USDT_MAIN
#define POLY_ADDR_USDT_MAIN  ""
#endif
#ifndef TRON_ADDR_USDT_MAIN
#define TRON_ADDR_USDT_MAIN  ""
#endif
#ifndef TRON_ADDR_USDC_MAIN
#define TRON_ADDR_USDC_MAIN  ""
#endif

/* Solana post-dates every config.h in the field: the test network defaults to
 * the public Devnet (SOL_URL_MAIN is required, above), and no fallback recipient,
 * so Solana shows "No payout set" until an address is proposed from the config
 * page and accepted on the panel. */
#ifndef SOL_URL
#define SOL_URL  "https://api.devnet.solana.com"
#endif
#ifndef SOL_ADDR_TO
#define SOL_ADDR_TO  ""
#endif

#endif /* CONFIG_DEFAULTS_H */
