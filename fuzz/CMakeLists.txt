cmake_minimum_required(VERSION 3.16)
project(cryptnox_pos_fuzz CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All three harnesses are single-TU: each #includes the production code (or a
# verbatim copy) under test, so the include path only needs the project's
# main/ directory plus, for the JSON target, the SDK and cJSON sources.

set(SDK_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/../cryptnox-sdk-esp32/cryptnox-sdk-cpp")

set(FUZZ_FLAGS -fsanitize=fuzzer,address -g -O1 -Wall -Wno-unused-function)

# ── Target 1: eth_rlp encoder (pure, no external deps) ────────────────
add_executable(fuzz_eth_rlp fuzz_eth_rlp.cpp)
target_compile_options(fuzz_eth_rlp PRIVATE ${FUZZ_FLAGS})
target_link_options(fuzz_eth_rlp    PRIVATE -fsanitize=fuzzer,address)

# ── Target 2: hex address parser (pure, verbatim copy) ────────────────
add_executable(fuzz_parse_address fuzz_parse_address.cpp)
target_compile_options(fuzz_parse_address PRIVATE ${FUZZ_FLAGS})
target_link_options(fuzz_parse_address    PRIVATE -fsanitize=fuzzer,address)

# ── Target 3: JSON-RPC response parser (needs cJSON + CW_Utils) ───────
# cJSON ships with ESP-IDF; locate it via $IDF_PATH.
if(DEFINED ENV{IDF_PATH})
    set(CJSON_DIR "$ENV{IDF_PATH}/components/json/cJSON")
endif()

if(EXISTS "${CJSON_DIR}/cJSON.c")
    add_executable(fuzz_eth_rpc_json
        fuzz_eth_rpc_json.cpp
        "${CJSON_DIR}/cJSON.c")
    target_include_directories(fuzz_eth_rpc_json PRIVATE
        "${SDK_DIR}"        # CW_Utils.h / CW_Utils.cpp
        "${CJSON_DIR}")     # cJSON.h
    target_compile_options(fuzz_eth_rpc_json PRIVATE ${FUZZ_FLAGS})
    target_link_options(fuzz_eth_rpc_json    PRIVATE -fsanitize=fuzzer,address)
else()
    message(WARNING
        "cJSON not found (set IDF_PATH, or CJSON_DIR=<dir with cJSON.c>) — "
        "skipping fuzz_eth_rpc_json. The other two targets still build.")
endif()
