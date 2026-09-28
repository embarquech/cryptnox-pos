# Improvement plan

Project review of 2026-09-25 (branch `preview`), covering security, payment
correctness, runtime reliability, architecture, tests, docs and build. Findings
were checked against the code; the ones marked *plausible* still need a
reproduction.

Item numbers are stable — refer to them in commits and PRs ("plan #2").
Item 20 (version numbering / release script) is intentionally left out of this
note.

**Status legend:** ☐ open · ◐ in progress · ☑ done

---

## Already done — UI typography

☑ **Fonts: Plus Jakarta Sans (primary) + Inter (secondary)**

| Where | Role | Face |
|---|---|---|
| Panel | Titles, messages | Plus Jakarta Sans Medium 20 |
| Panel | Button labels, amount cents | Plus Jakarta Sans SemiBold 20 |
| Panel | Amounts | Plus Jakarta Sans SemiBold 28 |
| Panel | Keypad digits | Plus Jakarta Sans Light 28 |
| Panel | Captions, body, hints, errors | Inter Regular 14 |
| Panel | Values (addresses, SSID, ticker, clock, tabs, TEST chip) | Inter Medium 14 |
| Panel | ✓ / ✕ / ⚠ status marks | FontAwesome-only 48 px |
| Portal | Brand, headings, buttons | Plus Jakarta Sans 600 |
| Portal | Body / labels / `<b>` | Inter 400 / 500 / 600 |

- Sources: `assets/fonts/*.ttf` with their OFL licences.
- Panel fonts: `tools/gen_fonts.py` → `main/fonts/*.c` (LVGL, 4 bpp, ASCII +
  `°` `•` + the full `LV_SYMBOL_*` set).
- Portal fonts: `tools/gen_portal_fonts.py` → `main/portal_fonts.h` (WOFF2
  subsets as data URIs, ~42 KB — the captive portal has no internet).
- Montserrat removed from `sdkconfig.defaults`; theme default font set in
  `theme_init()` (Kconfig default is now UNSCII 8, unused).
- Layout corrections for the new metrics: amount row centres on the digit ink
  (`amount_ink_shift()`), status band `BAND_Y` 3 and chip `pad_ver` 1 (Inter's
  18 px line box), small header title `+2`.
- Image 1.69 MB, 13 % of the app partition free. Flashed and booted on the dev
  unit; needs an on-screen visual check.

> ⚠ Commit `main/fonts/`, `main/portal_fonts.h`, `assets/fonts/` and both
> generators together with `main/CMakeLists.txt` / `ui.cpp` / `portal_page.h`
> — see #17.

---

## Phase 1 — Payment safety (fix before production)

### ☑ 1. Tron: parse the node-built transaction instead of substring-matching it — **Critical**
- **Where:** `main/tron_tx.cpp:71-88, 135-156, 183-238`
- **Problem:** `tron_tx_trc20_ok()` / the TRX check only require the expected
  bytes to appear *somewhere* in `raw_data` (byte-aligned). A malicious node (or
  anyone with a cert from any CA in the bundle, see #15) can place those bytes in
  the memo field (`raw_data.data`, field 10) while the real contract is
  `TriggerSmartContract(USDT, transfer(attacker, balance))`. `txID ==
  sha256(raw)` still holds, so the card signs it; the same node reports SUCCESS.
- **Fix:** decode the protobuf field by field. Require exactly one `contract` of
  the expected type, a byte-exact `parameter.value`, no `data` / `auths` /
  `scripts` / `Permission_id`, bounded `expiration` and exact `fee_limit`.
- **Done when:** host tests in `tests/units/test_tron_tx.cpp` reject: memo
  carrying the expected bytes, a second contract entry, an extra field, a wrong
  fee limit; and still accept real TronGrid-built transactions (TRX + TRC-20,
  with and without explicit `call_value`).

### ☑ 2. EVM: no double charge after a lost broadcast reply or confirmation timeout — **High**
- **Where:** `main/main.cpp:1256, 1277-1285, 2606-2611`, `main/eth_rpc.cpp:162, 398-401`
- **Problem:** if `eth_sendRawTransaction` times out after the node accepted the
  tx, or the 120 s receipt poll gives up on a stuck tx, the panel says
  "Declined". The operator retries; the nonce was read with `"pending"` so the
  retry uses N+1 and both transfers settle.
- **Fix:** compute `keccak256(signed_tx)` on the device before broadcasting. On
  transport error / timeout show **"Unconfirmed"** with the hash (never
  "Declined") and keep polling it; block a new sale until it resolves or the
  operator explicitly clears it. Read the nonce with `"latest"` so a retry
  replaces rather than stacks.
- **Done when:** with the RPC reply dropped after acceptance (test proxy), the
  panel shows Unconfirmed + hash, then Approved; a retry cannot produce a second
  on-chain transfer.

### ☑ 3. EVM: don't trust the RPC's tx hash or a bare receipt status — **High**
- **Where:** `main/eth_rpc.cpp:416-431`, `main/main.cpp:2572`
- **Problem:** a lying RPC can skip the broadcast and return the hash of some
  other successful tx; only `status == 0x1` is checked → PAID with no payment.
- **Fix:** poll the locally computed hash (#2). In the receipt, check `to` is the
  token contract and a `Transfer(from, payout, amount)` log is present with the
  exact recipient and amount.
- **Done when:** unit test on `eth_json` receipt parsing rejects a receipt whose
  log has a different recipient or amount.

### ☑ 4. Tron: "not broadcast" verdict can be wrong — **Medium**
- **Where:** `main/main.cpp:1465-1480`
- **Problem:** a single `gettransactioninfobyid` 4 s after a failed broadcast
  reply. It returns `{}` until the tx is in a block, so an accepted-but-unmined
  tx is shown "Not broadcast"; a retry inside ~60 s double-charges.
- **Fix:** poll until `raw_data.expiration` has passed (or use
  `gettransactionbyid` for mempool presence) before declaring it not sent.

### ☑ 5. Config portal: admin-code bypass — **High**
- **Where:** `main/provision.cpp:616-620, 630-641` (`auth_post`, `gate()`)
- **Problem:**
  - Once authorised, `POST /api/auth` returns the live token to *any* client on
    the SoftAP.
  - Before authorisation each request overwrites `s_token`, so the last asker is
    the browser the operator's panel entry authorises.
  - In Wi-Fi-only mode `gate()` lets every endpoint through with no code —
    including `/api/network` (reboots to testnet), `/api/fees`, `/api/clock`. That
    mode opens on its own when the venue network fails (can be forced by jamming).
- **Fix:** bind the token to the requesting browser and show a short pairing code
  for it on the panel; never re-issue the token from `/api/auth` once authorised;
  in Wi-Fi-only mode allow `/api/wifi` only.
- **Done when:** `tools/test_portal_render.js` + a host test cover: second client
  gets 403 after authorisation; Wi-Fi-only client gets 403 on fees/network/clock.

### ☑ 6. Refuse sales when a stored payout address is invalid — **Medium**
- **Where:** `main/main.cpp:2172-2176` (Tron), `main/main.cpp:438-441` (EVM)
- **Problem:** a stored Tron address that passes the page's shape check but fails
  the checksum at boot silently falls back to `TRON_ADDR_TO` from `config.h` (a
  Nile dev address) — even on mainnet. `settings_has_payout()` still returns
  true, so the "refuse rather than fall back" rule at `main.cpp:2458` never fires.
- **Fix:** if a stored address exists but does not parse, refuse sales with a
  clear panel message. Also make the portal run the full checksum before
  accepting a Tron address.

### ☑ 7. Verify token decimals — **Medium**
- **Where:** contract setter (portal → `settings_set_contract`), amount→units in
  `main/main.cpp:885-890, 1045-1056`
- **Problem:** decimals are hard-coded to 6. Setting an 18-decimal contract
  charges 10⁻¹² of the displayed amount.
- **Fix:** call `decimals()` when a proposed contract is accepted on the panel;
  refuse anything but 6 (or store the value and use it).

### ☑ 8. Release config: Secure Boot / signing-key mismatch — **High**
- **Where:** `sdkconfig.defaults.flash_encryption:8, 41-44`,
  `sdkconfig.defaults.release:5, 44`, `docs/ota.md`, `README.md:263`
- **Problem:** the flash-encryption overlay sets `CONFIG_SECURE_BOOT=y` /
  `SECURE_BOOT_V2_ENABLED=y` (while its header says Secure Boot is *not*
  enabled). The release overlay stacks on it and sets
  `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y`, which Kconfig drops because it
  `depends on !SECURE_BOOT` → the release build burns one-way Secure Boot eFuses
  although the docs say "No eFuses are burned". The two overlays also name
  different key paths (`secure_keys/secure_boot_signing_key.pem` vs
  `secure_boot_signing_key.pem`, which does not exist).
- **Fix:** choose one model (the dev unit already has SBv2 burned), make both
  overlays and `docs/ota.md` agree, use one key path.
- **Done when:** the generated `sdkconfig` of a release build is diffed and
  reviewed before any production flash.

---

## Phase 2 — Unattended reliability

### ☐ 9. Wi-Fi reconnects forever — **High**
- **Where:** `main/net.cpp:43, 83-91`
- **Problem:** after `WIFI_MAX_RETRY` (2) immediate retries only `FAIL_BIT` is
  set; nothing tries again. A router reboot mid-shift leaves the terminal offline
  until power-cycled; every sale fails after the tap.
- **Fix:** outside `net_wifi_connect()`, keep reconnecting from the disconnect
  handler / an `esp_timer` with backoff capped at 30–60 s. Show the state in the
  status band.

### ☐ 10. Boot faults retry or reboot instead of stopping — **High**
- **Where:** `main/main.cpp:2073, 2079, 2139, 2149, 2181, 2251, 2395`, loop `2335-2372`
- **Problem:** every boot fault `return`s from `app_main`; the failure screen's
  button posts to a queue nobody reads. After a power cut the router usually
  comes back later than the ESP32 → wizard waits forever, SNTP failure forces the
  panel picker, RPC probe failure stays on screen.
- **Fix:** on a configured unit keep retrying the saved network, SNTP and RPC in
  the background with backoff; for NFC / RPC hardware faults `esp_restart()`
  after a delay.

### ☐ 11. Persist the in-flight payment — **Medium**
- **Where:** `main/main.cpp:2552-2615`
- **Problem:** the tx hash lives only in RAM; a brownout or panic during the
  poll loses whether the customer paid.
- **Fix:** write `{hash, amount, chain, time}` to NVS just before broadcast,
  clear it on the final verdict, resume polling at boot and show the result.
  One write per sale — negligible wear.

### ☐ 12. Task watchdog resets the device — **Medium**
- **Where:** `sdkconfig` `CONFIG_ESP_TASK_WDT_PANIC` (unset)
- **Problem:** the TWDT only logs, and no app task is subscribed. A hung PN532
  I²C transaction, httpd or UI loop freezes the terminal.
- **Fix:** set `CONFIG_ESP_TASK_WDT_PANIC=y` in `sdkconfig.defaults`, subscribe
  the main and UI loops, feed around known long waits (card wait, receipt poll).

### ☑ 13. Tx hash truncated on "Approved" — **Medium, one-line fix**
- **Where:** `main/ui.cpp:601` (`s_tx_info[64]`), `hash_short` at `ui.cpp:4301`
- **Problem:** an EVM hash is 66 chars, a Tron txid 64; the buffer keeps 63, so
  the 4-char suffix shown for explorer comparison is wrong on every sale.
- **Fix:** `static char s_tx_info[72]`.

### ☐ 14. OTA confirmation should not depend on finishing setup — **Medium** *(plausible)*
- **Where:** `main/main.cpp:2412`, `main/settings.cpp:628-637`
- **Problem:** a new build wipes NVS, so first boot runs the wizard and
  `ota_mark_valid()` only happens after full setup + an RPC round-trip. A power
  cut or RPC outage during that boot rolls back a good image (and wipes NVS
  again).
- **Fix:** mark valid once panel, NFC and card stack are up; keep the RPC check
  as a warning.

### ☐ 15. TLS: pinning and heap headroom — **Medium**
- **Where:** `sdkconfig.defaults:20-22`, `main/config.h` (`*_CA_CERT_PEM`
  undefined), `main/https_post.cpp:145, 195-218`, `sdkconfig:1801-1803`
- **Problem:**
  - The comment claims `RPC_CA_CERT_PEM` pins GTS WE1; nothing defines it, so
    every RPC trusts the full ~150-CA bundle (widens #1 and #3).
  - Every call does a fresh handshake with a fixed 16 KB input buffer next to
    the 48 KB LVGL pool and no PSRAM *(plausible fragmentation over long uptime)*.
  - A response that fills the buffer is reported as success.
- **Fix:** pin every endpoint in release builds (or correct the comment/docs);
  enable `CONFIG_MBEDTLS_DYNAMIC_BUFFER` or reuse one keep-alive client; log
  `largest_free_block` periodically; return failure on a full buffer.

### ☐ 16. Smaller hardening items — **Low / Medium**
- **Admin code brute force** (`main/ui.cpp:3927-3934`): back-off caps at 60 s
  with a 4-digit minimum → ~720 guesses a night. Require 6 digits and keep
  escalating.
- **Fee cap on the confirm screen:** show the max network fee (gas limit × max
  fee) the customer can be charged.
- **Per-device flash-encryption keys** *(plausible)*: `tools/secure_provision.py:22,102`
  burns a host-generated key from one file; if reused, one leak decrypts every
  unit. Generate on-chip per device.
- **Fees read once per sale** (`main/main.cpp:1115`): a fee change from the page
  between the balance check and signing makes them disagree.
- **Cross-task buffers** (`ui.cpp:854, 4836`, `provision.cpp:924`): main / httpd
  write buffers the UI task is rendering. Copy under a mutex or pass via a queue
  (see #25).
- **Card pulled during `verifyPin`** (`main/main.cpp:1085`) is reported as
  "Wrong PIN" — distinguish transport errors.
- **`prov_stop()` on the UI task** (`ui.cpp:4870`) freezes LVGL up to ~2 s —
  move it to the main task.
- **Tron broadcast stack** *(plausible)*: ~4 KB of locals in
  `tron_rpc.cpp:427-442` on top of TLS in a 16 KB stack — log
  `uxTaskGetStackHighWaterMark` after a Tron sale.
- `eth_addr.cpp`: reject the zero address.
- Recovery-bit check (`main/main.cpp:1269-1275`) can be done locally instead of
  via RPC `ecrecover`.

---

## Phase 3 — Tests, tooling, docs

### ☐ 17. Commit the font work as one change — **High**
- `main/CMakeLists.txt` lists `fonts/*.c` and `portal_page.h` includes
  `portal_fonts.h`, all currently untracked. Commit together:
  `main/fonts/`, `main/portal_fonts.h`, `assets/fonts/`, `tools/gen_fonts.py`,
  `tools/gen_portal_fonts.py`, plus the modified `ui.cpp`, `portal_page.h`,
  `CMakeLists.txt`, `sdkconfig.defaults`.

### ☐ 18. CI runs the tests — **High**
- **Where:** `.github/workflows/codeql.yml:66-80` (compiles tests to objects
  only), `misra_check.yml`, `doxygen.yml`.
- **Fix:** add a job running `bash scripts/checks.sh` on Linux with
  `-fsanitize=address,undefined`; also build + run `fuzz/test_civil_time.cpp`
  and each libFuzzer harness for ~60 s with clang; add a firmware build job
  (`idf.py build` in the `espressif/idf:v5.5.4` container).

### ☐ 19. Known-answer tests for the money code — **High**
- **Where:** `static` in `main/main.cpp` / `ui.cpp`: `build_usdc_calldata`,
  `evm_fees_wei` (`811-830`), unit→wei conversion and overflow guards
  (`885-890`, `1045-1056`), `amount_cents_max` (`ui.cpp:887`); `eth_rlp.cpp` and
  `eth_json.cpp` are only fuzzed (no crash ≠ correct bytes).
- **Fix:** move them into pure headers (the `form_parse.h` pattern) and test
  against vectors from a known-good signer: an EIP-1559 tx, USDC `transfer`
  calldata, boundary amounts around `POS_AMOUNT_UNITS_MAX_NATIVE`. Same for
  settings validation (admin code, timezone range, fee setters, `dual_set`).

### ☐ 21. Update stale docs — **Medium**
- `docs/ota.md:76-104` still documents `OTA_MANIFEST_URL` (removed; lines 30-35
  of the same doc say so).
- `tests/units/README.md` lists 10 tests; `checks.sh` runs 14 (add
  `test_networks`, `test_assets`, `test_touch_cal`, `test_eth_hex`).
- `README.md:132-134`: PN532 I²C switch is **`1 0`**, power-cycle to apply.
- `README.md:306`: outdated `ecrecover` troubleshooting entry.
- `README.md:308`: Montserrat advice — replace with the font section above.
- `docs/security-analysis.md`: its "pinning" and "node untrusted" claims for
  Tron are contradicted by #1 and #15 — update after fixing.

### ☐ 22. Repository clean-up — **Low**
- Commit: `docs/security-analysis.md`, `docs/payment-path-change.md`, this file.
- Delete / keep out of git: `Screenshot 2026-09-19 172939.png`,
  `photo_2026-09-11_16-00-24.jpg`, `gettyimages-2275971367-2048x2048.jpg`
  (stock image — licensing risk, never commit). Add `Screenshot*.png` and
  `photo_*.jpg` to `.gitignore`.
- `assets/carte.png` (replaced per `ui.cpp:1852`) and
  `assets/contactless-icon.png` (the generator reads the `.svg`) are unused.
- `mockups/`: commit `mockup.py`, ignore its PNG output.
- Fuzz corpora have 1–4 seeds each: add captured RPC responses and a production
  RLP blob; consider a g++ corpus-replay mode so they run on Windows without
  clang.

---

## Phase 4 — Structure (after the payment code is stable)

### ☐ 23. One table per asset — **High, M**
- Facts about an asset are spread over `settings.h:36-95` (enum + predicates),
  `config.h:82-192` (contracts), `main.cpp:339, 367, 382, 2060-2200` (slots and
  boot loads), `ui.cpp:1745-1756` (icons). `coin_icon()` returns the USDC logo
  for any ticker that is not "USDT" — a new token would silently show USDC.
- Enum names no longer match meaning (`POS_CHAIN_ETH_SEPOLIA` = USDC on
  Ethereum, `TRON_NILE` = TRX); `settings_get_contract(bool tron)` blocks a
  third family.
- **Fix:** add family, test/main contract, decimals and icon id to each
  `POS_ASSETS` row; derive predicates from it; one slot array indexed by row;
  rename enumerators keeping values stable for NVS.

### ☐ 24. Split `main.cpp` (2 796 lines) — **High, L**
- `app_main` is ~780 lines; `sign_and_broadcast` (1011-1290) and
  `sign_and_broadcast_tron` (1294-1487) repeat reconcile → sign → cancel check →
  broadcast; 16 `chain_is_*` branches.
- **Fix:** a small chain-ops struct `{balance_ok, sign_and_broadcast,
  get_receipt, derive_path}` per family; files `pay_evm.cpp`, `pay_tron.cpp`,
  `card_io.cpp`, `boot.cpp`; one shared payment skeleton.

### ☐ 25. Split `ui.cpp` (5 085 lines) and synchronise the hand-off — **Medium, M-L**
- Existing section seams: theme (89-375), HAL (375-580), state (582-765),
  actions (767-1580), widgets (1582-2045), settings/modals (2200-3045), screens
  (3046-4705), task/API (4707+). ~70 file-scope `s_*` globals; `btn_event_cb` is
  a 216-line switch.
- `ui_show_*` writes payload buffers then sets a `volatile` flag; one
  `s_req_screen` slot, no ordering guarantee on a dual-core chip.
- **Fix:** `ui_theme.cpp`, `ui_hal.cpp`, `ui_widgets.cpp`, `ui_admin.cpp`,
  `ui_sale.cpp`, `ui_setup.cpp` + private `ui_internal.h`; group state per
  screen; FreeRTOS queue of `{screen, payload}` (or one mutex) between tasks.

### ☐ 26. Trim comments while splitting — **Medium**
- Comment/code ratio: `ui.cpp` 0.60, `main.cpp` 0.66, `settings.h` 4.2. Drop the
  history narration ("used to", "no longer" — 36 in `ui.cpp`), cap rationale at
  ~3 lines, move long explanations to `docs/`. Keep the security *why*
  comments (reconcile points, Tron v=0/1 retry).

### ☐ 27. Flash savings and dead code — **Low, S**
- Disable TFT_eSPI's unused fonts in `sdkconfig.defaults:82-87`
  (`CONFIG_TFT_LOAD_GLCD/FONT2/FONT4/FONT6/GFXFF/SMOOTH_FONT`): ~20 KB. Check
  TFT_eSPI still compiles with all fonts off.
- Remove unused `COL_TRON`, `COL_USDT`, `COL_ETH`; stale
  `APP_VERSION_TAG "ui-r27 …"` (`main.cpp:1502`); duplicate `chain_is_tron`
  (`main.cpp:237`, `ui.cpp:823`).
- Name the remaining layout literals (`ui.cpp:2184, 2250, 2307, 2586-2621, …`).
- Serve the portal page and fonts via `EMBED_FILES` as real files instead of C
  string literals (~25 % smaller fonts, lintable HTML/JS).

---

## Suggested order

1. #13 (one line) and #17 (commit fonts).
2. #1, #2, #3 — the ways to lose money.
3. #5, #6, #7, #8.
4. #9, #10, #11, #12.
5. #18, #19 so the Phase 1 fixes stay fixed.
6. The rest of Phase 2/3, then Phase 4.
