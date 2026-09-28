# moses

Three daemons that read a water meter, a valve and a BME280 on a
Raspberry Pi, and an LVGL front panel that shows them.

## Where things are

- `README.md` — what moses is, the architecture, a quick start, and a
  map of the rest
- `docs/` — `hardware.md` (parts, wiring, `/boot`), `system.md` (the
  Pi's own set-up), `building.md` (prerequisites, options, flavours),
  `programs.md` (options, environment), `interfaces.md` (MQTT, line
  protocol, D-Bus)
- `test/README.md` — what each test covers, and which target runs it
- `DESIGN.md` — why it is shaped this way
- `CHECKLIST.md` — what has to be true before a round here is done
- `CMakeLists.txt` — the authority on every build option either names
- `Makefile` — the targets; `make help`, `make options`, `make flavours`
- `CLAUDE.local.md` — this checkout: the Linux host, and what only builds
  there

## Gate

Gate: `make check && make tests-nohw` — `check` is preflight and runs
none of the project's code; `tests-nohw` is seven of the eight tests and
wants neither Linux, M-Bus nor mosquitto, which makes it the gate on a
development machine. `make tests` is all eight and needs both. Do not
read a passing `tests-nohw` as a passing suite: it compiles none of the
three daemons. `CHECKLIST.md` has the rest of what closes a round,
including when the display owes a build of its own.

## Traps

- **A hand-written `cmake` line quietly registers fewer tests.** `dbus`
  is behind `WITH_DBUS` and `dashboard` behind `WITH_DISPLAY_TESTS`, so a
  forgotten `-D` does not fail — the test is never built and `ctest`
  reports green on one test fewer. CMake accepts an unknown `-D` in
  silence too, which is how CI spent its life passing `-DWITH_PUT=1` and
  never compiling the stdout sink. Use the `make` targets;
  `test/README.md` prints what each expands to.
- **`parsers` is the one test that needs Linux**, because it links
  `moses_gpio` and those parsers speak in GPIO line flags. Everything
  else builds anywhere, so off Linux run the seven, call the daemons
  unverified, and say so.
- **libmbus is built from `3rd/libmbus`, not installed.** Linux is the
  only precondition for the daemons now. `MBUS_LIBRARY` and
  `MBUS_INCLUDE_DIR` still select an installed copy. Do not run its
  autotools inside the submodule: that leaves a stale `config.h` (it
  said 0.9.0 against a 0.10.3 tree), which the build deliberately
  ignores, and 66 untracked files in the tree.
- **`WITH_DISPLAY` compiles the whole of `3rd/lvgl`** — some seven hours
  on a Pi Zero, and it needs a C++ compiler (LVGL's build enables the
  language although nothing here uses it). It is off by default and
  nothing on the `ctest` path wants it: `dashboard` builds LVGL without
  the panel, `ups_estimate` builds neither.
- **A deployed daemon needs `dbus/moses.conf` in
  `/etc/dbus-1/system.d/`.** The gate does not — `test_dbus` gets a
  private session bus — so a daemon that logs `cannot own` once and then
  carries on with the bus silent looks like a code fault and is not.
