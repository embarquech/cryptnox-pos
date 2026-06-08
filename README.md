<p align="center">
  <img src="https://github.com/user-attachments/assets/6ce54a27-8fb6-48e6-9d1f-da144f43425a"/>
</p>

<h3 align="center">cryptnox-pos</h3>
<p align="center">Standalone USDC payment terminal powered by a Cryptnox smart card and the Cheap Yellow Display</p>

<br/>
<br/>

[![Platform: ESP32-2432S028 (CYD)](https://img.shields.io/badge/Platform-ESP32--2432S028%20(CYD)-blue.svg)](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)
[![Framework: ESP-IDF v5.5](https://img.shields.io/badge/Framework-ESP--IDF%20v5.5-blue.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.5/)
[![SDK: cryptnox-sdk-esp32](https://img.shields.io/badge/SDK-cryptnox--sdk--esp32-blue.svg)](https://github.com/embarquech/cryptnox-sdk-esp32)
[![License: LGPLv3](https://img.shields.io/badge/License-LGPLv3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)

`cryptnox-pos` is a self-contained payment terminal firmware that runs on the
ESP32-2432S028 "Cheap Yellow Display" (CYD). It also serves as a reference
**dev kit** showing how to integrate the [`cryptnox-sdk-esp32`](https://github.com/embarquech/cryptnox-sdk-esp32)
into a real-world end-user product.

The user selects a USDC amount on the touchscreen, taps a **Cryptnox smart
card** on the attached PN532 reader, and the terminal signs and broadcasts an
EIP-1559 transfer on Ethereum Sepolia via Infura.

> [!WARNING]
> **Reference / educational project — not a production-hardened terminal.**
> The ESP32 is a general-purpose microcontroller, not a tamper-resistant secure
> element: an attacker with physical access can compromise its runtime and
> recover its Wi-Fi credentials, which may then be used to reach the card. The
> card PIN and the destination address are also hard-coded into the build
> config. **The Cryptnox Hardware Wallet remains the trust anchor**, though —
> private keys never leave the card, so a compromised ESP32 still cannot sign
> without the card.

### Built on

- **[`cryptnox-sdk-esp32`](https://github.com/embarquech/cryptnox-sdk-esp32)** (vendored as a git submodule) — secure channel, ECDH/ECDSA, PN532 transport, ESP32 crypto provider
- **[TFT_eSPI](https://github.com/Bodmer/TFT_eSPI)** + **[XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen)** — native CYD display + touch
- **[arduino-esp32](https://github.com/espressif/arduino-esp32)** as an ESP-IDF managed component (Arduino runtime for the UI libraries; the rest of the firmware stays on native IDF)

---

## Supported hardware

### Cryptnox Hardware Wallet smart cards

Works with Cryptnox Hardware Wallet smart cards running firmware v1.6.0 or later.

| Smart card | Wallet version |
|------|---------------|
| [Crypto Hardware Wallet – Dual Card Set](https://shop.cryptnox.com/product/hardware-wallet-smartcard-dual/) | v1.6.1 |

### NFC readers

| Reader | Type | Interface |
|--------|------|-----------|
| [PN532 NFC Module](https://www.nxp.com/products/PN532) | Contactless (NFC/ISO 14443) | I²C |

### Host board

| Board | MCU | Display | Touch |
|-------|-----|---------|-------|
| [ESP32-2432S028 "Cheap Yellow Display"](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) | ESP32-WROOM-32 | ILI9341 320×240 | XPT2046 (resistive) |

---

## Installation

1. Install **[ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/index.html)**
   (the project uses Arduino-as-component plus the `lvgl/lvgl`-free Espressif
   managed components stack).
2. Clone this repository **with submodules**:
   ```
   git clone --recursive https://github.com/embarquech/cryptnox-pos.git
   cd cryptnox-pos
   ```
3. Copy the credentials template and fill it in:
   ```
   cp config.template.h main/config.h
   ```
   `main/config.h` is gitignored — see [Configuration](#configuration) below.
4. Build and flash (Windows PowerShell wrapper provided):
   ```
   .\auto_flash.ps1 set-target esp32
   .\auto_flash.ps1 monitor
   ```
   The first build also fetches `espressif/arduino-esp32` from the IDF
   component registry, so expect a longer initial compile.

> [!TIP]
> On macOS/Linux, source the IDF env (`. $IDF_PATH/export.sh`) and use the
> standard `idf.py set-target esp32`, `idf.py build flash monitor` commands.

## Hardware setup

> [!CAUTION]
> Always double-check the wiring before powering the board to prevent damage.

The CYD exposes its free GPIOs on the **CN1** header. The firmware uses
the **I²C** interface to the PN532.

### CYD CN1 ↔ PN532 NFC — I²C interface

| PN532 Pin | CYD Pin / Label  | Wire Color |
|-----------|------------------|------------|
| GND       | GND              | Black      |
| VCC       | 3.3V             | Red        |
| SDA       | GPIO 27 (CN1 SDA)| Blue       |
| SCL       | GPIO 22 (CN1 SCL)| Yellow     |

> [!IMPORTANT]
> Make sure the switches on the PN532 module are configured for **I²C** mode:
>
> - **Switch 0** → LOW
> - **Switch 1** → HIGH

<img width="800" alt="cyd_pn532_i2c" src="hardware/schematics/cyd_esp32_pn532_i2c_bb.png" />

The display, touchscreen and backlight pins are already wired internally on the
CYD; the firmware drives them through TFT_eSPI's `ILI9341_2_DRIVER` with
landscape rotation (320×240) and a calibrated XPT2046 touch mapping.

---

## Configuration

`main/config.h` is gitignored. Copy `config.template.h` to `main/config.h`
and fill in:

| Field | Description |
|-------|-------------|
| `WIFI_SSID` / `WIFI_PASSWORD` | WPA2 credentials for the terminal's STA connection |
| `RPC_URL` | Ethereum JSON-RPC endpoint (PublicNode Sepolia by default; an Infura variant is provided commented-out) |
| `RPC_PROJECT_ID` / `RPC_API_SECRET` | Optional — only when using Infura (HTTP Basic Auth); leave undefined for PublicNode |
| `ADDR_FROM` | Ethereum address of the **card** (`m/44'/60'/0'/0/0`) — used to fetch the nonce and validate the ecrecover parity |
| `ADDR_TO` | Destination address for every transfer |
| `ADDR_USDC` | USDC ERC-20 contract address on the target chain (Sepolia testnet by default) |
| `CARD_PIN` / `CARD_PIN_LEN` | PIN configured on the Cryptnox card (4–9 ASCII digits) and its length |
| `CHAIN_ID_SEPOLIA`, `MAX_PRIORITY_FEE`, `MAX_FEE`, `GAS_LIMIT_ERC20` | Chain ID and EIP-1559 gas parameters |

The transfer amount is selected on the touchscreen at run time; it is not a
config.h field.

> [!WARNING]
> Set a strong, unique `CARD_PIN` when initialising the card and never commit
> `main/config.h`. The build fails with a `static_assert` if `CARD_PIN` is the
> demo placeholder `"000000000"`; define `CRYPTNOX_POS_ALLOW_DEMO_PIN` only for
> throwaway bench builds, never on a deployed terminal.

---

## Payment flow

1. **Splash** — Cryptnox logo + "Loading…" while WiFi / RPC / wallet come up.
2. **Amount** — touchscreen +1 / −1 / +0.01 / −0.01 buttons; tap **CONFIRM**.
3. **Confirm** — review amount + destination, tap **Send** or **Cancel**.
4. **Transaction** — place the Cryptnox card on the PN532. The terminal
   establishes the secure channel, signs `keccak256(unsigned_tx)`, recovers
   the `v` parity via the `ecrecover` precompile, RLP-encodes the EIP-1559
   transaction and broadcasts it via `eth_sendRawTransaction`.
5. **Sent / Failed** — tap **New payment** to start the next transfer.

> [!NOTE]
> The card must have a seed loaded and the BIP-32 derivation path
> `m/44'/60'/0'/0/0` available. Provision it once with the
> [Cryptnox CLI](https://github.com/cryptnox/cryptnox-cli):
> ```
> cryptnox initialize
> cryptnox seed generate
> ```

---

## Troubleshooting

- **Blank or scrambled screen** → the CYD ships with two ILI9341 variants. The firmware uses TFT_eSPI's `ILI9341_2_DRIVER`; if your board uses the standard variant, set `CONFIG_TFT_ILI9341_DRIVER=y` (instead of `_2`) in `sdkconfig.defaults` and rebuild.
- **Touch hitboxes are offset** → the raw range used by the XPT2046 driver is calibrated for the panel shipped with the 2432S028. If yours differs, adjust the `map(p.x, 200, 3800, …)` ranges in `main/ui.cpp` (function `poll_touch`).
- **`Card not found`** → confirm the PN532 switches are set for I²C, the SDA/SCL wires match GPIO 27/22, and the card is well centred on the antenna.
- **`ecrecover did not match either parity`** → `ADDR_FROM` in `config.h` does not correspond to the card's `m/44'/60'/0'/0/0` derived key. Verify the seed and the path.
- **WiFi connect fails** → only WPA2 is supported; check SSID/password.
- **App partition full** → the project uses a custom 3 MB partition table (`partitions.csv`) on a 4 MB flash. Reduce TFT_eSPI font configs in `sdkconfig.defaults` if you need more headroom.

---

## License

`cryptnox-pos` is dual-licensed:

- **LGPL-3.0** for open-source projects and proprietary projects that comply with LGPL requirements
- **Commercial license** for projects that require a proprietary license without LGPL obligations

For commercial inquiries, contact: contact@cryptnox.com
