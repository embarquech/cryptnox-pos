# Unit tests — cryptnox-pos

Host-side unit tests. Each is a single translation unit that `#include`s the
production sources directly (same pattern as [`fuzz/`](../../fuzz)), so a test
can never drift from the firmware and no build system is required.

| Test               | Under test                                     |
|--------------------|------------------------------------------------|
| `test_eth_addr`    | `main/eth_addr.cpp` — hex parsing + EIP-55, both ways |
| `test_hardening`   | `main/hardening.h` — decision-integrity gate   |
| `test_tron_tx`     | `main/tron_tx.cpp` — Tron varints, TransferContract + TRC-20 checks, envelope |
| `test_prov_form`   | `main/form_parse.h` — config-portal urlencoded field extraction |
| `test_addr_check`  | `main/addr_check.h` — structural Tron address check |
| `test_json_out`    | `main/json_out.h` — escaping an SSID into a JSON response |
| `test_ota_version` | `main/ota_version.h` — update vs downgrade ordering |

Run all of them, plus the config portal page checks, with:

```sh
bash scripts/checks.sh            # add --build for the firmware build too
```

Or one at a time, from the repo root (any C++14 compiler):

```sh
for t in test_eth_addr test_hardening test_tron_tx test_prov_form test_addr_check \
         test_json_out test_ota_version; do
  g++ -std=c++14 -Wall -Imain -Icryptnox-sdk-esp32/cryptnox-sdk-cpp \
      tests/units/$t.cpp -o $t && ./$t || exit 1
done
```

Assertions only — a test either prints `... OK` and exits 0, or aborts.

## The config portal page

The page is a pair of C string literals, so the compiler proves the C is valid and
nothing proves the HTML or the JavaScript is. Two checks in `tools/` cover it, and
`scripts/checks.sh` runs both:

| | |
|---|---|
| `check_portal_page.py` | extracts the literals, runs the script through `node --check`, and asserts every element id is referenced from both sides |
| `test_portal_render.js` | drives `render()` against each `(mode, step, authed, pending)` the device can report, asserting which sections are visible |

The second one matters more than it looks: which sections show is a hand-written
pile of booleans, it decides whether setup can be completed at all, and a mistake
there is invisible until somebody is standing in front of a blank phone.

<!-- ponytail: no CMake/ctest wiring; add it when CI runs these. -->
