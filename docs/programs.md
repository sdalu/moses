Programs
========

What each program takes on its command line and from the environment,
and how to keep them running. What they publish is in
[interfaces.md](interfaces.md); how to build them is in
[building.md](building.md).

Moses is split into independent daemons that share a small support
library (`src/common.c`). Each one is configured from the command line,
reads its MQTT credentials from the environment, and publishes/subscribes
under a common topic prefix (`MQTT_TOPIC_PREFIX`, default
`water-breaker`).

The valve is *normally open*: `moses_breaker` only energises the relay
to close the water, so a power loss or a crash leaves the supply open.
The [`loop-runner`](#supervision) wrapper restarts a daemon if it dies
and reports the crash over MQTT.


Common options
--------------

These apply to every daemon:

| Option                  | Description                                                  |
|-------------------------|--------------------------------------------------------------|
| `-r`, `--reduced-latency` | Switch to the `SCHED_FIFO` real-time scheduler and lock memory (avoids missing pulses / delaying the valve). Usually needs root. |
| `-h`, `--help`          | Show the program-specific usage.                             |

GPIO pins are given as `chip:pin`, for instance `gpiochip0:16`. As a
convenience `rpi:<n>` names physical header pin `n`: the header's GPIO
controller is found by its label — `pinctrl-rp1` on a Pi 5,
`pinctrl-bcm2711` on a Pi 4, `pinctrl-bcm2835` on a Pi 1–3 and the Zero
— and the pin is mapped to its line on that controller. So `rpi:36` is
line 16, which on a Pi Zero is `gpiochip0:16`. On a machine with none of
those controllers, `rpi:` is an error.

This lookup belongs to the daemons. The `RPI_GPIO_CHIP` build option
does not change it; that one pins the controller only for the panel
pins `moses_display` drives through bitters (see
[building.md](building.md#build-with-cmake)).


`moses_watermeter`
------------------

| Option                  | Description                                          |
|-------------------------|------------------------------------------------------|
| `-d`, `--device=DEV`    | M-Bus serial device (default `/dev/ttyAMA0`)         |
| `-b`, `--baudrate=N`    | M-Bus baud rate (300 … 38400, default 2400)          |
| `-a`, `--address=ADDR`  | M-Bus primary or secondary address (default `1`)     |
| `-i`, `--interval=SEC`  | Index polling/reporting interval (default 60s)       |
| `-P`, `--pin=CTRL:PIN`  | GPIO line for pulse counting                         |
| `-L`, `--pin-label=STR` | GPIO consumer label                                  |
| `-D`, `--debounce=USEC` | GPIO hardware debounce time                          |
| `-B`, `--bias=...`      | GPIO bias: `as-is`, `disabled`, `pull-up`, `pull-down` |
| `-E`, `--edge=...`      | Counted edge: `rising` (default) or `falling`        |
| `-I`, `--idle-timeout=SEC` | Publish a `0` pulse if nothing is seen within SEC |

The M-Bus reader and the pulse counter are independent: provide `-d`
(and/or rely on its default) to enable index reading, and `-P` to enable
pulse counting. Either can be left out.

To find the meter on the bus (and the address to pass to `-a`), scan it with
the `mbus-serial-scan` tool shipped with libmbus:

~~~sh
/opt/libmbus/bin/mbus-serial-scan -b 2400 /dev/ttyAMA0
~~~



`moses_breaker`
---------------

Without a broker, the valve can still be shut from the Pi itself over
the system bus, as root: see
[From the command line](interfaces.md#from-the-command-line).

| Option                  | Description                                          |
|-------------------------|------------------------------------------------------|
| `-P`, `--pin=CTRL:PIN`  | GPIO line driving the relay (**required**)           |
| `-L`, `--pin-label=STR` | GPIO consumer label                                  |
| `-M`, `--mode=...`      | Output mode: `as-is`, `push-pull`, `open-drain`, `open-source` |
| `-A`, `--active=...`    | Active level: `low` or `high`                        |
| `-I`, `--idle-timeout=SEC` | Re-publish the current state every SEC (heartbeat) |



`moses_sensors`
---------------

| Option                  | Description                                          |
|-------------------------|------------------------------------------------------|
| `-i`, `--interval=SEC`  | Publishing interval (default 60s)                    |
| `-a`, `--altitude=M`    | Convert the reading to sea-level pressure for altitude M (meters) |

`moses_display`
---------------

A read-only front panel on the 0.96" 160x80 LCD the Automation HAT Mini
carries (see [LCD](hardware.md#lcd)). It subscribes to what the other daemons
publish, asks `upsd` about the battery, optionally reads the same
readings off the [system bus](interfaces.md#the-system-bus-beside-mqtt), and draws:

~~~text
+--------------------------------------+
| moses            21.4°        13:40  |
|--------------------------------------|
| (o)  213025 L                  [+3]  |
|                                      |
| +-----------------+ +--------------+ |
| | VALVE           | | UPS OB       | |
| | OPEN            | | 42% ~30m     | |
| +-----------------+ +--------------+ |
+--------------------------------------+
~~~

That is not a sketch: it is what `test/ref-imgs/dashboard-flow.png`
contains, rendered by the
[screenshot tests](../test/README.md#screenshot-tests).

The name in the top left is the **last segment of `MQTT_TOPIC_PREFIX`**
— `moses`, out of `water-breaker/moses`. That prefix is already how a
broker carrying several installations tells them apart, so the panel
reads the name from it rather than having it configured a second time
and kept in step by hand.

It is deliberately *not* the hostname. Over MQTT the machine running
the display is whichever one someone opened a window on, which is not
what the screen is about — the first SDL build sat on a workstation
reporting `hyperion` above moses's water meter. The hostname becomes
the right answer again only for a source that is local by construction,
which is what the [system bus](interfaces.md#the-system-bus-beside-mqtt) is.

It is dark on purpose: the panel sits in a technical room where a white
screen at full backlight is a lamp, black is the one thing an LCD
renders perfectly, and it leaves colour free to mean something.

### Colour

Colour carries the state, and the rule is three-valued:

| colour        | meaning                                                 |
|---------------|---------------------------------------------------------|
| its own       | a current reading — the meter cyan, an open valve green, the rest plain |
| dim grey      | nothing known, or nothing heard for `--stale` seconds — the figure shown may no longer be true |
| red           | known and bad: the valve shut, the room at freezing, the UPS on battery |

Keeping dim and red apart is the point. Not knowing whether the valve
is open is a different thing from knowing it is shut, and a panel that
painted both red would cry wolf every time the broker hiccuped. A figure
goes grey either because its daemon's retained `availability` says
`offline` or because nothing has arrived in `--stale` seconds — the
second catches a daemon wedged with its MQTT connection still open.

The flow marker is latched: a `pulse` report that counted anything shows
`+N` for 30 seconds, so a pulse is still visible to someone who glances
at the panel a moment later.

### Options

| Option                  | Description                                          |
|-------------------------|------------------------------------------------------|
| `-i`, `--interval=SEC`  | Poll `upsd` every SEC (default 10s)                  |
| `-s`, `--stale=SEC`     | Grey out a reading older than SEC (default 150s, i.e. two and a half missed one-minute reports) |
| `-u`, `--ups[=NAME]`    | Poll `upsd` **on this machine**, which is the only way to a charge and a remaining time. With no NAME, the first UPS `upsd` lists. Omit `-u` entirely and the UPS state comes from MQTT instead (see below). |

It takes only the panel's three pins. The relay on that same HAT is the
solenoid valve and belongs to `moses_breaker`; this program never goes
near it. There is deliberately no `--reduced-latency`: a screen refresh
has no deadline to miss, and putting one on `SCHED_FIFO` on a
single-core Pi would be competing with the program that shuts the water
off.

### The UPS

**The UPS has two ways in, and `--ups` picks between them.**

*Without `--ups`* — `upsd` is not touched at all, and the UPS state comes
from the MQTT events [`nut-notify`](system.md#upsmon) publishes on
`ups/+/notify/<TYPE>`. Those name a transition rather than a state, so
they are translated back to NUT's own status flags (`ONBATT` → `OB`,
`LOWBATT` → `OB LB`, `NOCOMM` → lost) and the panel says what the UPS is
doing — `on line`, `on batt` — with no charge and no remaining time,
because the events carry none. This is the right way round for a display
that is **not** on the machine the UPS is attached to: `localhost:3493`
has no `upsd` to ask there, and the broker is the only thing that knows
the power went.

Until the first event arrives the UPS reads `--`, not `none`. upsmon
sends one when something changes and nothing in between, so a display
that has just started legitimately knows nothing yet — and saying "there
is no UPS" would be a claim rather than the absence of one.

*With `--ups`* — `upsd` on `localhost:3493` is polled in NUT's own line
protocol every `--interval` seconds, which is the only way to a charge
and a remaining time. `--ups=NAME` picks a UPS; a bare `--ups` takes the
first one `upsd` lists. The events are still subscribed to — for the UPS
that was named or found, so a second one on the broker cannot write over
it — so going onto battery repaints at once rather than up to one poll
later.

Remaining time is shown only while actually running on the battery, and
comes from `battery.runtime` when the driver reports it. The `pijuice`
driver does not, so it is worked out from `battery.charge`,
`battery.capacity` and `battery.current` and marked with a leading `~`.
That is a division, not a measurement: on a 0.6 Ah pack the
instantaneous current moves with whatever the Pi is doing, so read it as
an order of magnitude.

`ups.status` is condensed before it is shown: `OL`, `HB`, `CHRG` and
`DISCHRG` are dropped — the first is the routine state and a white
reading already says the power is fine, the second is the battery
merely being full, and the last two repeat what `OL` and `OB` have
said. Everything else passes through, so `RB`, `ALARM` and any flag NUT
adds later still reach the panel.

What survives goes on the chip's **label** line (`UPS OB LB RB`) while
the figures go on its **value** line (`100% ~1h25`), so a long status
and a long runtime never compete for the same line — on one line, the
runtime was the half that fell off the end, which is the half you want
when the power has gone.


Supervision
-----------

The daemons are meant to run forever and are not expected to exit. The
[`loop-runner`](../scripts/loop-runner) helper restarts a command when it stops and
publishes a `crash` message on the `error` topic:

~~~sh
loop-runner -t watermeter -s 5 -- moses_watermeter -r -i 1min ...
~~~

| Option | Description                                         |
|--------|-----------------------------------------------------|
| `-t`   | Source name reported in the crash message           |
| `-s`   | Seconds to wait before restarting (default 5)       |
| `-v`   | Verbose (shell tracing)                             |

It reads the same `MQTT_HOST`, `MQTT_PORT`, `MQTT_USERNAME`,
`MQTT_PASSWORD` and `MQTT_TOPIC_PREFIX` environment variables (each
`MOSES_MQTT_…` first, see [Environment](#environment)) as the
daemons.


Environment
-----------

If using MQTT the following environment variables must be defined:

| Environment variable | Required | Comment                    |
|----------------------|:--------:|----------------------------|
| `MQTT_HOST`          |    ✓     | Hostname or IP address     |
| `MQTT_PORT`          |          | Port number (default 1883) |
| `MQTT_USERNAME`      |          | Username                   |
| `MQTT_PASSWORD`      |          | Password                   |
| `MQTT_CLIENT_ID`     |          | Client identifier          |
| `MQTT_TOPIC_PREFIX`  |          | Adjust topic               |

**Each of those is looked for as `MOSES_MQTT_…` first**, and only then
under the bare name — `MOSES_MQTT_HOST` before `MQTT_HOST`, and so on.
These are generic names, which is both why they were chosen and why they
are not quite enough: a machine carrying more than one MQTT client has
one environment between them, and pointing moses at a different broker
should not mean moving everything else on the box. The bare names keep
working, so nothing configured before has to change, and the prefixed one
wins when both are set, because whoever set it meant it for moses.

`MQTT_USERNAME` and `MQTT_PASSWORD` are read once at start-up and then
unset — under both spellings — so they do not linger in the process
environment where anything the daemon spawns would inherit them.


Example
-------

The three daemons, each of which would normally run under
[`loop-runner`](#supervision):

~~~sh
export MQTT_HOST=127.0.0.1
export MQTT_TOPIC_PREFIX=water-breaker/moses
moses_watermeter -r -i 1min -I 1min -P rpi:38 -B pull-up -E rising
moses_breaker    -r -P rpi:36 -I 1min -M open-source
moses_sensors    -r -i 1min
~~~

The `-P rpi:38 -B pull-up` above is how pulse counting is deployed, and
as wired it counts nothing: [Pulse counting](hardware.md#pulse-counting)
has why, and the `-B disabled` it wants instead.
