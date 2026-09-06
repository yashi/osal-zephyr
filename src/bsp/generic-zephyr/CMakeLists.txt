######################################################################
#
# CMAKE build recipe for Generic Zephyr Board Support Package (BSP)
#
######################################################################

add_library(osal_generic-zephyr_impl OBJECT
	src/bsp_console.c
	src/bsp_start.c
)

target_link_libraries(osal_generic-zephyr_impl PUBLIC zephyr_interface)
add_dependencies(osal_generic-zephyr_impl zephyr_generated_headers)

# This BSP only works with "zephyr" OS layer.
# Confirming this reduces risk of accidental misconfiguration
set_property(TARGET osal_generic-zephyr_impl PROPERTY OSAL_EXPECTED_OSTYPE "zephyr")
