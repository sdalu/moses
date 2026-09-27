# moses — session entry

- Gate: `ctest`, once the tree configures (README, *Tests*):

      cmake -B build -DWITH_TESTS=ON
      make  -C build
      ctest --test-dir build --output-on-failure

  Run it before modifying anything here (the baseline) and after (the
  proof).
- **The full gate needs libmbus**, but only because of
  `moses_watermeter`. The M-Bus and mosquitto lookups used to be
  `REQUIRED` and unconditional, so configure aborted before any target
  was built — including tests that never touch either. They are now
  conditional: turning the daemons off is what stops the lookups from
  happening at all, which is how the hardware-free tests configure on a
  machine that has neither. There is no longer one flag for all three --
  it is `-DWITH_WATERMETER=OFF -DWITH_BREAKER=OFF -DWITH_TEMPERATURE=OFF`,
  or just `make tests-nohw`, which passes them for you.

  For the daemons themselves,
  build libmbus first (README, *libmbus*; `CMakeLists.txt` hints at
  `/opt/libmbus`), or point `MBUS_INCLUDE_DIR` and `MBUS_LIBRARY` at
  wherever it landed.
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

  `test_ups_estimate` is the second: the UPS arithmetic behind
  `moses_display`, which pulls in neither LVGL nor mosquitto nor
  anything Linux-only.

  `dashboard` is the third, and the useful one — it renders the
  display's screen to a PNG with LVGL's headless test display and
  compares it against `test/ref-imgs/`. All three build off Linux:

      cmake -B build-nohw -DWITH_TESTS=ON -DWITH_DISPLAY_TESTS=ON \
                          -DWITH_WATERMETER=OFF -DWITH_BREAKER=OFF \
                          -DWITH_TEMPERATURE=OFF
      make  -C build-nohw
      ctest --test-dir build-nohw --output-on-failure

  or, the same thing without remembering any of it:

      make tests-nohw

  That is 4 of the 5 tests, and `dashboard` is the only way to see what
  the screen looks like without standing in front of the machine -- it
  found two overflow bugs that the running program would not have
  reported.
  `test_parsers` is the one that stays out: it links `moses_common`,
  which needs mosquitto and `<linux/gpio.h>`. Use that trio as the
  off-Linux gate, call the rest unverified, and say so; CLAUDE.local.md
  names the Linux host to get the real proof from.
- **`moses_display` is off by default and expensive.** `-DWITH_DISPLAY=ON`
  adds the LVGL front panel, which compiles the whole `3rd/lvgl`
  submodule: some seven hours on the Pi Zero, so start it detached and
  come back to it. It needs a C++ compiler (LVGL's build enables the
  language even though nothing uses it). Leave it off for the gate --
  nothing it contains is on the `ctest` path except `ups_estimate`,
  which builds without it.
- README.md says what the three programs do, how to build them, and how
  the MQTT interface is shaped; `CMakeLists.txt` is the authority on the
  build options it names.
- Note that the two preconditions are independent: satisfying libmbus
  gets you a successful `cmake` but not a successful `make` off Linux.
