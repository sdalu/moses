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

**The producing half is built** (`WITH_DGRAM`, `MOSES_DGRAM_PATH`,
default `/run/moses.sock`); the consuming half is not. What remains is
`src/display/source-unix.c`: bind the socket, parse what arrives, and
call the same `model_set_*()` as the other two sources -- which is what
`src/display/model.h` and `source.h` were shaped for, so nothing in the
model or the dashboard changes.

The socket carries the **line protocol** the daemons emit under
`WITH_LINEPROTOCOL`, not the `<topic> <payload>` first sketched for it.
`PUT_DATA` and `PUT_FAIL` are already at every reading, so they gained a
second sink instead of the daemons gaining a third call, and a line
carries a real timestamp, which an MQTT payload does not. It is also a
format other things read: Telegraf's `socket_listener` ingests it as it
stands.

That decides the shape of what is left. The measurement and field names
(`watermeter index=`, `environment temperature=`) are a different
vocabulary from the MQTT topics, so the dispatch is its own -- though
`payload_double()` and friends still parse the values, and they already
take a pointer and a length rather than a mosquitto message, precisely
because that is the shape a datagram arrives in. `test/test_payload.c`
covers them.

**The consumer binds.** `moses_display` creates and owns the socket;
producers `sendto()` it and never care whether it exists. One consumer is
all moses needs; if a second ever appears, the extension is a directory
of sockets that producers write to each of, which changes nothing here.

**The name on the panel.** Over MQTT it is the last segment of
`MQTT_TOPIC_PREFIX`, because that is what identifies an installation on a
shared broker. A datagram socket has no such prefix and no such ambiguity
-- whatever writes it is on this machine -- so there the hostname is the
right name to show, and `device_name()` in `src/display.c` is the one
place that would learn to say so.

**What the socket cannot carry, and the consumer has to live with:**

- *Retention.* MQTT's retained `index`, `state` and `availability` are
  why the panel fills in within a moment of starting. A datagram socket
  has no memory: a display started between reports shows dashes until the
  next one, up to a minute. Either accept that, or give producers a way
  to be asked -- which a one-way socket cannot do without a second one
  going the other way.
- *The last will.* MQTT's LWT is how the display learns a daemon died
  rather than merely went quiet. Over the socket there is no such signal,
  so liveness degrades to the `--stale` timeout alone: it degrades rather
  than breaks, but it stops being able to tell "dead" from "slow".

Both are reasons to keep MQTT as the primary path and treat the socket as
a local shortcut, not reasons to skip it. The broker is not on moses --
it is another host on the LAN -- so today the index `moses_watermeter`
reads travels over Wi-Fi to that machine and back to reach a panel ten
centimetres away, and if the broker or the Wi-Fi is down the panel goes
blank while every daemon behind it is working perfectly. That is
precisely the moment someone walks up to it.

**Effort:** perhaps 60 lines, mostly a `recvfrom()` loop over parsers
that already exist.


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

- [ ] systemd unit templates, and install the helper scripts. `make
      install` puts the four binaries under `$(BINDIR)`, but
      `scripts/loop-runner` and `scripts/nut-notify` are not installed
      and nothing has a unit.
- [ ] Run as a dedicated non-root user: RT scheduling + mlockall + GPIO only
      need `CAP_SYS_NICE` + `CAP_IPC_LOCK` + the `gpio` group, granted via
      systemd `AmbientCapabilities`.
