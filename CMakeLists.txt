# Pinscope as a Zephyr module: the portable core from device/pinscope plus the Zephyr port.
if(CONFIG_PINSCOPE)
  # In the public module the core is in core/; in this repository two levels up.
  if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/core/ps_core.c)
    set(PINSCOPE_CORE ${CMAKE_CURRENT_LIST_DIR}/core)
  else()
    set(PINSCOPE_CORE ${CMAKE_CURRENT_LIST_DIR}/../../device/pinscope/src)
  endif()
  zephyr_library()
  zephyr_library_sources(
    ${PINSCOPE_CORE}/ps_core.c
    ${PINSCOPE_CORE}/ps_crypto.c
    ps_port_zephyr.c
    checks.c
  )
  zephyr_library_sources_ifdef(CONFIG_PINSCOPE_AUTOSTART autostart.c)
  zephyr_library_sources_ifdef(CONFIG_ENTROPY_PINSCOPE_RP2040_ROSC drivers/entropy_rp2040_rosc.c)
  zephyr_include_directories(${PINSCOPE_CORE})
  # A build ID the firmware and the server can both read, see build_id.ld.
  zephyr_ld_options(-Wl,--build-id=sha1)
  zephyr_linker_sources(RODATA ${CMAKE_CURRENT_LIST_DIR}/build_id.ld)
  zephyr_library_sources_ifdef(CONFIG_PINSCOPE_LOG_BACKEND log_backend_pinscope.c)
  # The core's build-time settings arrive through Kconfig (CONFIG_PINSCOPE_*).
  zephyr_library_compile_definitions(PS_TASK_STACK=CONFIG_PINSCOPE_STACK_SIZE)
endif()
