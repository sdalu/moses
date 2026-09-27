Hardware
========

The parts moses is built from, and what each one needs from the
Raspberry Pi before the daemons can use it. What goes on the Pi besides
the hardware is in [system.md](system.md); the programs themselves are
in [programs.md](programs.md).

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

Shopping list
-------------

1. [Raspberry PI Zero WH](https://thepihut.com/products/raspberry-pi-zero-wh-with-pre-soldered-header)
1. [Hacker hat](https://thepihut.com/products/hat-hacker-hat)
1. [M-bus master hat (micro version)](https://www.packom.net/product/m-bus-master-hat/)
1. [Automation hat mini](https://thepihut.com/products/automation-hat-mini)
1. [PiJuice for PiZero](https://www.kubii.com/fr/poe-hat-cartes-d-extensions/2795-pijuice-pour-pi-zero-0616909468508.html) + [Battery 600mA](https://www.kubii.com/fr/batteries-piles/2818-1510-batterie-pijuice-3272496311428.html) (optional)
1. [spacer](https://www.amazon.fr/dp/B093FNWP39)
1. [Bürkert (type 6281): Solenoid valve for drinking water, brass, G3/4", NO, 24VDC](https://tameson.fr/products/electrovanne-d-eau-potable-g3-4-en-laiton-no-24vdc-6281-256576-256576) + [Connector](https://tameson.fr/products/connecteur-avec-led-din-a-as-cal-tameson-as-cal) 
1. [Sensus 620 watermeter](https://www.compteur-energie.com/compteurs-eau-froide-sensus-compteur-eau-620.htm) + [HRI B4/D1/8L](https://www.compteur-energie.com/eau-emetteur-impulsions-sensus-hri-b4-amrab152-amrab162.htm) (M-Bus: 2400 baud max)
1. [24VDC power supply (MeanWell LPV-35-24)](https://www.amazon.fr/gp/product/B00ID6L04S) + [5VDC stepdown buck regulator (Bauer Electronics, DC DC 8V-32V to 5V)](https://www.amazon.fr/gp/product/B09B7XZYJQ)
1. [Pimoroni BME280 Breakout](https://shop.pimoroni.com/products/bme280-breakout?variant=29420960677971) (optional)

It is also possible to replace the _Automation hat mini_ with a [Relay 4 zero](https://thepihut.com/products/relay-4-zero-4-channel-relay-board-for-pi-zero)

The M-Bus Master hat has a
[datasheet](https://www.packom.net/wp-content/uploads/2020/11/m-bus-master-hat-datasheet-1.7d.2.pdf).


Pin map
-------

The GPIO lines the sections below set up in `/boot/config.txt`:

| GPIO | Pin | Used by                  | `config.txt`                  |
| ---- | --- | ------------------------ | ----------------------------- |
| 7    | 26  | LCD chip select (`SPI0`) |                               |
| 9    | 21  | LCD                      | `gpio=9=op,dl`                |
| 16   | 36  | relay 1, the valve       | `gpio=16=op,dl`               |
| 20   | 38  | `IN2`, pulse counting    | `gpio=20=ip,pu`, wrong: below |
| 25   | 22  | LCD                      | `gpio=25=op,dl`               |
| 26   | 37  | M-Bus power (and `IN1`)  | `gpio=26=op,dh`               |


PiJuice
-------

The PiJuice comes with an RTC (Real Time Clock), which the Raspberry
PI was missing until version 5.

To use the RTC, the `/boot/config.txt` file must be edited to place
the following line, enabling the DS1307 component in the Linux kernel.

~~~text
dtoverlay=i2c-rtc,ds1307,addr=0x68
~~~

Correct activation can be check by running the `hwclock` command.


M-Bus Master hat
----------------

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
~~~text
gpio=26=op,dh
~~~

### UART


The *mBus Master Hat* relies on the Raspberry PI UART, which can
conflict with the Bluetooth device and the serial console, so they
must be de-activated.

In `/boot/config.txt`
~~~text
dtoverlay=miniuart-bt  # Either use the mini uart, crippling bluetooth
dtoverlay=disable-bt   #     Or disable bluetooth

dtoverlay=uart0-pi5    # For RPI 5 only
~~~

The use of `ttyAMA0` (as `serial0`) should be disabled; this is done
in `/boot/cmdline.txt` by removing/changing the parameter

~~~text
console=serial0,115200
~~~

Eventually you also need to run
~~~text
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
| ------- | ----------------------------- | --------------------------- |
| Relay 1 | `GPIO16/PIN36`                |                             |
| LCD     | `GPIO9/PIN21`, `GPIO25/PIN22` | `SPI0` + `CS`=`GPIO7/PIN26` |

Pin configuration is done in the `/boot/config.txt` file:
~~~text
gpio=9=op,dl
gpio=25=op,dl
~~~

We will also change some kernel parameters in `/boot/cmdline.txt`, as
the SPI buffer size is only 1 page (4096 bytes) by default.  This will
avoid us having to break some of the SPI transfers into multiple
chunks.

~~~text
spidev.bufsiz=65536
~~~

### Relay

The relay is controlled by the `GPIO 16`, the GPIO will be configured
as output and driving low by default, keeping the valve open. 
Configuration is done in `/boot/config.txt`.

~~~text
gpio=16=op,dl
~~~

The 24V need to be connect to the `COM` port, and the solenoid red wire to the
`NO` (Normaly Open) port.


### Pulse counting

We will use the `IN2` port (as previously seen, `IN1` port conflicts
with the *mBus Master Hat*) to do some pulse counting (HRI white
cable).

~~~text
gpio=20=ip,pu
~~~

**As wired above this does not work: no pulse ever reaches `GPIO 20`.**
Measured on 2026-09-26, with `moses_watermeter` running as deployed
(`-P rpi:38 -B pull-up -E rising -I 1min`):

| Check                                       | Result                            |
| ------------------------------------------- | --------------------------------- |
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
