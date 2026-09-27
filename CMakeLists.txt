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
#                  link; moses_breaker then cannot be built, having no
#                  other way to be told to shut the water.
#   LINEPROTOCOL   one line per reading on stdout, for piping into a
#                  time-series database.
#   DGRAM          the same line to a unix datagram socket, for a
#                  consumer on this machine -- no network, and nothing
#                  lost when the network is.
#
# The last two share one formatter (put_data(), put_fail()), so they
# cannot render the same reading differently.
#
option(WITH_MQTT         "Speak MQTT (off drops libmosquitto entirely)"     ON)
option(WITH_LINEPROTOCOL "Write each reading to stdout as InfluxDB line protocol" OFF)
option(WITH_DGRAM        "Also send each reading to a local unix datagram socket" OFF)

set(MQTT_TOPIC_PREFIX "water-breaker" CACHE STRING "Default prefix for topics")
# sockaddr_un.sun_path is 104 bytes on the BSDs and 108 on Linux, and a
# path too long to fit is refused rather than truncated -- keep it short.
set(MOSES_DGRAM_PATH "/run/moses.sock" CACHE STRING "Local datagram socket")


#
# What gets built.
#
option(WITH_DAEMONS  "Build the three daemons (needs mosquitto, and M-Bus)" ON)
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

if (WITH_LOG)
    add_compile_definitions(WITH_LOG)
endif()
if (WITH_LINEPROTOCOL)
    add_compile_definitions(WITH_LINEPROTOCOL)
endif()
add_compile_definitions(MQTT_TOPIC_PREFIX="${MQTT_TOPIC_PREFIX}")
if (WITH_MQTT)
    add_compile_definitions(WITH_MQTT)
endif()
if (WITH_DGRAM)
    add_compile_definitions(WITH_DGRAM)
    add_compile_definitions(MOSES_DGRAM_PATH="${MOSES_DGRAM_PATH}")
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
set(NEED_MOSQUITTO OFF)
set(NEED_MBUS      OFF)
if (WITH_DAEMONS)
    set(NEED_COMMON ON)               # every daemon links it
    set(NEED_MBUS   ON)               # moses_watermeter
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
# Two of the programs cannot mean anything without it, and are refused
# rather than built into something useless:
#
#   moses_breaker   `state/set` is the only way to command the valve, so
#                   without MQTT it would open a GPIO and wait forever.
#                   A valve controller that cannot be told to close is
#                   worse than no valve controller, because it looks
#                   like one.
#   moses_display   it is a subscriber and nothing else.
#
# moses_watermeter and moses_sensors still do their job: they read their
# hardware and write it out.
#
if (NOT WITH_MQTT)
    if (WITH_DISPLAY)
        message(FATAL_ERROR
            "WITH_DISPLAY needs WITH_MQTT: moses_display only ever "
            "subscribes, so there is nothing for it to show without a broker")
    endif()
    if (WITH_DAEMONS)
        message(STATUS
            "WITH_MQTT=OFF: building moses_watermeter and moses_sensors only "
            "(moses_breaker cannot be commanded without MQTT)")
    endif()
endif()

# Mosquitto (MQTT) -- used by everything that speaks to the broker
if (NEED_MOSQUITTO)
    find_path(   MOSQUITTO_INCLUDE_DIR NAMES mosquitto.h REQUIRED)
    find_library(MOSQUITTO_LIBRARY     NAMES mosquitto   REQUIRED)
endif()

# M-Bus -- used by moses_watermeter only (usually not packaged, see README)
if (NEED_MBUS)
    find_path(   MBUS_INCLUDE_DIR NAMES mbus/mbus.h REQUIRED HINTS /opt/libmbus/include)
    find_library(MBUS_LIBRARY     NAMES mbus        REQUIRED HINTS /opt/libmbus/lib)
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

# bitters (low-level gpio/spi/i2c). Exposed as an INTERFACE library so its
# sources are compiled with each consumer's own flags -- in particular only
# moses_display needs BITTERS_WITH_THREADS.
include(${3RD}/bitters/bitters.cmake)
message(STATUS "bitters ${BITTERS_VERSION}")
add_library(bitters INTERFACE)
target_include_directories(bitters INTERFACE ${BITTERS_INCLUDE_DIR})
target_compile_definitions(bitters INTERFACE BITTERS_SILENCE_WARNING)
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
target_link_libraries(moses_common PUBLIC Threads::Threads m)
if (WITH_MQTT)
    target_link_libraries(moses_common PUBLIC ${MOSQUITTO_LIBRARY})
endif()
endif()


######################################################################
# Programs
######################################################################

if (WITH_DAEMONS)

#
# The Linux GPIO half of the shared code. Split from moses_common so that
# common.c stays free of <linux/gpio.h> and moses_display can be built on
# a machine that has no such header -- see src/gpio.c.
#
add_library(moses_gpio STATIC src/gpio.c)
target_link_libraries(moses_gpio PUBLIC moses_common)

#
# Watermeter -- M-Bus index reading and/or GPIO pulse counting
#
add_executable(moses_watermeter src/watermeter.c)
target_include_directories(moses_watermeter PRIVATE ${MBUS_INCLUDE_DIR})
target_link_libraries(moses_watermeter PRIVATE moses_gpio ${MBUS_LIBRARY})

#
# Breaker -- drive the solenoid valve relay. Needs MQTT: see above.
#
if (WITH_MQTT)
    add_executable(moses_breaker src/breaker.c src/breaker_state.c)
    target_link_libraries(moses_breaker PRIVATE moses_gpio)
endif()

#
# Sensors -- read the BME280 (temperature, pressure, humidity)
#
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
    src/common.c src/gpio.c
    src/watermeter.c src/breaker.c src/breaker_state.c src/sensors.c
    src/display.c src/display/backend/sdl.c
    src/display/dashboard.c src/display/model.c
    src/display/backend/rpi-automation-hat-mini.c
    src/display/source-mqtt.c src/display/source-nut.c
    src/display/payload.c src/display/ups_estimate.c
    test/test_parsers.c test/test_breaker_state.c test/test_ups_estimate.c
    test/test_payload.c test/test_dashboard.c)

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
    add_test(NAME breaker_state COMMAND test_breaker_state)

    # The UPS arithmetic behind moses_display. Deliberately independent of
    # WITH_DISPLAY: src/display/ups_estimate.c pulls in neither LVGL nor
    # mosquitto nor anything Linux-only, so this runs -- like
    # test_breaker_state -- on a machine that cannot build the daemons.
    add_executable(test_ups_estimate
        test/test_ups_estimate.c src/display/ups_estimate.c)
    target_include_directories(test_ups_estimate PRIVATE src/display)
    target_link_libraries(test_ups_estimate PRIVATE m)
    add_test(NAME ups_estimate COMMAND test_ups_estimate)

    # The payload parsers, likewise free of LVGL, mosquitto and Linux.
    add_executable(test_payload
        test/test_payload.c src/display/payload.c)
    target_include_directories(test_payload PRIVATE src/display)
    target_link_libraries(test_payload PRIVATE m)
    add_test(NAME payload COMMAND test_payload)
endif()


######################################################################
# Display (optional, pulls in LVGL and a C++ toolchain)
######################################################################

# Both off by default: LVGL is a large submodule and a long compile
# (hours on the Pi Zero), and the three daemons do not need it. The
# panel is the Automation HAT Mini's ST7735 (README, *LCD*).
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
        src/display/payload.c
        src/display/source-mqtt.c
        src/display/source-nut.c
        src/display/ups_estimate.c)
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
    target_link_libraries(test_dashboard PRIVATE lvgl::lvgl Threads::Threads m)
    add_test(NAME dashboard COMMAND test_dashboard)
endif()
