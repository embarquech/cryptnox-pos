# Fuzzing — cryptnox-pos parsers

libFuzzer + AddressSanitizer harnesses for the three byte-parsing surfaces in
this firmware. Same pattern as the SDK's `cryptnox-sdk-cpp/fuzz` (single-TU
harness, ASan, corpus dir).

| Harness                  | Target                     | Source under test            |
|--------------------------|----------------------------|------------------------------|
| `fuzz_eth_rlp`           | `eth_rlp_encode_unsigned` / `_signed` | `main/eth_rlp.cpp` |
| `fuzz_parse_address`     | `eth_addr_parse`           | `main/eth_addr.cpp` |
| `fuzz_eth_rpc_json`      | `eth_json_result_string` + `eth_json_receipt_status` | `main/eth_json.cpp` (+ cJSON, `CW_Utils`) |

Each harness `#include`s the production `.cpp` under test directly — **no
copies**, so the fuzzers can never drift from the firmware. The two parsers
that used to be `static` inside ESP-IDF-welded translation units now live in
their own pure units (`eth_addr.cpp`, `eth_json.cpp`) precisely so both the
firmware and the fuzzers share one source.

The `eth_json` parsers are the only ones fed straight from the network (HTTP
response bodies), so `fuzz_eth_rpc_json` is the highest-value target — it runs
both JSON entry points (`result` extraction and receipt classification) over
each input.

## Build

Linux / macOS, or WSL on Windows. Requires `clang` (libFuzzer) and, for the
JSON target only, `IDF_PATH` exported so cJSON can be found.

```sh
cd fuzz
mkdir build && cd build
cmake .. -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
make                       # builds all available targets
```

If `IDF_PATH` is unset the JSON target is skipped (a warning prints) and the
other two still build. To point at cJSON manually:
`cmake .. -DCJSON_DIR=/path/to/cJSON`.

## Run

```sh
./fuzz_eth_rlp        ../corpus/eth_rlp        -max_len=4096 -jobs=4
./fuzz_parse_address  ../corpus/parse_address  -max_len=64   -jobs=4
./fuzz_eth_rpc_json   ../corpus/eth_rpc_json   -max_len=4096 -jobs=4
```

A crash drops a `crash-<hash>` reproducer in the working directory; replay with
`./fuzz_<target> crash-<hash>`.

## Corpus

Seed files live under `corpus/<target>/`. They are starting points — libFuzzer
mutates and grows them. Add real captures (a live JSON-RPC body, a production
RLP blob) for faster coverage.
