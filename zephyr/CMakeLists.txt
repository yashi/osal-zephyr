if(CONFIG_CFS_OSAL)
  set(OSAL_SYSTEM_BSPTYPE "generic-zephyr")

  # The initial console provider has no helper task. Keep command-line and
  # OSAL_CONFIGURATION_FILE overrides available; async creation reports that
  # the requested capability is not implemented.
  set(OSAL_CONFIG_CONSOLE_ASYNC FALSE CACHE BOOL
      "Controls spawning of a separate utility task for OS_printf")

  add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR} build)

  # We can't use zephyr_interface_library_named() on osal_public_api
  # because both the top-level CMakeLists.txt of OSAL and the macro
  # calls add_library() on it. Instead, append ZEPHYR_INTERFACE_LIBS
  # on osal_public_api
  #zephyr_interface_library_named(libcsp)
  #target_include_directories(libcsp INTERFACE ${ZEPHYR_CURRENT_MODULE_DIR}/include)
  #target_include_directories(libcsp INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/build/include)
  set_property(GLOBAL APPEND PROPERTY ZEPHYR_INTERFACE_LIBS osal_public_api)

  zephyr_append_cmake_library(osal)
  target_link_libraries(osal PUBLIC zephyr_interface)

  zephyr_append_cmake_library(osal_bsp)
  target_link_libraries(osal_bsp PUBLIC zephyr_interface)
endif()
