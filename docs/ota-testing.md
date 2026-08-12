# Testing firmware updates over Wi-Fi

Test plan for the browser-mediated OTA path added in `main/ota.cpp`. What the
feature is and why it has this shape: [`docs/ota.md`](ota.md).

Four layers, cheapest first. Do them in order — a failure at layer 1 makes the
rest meaningless, and §4 is the one that decides whether this feature is safe to
put in the field.

1. **Host** — version ordering, no hardware.
2. **Bench** — `curl` against the endpoints. Proves every rejection path without
   ever installing anything.
3. **Real update** — install a second build, from a browser, end to end.
4. **Rollback** — install a *deliberately broken* build and watch the terminal
   come back on the old one.

---

## 0. Prerequisites

### The first flash is not optional

`partitions.csv` changed. **The partition table is never rewritten by an update**,
so a unit carrying the old single-app table has to be flashed over serial once
before any of this works. Find the port, then flash:

```bash
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"
cmd //c "C:\Cryptnox\cryptnox-pos\scripts\idf-build-flash.bat COM7"
"C:/Users/Yann/.espressif/python_env/idf5.5_py3.14_env/Scripts/python.exe" \
    scripts/serial_tail.py COM7 20
```

`nvs` and `phy_init` keep their old offsets (0x9000, 0xF000), so **stored
settings survive** the table change on a plain build — admin code, Wi-Fi, payout
addresses all come back. Two exceptions:

| Situation | Consequence |
|---|---|
| Flash-encryption build | `nvs_keys` moved (0x310000 → 0x12000). The encrypted NVS is unreadable; re-provision from scratch. |
| Anything odd after the change | `idf.py -p COM7 erase-flash` and start clean, rather than debugging a half-migrated NVS. |

### Confirm the table and the config actually took

```bash
grep -n "BOOTLOADER_APP_ROLLBACK" sdkconfig
grep -n "define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE" build/config/sdkconfig.h
```

**Pass:** `=y` and `1`. `sdkconfig` is gitignored and **wins over
`sdkconfig.defaults`** — a stale `# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not
set` line leaves §4 silently doing nothing while everything else looks fine.
Delete `sdkconfig` and rebuild, or flip the line by hand.

```bash
grep -n "binary size" <build log>       # expect: Smallest app partition is 0x1f0000
```

**Pass:** the app fits with headroom to spare (~19% at the time of writing). Under
5% free, stop and shed LVGL fonts before continuing — a slot that cannot hold the
*next* build makes OTA useless.

### Reading the log

Every check below quotes lines from the `ota` tag. They only exist on a dev
build: `sdkconfig.defaults.release` sets `CONFIG_LOG_DEFAULT_LEVEL_NONE`, so on a
release image the panel and the browser are the only instrumentation.

---

## 1. Host tests

```bash
for t in test_eth_addr test_hardening test_tron_tx test_prov_form test_ota_version; do
  g++ -std=c++14 -Wall -Imain -Icryptnox-sdk-esp32/cryptnox-sdk-cpp \
      tests/units/$t.cpp -o $t && ./$t || exit 1
done
```

`test_ota_version` covers `main/ota_version.h`: numeric (not lexicographic)
ordering, optional leading `v`, absent components as zero, ignored `-rc1`/`+sha`
suffixes, clamped overflow, and the 32-byte boundary of
`esp_app_desc_t::version` pinned from both sides. All five must print `OK`.

### 1.1 The browser must agree with the panel

The page carries its own copy of the comparison (`cmp()` in the `<script>`), and
it decides which button the operator is offered. Open the update page (§2), then
in the browser console:

```js
[['1.2.0','1.10.0',-1],['9.0.0','10.0.0',-1],['v1.2.3','1.2.3',0],
 ['1.2','1.2.0',0],['1.2.3-rc1','1.2.3',0],['','1.0.0',-1]]
  .filter(([a,b,w]) => Math.sign(cmp(a,b)) !== w)
```

**Pass:** `[]`. Anything listed is a case where the browser would label a
downgrade as an update — the panel would still catch it in red, but the two
disagreeing means one of them is wrong.

---

## 2. Bench tests — `curl` against the endpoints

Finish setup so the terminal is joined to a network with an admin code set. Then
**Settings → About → Update**. The panel shows the address; the log agrees:

```
W (nnnn) ota: update server up at http://192.168.1.34/ for 15 min
```

Export it and the admin code:

```bash
T=http://192.168.1.34
C=1234
```

**Fail before you start:** if the panel says the terminal has to be on a Wi-Fi
network while it plainly is, the DHCP lease has not landed yet
(`not on a network - nothing could reach the update page`). Wait and retry.

### 2.1 The page and the info endpoint

```bash
curl -s $T/api/info
curl -s -o /dev/null -w '%{http_code} %{size_download}\n' $T/
```

**Pass:** `{"version":"1.0.0","slot":"ota_0","staged":false,"window_min":15}`
and a `200` of roughly 4.3 kB. `version` must match the About tab and
`version.txt`; `slot` names the partition currently *running*.

### 2.2 Authentication

```bash
head -c 400000 /dev/urandom > /tmp/junk.bin
curl -si -X POST --data-binary @/tmp/junk.bin $T/api/ota | head -1                        # no header
curl -si -X POST -H "X-Admin-Code: 9999" --data-binary @/tmp/junk.bin $T/api/ota | head -1 # wrong
```

**Pass:** both `401`, body `Wrong admin code.`, log
`upload rejected: bad or missing admin code` — and **no** `receiving ... bytes`
line. The flash must not be touched before the code is checked.

### 2.3 Size rejection

```bash
head -c 1000 /dev/urandom > /tmp/tiny.bin
head -c 2200000 /dev/urandom > /tmp/huge.bin
curl -s -X POST -H "X-Admin-Code: $C" --data-binary @/tmp/tiny.bin $T/api/ota
curl -s -X POST -H "X-Admin-Code: $C" --data-binary @/tmp/huge.bin $T/api/ota
```

**Pass:** `too small to be firmware` and `larger than the firmware slot`. Again no
`receiving` line — neither reaches `esp_ota_begin()`.

### 2.4 A plausible-sized file that is not firmware

This is the check that proves `esp_ota_end()` is doing its job:

```bash
curl -s -X POST -H "X-Admin-Code: $C" --data-binary @/tmp/junk.bin $T/api/ota
curl -s $T/api/info
```

**Pass:** the upload runs (`receiving 400000 bytes into 'ota_1'`), then
`image rejected: ESP_ERR_OTA_VALIDATE_FAILED`, and the response says the image is
not valid firmware or is not signed with a trusted key. `staged` stays `false`,
and **the panel shows nothing** — a rejected image never reaches the operator.

Then take a payment. The terminal must be completely unaffected by having had
400 kB of noise written into its spare slot.

### 2.5 A truncated upload

```bash
# throttled, then cut off part-way: ~600 kB of a 1.6 MB image arrives
curl -s -X POST -H "X-Admin-Code: $C" --data-binary @build/cryptnox_pos.bin \
     --limit-rate 200k --max-time 3 $T/api/ota
curl -s $T/api/info
```

**Pass:** log shows `upload aborted at <n>/<total> bytes`, `staged` is `false`,
nothing on the panel. Then repeat §2.4 or §3 successfully — a dropped upload must
not wedge the slot for the next one.

### 2.6 One at a time

Upload a real image (§3) so something is staged, and *before* touching the panel:

```bash
curl -s -X POST -H "X-Admin-Code: $C" --data-binary @build/cryptnox_pos.bin $T/api/ota
```

**Pass:** `409`, "An update is already waiting to be accepted on the terminal
screen." Two images cannot occupy one slot.

### 2.7 The window closes

Open the window, note the time, and leave the terminal alone for 15 minutes
(`OTA_WINDOW_MIN`).

```bash
curl -s -o /dev/null -w '%{http_code}\n' -X POST -H "X-Admin-Code: $C" \
     --data-binary @/tmp/junk.bin $T/api/ota
curl -s -o /dev/null -w '%{http_code}\n' $T/
```

**Pass:** `503` from the upload endpoint, log `update window expired` then
`update server down`, and **the card on the panel disappears by itself**. The
terminal must then accept a payment normally.

That last part matters more than it looks: these modals swallow every touch, so a
card left behind after its window closed is a terminal that cannot take money
until somebody power-cycles it.

For the impatient version, tap **Done** instead and check the same:
`update server down`, card gone, `curl $T/` refused.

---

## 3. The real update, from a browser

Make a second build to install:

```bash
echo 1.0.1 > version.txt
cmd //c "C:\Cryptnox\cryptnox-pos\scripts\idf-build.bat"
cp build/cryptnox_pos.bin /tmp/1.0.1.bin
echo 1.0.0 > version.txt        # keep 1.0.0 around for the downgrade check
```

### 3.1 File picker — the path that needs no internet

1. Open Update on the panel, open the address in a browser on the same network.
2. Enter the admin code, pick `/tmp/1.0.1.bin`, **Install this file**.
3. Watch the percentage climb. Expect `receiving 1653... bytes into 'ota_1'`.
4. Browser says *"Version 1.0.1 received and verified. Accept it on the terminal
   screen…"*; log says `staged 1.0.1 in 'ota_1' - awaiting on-screen accept`.
5. **The panel raises a card reading "New firmware / 1.0.1"** with Install and
   Discard. Nothing has rebooted.

**Fail:** any path where the terminal reboots into new firmware without a tap on
the panel. A browser may propose; only the panel may install.

### 3.2 Discard

Tap **Discard** first, on purpose.

**Pass:** the card closes, the window closes with it, and `/api/info` still
reports `1.0.0` in `ota_0`. Reboot the terminal — still `1.0.0`. A refused image
must not install itself later.

### 3.3 Install

Upload again, tap **Install**.

```
W (nnnn) ota: installing 1.0.1 from 'ota_1' - rebooting
```

**Pass, after the reboot:**

- About tab reads `1.0.1`.
- `curl -s $T/api/info` → `"version":"1.0.1","slot":"ota_1"` — the slot has
  flipped.
- The log carries `ota: update to 1.0.1 confirmed - rollback cancelled`, and it
  appears **after** `cryptnox_pos: Ready`, not before.
- Take a payment. The card, the panel, the RPC and the payout address all behave
  exactly as before.

**Fail:** `could not confirm this image - it WILL roll back` means the next reset
throws the update away. The terminal works right now and will be a different
version tomorrow — treat it as a blocker.

Install a third build (bump to `1.0.2`) and check the slot alternates back to
`ota_0`. A terminal that only ever writes one slot is not doing OTA.

### 3.4 The downgrade warning

Upload the original `1.0.0` image onto the `1.0.1` terminal.

**Pass:** the card reads **"Go back to firmware / 1.0.0"** and the body text
begins *"This is OLDER than the firmware installed now."* in red. The image is
signed exactly as well as the new one, so this warning is the only thing standing
between an operator and being talked into a build with a known fault.

Install it and confirm the terminal comes back on `1.0.0`, then go forward again.

### 3.5 Check for updates — the manifest path

Publish a `firmware.json` (shape in [`docs/ota.md`](ota.md)) at
`OTA_MANIFEST_URL`, pointing at `1.0.1.bin`, and host both somewhere that sends
`Access-Control-Allow-Origin: *`.

```bash
curl -sI -H "Origin: http://192.168.1.34" <manifest url> | grep -i access-control
```

**Pass:** `Access-Control-Allow-Origin: *` *before* you try the browser. Then tap
**Check for updates**:

| Manifest `version` | Expected |
|---|---|
| `1.0.1`, terminal on `1.0.0` | "Version 1.0.1 is available." + release notes + an **Install 1.0.1** button |
| same as running | "The terminal is up to date." and no button |
| older than running | "The published version … is OLDER than the one running." + a **Go back to** button |

Then the failure modes, both of which must produce the same friendly message and
never a blank screen:

1. Turn off the browser's internet (leave it on the terminal's network) → *"Could
   not reach the release list. This browser needs internet access…"*
2. Move the manifest to a host with no CORS header → the fetch is refused by the
   browser, same message. This is why §3.5 starts with `curl -I`.

**Also worth doing once:** a phone joined to the terminal's *setup* AP. Expect
"Check" to fail and the file picker to work. That combination is documented as
unsupported, not broken.

---

## 4. Rollback — the test that decides whether this ships

Everything above assumes new firmware boots. This checks what happens when it
does not. Do it on a bench unit, not a deployed one.

Break a build on purpose — after bring-up, before the image is confirmed:

```c
/* main.cpp, immediately before ota_mark_valid() — REVERT AFTER TESTING */
abort();
```

```bash
echo 9.9.9 > version.txt        # unmistakable in the log
cmd //c "C:\Cryptnox\cryptnox-pos\scripts\idf-build.bat"
cp build/cryptnox_pos.bin /tmp/broken.bin
git checkout main/main.cpp && echo 1.0.1 > version.txt
```

Upload `/tmp/broken.bin`, accept it on the panel, and watch the serial log.

**Pass:**

1. It boots into `9.9.9`, reaches the panel, then panics on the `abort()`.
2. The reset that follows does **not** come back on `9.9.9` — the bootloader
   reports falling back to the other app and the terminal boots the previous
   version.
3. About shows the old version; `/api/info` shows the old `slot`.
4. It takes a payment.

**Fail:** a terminal that boot-loops on `9.9.9` means rollback is not compiled in
(§0) or `ota_mark_valid()` is being called too early. Both make a bad build
unrecoverable without opening the case — which is the entire failure this
mechanism exists to prevent.

Worth doing the harsher variant once: move the `abort()` to *before* `ui_init()`,
so the image dies with a blank screen. The outcome must be identical. A terminal
that shows nothing and recovers by itself is the whole point.

---

## 5. Signed builds

Everything above runs on an unsigned dev build, where any correctly-formatted
image is accepted. The release path must be tested separately, on a sacrificial
unit, because it is the only thing that makes `POST /api/ota` safe to expose:

```bash
idf.py secure-generate-signing-key --version 2 secure_boot_signing_key.pem
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.flash_encryption;sdkconfig.defaults.release" build
```

| Check | Expected |
|---|---|
| Upload an image signed with the same key | installs normally |
| Upload the *unsigned* dev build to the signed unit | `400`, `image rejected: ESP_ERR_OTA_VALIDATE_FAILED` |
| Upload an image signed with a **different** key | same rejection |
| Flip one byte in the middle of a signed image (`dd`) | same rejection |

The third row is the one that matters. Generate a throwaway second key, sign a
build with it, and confirm the terminal refuses it — that proves the check is
verifying *the* key and not merely the presence of a signature.

Note the release overlay also sets `CONFIG_ESP32_REV_MIN_3`: on pre-ECO3 silicon
the image will not boot at all. Check `esptool.py chip_id` first.

---

## 6. Regressions on the money path

The update window is reachable from the settings menu, which is new — the setup
portal never was. These must pass on a terminal that is **not** being updated:

| Check | Expected |
|---|---|
| Boot an untouched terminal | `Ready`, no `ota` lines beyond the version, no new warnings |
| Open Update, tap Done, take a payment | unaffected |
| Open Update, upload nothing, wait out the window, take a payment | unaffected |
| **Upload while a payment is in progress** | see below |
| Factory reset after an update | settings gone, firmware version unchanged — a reset is not a downgrade |
| About tab layout | version, the Update row, then the licence text, all reachable by scrolling |

### 6.1 Upload during a payment

Start a payment and leave it on **Place card**, then upload a valid image.

**Pass:** the upload completes and is verified, but the panel offer **does not
appear over the transaction**. It appears once the payment finishes or is
cancelled. `UI_EVENT_OTA_STAGED` goes through the main task's queue, which a
payment occupies, so the offer waits its turn.

**Fail:** an install card appearing over a customer's transaction, or an
`esp_restart()` between signing and broadcast. Repeat this one with the card
actually tapped, mid-signing, before shipping.

---

## 7. What this plan does not cover

- **Flash encryption + OTA together.** `esp_ota_write()` encrypts transparently,
  so it should work, but nothing here proves it. Run §3 and §4 on an
  encryption-enabled unit before shipping one.
- **Two browsers uploading at once.** `s_receiving` refuses the second with
  `409`; the interleaving of two sockets into one `httpd` task is untested.
- **Heap headroom during an upload.** LVGL, `httpd` and the 4 kB OTA buffer
  coexist on an ESP32 with no measurement of free heap at the worst moment.
- **A hostile image that is validly signed.** Out of scope by construction: the
  signature is the trust boundary, so anything past it is trusted.
- **Resumed uploads.** There are none — a dropped connection means starting over.

<!-- ponytail: §1.1 and the page-extraction cross-check are manual. Wire them
     into CI when CI exists to wire them into. -->
