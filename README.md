Moses
=====



Build
-----

mkdir build
cmake . -B build

Hardware
--------

As we are dealing with water and the purpose is to avoid a water damage not to make one, we need to select good quality valve and watermater, which is expensive.
* The valve will be power by 24VDC (don't want to play with high voltage) and work as normaly open (NO) as we only want it to be powered when it need to emergently stop water 
(which hopfully should be never, and so less heat, less current consumption, less stress on the solenoid). 
* The watermeter will be certified MID R400, U0D0 for good précision and easy installation, and will communicate with m-bus or pulse counting
* Control will be performed with a Raspberry PI (but it should be possible to aim at a microcontroller instead)

### Shopping list
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
-------------

### PiJuice

The PiJuice come with an RTC (Real Time Clock) which the Rasbperry Pi until version 5 is missing.

To use it, the `/boot/config.txt` file need to be edited to place the following line which enable to DS1307 component in the linux kernel.
~~~
dtoverlay=i2c-rtc,ds1307,addr=0x68
~~~

Correct activation can be check by running the `hwclock` command


### mBus Master hat

There is a minor conflict with the Automation Hat mini,
as they both share the GPIO 26 (PIN 37), it's possible
to change the mBus power pin, by unsoldering R19 on the
top side, near the green led, and soldering it back on the pin selector on the
back side.

We will consider, we won't use the analog input A1 of the automation hat mini, and keep going with GPIO 26. 
It will be configured as output and driving high. This will ensure the mbus is powered, which allows the watermeter reader (HRI) to be powered by the bus instead of draining it's lithium battey

~~~
gpio=26,op,pn,dh
~~~


### Automation Hat mini

We only need the relay to drive the solenoid valve, but it also comme with a nice LCD display.

| Device  | Pin                       | Interface             |
|---------| --------------------------|-----------------------|
| Relay 1 | GPIO16/PIN36              |                       |
| LCD     | GPIO9/PIN21, GPIO25/PIN22 | SPI0 + CS=GPIO7/PIN26 |


~~~
# Relay
gpio=16,op,dl
# LCD
gpio=9,op,dl
gpio=25,op,dl
~~~








https://www.packom.net/m-bus-master-hat/
https://www.packom.net/product/m-bus-master-hat/
https://www.packom.net/wp-content/uploads/2020/11/m-bus-master-hat-datasheet-1.7d.2.pdf

1. Allocate swap

~~~sh
swapfile=swapfile
fallocate -l 1G $swapfile
mkswap $swapfile
swapon $swapfile
~~~

2. Use rustup to install last version of rust

https://rustup.rs/

~~~sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
~~

3.install libmbus

~~~sh
git clone https://github.com/rscada/libmbus
cd libmbus
./build.sh
./configure --prefix=/opt/libmbus
make clean
make
make install
~~~

4.Install mbus-httpd

https://github.com/packom/mbus-httpd

~~~sh
git clone https://github.com/packom/mbus-httpd
cd mbus-httpd
cargo build
~~~

env SERVER_IP=localhost \
env SERVER_PORT=8080 \
env LIBMBUS_PATH=~/libmbus/bin \
env LD_LIBRARY_PATH=/usr/local/lib \
env RUST_LOG=INFO \


LIBMBUS_PATH=<limbus binary path e.g. ~/libmbus/bin>
LD_LIBRARY_PATH<path libmbus.so is installed to e.g. /usr/local/lib>

cargo run
