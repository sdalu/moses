System configuration
====================

What the Raspberry Pi needs besides the hats.


Watchdog
--------

~~~sh
apt install watchdog
~~~


WiFi
----

For stable, low-latency WiFi, turn power management off:
`iwconfig wlan0 power off`. With `/etc/network/interfaces`:

~~~text
auto wlan0
iface wlan0 inet dhcp
    pre-up ifconfig wlan0 hw ether aa:bb:cc:dd:ee:ff
    pre-up iwconfig wlan0 power off
    wpa-conf /etc/wpa_supplicant/wpa_supplicant.conf
~~~


Nut
---

[NUT](https://networkupstools.org/) supports the PiJuice, and its
`upsmon` runs [`scripts/nut-notify`](../scripts/nut-notify) on each
event. That publishes the event on `ups/<ups>/notify/<type>` and the
current state, retained, on `ups/<ups>/state`, which is what
`moses_display` reads without `--ups`. Replace `pijuice` and `secret`
below with your own UPS name and password.

~~~sh
apt install nut
~~~

`nut.conf`:

~~~conf
MODE=standalone
~~~

`ups.conf`:

~~~conf
[pijuice]
driver = pijuice
port   = /dev/i2c-1
desc   = "PiJuice"
~~~

`upsd.conf`, loopback only, and `upsd.users`:

~~~conf
LISTEN 127.0.0.1 3493
~~~

~~~conf
[upsmon]
        password = secret
        upsmon primary
~~~

`upsmon.conf`: `upsmon` runs `NOTIFYCMD` only for events flagged
`EXEC`, and these are the ones `moses_display` understands:

~~~conf
MONITOR pijuice 1 upsmon secret primary
NOTIFYCMD /path/to/moses/scripts/nut-notify
NOTIFYFLAG ONLINE   SYSLOG+WALL+EXEC
NOTIFYFLAG ONBATT   SYSLOG+WALL+EXEC
NOTIFYFLAG LOWBATT  SYSLOG+WALL+EXEC
NOTIFYFLAG SHUTDOWN SYSLOG+WALL+EXEC
NOTIFYFLAG COMMBAD  SYSLOG+WALL+EXEC
NOTIFYFLAG NOCOMM   SYSLOG+WALL+EXEC
~~~

`nut-notify` takes the MQTT settings from the environment, as the
daemons do ([Environment](programs.md#environment)). `upsmon` usually
passes none, so it falls back to `$MOSES_CONFIG`, else
`$HOME/.config/moses.yaml` (`/root/.config/moses.yaml` without a
`HOME`), read with `yq`:

~~~yaml
:mqtt:
  :host: broker.example
  :port: 1883
  :username: moses
  :password: secret
~~~


Swap
----

A build that runs out of memory on a Pi Zero (512 MB) wants some swap:

~~~sh
fallocate -l 1G /swapfile
chmod 600 /swapfile
mkswap /swapfile
swapon /swapfile
~~~
