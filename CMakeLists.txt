cmake_minimum_required(VERSION 3.10)
project(Moses)

set(CMAKE_C_STANDARD   17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

set(3RD ${PROJECT_SOURCE_DIR}/3rd)

# m-bus library
find_path(   MBUS_INCLUDE_DIR
             NAMES mbus/mbus.h
             HINTS /opt/libmbus/include
	     REQUIRED)
find_library(MBUS_LIBRARY
             NAMES mbus
             HINTS /opt/libmbus/lib
	     REQUIRED)

mark_as_advanced(MBUS_INCLUDE_DIR MBUS_LIBRARY)



 


#
# Watermeter
#
add_executable(moses_watermeter src/watermeter.c src/common.c)
target_include_directories(moses_watermeter PUBLIC ${MBUS_INCLUDE_DIR})
target_compile_definitions(moses_watermeter PUBLIC WITH_LOG)
target_compile_definitions(moses_watermeter PUBLIC PUT_CLOCK=CLOCK_REALTIME
					           INTERVAL_CLOCK=CLOCK_REALTIME)
#target_link_directories(moses_watermeter PUBLIC /opt/libmbus/lib)
target_link_libraries(moses_watermeter ${MBUS_LIBRARY} m pthread)


#
# breaker
#
add_executable(moses_breaker src/breaker.c src/common.c)
target_compile_definitions(moses_breaker PUBLIC WITH_LOG)
target_compile_definitions(moses_breaker PUBLIC PUT_CLOCK=CLOCK_REALTIME
					        INTERVAL_CLOCK=CLOCK_REALTIME)
target_link_libraries(moses_breaker  m pthread)



#
# Sensors
#

add_executable(moses_sensors src/sensors.c src/common.c)

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

target_link_libraries(moses_sensors m)




#
# LVGL
#  
set(LV_CONF_BUILD_DISABLE_EXAMPLES 1)
set(LV_CONF_BUILD_DISABLE_DEMOS    1)
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/lv_conf.h
	         CACHE STRING "" FORCE)
add_subdirectory(3rd/lvgl)

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

