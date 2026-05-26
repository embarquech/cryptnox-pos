#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* =========================
 * WiFi Configuration
 * ========================= */
#define WIFI_SSID        "<YOUR_SSID>"
#define WIFI_PASSWORD    "<YOUR_PASSWORD>"

/* =========================
 * Ethereum / RPC
 * ========================= */
#define RPC_HOST          "<RPC_HOST>"          // e.g., sepolia.infura.io
#define RPC_PORT          443                   // HTTPS port
#define INFURA_PROJECT_ID "<INFURA_PROJECT_ID>"

/* =========================
 * Wallet / Keys (SENSITIVE)
 * ========================= */
/**
 * ⚠️ NEVER COMMIT THIS FILE
 * Add config.h to .gitignore
 */
/* =========================
 * Ethereum Addresses
 * ========================= */
// Sender address (checksummed, no 0x)
#define ADDR_FROM         "<SENDER_ADDRESS>"

// Recipient address
#define ADDR_TO           "<RECIPIENT_ADDRESS>"

// USDC contract address (Sepolia testnet)
#define ADDR_USDC         "<USDC_CONTRACT_ADDRESS>"

/* =========================
 * Transaction Parameters
 * ========================= */
#define CHAIN_ID_SEPOLIA  11155111

// Amount in token smallest unit (6 decimals for USDC)
#define AMOUNT_USDC       1000000UL  // 1.0 USDC

// Gas parameters (in wei)
#define MAX_PRIORITY_FEE  2000000000ULL  // 2 Gwei
#define MAX_FEE           4000000000ULL  // 4 Gwei
#define GAS_LIMIT_ERC20   60000ULL

#endif /* CONFIG_H */
