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

option(WITH_LOG "Enable log messages on stderr"                            OFF)
option(WITH_PUT "Write each reading to stdout (line protocol)"            OFF)
option(WITH_GUI "Build the experimental LVGL interface (needs a C++ compiler)" OFF)
option(WITH_WERROR   "Treat warnings as errors for moses code (CI)"        OFF)
option(WITH_ANALYZER "Run the GCC static analyzer on moses code (CI)"      OFF)
option(WITH_TESTS    "Build the unit tests"                                ON)
set(MQTT_TOPIC_PREFIX "water-breaker" CACHE STRING "Default prefix for topics")


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
if (WITH_PUT)
    add_compile_definitions(WITH_PUT)
endif()
add_compile_definitions(MQTT_TOPIC_PREFIX="${MQTT_TOPIC_PREFIX}")


#
# Directories
#

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set(3RD ${PROJECT_SOURCE_DIR}/3rd)


#
# External libraries
#

find_package(Threads REQUIRED)

# Mosquitto (MQTT) -- used by every program
find_path(   MOSQUITTO_INCLUDE_DIR NAMES mosquitto.h REQUIRED)
find_library(MOSQUITTO_LIBRARY     NAMES mosquitto   REQUIRED)

# M-Bus -- used by moses_watermeter only (usually not packaged, see README)
find_path(   MBUS_INCLUDE_DIR NAMES mbus/mbus.h REQUIRED HINTS /opt/libmbus/include)
find_library(MBUS_LIBRARY     NAMES mbus        REQUIRED HINTS /opt/libmbus/lib)


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
# the GUI needs BITTERS_WITH_THREADS.
add_library(bitters INTERFACE)
target_include_directories(bitters INTERFACE ${3RD}/bitters/include)
target_compile_definitions(bitters INTERFACE BITTERS_SILENCE_WARNING)
target_sources(bitters INTERFACE
    ${3RD}/bitters/src/bitters.c
    ${3RD}/bitters/src/spi.c
    ${3RD}/bitters/src/i2c.c
    ${3RD}/bitters/src/gpio.c)


#
# Shared moses support code (MQTT wrapper, parsers, ...)
#

add_library(moses_common STATIC src/common.c)
target_include_directories(moses_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
                                               ${MOSQUITTO_INCLUDE_DIR})
target_compile_definitions(moses_common PUBLIC PUT_CLOCK=CLOCK_REALTIME
                                               INTERVAL_CLOCK=CLOCK_REALTIME)
target_link_libraries(moses_common PUBLIC ${MOSQUITTO_LIBRARY} Threads::Threads m)


######################################################################
# Programs
######################################################################

#
# Watermeter -- M-Bus index reading and/or GPIO pulse counting
#
add_executable(moses_watermeter src/watermeter.c)
target_include_directories(moses_watermeter PRIVATE ${MBUS_INCLUDE_DIR})
target_link_libraries(moses_watermeter PRIVATE moses_common ${MBUS_LIBRARY})

#
# Breaker -- drive the solenoid valve relay
#
add_executable(moses_breaker src/breaker.c)
target_link_libraries(moses_breaker PRIVATE moses_common)

#
# Sensors -- read the BME280 (temperature, pressure, humidity)
#
add_executable(moses_sensors src/sensors.c)
target_link_libraries(moses_sensors PRIVATE moses_common bme280 bitters)


#
# Treat warnings as errors for first-party code only. -Werror is scoped to
# our own source files rather than to the targets, because the bundled
# bitters submodule is compiled *inside* moses_sensors (INTERFACE library)
# and is not warning-clean.
#
set(MOSES_SOURCES
    src/common.c src/watermeter.c src/breaker.c src/sensors.c
    test/test_parsers.c)

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

if (WITH_TESTS)
    enable_testing()
    add_executable(test_parsers test/test_parsers.c)
    target_link_libraries(test_parsers PRIVATE moses_common)
    add_test(NAME parsers COMMAND test_parsers)
endif()


######################################################################
# Experimental LVGL interface (optional, pulls in a C++ toolchain)
######################################################################

if (WITH_GUI)
    enable_language(CXX)

    # LVGL
    set(LV_CONF_BUILD_DISABLE_EXAMPLES 1)
    set(LV_CONF_BUILD_DISABLE_DEMOS    1)
    set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/lv_conf.h
                     CACHE STRING "" FORCE)
    add_subdirectory(3rd/lvgl)

    add_executable(main
        src/main.c
        src/integration/rpi-automation-hat-mini.c
        ${PROJECT_SOURCE_DIR}/analog-inputs-blank.c)
    target_compile_options(main PRIVATE -O3)
    target_compile_definitions(main PRIVATE BITTERS_WITH_THREADS)
    target_link_libraries(main PRIVATE
        bme280 bitters lvgl::lvgl lvgl::thorvg Threads::Threads m)

    add_custom_target(run COMMAND main DEPENDS main)
endif()
