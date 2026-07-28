# Unit tests — cryptnox-pos

Host-side unit tests. Each is a single translation unit that `#include`s the
production sources directly (same pattern as [`fuzz/`](../../fuzz)), so a test
can never drift from the firmware and no build system is required.

| Test               | Under test                                     |
|--------------------|------------------------------------------------|
| `test_eth_addr`    | `main/eth_addr.cpp` — hex parsing + EIP-55     |
| `test_hardening`   | `main/hardening.h` — decision-integrity gate   |

Build & run from the repo root (any C++14 compiler):

```sh
for t in test_eth_addr test_hardening; do
  g++ -std=c++14 -Wall -Imain -Icryptnox-sdk-esp32/cryptnox-sdk-cpp \
      tests/units/$t.cpp -o $t && ./$t || exit 1
done
```

Assertions only — a test either prints `... OK` and exits 0, or aborts.

<!-- ponytail: no CMake/ctest wiring; add it when CI runs these. -->
