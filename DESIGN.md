Design notes
============

Why moses has the shape it has. What the programs do, how to build them
and what they put on the wire is in [README.md](README.md).


Three sinks, and none of them a choice
--------------------------------------

A reading goes to the broker (`WITH_MQTT`), to stdout as line protocol
(`WITH_LINEPROTOCOL`) and onto the system bus (`WITH_DBUS`). They are not
alternatives: a build may have all three, one, or none, and where the
daemons run all three are on.

They are on together because they cost nothing to have together. The last
two share one formatter -- `put_data()` and `put_fail()` in
`src/common.c`, behind the `PUT_DATA` and `PUT_FAIL` macros every reading
already goes through -- so two sinks cannot render the same reading
differently. Adding the bus meant adding a sink under that formatter, not
a call at every reading site, and a sink nobody is listening on is the
normal state rather than a fault.

Which sink is wanted is a build-time question, not a run-time one. A
daemon built without MQTT does not carry libmosquitto and skip it; the
library leaves the link entirely.


The bus is beside the broker, not instead of it
----------------------------------------------

The broker is not on this machine, so a panel ten centimetres from the
meter would otherwise read it over the network twice, and go blank while
every daemon behind it was working. The bus is the local path for local
consumers; MQTT remains the path for everything off the box. README,
*The system bus, beside MQTT*, has what each daemon puts there and why
retention and the last will are what the replaced socket could not carry.

`moses_display` is a subscriber and nothing else, which is why
`WITH_MQTT=OFF` refuses it outright rather than building something with
nothing to show. `moses_breaker` has two ways in -- `state/set` on the
broker, `SetState` on the bus -- and is built as long as it has one.

Who may command the valve is the bus's policy and not the daemon's
opinion: `dbus/moses.conf` lets root call `SetState` and a stray process
is refused by `dbus-daemon` before the call reaches any of our code.


The line is the format, everywhere
----------------------------------

stdout and the bus carry the same InfluxDB line protocol, one message per
reading, and the bus adds no framing of its own beyond dropping the
trailing newline. Not a second format for the occasion: the daemons
already emit lines, a line carries a real nanosecond timestamp where an
MQTT payload does not, and `busctl` shows it as it stands.

That is also what makes the parsing testable without a bus. Reading a
line is `src/display/lineproto.c`, kept apart from the transport and
covered by `test/test_lineproto.c`, because what figure reaches the panel
is worth checking without a bus, a daemon or a Raspberry Pi.

A failure is a reading rather than a gap: the measurement stays and a
`failure` field says what went wrong. Its value is quoted because a field
value that is neither number, boolean nor quoted string makes the whole
line a parse error -- unquoted, every failure line was silently dropped
by whatever consumed them.


One availability topic per daemon
---------------------------------

`availability/<daemon>`, not one shared topic. Each is a retained MQTT
last will, and retention is what forces the split: on a single topic one
daemon's `offline` would sit there masking the others. The bus answers
the same question by name ownership, which is why a name never seen marks
nothing -- a daemon built without the bus is alive and merely silent
there, and MQTT keeps that say.


A sink never delays a reading
-----------------------------

`moses_watermeter` counts pulses and `moses_breaker` holds a valve, so
neither may be held up by a consumer. Publishing queues and returns. On
the bus, where the guarantee is written down: a consumer that has stopped
draining is noticed by `dbus_connection_get_outgoing_size()` against
`SINK_BACKLOG_MAX` and further signals are dropped rather than
accumulated, a bus that is absent is logged once and retried from a
thread of its own with what was emitted meanwhile kept for the first
consumer, and `dbus_connection_set_exit_on_disconnect()` is `FALSE` in
both halves, because libdbus ends the process on a lost connection by
default and a daemon holding a valve must survive `dbus-daemon`
restarting.

The same concern is why `--reduced-latency` exists at all, and why the
default build type is `RelWithDebInfo` rather than CMake's empty one: a
plain `cmake -B build` would otherwise compile these daemons at `-O0`.


The valve is normally open
--------------------------

`moses_breaker` energises the relay only to close the water. A power cut,
a crash or a daemon that is killed leaves the supply running, which is
the failure worth having: a house with no water is a fault, a house with
water and no meter reading is an inconvenience.


The Linux GPIO half is its own library
--------------------------------------

`<linux/gpio.h>` exists on the Pi and not on the machine this is
developed on, so everything that needs it lives in `src/gpio.c`, built as
`moses_gpio` and linked only by the two daemons that drive a pin.
`src/common.c` -- the MQTT wrapper, the time parsers, `sleep_until()` --
is portable, so `moses_display` builds against SDL anywhere and six of
the seven tests build off Linux entirely. `test_parsers` is the exception
because it links `moses_gpio` on purpose: those parsers speak in GPIO
line flags.


A library is looked for only when something wants it
----------------------------------------------------

M-Bus, mosquitto and libdbus are found under the conditions that select
them, so a machine with none of the three still configures the tree.
There are three questions and conflating them costs the support library:
whether anything needs `moses_common`, whether that needs libmosquitto
linked into it, and whether M-Bus is wanted at all. `WITH_DISPLAY_TESTS`
alone needs none of them.

This is what makes the hardware-free suite possible. When the lookups
were unconditional, a tree without libmbus could not be configured even
to build a test that never goes near M-Bus.


The knobs are an interface library, and LVGL is outside it
---------------------------------------------------------

`moses_defs` carries `MQTT_TOPIC_PREFIX` and the `WITH_*` defines, and
everything first-party links it -- including the tests that compile a
first-party source without `moses_common`. `test_dashboard` builds
`dashboard.c`, which has `#ifdef WITH_LOG` blocks in it; without that one
link line the test would compile them out while the program compiled them
in, and prove the wrong program.

The knobs stop there, and not for tidiness: LVGL reads none of them, but
CMake cannot know that, so a knob within its reach makes every one of
LVGL's ~480 objects stale for a macro none of them sees — on a Pi Zero,
the difference between a rebuild measured in minutes and one measured in
hours. `_GNU_SOURCE` and the warning flags are the deliberate exception,
directory-wide because they apply to everything, LVGL included, and never
change, so they cost no rebuild.


A flavour is a set of defaults, not a mode
------------------------------------------

`FLAVOUR` names the machine -- `device`, `sensors`, `viewer` -- and every
knob it sets can still be overridden on the command line, so `make build
FLAVOUR=viewer WITH_LOG=yes` is both. Nothing in the build reads the
flavour itself; it only decides what the knobs default to, which is why
`make flavours` can print the table and why no code has to know the word.

The three sinks are in that table rather than left as plain build
defaults because which sinks make sense is a property of the machine.
`sensors` keeps `WITH_DBUS` on although it builds no display, so a
consumer can be attached later without a rebuild and finds the last
readings waiting.


check is preflight, tests is the suite
--------------------------------------

`make check` answers "is this tree fit to build" and runs none of the
project's code, so it runs anywhere and it runs first: the flavour names
a real set of knobs, the submodules are at their committed commits, the
helper scripts pass shellcheck. `make tests` answers "does it work". One
command for both would not say which half was red.

The tests are off by default (`WITH_TESTS`), so a normal build does not
compile them, and the suite is not one number: `dbus` needs `WITH_DBUS`
and `dashboard` needs `WITH_DISPLAY_TESTS`, so the `make` targets exist
partly to stop a hand-written `cmake` line from quietly registering fewer
tests than the reader thinks. See README, *Tests*.


The screenshot test owns nothing but LVGL
-----------------------------------------

`test_dashboard` links neither the panel, nor `moses_common`, nor
mosquitto, nor anything Linux-only -- `dashboard.c` and `model.c` reach
LVGL and no further -- so the screen can be rendered and compared on a
machine that cannot build `moses_display` at all. It is independent of
`WITH_DISPLAY` for the same reason.

A missing reference image fails. LVGL will instead mint one from whatever
was just rendered, which is the more convenient default and means a
renamed or newly added screenshot can never fail, the test having become
the renderer checked against itself; so
`LV_TEST_SCREENSHOT_CREATE_REFERENCE_IMAGE` is 0 in `src/lv_conf.h` and
writing the references is a separate deliberate target.

What it cannot see is the backend's flush path -- byte order, the ST7735
RAM offset, the rotation -- and that gap is not hypothetical. README,
*Screenshot tests*, has the failure it missed.


Nothing here is versioned
-------------------------

moses is one deployment on one machine, not something anybody consumes,
so `project(Moses)` carries no version, there is no version header and no
tag is derived from anything. The only version facts that matter are the
submodules' -- `git submodule status`, and `make check` holds them to
their committed commits -- and the commit the running tree is at.

If moses ever is packaged, the number goes in one hand-edited file that a
consumer can read without running anything, and everything else parses it
from there. It is not there yet, and an unused version gate would be one
more thing to keep honest for no reader.
