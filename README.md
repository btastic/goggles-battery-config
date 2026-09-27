# battery-config

Change battery warning thresholds for your goggles.

A [wtfOS](https://github.com/fpv-wtf/wtfos) package for the DJI FPV Goggles V1
and V2. The goggles warn about a low battery (on-screen message and buzzer) at a
fixed 3.5 V per cell. That is sensible for LiPo packs but far too early for
Li-ion. This package makes the threshold configurable from the fpv.wtf
configurator, from 2.5 V to 3.5 V per cell in 0.125 V steps. The default is
3.0 V per cell.

Only lower the threshold for Li-ion packs. With a LiPo or the original DJI
battery, keep the stock setting.

## How it works

The warning fires in `gs_get_pigeon_battery_info` when

```
pack_voltage < cell_count * 3.5
```

The 3.5 is a single instruction, `vmov.f64 d19, #3.5` (`F0 EE 0C 3B`). Its VFP
immediate encodes `(1 + n/16) * 2` V, with `n` in the low nibble of byte 2, so
each 0.125 V step between 2.0 and 3.875 V is a one-byte change:

| V/cell | byte 2 |
|-------:|:------:|
| 3.5 (stock) | `0C` |
| 3.0 | `08` |
| 2.5 | `04` |

`libbattery_config.so` is preloaded into the goggles app by the wtfOS modloader
(`dji_gls_wm150_original` on V2, `dji_glasses_original` on V1). On load it reads
the configured value, finds the instruction in the app's executable mappings and
rewrites that byte in memory. The on-disk binary is never modified.

It patches nothing unless the instruction appears exactly once and still holds
the stock value, so an unknown firmware keeps the stock behaviour.

## Supported firmware

The instruction is present exactly once in every goggles app build that runs
wtfOS:

| Goggles | Firmware | Binary |
|---|---|---|
| V1 | 01.00.0606, 01.00.0608 | `dji_glasses` |
| V2 | 01.00.0606 through 01.07.0000 | `dji_gls_wm150` |

## Configuration

The setting appears under the package in the fpv.wtf configurator. Saving
restarts the `battery-config` dinit unit, which restarts the goggles app with
the new value. No reboot is needed.

The value is stored in `/opt/etc/package-config/battery-config/config.json`:

```json
{ "per_cell_mv": 3000 }
```

## Build

Requires the Android NDK, GNU make, `install` and `tar`.

```sh
make NDK=/path/to/android-ndk ipk    # build/battery-config_0.1.0_pigeon-glasses.ipk
```

`NDK` defaults to `$ANDROID_NDK_HOME`.

## Manual install

```sh
adb push build/battery-config_0.1.0_pigeon-glasses.ipk /tmp/
adb shell 'export HOME=/data PATH=/opt/bin:/opt/sbin:$PATH; opkg install /tmp/battery-config_0.1.0_pigeon-glasses.ipk'
```

`HOME=/data` lets the package scripts reach dinit from an adb shell. Remove with
`opkg remove battery-config` the same way.

## Package contents

```
/opt/etc/preload.d/libbattery_config.so
/opt/etc/package-config/battery-config/   settings schema and config.json
/opt/etc/dinit.d/battery-config           enables the mod via modmanager
```

## License

GPL-3.0-only. See [LICENSE](LICENSE).
