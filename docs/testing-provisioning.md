# Testing the phone-setup portal

Test plan for the SoftAP + captive portal + setup forms added in `main/provision.cpp`.

Three layers, cheapest first. Do them in order — a failure at layer 1 makes
layers 2 and 3 meaningless.

1. **Host** — the urlencoded parser, no hardware.
2. **Bench** — a laptop joined to the AP, `curl` and `nslookup`. Proves the DNS
   and HTTP halves independently of any phone's opinion.
3. **Handset** — real iOS and real Android. The only thing that proves the portal
   *opens by itself*, which is the whole feature.

---

## 0. Prerequisites

The CYD's CH340 moves COM ports between sessions. Find it first:

```bash
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"
```

Build, flash and read the log (substitute your port):

```bash
cmd //c "C:\Cryptnox\cryptnox-pos\scripts\idf-build-flash.bat COM7"
"C:/Users/Yann/.espressif/python_env/idf5.5_py3.14_env/Scripts/python.exe" \
    scripts/serial_tail.py COM7 20
```

### Getting into first-run setup

The portal only runs on a virgin or factory-reset terminal — the trigger is
`!settings_has_admin_code()`. Two ways in:

| Method | What it costs | When |
|---|---|---|
| Settings → Reset → confirm | admin code, Wi-Fi creds, brightness, fees, payout addresses | normal test loop |
| `idf.py -p COM7 erase-flash` then flash | all of the above, plus the AP passphrase in the `prov` namespace | testing a truly virgin unit |

Use `erase-flash` at least once: it is the only way to exercise passphrase
generation, since a factory reset deliberately leaves the `prov` namespace alone.

---

## 1. Host tests

```bash
for t in test_eth_addr test_hardening test_tron_tx test_prov_form; do
  g++ -std=c++14 -Wall -Imain -Icryptnox-sdk-esp32/cryptnox-sdk-cpp \
      tests/units/$t.cpp -o $t && ./$t || exit 1
done
```

`test_prov_form` covers `main/form_parse.h`: prefix anchoring (`wifipass` must
not answer a lookup for `pass`), percent and plus decoding, malformed escapes,
encoded `&` and `=` inside values, and truncation at the buffer boundary. All
four must print `OK`.

---

## 2. Bench tests — laptop on the AP

Factory-reset, tap **Start** on the welcome screen. The screen should show the
QR code, `Cryptnox-XXXX` and a 10-character passphrase. The serial log carries
both:

```
I (nnnn) prov: setup portal up: SSID 'Cryptnox-A2E8', pass 'K7QMR3XPZW', http://192.168.4.1/
```

Join that network from a laptop, then:

### 2.1 DNS hijack

```bash
nslookup captive.apple.com 192.168.4.1
nslookup anything-at-all.invalid 192.168.4.1
```

**Pass:** both answer `192.168.4.1`. A made-up name answering is the point, not a
bug — every lookup resolves to the portal.

**Fail:** timeout means the DNS task never started. Check the log for
`DNS task failed - captive portal will NOT auto-open`.

### 2.2 Probe endpoints

```bash
for u in generate_204 gen_204 connecttest.txt ncsi.txt canonical.html \
         success.txt chat some/random/path; do
  printf '%-22s ' "$u"
  curl -s -o /dev/null -w '%{http_code} -> %{redirect_url}\n' \
       "http://192.168.4.1/$u"
done
curl -s http://192.168.4.1/hotspot-detect.html
```

| URL | Expected | Why it matters |
|---|---|---|
| `/generate_204`, `/gen_204` | `302 -> http://192.168.4.1/` | Android needs a **non**-204 |
| `/connecttest.txt`, `/ncsi.txt` | `302` | Windows NCSI |
| `/canonical.html`, `/success.txt` | `302` | Firefox |
| any unregistered path | `302` | the 404 catch-all |
| `/hotspot-detect.html` | `200`, body **without** the word `Success` | iOS follows redirects then compares the body, so a 302 here is wrong |

A `204` from `/generate_204` or a body containing `Success` from the Apple probe
means the portal will not auto-open on that platform. That is the failure this
whole feature exists to avoid — treat it as a blocker, not a cosmetic issue.

### 2.3 Step gating

The page must only accept the step the device is actually waiting on:

```bash
# while the device is on step 1 (admin code)
curl -si -X POST -d 'ssid=x&pass=y' http://192.168.4.1/wifi | head -1
curl -si -X POST -d 'addr=0x0&net=eth' http://192.168.4.1/addr | head -1
```

**Pass:** both `400`, body says "Not the current step."

### 2.4 Admin code form

```bash
P=http://192.168.4.1/admin
curl -s -X POST -d 'code=123&again=123'      $P | grep -o 'Not accepted'   # too short
curl -s -X POST -d 'code=1234&again=4321'    $P | grep -o 'Not accepted'   # mismatch
curl -s -X POST -d 'code=12a4&again=12a4'    $P | grep -o 'Not accepted'   # not digits
curl -s -X POST -d 'code=1234567890&again=1234567890' $P | grep -o 'Not accepted'  # too long
curl -s -X POST -d 'code=1234&again=1234'    $P | grep -o 'Done'           # accepted
```

**Pass:** the first four print `Not accepted`, the last prints `Done` and the
device advances to step 2 on its own.

Then confirm the code actually works: finish setup, open the burger menu, and
check that `1234` unlocks it and `9999` does not.

### 2.5 Wi-Fi form

```bash
curl -s -X POST -d 'ssid=&pass=x' http://192.168.4.1/wifi | grep -o 'Not accepted'
curl -s -X POST --data-urlencode 'ssid=My Cafe' \
     --data-urlencode 'pass=p@ss&word' http://192.168.4.1/wifi
```

**Pass:** empty SSID is rejected. A valid submission returns the "about to drop"
page *before* the radio moves, and the device screen switches to
"Connecting to My Cafe…".

Note the second case deliberately puts `&` and a space in the credentials —
that is the path `test_prov_form` covers, verified here end to end.

**Expected and not a bug:** the laptop loses the AP a second or two later. One
radio, one channel. If the network was wrong, the device screen returns to the
Wi-Fi step with a reason and the AP comes back.

### 2.6 Payout address form

Get to step 3 (finish steps 1 and 2 first), then:

```bash
A=http://192.168.4.1/addr
# Ethereum — bad EIP-55 checksum (one character case-flipped)
curl -s -X POST -d 'net=eth&addr=0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAeD' $A | grep -o 'Not accepted'
# Ethereum — too short
curl -s -X POST -d 'net=eth&addr=0xdead' $A | grep -o 'Not accepted'
# Tron — 33 characters
curl -s -X POST -d 'net=tron&addr=THQGuFzL87ZqhxkgqYEryRAd7gqFqL5rd' $A | grep -o 'Not accepted'
# Tron — wrong prefix
curl -s -X POST -d 'net=tron&addr=AHQGuFzL87ZqhxkgqYEryRAd7gqFqL5rdc' $A | grep -o 'Not accepted'
# Tron — non-base58 character ('0')
curl -s -X POST -d 'net=tron&addr=T0QGuFzL87ZqhxkgqYEryRAd7gqFqL5rdc' $A | grep -o 'Not accepted'
# Valid, all-lowercase Ethereum (no checksum to verify — accepted by design)
curl -s -X POST -d 'net=eth&addr=0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed' $A | grep -o 'Done'
```

**Pass:** the five bad ones are rejected with a reason; the lowercase one is
accepted, because an all-lowercase address carries no checksum to check (same
lenient rule `eth_addr_parse` applies to `config.h`, see `eth_addr.cpp:76`).

### 2.7 The security boundary

This is the most important test in the document.

1. Submit a valid address from the laptop.
2. **The address must appear on the device screen**, wrapped in full, with
   Accept and Reject buttons. Nothing is stored yet.
3. Tap **Reject**. Check the log: no `payout(...) set to ...` line.
4. Submit a *second* address while the first modal is still up → expect `400`,
   "Another address is already waiting".
5. Submit again, tap **Accept**. Expect:
   ```
   W (nnnn) settings: payout(eth) set to 0x...
   W (nnnn) cryptnox_pos: payout address stored - restarting to apply it
   ```
6. After the restart, take a payment and check the Confirm screen's **To** row
   shows the new address.

**Fail:** any path that stores an address without a tap on the panel. A phone is
allowed to propose; only the panel may commit.

### 2.8 Portal lifetime

After setup finishes (or **Keep current address** is tapped), scan for Wi-Fi
networks from the laptop.

**Pass:** `Cryptnox-XXXX` is gone, log shows `setup portal down` and
`SoftAP down`. The AP must not outlive setup — it is the attack surface.

---

## 3. Handset tests

Repeat on **both** a real iPhone and a real Android phone. Simulators do not
run the connectivity probes.

1. Factory-reset, tap Start.
2. Open the camera, point it at the QR code.
3. **Pass:** the phone offers to join `Cryptnox-XXXX`, joins, and the setup page
   opens by itself within a few seconds. No typing, no URL.
4. Walk all three steps from the phone.
5. **Pass:** step 3's address appears on the terminal for acceptance.

Failure modes worth telling apart:

| Symptom | Cause |
|---|---|
| Joins, no page | probe answered correctly — check §2.2 for that platform |
| Joins, then drops back to cellular | phone decided there is no internet; usually the Apple probe body |
| "Cannot join" | passphrase mismatch — check the screen against the log |
| Page opens but is blank | `httpd` out of memory; check the log for allocation failures |

Also worth doing once on each platform: put the phone into airplane mode and
back, and re-join from Settings rather than the QR code. The portal should open
both times.

---

## 4. Regression tests

The setup portal touched the money path. These must pass on a terminal that is
**not** in first-run:

| Check | Expected |
|---|---|
| Boot with no stored payout address | log shows the `config.h` recipient; `Tron recipient: THQGuFzL...` |
| Boot after storing an Ethereum address | Confirm screen **To** row shows the stored one |
| Factory reset, then boot | back to the `config.h` recipient |
| All-lowercase `ADDR_TO` in `config.h` | `recipient is all-lowercase: no EIP-55 checksum verified` |
| Existing terminal, unchanged NVS | boots to `Ready` with no portal and no new warnings |
| Wi-Fi picker on a later boot (saved network unplugged) | the on-device picker still appears — the portal is down, so there is no QR screen to offer |

### 4.1 Corruption fallback

`settings_get_payout()` stores the address twice and compares. To exercise the
mismatch path you need to write one copy and not the other, which nothing in the
firmware does — it is currently covered by inspection only.

<!-- ponytail: no test hook for the echo-mismatch fallback. Add a debug-only
     command that writes pay_eth without pay_eth_e if this path ever changes. -->

---

## 5. What this plan does not cover

- **Two phones at once.** `max_connection` is 2, and the step gating plus the
  single-slot address proposal are the only concurrency controls. Untested.
- **Portal RAM headroom.** `httpd` plus LVGL plus TLS on an ESP32 is tight;
  nothing here measures free heap with the AP up.
- **The `esp_restart()` path under a payment.** The address step runs before the
  NFC reader starts, so the *portal* cannot collide today. The firmware-update
  window can — it is offered from the settings menu and it reboots on purpose.
  Covered in [`docs/ota-testing.md`](ota-testing.md) §6.1.
