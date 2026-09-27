# battery-config

Change battery warning thresholds for your goggles.

A [wtfOS](https://github.com/fpv-wtf/wtfos) package for the DJI FPV Goggles V1 and V2. The goggles warn about a low battery (on-screen message and buzzer) at a fixed 3.5 V per cell. That is sensible for LiPo packs but far too early for Li-ion. This package makes the threshold configurable from the fpv.wtf configurator, from 2.5 V to 3.5 V per cell in 0.125 V steps. The default is 3.0 V per cell.

The cell count can be set as well. The goggles detect it from the pack voltage when they start, and a pack that is not fully charged is detected with too few cells, e.g. a 4S pack below 12.9 V as 3S. The warning then fires far too late.

Only lower the threshold for Li-ion packs. With a LiPo or the original DJI battery, keep the stock setting.

## How it works

`gs_get_pigeon_battery_info` detects the cell count once, on the first reading after the goggles app starts, and warns when

```
cell_count = (uint)(pack_voltage * 100.0 / 430.0 + 1.0)
pack_voltage < cell_count * 3.5
```

The 3.5 is a single instruction, `vmov.f64 d19, #3.5` (`F0 EE 0C 3B`). Its VFP immediate encodes `(1 + n/16) * 2` V, with `n` in the low nibble of byte 2, so each 0.125 V step between 2.0 and 3.875 V is a one-byte change:

| V/cell | byte 2 |
|-------:|:------:|
| 3.5 (stock) | `0C` |
| 3.0 | `08` |
| 2.5 | `04` |

For the cell count, the `+ 1.0` (`vadd.f32 s6, s4, s5`) is replaced with `vmov.f32 s6, #cells`.

`libbattery_config.so` is preloaded into the goggles app by the wtfOS modloader (`dji_gls_wm150_original` on V2, `dji_glasses_original` on V1). On load it reads the configured values, finds the instructions in the app's executable mappings and rewrites them in memory. The on-disk binary is never modified.

Each patch is only applied when its instruction sequence appears exactly once with the stock bytes, so an unknown firmware keeps the stock behaviour.

## Supported firmware

Both instruction sequences are present exactly once in every goggles app build that runs wtfOS:

| Goggles | Firmware | Binary |
|---|---|---|
| V1 | 01.00.0606, 01.00.0608 | `dji_glasses` |
| V2 | 01.00.0606 through 01.07.0000 | `dji_gls_wm150` |

## Configuration

The settings appear under the package in the fpv.wtf configurator. Saving restarts the `battery-config` dinit unit, which restarts the goggles app with the new values. No reboot is needed.

The values are stored in `/opt/etc/package-config/battery-config/config.json`:

```json
{ "per_cell_mv": 3000, "cells": 0 }
```

`cells` is `0` for automatic detection, or `2` to `4`.

## Build

Requires the Android NDK, GNU make, `install` and `tar`.

```sh
make NDK=/path/to/android-ndk ipk    # build/battery-config_0.2.0_pigeon-glasses.ipk
```

`NDK` defaults to `$ANDROID_NDK_HOME`.

## Manual install

```sh
adb push build/battery-config_0.2.0_pigeon-glasses.ipk /tmp/
adb shell 'export HOME=/data PATH=/opt/bin:/opt/sbin:$PATH; opkg install /tmp/battery-config_0.2.0_pigeon-glasses.ipk'
```

`HOME=/data` lets the package scripts reach dinit from an adb shell. Remove with `opkg remove battery-config` the same way.

## Package contents

```
/opt/etc/preload.d/libbattery_config.so
/opt/etc/package-config/battery-config/   settings schema and config.json
/opt/etc/dinit.d/battery-config           enables the mod via modmanager
```

## License

GPL-3.0-only. See [LICENSE](LICENSE).
