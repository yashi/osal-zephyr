######################################################################
#
# CMAKE build recipe for Zephyr OSAL implementation
#
######################################################################

# This CMake script generates targets specific to the Zephyr implementation

# The basic set of files which are always built
set(ZEPHYR_BASE_SRCLIST
    src/os-impl-common.c
    src/os-impl-console.c
    src/os-impl-idmap.c
    src/os-impl-tasks.c
)

set(ZEPHYR_IMPL_SRCLIST
    ../portable/os-impl-console-bsp.c
)

# Defines an OBJECT target named "osal_zephyr_impl" with selected source files
add_library(osal_zephyr_impl OBJECT
    ${ZEPHYR_BASE_SRCLIST}
    ${ZEPHYR_IMPL_SRCLIST}
)

target_link_libraries(osal_zephyr_impl PUBLIC zephyr_interface)
add_dependencies(osal_zephyr_impl zephyr_generated_headers)

target_include_directories(osal_zephyr_impl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
