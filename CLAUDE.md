# moses

Three daemons that read a water meter, a valve and a BME280 on a
Raspberry Pi, and an LVGL front panel that shows them.

## Where things are

- `README.md` — the hardware, what the programs do, how to build them,
  the MQTT and D-Bus interfaces, the tests
- `DESIGN.md` — why it is shaped this way
- `CHECKLIST.md` — what has to be true before a round here is done
- `CMakeLists.txt` — the authority on every build option either names
- `Makefile` — the targets; `make help`, `make options`, `make flavours`
- `CLAUDE.local.md` — this checkout: the Linux host, and what only builds
  there

## Gate

Gate: `make check && make tests-nohw` — `check` is preflight and runs
none of the project's code; `tests-nohw` is six of the seven tests and
wants neither Linux, M-Bus nor mosquitto, which makes it the gate on a
development machine. `make tests` is all seven and needs both. Do not
read a passing `tests-nohw` as a passing suite: it compiles none of the
three daemons. `CHECKLIST.md` has the rest of what closes a round,
including when the display owes a build of its own.

## Traps

- **A hand-written `cmake` line quietly registers fewer tests.** `dbus`
  is behind `WITH_DBUS` and `dashboard` behind `WITH_DISPLAY_TESTS`, so a
  forgotten `-D` does not fail — the test is never built and `ctest`
  reports green on one test fewer. CMake accepts an unknown `-D` in
  silence too, which is how CI spent its life passing `-DWITH_PUT=1` and
  never compiling the stdout sink. Use the `make` targets; README,
  *Tests*, prints what each expands to.
- **`parsers` is the one test that needs Linux**, because it links
  `moses_gpio` and those parsers speak in GPIO line flags. Everything
  else builds anywhere, so off Linux run the six, call the daemons
  unverified, and say so.
- **libmbus and Linux are independent preconditions.** Satisfying
  libmbus gets a successful `cmake` and not a successful `make`. For the
  daemons, build libmbus first (README, *libmbus*; `CMakeLists.txt` hints
  at `/opt/libmbus`) or point `MBUS_INCLUDE_DIR` and `MBUS_LIBRARY` at
  wherever it landed.
- **`WITH_DISPLAY` compiles the whole of `3rd/lvgl`** — some seven hours
  on a Pi Zero, and it needs a C++ compiler (LVGL's build enables the
  language although nothing here uses it). It is off by default and
  nothing on the `ctest` path wants it: `dashboard` builds LVGL without
  the panel, `ups_estimate` builds neither.
- **A deployed daemon needs `dbus/moses.conf` in
  `/etc/dbus-1/system.d/`.** The gate does not — `test_dbus` gets a
  private session bus — so a daemon that logs `cannot own` once and then
  carries on with the bus silent looks like a code fault and is not.
