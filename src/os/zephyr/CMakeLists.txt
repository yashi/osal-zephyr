######################################################################
#
# CMAKE build recipe for Zephyr OSAL implementation
#
######################################################################

# This CMake script generates targets specific to the Zephyr implementation

set(ZEPHYR_IMPL_SRCLIST
    ../portable/os-impl-console-bsp.c
)

# Defines an OBJECT target named "osal_zephyr_impl" with selected source files
add_library(osal_zephyr_impl OBJECT
    ${ZEPHYR_IMPL_SRCLIST}
)

target_link_libraries(osal_zephyr_impl PUBLIC zephyr_interface)
add_dependencies(osal_zephyr_impl zephyr_generated_headers)

target_include_directories(osal_zephyr_impl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
