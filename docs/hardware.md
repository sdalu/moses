Hardware
========

The parts moses is built from, and what each needs from the Raspberry
Pi. The settings below go in `/boot/config.txt` and
`/boot/cmdline.txt` — under `/boot/firmware/` on Raspberry Pi OS since
Bookworm.

Dealing with water, and the purpose being to avoid water damage rather
than cause it, a quality valve (~ 250€) and water meter (~ 100€) were
chosen. The result costs more than a
[Hydrelis Stop-Flow](https://www.hydrelis.fr/stop-flow.php) or a
[Grohe Sense Guard](https://www.grohe.fr/fr_fr/smarthome/grohe-sense-guard/),
but locks you into no proprietary system, and either part can be swapped
for another.

* The valve is 24VDC (no mains voltage) and *normally open*: it is only
  powered to stop the water, which should hopefully never happen — less
  heat, less current, less solenoid stress.
* The water meter is certified MID R400 and U0D0, for accuracy and easy
  installation, and speaks M-Bus or gives pulses.
* A Raspberry Pi runs both, optionally on a battery so that it rides
  through a power cut.

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

It is also possible to replace the _Automation hat mini_ with a
[Relay 4 zero](https://thepihut.com/products/relay-4-zero-4-channel-relay-board-for-pi-zero).
The M-Bus Master hat has a
[datasheet](https://www.packom.net/wp-content/uploads/2020/11/m-bus-master-hat-datasheet-1.7d.2.pdf).


Pin map
-------

| GPIO | Pin | Used by                       | `config.txt`    |
| ---- | --- | ----------------------------- | --------------- |
| 7    | 26  | LCD chip select (`SPI0`)      |                 |
| 9    | 21  | LCD data/command              | see *LCD* below |
| 16   | 36  | relay 1, the valve            | `gpio=16=op,dl` |
| 20   | 38  | `IN2`, pulse counting         | none: see below |
| 25   | 22  | LCD                           | `gpio=25=op,dl` |
| 26   | 37  | M-Bus power (and HAT's `IN1`) | `gpio=26=op,dh` |


PiJuice
-------

The PiJuice carries an RTC, which the Raspberry Pi lacked until the
Pi 5. Enable it in `config.txt` and check it with `hwclock`:

~~~text
dtoverlay=i2c-rtc,ds1307,addr=0x68
~~~


M-Bus Master hat
----------------

### M-Bus power

The hat and the *Automation hat mini* share `GPIO 26` (pin 37). The
M-Bus power pin can be moved by resoldering `R19` (top side, near the
green LED) onto the pin selector on the back; here the HAT's `IN1` is
given up instead. `GPIO 26` driven high keeps the bus powered, so the
HRI runs off the bus rather than its lithium battery (`gpio=26=op,dh`).

### UART

The hat uses the Pi's UART, which Bluetooth and the serial console must
give up. In `config.txt`, one of:

~~~text
dtoverlay=miniuart-bt   # move Bluetooth to the mini UART
dtoverlay=disable-bt    # or disable Bluetooth
~~~

plus `dtoverlay=uart0-pi5` on a Pi 5. Remove `console=serial0,115200`
from `cmdline.txt`, then:

~~~sh
systemctl disable hciuart.service bluealsa.service bluetooth.service
systemctl stop    serial-getty@ttyAMA0.service
systemctl mask    serial-getty@ttyAMA0.service
~~~

Otherwise the M-Bus answers `Failed to receive M-Bus response frame.`


Automation Hat mini
-------------------

Its relay drives the valve; its LCD is `moses_display`'s panel.

### LCD

A 0.96" 160x80 ST7735 on `SPI0` (pins in the [pin map](#pin-map)). Add
to `cmdline.txt` a larger SPI buffer than the default one page, so a
frame goes in one transfer:

~~~text
spidev.bufsiz=65536
~~~

Its data/command line is `GPIO 9`, which is also `SPI0`'s MISO. The SPI
driver claims that pin, and the panel cannot then have it, so `SPI0` has
to be told to leave it alone. In `config.txt`:

~~~text
dtparam=spi=on
dtoverlay=spi0-2cs,no_miso
gpio=9=op,dl
gpio=25=op,dl
~~~

`no_miso` keeps both chip selects and gives up only the pin the ST7735
never drives -- it is write-only, and nothing reads back from it.

Without it `moses_display` stops at *"cannot claim the LCD data/command
pin"* and the kernel logs, in `dmesg` and nowhere else:

~~~text
pinctrl-bcm2835 20200000.gpio: pin gpio9 already requested by
20204000.spi; cannot claim for pinctrl-bcm2835:521
~~~

A kernel that allows the overlap is not evidence that the pin is free:
Linux 6.18.39 permits the claim and 6.18.50 refuses it, on the same
device tree, so a panel that worked before a kernel upgrade can stop at
the reboot that installs one.

### Relay

`GPIO 16`, driven low by default so the valve stays open. Wire +24V to
`COM` and the solenoid's red wire to `NO`.

### Pulse counting

The HRI's pulse output (white wire) goes to `IN2`, `GPIO 20` (pin 38).
**Wired straight, it counts nothing**: over 13 days on moses the M-Bus
index followed the flow litre by litre while not one pulse reached the
pin. Two causes, both electrical:

1. The HRI output is open-drain (24V / 20mA max, 124ms pulse): it only
   pulls to ground. The HAT's inputs sense voltage (820k/120k divider,
   on at 3V, off at 1V), so nothing ever lifts `IN2` to 3V.
2. The SoC's internal pull-up (`gpio=20=ip,pu` or `-B pull-up`), about
   50k against the divider's 120k to ground, holds `GPIO 20` high
   whatever the HAT does.

To make it work:

* Pull the white wire up to +24V through about 10k, brown to ground.
  (The Sensus diagram's 1k would draw 24mA, over the HRI's limit.)
* Leave `GPIO 20` out of `config.txt` and run with `-B disabled`.
* Keep `-E rising`: a pulse pulls low for 124ms, and its release is
  counted, one per litre with the `D1` divisor.

Sensus calls the pulse output and M-Bus *alternatives* and wants a
potential-free connection when both are used, so an opto-coupler belongs
between the white wire and `IN2`. Since the M-Bus index already counts
single litres, polling it more often (`-i`) is the alternative that
needs no hardware at all.
