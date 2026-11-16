# Embedded CA bundle

Unpinned RPC endpoints (no `*_CA_CERT_PEM` in `config.h`) trust only the `.pem` files in this
directory (119 roots):

- `cacert-2026-10-20.pem`: curl's Mozilla-derived snapshot of October 20, 2026 (118 roots), unmodified.

  - Source: https://curl.se/ca/cacert-2026-10-20.pem
  - SHA-256: `796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe`
    (`cacert-2026-10-20.pem.sha256`, as published by curl)
  - License: MPL 2.0, see https://curl.se/docs/caextract.html

ESP-IDF v5.5.4's built-in lists still include `emSign ECC Root CA - G3` and
`AAA Certificate Services`, whose subordinate CA keys were compromised in the
eMudhra incident (https://emudhra.we-have-a-problem.com/). With those roots a
man-in-the-middle could answer the terminal's balance and receipt queries and
make it accept a payment that never happened. This snapshot drops both.

It keeps one eMudhra root, on purpose: **emSign Root CA - G1** (RSA 2048, SHA-256
`40F6AF0346A99AA1CD1D555A4E9CCE62C7F9634603EE406615833DC8C8D00367`, expires
2043-02-18). The compromised intermediates were P-384 keys under the ECC G3 root,
and Mozilla kept G1 (bug 2083449). None of our RPC providers chains to it, so it
buys nothing, but removing it means editing curl's file and losing the check
against curl's published hash. `scripts/checks.sh` fails if any other eMudhra root
appears. If eMudhra trust is ever pulled further, drop the snapshot for one that
excludes G1 rather than editing this one.

- `globalsign-root-ca.pem`: **GlobalSign Root CA** (R1, SHA-256
  `EBD41040E4BB3EC742C9E381D31EF2A41A48B6685C96E7CEF3C1DF6CD4331C99`, expires
  2028-01-28), copied from IDF's `cacrt_all.pem`. Mozilla retired it, but
  publicnode (the EVM RPCs) serves GTS Root R4 cross-signed by it, and IDF's bundle
  check only looks up the issuer of the topmost certificate the server sends; it
  does not path-build to the GTS Root R4 already in curl's file. Without it every
  EVM call fails "No matching trusted root certificate found". Drop it once
  publicnode stops sending that cross-sign. Dropping it does not fix the 2028
  expiry: if publicnode still sends the cross-sign then, EVM breaks either way, so
  pin publicnode's chain or change provider before that date. `scripts/checks.sh`
  fails 180 days before any root here expires.

`sdkconfig.defaults` selects `DEFAULT_NONE` plus this directory as the custom bundle
(every `.pem` here is trusted, so add nothing else); a custom
bundle is otherwise *added* to IDF's list. `CMakeLists.txt` refuses to configure
if `sdkconfig` says anything else. After changing the bundle, do a clean build
(`idf.py fullclean`); terminals in the field get it only through a firmware update.

To update: download a newer dated snapshot and its `.sha256` from curl, check the
hash, replace the old snapshot here, and update the name and root count in
`scripts/checks.sh`, which CI runs.
Never point the build at a mutable URL.
