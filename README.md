Moses
=====

Moses is a do-it-yourself water-leak breaker built around a Raspberry
Pi. It watches household water usage and can shut the main supply with a
solenoid valve, so a leak does not turn into water damage. Everything is
driven over [MQTT](https://mqtt.org/), so it integrates with existing
home-automation setups (Home Assistant, Node-RED, …) without locking you
into a proprietary cloud.

It is made of small C programs, each doing one job:

| Program            | Role                                                       |
| ------------------ | ---------------------------------------------------------- |
| `moses_watermeter` | Read the water meter (M-Bus index, GPIO pulses)            |
| `moses_breaker`    | Open/close the solenoid valve through a relay              |
| `moses_sensors`    | Read the optional BME280 (temperature, pressure, humidity) |
| `moses_display`    | Show the installation on the HAT's LCD                     |

The first three are the installation; `moses_display` only watches them
and the `sensors` flavour leaves it out.


Documentation
-------------

| Document                                 | What is in it                             |
| ---------------------------------------- | ----------------------------------------- |
| [docs/hardware.md](docs/hardware.md)     | Parts, wiring, each hat's `/boot` set-up  |
| [docs/system.md](docs/system.md)         | Watchdog, WiFi, NUT                       |
| [docs/building.md](docs/building.md)     | Prerequisites, options, flavours, install |
| [docs/programs.md](docs/programs.md)     | Options, environment, `loop-runner`       |
| [docs/interfaces.md](docs/interfaces.md) | MQTT, line protocol, D-Bus                |
| [test/README.md](test/README.md)         | The suite, screenshot references          |
| [DESIGN.md](DESIGN.md)                   | Why it is shaped this way                 |


How it fits together
--------------------

Each daemon publishes under a common topic prefix (`MQTT_TOPIC_PREFIX`,
default `water-breaker`); the topics are listed in
[docs/interfaces.md](docs/interfaces.md#mqtt-topics).

~~~text
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


Quick start
-----------

On the Raspberry Pi, once the hats are set up
([docs/hardware.md](docs/hardware.md)) and
[libmbus](docs/building.md#libmbus) is installed:

~~~sh
sudo apt install build-essential cmake libmosquitto-dev libdbus-1-dev
git clone --recursive https://github.com/sdalu/moses
cd moses
make build FLAVOUR=sensors        # the three daemons, without the panel
sudo make install FLAVOUR=sensors
sudo install -m 0644 dbus/moses.conf /etc/dbus-1/system.d/
~~~

`FLAVOUR=sensors` leaves out `moses_display`, whose LVGL compile takes
some seven hours on a Pi Zero; the default `device` flavour includes it
(see [Flavours](docs/building.md#flavours)). Then point the daemons at
the broker and start them:

~~~sh
export MQTT_HOST=broker.example
export MQTT_TOPIC_PREFIX=water-breaker/moses
moses_watermeter -i 1min
moses_breaker    -P rpi:36 -M open-source
moses_sensors    -i 1min
~~~

Each is meant to run under [`loop-runner`](docs/programs.md#supervision),
which restarts it and reports the crash. Every option is in
[docs/programs.md](docs/programs.md).


When something is wrong
-----------------------

* **The pulse count stays at zero.** As wired, the HRI never reaches the
  pin: [Pulse counting](docs/hardware.md#pulse-counting) has why and
  the fix.
* **`Failed to receive M-Bus response frame.`** The UART is still taken
  by Bluetooth or the serial console: see
  [UART](docs/hardware.md#uart).
* **A daemon logs `cannot own moses.…` once and the bus stays silent.**
  `dbus/moses.conf` is not installed: see
  [Install](docs/building.md#install).
* **The broker is down and the water must be shut.** Do it on the Pi,
  over the system bus, as root:
  [From the command line](docs/interfaces.md#from-the-command-line).


License
-------

GNU General Public License, version 2; see [LICENSE](LICENSE).
