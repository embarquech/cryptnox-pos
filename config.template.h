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
/* Wi-Fi is NOT set here. It is chosen on the device during setup — from a browser,
 * out of a list the terminal scans for itself — and stored in NVS. No SSID or
 * password is baked into the firmware. See docs/config-portal.md. */

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

/* FALLBACK recipient address — where transfers go until an operator sets one on
 * the device (no 0x). Setup asks for the real one and stores it in NVS, either
 * typed in a browser or read off the operator's Cryptnox card; this value is what
 * a terminal pays to before that happens, so it is worth being a real address of
 * yours rather than a placeholder.
 *
 * MUST be in EIP-55 mixed-case checksum form: the firmware verifies the checksum
 * at boot and refuses to start on a mismatch, catching the most probable typo of
 * the recipient. An all-lowercase address is accepted but bypasses that typo
 * protection (and logs a warning at boot).
 *
 * Note that an asset whose payout address was never *stored* is not offered on the
 * amount screen at all — the fallback keeps a configured terminal working, it does
 * not make an unconfigured one sell things. See docs/config-portal.md. */
#define ADDR_TO           "<RECIPIENT_ADDRESS_EIP55>"

/* FALLBACK USDC ERC-20 contract address (Sepolia testnet, no 0x). Also settable
 * from the config page, with the same dual-store and on-screen accept as the
 * recipient — the contract decides which asset moves.
 * Same rule as ADDR_TO — use the EIP-55 mixed-case form for boot-time
 * checksum verification. */
#define ADDR_USDC         "<USDC_CONTRACT_ADDRESS_EIP55>"

/* =========================
 * Tron (Nile testnet)
 * ========================= */
/* Native TRX and TRC-20 (USDT / USDC) transfers, selectable at runtime from the
 * amount screen. TronGrid's public Nile endpoint needs no API key for these. */
#define TRON_URL          "https://nile.trongrid.io"

/* Optional, and worth setting in production: pin the Tron endpoint's certificate
 * (leaf or its issuing CA, PEM) so the connection is validated against that alone
 * instead of the ~150-CA Mozilla bundle. Same form as RPC_CA_CERT_PEM above.
 *
 * Leaving it unset is not a hole — a Tron terminal signs a txID the node builds,
 * so the node is treated as hostile regardless and every transaction it returns
 * is re-derived and compared before the card sees it. Pinning just means an
 * attacker has to defeat that check AND TLS.
 *
 * #define TRON_CA_CERT_PEM  "-----BEGIN CERTIFICATE-----\n...\n" \
 *                           "-----END CERTIFICATE-----\n"
 */

/* --- Tron addresses (Nile testnet) ---------------------------------
 * FALLBACK recipient, in base58 "T..." form, exactly as a Tron wallet shows it —
 * same story as ADDR_TO above: setup stores the real one in NVS and this is what a
 * terminal pays to until it does. The device decodes whichever is in use at boot
 * (checksum verified, so a typo is refused rather than paid) and derives the
 * "41"-prefixed hex the HTTP API wants.
 *
 * Only the recipient is configured. The sender is whatever card is presented:
 * its m/44'/195'/0'/0/0 public key is read over the secure channel and turned
 * into an address by CW_Tron, so swapping cards needs no rebuild. */
#define TRON_ADDR_TO      "THQGuFzL87ZqhxkgqYEryRAd7gqFqL5rdc"

/* --- TRC-20 token contract (Nile testnet) ---------------------------
 * FALLBACK again — settable from the config page. Base58 "T..." form. Confirm the
 * current Nile deployment on
 * https://nile.tronscan.org before trusting this — testnet token contracts get
 * redeployed, and a stale address decodes fine while paying the wrong asset.
 * The contract below answered symbol "USDT" / decimals 6 on 2026-08-07; the
 * amount keypad assumes 6 decimals, so re-check that too if you change it.
 *
 * Checksum-decoded at boot. Unlike TRON_ADDR_TO this is NOT fatal: a bad value
 * only disables the asset in the picker, so a terminal that charges in TRX or
 * on Sepolia still boots.
 *
 * There is deliberately no USDC-on-Tron entry — Circle stopped minting it in
 * February 2024 and closed redemptions in February 2025. */
#define TRON_ADDR_USDT    "TXYZopYRdj2D9XRtbG411XZZ3kM5VkAeBf"

/* Ceiling, in sun, on the TRX a TRC-20 call may burn when the card's account
 * has no energy staked. A transfer to an address that has never held the token
 * is the expensive case (~130k energy). This is signed into the transaction and
 * re-checked against the node's serialisation, so it is a real cap, not a hint.
 * 100 TRX is the usual wallet default. */
#define TRON_TRC20_FEE_LIMIT_SUN  100000000ULL


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
