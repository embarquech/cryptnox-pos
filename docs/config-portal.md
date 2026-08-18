# The config portal

One web app configures a terminal, in two modes. Implementation is
`main/provision.cpp`; the reasoning lives in `main/provision.h`.

| | **Wizard** (`PROV_MODE_WIZARD`) | **Admin** (`PROV_MODE_ADMIN`) |
|---|---|---|
| When | blank terminal, or saved Wi-Fi unreachable | any time, from the settings menu |
| Network | the terminal's own WPA2 SoftAP | the venue network it is already on |
| Reached by | captive portal — the browser opens itself | QR code / URL on the panel |
| Transport | plain HTTP, port 80 | HTTPS, port 443, self-signed |
| Closes | when setup ends | after `PROV_WINDOW_MIN` (15 min), or **Done** |
| Serves | one step at a time | everything at once |

## Authorisation: the panel, never the wire

There is no admin-code field in the page. The browser asks to be let in; the
terminal shows its admin-code screen; the operator types the code **on the panel**;
the session becomes authorised and carries a random 128-bit token
(`X-Prov-Token`) from then on.

So the code never crosses the network in either direction, on either mode's
transport, and a browser that can reach the page without the terminal in reach can
do nothing at all. (One exception, in the wizard only: the Wi-Fi-only re-join below
asks for no code at all.) Guessing at the panel earns the same escalating lockout the
settings menu imposes — it is the same screen and the same counter, so this is not
a cheaper door.

Everything that decides where money goes goes further than that. A browser may
**propose** a payout address or a token contract; the value is then displayed on
the panel and somebody has to accept it there. Nothing is stored until they do,
and the same is true of firmware: an upload is verified and staged, and only an
on-screen accept makes it bootable.

## Why the wizard is HTTP and the admin page is HTTPS

Not an oversight, and not laziness in the one that is plaintext.

**Wizard mode has to be HTTP.** A captive-portal probe fetches a bare `http://`
URL on port 80 and will not follow a redirect to 443. TLS there means the phone's
browser never opens by itself, which is the entire feature. And the link is already
encrypted: a WPA2 SoftAP whose random per-device passphrase is on the panel in
front of the operator, existing only during setup. The admin code is not on that
link, and the values that are get confirmed on the panel.

**Admin mode is HTTPS**, because a venue LAN is a different proposition — every
device holding the same PSK is on it. The certificate is self-signed and generated
**on the terminal**, once, into NVS (`tls_generate()`), so:

* the browser warns the first time, and the panel says it will;
* no two terminals share a private key, which a certificate baked into the image
  could not manage — that key is in the published firmware.

P-256 rather than RSA: RSA-2048 keygen on this chip is tens of seconds of frozen
panel, an EC key is well under one. Validity is a fixed decade rather than a window
around "now", because the key is generated the first time the admin page is opened
and that can be before any SNTP sync — a window derived from a wrong clock lands in
the past and the browser rejects it for a reason nobody could diagnose.

A factory reset erases the TLS identity along with the AP passphrase. A new
operator must not inherit the last one's.

## The wizard flow

```
 1. admin code       panel     the one secret that never touches a network
 2. QR code          panel     camera joins the SoftAP; portal opens the page
 3. authorise        both      browser asks, panel takes the code
 4. addresses        browser   typed, or read off a Cryptnox card
 5. Wi-Fi            browser   scanned list from the device's own radio
 6. Finish           panel     restarts, which is what applies everything
```

Steps 1 and 6 are on the panel, and the four in between are in the browser. There
is deliberately **no "use this screen instead"** escape: typing a payout address or
a venue passphrase on a resistive 240x320 panel is the thing this module exists to
avoid, and a second, worse path meant maintaining two of everything. The panel
picker still exists (`wifi_picker()` in `main/main.cpp`) but it is now the
settings-menu route and the fallback for when the SoftAP itself will not come up.

**A configured terminal whose network has gone** gets `2 → 5 → 6`, and no admin code:
the addresses are already set and there is no code to create, so the page opens
straight on the Wi-Fi card and the first browser to ask is let in
(`prov_set_wifi_only()`). Three screens guarding a form whose only unconfirmed power
is "try this network" is three screens between an operator and a working till.

What still holds in that flow: the AP passphrase — per device, on the panel in front
of whoever is asking — is the perimeter, and every value that decides where money
goes, or which firmware boots, is still accepted on the panel. The panel drops the
step numbers there, since there are no steps 1, 3 or 4 to count.

**Finishing restarts the terminal.** The recipient and contract dual stores are
built at boot from validated strings, so applying a change in place would mean a
second path into the money code that has to re-validate everything the boot path
already validates. Three seconds during setup buys reusing those checks verbatim.

### Payout addresses from a Cryptnox card

The alternative to typing 42 characters. One tap reads both networks —
`m/44'/60'/0'/0/0` for Ethereum, `m/44'/195'/0'/0/0` for Tron — and each address
goes through the same accept-on-the-panel handshake a typed one does, because "the
card said so" is not the same claim as "the operator checked it". A customer's card
presented at the wrong moment would otherwise redirect the takings.

The card will not export a public key without a verified PIN, so this needs the
card PIN on the keypad first, exactly as signing does.

The Ethereum address is rendered in EIP-55 mixed case (`eth_addr_format`). That is
not cosmetic: it is the form the operator's own wallet shows, which is what they are
being asked to compare against, and it means the stored string carries a checksum
so the boot-time parse is a real check on it.

## An asset with no payout address is not offered

A terminal set up for Ethereum and interrupted before Tron used to keep offering
Tron payments — to the `config.h` recipient, which is somebody else's address, while
looking entirely normal. Three things changed:

* the network picker greys out a network with no stored payout address, saying so
  rather than hiding the row;
* the Tx tab says in red when the recipient shown is the built-in default;
* boot corrects a stored chain selection whose payout address was never set,
  switching to one that was (`main/main.cpp`, after the address resolution).

`settings_has_payout()` is the question being asked, and it means *stored*, not
*resolvable* — the `config.h` fallback still supplies the value, it just no longer
counts as configured.

## Endpoints

Everything except `GET /` and `GET /api/state` requires the session token.

| | |
|---|---|
| `GET /` | the page — HTML then JS, two strings in `provision.cpp` |
| `GET /api/state` | mode, step, `authed`, version; and once authorised the addresses, contracts, SSID, pending value, note, scan generation |
| `GET /api/scan` | the device's last Wi-Fi scan |
| `POST /api/auth` | ask to be authorised → returns the token; the panel decides (except the Wi-Fi-only wizard, which grants it) |
| `POST /api/payout` | `net=eth\|tron`, `addr=…` → proposed, panel confirms |
| `POST /api/contract` | same shape, for the ERC-20 / TRC-20 contract |
| `POST /api/card` | read the payout addresses off a Cryptnox card |
| `POST /api/wifi` | `ssid=…`, `pass=…` |
| `POST /api/rescan` | ask the device to scan again |
| `POST /api/next` | wizard: leave this step |
| `POST /api/ota` | firmware image body (see [ota.md](ota.md)) |

Bodies are `application/x-www-form-urlencoded` and parsed by `form_field()`
(`main/form_parse.h`), which is the one piece of this that reads input a stranger
controls and is therefore host-tested — `tests/units/test_prov_form.cpp`, and
`test_addr_check.cpp` for the address checks it feeds.

### Why the Wi-Fi scan is not in the handler

Scanning makes the radio hop channels, which briefly drops whoever is joined to the
SoftAP — including the browser making the request. So `net_wifi_scan()` runs on the
main task at known moments (entering the Wi-Fi step, or a rescan the browser asked
for) and hands the result to `prov_set_scan()`; the handler serves the cached list.
The page refetches when `scan_gen` moves.

In admin mode a scan interrupts the page rather than the AP, which is why the
rescan there is an explicit button and the page says what it will do.
