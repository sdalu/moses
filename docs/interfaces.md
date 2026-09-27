Interfaces
==========

Where a reading goes. Each daemon has up to three sinks, chosen at build
time and none exclusive of the others (see
[building.md](building.md#build-with-cmake)): the MQTT broker, one line
of InfluxDB line protocol per reading on stdout, and the same line on
the system D-Bus. `moses_breaker` also takes commands from the broker
and from the bus.


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


Line protocol output
--------------------

With `WITH_LINEPROTOCOL`, every reading is also written to stdout as one
line, ready to pipe into anything that speaks InfluxDB line protocol:

~~~text
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


The system bus, beside MQTT
---------------------------

Built under [`WITH_DBUS`](building.md#build-with-cmake), which does two things: each
daemon puts every reading on the system D-Bus as well as on the broker,
and `moses_display` reads it there.

It exists because **the broker is not on this machine**. Without it, the
index `moses_watermeter` reads travels over the network to another host
and back again to reach a panel ten centimetres away — and when that
network is down the panel goes blank while every daemon behind it is
working perfectly. That is precisely the moment somebody walks up to it.

The bus is the one a Raspberry Pi OS already runs: `dbus-daemon
--system` is there for the rest of the system, so nothing is added to
the machine but one policy file. What each daemon puts on it:

~~~text
bus name    moses.watermeter   moses.breaker   moses.sensors
object      /moses/watermeter  /moses/breaker  /moses/sensors
interface   moses.Readings
  property  Lines  as   the last line of each kind the daemon emitted
  signal    Line   s    every line, as it is emitted
interface   moses.Actuator        on moses.breaker only
  method    SetState (s state)    state/set, without the broker
~~~

What travels is the same [line protocol](#line-protocol-output) the
stdout sink writes, one message per reading, without the trailing
newline. Not a second format invented for the occasion: `PUT_DATA` and
`PUT_FAIL` are already at every reading, so they gained a sink rather
than the daemons gaining a call, a line carries a real timestamp where
an MQTT payload does not, and the standard tools show it as it stands (see
[From the command line](#from-the-command-line)).

"Each kind" is a measurement and its first field — `watermeter index=`,
`watermeter pulse=`, `environment temperature=`, `breaker state=` and a
`failure=` for each — so a consumer that connects late gets the last
index *and* the last pulse count, not merely whichever line came last.

**The daemon never waits on the bus.** Sending queues a message and
returns; a bus that has stopped draining is noticed by the size of that
queue and further signals are dropped rather than accumulated. A bus that
is not there, or that refuses the name, is logged once and retried every
few seconds from a thread of its own, and the readings emitted meanwhile
are kept so that the first consumer still gets them. `dbus-daemon`
restarting is a dropped connection and a reconnect, and never the
`exit()` libdbus performs by default.

**Two things the socket this replaced could not carry**, and this can:

- *Retention.* A display started between reports asks each daemon for
  its `Lines` and is full within a round trip, rather than showing
  dashes until the next report a minute later. Every line carries when
  it was taken, and the model is stamped with that when it is plausible —
  not in the future, not older than a day — so a reading read back this
  way ages from when it was really taken. A line stamped in 1970 by a Pi
  that had not yet set its clock is stamped on arrival instead.
- *The last will.* Owning a name is being alive. The bus emits
  `NameOwnerChanged` the moment an owner drops off it — exits, crashes,
  is killed — which is a last will delivered in milliseconds rather than
  after a keepalive lapses, and a daemon that comes back reclaims its
  name and the same signal says so. Nothing on the display's side is
  re-subscribed for it.

What it will *not* do is claim a daemon is gone because its name was
never seen: a daemon built without the bus, or whose name the policy
refused, is alive and merely silent here. A name that is owned marks its
daemon online, a name seen to lose its owner marks it offline, and a
name with no owner at startup marks nothing — the availability topics
on MQTT keep that say.

**The valve can be commanded here too.** `SetState` on `moses.Actuator`
takes exactly what `state/set` takes, goes through the same parser and
the same setter in `moses_breaker`, and reports the result the same way
— the new state as a line to every sink and a `critical` error if the
relay would not move. It exists for the one moment the broker path
fails, a network outage, which is when a local shut-off matters most:

~~~sh
sudo busctl call moses.breaker /moses/breaker moses.Actuator SetState s 1
~~~

Only `moses.breaker` has the interface; the other two answer
`UnknownMethod`. And only **root** may call it, which is the bus's
policy rather than the daemon's opinion: a stray process on the machine
gets `AccessDenied` from `dbus-daemon` before the call reaches anything.
With this in place `moses_breaker` no longer needs MQTT to be
commandable, so it builds and is useful with `WITH_MQTT` off and
`WITH_DBUS` on.

The bus's policy allows nobody to own a name, and nobody to call a
method, until told. `dbus/moses.conf` lets root own `moses.*` and call
`SetState`, lets anyone ask a daemon for its properties, and nothing
else — `moses.Readings` has no methods, its one property is read-only,
and `Set` is refused by the daemon regardless. It is installed by hand
(see [Install](building.md#install)), and `dbus-daemon` notices the file
on its own. Without it the daemons log
`cannot own moses.watermeter` once and carry on without the bus, and the
display finds nothing there.

The UPS is not on this bus at all. `nut-notify` publishes to MQTT and
`upsd` is asked directly, so the battery comes from one of those two
however this is configured.

### From the command line

Everything below runs on the Pi. `busctl` comes with systemd;
`dbus-send`, `dbus-monitor` and `gdbus` are the same thing without it,
and one of each is shown where the spelling differs.

**Who is on the bus.** A name with no owner is a daemon that is not
running, or whose name the policy refused:

~~~sh
busctl list | grep moses                       # names, owning PID and user
busctl status moses.breaker                    # PID, UID and command line
busctl status moses.breaker >/dev/null 2>&1 || echo "breaker is down"
gdbus wait --system --timeout 30 moses.breaker # block until it is up
~~~

**What a daemon last said**, one line per kind:

~~~sh
busctl get-property moses.watermeter /moses/watermeter moses.Readings Lines
#   as 2 "watermeter index=213044.000 1790489588441408829" "watermeter pulse=3 1790489648441408829"

dbus-send --system --print-reply --dest=moses.watermeter /moses/watermeter \
    org.freedesktop.DBus.Properties.Get string:moses.Readings string:Lines
~~~

`--json=short` makes the same answer machine-readable, and `jq` plus
`awk` pulls one figure out of the line:

~~~sh
busctl --json=short get-property moses.watermeter /moses/watermeter moses.Readings Lines \
  | jq -r '.data[]' \
  | awk '$1 == "watermeter" && $2 ~ /^index=/ { sub("index=", "", $2); print $2 }'
#   213044.000
~~~

**Readings as they happen**, each a `Line` signal from `/moses/<daemon>`:

~~~sh
busctl monitor --match "type='signal',interface='moses.Readings'"
dbus-monitor --system "type='signal',interface='moses.Readings'"
~~~

And daemons coming and going, which is the bus's own signal for names
under `moses.`:

~~~sh
dbus-monitor --system "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0namespace='moses'"
~~~

**What an object offers.** The breaker shows `moses.Readings` and
`moses.Actuator`; the other two show `moses.Readings` alone:

~~~sh
busctl introspect moses.breaker /moses/breaker
gdbus introspect --system --dest moses.breaker --object-path /moses/breaker
~~~

**Shutting the water**, as root, with what `state/set` takes:

~~~sh
sudo busctl call moses.breaker /moses/breaker moses.Actuator SetState s 1   # close
sudo busctl call moses.breaker /moses/breaker moses.Actuator SetState s 0   # open
~~~

Anyone else gets `AccessDenied` from `dbus-daemon` before the call
reaches the daemon; a non-state gets `InvalidArgs` back from the daemon;
a relay that would not move gets `Failed`, and the daemon raises its
usual `critical` error as well. That is the whole of a local override
for when the broker is unreachable — a watchdog on the Pi that decides
the water has run for too long needs the one line above and nothing
else.

Every answer is a line of the [line protocol](#line-protocol-output),
so a consumer parses `measurement field=value,... timestamp`, with the
timestamp in nanoseconds, exactly as it would parse stdout.

`src/dbus_sink.c` is the producing half and `src/display/source-dbus.c`
the consuming one; `test/test_dbus.c` runs the two through a real
`dbus-daemon` that `dbus-run-session` starts for it, and checks exactly
the four things above. Parsing is `src/display/lineproto.c`, kept apart
from the bus and tested by `test/test_lineproto.c` — it decides what
figure reaches the panel, which is worth being able to check without a
bus, a daemon or a Raspberry Pi.
