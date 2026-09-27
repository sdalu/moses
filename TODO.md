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

Both are now closed upstream, and vendored here:
`BITTERS_RPI_GPIO_CHIP` expands to
`"pinctrl-rp1|pinctrl-bcm2711|pinctrl-bcm2835|gpiochip0"` -- a list of
chip labels or device names tried in order and resolved when the pin is
enabled -- and `bitters_gpio_cfg_t` has gained an `active_low` field.
That is the same three labels `rpi_gpio_chip()` matches here, with the
old device name kept as a fallback.

**That wait is over, and the submodule has moved**: `3rd/bitters` is at
`f402d03`, 1.3.0, whose own commit records `check`, `tests`, a `-Werror`
build and the gpio-mockup tests passing on a Raspberry Pi (armv7, Linux
6.18). `RPI_GPIO_CHIP` in `CMakeLists.txt` can now pin the controller at
build time instead of letting it be looked up, since bitters guards the
macro with `#ifndef` for exactly that.

This tree builds against it: the gate passes on the Pi with the three
daemons compiled under GCC `-Werror`, and nothing here needed changing
-- the two `bitters_gpio_cfg_t` in `src/display/backend/` use
designated initializers, so the appended `active_low` zero-initialises
to "not active low", which is the right default.

Then: everything else the two daemons ask of GPIO is already in bitters
-- direction, default value, label, open-drain/open-source, bias, edge
selection, debounce, and the irq wait/poll/callback trio -- so
`src/gpio.c` collapses to nothing, `parse_gpio_*` re-target from
`GPIO_V2_LINE_FLAG_*` to `BITTERS_GPIO_*`, and `moses_breaker` gets Pi 5
correctness from the library instead of from its own copy of it.


Deployment / hardening
----------------------

- [ ] Service definitions. `make install` puts the binaries and the two
      helper scripts under `$(BINDIR)`, and the bus policy under
      `$(DBUSDIR)`, but nothing starts them: no systemd unit, and no
      init script for a machine without systemd.
- [ ] Run as a dedicated non-root user: RT scheduling + mlockall + GPIO only
      need `CAP_SYS_NICE` + `CAP_IPC_LOCK` + the `gpio` group, granted via
      systemd `AmbientCapabilities`.
