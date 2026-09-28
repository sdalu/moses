Tests
=====

What the suite covers, which `make` target runs which part of it, and
how the screenshot references are kept. Build options are in
[docs/building.md](../docs/building.md).


The suite
---------

The unit tests are off by default. `make tests` turns them on, builds
them and runs them:

~~~sh
make tests
~~~

which is this, with the flavour's knobs filled in:

~~~sh
cmake -B build -DWITH_TESTS=ON -DWITH_DBUS=ON
make    -C build
ctest --test-dir build --output-on-failure
~~~

`WITH_TESTS` alone registers six of the eight: `dbus` also wants
`WITH_DBUS`, and `dashboard` wants `WITH_DISPLAY_TESTS`. Every flavour
that produces readings has `WITH_DBUS` on, so `make tests` gets the
sixth without being told.

| Test            | Covers                                                  |
|-----------------|---------------------------------------------------------|
| `parsers`       | the nine option parsers: the M-Bus baudrate and the period and timeout spellings in `src/common.c`, and the `chip:pin`, edge, bias, mode and active-level words in `src/gpio.c` — which is why this one test needs Linux |
| `leak`          | `src/leak.c`, the leak signatures behind `--leak`: each rule's edges on synthetic streams (a flow a minute short, a drip broken by a flush, irregular lone litres, a night that is quiet enough), and replays of the real meter (`test/leak_traces.h`) -- the pipe leak's first night, the toilet stuck at 5.6 L/min, an ordinary day -- so the thresholds keep catching what they were set on; and the pulse check behind `--leak-source=auto`: believed after a matching window, broken on no pulse, half the pulses or pulses with no water, believed again only after two matching windows |
| `breaker_state` | `breaker_parse_state()`, the valve command vocabulary — also what `moses_display` reads the `state` topic with, so the two cannot disagree |
| `ups_estimate`  | `ups_on_battery()` and `ups_runtime()`: which `ups.status` flags mean on-battery, and the remaining-time division, including every way its inputs can fail to add up |
| `payload`       | `src/display/payload.c`: what `moses_display` makes of a published payload — the index, the pulse count, the sensors JSON and an availability |
| `lineproto`     | `src/display/lineproto.c`: what `moses_display` makes of a line off the bus — the wire exactly as the daemons emit it, its timestamp, and every malformed line that must yield nothing rather than half a figure |
| `dbus`          | the bus itself, under `WITH_DBUS`: a daemon's sink and the display's source through a `dbus-daemon` that `dbus-run-session` starts for it — a consumer that starts late is filled in, a live line arrives, a producer that leaves is seen to leave, one that comes back is picked up, one never seen is never called gone, and `SetState` is applied on the breaker, refused with the right error otherwise |
| `dashboard`     | the screen itself, rendered to PNG — see below           |

All but `parsers` need neither mosquitto nor M-Bus nor anything
Linux-only, so they run on a development machine too — `dbus` wanting
only libdbus and `dbus-run-session`, which come with any desktop.
`make tests-nohw` is those seven:

~~~sh
make tests-nohw
~~~

which is this:

~~~sh
cmake -B build -DWITH_TESTS=ON -DWITH_DISPLAY_TESTS=ON -DWITH_DBUS=ON \
      -DWITH_WATERMETER=OFF -DWITH_BREAKER=OFF -DWITH_TEMPERATURE=OFF
make  -C build
ctest --test-dir build --output-on-failure
~~~

Leaving `-DWITH_DBUS=ON` out of that is the quiet way to get five: the
`dbus` test is simply never built, and `ctest` reports 6/6 green with no
sign that the bus went untested.


Screenshot tests
----------------

`ctest -R dashboard` renders the display's screen with LVGL's own
headless test display and compares it against the reference images in
`test/ref-imgs/`:

~~~sh
make tests-display
~~~

which is this:

~~~sh
cmake -B build -DWITH_DISPLAY_TESTS=ON \
      -DWITH_WATERMETER=OFF -DWITH_BREAKER=OFF -DWITH_TEMPERATURE=OFF
make  -C build
ctest --test-dir build --output-on-failure -R dashboard
~~~

It needs LVGL but no panel, no GPIO, no SPI and no broker — nothing
Linux-only — so it runs on a development machine as well as on the Pi,
which the rest of `moses_display` does not. Turning the three programs off
is what lets it configure where mosquitto is not installed.

A reference image that does not exist **fails** the test, rather than
being created from whatever was just rendered. LVGL will do the latter,
and it is the more convenient default, but it means a renamed or newly
added screenshot mints its own golden image and can never fail — the test
would be checking the renderer against itself. So
`LV_TEST_SCREENSHOT_CREATE_REFERENCE_IMAGE` is 0 in `src/lv_conf.h`.

Writing the references is a separate, deliberate step:

~~~sh
make tests-refs     # rewrite test/ref-imgs/ from what the screen renders
git diff --stat test/ref-imgs
make tests-display  # confirm they compare equal
~~~

`make tests-refs` runs the test with `-w`, which writes every reference
instead of comparing it. The images are in the source tree, so `git diff`
says exactly what moved and the images themselves say whether it was
right — **look at them**, because a reference nobody looked at proves
nothing. A reference that exists but does not match leaves
`<name>_err.png` beside it, so a regression can be looked at rather than
guessed at.

Every input is fixed — the clock and the device name are arguments to the
dashboard rather than things it reads, and `TZ` is pinned — so the same
bytes come out on every machine.

What it proves: the layout, the fonts, the three inks, and the whole
path from model to pixels, at the exact 160x80 RGB888 the panel gets.
What it cannot: that those pixels reach the glass. The byte order the
controller reads a pixel in, the (1,26) offset into the ST7735's RAM and
the 270° rotation all live in the backend's flush path, past the point
this sees. Only the panel proves those — and that gap is not
hypothetical: these images were correct while the panel showed the water
drop gold instead of cyan, red and blue exchanged by MADCTL's BGR bit.
