cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_persistence)

# Real production sources we want to exercise
target_sources(app PRIVATE
	src/test_persistence.c
	src/stubs.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src/sys_data.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src/json_io.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src/cmd_interpreter.c
)

target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

# sys_data.c uses CONFIG_BT_DEVICE_NAME as a default-name string. That
# symbol only exists with CONFIG_BT=y. We inject it directly so tests
# don't need to drag the BT subsystem in.
target_compile_definitions(app PRIVATE
	CONFIG_BT_DEVICE_NAME=\"CoolingDock_NRF52\"
)

# Sanitizers + coverage. ASan needs more main stack on native_sim, see prj.conf.
target_compile_options(app PRIVATE
	-Wall -Wextra
	-fsanitize=address -fsanitize=undefined
	-fno-sanitize-recover=all
	--coverage
)
target_link_options(app PRIVATE
	-fsanitize=address -fsanitize=undefined
	--coverage
)
