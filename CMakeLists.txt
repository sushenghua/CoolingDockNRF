cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(coolingdock_nrf)

target_sources(app PRIVATE
	src/main.c
	src/sys_data.c
	src/sensor.c
	src/fan.c
	src/control.c
	src/control_logic.c
	src/json_io.c
	src/cmd_interpreter.c
	src/ble_svc.c
)

target_include_directories(app PRIVATE src)

# Tighten warnings on app sources. We don't add -Werror here because
# Zephyr's own headers occasionally trip GCC pedantic warnings on
# specific board targets; -Werror is enabled in the test builds (see
# tests/*/prj.conf) where we control the surface area.
target_compile_options(app PRIVATE -Wall -Wextra)
