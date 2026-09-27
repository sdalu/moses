Building
========

The `Makefile` wraps CMake; `make help`, `make options` and
`make flavours` print every target and knob with its current value.


Prerequisites
-------------

A C23 compiler, [CMake](https://cmake.org/) ≥ 3.13, and:

| Needed for                        | Debian package / source                    |
| --------------------------------- | ------------------------------------------ |
| MQTT (`WITH_MQTT`, on by default) | `libmosquitto-dev`                         |
| `moses_watermeter`                | libmbus, usually not packaged: see below   |
| `WITH_DBUS`                       | `libdbus-1-dev`                            |
| `moses_display` (`WITH_DISPLAY`)  | a C++ compiler; SDL2 for the `sdl` backend |

LVGL, the BME280 sensor API and `bitters` are git submodules under
`3rd/`: clone with `--recursive`, or run
`git submodule update --init --recursive` afterwards. A Pi Zero that
runs out of memory building LVGL wants some [swap](system.md#swap).

### libmbus

`moses_watermeter` links [libmbus](https://github.com/rscada/libmbus),
which the build looks for under `/opt/libmbus`:

~~~sh
git clone https://github.com/rscada/libmbus
cd libmbus
./build.sh
./configure --prefix=/opt/libmbus
make
make install
~~~

Its command-line tools are the quickest check that the meter answers,
before any daemon is involved:

~~~sh
/opt/libmbus/bin/mbus-serial-scan -b 2400 /dev/ttyAMA0            # find its address
/opt/libmbus/bin/mbus-serial-request-data -b 2400 /dev/ttyAMA0 1  # read it
~~~


Flavours
--------

`FLAVOUR` names the machine being built for and sets the knobs'
defaults; any knob given on the command line still wins
(`make build FLAVOUR=viewer WITH_LOG=yes`).

~~~sh
make build                      # device:  the Pi, daemons and panel
make build FLAVOUR=sensors      # sensors: the same Pi, no panel
make build FLAVOUR=viewer       # viewer:  the panel in a window, anywhere
~~~

| knob                | `device`            | `sensors`           | `viewer` |
| ------------------- | ------------------- | ------------------- | -------- |
| `WITH_MQTT`         | yes                 | yes                 | yes      |
| `WITH_LINEPROTOCOL` | yes                 | yes                 | no       |
| `WITH_DBUS`         | yes                 | yes                 | no       |
| `WITH_WATERMETER`   | yes                 | yes                 | no       |
| `WITH_BREAKER`      | yes                 | yes                 | no       |
| `WITH_TEMPERATURE`  | yes                 | yes                 | no       |
| `WITH_DISPLAY`      | yes                 | no                  | yes      |
| `DISPLAY_BACKEND`   | automation-hat-mini | automation-hat-mini | sdl      |

**`device`, the default, includes the panel and so the whole of
`3rd/lvgl`** — some seven hours on a Pi Zero. `sensors` is the same Pi
without it. `viewer` builds `moses_display` alone against SDL and
watches the broker from any machine; it needs libmosquitto but none of
the Pi's hardware. Why the sinks follow the flavour is in
[DESIGN.md](../DESIGN.md).


Knobs
-----

Set them on the `make` command line as `KNOB=yes` or `KNOB=no`. The
default shown is the one without a flavour, as `make options` prints it.

| Knob                 | Default               | What it does                                                               |
| -------------------- | --------------------- | -------------------------------------------------------------------------- |
| `WITH_MQTT`          | yes                   | Publish to the broker, and take `state/set` from it                        |
| `WITH_LINEPROTOCOL`  | no                    | One [line](interfaces.md#line-protocol-output) per reading on stdout       |
| `WITH_DBUS`          | no                    | The same line on the [system bus](interfaces.md#the-system-bus)            |
| `MQTT_TOPIC_PREFIX`  | `water-breaker`       | Compiled-in topic prefix; the environment overrides it                     |
| `WITH_WATERMETER`    | yes                   | Build `moses_watermeter`, the only one wanting libmbus                     |
| `WITH_BREAKER`       | yes                   | Build `moses_breaker`                                                      |
| `WITH_TEMPERATURE`   | yes                   | Build `moses_sensors`                                                      |
| `WITH_DISPLAY`       | no                    | Build `moses_display`; needs a C++ compiler and LVGL                       |
| `WITH_DISPLAY_TESTS` | no                    | The [screenshot tests](../test/README.md#screenshot-tests); LVGL, no panel |
| `DISPLAY_BACKEND`    | `automation-hat-mini` | Or `sdl`, for a window                                                     |
| `RPI_GPIO_CHIP`      | empty                 | Pin the panel's GPIO controller, e.g. `pinctrl-bcm2835`                    |
| `WITH_TESTS`         | no                    | Build the [unit tests](../test/README.md)                                  |
| `WITH_LOG`           | no                    | Log messages on stderr                                                     |
| `WITH_WERROR`        | no                    | Warnings are errors (CI)                                                   |
| `WITH_ANALYZER`      | no                    | Run the GCC static analyzer (CI)                                           |

The three sinks are independent: a build may have any of them. With
`WITH_MQTT` off, `moses_breaker` can be commanded only over the bus, or
not at all without `WITH_DBUS` — it still holds the line and fails the
valve open when it stops — and `WITH_DISPLAY` is refused.

`RPI_GPIO_CHIP` concerns only the panel's pins, which go through
bitters; left empty, bitters tries `pinctrl-rp1`, `pinctrl-bcm2711`,
`pinctrl-bcm2835`, then `gpiochip0`. The daemons' `rpi:<n>` pins are
looked up on their own (see [programs.md](programs.md#gpio-pins)).

The executables land in `bin/`.


Install
-------

`make install` copies what the configuration built into `$(PREFIX)/bin`
(`/usr/local` by default; `DESTDIR` stages it), and `make uninstall`
removes it. With `WITH_DBUS`, the bus policy goes where `dbus-daemon`
reads it, or the daemons cannot own their names:

~~~sh
make install FLAVOUR=sensors
install -m 0644 dbus/moses.conf /etc/dbus-1/system.d/
~~~

The scripts under `scripts/` — [`loop-runner`](programs.md#supervision)
and [`nut-notify`](system.md#nut) — run from anywhere and need no
installation step.
