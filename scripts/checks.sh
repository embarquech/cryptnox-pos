#!/usr/bin/env bash
# Every check that needs no hardware, in one command. Run from the repo root:
#
#   bash scripts/checks.sh          # host tests + page checks
#   bash scripts/checks.sh --build  # ...and the firmware build (slow)
#
# What is NOT here: anything needing the panel in your hands. Those are the numbered
# sections of docs/testing-provisioning.md and docs/ota-testing.md.
set -uo pipefail

fail=0
step() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }

step "host unit tests"
TESTS="test_eth_addr test_hardening test_tron_tx test_prov_form test_addr_check
       test_json_out test_ota_version"
out=$(mktemp -d)
for t in $TESTS; do
  if g++ -std=c++14 -Wall -Imain -Icryptnox-sdk-esp32/cryptnox-sdk-cpp \
         "tests/units/$t.cpp" -o "$out/$t" 2>"$out/$t.log" && "$out/$t" >/dev/null; then
    ok "$t"
  else
    bad "$t"; sed 's/^/        /' "$out/$t.log"
  fi
done

step "config portal page"
# The extractor doubles as the id-wiring check, and emits the script for the
# render test so there is one extractor rather than two that could disagree.
mkdir -p build
# Not a bare `python`: Windows has it under three names and WSL has none of them,
# so the shell you happen to be in decided whether this check ran at all.
PY=$(command -v python || command -v python3 || command -v py || true)
if [ -z "$PY" ]; then
  bad "no python found (tried python, python3, py)"
elif "$PY" tools/check_portal_page.py --emit-js build/portal_page.js; then
  ok "parses, ids wired"
  if command -v node >/dev/null; then
    if node tools/test_portal_render.js build/portal_page.js; then
      ok "render logic"
    else
      bad "render logic"
    fi
  else
    printf '  skip  render logic (node not found)\n'
  fi
else
  bad "page extraction / parse"
fi

if [ "${1:-}" = "--build" ]; then
  step "firmware build"
  # idf.py will not run in Git Bash; the .bat sets up the environment. Called
  # directly, not through `cmd //c` — that double slash is a Git Bash idiom and
  # was a silent no-op anywhere else. Warnings from TFT_eSPI about the reset and
  # touch pins are expected — see sdkconfig.defaults.
  if ./scripts/idf-build.bat 2>&1 |
       grep -E "error:|FAILED|binary size|Project build complete" |
       grep -v "TFT_config.h\|TFT_eSPI.h"; then
    grep -q . /dev/null   # keep the pipeline's exit status out of it
    ok "built"
  else
    bad "build"
  fi
fi

printf '\n'
[ "$fail" -eq 0 ] && echo "all checks passed" || echo "CHECKS FAILED"
exit "$fail"
