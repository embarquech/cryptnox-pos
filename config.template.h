/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file config.template.h
 * @brief Build-time configuration template — copy to main/config.h and fill
 *        in (main/config.h is gitignored, NEVER commit it).
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* =========================
 * WiFi
 * ========================= */
/* Wi-Fi is NOT set here. It is provisioned on the device at first boot (the
 * touchscreen network picker) and stored in NVS — no SSID/password is baked
 * into the firmware. */

/* =========================
 * Ethereum / RPC
 * ========================= */
/**
 * Choose ONE provider below and comment out the other.
 *
 * Option A — PublicNode (free, no account required)
 *   Keep the lines under "Option A" as-is.
 *
 * Option B — Infura (requires a free account at app.infura.io)
 *   1. Create an API key in the Infura dashboard.
 *   2. In the key's Settings tab, reveal (or generate) the API Secret.
 *   3. Comment out Option A, uncomment the lines under "Option B" and
 *      fill in the values (the API Secret must have NO leading or
 *      trailing spaces).
 */

/* --- Option A: PublicNode ----------------------------------------- */
#define RPC_HOST       "ethereum-sepolia-rpc.publicnode.com"
#define RPC_PORT       443
#define RPC_URL        "https://" RPC_HOST
/* No authentication needed — leave RPC_PROJECT_ID / RPC_API_SECRET
 * undefined (or commented out) when using PublicNode.               */

/* --- Option B: Infura --------------------------------------------- */
/* #define RPC_HOST        "sepolia.infura.io"                        */
/* #define RPC_PORT        443                                        */
/* #define RPC_PROJECT_ID  "<YOUR_INFURA_PROJECT_ID>"                 */
/* #define RPC_URL         "https://sepolia.infura.io/v3/" RPC_PROJECT_ID */
/* #define RPC_API_SECRET  "<YOUR_INFURA_API_SECRET>"                 */

/* The card PIN is NOT set here — the operator types it on the touchscreen
 * keypad at sign time, and it is scrubbed from RAM right after signing.
 * Nothing PIN-related is baked into the firmware. */

/* =========================
 * Ethereum Addresses
 * ========================= */
/* Sender address — the card's m/44'/60'/0'/0/0 key, lowercase hex, no 0x */
#define ADDR_FROM         "<SENDER_ADDRESS>"

/* Recipient address — destination of every transfer (no 0x) */
#define ADDR_TO           "<RECIPIENT_ADDRESS>"

/* USDC contract address (Sepolia testnet, no 0x) */
#define ADDR_USDC         "<USDC_CONTRACT_ADDRESS>"

/* =========================
 * Transaction Parameters
 * ========================= */
#define CHAIN_ID_SEPOLIA  11155111

/* Gas parameters (in wei). MAX_FEE / MAX_PRIORITY_FEE are only the first-boot
 * defaults — they're editable at runtime from the settings "Tx" tab (in Gwei)
 * and persisted to NVS. GAS_LIMIT_ERC20 stays compile-time. */
#define MAX_PRIORITY_FEE  20000000000ULL   /* 20 Gwei — tip to the validator   */
#define MAX_FEE           80000000000ULL   /* 80 Gwei — absolute cap           */
#define GAS_LIMIT_ERC20   100000ULL

#endif // CONFIG_H
