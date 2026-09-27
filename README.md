Moses
=====

Moses is a do-it-yourself water-leak breaker built around a Raspberry
Pi. It watches household water usage and can shut the main supply with a
solenoid valve, so a leak does not turn into water damage. Everything is
driven over [MQTT](https://mqtt.org/), so it integrates with existing
home-automation setups (Home Assistant, Node-RED, …) without locking you
into a proprietary cloud.

It is made of small C programs, each doing one job and talking to the
MQTT broker:

| Program            | Role                                                            |
|--------------------|-----------------------------------------------------------------|
| `moses_watermeter` | Read the water meter (M-Bus index and/or GPIO pulse counting)   |
| `moses_breaker`    | Open/close the solenoid valve through a relay                   |
| `moses_sensors`    | Read the optional BME280 (temperature, pressure, humidity)      |
| `moses_display`    | Show the installation on the HAT's LCD (optional, see [Display](#moses_display)) |

The first three are the installation; `moses_display` only watches them
and is not built by default.

See [Software](#software) for the architecture and the MQTT interface,
and [Build and installation](#build-and-installation) to compile and run
them. The bulk of this document covers the hardware and the host system
configuration.


Hardware
========

Dealing with water and the purpose being to avoid water damage, not
to make one, quality valve 
(~ 250€) and watermeter (~ 100€) have been selected, which is expensive.
But you should be able to swap them with other devices.

The result will be more costly than a [Hydrelis
Stop-Flow](https://www.hydrelis.fr/stop-flow.php) or a [Grohe Sense
Guard](https://www.grohe.fr/fr_fr/smarthome/grohe-sense-guard/), but
you won't be locked into proprietary systems.

* The valve will be 24VDC powered (don't want to play with high
  voltage) and work as normally open (NO). This means it will only be
  powered when it needs emergency stop water (which hopefully should
  never be), so there will be less heat, current consumption, and
  solenoid stress.
* The water meter will be certified MID R400 and U0D0 for accuracy and
  easy installation. It will communicate with M-Bus or perform pulse
  counting.
* The valve and the water meter will be managed by a Raspberry Pi
  (though a microcontroller would be more interesting).
* Optional backup with a battery. It is beneficial if doing a pulse
  counting, as we don't want to miss pulses due to Raspberry PI being
  off-line.

## Shopping list
1. [Raspberry PI Zero WH](https://thepihut.com/products/raspberry-pi-zero-wh-with-pre-soldered-header)
1. [Hacker hat](https://thepihut.com/products/hat-hacker-hat)
2. [M-bus master hat (micro version)](https://www.packom.net/product/m-bus-master-hat/)
3. [Automation hat mini](https://thepihut.com/products/automation-hat-mini)
4. [PiJuice for PiZero](https://www.kubii.com/fr/poe-hat-cartes-d-extensions/2795-pijuice-pour-pi-zero-0616909468508.html) + [Battery 600mA](https://www.kubii.com/fr/batteries-piles/2818-1510-batterie-pijuice-3272496311428.html) (optional)
5. [spacer](https://www.amazon.fr/dp/B093FNWP39)
6. [Bürkert (type 6281): Solenoid valve for drinking water, brass, G3/4", NO, 24VDC](https://tameson.fr/products/electrovanne-d-eau-potable-g3-4-en-laiton-no-24vdc-6281-256576-256576) + [Connector](https://tameson.fr/products/connecteur-avec-led-din-a-as-cal-tameson-as-cal) 
7. [Sensus 620 watermeter](https://www.compteur-energie.com/compteurs-eau-froide-sensus-compteur-eau-620.htm) + [HRI B4/D1/8L](https://www.compteur-energie.com/eau-emetteur-impulsions-sensus-hri-b4-amrab152-amrab162.htm) (M-Bus: 2400 baud max)
8. [24VDC power supply (MeanWell LPV-35-24)](https://www.amazon.fr/gp/product/B00ID6L04S) + [5VDC stepdown buck regulator (Bauer Electronics, DC DC 8V-32V to 5V)](https://www.amazon.fr/gp/product/B09B7XZYJQ)
9. [Pimoroni BME280 Breakout](https://shop.pimoroni.com/products/bme280-breakout?variant=29420960677971) (optional)

It is also possible to replace the _Automation hat mini_ with a [Relay 4 zero](https://thepihut.com/products/relay-4-zero-4-channel-relay-board-for-pi-zero)


Hardware configuration
======================

PiJuice
-------


The PiJuice comes with an RTC (Real Time Clock), which the Raspberry
PI was missing until version 5.

To use the RTC, the `/boot/config.txt` file must be edited to place
the following line, enabling the DS1307 component in the Linux kernel.

~~~
dtoverlay=i2c-rtc,ds1307,addr=0x68
~~~

Correct activation can be check by running the `hwclock` command.


M-Bus Master hat
---------------

### M-Bus power enabling 

There is a minor conflict with the *Automation Hat mini*, as they both
share the `GPIO 26` (`PIN 37`), it's possible to change the M-Bus power
pin, by unsoldering `R19` on the top side, near the green LED, and
soldering it back on the pin selector on the back side. But we will
consider, we won't use the input `IN1` of the *automation hat mini*, and
keep going with `GPIO 26`.

It will be configured as output and driving high. This will ensure the
M-Bus is powered, which allows the water meter reader (HRI) to be
powered by the bus instead of draining its lithium battery

Add in `/boot/config.txt`:
~~~
gpio=26=op,dh
~~~

### UART


The *mBus Master Hat* relies on the Raspberry PI UART, which can
conflict with the Bluetooth device and the serial console, so they
must be de-activated.

In `/boot/config.txt`
~~~
dtoverlay=miniuart-bt  # Either use the mini uart, crippling bluetooth
dtoverlay=disable-bt   #     Or disable bluetooth

dtoverlay=uart0-pi5    # For RPI 5 only
~~~

The use of `ttyAMA0` (as `serial0`) should be disabled; this is done
in `/boot/cmdline.txt` by removing/changing the parameter

~~~
console=serial0,115200
~~~

Eventually you also need to run
~~~
systemctl disable hciuart.service
systemctl disable bluealsa.service
systemctl disable bluetooth.service
systemctl stop    serial-getty@ttyAMA0.service
systemctl disable serial-getty@ttyAMA0.service
systemctl mask    serial-getty@ttyAMA0.service
~~~

Failing to address this problem will lead to messages such as `Failed
to receive M-Bus response frame.` when using the `mbus-serial-*`
programs.




Automation Hat mini
-------------------

We only need the relay to drive the solenoid valve, but it also comes
with a small LCD, that can be used to display informations.

### LCD 

| Device  | Pin                           | Interface                   |
|---------| ------------------------------|-----------------------------|
| Relay 1 | `GPIO16/PIN36`                |                             |
| LCD     | `GPIO9/PIN21`, `GPIO25/PIN22` | `SPI0` + `CS`=`GPIO7/PIN26` |

Pin configuration is done in the `/boot/config.txt` file:
~~~
gpio=9=op,dl
gpio=25=op,dl
~~~

We will also change some kernel parameters in `/boot/cmline.txt`, as
the SPI buffer size is only 1 page (4096 bytes) by default.  This will
avoid us having to break some of the SPI transfers into multiple
chunks.

~~~
spidev.bufsiz=65536
~~~

### Relay

The relay is controlled by the `GPIO 16`, the GPIO will be configured
as output and driving low by default, keeping the valve open. 
Configuration is done in `/boot/config.txt`.

~~~
gpio=16=op,dl
~~~

The 24V need to be connect to the `COM` port, and the solenoid red wire to the
`NO` (Normaly Open) port.


### Pulse counting

We will use the `IN2` port (as previously seen, `IN1` port conflicts
with the *mBus Master Hat*) to do some pulse counting (HRI white
cable).

~~~
gpio=20=ip,pu
~~~

**As wired above this does not work: no pulse ever reaches `GPIO 20`.**
Measured on 2026-09-26, with `moses_watermeter` running as deployed
(`-P rpi:38 -B pull-up -E rising -I 1min`):

| Check                                       | Result                            |
|---------------------------------------------|-----------------------------------|
| Line request (`gpioinfo`)                   | `input bias=pull-up edges=rising` |
| Edge IRQs (`/proc/interrupts`), 13 days     | `1`                               |
| 12 litres drawn, index `213011` -> `213023` | `0` pulses counted                |
| `GPIO 20` level, throughout                 | `1`                               |

That single edge is most likely an artifact rather than a pulse: the
pull changes in the measurement under (2) below glitched the edge
detector the same way, twice in a row. The true count over those 13
days is almost certainly zero.

The M-Bus index followed the flow litre by litre while the pulse
counter stayed at zero, so the meter, the HRI and the software are all
sound -- the signal never arrives at the pin. There are two
independent causes, both electrical:

1. *The HRI output cannot drive an Automation HAT input.* The HRI-B
   pulse output is an open-drain transistor (max 24V / 20mA, 124ms
   fixed pulse width): it only pulls to ground, and sources no
   voltage. The *Automation hat mini* inputs are active-high voltage
   sensors -- an 820k/120k divider scaling 0-25.85V down to 0-3.3V,
   switching on at 3V and off at 1V. With nothing pulling the white
   wire up, `IN2` never reaches its 3V threshold. The Sensus wiring
   diagram for a PLC input shows a pull-up resistor to +24V on the
   white wire; it is missing here.

2. *The internal pull-up masks the input.* The `ip,pu` above (and
   `-B pull-up`) enable the SoC's internal pull-up, roughly 50k, on a
   pin the HAT already drives through its divider. Against the
   divider's 120k leg to ground that holds `GPIO 20` high whatever the
   HAT does. Measured: with `raspi-gpio set 20 pn` the level drops to
   0 and stays there, and returns to 1 when the pull-up is restored.

To make it work:

* Pull the HRI white wire up to +24V through about 10k, and connect
  brown to the system ground. The 1k of the Sensus diagram would draw
  24mA, above the HRI's 20mA limit; 10k draws 2.4mA.
* Drop `gpio=20=ip,pu`, and pass `-B disabled` rather than
  `-B pull-up`, so the HAT drives the line unopposed.
* Keep `-E rising`: idle then sits at 24V, a pulse pulls low for
  124ms, and its release is the counted edge -- one pulse per litre
  with the `D1` divisor.

Note that Sensus documents the pulse output and the M-Bus data
interface as *alternatives* ("parallel usage of serial output and
pulse output is not recommended and can cause problems") and asks for
a potential-free connection when both are used; the HAT input is not
isolated, so an opto-coupler belongs between the white wire and `IN2`.

The M-Bus index already has one-litre resolution, so pulse counting
only buys latency. Polling the index more often (`-i`) is the
alternative that needs no hardware change: the HRI is bus-powered (see
*M-Bus power enabling*) and Sensus place no limit on M-Bus read
frequency.


System configuration
====================

Watchdog
--------

~~~sh
apt install watchdog
~~~


WiFi
----

For stable low-latency WiFi power management must be disabled
~~~sh
iwconfig wlan0 power off
~~~

If using `/etc/network/interfaces` for the configuration:
~~~
auto wlan0
iface wlan0 inet dhcp
    pre-up ifconfig wlan0 hw ether aa:bb:cc:dd:ee:ff
    pre-up iwconfig wlan0 power off
    wpa-conf /etc/wpa_supplicant/wpa_supplicant.conf
~~~

Nut
---

PiJuice is supported by the [NUT (Network UPS
Tools)](https://networkupstools.org/), we will configure it to send
notifications using MQTT.

In the following configuration fragments, `__ups_name__` and
`__password__` need to be replaced with appropriate values.

~~~sh
apt install nut
~~~


### nut

Configure to run in `standalone` mode be editing the `nut.conf` file:

~~~conf
MODE=standalone
~~~


### ups

The `ups.conf` contains the list of available UPS devices, here we
only have the PiJuice:

~~~conf
[__ups_name__]
driver = pijuice
port   = /dev/i2c-1
desc   = "PiJuice"
~~~


### upsd

UPS daemon listening for requests
* user and autorisations are configured in `upsd.users`
~~~conf
[upsmon]
        password = __password__
        upsmon primary
~~~

* will only listen on the loopback interface
`upsd.conf`
~~~
LISTEN 127.0.0.1 3493
~~~


### upsmon

Monitoring is defined in `upsmon.conf`, a custom notification hook is
declared using `NOTIFYCMD` so UPS state is send using MQTT.

~~~conf
MONITOR __ups_name__ 1 upsmon __password__ primary
NOTIFYCMD nut-notify
~~~

A simple `nut-notify` script can be defined as follow:

~~~sh
#!/bin/sh

# -- Config ------------------------------------------------------------

MQTT_HOST="mqtt-host"
MQTT_USER="mqtt-user"
MQTT_PASSWD="mqtt-pasword"

# ---------------------------------------------------------------------- 
# $NOTIFYTYPE / $UPSNAME / $HOSTNAME

# Path to commands
MOSQUITTO_PUB=/usr/bin/mosquitto_pub

# Notify (credential flags are only passed when set)
${MOSQUITTO_PUB}                                \
    -h "${MQTT_HOST}"                           \
    ${MQTT_PORT:+-p "${MQTT_PORT}"}             \
    ${MQTT_USER:+-u "${MQTT_USER}"}             \
    ${MQTT_PASSWD:+-P "${MQTT_PASSWD}"}         \
    -t "ups/${UPSNAME}/notify/${NOTIFYTYPE}"    \
    -m "$1"
~~~


libmbus
-------

It is the [libmbus](https://github.com/rscada/libmbus) software which
will query the M-Bus through
`/opt/libmbus/bin/mbus-serial-request-data -b 300 /dev/ttyAMA0 1`. If
not already available as a package, it can be installed using:

~~~sh
git clone https://github.com/rscada/libmbus
cd libmbus
./build.sh
./configure --prefix=/opt/libmbus
make clean
make
make install
~~~

mosquitto
---------

[Mosquitto](https://mosquitto.org/) will provide the libraries to 
perform MQTT queries. Necessary pacakges are installed using:

~~~sh
apt install mosquitto-dev
~~~




Software
========

Moses is split into independent daemons that share a small support
library (`src/common.c`). Each one is configured from the command line,
reads its MQTT credentials from the environment, and publishes/subscribes
under a common topic prefix (`MQTT_TOPIC_PREFIX`, default
`water-breaker`).

~~~
                    +----------------------+
   M-Bus ---------> |                      | --> <prefix>/index
   pulse (GPIO) --> |  moses_watermeter    | --> <prefix>/pulse
                    +----------------------+ --> <prefix>/error
                                             --> <prefix>/availability/watermeter

   <prefix>/state/set --> +----------------+
                          | moses_breaker  | --> relay --> solenoid valve
   <prefix>/state     <-- +----------------+ --> <prefix>/error
                                             --> <prefix>/availability/breaker

                    +----------------------+
   BME280 (I2C) --> |  moses_sensors       | --> <prefix>/sensors
                    +----------------------+ --> <prefix>/error
                                             --> <prefix>/availability/sensors

   <prefix>/index      --> +----------------+
   <prefix>/pulse      --> |                |
   <prefix>/state      --> | moses_display  | --> LCD
   <prefix>/sensors    --> |                |
   <prefix>/availability/* +----------------+
                           ^        ^
   ups/+/notify/* ---------+        +----- upsd on localhost:3493
                                           (only with --ups)
~~~

Everything flows one way except the breaker: `moses_display` publishes
nothing and commands nothing.

The valve is *normally open*: `moses_breaker` only energises the relay
to close the water, so a power loss or a crash leaves the supply open.
The [`loop-runner`](#supervision) wrapper restarts a daemon if it dies
and reports the crash over MQTT.


MQTT topics
-----------

All topics are relative to `MQTT_TOPIC_PREFIX`.

| Topic         | Direction | Producer / Consumer | Payload                                              |
|---------------|-----------|---------------------|------------------------------------------------------|
| `index`       | publish   | `moses_watermeter`  | Meter index in litres, e.g. `213011.000`             |
| `pulse`       | publish   | `moses_watermeter`  | Number of pulses counted (`0` heartbeat on timeout)  |
| `state`       | publish   | `moses_breaker`     | Current valve state, `0` (open) or `1` (closed)      |
| `state/set`   | subscribe | `moses_breaker`     | Requested state: `0`/`1`, `off`/`on`, `false`/`true` |
| `sensors`     | publish   | `moses_sensors`     | JSON `{ "temperature", "pressure", "humidity" }`     |
| `error`       | publish   | all                 | JSON `{ "source", "type", "msg" }`                   |
| `availability/<daemon>` | publish | each daemon | `online` while connected; retained `offline` last-will on disconnect |

`moses_display` subscribes to `index`, `pulse`, `state`, `sensors` and
every `availability/<daemon>`, and publishes nothing at all — not even
an availability of its own, since a display being up says nothing about
the water.

`error` is shared by all daemons; its `source` field says which one
reported the problem. `availability` is instead **per-daemon**
(`availability/watermeter`, `availability/breaker`, `availability/sensors`):
it is a retained MQTT *last will*, so each daemon publishes `online` once
connected and registers `offline` with the broker, published automatically
if that daemon dies or its link drops. A single shared topic would not work
here — being retained, one daemon's `offline` would mask the others — so
each gets its own, letting Home Assistant (and friends) track them
independently.


### Line protocol output

With `WITH_LINEPROTOCOL`, every reading is also written to stdout as one
line, ready to pipe into anything that speaks InfluxDB line protocol:

~~~
watermeter index=213044.000 1790489588441408829
environment temperature=21.42,pressure=102134,humidity=31.68 1790489588441443042
breaker state=0 1790489588441447164
watermeter failure="read" 1790489588441449592
~~~

A failure is a reading too: the measurement stays the same and a
`failure` field says what went wrong, so a gap in the data has a reason
beside it rather than being merely a gap.

Its value is quoted, and that is not cosmetic — a field value which is
neither a number, a boolean nor a quoted string is a parse error and the
whole line is refused. Before that quoting, `failure=set-state` could
not be read as anything at all, and every failure line was dropped by
whatever was consuming them.


Common options
--------------

These apply to every daemon:

| Option                  | Description                                                  |
|-------------------------|--------------------------------------------------------------|
| `-r`, `--reduced-latency` | Switch to the `SCHED_FIFO` real-time scheduler and lock memory (avoids missing pulses / delaying the valve). Usually needs root. |
| `-h`, `--help`          | Show the program-specific usage.                             |

GPIO pins are given as `chip:pin`, for instance `gpiochip0:16`. As a
convenience `rpi:<n>` refers to physical header pin `n` and is mapped to
the matching `gpiochip0` line, so `rpi:36` is the same as `gpiochip0:16`.


### `moses_watermeter`

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
mbus-serial-scan -b 2400 /dev/ttyAMA0
~~~


### `moses_breaker`

| Option                  | Description                                          |
|-------------------------|------------------------------------------------------|
| `-P`, `--pin=CTRL:PIN`  | GPIO line driving the relay (**required**)           |
| `-L`, `--pin-label=STR` | GPIO consumer label                                  |
| `-M`, `--mode=...`      | Output mode: `as-is`, `push-pull`, `open-drain`, `open-source` |
| `-A`, `--active=...`    | Active level: `low` or `high`                        |
| `-I`, `--idle-timeout=SEC` | Re-publish the current state every SEC (heartbeat) |


### `moses_sensors`

| Option                  | Description                                          |
|-------------------------|------------------------------------------------------|
| `-i`, `--interval=SEC`  | Publishing interval (default 60s)                    |
| `-a`, `--altitude=M`    | Convert the reading to sea-level pressure for altitude M (meters) |


### `moses_display`

A read-only front panel on the 0.96" 160x80 LCD the Automation HAT Mini
carries (see [LCD](#lcd)). It subscribes to what the other daemons
publish, asks `upsd` about the battery, optionally reads the same
readings off a [local socket](#a-local-socket-beside-mqtt), and draws:

~~~
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
contains, rendered by the screenshot tests below.

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
which is what the [local socket](#a-local-socket-beside-mqtt) is.

It is dark on purpose: the panel sits in a technical room where a white
screen at full backlight is a lamp, black is the one thing an LCD
renders perfectly, and it leaves colour free to mean something.

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

**The UPS has two ways in, and `--ups` picks between them.**

*Without `--ups`* — `upsd` is not touched at all, and the UPS state comes
from the MQTT events [`nut-notify`](#upsmon) publishes on
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


Supervision
-----------

The daemons are meant to run forever and are not expected to exit. The
[`loop-runner`](scripts/loop-runner) helper restarts a command when it stops and
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
`MOSES_MQTT_…` first, see [Run](#run)) as the
daemons.


Build and installation
======================

Prerequisites
-------------

A C23 compiler, [CMake](https://cmake.org/) (≥ 3.13) and the following
libraries are required:

* `libmosquitto-dev` — MQTT client (used by all three programs)
* `libmbus` — M-Bus library (used by `moses_watermeter` only); see the
  [libmbus](#libmbus) section for building it, as it is usually not
  packaged. The build expects it under `/opt/libmbus`.

The remaining dependencies (LVGL, BME280 sensor API and `bitters`) are
bundled as git submodules under `3rd/`.

On a Debian/Raspberry Pi OS host:

~~~sh
sudo apt install build-essential cmake libmosquitto-dev
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

Build
-----

~~~sh
cmake -B build -DWITH_LOG=1 -DWITH_LINEPROTOCOL=1 -DMQTT_TOPIC_PREFIX=water-breaker
make  -C build
~~~

The first three say **where a reading goes** and are independent of each
other — a build may have all three, one, or none. The rest say what gets
built, one option per program, and the last is for diagnosis.

| CMake options       | Description                                                 |
|---------------------|-------------------------------------------------------------|
| `WITH_MQTT`         | Speak MQTT (**on** by default). Off compiles the MQTT half of `src/common.c` out and drops libmosquitto from the link entirely, for a machine that wants nothing but [line protocol](#line-protocol-output) on stdout. `moses_watermeter` and `moses_sensors` still do their job; `moses_breaker` is **not built** — `state/set` is the only way to command the valve, so without a broker it would open a GPIO and wait forever, which is worse than absent because it looks like a valve controller. `WITH_DISPLAY` is refused outright, being a subscriber and nothing else. |
| `WITH_LINEPROTOCOL` | Also write each reading to stdout as one line of [InfluxDB line protocol](https://docs.influxdata.com/influxdb/latest/reference/syntax/line-protocol/) — `<measurement> <fields> <nanosecond-timestamp>` — for piping into a time-series database. Telegraf, VictoriaMetrics and QuestDB read the same format |
| `WITH_DGRAM`        | Also send each reading to a local unix datagram socket (`DGRAM_PATH`), in the same [line protocol](#line-protocol-output) written to stdout, **and** have `moses_display` read it. For a consumer on **this** machine, which would otherwise cross the network twice to reach a broker on another one — and be cut off entirely when that network is. Sending is non-blocking and silent about every error: nobody listening is the normal state, not a fault, and nothing may hold up a daemon counting pulses or holding a valve. |
| `DGRAM_PATH`        | Where that socket is. Defaults to `/run/moses.sock` on Linux and `/var/run/moses.sock` elsewhere, because only one of those directories exists on each — `make` picks by looking. Keep it short: `sockaddr_un.sun_path` is 104 bytes on the BSDs and a path too long to fit is refused rather than truncated. |
| `MQTT_TOPIC_PREFIX` | Change the default prefix applied to topic (`water-breaker`)|
| `WITH_WATERMETER`   | Build `moses_watermeter` (**on** by default). The only program that wants M-Bus, so turning it off is what lets the tree configure where libmbus is not installed |
| `WITH_BREAKER`      | Build `moses_breaker` (**on** by default). `state/set` is the only way to *command* the valve, so built without `WITH_MQTT` it cannot be told to shut — it still holds the line, reports its state to whatever sinks are on, and fails the valve open when it stops |
| `WITH_TEMPERATURE`  | Build `moses_sensors`, the BME280 reader (**on** by default) |
| `WITH_DISPLAY`      | Build [`moses_display`](#moses_display), the LVGL front panel. Off by default; needs a C++ compiler (LVGL's build enables the language even though nothing here uses it) and pulls in the `3rd/lvgl` submodule, which is a long compile on a Pi Zero. The three daemons build with just a C compiler. |
| `WITH_DISPLAY_TESTS`| Build the [screenshot tests](#tests). Needs LVGL but no panel, so it stands alone on a machine that cannot build `moses_display` at all. |
| `WITH_TESTS`        | Build the unit tests (off by default, so a normal build skips them); see [Tests](#tests). |
| `RPI_GPIO_CHIP`     | Pin the Raspberry Pi's GPIO controller instead of letting bitters find it. Empty by default, which leaves `BITTERS_RPI_GPIO_CHIP` as its list of chip labels — `pinctrl-rp1` (Pi 5), `pinctrl-bcm2711` (Pi 4), `pinctrl-bcm2835` (Pi 1–3 and the Zero), then `gpiochip0` — tried in order and resolved the first time a pin is enabled, which is what makes one binary work on any of them. Naming one skips that search, and is worth it where the board is known and fixed. |
| `WITH_LOG`          | Enable log messages on stderr                               |

The three resulting executables (`moses_watermeter`, `moses_breaker`,
`moses_sensors`) are produced under `bin/`, joined by `moses_display`
when `WITH_DISPLAY` is on. Their command-line options and MQTT topics
are documented in the [Software](#software) section.

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

| knob               | `device`            | `sensors`           | `viewer` |
|--------------------|---------------------|---------------------|----------|
| `WITH_WATERMETER`  | yes                 | yes                 | no       |
| `WITH_BREAKER`     | yes                 | yes                 | no       |
| `WITH_TEMPERATURE` | yes                 | yes                 | no       |
| `WITH_DISPLAY`     | yes                 | no                  | yes      |
| `DISPLAY_BACKEND`  | automation-hat-mini | automation-hat-mini | sdl      |

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

A flavour only supplies *defaults*: any knob set on the command line
still wins, so `make build FLAVOUR=viewer WITH_LOG=yes` is both. `make
flavours` prints the table above with the current values.


Tests
-----

The unit tests are off by default. Enable them at configure time, build,
and run them with `ctest`:

~~~sh
cmake -B build -DWITH_TESTS=ON
make    -C build
ctest --test-dir build --output-on-failure
~~~

| Test            | Covers                                                  |
|-----------------|---------------------------------------------------------|
| `parsers`       | the option and payload parsers in `src/common.c`        |
| `breaker_state` | `breaker_parse_state()`, the valve command vocabulary — also what `moses_display` reads the `state` topic with, so the two cannot disagree |
| `ups_estimate`  | `ups_on_battery()` and `ups_runtime()`: which `ups.status` flags mean on-battery, and the remaining-time division, including every way its inputs can fail to add up |
| `payload`       | `src/display/payload.c`: what `moses_display` makes of a published payload — the index, the pulse count, the sensors JSON and an availability |
| `dashboard`     | the screen itself, rendered to PNG — see below           |

All but `parsers` need neither mosquitto nor M-Bus nor anything
Linux-only, so they run on a development machine too:

~~~sh
cmake -B build-nohw -DWITH_TESTS=ON -DWITH_DISPLAY_TESTS=ON \
      -DWITH_WATERMETER=OFF -DWITH_BREAKER=OFF -DWITH_TEMPERATURE=OFF
make  -C build-nohw
ctest --test-dir build-nohw --output-on-failure
~~~


### A local socket, beside MQTT

Built under [`WITH_DGRAM`](#build-options), which does two things: the
daemons send every reading to a unix datagram socket as well as to the
broker, and `moses_display` binds that socket and reads it.

It exists because **the broker is not on this machine**. Without it, the
index `moses_watermeter` reads travels over the network to another host
and back again to reach a panel ten centimetres away — and when that
network is down the panel goes blank while every daemon behind it is
working perfectly. That is precisely the moment somebody walks up to it.

What travels is the same [line protocol](#line-protocol-output) the
stdout sink writes, one datagram per reading, without the trailing
newline. Not a second format invented for the occasion: `PUT_DATA` and
`PUT_FAIL` are already at every reading, so they gained a sink rather
than the daemons gaining a call, a line carries a real timestamp where
an MQTT payload does not, and other things read it as it stands —
Telegraf's `socket_listener` among them.

The consumer binds and the producers only ever send, so a daemon never
waits on a display: sending is non-blocking, and `ENOENT` for no socket,
`ECONNREFUSED` for one nobody has open and `EAGAIN` for a consumer not
keeping up are all simply "nobody is listening", which is the ordinary
state of the machine. `moses_display` refuses to start if the path is an
existing non-socket file, or if another consumer is already bound to it
— either would otherwise end with one of the two receiving nothing and
neither saying so. The socket is created mode 0660, so a producer running
as another user in a shared group can write to it and the world cannot.

**Two things it cannot carry**, which is why MQTT stays the primary path
rather than being replaced:

- *Retention.* MQTT's retained `index`, `state` and `availability` are
  why the panel fills in within a moment of starting. A datagram socket
  has no memory: a display started between reports shows dashes until the
  next one, which can be a minute.
- *The last will.* MQTT's LWT is how the display learns a daemon died
  rather than merely went quiet. There is no such signal here, so the
  availability marks are deliberately left untouched by this source — a
  datagram proves a daemon was alive a moment ago, but nothing would ever
  clear the mark again, and a panel claiming "online" about a process
  that died an hour ago is worse than one that does not claim to know.
  Staleness covers it instead: every figure carries when it arrived.

The UPS is not on this wire at all. `nut-notify` publishes to MQTT and
`upsd` is asked directly, so the battery comes from one of those two
however this is configured.

Parsing is `src/display/lineproto.c`, kept apart from the socket and
tested by `test/test_lineproto.c` — it decides what figure reaches the
panel, which is worth being able to check without a socket, a daemon or
a Raspberry Pi. `src/display/source-unix.c` is the socket.


### Screenshot tests

`ctest -R dashboard` renders the display's screen with LVGL's own
headless test display and compares it against the reference images in
`test/ref-imgs/`:

~~~sh
cmake -B build-display -DWITH_DISPLAY_TESTS=ON \
      -DWITH_WATERMETER=OFF -DWITH_BREAKER=OFF -DWITH_TEMPERATURE=OFF
make  -C build-display
ctest --test-dir build-display --output-on-failure
~~~

It needs LVGL but no panel, no GPIO, no SPI and no broker — nothing
Linux-only — so it runs on a development machine as well as on the Pi,
which the rest of `moses_display` does not. Turning the three programs off
is what lets it configure where mosquitto and libmbus are not installed.

A reference image that does not exist **fails** the test, rather than
being created from whatever was just rendered. LVGL will do the latter,
and it is the more convenient default, but it means a renamed or newly
added screenshot mints its own golden image and can never fail — the test
would be checking the renderer against itself. So
`LV_TEST_SCREENSHOT_CREATE_REFERENCE_IMAGE` is 0 in `src/lv_conf.h`.

Writing the references is a separate, deliberate step:

~~~sh
make tests-refs     # rewrite test/ref-imgs/ from what the screen renders
git diff --stat test/ref-imgs
make tests-display  # confirm they compare equal
~~~

`make tests-refs` runs the test with `-w`, which writes every reference
instead of comparing it. The images are in the source tree, so `git diff`
says exactly what moved and the images themselves say whether it was
right — **look at them**, because a reference nobody looked at proves
nothing. A reference that exists but does not match leaves
`<name>_err.png` beside it, so a regression can be looked at rather than
guessed at.

Every input is fixed — the clock and the device name are arguments to the
dashboard rather than things it reads, and `TZ` is pinned — so the same
bytes come out on every machine.

What it proves: the layout, the fonts, the three inks, and the whole
path from model to pixels, at the exact 160x80 RGB888 the panel gets.
What it cannot: that those pixels reach the glass. The byte order the
controller reads a pixel in, the (1,26) offset into the ST7735's RAM and
the 270° rotation all live in the backend's flush path, past the point
this sees. Only the panel proves those — and that gap is not
hypothetical: these images were correct while the panel showed the water
drop gold instead of cyan, red and blue exchanged by MADCTL's BGR bit.

Install
-------

There is no `make install` target; copy the executables where you want
them, for instance:

~~~sh
sudo install -m 0755 bin/moses_watermeter bin/moses_breaker \
                     bin/moses_sensors    /usr/local/bin
~~~

The helper scripts under `scripts/` (`loop-runner`, `nut-notify`) are
optional; run them straight from the repository or copy them wherever
suits your setup — they need no particular installation step.

The daemons are meant to run continuously; supervise each one with
[`loop-runner`](#supervision) so it is restarted on failure (and a crash
is reported over MQTT).

Run
---

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

Example running them:

~~~
export MQTT_HOST=127.0.0.1
export MQTT_TOPIC_PREFIX=water-breaker/moses
moses_watermeter -r -i 1min -I 1min -P rpi:38 -B pull-up -E rising 
moses_breaker    -r -P rpi:36 -I 1min -M open-source
moses_sensors    -r -i 1min 
~~~


Various notes
=============


Various documentations:
* https://www.packom.net/wp-content/uploads/2020/11/m-bus-master-hat-datasheet-1.7d.2.pdf




1. Allocate swap

~~~sh
swapfile=swapfile
fallocate -l 1G $swapfile
mkswap $swapfile
swapon $swapfile
~~~
