# Firmware updates over Wi-Fi

The terminal never connects to GitHub. A browser does, and carries the bytes:

```
 browser ──HTTPS──> raw.githubusercontent.com/…/firmware.json   version, url, notes
 browser ──HTTPS──> …/cryptnox_pos.bin                          the image itself
 browser ──HTTP───> http://<terminal>/api/ota                   streamed to flash
 panel:   operator accepts the version → reboot into the other slot
```

Why the browser and not `esp_https_ota`, which would be a tenth of the code: a
terminal that phones a third party for updates tells that third party how many
units exist, roughly where they are and what each one runs. Nobody needs a
register of deployed payment terminals, least of all one kept by somebody else.

Implementation is `main/ota.cpp`; the reasoning lives in `main/ota.h`.

## Why the flow has this shape

The update page is served *by the terminal* over plain HTTP. That is forced, not
chosen: a page served over HTTPS cannot POST to an `http://` address — mixed
content, blocked in every browser — so hosting the page on a website and pushing
from there does not work. The other direction is allowed, which is the one
needed: an HTTP page may `fetch()` an HTTPS URL.

So the browser needs the internet **and** the terminal at the same time:

* **On the venue network** (the normal case). The terminal is already joined to
  it; put the laptop or phone on the same network and both are reachable. The
  terminal makes no outbound connection at any point.
* **No internet on that network.** The page's file picker takes a `.bin`
  downloaded anywhere else. Everything except the "Check for updates" button
  works with no internet at all.

A phone joined to the terminal's *setup* AP is not a usable combination — that
AP has no route to the internet, and which interface a phone uses for a given
request while a captive network is joined is not something to build on. Use the
file picker.

## Operating it

1. Settings → About → **Update**. The panel shows the address to open, e.g.
   `http://192.168.1.34/`, and starts a **15-minute** window
   (`OTA_WINDOW_MIN`). Tapping **Done** closes it immediately.
2. Open that address. Enter the terminal's admin code, then either **Check for
   updates** (needs internet in *that browser*) or pick a `.bin` file.
3. The terminal verifies the image and asks on its own screen. Nothing reboots
   until somebody accepts it there — the same rule payout addresses follow: a
   browser may propose, only the panel may accept. A version that goes
   *backwards* is called out in red.
4. First boot after an update is on probation. `ota_mark_valid()` runs only once
   the panel, the card reader, the wallet layer and one authenticated RPC
   round-trip have all come up; a reset before that point returns the bootloader
   to the previous slot.

## Publishing a release

`OTA_MANIFEST_URL` in `main/ota.cpp` points at a JSON file. Host it — and the
`.bin` — somewhere that sends `Access-Control-Allow-Origin: *`, or the browser
will refuse to hand the response to a page served from `http://<terminal>/`:

| Host | CORS | Notes |
|---|---|---|
| `raw.githubusercontent.com` | `*` | verified; simplest option |
| GitHub Pages (`*.github.io`) | `*` | verified |
| GitHub **release assets** | redirects to `objects.githubusercontent.com` | confirm from a real browser before relying on it |

```json
{
  "version": "1.1.0",
  "url": "https://raw.githubusercontent.com/Cryptnox/cryptnox-pos-releases/main/1.1.0/cryptnox_pos.bin",
  "size": 1712345,
  "notes": "USDT (TRC-20) fee cap raised to 30 TRX.\nFixes a stuck 'Confirming' screen after a dropped uplink."
}
```

`version` is compared against the running firmware's, which comes from
`version.txt` at the project root — bump it in the same commit as the release.
Ordering is dotted-numeric with an optional leading `v`; anything after the
numbers (`-rc1`, `+sha`) is ignored rather than ordered
(`main/ota_version.h`, `tests/units/test_ota_version.cpp`). `size` and `notes`
are shown to the operator; the device ignores both and trusts neither.

## Signing — do not ship without this

The terminal cannot validate a TLS certificate chain to GitHub, because it never
talks to GitHub. Its only way to know an image is genuine is a signature over
that image, checked against a public key compiled into the firmware already
running. `sdkconfig.defaults.release` turns that on
(`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`), and `esp_ota_end()` refuses
anything that fails it — before the staged slot can ever become bootable.

Without it, `POST /api/ota` is one admin code away from replacing the firmware on
a device that signs cryptocurrency transactions. The admin code and the
15-minute window are not a substitute: both cross a venue LAN in clear text.

No eFuses are burned and nothing is irreversible — this is Secure Boot V2's
signature scheme used for *update verification only*, not hardware secure boot.

Generate the key once:

```sh
idf.py secure-generate-signing-key --version 2 secure_boot_signing_key.pem
```

Keep it out of this repository and off any build machine you do not control; it
is the whole of the trust anchor. Losing it means no unit already in the field
can ever be updated again. The RSA scheme needs ESP32 revision v3.0 (ECO3) —
if `esptool.py chip_id` reports less, switch to
`CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME` and drop `CONFIG_ESP32_REV_MIN_3`.

## Flash budget

`partitions.csv` carries two 1.94 MB app slots (`ota_0`, `ota_1`) — the largest
64 KB-aligned pair that fits in 4 MB alongside the data partitions. The image is
~1.6 MB, so there is roughly 390 KB of headroom; check `idf.py size` before
adding fonts or images.

**A unit flashed with the old single-slot table cannot be updated over the air.**
The partition table is never rewritten by an update, so those units need one
serial reflash before any of this applies to them.

## Endpoints

| | |
|---|---|
| `GET /` | the update page (HTML + JS, one string in `ota.cpp`) |
| `GET /api/info` | `{"version","slot","staged","window_min"}` |
| `POST /api/ota` | image body, `X-Admin-Code` header, streamed to the idle slot |

Same-origin, so no CORS headers are needed on the device side and no preflight
happens. The page cannot hash the download itself — `crypto.subtle` is
unavailable on a non-secure origin like `http://192.168.1.34` — which is fine:
the image carries its own SHA-256 and `esp_ota_end()` is what checks it.
