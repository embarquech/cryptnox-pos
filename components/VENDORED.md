# Third-party display/touch components — provenance and pinning (F-20)

Both UI libraries are consumed as **git submodules pinned to an exact
upstream commit**, wrapped in a thin ESP-IDF component layer. The upstream
sources are **not modified** — all ESP-IDF glue lives in the wrapper
directories of this repository.

| Component | Upstream | Pinned commit | Upstream state at pin |
|-----------|----------|---------------|----------------------|
| `TFT_eSPI` | https://github.com/Bodmer/TFT_eSPI | `cbf06d7a214938d884b21d5aeb465241c25ce774` | master, 2023-12-22 ("Fix #3036"), between releases V2.5.34 and V2.5.43 (`git describe`: V2.5.34-14) — `library.properties` reads 2.5.43, bumped early upstream |
| `XPT2046_Touchscreen` | https://github.com/PaulStoffregen/XPT2046_Touchscreen | `f956c5d8ce3bf39169c7378416b89e7cfe70a034` | master, post-v1.4 (includes the upstream `Z_THRESHOLD` 400→300 change and `begin(SPI1)` doc) |

## Layout

```
components/TFT_eSPI/
├── CMakeLists.txt      <- wrapper (this repo): registers the upstream sources
├── Kconfig             <- wrapper (this repo): rsource of the upstream Kconfig
└── TFT_eSPI/           <- submodule, pristine upstream @ cbf06d7a
components/XPT2046_Touchscreen/
├── CMakeLists.txt      <- wrapper (this repo)
└── XPT2046_Touchscreen/<- submodule, pristine upstream @ f956c5d8
```

TFT_eSPI is configured exclusively through `sdkconfig.defaults`
(`CONFIG_TFT_*` symbols from the upstream Kconfig) — `User_Setup.h` inside
the library is **not** edited.

## History

The previous in-tree vendored copies were verified file-by-file against
upstream before the conversion (2026-06-04): they were exact snapshots of
the commits pinned above, with no local source patches (the only local
file was each component's ESP-IDF `CMakeLists.txt`, now a wrapper).

## Updating

1. `cd components/<name>/<name> && git fetch && git checkout <new tag/commit>`
2. Re-run a full build + on-device display/touch test.
3. Update the pinned commit in the table above and commit the gitlink.
