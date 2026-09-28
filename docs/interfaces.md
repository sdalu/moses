Interfaces
==========

Each daemon has up to three sinks, chosen at build time and none
exclusive of the others: the MQTT broker, one line of InfluxDB line
protocol per reading on stdout, and the same line on the system D-Bus.
`moses_breaker` takes commands from the broker and from the bus. Why
three, and why the bus sits beside the broker, is in
[DESIGN.md](../DESIGN.md).


MQTT topics
-----------

Relative to `MQTT_TOPIC_PREFIX`:

| Topic                   | Direction | By                 | Payload                                              |
| ----------------------- | --------- | ------------------ | ---------------------------------------------------- |
| `index`                 | publish   | `moses_watermeter` | Meter index in litres, e.g. `213011.000`             |
| `pulse`                 | publish   | `moses_watermeter` | Pulses counted (`0` heartbeat on idle timeout)       |
| `leak`                  | publish   | `moses_watermeter` | Retained JSON, with `--leak`: see [Leaks](#leaks)    |
| `state`                 | publish   | `moses_breaker`    | Valve state, `0` (open) or `1` (closed)              |
| `state/set`             | subscribe | `moses_breaker`    | Requested state: `0`/`1`, `off`/`on`, `false`/`true` |
| `sensors`               | publish   | `moses_sensors`    | JSON `{ "temperature", "pressure", "humidity" }`     |
| `error`                 | publish   | all                | JSON `{ "source", "type", "msg" }`                   |
| `availability/<daemon>` | publish   | each daemon        | `online`; retained `offline` as its last will        |

`availability` is per daemon (`watermeter`, `breaker`, `sensors`), so
one daemon's retained `offline` cannot mask the others'.
`moses_display`, with `--source=mqtt`, subscribes to all of these but
`error`; it publishes nothing. The UPS topics, `ups/<ups>/…`, are outside the prefix; see
[system.md](system.md#nut).


### Leaks

With [`--leak`](programs.md#leak-signatures), `moses_watermeter`
publishes its leak report ([leak.md](leak.md)) on `leak`, retained and QoS 1, at start (so a
report from before a restart is replaced) and whenever the level, the
kind or the start of the signature changes:

~~~json
{ "level": "alert", "kind": "flow", "since": 1790483260, "volume": 134, "rate": 5.60, "source": "index" }
~~~

| Field    | Meaning                                                              |
| -------- | -------------------------------------------------------------------- |
| `level`  | `ok`, `warn` or `alert`                                              |
| `kind`   | `none`, `flow`, `slow` or `quiet`                                    |
| `since`  | Unix time the signature started, `0` when `ok`                       |
| `volume` | In litres: counted since then (`flow`), or the run's length (`slow`) |
| `rate`   | L/min for `flow`, L/h for `slow`, `0` otherwise                      |
| `source` | Where the litres came from: `index` or `pulse` ([`--leak-source`](programs.md#leak-signatures)) |

Under `--leak-source=auto` a change of source is published too, and
pulses found not to match the index are reported on `error`:

~~~json
{ "source": "watermeter", "type": "pulse", "msg": "pulses do not match the index: no pulse for 12 L of index" }
~~~

`moses_display` shows it as a banner in its header's place, from the
topic or, under `--source=local`, from the `watermeter leak=` line on
the bus ([programs.md](programs.md#moses_display)).

It is a report, not a latch: a `flow` goes back to `ok` when the water
stops, a `slow` when other use breaks the run, so the next night sends
it again. Closing the valve on it is the consumer's decision.


Line protocol output
--------------------

With `WITH_LINEPROTOCOL`, every reading is also written to stdout as
`<measurement> <fields> <nanosecond-timestamp>`, ready for Telegraf,
InfluxDB, VictoriaMetrics or QuestDB:

~~~text
watermeter index=213044.000 1790489588441408829
environment temperature=21.42,pressure=102134,humidity=31.68 1790489588441443042
breaker state=0 1790489588441447164
watermeter failure="read" 1790489588441449592
watermeter leak=2,kind="flow",since=1790483260,volume=134,rate=5.60,source="index" 1790489588441452110
~~~

A failure is a reading too: the same measurement, with a quoted
`failure` field saying what went wrong.


The system bus
--------------

With `WITH_DBUS`, each daemon also puts every reading on the system
D-Bus, and `moses_display` reads it there — so a panel beside the
daemons keeps working when the network to the broker does not.

~~~text
bus name    moses.watermeter   moses.breaker   moses.sensors
object      /moses/watermeter  /moses/breaker  /moses/sensors
interface   moses.Readings
  property  Lines  as   the last line of each kind the daemon emitted
  signal    Line   s    every line, as it is emitted
interface   moses.Actuator        on moses.breaker only
  method    SetState (s state)    state/set, without the broker
~~~

What travels is the [line protocol](#line-protocol-output) above,
without the trailing newline. "Each kind" is a measurement and its first
field (`watermeter index=`, `watermeter pulse=`, `watermeter leak=`, …
and a `failure=` for each), so a consumer that starts late gets the last index *and* the
last pulse count. Owning a name is being alive: the bus's
`NameOwnerChanged` says at once when a daemon goes or comes back.

A daemon never waits on the bus: sending queues and returns, and a bus
that is missing or refuses the name is logged once and retried in the
background.

`moses_display` owns `moses.display` and serves nothing under it:
owning it is how the display learns the policy is loaded
([`--source`](programs.md#moses_display)).

`dbus/moses.conf` lets root own those four names and call `SetState`,
and anyone read the properties; install it as
[Install](building.md#install) shows. Without it the daemons log
`cannot own moses.…` once and carry on without the bus.

### Reading it

`busctl` comes with systemd; `dbus-send`, `dbus-monitor` and `gdbus`
work without it.

~~~sh
busctl list | grep moses                      # who is on the bus
busctl status moses.breaker                   # PID, UID, command line
gdbus wait --system --timeout 30 moses.breaker

busctl get-property moses.watermeter /moses/watermeter moses.Readings Lines
#   as 2 "watermeter index=213044.000 1790489588441408829" "watermeter pulse=3 1790489648441408829"

busctl --json=short get-property moses.watermeter /moses/watermeter moses.Readings Lines \
  | jq -r '.data[]' \
  | awk '$1 == "watermeter" && $2 ~ /^index=/ { sub("index=", "", $2); print $2 }'
#   213044.000

busctl monitor --match "type='signal',interface='moses.Readings'"
dbus-monitor --system "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0namespace='moses'"

busctl introspect moses.breaker /moses/breaker
~~~

### Shutting the water

`SetState` takes what `state/set` takes and goes through the same code
in `moses_breaker`. Only root may call it:

~~~sh
busctl call moses.breaker /moses/breaker moses.Actuator SetState s 1   # close
busctl call moses.breaker /moses/breaker moses.Actuator SetState s 0   # open
~~~

Anyone else gets `AccessDenied` from `dbus-daemon`; a bad state gets
`InvalidArgs`; a relay that would not move gets `Failed`, and the usual
`critical` error goes out as well. That is the whole of a local override
for when the broker is unreachable.
