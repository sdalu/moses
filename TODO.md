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

- [ ] Document the fail-safe behavior: on exit the kernel releases the GPIO
      line, de-energizing the relay; with a normally-open valve that means
      water flows (safe). Make this explicit in code/README.
- [ ] `reduced_lattency()` ignores the return values of `sched_setscheduler`
      and `mlockall`; log when real-time setup fails.

Considered and rejected:

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

- [ ] CI: add a static-analysis pass (`gcc -fanalyzer` or `cppcheck`).
      (`-Werror` build is already wired in via WITH_WERROR.)
- [ ] Extend the parser unit tests to cover `breaker_parse_state` (currently
      untested because it lives in breaker.c with main(), not moses_common).

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
