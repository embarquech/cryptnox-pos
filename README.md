<h3 align="center">cryptnox-pos</h3>
<p align="center">Standalone USDC payment terminal powered by a Cryptnox smart card and the Cheap Yellow Display</p>

<br/>
<br/>

[![Platform: ESP32-2432S028R (CYD)](https://img.shields.io/badge/Platform-ESP32--2432S028R%20(CYD)-blue.svg)](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)
[![Framework: ESP-IDF v5.5](https://img.shields.io/badge/Framework-ESP--IDF%20v5.5-blue.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.5/)
[![License: LGPLv3](https://img.shields.io/badge/License-LGPLv3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)

`cryptnox-pos` is a self-contained payment terminal firmware that runs on the
ESP32-2432S028R "Cheap Yellow Display" (CYD). The user selects a USDC amount
on the touchscreen, taps a **Cryptnox smart card** on the attached PN532
reader, and the terminal signs and broadcasts an EIP-1559 transfer on
Ethereum Sepolia via Infura.

The firmware sits on top of [`cryptnox-sdk-esp32`](https://github.com/embarquech/cryptnox-sdk-esp32)
(secure channel, ECDH/ECDSA, PN532 transport) and pairs it with TFT_eSPI +
XPT2046_Touchscreen for a native CYD UI.

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
| [ESP32-2432S028R "Cheap Yellow Display"](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) | ESP32-WROOM-32 | ILI9341 320×240 | XPT2046 (resistive) |

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

| PN532 Pin | CYD Pin / Label | Wire Color |
|-----------|------------------|------------|
| VCC       | 3.3V             | Red        |
| GND       | GND              | Black      |
| SDA       | GPIO 27 (CN1 SDA)| Yellow     |
| SCL       | GPIO 22 (CN1 SCL)| Blue       |

> [!IMPORTANT]
> Make sure the switches on the PN532 module are configured for **I²C** mode:
>
> - **Switch 0** → LOW
> - **Switch 1** → HIGH

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
| `RPC_URL` | Ethereum JSON-RPC endpoint (Sepolia by default) |
| `RPC_PROJECT_ID` / `RPC_API_SECRET` | Infura project credentials (Basic Auth) |
| `ADDR_FROM` | Ethereum address of the **card** (`m/44'/60'/0'/0/0`) — used to fetch the nonce and validate the ecrecover parity |
| `ADDR_TO` | Destination address for every transfer |
| `ADDR_USDC` | USDC ERC-20 contract address on the target chain (Sepolia testnet by default) |
| `CARD_PIN` | PIN configured on the Cryptnox card (default `"000000000"` for demo) |
| `AMOUNT_USDC` | Unused; the on-screen UI is the source of truth for the amount |

> [!WARNING]
> The PIN `"000000000"` is a demo placeholder. Set a strong PIN when
> initialising the card, change any factory default before storing real
> funds, and never commit `main/config.h`.

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
- **Touch hitboxes are offset** → the raw range used by the XPT2046 driver is calibrated for the panel shipped with the 2432S028R. If yours differs, adjust the `map(p.x, 200, 3800, …)` ranges in `main/ui.cpp` (function `poll_touch`).
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
