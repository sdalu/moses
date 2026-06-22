# CLAUDE.md

Guidance for working in this repository.

## Project

Moses is a DIY water-leak breaker for a Raspberry Pi. Three small C programs
talk to an MQTT broker:

- `moses_watermeter` — read the water meter (M-Bus index and/or GPIO pulse counting)
- `moses_breaker`    — open/close the solenoid valve via a relay
- `moses_sensors`    — read the optional BME280 (temperature, pressure, humidity)

Shared code lives in `src/common.c` / `src/common.h` (MQTT wrapper, option
parsers, latency tuning). Bundled deps are git submodules under `3rd/`.

## Building libmbus (required for the watermeter target)

`libmbus` is usually not packaged, so CMake's `find_path`/`find_library` for
`mbus/mbus.h` will fail until it is built and installed somewhere. It is
vendored as a submodule at `3rd/libmbus`.

To build and install it to a throwaway prefix (e.g. for local testing):

```sh
cd 3rd/libmbus
./build.sh                          # only needed once, regenerates autotools files
./configure --prefix=/tmp/libmbus
make -j"$(nproc)"
make install
```

The configure artifacts may already be present in the submodule, in which case
`./build.sh` can be skipped and `./configure --prefix=...` run directly.

This produces:

- `/tmp/libmbus/include/mbus/mbus.h`
- `/tmp/libmbus/lib/libmbus.{a,so}`

## Building the project

Point the `MBUS_*` cache variables at the prefix used above (the default
`HINTS` in `CMakeLists.txt` is `/opt/libmbus`):

```sh
cmake -B build -DWITH_LOG=1 -DWITH_PUT=1 \
  -DMBUS_INCLUDE_DIR=/tmp/libmbus/include \
  -DMBUS_LIBRARY=/tmp/libmbus/lib/libmbus.so
cmake --build build --parallel "$(nproc)"
```

Binaries are written to `bin/`.

Useful CMake options (see `CMakeLists.txt`): `WITH_LOG` (stderr logging),
`WITH_PUT` (line-protocol on stdout), `WITH_GUI` (experimental LVGL UI, needs a
C++ toolchain), `MQTT_TOPIC_PREFIX`.

If only the breaker/sensors/common code changed, those translation units do not
need M-Bus and can be syntax-checked directly without installing libmbus.
