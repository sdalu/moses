cmake_minimum_required(VERSION 3.13)
project(Moses LANGUAGES C)

#
# Default build type: a plain `cmake -B build` otherwise compiles at -O0,
# which is a poor default for the latency-sensitive daemons. Only applies to
# single-config generators (Make/Ninja); multi-config ones pick per build.
#
get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()


#
# Options
#

#
# Where a reading goes. Three sinks, none of them exclusive: a build may
# have all three, one, or none, and every one of them is decided here
# rather than at run time. They are grouped because they answer the same
# question and are the options most likely to be changed together.
#
#   MQTT           the broker, wherever it is. Off compiles the MQTT
#                  half of common.c out and drops libmosquitto from the
#                  link. moses_breaker is still built -- DBUS is its
#                  other way in -- and so is moses_display, which then
#                  reads the bus, or only upsd. See the WITH_MQTT rules
#                  below.
#   LINEPROTOCOL   one line per reading on stdout, for piping into a
#                  time-series database.
#   DBUS           the same line on the system bus, for a consumer on
#                  this machine -- no network, and nothing lost when
#                  the network is. Each daemon owns a name there and
#                  keeps the last line of each kind, so a consumer that
#                  starts late still gets them and knows who is alive
#                  (src/dbus_sink.h). The breaker also takes its command
#                  there -- SetState, root only -- so it can be shut
#                  from this machine with the broker unreachable. Needs
#                  libdbus, and dbus/moses.conf installed for the bus
#                  to let the daemons own their names.
#
# The last two share one formatter (put_data(), put_fail()), so they
# cannot render the same reading differently.
#
option(WITH_MQTT         "Speak MQTT (off drops libmosquitto entirely)"     ON)
option(WITH_LINEPROTOCOL "Write each reading to stdout as InfluxDB line protocol" OFF)
option(WITH_DBUS         "Also put each reading on the system bus (needs libdbus)" OFF)

set(MQTT_TOPIC_PREFIX "water-breaker" CACHE STRING "Default prefix for topics")

# Which GPIO controller the Raspberry Pi pins are on, when it is worth
# saying rather than letting bitters look.
#
# Left empty, BITTERS_RPI_GPIO_CHIP is bitters' own default: the three
# pinctrl labels tried in order -- pinctrl-rp1 (Pi 5), pinctrl-bcm2711
# (Pi 4), pinctrl-bcm2835 (Pi 1-3 and the Zero) -- with the old
# "gpiochip0" device name last. That is resolved when a pin is first
# enabled, by asking each chip for its label, and it is what makes one
# binary work on any of them.
#
# Set to one of those labels (or to a /dev name) and that search does not
# happen: the build says which controller this machine has and only that
# one is opened. Worth it where the target is known and fixed, which is
# what moses is -- one Pi Zero, one HAT. bitters guards the macro with
# #ifndef exactly so a build can do this.
set(RPI_GPIO_CHIP "" CACHE STRING
    "Pin the Pi's GPIO controller (e.g. pinctrl-bcm2835); empty = let bitters look")


#
# What gets built.
#
# One option per program rather than one for all three, because they do
# not cost the same to build and do not need the same things present:
# only the watermeter wants M-Bus, only the breaker is commanded rather
# than merely read, and a machine may legitimately want one of them and
# not the others.
option(WITH_WATERMETER "Build moses_watermeter (needs M-Bus)"              ON)
option(WITH_BREAKER    "Build moses_breaker (MQTT is how it is commanded)" ON)
option(WITH_TEMPERATURE "Build moses_sensors, the BME280 reader"           ON)
option(WITH_DISPLAY "Build moses_display, the LVGL front panel (needs a C++ compiler)" OFF)
option(WITH_DISPLAY_TESTS "Build the display screenshot tests (implies LVGL)"          OFF)
option(WITH_TESTS    "Build the unit tests"                                OFF)

# Which panel moses_display drives. They are not interchangeable at run
# time: one talks to an ST7735 over SPI and needs Linux, the other opens
# a window and needs SDL2, so the choice is made here.
set(DISPLAY_BACKEND "automation-hat-mini" CACHE STRING "moses_display backend")
set_property(CACHE DISPLAY_BACKEND PROPERTY STRINGS automation-hat-mini sdl)


#
# Diagnostics, and what CI turns on.
#
option(WITH_LOG      "Enable log messages on stderr"                       OFF)
option(WITH_WERROR   "Treat warnings as errors for moses code (CI)"        OFF)
option(WITH_ANALYZER "Run the GCC static analyzer on moses code (CI)"      OFF)


#
# Common compiler options
#

set(CMAKE_C_STANDARD          23)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wformat-security)
add_compile_definitions(_GNU_SOURCE)

#
# What this project's own code is compiled with.
#
# An INTERFACE library, linked by every first-party target, and
# deliberately NOT add_compile_definitions().
#
# add_compile_definitions() is directory-wide, and this directory also
# contains add_subdirectory(3rd/lvgl) some 350 lines below -- so every one
# of these reached all ~483 LVGL objects. LVGL reads none of them, but
# CMake cannot know that, so flipping any single knob invalidated the
# whole of LVGL: on the Pi Zero that is the difference between a rebuild
# measured in minutes and one measured in hours, for macros it never sees.
#
# Everything first-party links this, including the tests that compile a
# first-party source without moses_common -- test_dashboard builds
# dashboard.c, which has two `#ifdef WITH_LOG` blocks, and it would
# otherwise compile them out while the program compiled them in. A target
# added later needs one line here rather than a define it silently lacks.
#
# _GNU_SOURCE and the warning flags above stay directory-wide on purpose:
# they apply to everything compiled here, LVGL included, and they never
# change, so they cost no rebuild.
#
add_library(moses_defs INTERFACE)
target_compile_definitions(moses_defs INTERFACE
    MQTT_TOPIC_PREFIX="${MQTT_TOPIC_PREFIX}")
if (WITH_LOG)
    target_compile_definitions(moses_defs INTERFACE WITH_LOG)
endif()
if (WITH_LINEPROTOCOL)
    target_compile_definitions(moses_defs INTERFACE WITH_LINEPROTOCOL)
endif()
if (WITH_MQTT)
    target_compile_definitions(moses_defs INTERFACE WITH_MQTT)
endif()
if (WITH_DBUS)
    target_compile_definitions(moses_defs INTERFACE WITH_DBUS)
endif()


#
# Directories
#

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set(3RD ${PROJECT_SOURCE_DIR}/3rd)


#
# External libraries
#

find_package(Threads REQUIRED)

# Looked up only when something being built actually wants them. They used
# to be unconditional, which meant a tree with no libmbus could not be
# configured at all -- not even to build a test that never goes near
# M-Bus, on a machine that could not have run the daemons anyway. That is
# now the normal case: WITH_DISPLAY_TESTS alone needs neither library.
# Three separate questions, and conflating the first two costs the
# support library: whether anything needs moses_common at all, whether
# that needs libmosquitto linked into it, and whether M-Bus is wanted.
set(NEED_COMMON    OFF)
set(NEED_GPIO      OFF)
set(NEED_MOSQUITTO OFF)
set(NEED_MBUS      OFF)
if (WITH_WATERMETER)
    set(NEED_COMMON ON)
    set(NEED_GPIO   ON)               # pulse counting
    set(NEED_MBUS   ON)               # and only this one wants M-Bus
endif()
if (WITH_BREAKER)
    set(NEED_COMMON ON)
    set(NEED_GPIO   ON)               # the relay line
endif()
if (WITH_TEMPERATURE)
    set(NEED_COMMON ON)               # the BME280 is I2C, through bitters
endif()
if (WITH_DISPLAY)
    set(NEED_COMMON ON)               # moses_display links it too
endif()
if (NEED_COMMON AND WITH_MQTT)
    set(NEED_MOSQUITTO ON)            # ... and only then is the library wanted
endif()

#
# WITH_MQTT=OFF is for a machine that wants nothing but the line
# protocol on stdout: src/common.c compiles its MQTT half out and
# libmosquitto leaves the link entirely.
#
# moses_display has other ways to its figures: the system bus with
# WITH_DBUS (--source=local), and upsd on this machine for the UPS
# (--ups). With both off it is a panel showing the UPS and dashes, which
# is still a panel -- said at configure time, below, rather than
# refused.
#
# moses_breaker has two ways to be commanded, `state/set` over the
# broker and SetState over the system bus, and is built as long as it
# has one. With WITH_DBUS on it is commanded from this machine alone,
# which is a valve controller with a shorter reach rather than a
# useless one. With both off it holds the line, reports its state, and
# cannot be told to close -- said at configure time, below, because a
# valve controller that cannot be told to close looks like one that can.
#
# moses_watermeter and moses_sensors still do their job: they read their
# hardware and write it out.
#
if (NOT WITH_MQTT)
    if (WITH_DISPLAY AND WITH_DBUS)
        message(STATUS
            "WITH_MQTT=OFF: moses_display reads the system bus only "
            "(--source=local), and the UPS from upsd")
    elseif (WITH_DISPLAY)
        message(STATUS
            "WITH_MQTT=OFF and WITH_DBUS=OFF: moses_display has no source "
            "for the readings -- it shows the UPS from upsd (--ups) and "
            "dashes for the rest")
    endif()
    if (WITH_BREAKER AND WITH_DBUS)
        message(STATUS
            "WITH_MQTT=OFF: moses_breaker is commanded over the system bus "
            "only -- SetState on moses.Actuator, root only (dbus/moses.conf)")
    elseif (WITH_BREAKER)
        message(STATUS
            "WITH_MQTT=OFF: moses_breaker cannot be commanded -- `state/set` "
            "and the bus's SetState are the only ways in, and WITH_DBUS is "
            "off too -- but it still holds the line, reports its state to "
            "whatever sinks are on, and fails the valve open")
    endif()
endif()

# Mosquitto (MQTT) -- used by everything that speaks to the broker
if (NEED_MOSQUITTO)
    find_path(   MOSQUITTO_INCLUDE_DIR NAMES mosquitto.h REQUIRED)
    find_library(MOSQUITTO_LIBRARY     NAMES mosquitto   REQUIRED)
endif()

# M-Bus -- used by moses_watermeter only. Built from the 3rd/libmbus
# submodule (see the bundled dependencies below), unless MBUS_LIBRARY
# and MBUS_INCLUDE_DIR name an installed copy instead.
if (NEED_MBUS AND MBUS_LIBRARY)
    find_path(MBUS_INCLUDE_DIR NAMES mbus/mbus.h REQUIRED)
endif()

# libdbus -- the system bus sink in the daemons, the source in
# moses_display, and the test that runs the two against a private bus.
# Wanted by anything WITH_DBUS puts on the bus, which is everything
# moses_common goes into, and by the test on its own; not looked for
# at all when the knob is off, so a machine without it still configures.
if (WITH_DBUS AND (NEED_COMMON OR WITH_TESTS))
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    message(STATUS "libdbus ${DBUS_VERSION}")
endif()


#
# Bundled dependencies (git submodules under 3rd/)
#

# Bosch BME280 sensor API: compiled once and shared. The compile
# definitions are PUBLIC because BME280_DOUBLE_ENABLE changes the types
# exposed by bme280.h, so consumers must see the same flags.
add_library(bme280 STATIC ${3RD}/BME280_SensorAPI/bme280.c)
target_include_directories(bme280 PUBLIC ${3RD}/BME280_SensorAPI)
target_compile_definitions(bme280 PUBLIC
    BME280_DOUBLE_ENABLE
    BME280_INTF_RET_TYPE=int
    BME280_INTF_RET_SUCCESS=0)

# libmbus, unless an installed copy was named: its five sources, built
# the way BME280 is rather than through its autotools. One wrinkle: mbus.c
# includes "../config.h", which only configure writes -- and a stale one
# left in the submodule by an in-tree build would win, being next to the
# source. So mbus.c is compiled from a copy in the build tree, beside a
# config.h carrying the one thing it reads, VERSION, from configure.ac.
# Third-party code, so none of our warnings.
if (NEED_MBUS AND NOT MBUS_LIBRARY)
    set(_mbus ${3RD}/libmbus)
    file(STRINGS ${_mbus}/configure.ac _mbus_init REGEX "^AC_INIT")
    string(REGEX REPLACE "^AC_INIT\\(\\[libmbus\\], \\[([^]]*)\\].*" "\\1"
           MBUS_VERSION "${_mbus_init}")
    file(WRITE ${CMAKE_BINARY_DIR}/libmbus/config.h
         "#define VERSION \"${MBUS_VERSION}\"\n")
    configure_file(${_mbus}/mbus/mbus.c
                   ${CMAKE_BINARY_DIR}/libmbus/mbus/mbus.c COPYONLY)
    add_library(mbus STATIC
        ${CMAKE_BINARY_DIR}/libmbus/mbus/mbus.c
        ${_mbus}/mbus/mbus-protocol.c
        ${_mbus}/mbus/mbus-protocol-aux.c
        ${_mbus}/mbus/mbus-serial.c
        ${_mbus}/mbus/mbus-tcp.c)
    target_include_directories(mbus PRIVATE ${_mbus}/mbus PUBLIC ${_mbus})
    target_compile_options(mbus PRIVATE -w)
    target_link_libraries(mbus PUBLIC m)
    set(MBUS_LIBRARY mbus)
    message(STATUS "libmbus ${MBUS_VERSION}, built from 3rd/libmbus")
endif()

# bitters (low-level gpio/spi/i2c). Exposed as an INTERFACE library so its
# sources are compiled with each consumer's own flags -- in particular only
# moses_display needs BITTERS_WITH_THREADS.
include(${3RD}/bitters/bitters.cmake)
message(STATUS "bitters ${BITTERS_VERSION}")
add_library(bitters INTERFACE)
target_include_directories(bitters INTERFACE ${BITTERS_INCLUDE_DIR})
target_compile_definitions(bitters INTERFACE BITTERS_SILENCE_WARNING)

# Pinning the Pi's GPIO controller, when RPI_GPIO_CHIP says which.
#
# BITTERS_RPI_BCM_GPIO_CHIP is the macro bitters guards with #ifndef, and
# BITTERS_RPI_GPIO_CHIP is defined from it -- so this is the one to set,
# and setting it replaces the search list rather than adding to it.
#
# On this target rather than directory-wide: bitters' sources are compiled
# inside whichever target links it, so this is where they are, and LVGL
# does not have to be rebuilt to change a Raspberry Pi pin map.
if (RPI_GPIO_CHIP)
    target_compile_definitions(bitters INTERFACE
        BITTERS_RPI_BCM_GPIO_CHIP="${RPI_GPIO_CHIP}")
    message(STATUS "GPIO controller pinned to ${RPI_GPIO_CHIP}")
endif()
# The subsystems every consumer of this target takes. Only I2C: the
# daemons reach nothing else, and a stray GPIO or SPI call in one of them
# is now a compile error in the file that made it rather than a symbol
# that happens to be there. moses_display adds GPIO and SPI for its
# panel, below.
# delay.c is left out entirely: nothing reaches into it.
# One list, so the sources and the BITTERS_WITH_* definitions cannot drift
# apart -- a source without its flag is a compile error in the caller, a
# flag without its source is a missing symbol at link.
set(BITTERS_TAKEN I2C)
target_sources(bitters INTERFACE ${BITTERS_SOURCES_CORE})
foreach(sub IN LISTS BITTERS_TAKEN)
    target_sources(bitters            INTERFACE ${BITTERS_SOURCES_${sub}})
    target_compile_definitions(bitters INTERFACE BITTERS_WITH_${sub})
endforeach()


#
# Shared moses support code (MQTT wrapper, parsers, ...)
#

if (NEED_COMMON)
add_library(moses_common STATIC src/common.c)
target_include_directories(moses_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if (WITH_MQTT)
    target_include_directories(moses_common PUBLIC ${MOSQUITTO_INCLUDE_DIR})
endif()
target_compile_definitions(moses_common PUBLIC PUT_CLOCK=CLOCK_REALTIME
                                               INTERVAL_CLOCK=CLOCK_REALTIME)
target_link_libraries(moses_common PUBLIC moses_defs Threads::Threads m)
if (WITH_MQTT)
    target_link_libraries(moses_common PUBLIC ${MOSQUITTO_LIBRARY})
endif()
# The bus sink goes with the formatter it is fed by. PUBLIC, so that
# moses_display -- which links this and adds its own consuming half --
# gets libdbus from the same place.
if (WITH_DBUS)
    target_sources(moses_common PRIVATE src/dbus_sink.c)
    target_link_libraries(moses_common PUBLIC PkgConfig::DBUS)
endif()
endif()


######################################################################
# Programs
######################################################################

#
# The Linux GPIO half of the shared code. Split from moses_common so that
# common.c stays free of <linux/gpio.h> and moses_display can be built on
# a machine that has no such header -- see src/gpio.c. Built for whoever
# drives a pin, which is the watermeter and the breaker.
#
if (NEED_GPIO)
    add_library(moses_gpio STATIC src/gpio.c)
    target_link_libraries(moses_gpio PUBLIC moses_common)
endif()

#
# Watermeter -- M-Bus index reading and/or GPIO pulse counting
#
if (WITH_WATERMETER)
    add_executable(moses_watermeter src/watermeter.c src/leak.c)
    if (MBUS_INCLUDE_DIR)
        target_include_directories(moses_watermeter PRIVATE ${MBUS_INCLUDE_DIR})
    endif()
    target_link_libraries(moses_watermeter PRIVATE moses_gpio ${MBUS_LIBRARY} m)
endif()

#
# Breaker -- drive the solenoid valve relay. Needs MQTT: see above.
#
if (WITH_BREAKER)
    add_executable(moses_breaker src/breaker.c src/breaker_state.c)
    target_link_libraries(moses_breaker PRIVATE moses_gpio)
endif()

#
# Sensors -- read the BME280 (temperature, pressure, humidity)
#
if (WITH_TEMPERATURE)
    add_executable(moses_sensors src/sensors.c)
    target_link_libraries(moses_sensors PRIVATE moses_common bme280 bitters)
endif()


#
# Treat warnings as errors for first-party code only. -Werror is scoped to
# our own source files rather than to the targets, because the bundled
# bitters submodule is compiled *inside* moses_sensors (INTERFACE library)
# and is not warning-clean.
#
set(MOSES_SOURCES
    src/common.c src/gpio.c src/dbus_sink.c
    src/watermeter.c src/leak.c src/breaker.c src/breaker_state.c src/sensors.c
    src/display.c src/display/backend/sdl.c
    src/display/dashboard.c src/display/model.c
    src/display/backend/rpi-automation-hat-mini.c
    src/display/source-mqtt.c src/display/source-nut.c
    src/display/source-dbus.c src/display/lineproto.c
    src/display/payload.c src/display/ups_estimate.c
    test/test_parsers.c test/test_breaker_state.c test/test_ups_estimate.c
    test/test_payload.c test/test_dashboard.c test/test_lineproto.c
    test/test_dbus.c test/test_leak.c)

if (WITH_WERROR)
    set_property(SOURCE ${MOSES_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -Werror)
endif()

# GCC static analyzer, scoped to first-party sources -- the bundled bitters
# submodule (compiled inside moses_sensors) is not analyzer-clean.
if (WITH_ANALYZER)
    set_property(SOURCE ${MOSES_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -fanalyzer)
endif()


######################################################################
# Tests
######################################################################

# Off by default, so a standard build does not compile them. Enable for the
# test process with -DWITH_TESTS=ON; they then build with `all` and run via ctest.
if (WITH_TESTS)
    enable_testing()

    # Links moses_gpio, so it exists only where <linux/gpio.h> does.
    if (TARGET moses_gpio)
        add_executable(test_parsers test/test_parsers.c)
        target_link_libraries(test_parsers PRIVATE moses_gpio)
        add_test(NAME parsers COMMAND test_parsers)
    endif()

    add_executable(test_breaker_state test/test_breaker_state.c src/breaker_state.c)
    target_include_directories(test_breaker_state PRIVATE src)
    target_link_libraries(test_breaker_state PRIVATE moses_defs)
    add_test(NAME breaker_state COMMAND test_breaker_state)

    # src/leak.c, the leak signatures moses_watermeter reports: pure
    # logic, so it builds and runs on any machine, synthetic streams and
    # replays of the real meter alike (test/leak_traces.h).
    add_executable(test_leak test/test_leak.c src/leak.c)
    target_include_directories(test_leak PRIVATE src test)
    target_link_libraries(test_leak PRIVATE moses_defs m)
    add_test(NAME leak COMMAND test_leak)

    # The UPS arithmetic behind moses_display. Deliberately independent of
    # WITH_DISPLAY: src/display/ups_estimate.c pulls in neither LVGL nor
    # mosquitto nor anything Linux-only, so this runs -- like
    # test_breaker_state -- on a machine that cannot build the daemons.
    add_executable(test_ups_estimate
        test/test_ups_estimate.c src/display/ups_estimate.c)
    target_include_directories(test_ups_estimate PRIVATE src/display)
    target_link_libraries(test_ups_estimate PRIVATE moses_defs m)
    add_test(NAME ups_estimate COMMAND test_ups_estimate)

    # The payload parsers, likewise free of LVGL, mosquitto and Linux.
    add_executable(test_payload
        test/test_payload.c src/display/payload.c)
    target_include_directories(test_payload PRIVATE src/display)
    target_link_libraries(test_payload PRIVATE moses_defs m)
    add_test(NAME payload COMMAND test_payload)

    # The line protocol the system bus carries, read back. Built
    # whatever WITH_DBUS says: lineproto.c is deliberately free of the
    # bus -- and of mosquitto, LVGL and anything Linux-only -- so the
    # parser that decides what figure reaches the panel can be checked
    # on any machine, which is the same reason test_payload and
    # test_breaker_state exist. It takes breaker_state.c because a
    # `state` field is read with the very parser moses_breaker reads its
    # own commands with.
    add_executable(test_lineproto
        test/test_lineproto.c
        src/display/lineproto.c
        src/display/payload.c
        src/breaker_state.c)
    target_include_directories(test_lineproto PRIVATE src src/display)
    target_link_libraries(test_lineproto PRIVATE moses_defs m)
    add_test(NAME lineproto COMMAND test_lineproto)

    # The bus itself: a daemon's sink and the display's source, through
    # a real dbus-daemon that dbus-run-session starts for the occasion.
    # Only under WITH_DBUS, since that is the code it tests, and free of
    # mosquitto, LVGL and Linux like the rest -- it is deliberately not
    # linked against moses_common or moses_defs, so that common.h is
    # read without WITH_MQTT and pulls in no mosquitto.h on a machine
    # that has none. Without dbus-run-session it is built and not run;
    # run by hand it says SKIP and exits 77 when there is no bus.
    if (WITH_DBUS)
        add_executable(test_dbus
            test/test_dbus.c
            src/dbus_sink.c
            src/display/source-dbus.c
            src/display/model.c
            src/display/lineproto.c
            src/display/payload.c
            src/breaker_state.c)
        target_include_directories(test_dbus PRIVATE src src/display)
        target_compile_definitions(test_dbus PRIVATE WITH_DBUS)
        target_link_libraries(test_dbus PRIVATE PkgConfig::DBUS Threads::Threads m)

        find_program(DBUS_RUN_SESSION dbus-run-session)
        if (DBUS_RUN_SESSION)
            add_test(NAME dbus COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_dbus>)
            set_tests_properties(dbus PROPERTIES SKIP_RETURN_CODE 77)
        else()
            message(STATUS "dbus-run-session not found: test_dbus is built but not run")
        endif()
    endif()
endif()


######################################################################
# Display (optional, pulls in LVGL and a C++ toolchain)
######################################################################

# Both off by default: LVGL is a large submodule and a long compile
# (hours on the Pi Zero), and the three daemons do not need it. The
# panel is the Automation HAT Mini's ST7735 (docs/hardware.md, *LCD*).
#
# WITH_DISPLAY_TESTS builds the screenshot tests, which need LVGL but
# not the panel -- so it stands alone on a machine that cannot build
# moses_display at all. It switches LVGL's own test display and PNG
# support on through MOSES_LV_TEST, which src/lv_conf.h reads; that is
# why it is a separate option rather than part of WITH_DISPLAY, since it
# would otherwise put lodepng and the test scaffolding in the program
# that drives the water breaker's panel.
if (WITH_DISPLAY OR WITH_DISPLAY_TESTS)
    # LVGL's build enables the language even though nothing here uses it.
    enable_language(CXX)

    # Named before add_subdirectory() below, because lv_conf.h switches on
    # these and LVGL itself is compiled against that file.
    if (WITH_DISPLAY AND DISPLAY_BACKEND STREQUAL "sdl")
        add_compile_definitions(MOSES_LV_SDL)

        # LVGL's own SDL driver is compiled as part of the lvgl target,
        # not ours, so SDL's headers have to be in scope here -- before
        # add_subdirectory() -- rather than on moses_display. And the
        # driver includes <SDL2/SDL.h> (LV_SDL_INCLUDE_PATH), so it wants
        # the directory *containing* SDL2/, which is not what the
        # imported target advertises on every platform: FreeBSD puts the
        # headers in /usr/local/include/SDL2 and needs /usr/local/include
        # on the path. Both are added, so either spelling resolves.
        find_package(SDL2 REQUIRED)
        get_target_property(_sdl2_incs SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
        foreach(_dir IN LISTS _sdl2_incs)
            get_filename_component(_parent "${_dir}" DIRECTORY)
            include_directories("${_dir}" "${_parent}")
        endforeach()
        message(STATUS "SDL2 includes: ${_sdl2_incs}")
    endif()
    if (WITH_DISPLAY_TESTS)
        add_compile_definitions(MOSES_LV_TEST)
        # Where lv_test_screenshot_compare() looks for a reference image,
        # writes one that is missing, and drops a *_err.png beside one
        # that does not match. It points into the source tree so all
        # three land where they can be looked at and committed.
        #
        # Set globally rather than on the test target because the code
        # that reads it is lv_test_screenshot_compare.c, which belongs to
        # the lvgl target. The "A:" is LVGL's POSIX filesystem drive
        # letter (LV_FS_POSIX_LETTER in src/lv_conf.h): LVGL's lodepng
        # goes through lv_fs_* rather than stdio, so a bare path does not
        # open.
        add_compile_definitions(
            REF_IMGS_PATH="A:${CMAKE_CURRENT_SOURCE_DIR}/test/ref-imgs/")
    endif()

    # LVGL. Its configuration is src/lv_conf.h; examples, demos and ThorVG
    # are not built, nothing here draws vectors.
    set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/lv_conf.h
        CACHE PATH "LVGL configuration" FORCE)
    set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_BUILD_DEMOS    OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_USE_THORVG     OFF CACHE BOOL "" FORCE)
    add_subdirectory(${3RD}/lvgl)
endif()

if (WITH_DISPLAY)
    message(STATUS "Display backend: ${DISPLAY_BACKEND}")

    add_executable(moses_display
        src/display.c
        src/breaker_state.c		# the `state` payload vocabulary, shared
        src/display/dashboard.c
        src/display/model.c
        src/display/payload.c		# the numbers and text, for either source
        src/display/source-nut.c
        src/display/ups_estimate.c)

    # The broker's half. Only under WITH_MQTT, which is also what brings
    # libmosquitto in.
    if (WITH_MQTT)
        target_sources(moses_display PRIVATE
            src/display/source-mqtt.c)
    endif()

    # The bus's consuming half. Only under WITH_DBUS, which is also what
    # puts the producing half into the daemons -- there is no point
    # watching a bus nothing speaks on. libdbus itself arrives through
    # moses_common, which links it PUBLIC for the same reason.
    if (WITH_DBUS)
        target_sources(moses_display PRIVATE
            src/display/lineproto.c
            src/display/source-dbus.c)
    endif()
    target_include_directories(moses_display PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/display
        ${CMAKE_CURRENT_SOURCE_DIR}/src/display/backend)

    # This program is threaded -- mosquitto's network thread, the upsd
    # poller, and LVGL's own software render unit (lv_conf.h sets
    # LV_USE_OS to LV_OS_PTHREAD).
    target_link_libraries(moses_display PRIVATE
        moses_common lvgl::lvgl Threads::Threads m)

    if (DISPLAY_BACKEND STREQUAL "automation-hat-mini")
        # The real panel: an ST7735 on SPI0 CE1 with two GPIO lines.
        target_sources(moses_display PRIVATE
            src/display/backend/rpi-automation-hat-mini.c)
        target_compile_definitions(moses_display PRIVATE BITTERS_WITH_THREADS)

        # Two subsystems the daemons do not take. Same one-list rule as
        # above: a source without its flag is a compile error in the
        # caller, a flag without its source is a missing symbol at link.
        set(BITTERS_TAKEN_DISPLAY GPIO SPI)
        foreach(sub IN LISTS BITTERS_TAKEN_DISPLAY)
            target_sources(moses_display             PRIVATE ${BITTERS_SOURCES_${sub}})
            target_compile_definitions(moses_display PRIVATE BITTERS_WITH_${sub})
        endforeach()
        target_link_libraries(moses_display PRIVATE bitters)

    elseif (DISPLAY_BACKEND STREQUAL "sdl")
        # A window on a development machine. No bitters, no GPIO, no SPI,
        # and with gpio.c split out of moses_common, nothing Linux-only
        # in the whole program.
        target_sources(moses_display PRIVATE src/display/backend/sdl.c)
        target_link_libraries(moses_display PRIVATE SDL2::SDL2)

    else()
        message(FATAL_ERROR
            "DISPLAY_BACKEND must be automation-hat-mini or sdl "
            "(got \"${DISPLAY_BACKEND}\")")
    endif()
endif()

# The screenshot tests. Deliberately independent of WITH_DISPLAY and of
# moses_common: dashboard.c and model.c reach nothing but LVGL, so this
# links neither the panel, nor mosquitto, nor anything Linux-only, and
# builds wherever LVGL does. The reference images live under
# test/ref-imgs/; see REF_IMGS_PATH above.
if (WITH_DISPLAY_TESTS)
    enable_testing()

    add_executable(test_dashboard
        test/test_dashboard.c
        src/display/dashboard.c
        src/display/model.c
        src/display/ups_estimate.c)
    target_include_directories(test_dashboard PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/display)
    target_link_libraries(test_dashboard PRIVATE moses_defs lvgl::lvgl Threads::Threads m)
    add_test(NAME dashboard COMMAND test_dashboard)
endif()
