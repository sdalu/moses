TODO
====

Proposed improvements, grouped by theme. Roughly ordered by value/effort
within each group.

Bugs / correctness
------------------

- [ ] `rpi:` pin mapping is hardcoded to `gpiochip0` (common.c). On the
      Raspberry Pi 5 the 40-pin header is on RP1 (`pinctrl-rp1`, usually
      `gpiochip4`), so `rpi:<pin>` resolves to the wrong chip. Auto-detect
      the chip by label, or at least document the limitation.
- [ ] Period parsers (`parse_s_period` / `parse_us_period`) can silently
      overflow on the unit multiplication (e.g. a huge value times 604800).
- [ ] `parse_idle_timeout` (common.c): drop the always-false `v > UINT64_MAX`
      check; `v <= 0` can only ever mean `v == 0`. (cosmetic, no behavior
      change)

Safety (valve controller)
-------------------------

- [ ] breaker: read the line back (`GPIO_V2_LINE_GET_VALUES_IOCTL`) after
      driving it, and confirm before reporting success on `state`. Today it
      records the *requested* value as truth.
- [ ] Signal handling: trap SIGTERM/SIGINT in all three daemons for a clean
      `mosquitto_disconnect` (and, for the breaker, optionally drive the
      valve to a known-safe position) instead of relying on kernel GPIO
      release + the MQTT last will.
- [ ] Document the fail-safe behavior: on exit the kernel releases the GPIO
      line, de-energizing the relay; with a normally-open valve that means
      water flows (safe). Make this explicit in code/README.
- [ ] `reduced_lattency()` ignores the return values of `sched_setscheduler`
      and `mlockall`; log when real-time setup fails.

MQTT / integration
------------------

- [ ] TLS support: `mqtt_start` connects in plaintext on 1883 and sends
      credentials in the clear. Add `mosquitto_tls_set` (CA cert + 8883),
      configured via env like the rest.
- [ ] Decide on retained breaker `state` (deferred): pairs with the
      availability/LWT for fast reconnect, but can go stale — relies on a
      consumer wiring `state` to the availability topic.
- [ ] Pulse counting publishes per-read event counts with retain=false; a
      consumer that misses messages loses counts. Consider QoS/retain or a
      cumulative counter for meter data integrity.
- [ ] `nut-notify` hardcodes the `ups/...` topic and ignores
      `MQTT_TOPIC_PREFIX`, unlike the rest of the system.

Build / CI
----------

- [ ] Default `CMAKE_BUILD_TYPE` (e.g. RelWithDebInfo) when unset; a plain
      `cmake -B build` currently compiles the latency-sensitive daemons at
      -O0.
- [ ] CI: add a `-Werror` build and a static-analysis pass (`gcc -fanalyzer`
      or `cppcheck`).
- [ ] Unit tests for the pure parsers (`parse_s_period`, `parse_gpio` incl.
      the `rpi:` mapping, `breaker_parse_state`), wired into CI.

Deployment / hardening
----------------------

- [ ] `install()` targets and systemd unit templates (loop-runner /
      nut-notify already exist but nothing is installed).
- [ ] Run as a dedicated non-root user: RT scheduling + mlockall + GPIO only
      need `CAP_SYS_NICE` + `CAP_IPC_LOCK` + the `gpio` group, granted via
      systemd `AmbientCapabilities`.

Cleanup
-------

- [ ] `analog-inputs-blank.c` (~151 KB) sits at the repo root, unreferenced
      by CMake. Move it under `3rd/`/`doc/` or remove it.
- [ ] Fix spelling that leaks into the shared API: `reduced_lattency`
      (latency), plus "Infered"/"lattency" in comments.
