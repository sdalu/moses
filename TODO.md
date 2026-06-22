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

Deployment / hardening
----------------------

- [ ] `install()` targets and systemd unit templates (loop-runner /
      nut-notify already exist but nothing is installed).
- [ ] Run as a dedicated non-root user: RT scheduling + mlockall + GPIO only
      need `CAP_SYS_NICE` + `CAP_IPC_LOCK` + the `gpio` group, granted via
      systemd `AmbientCapabilities`.

Cleanup
-------

- [ ] `analog-inputs-blank.c` (~151 KB) sits at the repo root. It is only
      used by the experimental WITH_GUI target; move it under `src/` (or a
      subdir) so the root is not cluttered, updating the CMake reference.
