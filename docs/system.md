System configuration
====================

What the Raspberry Pi needs besides the hats: a watchdog, a WiFi link
that does not doze, and NUT to report the battery. The `/boot` settings
each hat wants are in [hardware.md](hardware.md); the libraries the
build wants are in [building.md](building.md).

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
~~~text
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
~~~text
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

The repository ships a fuller [`scripts/nut-notify`](../scripts/nut-notify),
which reads the MQTT settings the way the daemons do (see
[Environment](programs.md#environment)) and also publishes the retained
`ups/<ups>/state` that `moses_display` reads.


Swap
----

A build that runs out of memory on a Pi Zero (512 MB) wants some
swap:

~~~sh
swapfile=swapfile
fallocate -l 1G $swapfile
mkswap $swapfile
swapon $swapfile
~~~
