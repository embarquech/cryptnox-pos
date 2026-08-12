# Unit tests — cryptnox-pos

Host-side unit tests. Each is a single translation unit that `#include`s the
production sources directly (same pattern as [`fuzz/`](../../fuzz)), so a test
can never drift from the firmware and no build system is required.

| Test               | Under test                                     |
|--------------------|------------------------------------------------|
| `test_eth_addr`    | `main/eth_addr.cpp` — hex parsing + EIP-55     |
| `test_hardening`   | `main/hardening.h` — decision-integrity gate   |
| `test_tron_tx`     | `main/tron_tx.cpp` — Tron varints, TransferContract + TRC-20 checks, envelope |
| `test_prov_form`   | `main/form_parse.h` — setup-portal urlencoded field extraction |
| `test_ota_version` | `main/ota_version.h` — update vs downgrade ordering |

Build & run from the repo root (any C++14 compiler):

```sh
for t in test_eth_addr test_hardening test_tron_tx test_prov_form test_ota_version; do
  g++ -std=c++14 -Wall -Imain -Icryptnox-sdk-esp32/cryptnox-sdk-cpp \
      tests/units/$t.cpp -o $t && ./$t || exit 1
done
```

Assertions only — a test either prints `... OK` and exits 0, or aborts.

<!-- ponytail: no CMake/ctest wiring; add it when CI runs these. -->
