cmake_minimum_required(VERSION 3.20.0)
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(table_lifter)
target_include_directories(app PRIVATE include)
option(TABLE_LIFTER_BENCH_TEST "Unloaded motor: automatic timed test on every boot" OFF)
option(TABLE_LIFTER_SPI_PROBE "Read-only continuous TMC SPI scope test" OFF)
if(TABLE_LIFTER_SPI_PROBE)
  target_sources(app PRIVATE src/spi_probe.c)
elseif(TABLE_LIFTER_BENCH_TEST)
  target_sources(app PRIVATE src/bench_main.c)
else()
  target_sources(app PRIVATE src/main.c)
endif()
target_sources(app PRIVATE
  src/control.c
  src/buttons.c
  src/motor.c
  src/range.c
  src/pd.c
  src/led.c
  src/ble.c
)
