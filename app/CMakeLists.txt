cmake_minimum_required(VERSION 3.20.0)
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
option(TABLE_LIFTER_ROTATION_TEST "Temporary handset control using commanded steps" OFF)
if(TABLE_LIFTER_ROTATION_TEST)
  set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/rotation.overlay"
      CACHE STRING "Rotation test hardware configuration" FORCE)
elseif(DTC_OVERLAY_FILE MATCHES "rotation\\.overlay")
  message(FATAL_ERROR "Use a pristine/separate build directory to leave rotation mode")
endif()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(table_lifter)
target_include_directories(app PRIVATE include)
option(TABLE_LIFTER_BENCH_TEST "Unloaded motor: automatic timed test on every boot" OFF)
option(TABLE_LIFTER_SPI_PROBE "Read-only continuous TMC SPI scope test" OFF)
if(TABLE_LIFTER_ROTATION_TEST AND (TABLE_LIFTER_SPI_PROBE OR TABLE_LIFTER_BENCH_TEST))
  message(FATAL_ERROR "Select only one test mode")
endif()
if(TABLE_LIFTER_ROTATION_TEST)
  target_sources(app PRIVATE src/rotation_main.c src/rotation_control.c)
elseif(TABLE_LIFTER_SPI_PROBE)
  target_sources(app PRIVATE src/spi_probe.c)
elseif(TABLE_LIFTER_BENCH_TEST)
  target_sources(app PRIVATE src/bench_main.c)
else()
  target_sources(app PRIVATE src/main.c)
endif()
target_sources(app PRIVATE
  src/buttons.c
  src/motor.c
  src/pd.c
  src/led.c
)

if(NOT TABLE_LIFTER_ROTATION_TEST)
  target_sources(app PRIVATE src/control.c src/range.c src/ble.c)
endif()
