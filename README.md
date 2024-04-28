Moses
=====

Hardware
========

As I'm dealing with water and the purpose is to avoid a water damage, not to make one, I select good quality valve (~ 250€) and watermater (~ 100€), which is expensive. The result willl be more costy than a [Hydrelis Stop-Flow](https://www.hydrelis.fr/stop-flow.php) or a [Grohe Sense Guard](https://www.grohe.fr/fr_fr/smarthome/grohe-sense-guard/), but I won't be locked into proprietary systems, and the this type of valve instantaneously cut water.

* The valve will be power by 24VDC (don't want to play with high voltage) and work as normaly open (NO) as we only want it to be powered when it need to emergently stop water 
(which hopfully should be never, and so less heat, less current consumption, less stress on the solenoid). 
* The watermeter will be certified MID R400, U0D0 for good accuracie and easy installation, and will communicate with m-bus or pulse counting
* Control will be performed with a Raspberry PI (it should be also possible to aim at a microcontroller instead). 
* Optional backup with a battery. Especially useful if doing pulse counting, as we don't want to miss pulses due to Raspberry PI being off-line.

## Shopping list
1. [Raspberry PI Zero WH](https://thepihut.com/products/raspberry-pi-zero-wh-with-pre-soldered-header)
1. [Hacker hat](https://thepihut.com/products/hat-hacker-hat)
2. [M-bus master hat (micro version)](https://www.packom.net/product/m-bus-master-hat/)
3. [Automation hat mini](https://thepihut.com/products/automation-hat-mini)
4. [PiJuice for PiZero](https://www.kubii.com/fr/poe-hat-cartes-d-extensions/2795-pijuice-pour-pi-zero-0616909468508.html) + [Battery 600mA](https://www.kubii.com/fr/batteries-piles/2818-1510-batterie-pijuice-3272496311428.html) (optional)
5. [spacer](https://www.amazon.fr/dp/B093FNWP39)
6. [Bürkert (type 6281): Solenoid valve for drinking water, brass, G3/4", NO, 24VDC](https://tameson.fr/products/electrovanne-d-eau-potable-g3-4-en-laiton-no-24vdc-6281-256576-256576) + [Connector](https://tameson.fr/products/connecteur-avec-led-din-a-as-cal-tameson-as-cal) 
7. [Sensus 620 watermeter](https://www.compteur-energie.com/compteurs-eau-froide-sensus-compteur-eau-620.htm) + [HRI B4/D1/8L](https://www.compteur-energie.com/eau-emetteur-impulsions-sensus-hri-b4-amrab152-amrab162.htm)
8. [24VDC power supply (MeanWell LPV-35-24)](https://www.amazon.fr/gp/product/B00ID6L04S) + [5VDC stepdown buck regulator (Bauer Electronics, DC DC 8V-32V to 5V)](https://www.amazon.fr/gp/product/B09B7XZYJQ)

It is also possible to replace the _Automation hat mini_ with a [Replay 4 zero](https://thepihut.com/products/relay-4-zero-4-channel-relay-board-for-pi-zero)

Configuration
=============

PiJuice
-------

The PiJuice come with an RTC (Real Time Clock) which the Rasbperry PI until version 5 is missing.

To use it, the `/boot/config.txt` file need to be edited to place the following line which enable the DS1307 component in the linux kernel.
~~~
dtoverlay=i2c-rtc,ds1307,addr=0x68
~~~

Correct activation can be check by running the `hwclock` command


mBus Master hat
---------------

### mBus power enabling 

There is a minor conflict with the *Automation Hat mini*, as they both
share the `GPIO 26` (`PIN 37`), it's possible to change the mBus power
pin, by unsoldering `R19` on the top side, near the green led, and
soldering it back on the pin selector on the back side.  But we will
consider, we won't use the input `I1` of the automation hat mini, and
keep going with `GPIO 26`.

It will be configured as output and driving high. This will ensure the
mbus is powered, which allows the watermeter reader (HRI) to be
powered by the bus instead of draining its lithium battey

Add in `/boot/config.txt`:
~~~
gpio=26,op,pn,dh
~~~

### UART

The *mBus Master Hat* rely on the Rasperry PI UART, which can conflict
with the bluetooth device and/or the serial console, so they must be
de-activated.

In `/boot/config.txt`
~~~
dtoverlay=miniuart-bt  # Either use the mini uart, crippling bluetooth
dtoverlay=disable-bt   #     Or disable bluetooth

dtoverlay=uart0-pi5    # For RPI 5 only
~~~

The use of `ttyAMA0` (as `serial0`) should be disabled, this is done
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
to receive M-Bus response frame.`  when using the `mbus-serial-*`
programs




Automation Hat mini
-------------------

We only need the relay to drive the solenoid valve, but it also come
with a nice LCD display.

### LCD 

| Device  | Pin                           | Interface                   |
|---------| ------------------------------|-----------------------------|
| Relay 1 | `GPIO16/PIN36`                |                             |
| LCD     | `GPIO9/PIN21`, `GPIO25/PIN22` | `SPI0` + `CS`=`GPIO7/PIN26` |

Pin configuration will be done in the `/boot/config.txt` file:
~~~
gpio=9,op,dl
gpio=25,op,dl
~~~

We will also chage some kernel parameter in `/boot/cmline.txt`, as by
default the SPI buffer size is only 1 page (4096 bytes).  This will
avoid us from having to break some of the SPI transfer into multiple
chunks.

~~~
spidev.bufsiz=65536
~~~

### Relay

In `/boot/config.txt`
~~~
gpio=16,op,dl
~~~

24V need to be connect to the `COM`, and the solenoid wire to the `NO`


### Pulse counting

We will use the `I2` (as `I1` conflict with the *mBus Master Hat*) to 
do some pulse counting (HRI white cable).

~~~
gpio=20,ip,pu
~~~


Software
========

Nut
---

PiJuice is supported by the nut (Network UPS Tools)

~~~sh
apt install nut
~~~

### nut
Configure to run in `standalone` mode be editing the`nut.conf` file:
~~~conf
MODE=standalone
~~~

### ups
The `ups.conf` contains the list of available UPS devices, here we only have the PiJuice:
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
Monitoring is defined in `upsmon.conf`, we will run a custom notification hook declared by `NOTIFYCMD` so UPS state is send using MQTT
~~~conf
MONITOR __ups_name__ 1 upsmon __password__ primary
NOTIFYCMD nut-notify
~~~

The `nut-notify` script is
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

# Notify
${MOSQUITTO_PUB}                                \
    -h "${MQTT_HOST}"                           \
    -u "${MQTT_USER}"                           \
    -P "${MQTT_PASSWD}"                         \
    -t "ups/${UPSNAME}/notify/${NOTIFYTYPE}"    \
    -m "$1"
~~~


libmbus
-------

It is the libmbus sotware which will query the mbus through
`/opt/libmbus/bin/mbus-serial-request-data -b 300 /dev/ttyAMA0 1`. If not already available as a package it can be installed using:

~~~sh
git clone https://github.com/rscada/libmbus
cd libmbus
./build.sh
./configure --prefix=/opt/libmbus
make clean
make
make install
~~~

Ruby
----

~~~sh
rubyver=3.1
apt install ruby${rubyver}dev
bundler${rubyver} config set --local path 'vendors'
bundler${rubyver} update
~~~


Various documentations:
* https://www.packom.net/wp-content/uploads/2020/11/m-bus-master-hat-datasheet-1.7d.2.pdf




1. Allocate swap

~~~sh
swapfile=swapfile
fallocate -l 1G $swapfile
mkswap $swapfile
swapon $swapfile
~~~



Build
=====

~~~sh
cmake -B build
make -C build
~~~
