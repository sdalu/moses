Programs
========

What each program takes on its command line and from the environment,
and how to keep them running. What they publish is in
[interfaces.md](interfaces.md).


Common options
--------------

| Option                    | Description                                                |
| ------------------------- | ---------------------------------------------------------- |
| `-r`, `--reduced-latency` | `SCHED_FIFO` and locked memory, so no pulse or valve waits |
| `-h`, `--help`            | Program-specific usage                                     |

Durations (`SEC`) are seconds, or take a unit: `s`, `min`, `h`, `d`, `w`
(`1min`).


GPIO pins
---------

Pins are given as `chip:pin`, for instance `gpiochip0:16`, or as
`rpi:<n>` for physical header pin `n`. `rpi:` finds the header's
controller by its label — `pinctrl-rp1` (Pi 5), `pinctrl-bcm2711`
(Pi 4), `pinctrl-bcm2835` (Pi 1–3, Zero) — and is an error anywhere
else; on a Pi Zero `rpi:36` is `gpiochip0:16`. The `RPI_GPIO_CHIP` build
knob does not affect it.


`moses_watermeter`
------------------

| Option                     | Description                                        |
| -------------------------- | -------------------------------------------------- |
| `-d`, `--device=DEV`       | M-Bus serial device (default `/dev/ttyAMA0`)       |
| `-b`, `--baudrate=N`       | M-Bus baud rate, 300 … 38400 (default 2400)        |
| `-a`, `--address=ADDR`     | M-Bus primary or secondary address (default `1`)   |
| `-i`, `--interval=SEC`     | Index polling and reporting interval (default 60s) |
| `-P`, `--pin=CTRL:PIN`     | GPIO line for pulse counting                       |
| `-L`, `--pin-label=STR`    | GPIO consumer label                                |
| `-D`, `--debounce=USEC`    | GPIO hardware debounce time                        |
| `-B`, `--bias=...`         | `as-is`, `disabled`, `pull-up`, `pull-down`        |
| `-E`, `--edge=...`         | Counted edge: `rising` (default) or `falling`      |
| `-I`, `--idle-timeout=SEC` | Publish a `0` pulse if nothing is seen within SEC  |

The M-Bus index and the pulse counter are independent; `-P` turns the
counter on. For the HRI on the Automation HAT's `IN2`, once rewired as
[Pulse counting](hardware.md#pulse-counting) says:
`-P rpi:38 -B disabled -E rising`.


`moses_breaker`
---------------

| Option                     | Description                                       |
| -------------------------- | ------------------------------------------------- |
| `-P`, `--pin=CTRL:PIN`     | GPIO line driving the relay (**required**)        |
| `-L`, `--pin-label=STR`    | GPIO consumer label                               |
| `-M`, `--mode=...`         | `as-is`, `push-pull`, `open-drain`, `open-source` |
| `-A`, `--active=...`       | Active level: `low` or `high`                     |
| `-I`, `--idle-timeout=SEC` | Re-publish the current state every SEC            |

It is commanded over MQTT (`state/set`) or, without a broker, over the
system bus: [Shutting the water](interfaces.md#shutting-the-water).


`moses_sensors`
---------------

| Option                 | Description                                           |
| ---------------------- | ----------------------------------------------------- |
| `-i`, `--interval=SEC` | Publishing interval (default 60s)                     |
| `-a`, `--altitude=M`   | Report sea-level pressure for an altitude of M metres |


`moses_display`
---------------

A read-only front panel on the Automation HAT Mini's 160x80 LCD (see
[LCD](hardware.md#lcd)), or in a window with the `sdl` backend. It reads
what the daemons publish, from the broker or, with `WITH_DBUS`, from
the [system bus](interfaces.md#the-system-bus) (`--source`), and draws:

~~~text
+--------------------------------------+
| moses           21.4°C        13:40  |
|--------------------------------------|
| (o)  213025 L                  [+3]  |
|                                      |
| +-----------------+ +--------------+ |
| | VALVE           | | UPS OB       | |
| | OPEN            | | 42% ~30m     | |
| +-----------------+ +--------------+ |
+--------------------------------------+
~~~

That is what `test/ref-imgs/dashboard-flow.png` holds. The name at the
top left is the last segment of `MQTT_TOPIC_PREFIX` (`moses`, out of
`water-breaker/moses`), not the hostname of whatever machine is showing
it. With `--source=local` every figure is this machine's, so there the
hostname's short name is used when `MQTT_TOPIC_PREFIX` is not set; when
it is, its last segment still names the panel, and a start-up line says
so if it is not the hostname. A flow of `+N` stays up for 30 seconds after a pulse report.

| Colour   | Meaning                                                           |
| -------- | ----------------------------------------------------------------- |
| its own  | a current reading: the meter cyan, an open valve green            |
| dim grey | unknown: the daemon is `offline`, or silent for `--stale` seconds |
| red      | known and bad: the valve shut, the room at freezing, on battery   |

| Option                 | Description                                              |
| ---------------------- | -------------------------------------------------------- |
| `-i`, `--interval=SEC` | Poll `upsd` every SEC (default 10s)                      |
| `-s`, `--stale=SEC`    | Grey out a reading older than SEC (default 150s)         |
| `-u`, `--ups[=NAME]`   | Poll `upsd` on this machine; without NAME, its first UPS |
| `-S`, `--source=WHERE` | `mqtt`, `local`, or `auto` (default)                     |
| `-c`, `--check`        | Bring the panel up, report, and exit                     |

`--check` is for a new or rewired machine: it makes the same checks a
normal start does -- the data/command pin, the SPI device, spidev's
buffer against the largest transfer -- and exits 0 once the panel is up,
without drawing. [`make check-spi`](hardware.md#lcd) asks the boot
configuration the same questions without a built binary.

**The source** is one of two, never both:

* *`local`*: nothing off this machine. The readings from the system bus,
  the UPS from `upsd`, and no broker at all, so the panel keeps working
  while the network is down. Needs `WITH_DBUS`.
* *`mqtt`*: the broker, the way for a display on any other machine. The
  UPS as the next section says.
* *`auto`* picks `local` when the display may own the bus name
  `moses.display`, and `mqtt` otherwise -- or when there is no system bus
  at all. [`dbus/moses.conf`](interfaces.md#the-system-bus) lets root own
  it, and the bus has no call that reports its policy, so asking for the
  name is how the display finds out the policy is loaded. It goes by
  that rather than by whether a daemon is on the bus right now, because
  at boot the panel may well start before the daemons do.

With `local` chosen by hand, a refused name is logged and the bus read
regardless. Another `moses_display` already owning the name stops this
one, whichever source was asked for.

It never touches the relay, and has no `--reduced-latency`: on a
single-core Pi that would compete with the program that shuts the water.

**The UPS**, with `mqtt`, comes one of two ways; `local` is always the
second:

* *Without `--ups`*, from the events [`nut-notify`](system.md#nut)
  publishes on MQTT: on line or on battery, no charge and no time. It
  reads `--` until the first event. This is the way for a display that
  is not on the UPS's machine.
* *With `--ups`*, from `upsd` on `localhost:3493`, which adds the charge
  and, on battery, the remaining time — from `battery.runtime`, or
  worked out from charge, capacity and current and marked `~` (the
  `pijuice` driver), in which case read it as an order of magnitude.
  The MQTT events still repaint at once on a change.

`OL`, `HB`, `CHRG` and `DISCHRG` are left out of the status shown;
every other flag (`OB`, `LB`, `RB`, `ALARM`, …) goes on the chip's
label line, the figures on its value line.


Supervision
-----------

The daemons are not expected to exit.

Each runs once per machine. A second copy of a daemon exits with
status 2 before touching anything, naming the one already running:
*"moses_watermeter: already running as pid 10838"*. Each holds a lock,
`/var/run/moses/<name>.pid` (`/run/moses` on Linux) locked with
`flock()`, which the kernel drops however the process ends, so a crash
leaves nothing to clean up; a copy renamed on disk takes the same lock.
Another process with the same name is refused too, for copies built
before the lock, and that is all that is checked when the lock cannot
be made -- running as a user who cannot write `/var/run`. For `moses_display`
the check is the panel backend's, made before any pin is claimed, so
`--check` next to a running panel says so rather than blaming the
wiring. The SDL backend has no such check; there a second display is
stopped only when the first one owns `moses.display` -- it reads the bus
and the policy lets it take the name. With `--source=mqtt`, without
`WITH_DBUS`, with no system bus, or with the name refused, nothing
stops a second window.
Under `loop-runner` a refused copy comes back every `-s` seconds with a
`crash` each time.

[`loop-runner`](../scripts/loop-runner) restarts a command when it stops
and publishes a `crash` on the `error` topic. It reads the same
environment as the daemons.

~~~sh
loop-runner -t watermeter -s 5 -- moses_watermeter -r -i 1min
~~~

| Option | Description                                   |
| ------ | --------------------------------------------- |
| `-t`   | Source name reported in the crash message     |
| `-s`   | Seconds to wait before restarting (default 5) |
| `-v`   | Verbose (shell tracing)                       |


Environment
-----------

| Variable            | Description                                 |
| ------------------- | ------------------------------------------- |
| `MQTT_HOST`         | Broker hostname or address (**required**)   |
| `MQTT_PORT`         | Port (default 1883)                         |
| `MQTT_USERNAME`     | Username                                    |
| `MQTT_PASSWORD`     | Password                                    |
| `MQTT_CLIENT_ID`    | Client identifier                           |
| `MQTT_TOPIC_PREFIX` | Topic prefix (default: the compiled-in one) |

Each is looked for as `MOSES_MQTT_…` first, so moses can be pointed at
its own broker on a machine whose other clients use the bare names.
`MQTT_USERNAME` and `MQTT_PASSWORD` are unset, under both spellings,
once read.
