TODO
====

Proposed improvements, grouped by theme. Roughly ordered by value/effort
within each group.

Bugs / correctness
------------------

- [ ] `parse_idle_timeout` (common.c): drop the always-false `v > UINT64_MAX`
      check; `v <= 0` can only ever mean `v == 0`. (cosmetic, no behavior
      change)

Safety (valve controller)
-------------------------

Considered and rejected:

- TLS for MQTT. Deprioritized: this targets a LAN broker, and the cost of
  rotating certificates yearly outweighs the benefit over an isolated
  network. Credentials are still kept out of argv (env + unset). Revisit
  if the broker is ever exposed beyond the local network.
- GPIO line read-back after a write. A successful SET_VALUES ioctl already
  means the SoC driver applied the register write; it cannot confirm the
  relay/valve physically moved (no independent sensing), and it would
  produce spurious failures in open-drain/open-source modes where the
  read reflects the electrical level, not the driven request.
- Signal handling for "graceful" shutdown. The MQTT last will already
  fires on every ungraceful disconnect, which includes the default
  SIGTERM termination (no handler = process dies = socket closes). A
  clean `mosquitto_disconnect` would instead *suppress* the will, so a
  naive handler is strictly worse. The NO valve also fails safe on
  process death and the kernel releases the GPIO, so there is nothing to
  clean up.

MQTT / integration
------------------

- [ ] Decide on retained breaker `state` (deferred): pairs with the
      availability/LWT for fast reconnect, but can go stale — relies on a
      consumer wiring `state` to the availability topic.
- [ ] Pulse counting publishes per-read event counts with retain=false; a
      consumer that misses messages loses counts. Consider QoS/retain or a
      cumulative counter for meter data integrity.
- [ ] `nut-notify` hardcodes the `ups/...` topic and ignores
      `MQTT_TOPIC_PREFIX`, unlike the rest of the system.

A local socket, beside MQTT
---------------------------

Proposed, not built. A unix datagram socket carrying the same messages
as MQTT, so a producer on this machine can feed a consumer on this
machine without the broker in between.

**Why it is worth it here.** The broker is not on moses -- it is at
another host on the LAN. Today the index that `moses_watermeter` reads
travels over Wi-Fi to that machine and back again to reach a panel ten
centimetres away, and if the broker or the Wi-Fi is down the panel goes
blank while every daemon behind it is working perfectly. That is
precisely the moment someone walks up to it. A local socket removes the
round trip and makes the panel independent of everything off-box.

**Socket type: `SOCK_DGRAM`.** It keeps message boundaries, needs no
connection state on either side, and on `AF_UNIX` it is reliable and
ordered -- it is not UDP, nothing is dropped in transit. One `sendto()`
per reading and there is nothing to reconnect.

The hazard is the one that matters and it has a name: if the consumer
is slow or gone, `sendto()` on a full or missing socket blocks or fails.
A blocked `moses_breaker` is a valve controller that is not controlling
a valve. So **every producer opens the socket `O_NONBLOCK` and treats
`EAGAIN`, `ENOENT` and `ECONNREFUSED` as "nobody is listening", carrying
on without so much as a log line.** This is fire-and-forget telemetry to
a display; it must never be able to hold up the machine. `SOCK_STREAM`
was considered and rejected: it needs framing and reconnection logic,
and a stuck reader still applies backpressure.

**Who binds: the consumer.** `moses_display` creates and owns
`/run/moses/display.sock`; producers `sendto()` it and never care
whether it exists. Filesystem namespace rather than abstract, so the
socket has ordinary permissions (mode 0660, a shared group) and a
display running unprivileged still works. One consumer is all moses
needs; if a second ever appears, the extension is a directory of
sockets that producers write to each of, which changes nothing here.

**Wire format: `<topic> <payload>`, one datagram per message**, with the
topic relative to the prefix exactly as now -- `index 213025.000`,
`state 1`. Deliberately the same vocabulary as MQTT rather than a second
protocol: the socket then carries *the same messages by another route*,
the consumer's parsers are the ones it already has, and a producer's
publish is the same two strings sent twice.

**Where it hooks in, on the producing side: a `dgram_publish()` of its
own, behind `WITH_DGRAM`** -- deliberately *not* inside
`mqtt_publish()`, which was the first shape considered and is the wrong
one:

- `WITH_PUT` already establishes the pattern. It adds a parallel output
  (`PUT_DATA`, a line to stdout) beside each MQTT publish, compiled in
  conditionally and called explicitly at the site. A local socket is the
  same kind of thing and should look like it; `DGRAM_PUBLISH()` in
  `common.h` compiling to nothing without the option is exactly
  `PUT_DATA`'s shape.
- A datagram has no QoS and no retain, and returns nothing an MQTT
  caller would recognise. Threading it through `mqtt_publish()`'s
  signature means parameters that are meaningless on one of the two
  paths, and a return value that has to mean two things.
- It cannot be compiled out cleanly from inside `mqtt_publish()` --
  that needs `#ifdef`s in the middle of the MQTT path, where they would
  sit in the way of anyone reading it.
- The two paths stop being able to break each other. A function called
  `mqtt_publish` that also writes a socket is a function whose name
  lies, and whose failure modes are entangled for no reason.
- It removes a wart the first shape had: `mqtt_publish()` returns early
  when `mqtt->mosq` is NULL, so a socket driven from inside it would go
  silent on exactly the machine with no broker configured -- one of the
  cases worth having it for. A separate function does not know or care
  what MQTT is doing.

The cost, stated plainly: each daemon gains a line beside each of its
publishes, rather than all three inheriting it from one place. That is
the same cost `WITH_PUT` already pays, and it buys a call that is
visible where it happens instead of hidden two layers down.

**The name on the panel.** Over MQTT it is the last segment of
`MQTT_TOPIC_PREFIX`, because that is what identifies an installation on
a shared broker. A datagram socket has no such prefix and no such
ambiguity -- whatever writes it is on this machine -- so there the
hostname is the right name to show, and `device_name()` in
`src/display.c` is the one place that would learn to say so.

**On the consuming side:** a `src/display/source-unix.c` binding the
socket and calling the same `model_set_*()` as the other two sources --
which is what `src/display/model.h` and `source.h` were shaped for, so
nothing in the model or the dashboard changes. The parsers are ready
for it: `src/display/payload.c` already takes a pointer and a length
rather than a mosquitto message, precisely because that is the shape a
datagram arrives in, and `test/test_payload.c` covers them.

**What is lost, and it is not nothing:**

- *Retention.* MQTT's retained `index`, `state` and `availability` are
  why the panel fills in within a moment of starting. A datagram socket
  has no memory: a display started between reports shows dashes until
  the next one, up to a minute. Either accept that, or give producers a
  way to be asked -- which a one-way datagram socket cannot do without
  a second socket going the other way.
- *The last will.* MQTT's LWT is how the display learns a daemon died
  rather than merely went quiet. Over the socket there is no such
  signal, so liveness degrades to the `--stale` timeout alone. The
  display already treats silence as staleness, so it degrades rather
  than breaks -- but it stops being able to tell "dead" from "slow".

Neither is a reason not to do it; both are reasons to keep MQTT as the
primary path and treat the socket as a local shortcut that survives the
network being gone.

**Effort:** perhaps 120 lines -- `dgram_publish()` and its macro in
`common.{c,h}`, a `WITH_DGRAM` option, one line per publish site in the
three daemons, and `source-unix.c` on the other end, which is mostly a
`recvfrom()` loop over parsers that already exist.


Delete src/gpio.c, once bitters can replace it
-----------------------------------------------

`src/gpio.c` is ~265 lines of raw `<linux/gpio.h>`: the Raspberry Pi pin
map, `gpio_open_line()` over `GPIO_V2_GET_LINE_IOCTL`, and the option
parsers that speak in line flags. `moses_breaker` and `moses_watermeter`
use it; `moses_sensors` and `moses_display` use bitters instead. That
split was not a preference -- bitters could not do two of the things the
two hardware daemons need:

- the header's gpiochip was hardcoded to `gpiochip0`, which is the wrong
  controller on a Pi 5 (the 40-pin header moved to the RP1), and
- there was no equivalent of `GPIO_V2_LINE_FLAG_ACTIVE_LOW`, which
  `moses_breaker --active=low` needs to match how a relay is wired.

Both are now closed upstream (unreleased at the time of writing):
`BITTERS_RPI_GPIO_CHIP` expands to
`"pinctrl-rp1|pinctrl-bcm2711|pinctrl-bcm2835|gpiochip0"` -- a list of
chip labels or device names tried in order and resolved when the pin is
enabled -- and `bitters_gpio_cfg_t` has gained an `active_low` field.
That is the same three labels `rpi_gpio_chip()` matches here, with the
old device name kept as a fallback.

**Wait for a released, Linux-tested bitters before acting.** As of
writing those changes are uncommitted upstream and have only had a
syntax check, because that machine is FreeBSD; the gpio-mockup tests
still have to run on Linux. The cfg struct grew, so it warrants a minor
bump (1.3.0) and the vendored submodule here has to move to it --
CLAUDE.local.md records 1.2.0 as what the gate is green on.

Then: everything else the two daemons ask of GPIO is already in bitters
-- direction, default value, label, open-drain/open-source, bias, edge
selection, debounce, and the irq wait/poll/callback trio -- so
`src/gpio.c` collapses to nothing, `parse_gpio_*` re-target from
`GPIO_V2_LINE_FLAG_*` to `BITTERS_GPIO_*`, and `moses_breaker` gets Pi 5
correctness from the library instead of from its own copy of it.

Nothing here has to change to *absorb* the new bitters: the only two
`bitters_gpio_cfg_t` in this tree (`src/display/backend/`) use
designated initializers, so the appended field zero-initialises to "not
active low", which is the right default.


Deployment / hardening
----------------------

- [ ] `install()` targets and systemd unit templates (loop-runner /
      nut-notify already exist but nothing is installed).
- [ ] Run as a dedicated non-root user: RT scheduling + mlockall + GPIO only
      need `CAP_SYS_NICE` + `CAP_IPC_LOCK` + the `gpio` group, granted via
      systemd `AmbientCapabilities`.

Cleanup
-------

- [x] `analog-inputs-blank.c` (~151 KB) sat at the repo root, used only by
      the experimental `WITH_GUI` target. Both are gone: `moses_display`
      replaced that target, and `src/main.c` and `src/integration/` went
      with it.
