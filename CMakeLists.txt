cmake_minimum_required(VERSION 3.10)
project(Moses)

option(WITH_LOG "Enable log messages")
option(WITH_PUT "Write one line data to output")
set(MQTT_TOPIC_PREFIX "water-breaker" CACHE STRING "Prefix for topics")

#
# Common compiler options
#

set(CMAKE_C_STANDARD   23)
add_compile_options(-Wall)
add_compile_options( -Wextra -Wpedantic)
add_definitions(-D_GNU_SOURCE)

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

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
set(3RD ${PROJECT_SOURCE_DIR}/3rd)


#
# Libraries detection
#

# M-Bus
find_path(   MBUS_INCLUDE_DIR             NAMES mbus/mbus.h
	     REQUIRED             	  HINTS /opt/libmbus/include)
find_library(MBUS_LIBRARY                 NAMES mbus
             REQUIRED			  HINTS /opt/libmbus/lib)

# Mosquitto library
find_path(   MOSQUITTO_INCLUDE_DIR        NAMES mosquitto.h
	     REQUIRED)
find_library(MOSQUITTO_LIBRARY            NAMES mosquitto
	     REQUIRED)

#mark_as_advanced(MBUS_INCLUDE_DIR MBUS_LIBRARY)


#
# Dependencies
#

# LVGL
set(LV_CONF_BUILD_DISABLE_EXAMPLES 1)
set(LV_CONF_BUILD_DISABLE_DEMOS    1)
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/lv_conf.h
	         CACHE STRING "" FORCE)
add_subdirectory(3rd/lvgl)


###################################################################### 


#
# Watermeter
#
add_executable(moses_watermeter src/watermeter.c src/common.c)
target_include_directories(moses_watermeter PUBLIC ${MBUS_INCLUDE_DIR})
target_include_directories(moses_watermeter PUBLIC ${MOSQUITTO_INCLUDE_DIR})
target_compile_definitions(moses_watermeter PUBLIC PUT_CLOCK=CLOCK_REALTIME
					           INTERVAL_CLOCK=CLOCK_REALTIME)
#target_link_directories(moses_watermeter PUBLIC /opt/libmbus/lib)
target_link_libraries(moses_watermeter ${MBUS_LIBRARY} ${MOSQUITTO_LIBRARY} m pthread)



#
# breaker
#
add_executable(moses_breaker src/breaker.c src/common.c)
target_compile_definitions(moses_breaker PUBLIC PUT_CLOCK=CLOCK_REALTIME
					        INTERVAL_CLOCK=CLOCK_REALTIME)
target_include_directories(moses_watermeter PUBLIC ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(moses_breaker ${MOSQUITTO_LIBRARY} m pthread)



#
# Sensors
#

add_executable(moses_sensors src/sensors.c src/common.c)

target_include_directories(moses_watermeter PUBLIC ${MOSQUITTO_INCLUDE_DIR})

# BME 280
target_include_directories(moses_sensors PUBLIC ${3RD}/BME280_SensorAPI)
target_compile_definitions(moses_sensors PUBLIC BME280_DOUBLE_ENABLE)
target_compile_definitions(moses_sensors PUBLIC BME280_INTF_RET_TYPE=int
				 	        BME280_INTF_RET_SUCCESS=0)
target_sources(moses_sensors PUBLIC ${3RD}/BME280_SensorAPI/bme280.c)

# Bitters
target_include_directories(moses_sensors PUBLIC ${3RD}/bitters/include)
target_compile_definitions(moses_sensors PUBLIC _GNU_SOURCE)
target_compile_definitions(moses_sensors PUBLIC  BITTERS_SILENCE_WARNING)
target_sources(moses_sensors PUBLIC ${3RD}/bitters/src/bitters.c
		       	      ${3RD}/bitters/src/spi.c
			      ${3RD}/bitters/src/i2c.c
			      ${3RD}/bitters/src/gpio.c)

target_link_libraries(moses_sensors m pthread ${MOSQUITTO_LIBRARY})



#
# Main
#

add_executable(main src/main.c src/integration/rpi-automation-hat-mini.c)
target_compile_options(main PUBLIC "-O3")

target_include_directories(main PUBLIC ${3RD}/lvgl)

target_sources(main PUBLIC analog-inputs-blank.c)


# BME 280
target_include_directories(main PUBLIC ${3RD}/BME280_SensorAPI)
target_compile_definitions(main PUBLIC BME280_DOUBLE_ENABLE)
target_compile_definitions(main PUBLIC BME280_INTF_RET_TYPE=int
				       BME280_INTF_RET_SUCCESS=0)
target_sources(main PUBLIC ${3RD}/BME280_SensorAPI/bme280.c)

# Bitters
target_include_directories(main PUBLIC ${3RD}/bitters/include)
target_compile_definitions(main PUBLIC _GNU_SOURCE)
target_compile_definitions(main PUBLIC  BITTERS_WITH_THREADS)
target_compile_definitions(main PUBLIC  BITTERS_SILENCE_WARNING)
target_sources(main PUBLIC ${3RD}/bitters/src/bitters.c
		    	   ${3RD}/bitters/src/spi.c
			   ${3RD}/bitters/src/i2c.c
			   ${3RD}/bitters/src/gpio.c)

target_link_libraries(main lvgl::lvgl lvgl::thorvg m pthread)

add_custom_target (run COMMAND ${EXECUTABLE_OUTPUT_PATH}/main DEPENDS main)

