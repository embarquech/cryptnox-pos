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
#define RPC_URL        "https://" RPC_HOST
/* No authentication needed — leave RPC_PROJECT_ID / RPC_API_SECRET
 * undefined (or commented out) when using PublicNode.               */

/* --- Option B: Infura --------------------------------------------- */
/* #define RPC_HOST        "sepolia.infura.io"                        */
/* #define RPC_PORT        443                                        */
/* #define RPC_PROJECT_ID  "<YOUR_INFURA_PROJECT_ID>"                 */
/* #define RPC_URL         "https://sepolia.infura.io/v3/" RPC_PROJECT_ID */
/* #define RPC_API_SECRET  "<YOUR_INFURA_API_SECRET>"                 */

/* --- Optional: pin the RPC TLS certificate ----------- */
/* By default the HTTPS chain is validated against the full Mozilla CA bundle
 * (~150 CAs, any of which could MITM). Define RPC_CA_CERT_PEM with the
 * endpoint's certificate (leaf, or — more stable across renewals — its issuing
 * CA) to trust ONLY that. Fetch it with:
 *   openssl s_client -connect ethereum-sepolia-rpc.publicnode.com:443 -showcerts </dev/null
 * then paste the relevant PEM block as a C string literal, e.g.:
 *   #define RPC_CA_CERT_PEM "-----BEGIN CERTIFICATE-----\n" \
 *                           "MIIB...\n" \
 *                           "-----END CERTIFICATE-----\n"
 * Note: pinning ties the build to that endpoint — update it if you change RPC. */

/* The card PIN is NOT set here — the operator types it on the touchscreen
 * keypad at sign time, and it is scrubbed from RAM right after signing.
 * Nothing PIN-related is baked into the firmware. */

/* =========================
 * Ethereum Addresses
 * ========================= */
/* Sender address — the card's m/44'/60'/0'/0/0 key, lowercase hex, no 0x */
#define ADDR_FROM         "<SENDER_ADDRESS>"

/* Recipient address — destination of every transfer (no 0x).
 * MUST be in EIP-55 mixed-case checksum form: the firmware verifies the
 * checksum at boot and refuses to start on a mismatch, catching the most
 * probable typo of the recipient. An all-lowercase address is accepted but
 * bypasses that typo protection (and logs a warning at boot). */
#define ADDR_TO           "<RECIPIENT_ADDRESS_EIP55>"

/* USDC contract address (Sepolia testnet, no 0x).
 * Same rule as ADDR_TO — use the EIP-55 mixed-case form for boot-time
 * checksum verification. */
#define ADDR_USDC         "<USDC_CONTRACT_ADDRESS_EIP55>"

/* =========================
 * Tron (Nile testnet)
 * ========================= */
/* Native TRX transfers, selectable at runtime from the settings menu.
 * TronGrid's public Nile endpoint needs no API key for these calls. */
#define TRON_URL          "https://nile.trongrid.io"

/* --- Tron addresses (Nile testnet) ---------------------------------
 * Base58 "T..." form, exactly as a Tron wallet shows it — paste it here, and
 * the terminal displays this same string on the confirm screen. The device
 * decodes it at boot (checksum verified, so a typo is refused rather than paid)
 * and derives the "41"-prefixed hex the HTTP API wants.
 *
 * Only the recipient is configured. The sender is whatever card is presented:
 * its m/44'/195'/0'/0/0 public key is read over the secure channel and turned
 * into an address by CW_Tron, so swapping cards needs no rebuild. */
#define TRON_ADDR_TO      "THQGuFzL87ZqhxkgqYEryRAd7gqFqL5rdc"


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
