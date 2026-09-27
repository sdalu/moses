Building
========

How to get moses compiled and installed. Most of it is `make`: the
`Makefile` wraps CMake, and `make help`, `make options` and
`make flavours` print every target and knob with its current value.

Prerequisites
-------------

A C23 compiler, [CMake](https://cmake.org/) (≥ 3.13) and the following
libraries are required:

* `libmosquitto-dev` — MQTT client (used by all four programs, unless
  `WITH_MQTT` is off)
* `libmbus` — M-Bus library (used by `moses_watermeter` only); see the
  [libmbus](#libmbus) section for building it, as it is usually not
  packaged. The build expects it under `/opt/libmbus`.

The remaining dependencies (LVGL, BME280 sensor API and `bitters`) are
bundled as git submodules under `3rd/`.

On a Debian/Raspberry Pi OS host:

~~~sh
sudo apt install build-essential cmake libmosquitto-dev
~~~

and, depending on what is built:

* `libdbus-1-dev` — with `WITH_DBUS`, which the `device` and `sensors`
  [flavours](#flavours) turn on;
* SDL2 — for the `viewer` flavour, whose display draws in a window;
* a C++ compiler — with `WITH_DISPLAY`, see the table below.

A Pi Zero that runs out of memory building LVGL wants some
[swap](system.md#swap).

### libmbus

`moses_watermeter` links [libmbus](https://github.com/rscada/libmbus)
to read the meter, at 2400 baud unless `-b` says otherwise (see
[programs.md](programs.md#moses_watermeter)). The library also installs
command-line tools that talk to the bus by hand, which is the quickest
check that the meter answers before any daemon is involved:

~~~sh
/opt/libmbus/bin/mbus-serial-request-data -b 2400 /dev/ttyAMA0 1
~~~

It is usually not packaged; to install it under `/opt/libmbus`:

~~~sh
git clone https://github.com/rscada/libmbus
cd libmbus
./build.sh
./configure --prefix=/opt/libmbus
make clean
make
make install
~~~

Get the source
--------------

Clone the repository together with its submodules:

~~~sh
git clone --recursive https://github.com/sdalu/moses
cd moses
~~~

If you already cloned without `--recursive`, fetch the submodules with:

~~~sh
git submodule update --init --recursive
~~~

Build with CMake
----------------

~~~sh
cmake -B build -DWITH_LOG=1 -DWITH_LINEPROTOCOL=1 -DMQTT_TOPIC_PREFIX=water-breaker
make  -C build
~~~

The first three say **where a reading goes** and are independent of each
other — a build may have all three, one, or none. The rest say what gets
built, one option per program, and the last is for diagnosis.

| CMake options       | Description                                                 |
|---------------------|-------------------------------------------------------------|
| `WITH_MQTT`         | Speak MQTT (**on** by default). Off compiles the MQTT half of `src/common.c` out and drops libmosquitto from the link entirely, for a machine that wants nothing but [line protocol](interfaces.md#line-protocol-output) on stdout. `moses_watermeter` and `moses_sensors` still do their job. `moses_breaker` is still built, and is then commanded over the [system bus](interfaces.md#the-system-bus-beside-mqtt) if `WITH_DBUS` is on; with neither it cannot be told to shut at all — it holds the line, reports its state, and fails the valve open when it stops. `WITH_DISPLAY` is refused outright, being a subscriber and nothing else. |
| `WITH_LINEPROTOCOL` | Also write each reading to stdout as one line of [InfluxDB line protocol](https://docs.influxdata.com/influxdb/latest/reference/syntax/line-protocol/) — `<measurement> <fields> <nanosecond-timestamp>` — for piping into a time-series database. Telegraf, VictoriaMetrics and QuestDB read the same format |
| `WITH_DBUS`         | Also put each reading on the [system bus](interfaces.md#the-system-bus-beside-mqtt), in the same [line protocol](interfaces.md#line-protocol-output) written to stdout, **and** have `moses_display` read it. For a consumer on **this** machine, which would otherwise cross the network twice to reach a broker on another one — and be cut off entirely when that network is. Each daemon owns a name on the bus while it runs and keeps the last line of each kind it emitted, so a consumer that starts late still gets them and learns at once when a daemon goes. Needs libdbus to build and `dbus/moses.conf` installed for the bus to allow it; sending queues and never blocks, and a bus that is missing is logged once and retried, so nothing may hold up a daemon counting pulses or holding a valve. |
| `MQTT_TOPIC_PREFIX` | Change the default prefix applied to topic (`water-breaker`)|
| `WITH_WATERMETER`   | Build `moses_watermeter` (**on** by default). The only program that wants M-Bus, so turning it off is what lets the tree configure where libmbus is not installed |
| `WITH_BREAKER`      | Build `moses_breaker` (**on** by default). `state/set` over MQTT and `SetState` over the [system bus](interfaces.md#the-system-bus-beside-mqtt) are the only ways to *command* the valve, so built with neither `WITH_MQTT` nor `WITH_DBUS` it cannot be told to shut — it still holds the line, reports its state to whatever sinks are on, and fails the valve open when it stops |
| `WITH_TEMPERATURE`  | Build `moses_sensors`, the BME280 reader (**on** by default) |
| `WITH_DISPLAY`      | Build [`moses_display`](programs.md#moses_display), the LVGL front panel. Off by default; needs a C++ compiler (LVGL's build enables the language even though nothing here uses it) and pulls in the `3rd/lvgl` submodule, which is a long compile on a Pi Zero. The three daemons build with just a C compiler. |
| `WITH_DISPLAY_TESTS`| Build the [screenshot tests](../test/README.md#screenshot-tests). Needs LVGL but no panel, so it stands alone on a machine that cannot build `moses_display` at all. |
| `WITH_TESTS`        | Build the unit tests (off by default, so a normal build skips them); see [Tests](../test/README.md). |
| `RPI_GPIO_CHIP`     | Pin the Raspberry Pi's GPIO controller instead of letting bitters find it. Empty by default, which leaves `BITTERS_RPI_GPIO_CHIP` as its list of chip labels — `pinctrl-rp1` (Pi 5), `pinctrl-bcm2711` (Pi 4), `pinctrl-bcm2835` (Pi 1–3 and the Zero), then `gpiochip0` — tried in order and resolved the first time a pin is enabled, which is what makes one binary work on any of them. Naming one skips that search, and is worth it where the board is known and fixed. It applies to the panel pins `moses_display` drives through bitters only: the daemons' `rpi:<n>` pins are looked up on their own (see [programs.md](programs.md#common-options)). |
| `WITH_LOG`          | Enable log messages on stderr                               |

The three resulting executables (`moses_watermeter`, `moses_breaker`,
`moses_sensors`) are produced under `bin/`, joined by `moses_display`
when `WITH_DISPLAY` is on. Their command-line options are documented
in [programs.md](programs.md), and what they publish in
[interfaces.md](interfaces.md).

Flavours
--------

`FLAVOUR` names the machine being built for, so a set of knobs does not
have to be spelled out one by one:

~~~sh
make build                      # device:  the Pi, daemons and panel both
make build FLAVOUR=sensors      # sensors: the same Pi, no panel
make build FLAVOUR=viewer       # viewer:  a window, anywhere else
~~~

Each row is a knob you can also set on its own; the flavour only decides
what it defaults to.

| knob                | `device`            | `sensors`           | `viewer` |
|---------------------|---------------------|---------------------|----------|
| `WITH_MQTT`         | yes                 | yes                 | yes      |
| `WITH_LINEPROTOCOL` | yes                 | yes                 | no       |
| `WITH_DBUS`         | yes                 | yes                 | no       |
| `WITH_WATERMETER`   | yes                 | yes                 | no       |
| `WITH_BREAKER`      | yes                 | yes                 | no       |
| `WITH_TEMPERATURE`  | yes                 | yes                 | no       |
| `WITH_DISPLAY`      | yes                 | no                  | yes      |
| `DISPLAY_BACKEND`   | automation-hat-mini | automation-hat-mini | sdl      |

`device` is the default: the Raspberry Pi the hardware is on, fully
equipped. **It includes the front panel, and so the whole of
`3rd/lvgl`** — some seven hours on a Pi Zero, which makes a bare `make
build` there an overnight job. `sensors` is the same machine without the
panel: the three daemons, and none of that compile. Reach for it
whenever the readings are what you are after.

`viewer` builds `moses_display` alone against SDL — no daemons, so
neither the GPIO and I2C hardware they drive nor M-Bus need exist on
that machine, and it watches the same broker from wherever it runs.
mosquitto does still have to be there: the broker is what it reads.

The three sinks go with the machine too, which is why they are in the
table rather than left as plain build defaults. Where the daemons run —
`device` and `sensors` — all three are on: the broker for everything off
the box, the [system bus](interfaces.md#the-system-bus-beside-mqtt) for a panel
beside it, and stdout for whatever is collecting lines. They are not
alternatives and cost nothing to have together: one formatter feeds each
of them, and a bus nobody is listening on is the normal state, not a
fault. That is also why `sensors` keeps `WITH_DBUS` on although it
builds no display — a consumer can be attached later without a rebuild,
and finds the last readings waiting for it. The viewer produces no
readings at all, so it has only the broker, which is where it reads them
from.

A flavour only supplies *defaults*: any knob set on the command line
still wins, so `make build FLAVOUR=viewer WITH_LOG=yes` is both. `make
flavours` prints the table above with the current values.


Install
-------

`make install` copies what the configuration built into `$(PREFIX)/bin`
(`/usr/local` by default; `DESTDIR` stages it for packaging), and
`make uninstall` takes it away again:

~~~sh
make build FLAVOUR=sensors
sudo make install FLAVOUR=sensors
~~~

With `WITH_DBUS`, the bus's policy file goes where `dbus-daemon` reads
it, or the daemons cannot own their names (see [the system
bus](interfaces.md#the-system-bus-beside-mqtt)):

~~~sh
sudo install -m 0644 dbus/moses.conf /etc/dbus-1/system.d/
~~~

The helper scripts under `scripts/` (`loop-runner`, `nut-notify`) are
optional; run them straight from the repository or copy them wherever
suits your setup — they need no particular installation step. Both read
the MQTT settings from the environment, `MOSES_MQTT_*` before the bare
names, exactly as the daemons do. `nut-notify` additionally falls back to
a YAML config — `$MOSES_CONFIG`, or `$HOME/.config/moses.yaml`, or
`/root/.config/moses.yaml` when the environment has no `HOME`, which is
the usual state of a `NOTIFYCMD` run by `upsmon`.

The daemons are meant to run continuously; supervise each one with
[`loop-runner`](programs.md#supervision) so it is restarted on failure (and a crash
is reported over MQTT).

How to run them, and the environment they read, is in
[programs.md](programs.md).
