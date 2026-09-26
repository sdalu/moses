# moses — session entry

- Gate: `ctest`, once the tree configures (README, *Tests*):

      cmake -B build -DWITH_TESTS=ON
      make  -C build
      ctest --test-dir build --output-on-failure

  Run it before modifying anything here (the baseline) and after (the
  proof).
- **The gate does not run on a machine without libmbus.** The M-Bus
  `find_path` in `CMakeLists.txt` is `REQUIRED` and unconditional, so
  configure aborts with *"Could not find MBUS_INCLUDE_DIR"* before any
  target is built — including `test_parsers` and `test_breaker_state`,
  which never touch M-Bus. README, *Tests*, gives the three commands
  without this precondition. Build libmbus first (README, *libmbus*;
  `CMakeLists.txt` hints at `/opt/libmbus`), or point `MBUS_INCLUDE_DIR`
  and `MBUS_LIBRARY` at wherever it landed.
- **The gate needs a Linux host.** `src/common.c`, `src/breaker.c`,
  `src/watermeter.c` and the vendored bitters (`gpio.c`, `spi.c`, `i2c.c`)
  include `<linux/gpio.h>`, `<linux/spi/spidev.h>` and `<linux/i2c-dev.h>`,
  so anywhere else the build stops at *"'linux/gpio.h' file not found"* in
  `moses_common` — and `make` builds `all`, so it dies there even when the
  target you want would compile. `test_breaker_state` is the exception: it
  compiles `test/test_breaker_state.c` and `src/breaker_state.c` alone,
  links no `moses_common`, and so is the one thing a non-Linux host can
  prove:

      make  -C build test_breaker_state
      ctest --test-dir build -R breaker_state --output-on-failure

  `test_parsers` does link `moses_common`, so it cannot be built off Linux
  at all. Use that pair as a syntax check, call the rest unverified, and
  say so; CLAUDE.local.md names the Linux host to get the real proof from.
- README.md says what the three programs do, how to build them, and how
  the MQTT interface is shaped; `CMakeLists.txt` is the authority on the
  build options it names.
- Trap: only `moses_watermeter` needs M-Bus, so a change confined to the
  breaker, the sensors or `src/common.c` can be compiled and tested
  without it — except that the configure step above will not let you.
  Making that `find_path` conditional is the fix; until then the
  dependency is global. Note that the two preconditions are independent:
  satisfying libmbus gets you a successful `cmake` but not a successful
  `make` off Linux.
